/**
 * main.cpp — Multicore event-driven simulation demo & benchmark.
 *
 * THREE separate experiments, each measuring one variable at a time:
 *
 *   EXPERIMENT 1 — Lock Granularity (both use std::mutex):
 *     Modern engine + CoarseGrainedStrategy  (1 global mutex)
 *     Modern engine + FineGrainedStrategy    (1 mutex per signal)
 *     Question answered: does splitting one lock into N locks help?
 *
 *   EXPERIMENT 2 — Synchronization Primitive (both are fine-grained):
 *     Modern engine + FineGrainedStrategy    (per-signal std::shared_mutex)
 *     Modern engine + AtomicStrategy         (per-signal std::atomic)
 *     Question answered: does replacing mutex with atomic help?
 *
 *   EXPERIMENT 3 — Flag Primitive Microbenchmark (hot path isolation):
 *     std::mutex  + bool                     (lock + branch + set + unlock)
 *     std::atomic<bool> + CAS                (compare_exchange_strong)
 *     std::atomic_flag + test_and_set        (single hardware instruction)
 *     Question answered: for a simple ready-flag, what is the cheapest primitive?
 *     This maps to process activation: "set flag if not already set"
 *
 *   LEGACY SECTION (educational only — different engine, not a clean comparison):
 *     LegacySimEngine  — shows the hand-rolled barrier, volatile bool, no latch
 *
 * Design:
 *   Signals:  CLK, RESET, DATA_IN, DATA_VALID, DATA_OUT, ADDR, ACK, ERR
 *   Processes: clk_gen, reset_ctrl, data_producer, data_consumer,
 *              ack_generator, error_monitor
 *   Regions:  0=clocking, 1=data_path, 2=control
 */

#include "engine/SimulationEngine.hpp"
#include "legacy/LegacySimEngine.hpp"
#include "sync/SyncStrategyFactory.hpp"

#include <iostream>
#include <iomanip>
#include <chrono>
#include <string>
#include <atomic>
#include <mutex>
#include <barrier>
#include <numeric>

using namespace celeris;
using namespace celeris::legacy;
using Clock = std::chrono::high_resolution_clock;

// ============================================================================
// Signal and process IDs
// ============================================================================
namespace SigID {
    constexpr int CLK        = 0;
    constexpr int RESET      = 1;
    constexpr int DATA_IN    = 2;
    constexpr int DATA_VALID = 3;
    constexpr int DATA_OUT   = 4;
    constexpr int ADDR       = 5;
    constexpr int ACK        = 6;
    constexpr int ERR        = 7;
    constexpr int COUNT      = 8;
}

namespace ProcID {
    constexpr int CLK_GEN       = 0;
    constexpr int RESET_CTRL    = 1;
    constexpr int DATA_PRODUCER = 2;
    constexpr int DATA_CONSUMER = 3;
    constexpr int ACK_GENERATOR = 4;
    constexpr int ERR_MONITOR   = 5;
    constexpr int COUNT         = 6;
}

// ============================================================================
// Banner
// ============================================================================
static void print_banner()
{
    std::cout << R"(
╔══════════════════════════════════════════════════════════════════════════╗
║  celeris — Multicore Event-Driven Simulation Engine                        ║
║  C++20 synchronization benchmarks for multicore simulation              ║
║                                                                          ║
║  Benchmark design:                                                       ║
║    Experiment 1 — Lock Granularity  (coarse mutex vs fine mutex)        ║
║    Experiment 2 — Sync Primitive    (fine mutex vs fine atomic)         ║
║    Legacy section — educational coarse-grained + hand-rolled barrier    ║
╚══════════════════════════════════════════════════════════════════════════╝
)" << std::flush;
}

// ============================================================================
// Populate the modern engine with signals, processes, regions, and events
// ============================================================================
static void populate_engine(SimulationEngine& engine, int num_events)
{
    // ── Signals ───────────────────────────────────────────────────────────
    auto add_sig = [&](int id, const char* name, std::vector<int> procs) {
        Signal s;
        s.id = id;
        s.name = name;
        s.current_value = LogicValue::ZERO;
        s.atomic_value.store(0, std::memory_order_relaxed);
        s.sensitive_processes = std::move(procs);
        engine.add_signal(std::move(s));
    };

    add_sig(SigID::CLK,        "CLK",        {ProcID::CLK_GEN, ProcID::DATA_PRODUCER, ProcID::DATA_CONSUMER});
    add_sig(SigID::RESET,      "RESET",      {ProcID::RESET_CTRL});
    add_sig(SigID::DATA_IN,    "DATA_IN",    {ProcID::DATA_PRODUCER});
    add_sig(SigID::DATA_VALID, "DATA_VALID", {ProcID::DATA_CONSUMER, ProcID::ACK_GENERATOR});
    add_sig(SigID::DATA_OUT,   "DATA_OUT",   {ProcID::DATA_CONSUMER});
    add_sig(SigID::ADDR,       "ADDR",       {ProcID::DATA_PRODUCER});
    add_sig(SigID::ACK,        "ACK",        {ProcID::DATA_PRODUCER, ProcID::ACK_GENERATOR});
    add_sig(SigID::ERR,        "ERR",        {ProcID::ERR_MONITOR});

    // ── Processes ─────────────────────────────────────────────────────────
    auto add_proc = [&](int id, const char* name, std::vector<int> sens,
                        std::function<void(SimContext&)> fn) {
        Process p;
        p.id = id;
        p.name = name;
        p.sensitivity_list = std::move(sens);
        p.evaluate = std::move(fn);
        engine.add_process(std::move(p));
    };

    add_proc(ProcID::CLK_GEN, "clk_gen",
        {SigID::CLK},
        [](SimContext& ctx) {
            ctx.total_events_processed.fetch_add(1, std::memory_order_relaxed);
        }
    );
    add_proc(ProcID::RESET_CTRL, "reset_ctrl",
        {SigID::RESET},
        [](SimContext& ctx) {
            ctx.signals[SigID::RESET].current_value = LogicValue::ZERO;
            ctx.total_events_processed.fetch_add(1, std::memory_order_relaxed);
        }
    );
    add_proc(ProcID::DATA_PRODUCER, "data_producer",
        {SigID::CLK, SigID::ADDR},
        [](SimContext& ctx) {
            if (ctx.signals[SigID::CLK].current_value == LogicValue::ONE) {
                ctx.signals[SigID::DATA_IN].current_value    = LogicValue::ONE;
                ctx.signals[SigID::DATA_VALID].current_value = LogicValue::ONE;
            }
            ctx.total_events_processed.fetch_add(1, std::memory_order_relaxed);
        }
    );
    add_proc(ProcID::DATA_CONSUMER, "data_consumer",
        {SigID::DATA_VALID, SigID::DATA_OUT},
        [](SimContext& ctx) {
            if (ctx.signals[SigID::DATA_VALID].current_value == LogicValue::ONE)
                ctx.signals[SigID::DATA_OUT].current_value =
                    ctx.signals[SigID::DATA_IN].current_value;
            ctx.total_events_processed.fetch_add(1, std::memory_order_relaxed);
        }
    );
    add_proc(ProcID::ACK_GENERATOR, "ack_generator",
        {SigID::DATA_VALID, SigID::ACK},
        [](SimContext& ctx) {
            ctx.signals[SigID::ACK].current_value =
                ctx.signals[SigID::DATA_VALID].current_value;
            ctx.total_events_processed.fetch_add(1, std::memory_order_relaxed);
        }
    );
    add_proc(ProcID::ERR_MONITOR, "error_monitor",
        {SigID::ERR},
        [](SimContext& ctx) {
            if (ctx.signals[SigID::ERR].current_value == LogicValue::ONE)
                ctx.signals[SigID::RESET].current_value = LogicValue::ONE;
            ctx.total_events_processed.fetch_add(1, std::memory_order_relaxed);
        }
    );

    // ── Regions ───────────────────────────────────────────────────────────
    { Region r; r.id=0; r.process_ids={ProcID::CLK_GEN, ProcID::RESET_CTRL};
      r.signal_ids={SigID::CLK, SigID::RESET}; r.boundary_signals={SigID::CLK};
      engine.add_region(std::move(r)); }
    { Region r; r.id=1; r.process_ids={ProcID::DATA_PRODUCER, ProcID::DATA_CONSUMER};
      r.signal_ids={SigID::DATA_IN, SigID::DATA_VALID, SigID::DATA_OUT, SigID::ADDR};
      r.boundary_signals={SigID::DATA_VALID}; engine.add_region(std::move(r)); }
    { Region r; r.id=2; r.process_ids={ProcID::ACK_GENERATOR, ProcID::ERR_MONITOR};
      r.signal_ids={SigID::ACK, SigID::ERR}; r.boundary_signals={};
      engine.add_region(std::move(r)); }

    // ── Seed events ───────────────────────────────────────────────────────
    for (int i = 0; i < num_events; ++i) {
        Event e;
        e.type      = EventType::SIGNAL_UPDATE;
        e.signal_id = i % SigID::COUNT;
        e.new_value = (i % 2 == 0) ? LogicValue::ONE : LogicValue::ZERO;
        e.when.time = static_cast<uint64_t>((i + 1) * 10);
        engine.add_event(e);
    }
    // Delta events at t=0
    for (auto [sid, val] : std::initializer_list<std::pair<int,LogicValue>>{
            {SigID::RESET, LogicValue::ONE}, {SigID::CLK, LogicValue::ZERO}}) {
        Event e; e.type=EventType::SIGNAL_UPDATE; e.signal_id=sid; e.new_value=val;
        e.when.time=0; e.when.delta=0; engine.add_event(e);
    }
}

// ============================================================================
// Legacy engine setup
// ============================================================================
static void populate_legacy(LegacySimEngine& engine, int num_events)
{
    for (int i = 0; i < SigID::COUNT; ++i) {
        LegacySignal s; s.id = i; s.value = LegacyLogicValue::X;
        for (int p = 0; p < ProcID::COUNT; ++p) s.sensitive_processes.push_back(p);
        engine.add_signal(s);
    }
    for (int i = 0; i < ProcID::COUNT; ++i) {
        LegacyProcess p; p.id = i;
        p.evaluate = [](){
            volatile int x = 0;
            for (int k = 0; k < 50; ++k) x += k;
            (void)x;
        };
        engine.add_process(p);
    }
    for (int i = 0; i < num_events; ++i) {
        LegacyEvent e;
        e.type      = LegacyEventType::SIGNAL_UPDATE;
        e.signal_id = i % SigID::COUNT;
        e.new_value = (i % 2 == 0) ? LegacyLogicValue::ONE : LegacyLogicValue::ZERO;
        e.sim_time  = static_cast<uint64_t>((i + 1) * 10);
        engine.add_event(e);
    }
    { LegacyEvent e; e.type=LegacyEventType::SIGNAL_UPDATE;
      e.signal_id=SigID::RESET; e.new_value=LegacyLogicValue::ONE; e.sim_time=0;
      engine.add_event(e); }
}

// ============================================================================
// Helpers
// ============================================================================
static void sep(const char* title)
{
    std::cout << "\n\n══════════════════════════════════════════════════════════\n";
    std::cout << "  " << title << "\n";
    std::cout << "══════════════════════════════════════════════════════════\n";
}

// Run engine with a given mode; return events/sec
static double run_modern(int num_threads, SyncMode mode, int num_events,
                         uint64_t sim_end, bool verbose = true)
{
    SimulationEngine engine(num_threads, mode);
    populate_engine(engine, num_events);

    auto t0 = Clock::now();
    engine.run_until(sim_end);
    auto t1 = Clock::now();
    std::chrono::duration<double> elapsed = t1 - t0;

    if (verbose)
        engine.profiler().report(std::cout, elapsed,
                                 SyncStrategyFactory::mode_name(mode));

    return engine.profiler().events_per_second(elapsed);
}

// ============================================================================
// Print comparison table
// ============================================================================
struct Row { const char* label; double eps; };

static void print_table(const char* title, std::initializer_list<Row> rows)
{
    double base = rows.begin()->eps;
    std::cout << "\n  " << title << "\n";
    std::cout << "  ┌──────────────────────────────┬────────────────┬────────────┐\n";
    std::cout << "  │  Strategy                    │  Events/sec    │  Speedup   │\n";
    std::cout << "  ├──────────────────────────────┼────────────────┼────────────┤\n";
    for (auto& r : rows) {
        std::cout << "  │  " << std::left  << std::setw(28) << r.label << "│"
                  << std::right << std::setw(14) << std::fixed
                  << std::setprecision(0) << r.eps << "  │"
                  << std::setw(8) << std::setprecision(2) << r.eps / base << "x  │\n";
    }
    std::cout << "  └──────────────────────────────┴────────────────┴────────────┘\n";
}

// ============================================================================
// EXPERIMENT 3 — Flag Primitive Microbenchmark
//
// Models the hot-path pattern: "activate a process flag if not already set".
// This is exactly what happens on every signal change: the engine must set a
// per-process ready-flag exactly once, even if multiple threads see the same
// signal change simultaneously.
//
// Three implementations — same semantics, different primitives:
//
//   mutex_flag:   std::mutex + bool
//     → lock(), if !flag: flag=true, unlock()
//     → expensive: kernel object, possible context switch
//
//   atomic_cas:   std::atomic<bool> + compare_exchange_strong
//     → CAS(false → true): single RMW instruction + cache coherence
//     → no kernel involvement; fails gracefully if already set
//
//   atomic_flag_tas: std::atomic_flag + test_and_set
//     → single LOCK XCHG instruction on x86 (cheapest possible)
//     → no spurious wakeups, no OS, no CAS retry loop needed
//     → the primitive std::binary_semaphore is built on top of this
//
// Benchmark protocol:
//   N threads share one flag.  Each thread runs ITERS rounds of:
//     1. Spin-wait until flag is clear (simulates: wait for process to finish)
//     2. Set the flag (simulates: activate the process)
//   A coordinator thread clears the flag after each round (simulates: worker
//   running the process and setting it back to DORMANT).
//   We measure: total successful activations per second across all threads.
// ============================================================================

static constexpr int    FLAG_THREADS = 3;
static constexpr int    FLAG_ITERS   = 200'000;  // per thread

// ── Variant 1: mutex + bool ──────────────────────────────────────────────────
static double bench_mutex_flag(int num_threads, int iters)
{
    std::mutex       mtx;
    bool             flag{false};
    std::atomic<int> activations{0};

    // std::barrier completion: clear flag each round so threads can retry.
    // This models the worker finishing and setting process back to DORMANT.
    std::barrier sync(num_threads, [&]() noexcept {
        std::lock_guard lk(mtx);
        flag = false;  // "process finished, ready to be activated again"
    });

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    auto t0 = Clock::now();

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, iters]{
            for (int i = 0; i < iters; ++i) {
                // Try to activate: set flag if not already set.
                {
                    std::lock_guard lk(mtx);
                    if (!flag) {
                        flag = true;
                        activations.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                // All threads sync here; barrier completion clears the flag.
                sync.arrive_and_wait();
            }
        });
    }
    for (auto& th : threads) th.join();

    double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
    return activations.load() / elapsed;
}

// ── Variant 2: atomic<bool> + CAS ───────────────────────────────────────────
static double bench_atomic_cas(int num_threads, int iters)
{
    std::atomic<bool> flag{false};
    std::atomic<int>  activations{0};

    std::barrier sync(num_threads, [&]() noexcept {
        flag.store(false, std::memory_order_release);  // clear for next round
    });

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    auto t0 = Clock::now();

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, iters]{
            for (int i = 0; i < iters; ++i) {
                // CAS: atomically set true only if currently false.
                // Exactly one thread wins per round (the first to CAS).
                bool expected = false;
                if (flag.compare_exchange_strong(
                        expected, true,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed)) {
                    activations.fetch_add(1, std::memory_order_relaxed);
                }
                sync.arrive_and_wait();
            }
        });
    }
    for (auto& th : threads) th.join();

    double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
    return activations.load() / elapsed;
}

// ── Variant 3: atomic_flag + test_and_set ───────────────────────────────────
static double bench_atomic_flag_tas(int num_threads, int iters)
{
    // std::atomic_flag: the only C++ type guaranteed lock-free on ALL platforms.
    // test_and_set() → LOCK XCHG on x86: single bus-locked instruction.
    // Returns true if flag WAS already set (i.e., we lost the race).
    std::atomic_flag flag = ATOMIC_FLAG_INIT;   // starts clear
    std::atomic<int> activations{0};

    std::barrier sync(num_threads, [&]() noexcept {
        flag.clear(std::memory_order_release);  // clear for next round
    });

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    auto t0 = Clock::now();

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, iters]{
            for (int i = 0; i < iters; ++i) {
                // test_and_set: sets flag and returns previous value.
                // If previous was false → we activated; if true → someone else did.
                if (!flag.test_and_set(std::memory_order_acq_rel)) {
                    activations.fetch_add(1, std::memory_order_relaxed);
                }
                sync.arrive_and_wait();
            }
        });
    }
    for (auto& th : threads) th.join();

    double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
    return activations.load() / elapsed;
}

// ============================================================================
// EXPERIMENT 4 — Shared Variable Contention Microbenchmark
//
// Models a hot shared variable — e.g. a simulation-wide event counter,
// a signal value written by every thread on every delta cycle, or any
// "one global thing all threads update".
//
// This is DIFFERENT from Experiment 3 (set-once flag per round):
//   Exp 3: only ONE thread wins per round; others do nothing after losing.
//   Exp 4: ALL threads write on EVERY iteration — sustained maximum contention.
//
// Three implementations:
//
//   mutex + int:
//     Classic approach. lock_guard → counter++ → unlock on every iteration.
//     Every thread serializes through the mutex every time.
//     Even uncontested, mutex acquire/release costs ~10-30ns per call.
//     Under contention: losing threads park (OS context switch), winning
//     thread pays wake-up cost — can collapse to single-thread throughput.
//
//   atomic<int> fetch_add:
//     Hardware RMW (Read-Modify-Write) instruction: LOCK XADD on x86.
//     All threads still serialize at the cache line — but in user-space,
//     no OS involvement, no sleeping. The CPU cache-coherence protocol
//     (MESI) arbitrates: one core "owns" the cache line per operation.
//     Under high contention: cache line bounces between cores → "cache
//     line ping-pong". Still faster than mutex because no syscall.
//
//   thread-local shard + merge:
//     Each thread owns a local counter — zero shared state on hot path.
//     No mutex, no atomic RMW, no cache line contention whatsoever.
//     At the end (or at each barrier), shards are summed into the total.
//     This is the pattern used in real simulators for profiling counters,
//     event tallies, and any "accumulate then read" variable.
//     Tradeoff: reading the "current" global total requires summing all
//     shards — only practical when writes >> reads.
//
// Connection to celeris:
//   Profiler::total_events is currently std::atomic<uint64_t> (fetch_add).
//   In a 32-core simulation, every worker calls fetch_add on every event.
//   Replacing with per-thread shards + barrier-time merge would be the
//   next optimization step.
// ============================================================================

static constexpr int SHARED_THREADS = 3;
static constexpr int SHARED_ITERS   = 500'000;  // per thread, all write every iteration

// ── Variant G: mutex + int ───────────────────────────────────────────────────
static double bench_shared_mutex(int num_threads, int iters)
{
    std::mutex  mtx;
    long long   counter{0};

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    auto t0 = Clock::now();

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, iters]{
            for (int i = 0; i < iters; ++i) {
                std::lock_guard lk(mtx);   // acquire + release every iteration
                ++counter;
            }
        });
    }
    for (auto& th : threads) th.join();

    double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
    // Return increments/sec (all threads combined)
    return static_cast<double>(num_threads * iters) / elapsed;
}

// ── Variant H: atomic<int> fetch_add ────────────────────────────────────────
static double bench_shared_atomic(int num_threads, int iters)
{
    std::atomic<long long> counter{0};

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    auto t0 = Clock::now();

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, iters]{
            for (int i = 0; i < iters; ++i) {
                // LOCK XADD on x86: atomic fetch-and-add, single instruction
                // Still contended: cache line bounces between cores
                counter.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : threads) th.join();

    double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
    return static_cast<double>(num_threads * iters) / elapsed;
}

// ── Variant I: per-thread shard + merge ─────────────────────────────────────
static double bench_shared_sharded(int num_threads, int iters)
{
    // One counter per thread — cache-line padded to prevent false sharing.
    // Padding ensures each thread's counter lives on its own 64-byte cache line.
    // Without padding: adjacent counters share a cache line → false sharing →
    // same cache-line-ping-pong as a single atomic, despite being logically separate.
    struct alignas(64) PaddedCounter {
        long long value{0};
        // 64 - sizeof(long long) = 56 bytes of padding
        char pad[64 - sizeof(long long)];
    };

    std::vector<PaddedCounter> shards(num_threads);
    std::vector<std::thread>   threads;
    threads.reserve(num_threads);
    auto t0 = Clock::now();

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t, iters]{
            for (int i = 0; i < iters; ++i) {
                // Pure local write — no shared state, no lock, no atomic RMW
                // This cache line is owned exclusively by thread t
                ++shards[t].value;
            }
        });
    }
    for (auto& th : threads) th.join();

    // Merge: sum all shards into a global total (done once, not on hot path)
    long long total = 0;
    for (auto& s : shards) total += s.value;
    (void)total;

    double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
    return static_cast<double>(num_threads * iters) / elapsed;
}

// ============================================================================
// JSON mode helpers
// ============================================================================

struct RunResult {
    uint64_t total_events{0};
    uint64_t delta_cycles{0};
    uint64_t signal_updates{0};
    uint64_t process_evaluations{0};
    uint64_t barrier_waits{0};
    uint64_t contention_events{0};
    uint64_t nba_updates{0};
    double   wall_ms{0};
    double   events_per_sec{0};
};

static RunResult run_modern_capture(int num_threads, SyncMode mode,
                                    int num_events, uint64_t sim_end)
{
    SimulationEngine engine(num_threads, mode);
    populate_engine(engine, num_events);
    auto t0 = Clock::now();
    engine.run_until(sim_end);
    auto t1 = Clock::now();
    std::chrono::duration<double> elapsed = t1 - t0;
    const auto& p = engine.profiler();
    return RunResult{
        p.total_events.load(),
        p.delta_cycles.load(),
        p.signal_updates.load(),
        p.process_evaluations.load(),
        p.barrier_waits.load(),
        p.contention_events.load(),
        p.nba_updates.load(),
        elapsed.count() * 1000.0,
        p.events_per_second(elapsed)
    };
}

static void emit_run(std::ostream& out, const RunResult& r)
{
    out << "{"
        << "\"total_events\":"        << r.total_events        << ","
        << "\"delta_cycles\":"        << r.delta_cycles        << ","
        << "\"signal_updates\":"      << r.signal_updates      << ","
        << "\"process_evaluations\":" << r.process_evaluations << ","
        << "\"barrier_waits\":"       << r.barrier_waits       << ","
        << "\"contention_events\":"   << r.contention_events   << ","
        << "\"nba_updates\":"         << r.nba_updates         << ","
        << "\"wall_ms\":"             << std::fixed << std::setprecision(3) << r.wall_ms << ","
        << "\"events_per_sec\":"      << std::setprecision(0)  << r.events_per_sec
        << "}";
}

static void run_json_mode(int num_threads, int num_events, uint64_t sim_end)
{
    // Experiment 1 — granularity
    auto coarse = run_modern_capture(num_threads, SyncMode::COARSE_GRAINED, num_events, sim_end);
    auto fine   = run_modern_capture(num_threads, SyncMode::FINE_GRAINED,   num_events, sim_end);
    // Experiment 2 — primitive  (reuse fine from exp1)
    auto atomic = run_modern_capture(num_threads, SyncMode::ATOMIC,         num_events, sim_end);
    // Experiment 3 — flag primitive
    double flag_mutex = bench_mutex_flag(    std::min(num_threads, FLAG_THREADS), FLAG_ITERS);
    double flag_cas   = bench_atomic_cas(    std::min(num_threads, FLAG_THREADS), FLAG_ITERS);
    double flag_tas   = bench_atomic_flag_tas(std::min(num_threads, FLAG_THREADS), FLAG_ITERS);
    // Experiment 4 — shared variable contention
    double sv_mutex   = bench_shared_mutex(  std::min(num_threads, SHARED_THREADS), SHARED_ITERS);
    double sv_atomic  = bench_shared_atomic( std::min(num_threads, SHARED_THREADS), SHARED_ITERS);
    double sv_sharded = bench_shared_sharded(std::min(num_threads, SHARED_THREADS), SHARED_ITERS);

    std::cout << "{"
        << "\"config\":{"
            << "\"num_threads\":" << num_threads << ","
            << "\"num_events\":"  << num_events  << ","
            << "\"sim_end\":"     << sim_end
        << "},"
        << "\"exp1\":{"
            << "\"title\":\"Lock Granularity\","
            << "\"subtitle\":\"1 global mutex vs 1 mutex per signal (same primitive)\","
            << "\"coarse\":"; emit_run(std::cout, coarse);
    std::cout << ",\"fine\":";   emit_run(std::cout, fine);
    std::cout << "},"
        << "\"exp2\":{"
            << "\"title\":\"Sync Primitive\","
            << "\"subtitle\":\"Per-signal shared_mutex vs per-signal atomic (same granularity)\","
            << "\"fine_mutex\":"; emit_run(std::cout, fine);
    std::cout << ",\"atomic\":"; emit_run(std::cout, atomic);
    std::cout << "},"
        << "\"exp3\":{"
            << "\"title\":\"Flag Primitive — Process Activation Hot Path\","
            << "\"subtitle\":\"N threads race to set a flag exactly once per round\","
            << "\"mutex_flag\":"   << std::setprecision(0) << flag_mutex << ","
            << "\"atomic_cas\":"   << flag_cas   << ","
            << "\"atomic_flag\":"  << flag_tas
        << "},"
        << "\"exp4\":{"
            << "\"title\":\"Shared Variable Contention\","
            << "\"subtitle\":\"All threads increment ONE counter on every iteration\","
            << "\"mutex\":"   << sv_mutex   << ","
            << "\"atomic\":"  << sv_atomic  << ","
            << "\"sharded\":" << sv_sharded
        << "}"
        << "}" << std::endl;
}

// ============================================================================
// main
// ============================================================================
int main(int argc, char* argv[])
{
    constexpr int      NUM_THREADS = 3;
    constexpr int      NUM_EVENTS  = 200;
    constexpr uint64_t SIM_END     = 5000;

    bool json_mode = false;
    int  num_threads = NUM_THREADS;
    int  num_events  = NUM_EVENTS;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--json") {
            json_mode = true;
        } else if ((a == "--threads" || a == "-t") && i + 1 < argc) {
            try { num_threads = std::clamp(std::stoi(argv[++i]), 1, 16); } catch (...) {}
        } else if ((a == "--events" || a == "-e") && i + 1 < argc) {
            try { num_events = std::clamp(std::stoi(argv[++i]), 10, 10000); } catch (...) {}
        } else {
            // legacy: first positional arg = num_events
            try { num_events = std::stoi(a); } catch (...) {}
        }
    }

    if (json_mode) {
        run_json_mode(num_threads, num_events, SIM_END);
        return 0;
    }

    print_banner();

    std::cout << "\n  Config: " << num_threads << " worker threads, "
              << num_events << " seed events, simulate until t=" << SIM_END
              << "\n" << std::flush;

    // ========================================================================
    // EXPERIMENT 1 — Lock Granularity
    //   Both strategies use std::mutex. Only the number of locks changes.
    //   This isolates the effect of splitting one lock into N locks.
    // ========================================================================
    sep("EXPERIMENT 1: Lock Granularity  (mutex vs mutex, coarse vs fine)");
    std::cout << "  Variable:  number of locks   (1 global  vs  1-per-signal)\n";
    std::cout << "  Constant:  synchronization primitive = std::mutex\n\n";

    std::cout << "  Run A — COARSE_GRAINED  (1 global std::mutex for all signals)\n\n";
    double eps_coarse = run_modern(num_threads, SyncMode::COARSE_GRAINED,
                                   num_events, SIM_END);

    std::cout << "\n  Run B — FINE_GRAINED  (1 std::shared_mutex per signal)\n\n";
    double eps_fine   = run_modern(num_threads, SyncMode::FINE_GRAINED,
                                   num_events, SIM_END);

    print_table("Experiment 1 result — granularity effect:", {
        {"COARSE  (1 global mutex)",  eps_coarse},
        {"FINE    (1 mutex/signal)",  eps_fine},
    });

    std::cout << "\n  Interpretation:\n"
              << "    Speedup comes PURELY from reducing lock contention:\n"
              << "    threads touching different signals no longer block each other.\n"
              << "    The synchronization primitive (mutex) is the same in both.\n";

    // ========================================================================
    // EXPERIMENT 2 — Synchronization Primitive
    //   Both strategies are fine-grained (per-signal). Only the primitive changes.
    //   This isolates mutex overhead vs atomic instruction overhead.
    // ========================================================================
    sep("EXPERIMENT 2: Sync Primitive  (mutex vs atomic, both fine-grained)");
    std::cout << "  Variable:  synchronization primitive (mutex  vs  atomic)\n";
    std::cout << "  Constant:  lock granularity = per-signal\n\n";

    std::cout << "  Run B — FINE_GRAINED  (per-signal std::shared_mutex)\n\n";
    // Reuse eps_fine from above (same config).
    run_modern(num_threads, SyncMode::FINE_GRAINED, num_events, SIM_END, false);

    std::cout << "\n  Run C — ATOMIC  (per-signal std::atomic, zero mutex)\n\n";
    double eps_atomic = run_modern(num_threads, SyncMode::ATOMIC,
                                   num_events, SIM_END);

    print_table("Experiment 2 result — primitive effect:", {
        {"FINE_GRAINED  (shared_mutex)", eps_fine},
        {"ATOMIC        (atomic store)", eps_atomic},
    });

    std::cout << "\n  Interpretation:\n"
              << "    Speedup comes from eliminating mutex kernel overhead:\n"
              << "    atomic store/load = single CPU instruction + memory fence.\n"
              << "    No OS scheduler involvement, no cache-line bouncing on mutex state.\n"
              << "    Gap widens with thread count as mutex contention grows.\n";

    // ========================================================================
    // EXPERIMENT 3 — Flag Primitive Microbenchmark
    //   Isolates the flag-set operation from all engine overhead.
    //   Models process activation: "set ready-flag exactly once per round,
    //   even when N threads all try simultaneously."
    // ========================================================================
    sep("EXPERIMENT 3: Flag Primitive  (mutex+bool vs atomic<bool> vs atomic_flag)");
    std::cout << "  Variable:  flag primitive\n";
    std::cout << "  Constant:  " << FLAG_THREADS << " threads, "
              << FLAG_ITERS << " rounds each\n";
    std::cout << "  Pattern:   N threads race to set flag; exactly 1 wins per round\n";
    std::cout << "             (models process activation on signal change)\n\n";

    std::cout << "  Run D — mutex + bool\n";
    std::cout << "           lock_guard → if (!flag) flag=true → unlock\n";
    double eps_mutex_flag = bench_mutex_flag(FLAG_THREADS, FLAG_ITERS);
    std::cout << "           activations/sec: " << std::fixed << std::setprecision(0)
              << eps_mutex_flag << "\n\n";

    std::cout << "  Run E — atomic<bool> + CAS\n";
    std::cout << "           compare_exchange_strong(false → true)\n";
    double eps_atomic_cas = bench_atomic_cas(FLAG_THREADS, FLAG_ITERS);
    std::cout << "           activations/sec: " << eps_atomic_cas << "\n\n";

    std::cout << "  Run F — atomic_flag + test_and_set\n";
    std::cout << "           test_and_set(acq_rel)  [LOCK XCHG on x86]\n";
    double eps_atomic_tas = bench_atomic_flag_tas(FLAG_THREADS, FLAG_ITERS);
    std::cout << "           activations/sec: " << eps_atomic_tas << "\n";

    print_table("Experiment 3 result — flag primitive effect:", {
        {"mutex + bool",           eps_mutex_flag},
        {"atomic<bool> + CAS",     eps_atomic_cas},
        {"atomic_flag + TAS",      eps_atomic_tas},
    });

    std::cout << "\n  Interpretation:\n"
              << "    mutex + bool     — lock/unlock on every attempt, even uncontested.\n"
              << "                       OS involvement if any thread sleeps.\n"
              << "    atomic<bool> CAS — single RMW instruction; fails fast if flag already set.\n"
              << "                       No syscall, no sleep, pure user-space.\n"
              << "    atomic_flag TAS  — same as CAS but even simpler: no loop, no expected value.\n"
              << "                       LOCK XCHG = cheapest possible flag set in C++.\n"
              << "    In the simulation hot path, replacing mutex+bool with atomic_flag\n"
              << "    removes the lock from every process activation call.\n";

    // ========================================================================
    // EXPERIMENT 4 — Shared Variable Contention
    //   All threads write to a SINGLE shared variable on every iteration.
    //   Maximum sustained contention: no round structure, no winner/loser.
    //   Models: global event counter, hot signal written by all regions,
    //           any "everyone always writes" pattern on the simulation hot path.
    //
    //   This is the key difference from Experiment 3:
    //     Exp 3: only 1 winner per round (others do nothing after losing CAS)
    //     Exp 4: ALL threads write on EVERY iteration → sustained cache-line thrash
    // ========================================================================
    sep("EXPERIMENT 4: Shared Variable Contention  (mutex vs atomic vs shard)");
    std::cout << "  Variable:  how the shared counter is protected\n";
    std::cout << "  Constant:  " << SHARED_THREADS << " threads, all write every iteration, "
              << SHARED_ITERS << " iters each\n";
    std::cout << "  Pattern:   ALL threads increment ONE variable on EVERY iteration\n";
    std::cout << "             (models: global event counter, hot signal, profiling var)\n\n";

    std::cout << "  Run G — mutex + int\n";
    std::cout << "           lock_guard → ++counter → unlock  (every iteration)\n";
    double eps_smutex = bench_shared_mutex(SHARED_THREADS, SHARED_ITERS);
    std::cout << "           increments/sec: " << std::fixed << std::setprecision(0)
              << eps_smutex << "\n\n";

    std::cout << "  Run H — atomic<int> fetch_add\n";
    std::cout << "           fetch_add(1, relaxed)  [LOCK XADD on x86]\n";
    std::cout << "           still contended: cache line bounces between cores\n";
    double eps_satomic = bench_shared_atomic(SHARED_THREADS, SHARED_ITERS);
    std::cout << "           increments/sec: " << eps_satomic << "\n\n";

    std::cout << "  Run I — per-thread shard + merge\n";
    std::cout << "           each thread owns a cache-line-padded local counter\n";
    std::cout << "           merge (sum shards) happens once at end, not on hot path\n";
    double eps_ssharded = bench_shared_sharded(SHARED_THREADS, SHARED_ITERS);
    std::cout << "           increments/sec: " << eps_ssharded << "\n";

    print_table("Experiment 4 result — shared variable contention:", {
        {"mutex + int         (serialized)", eps_smutex},
        {"atomic fetch_add    (RMW, still contended)", eps_satomic},
        {"per-thread shard    (zero sharing)", eps_ssharded},
    });

    std::cout << "\n  Interpretation:\n"
              << "    mutex + int:      every thread serializes through the lock.\n"
              << "                      At N threads: throughput barely exceeds single-thread.\n"
              << "    atomic fetch_add: no mutex, but cache line still bounces between cores.\n"
              << "                      MESI protocol: only one core 'owns' the line per write.\n"
              << "                      Scales poorly: more threads = more cache-line transfers.\n"
              << "    per-thread shard: zero shared state on hot path. Each thread owns its\n"
              << "                      cache line exclusively — no coherence traffic at all.\n"
              << "                      Throughput scales linearly with thread count.\n"
              << "    Real-world use: celeris Profiler uses atomic fetch_add (simple, correct).\n"
              << "                    Sharding is the next optimization: replace one global\n"
              << "                    atomic with per-thread counters merged at barrier time.\n";

    // ========================================================================
    // Combined summary
    // ========================================================================
    sep("COMBINED SUMMARY  (all three strategies, same modern engine)");
    print_table("Full comparison:", {
        {"COARSE_GRAINED (1 global mutex)", eps_coarse},
        {"FINE_GRAINED   (1 mutex/signal)", eps_fine},
        {"ATOMIC         (lock-free)",      eps_atomic},
    });

    std::cout << R"(
  What each speedup measures:
    COARSE → FINE   : granularity effect   (fewer threads blocked per op)
    FINE   → ATOMIC : primitive effect     (syscall overhead removed)
    COARSE → ATOMIC : combined effect      (both improvements together)
)";

    // ========================================================================
    // Strategy swap demo
    // ========================================================================
    sep("STRATEGY PATTERN — Runtime Swap Demo");
    std::cout << "  Same SimulationEngine instance; strategy swapped between runs.\n\n";
    {
        SimulationEngine engine(num_threads, SyncMode::FINE_GRAINED);
        populate_engine(engine, num_events);

        std::cout << "  Active: " << engine.strategy().name() << "\n";
        engine.set_strategy(SyncMode::ATOMIC);
        std::cout << "  After set_strategy(ATOMIC): " << engine.strategy().name() << "\n";
        engine.set_strategy(SyncMode::COARSE_GRAINED);
        std::cout << "  After set_strategy(COARSE): " << engine.strategy().name() << "\n";
        engine.set_strategy(SyncMode::FINE_GRAINED);
        std::cout << "  Restored: " << engine.strategy().name() << "\n";
        std::cout << "  (ISyncStrategy vtable swap — zero changes to WorkerThread or engine)\n";
    }

    // ========================================================================
    // Legacy engine — educational only
    // ========================================================================
    sep("LEGACY ENGINE  (educational — not a clean benchmark comparison)");
    std::cout << "  NOTE: this is a different engine implementation (not just a strategy).\n";
    std::cout << "  It shows what the code looked like before the refactoring:\n";
    std::cout << "    - Hand-rolled barrier (mutex + CV + generation counter)\n";
    std::cout << "    - volatile bool stop_flag\n";
    std::cout << "    - No startup latch (threads race with initialization)\n";
    std::cout << "    - Two coarse global mutexes (event_lock + signal_lock)\n\n";

    {
        LegacySimEngine legacy(num_threads);
        populate_legacy(legacy, num_events);

        auto t0 = Clock::now();
        legacy.run_all();
        auto t1 = Clock::now();
        double elapsed = std::chrono::duration<double>(t1 - t0).count();

        auto& lp = legacy.profiler();
        double eps = elapsed > 0 ? lp.total_events.load() / elapsed : 0.0;

        std::cout << "  ┌────────────────────────────────────────────┐\n";
        std::cout << "  │  Legacy coarse-grained engine              │\n";
        std::cout << "  ├────────────────────────────────────────────┤\n";
        std::cout << "  │  Total events   : " << std::setw(24) << lp.total_events.load()   << "  │\n";
        std::cout << "  │  Delta cycles   : " << std::setw(24) << lp.delta_cycles.load()   << "  │\n";
        std::cout << "  │  Signal updates : " << std::setw(24) << lp.signal_updates.load() << "  │\n";
        std::cout << "  │  Process evals  : " << std::setw(24) << lp.process_evals.load()  << "  │\n";
        std::cout << "  │  Wall time (ms) : " << std::setw(24) << std::fixed << std::setprecision(2)
                  << elapsed * 1000.0 << "  │\n";
        std::cout << "  │  Events/sec     : " << std::setw(24) << std::setprecision(0) << eps << "  │\n";
        std::cout << "  └────────────────────────────────────────────┘\n";
        std::cout << "\n  ⚠  Do not compare this directly with Experiment 1/2:\n"
                  << "     different engine code, different process model, no startup latch.\n";
    }

    std::cout << "\n";
    return 0;
}
