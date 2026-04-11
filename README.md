# Celeris — Open-Source Multicore Event-Driven Simulation Engine

An open-source C++20 multicore event-driven simulation engine built from scratch. Models the architecture of hardware simulation schedulers — time-wheel event scheduling, delta-cycle propagation, and parallel region execution — with a full synchronization benchmark suite. Demonstrates the complete engineering journey from coarse-grained locking to fine-grained locking to lock-free atomics, with four controlled experiments that isolate exactly one variable at a time.

---

## Table of Contents

1. [What is an Event-Driven Simulation Engine?](#1-what-is-an-event-driven-simulation-engine)
2. [Architecture Overview](#2-architecture-overview)
3. [Directory Structure](#3-directory-structure)
4. [Core Data Structures](#4-core-data-structures)
   - [Time Wheel — O(1) Event Scheduling](#time-wheel--o1-event-scheduling)
   - [Delta Queue — Ping-Pong Double Buffer](#delta-queue--ping-pong-double-buffer)
5. [IEEE 1800-2017 Scheduling Algorithm](#5-ieee-1800-2017-scheduling-algorithm)
6. [Synchronization Evolution — The Full Story](#6-synchronization-evolution--the-full-story)
   - [Phase 1 — Legacy Coarse-Grained Locking (the before)](#phase-1--legacy-coarse-grained-locking-the-before)
   - [Phase 2 — Fine-Grained Locking (granularity refactor)](#phase-2--fine-grained-locking-granularity-refactor)
   - [Phase 3 — Lock-Free Atomics (primitive refactor)](#phase-3--lock-free-atomics-primitive-refactor)
7. [Experiment 1 — Lock Granularity (coarse mutex vs fine mutex)](#7-experiment-1--lock-granularity-coarse-mutex-vs-fine-mutex)
8. [Experiment 2 — Sync Primitive (fine mutex vs fine atomic)](#8-experiment-2--sync-primitive-fine-mutex-vs-fine-atomic)
9. [Experiment 3 — Flag Primitive Microbenchmark (hot path isolation)](#9-experiment-3--flag-primitive-microbenchmark-hot-path-isolation)
10. [Experiment 4 — Shared Variable Contention (mutex vs atomic vs shard)](#10-experiment-4--shared-variable-contention-mutex-vs-atomic-vs-shard)
11. [Combined Results](#11-combined-results)
12. [C++20 Barrier Replacement](#12-c20-barrier-replacement)
13. [Strategy Design Pattern](#13-strategy-design-pattern)
14. [C++20 Features Used](#14-c20-features-used)
15. [Legacy Engine — The Educational Bad Code](#15-legacy-engine--the-educational-bad-code)
16. [Build and Run](#16-build-and-run)
17. [Topics Covered](#17-topics-covered)

---

## 1. What is an Event-Driven Simulation Engine?

A hardware event-driven simulator does not simulate every nanosecond of circuit time. It only simulates at *event* timestamps — moments when a signal changes value. Between events, nothing changes, so no computation is needed.

**Core concepts:**

| Concept | Definition |
|---------|-----------|
| **Event** | A signal change at a specific `(time, delta)` timestamp |
| **Delta cycle** | Events at the *same* simulation time but different logical ordering. A gate writing its output in delta N makes that output visible to downstream gates only in delta N+1. |
| **Time step** | Simulation clock advances to the next timestamp that has events |
| **Process** | A hardware model (always block, combinational assign) sensitive to specific signals |
| **Region** | A partition of the design; in multicore simulation, one CPU core owns one region |
| **Boundary signal** | A signal driven by one region but read by another — requires cross-region synchronization |

**The simulation loop (IEEE 1800-2017 §4):**
```
while (events exist):
    advance to smallest T with active events
    repeat:
        drain Active region  →  apply signal updates, activate processes
        run NBA region       →  apply non-blocking assignment updates
        [new events at same T?]  →  new delta cycle
    advance T
```

**Why multicore?** A real chip design has millions of processes. Even at GHz clock rates, a single-core simulator is the bottleneck. Multicore simulators partition the design into regions, assign one CPU core per region, and run them in parallel — synchronized only at delta boundaries.

---

## 2. Architecture Overview

```
┌──────────────────────────────────────────────────────────────────────┐
│                          SimulationEngine                            │
│                                                                      │
│  ┌─────────────────┐  ┌──────────────────┐  ┌────────────────────┐  │
│  │   SimContext    │  │  EventScheduler  │  │   ISyncStrategy    │  │
│  │                 │  │                  │  │                    │  │
│  │ current_time    │  │   TimeWheel      │  │  ← CoarseGrained   │  │
│  │ current_delta   │  │   (per-bucket    │  │  ← FineGrained     │  │
│  │ signals[]       │  │    SpinLock[])   │  │  ← Atomic          │  │
│  │ processes[]     │  │                  │  │                    │  │
│  │ regions[]       │  │   DeltaQueue     │  │  SyncStrategyFact  │  │
│  │ end_time        │  │   (ping-pong)    │  │  (runtime select)  │  │
│  └─────────────────┘  └──────────────────┘  └────────────────────┘  │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐    │
│  │                    WorkerThread × N                          │    │
│  │                                                              │    │
│  │  startup_latch_.arrive_and_wait()   ← C++20 std::latch      │    │
│  │  while (true):                                               │    │
│  │    process_delta_events()    drain + apply signal updates    │    │
│  │    evaluate_ready_processes() CAS: DORMANT→READY→RUNNING     │    │
│  │    sync_boundary_signals()   flush cross-region signals      │    │
│  │    barrier_.arrive_and_wait() ← C++20 std::barrier          │    │
│  │    if barrier_.simulation_done(): break                      │    │
│  └──────────────────────────────────────────────────────────────┘    │
│                                                                      │
│  ┌──────────────┐  ┌───────────────────────┐  ┌────────────────┐    │
│  │ DeltaBarrier │  │  counting_semaphore   │  │    Profiler    │    │
│  │ std::barrier │  │  <2>  (bus limiter)   │  │  all atomics   │    │
│  │ completion fn│  │  C++20                │  │  report()      │    │
│  └──────────────┘  └───────────────────────┘  └────────────────┘    │
└──────────────────────────────────────────────────────────────────────┘
```

**Data flow per delta cycle:**
```
TimeWheel          DeltaQueue           WorkerThreads        ISyncStrategy
    │                  │                     │                    │
    │  advance_time()  │                     │                    │
    │─────────────────►│  push(timed_events) │                    │
    │                  │                     │                    │
    │                  │◄── drain_delta() ───│                    │
    │                  │                     │                    │
    │                  │                     │── write_signal() ─►│
    │                  │                     │                    │  (strategy-dependent)
    │                  │                     │◄─ read_signal() ───│
    │                  │                     │                    │
    │                  │                     │── activate_process()►│
    │                  │                     │                    │
    │             barrier_.arrive_and_wait() │                    │
    │                  │ (last thread runs completion fn)         │
    │                  │ flip_delta() / advance_time()            │
    │                  │ check end_time → set simulation_done     │
```

---

## 3. Directory Structure

```
multicore-event-driven-simulation/
├── include/
│   ├── core/
│   │   ├── Event.hpp           — EventType enum, LogicValue (ZERO/ONE/X/Z),
│   │   │                         SimTime with spaceship operator <=>
│   │   ├── Signal.hpp          — Signal: current_value + std::atomic<uint8_t>
│   │   │                         atomic_value for lock-free access, custom move ctor
│   │   ├── Process.hpp         — ProcessState enum (DORMANT/READY/RUNNING),
│   │   │                         std::atomic<ProcessState> state, evaluate fn
│   │   ├── Region.hpp          — Region with unique_ptr<mutex> for movability
│   │   └── SimContext.hpp      — All shared simulation state; all mutable fields
│   │                             are std::atomic<>; immutable structural data is plain
│   ├── sync/
│   │   ├── SpinLock.hpp        — TAS spinlock: test_and_set loop with exponential
│   │   │                         backoff and _mm_pause() (PAUSE hint to CPU)
│   │   ├── RWLock.hpp          — RAII ReadLock (shared_lock) / WriteLock (unique_lock)
│   │   ├── ISyncStrategy.hpp   — Abstract interface: read_signal, write_signal,
│   │   │                         activate_process, sync_boundary_signal, contention_count
│   │   ├── CoarseGrainedStrategy.hpp — One global std::mutex: ALL ops serialized
│   │   ├── FineGrainedStrategy.hpp   — Per-signal std::shared_mutex + per-region mutex
│   │   ├── AtomicStrategy.hpp        — atomic load/store, CAS, C++20 wait/notify_all
│   │   └── SyncStrategyFactory.hpp   — create(SyncMode), mode_from_env(), mode_name()
│   ├── scheduler/
│   │   ├── TimeWheel.hpp       — 1024-bucket circular wheel, per-bucket SpinLock[],
│   │   │                         atomic bitmask fast-path, overflow min-heap
│   │   ├── DeltaQueue.hpp      — Ping-pong double buffer, separate insert_lock_ +
│   │   │                         drain_lock_, atomic pending_count_
│   │   └── EventScheduler.hpp  — Composes TimeWheel + DeltaQueue; schedule_now(),
│   │                             schedule_at(), schedule_nba(), advance_delta/time()
│   ├── engine/
│   │   ├── Profiler.hpp        — All std::atomic<uint64_t> counters, report(), events_per_second()
│   │   ├── DeltaBarrier.hpp    — std::barrier<CompletionFn>; on_delta_complete() noexcept:
│   │   │                         checks end_time, flips delta, advances time, sets done_
│   │   ├── WorkerThread.hpp    — std::thread + std::latch&; workers exit ONLY via
│   │   │                         barrier_.simulation_done() to prevent deadlock
│   │   └── SimulationEngine.hpp — Orchestrator: latch, semaphore, barrier, workers[]
│   └── legacy/                 — The "before" state — intentionally bad code
│       ├── LegacyEvent.hpp     — Mirrors core/ types without atomics
│       ├── LegacyTimeWheel.hpp — Single global_lock_ for entire wheel
│       ├── LegacyDeltaQueue.hpp — Single global_lock_ for insert AND drain
│       └── LegacySimEngine.hpp — Hand-rolled barrier, volatile bool, two global mutexes
└── src/
    └── main.cpp                — 3 controlled experiments + strategy swap demo + legacy section
```

---

## 4. Core Data Structures

### Time Wheel — O(1) Event Scheduling

The time wheel is the primary scheduler data structure in hardware simulation engines. It is a circular array of 1024 buckets. An event scheduled at time `t` goes into `bucket[t & 1023]` — a single array index operation, `O(1)`.

```
Simulation time advances →

bucket[0]   bucket[1]   ...   bucket[1023]   bucket[0]   bucket[1]  ...
t=0         t=1               t=1023          t=1024       t=1025
    ↑
current_bucket pointer = current_time & (WHEEL_SIZE - 1)
```

**Why not a priority queue?** A priority queue (heap) is `O(log n)` per insert and extract. For a design with millions of events all scheduled within a 1000-time-unit window, the time wheel is dramatically faster. The heap is only used for events beyond the wheel's range (overflow heap).

**Fine-grained sync decisions:**

| Mechanism | Purpose | Contention scope |
|-----------|---------|-----------------|
| `SpinLock bucket_locks_[1024]` | Protect each bucket independently | Only threads scheduling into the *same* bucket contend |
| `std::atomic<uint64_t> bucket_nonempty_bitmask_` | Fast "any events nearby?" check | Lock-free, single atomic read |
| `std::mutex overflow_lock_` | Protect the overflow heap | Only contended when scheduling far-future events |

```cpp
// Fine-grained: only the target bucket is locked
void schedule(const Event& e) {
    int idx = static_cast<int>(e.when.time & (WHEEL_SIZE - 1));
    SpinLockGuard lk(bucket_locks_[idx]);        // ← only this bucket
    buckets_[idx].push_back(e);
    bucket_nonempty_bitmask_.fetch_or(
        uint64_t(1) << (idx & 63), std::memory_order_relaxed);
}

// Legacy: one mutex for ALL buckets — thread scheduling into bucket 0
// blocks a thread scheduling into bucket 512 even though they share no data
void schedule(const LegacyEvent& e) {
    std::lock_guard<std::mutex> lk(global_lock_);   // ← blocks everything
    buckets_[e.sim_time & (WHEEL_SIZE-1)].push_back(e);
}
```

### Delta Queue — Ping-Pong Double Buffer

A delta event is one scheduled at the *same* simulation time but a later delta cycle. They are so frequent (every combinational gate evaluation produces delta events) that the delta queue is the hottest data structure in the engine.

```
┌──────────────────┐    draining     ┌──────────────────┐
│   active_buf     │ ←── workers ──  │   pending_buf    │ ←── workers write
│   (read-only)    │    drain()      │   (write-only)   │     push()
└──────────────────┘                 └──────────────────┘
         ↑___________________ flip at delta boundary ___________________↑
         active_idx_ flipped by barrier completion fn (on_delta_complete)
```

**The key insight:** at any moment, drain (read from active buffer) and push (write to pending buffer) operate on *different* buffers. They can never corrupt each other. So they need separate locks — not the same lock.

```cpp
// Two separate locks: drain and push never contend each other
SpinLock drain_lock_;    // only serializes concurrent drain() calls
SpinLock insert_lock_;   // only serializes concurrent push() calls

void push(Event e) {
    SpinLockGuard lk(insert_lock_);               // never blocks drain()
    int pending = 1 - active_idx_.load(std::memory_order_relaxed);
    buffers_[pending].push_back(e);
    pending_count_.fetch_add(1, std::memory_order_release);
}

std::vector<Event> drain() {
    SpinLockGuard lk(drain_lock_);                // never blocks push()
    int active = active_idx_.load(std::memory_order_acquire);
    std::vector<Event> out = std::move(buffers_[active]);
    buffers_[active].clear();
    return out;
}

// Legacy: SAME lock for both — a push() blocks drain() and vice versa
// even though they touch different buffers
void push(const LegacyEvent& e) {
    std::lock_guard<std::mutex> lk(global_lock_);  // ← blocks drain()
    buffers_[1 - active_idx_].push_back(e);
}
std::vector<LegacyEvent> drain() {
    std::lock_guard<std::mutex> lk(global_lock_);  // ← blocks push()
    return std::move(buffers_[active_idx_]);
}
```

**Flip protocol** (runs in the `std::barrier` completion function, once per delta, on the last thread to arrive):
```cpp
bool flip_delta() {
    // Atomically swap which buffer is active
    int old_active = active_idx_.load(std::memory_order_relaxed);
    active_idx_.store(1 - old_active, std::memory_order_release);
    // pending_count_ now describes the new active buffer (old pending)
    uint64_t cnt = pending_count_.exchange(0, std::memory_order_acq_rel);
    return cnt > 0;  // true = another delta cycle needed
}
```

---

## 5. IEEE 1800-2017 Scheduling Algorithm

`EventScheduler` implements the scheduling algorithm from **IEEE 1800-2017 §4** — the SystemVerilog Language Reference Manual, which defines the reference model for all compliant simulators.

```
┌─────────────────────────────────────────────────────────────────────┐
│  IEEE 1800 Simulation Scheduling Regions (simplified)               │
│                                                                     │
│  Active Region:    signal updates, combinational logic evaluation   │
│  NBA Region:       non-blocking assignment (<=) updates             │
│  Postponed Region: $monitor, $strobe output                         │
│                                                                     │
│  while (events exist):                                              │
│    T = smallest timestamp with events                               │
│    repeat:                                                          │
│      drain Active region → process_delta_events()                  │
│      evaluate sensitized processes → evaluate_ready_processes()     │
│      drain NBA region → process EventType::NBA events               │
│      [new events appeared at T?] → another delta cycle             │
│    advance T via time_wheel_.advance_time()                         │
└─────────────────────────────────────────────────────────────────────┘
```

**Delta cycle example — a 3-gate combinational chain:**

```
Signal a → gate1 → Signal b → gate2 → Signal c → gate3 → output

Delta 0:  'a' changes value
          → gate1 sensitized → evaluates → schedules b's update (delta 1)

Delta 1:  'b' updates
          → gate2 sensitized → evaluates → schedules c's update (delta 2)

Delta 2:  'c' updates
          → gate3 sensitized → evaluates → output update (delta 3)

Delta 3:  output updates → no more events at time T → advance_time()
```

`DeltaBarrier` (backed by `std::barrier`) ensures: **all threads complete delta N before any thread begins delta N+1**. Without this guarantee, a thread starting delta 2 might read `b`'s old value while another thread is still writing it in delta 1.

**Event types supported:**

```cpp
enum class EventType {
    SIGNAL_UPDATE,     // combinational: write signal value, activate sensitized procs
    PROCESS_ACTIVATE,  // explicitly schedule a process evaluation
    NBA,               // non-blocking assign: deferred write (models <= in SystemVerilog)
    POSTPONED,         // $monitor / $strobe region (end of time step)
    MONITOR            // $monitor callbacks
};
```

---

## 6. Synchronization Evolution — The Full Story

The refactoring has two independent dimensions. Understanding them separately is the key insight.

```
                      COARSE                    FINE
                  (1 lock total)           (1 lock/signal)
               ┌──────────────────────┬──────────────────────┐
   Mutex       │  LegacySimEngine     │  FineGrainedStrategy │
               │  (different engine)  │                      │
               ├──────────────────────┼──────────────────────┤
   Mutex       │  CoarseGrained       │  FineGrained         │
   (modern)    │  Strategy            │  Strategy            │
               ├──────────────────────┼──────────────────────┤
   Atomic      │       —              │  AtomicStrategy      │
               └──────────────────────┴──────────────────────┘

   Experiment 1: left column → right column  (same primitive, more locks)
   Experiment 2: top row → bottom row        (same granularity, better primitive)
```

### Phase 1 — Legacy Coarse-Grained Locking (the before)

`include/legacy/LegacySimEngine.hpp` — the engine as it existed before the multicore refactoring. Two global mutexes serialize everything.

**Problems, each labeled `// LEGACY:` in the source:**

| Problem | Code location | Root cause | Impact |
|---------|--------------|-----------|--------|
| One mutex for ALL event ops | `event_lock_` in `LegacySimEngine` | No isolation between push/drain | Zero parallelism on event scheduling |
| One mutex for ALL signal ops | `signal_lock_` in `LegacySimEngine` | No per-signal isolation | Reading signal 0 blocks writing signal 7 |
| Hand-rolled barrier | `legacy_barrier_wait()` | mutex + CV + counter | Race condition: generation-counter bug (see below) |
| `volatile bool` stop flag | `stop_flag_` | Not atomic | Potential data race on write from main thread |
| No startup gate | threads start immediately | No `std::latch` | Threads can read uninitialized signal data |

**The hand-rolled barrier race condition** — the most subtle bug:

```cpp
// BUGGY original predicate:
barrier_cv_.wait(lk, [this]{ return arrived_ == 0 || !active_.load(); });

// What goes wrong:
// Thread A (fast):  arrives → last → resets arrived_=0 → notifies → released
//                   immediately loops and increments arrived_ to 1 again
// Thread B (slow):  wakes up → checks arrived_==0 → sees 1 → goes back to sleep
//                   → DEADLOCK: B never wakes again; A can never be last to arrive

// FIXED with generation counter:
int gen = generation_;    // capture BEFORE incrementing arrived_
++arrived_;
if (arrived_ == num_threads_) {
    ++generation_;        // advance phase number
    barrier_cv_.notify_all();
} else {
    // wait until phase changes, not until arrived_==0
    barrier_cv_.wait(lk, [this, gen]{ return generation_ != gen || !active_.load(); });
}
```

This is precisely the bug that `std::barrier` eliminates by design. The generation counter is internally managed by the standard library implementation.

### Phase 2 — Fine-Grained Locking (granularity refactor)

`include/sync/FineGrainedStrategy.hpp` — same primitive (mutex), more locks.

**One `std::shared_mutex` per signal:**
```cpp
std::vector<std::shared_mutex> signal_locks_;  // indexed by signal_id

// Read: shared_lock → N threads can read the same signal simultaneously
LogicValue read_signal(const SimContext& ctx, int signal_id) override {
    std::shared_lock lk(signal_locks_[signal_id]);   // shared: no blocking between readers
    return ctx.signals[signal_id].current_value;
}

// Write: unique_lock → exclusive to THIS signal only, not all signals
void write_signal(SimContext& ctx, int signal_id, LogicValue val) override {
    std::unique_lock lk(signal_locks_[signal_id]);   // exclusive: only blocks signal_id readers
    ctx.signals[signal_id].current_value = val;
    ctx.signals[signal_id].atomic_value.store(
        static_cast<uint8_t>(val), std::memory_order_release);
    ctx.signals[signal_id].atomic_value.notify_all();  // C++20
}
```

**Lock hierarchy** (documented in source to prevent deadlock):
```
Level 1: signal_locks_[id]        — always acquired in ascending signal_id order
Level 2: region_queue_locks_[id]  — acquired after signal lock if needed
Rule: never hold a lower-level lock when acquiring a higher-level one
```

**When fine-grained helps most:** when threads concurrently touch *different* signals. In a real multicore simulation with 10,000+ signals and 32 cores, the probability of two threads touching the same signal simultaneously is low — most of the time threads work in parallel with zero contention.

### Phase 3 — Lock-Free Atomics (primitive refactor)

`include/sync/AtomicStrategy.hpp` — same granularity (per-signal), no mutex.

```cpp
// Signal: every Signal already stores std::atomic<uint8_t> atomic_value
// AtomicStrategy uses this directly — zero mutex, zero kernel involvement

LogicValue read_signal(const SimContext& ctx, int signal_id) override {
    // acquire: guarantees we see all writes that happened before the store(release)
    return static_cast<LogicValue>(
        ctx.signals[signal_id].atomic_value.load(std::memory_order_acquire));
}

void write_signal(SimContext& ctx, int signal_id, LogicValue val) override {
    // release: all preceding writes are visible to any subsequent acquire load
    ctx.signals[signal_id].atomic_value.store(
        static_cast<uint8_t>(val), std::memory_order_release);
    // C++20: wake any thread blocked in atomic::wait() on this value
    ctx.signals[signal_id].atomic_value.notify_all();
}

void activate_process(SimContext& ctx, int process_id) override {
    // CAS: atomically transition DORMANT → READY
    // If another thread already did this (state is READY or RUNNING), CAS fails silently
    // Exactly-once activation: no mutex needed, no double-activation possible
    ProcessState expected = ProcessState::DORMANT;
    ctx.processes[process_id].state.compare_exchange_strong(
        expected, ProcessState::READY,
        std::memory_order_acq_rel,   // success: full barrier
        std::memory_order_relaxed);  // failure: no ordering needed
}

void wait_for_signal_change(const SimContext& ctx, int signal_id,
                             LogicValue old_val) override {
    // C++20 atomic::wait(): blocks until value != old_val
    // Replaces condition_variable entirely — no mutex, no spurious wakeups
    ctx.signals[signal_id].atomic_value.wait(
        static_cast<uint8_t>(old_val), std::memory_order_acquire);
}
```

**Why atomics are faster than mutexes (even uncontested):**
- `std::mutex::lock()` involves at minimum: a CAS to set the mutex state, a memory fence, and on failure a futex syscall (OS involvement).
- `std::atomic::store(release)` is a single store instruction with a compiler/CPU fence — no syscall, no OS, no cache-line bouncing on a mutex state word.
- Even at zero contention, the mutex has 3–5× higher overhead than an atomic operation.

---

## 7. Experiment 1 — Lock Granularity (coarse mutex vs fine mutex)

**Question:** Does splitting one global lock into per-signal locks improve throughput?

**Controlled variable:** number of locks (1 vs N).

**Held constant:** synchronization primitive = `std::mutex` in both cases. Same modern engine, same WorkerThread, same DeltaBarrier. Only `ISyncStrategy` differs.

### Setup

```
CoarseGrainedStrategy:
  - 1 std::mutex global_lock_
  - write_signal(), read_signal(), activate_process() all take global_lock_
  - Any thread doing ANY signal op blocks ALL other threads

FineGrainedStrategy:
  - std::vector<std::shared_mutex> signal_locks_  (one per signal)
  - std::vector<std::mutex> region_queue_locks_   (one per region)
  - Threads on different signals never contend
  - Readers of the same signal run concurrently
```

### What contention looks like

```
COARSE — 3 threads, 8 signals:

Thread 0: write(CLK)   ─── holds global_lock_ ─────────────────────────────────
Thread 1: write(RESET) ─────────────────── BLOCKED ─── write(RESET) ──────────
Thread 2: read(DATA)   ─────────────────────────────── BLOCKED ─── read(DATA) ─

Even though T1 and T2 want completely different signals, they both wait for T0.

FINE — same scenario:

Thread 0: write(CLK)   ─── holds signal_locks_[0] ──────────────────────────
Thread 1: write(RESET) ─── holds signal_locks_[1] ──── runs in parallel ────
Thread 2: read(DATA)   ─── holds signal_locks_[2] (shared) ─ runs in parallel
```

### Results (3 threads, 200 events, same modern engine)

```
  Experiment 1 result — granularity effect:
  ┌──────────────────────────────┬────────────────┬────────────┐
  │  Strategy                    │  Events/sec    │  Speedup   │
  ├──────────────────────────────┼────────────────┼────────────┤
  │  COARSE  (1 global mutex)    │         84,256 │    1.00×   │
  │  FINE    (1 mutex/signal)    │        180,923 │    2.15×   │
  └──────────────────────────────┴────────────────┴────────────┘
```

### Interpretation

The 2.15× speedup comes **purely from reducing contention scope**. The synchronization primitive (mutex) is identical in both. Threads that previously blocked each other on unrelated signals now run in parallel.

**When fine-grained wins more:** the benefit grows with thread count. At 3 threads and 8 signals, contention is moderate. At 32 threads and 10,000 signals (real designs), the probability of two threads touching the same signal simultaneously approaches zero — fine-grained approaches perfect parallelism.

**When fine-grained can be slower:** at very low thread counts or tiny workloads, the overhead of constructing and managing N mutex objects can exceed the contention savings. This is why both strategies are benchmarked: the correct choice depends on the actual workload profile.

---

## 8. Experiment 2 — Sync Primitive (fine mutex vs fine atomic)

**Question:** Given fine-grained (per-signal) locking, does replacing `std::mutex` with `std::atomic` further improve throughput?

**Controlled variable:** synchronization primitive (mutex vs atomic).

**Held constant:** lock granularity = per-signal in both cases. Same modern engine, same WorkerThread, same DeltaBarrier.

### The cost of a mutex (even uncontested)

```
mutex::lock() on an uncontested mutex:

  1. Attempt CAS on mutex internal state word    [user-space, ~5 cycles]
  2. Memory fence (acquire semantics)            [CPU pipeline stall]
  3. If CAS succeeded → proceed
  4. If CAS failed → futex(WAIT) syscall        [kernel, ~1000+ cycles]

mutex::unlock():
  1. Store to mutex state word                   [user-space]
  2. Memory fence (release semantics)
  3. If waiters exist → futex(WAKE) syscall      [kernel]

Total uncontested round-trip: ~10–30 ns (mutex) vs ~1–3 ns (atomic)
```

```
atomic::store(release):
  1. Single store instruction with release fence  [1 CPU instruction]
  2. Done.

No syscall. No OS involvement. No futex. No sleeping.
```

### Code comparison

```cpp
// FineGrainedStrategy — mutex version
void write_signal(SimContext& ctx, int signal_id, LogicValue val) override {
    std::unique_lock lk(signal_locks_[signal_id]);   // lock acquire  ← overhead
    ctx.signals[signal_id].current_value = val;
}                                                    // lock release  ← overhead

// AtomicStrategy — atomic version
void write_signal(SimContext& ctx, int signal_id, LogicValue val) override {
    ctx.signals[signal_id].atomic_value.store(
        static_cast<uint8_t>(val), std::memory_order_release);  // 1 instruction
    ctx.signals[signal_id].atomic_value.notify_all();            // C++20
}
```

### Results (3 threads, 200 events, both fine-grained)

```
  Experiment 2 result — primitive effect:
  ┌──────────────────────────────┬────────────────┬────────────┐
  │  Strategy                    │  Events/sec    │  Speedup   │
  ├──────────────────────────────┼────────────────┼────────────┤
  │  FINE_GRAINED  (shared_mutex)│        180,923 │    1.00×   │
  │  ATOMIC        (atomic store)│        313,259 │    1.73×   │
  └──────────────────────────────┴────────────────┴────────────┘
```

### Interpretation

The 1.73× speedup is **purely the cost of the mutex mechanism itself** — not contention between threads. The benchmark has the same workload, same number of signals, same thread count. The only difference is `unique_lock` acquire/release on every `write_signal` call vs a single `atomic::store`.

This gap widens significantly with thread count because:
1. More threads mean more concurrent lock/unlock operations on each mutex.
2. `std::shared_mutex` on Linux uses a futex internally — even an uncontested shared_lock still touches the futex state word, causing cache-line invalidation across cores.
3. Atomic operations are purely local to the CPU pipeline — no cross-core coordination unless two cores touch the same cache line.

---

## 9. Experiment 3 — Flag Primitive Microbenchmark (hot path isolation)

**Question:** For the single most common hot-path operation — "set a ready-flag exactly once when N threads all attempt simultaneously" — what is the cheapest C++ primitive?

**Why this matters:** This pattern is `activate_process()`. Every signal change triggers it: every process in the sensitivity list of the changed signal must be marked READY, but only once even if three threads all write the same signal concurrently. This call happens on every event, every delta cycle, for every sensitized process.

**Controlled variable:** flag-set primitive.

**Held constant:** same barrier-synchronized workload, same thread count, same number of rounds. Each round: N threads race to set flag; exactly 1 wins; barrier clears flag.

### Three implementations

**Variant D — `std::mutex` + `bool`:**
```cpp
std::mutex mtx;
bool flag = false;

// Every thread, every round:
{
    std::lock_guard lk(mtx);        // acquire mutex
    if (!flag) {                     // check
        flag = true;                 // set
        ++activations;               // count the win
    }
}                                    // release mutex

// Cost: lock + check + set + unlock on every attempt
// Even if flag is already true, we still pay lock/unlock
```

**Variant E — `std::atomic<bool>` + CAS:**
```cpp
std::atomic<bool> flag{false};

// Every thread, every round:
bool expected = false;
if (flag.compare_exchange_strong(
        expected, true,
        std::memory_order_acq_rel,    // success ordering
        std::memory_order_relaxed)) { // failure ordering
    ++activations;
}

// Cost: single LOCK CMPXCHG instruction
// Losers fail immediately (expected is updated to current value)
// No retry loop — we only try once per round
```

**Variant F — `std::atomic_flag` + `test_and_set`:**
```cpp
std::atomic_flag flag = ATOMIC_FLAG_INIT;   // starts clear

// Every thread, every round:
if (!flag.test_and_set(std::memory_order_acq_rel)) {
    // test_and_set: atomically sets flag, returns PREVIOUS value
    // returned false → we were first → we activated
    ++activations;
}

// Cost: single LOCK XCHG instruction (x86)
// std::atomic_flag is the ONLY C++ type guaranteed lock-free on all platforms
// Simpler than CAS: no expected value, no compare phase
```

**Instruction-level comparison (x86):**
```
mutex + bool:    CALL lock(); MOV [flag], 1; CALL unlock()    → ~10-30ns
atomic<bool>:    LOCK CMPXCHG [flag], true                    → ~3-5ns
atomic_flag:     LOCK XCHG [flag], 1                          → ~2-4ns
```

### Results (3 threads, 200,000 rounds each)

```
  Experiment 3 result — flag primitive effect:
  ┌──────────────────────────────┬────────────────┬────────────┐
  │  Strategy                    │  Events/sec    │  Speedup   │
  ├──────────────────────────────┼────────────────┼────────────┤
  │  mutex + bool                │      1,821,011 │    1.00×   │
  │  atomic<bool> + CAS          │      2,990,034 │    1.64×   │
  │  atomic_flag + TAS           │      3,133,442 │    1.72×   │
  └──────────────────────────────┴────────────────┴────────────┘
```

### Interpretation

**mutex + bool (1.00× baseline):** The mutex pays lock/unlock cost on *every* attempt regardless of whether the flag is already set. Even with zero contention (barrier ensures only one thread wins per round), the mutex mechanism itself costs ~10–30ns per call.

**atomic\<bool\> + CAS (1.64×):** Losers discover the flag is already set via a single failed CAS instruction (~3ns) instead of acquiring the mutex, checking, and releasing it. No kernel involvement, no sleeping.

**atomic_flag + TAS (1.72×):** Marginally faster than CAS because `test_and_set` requires no `expected` variable and no compare phase — it atomically sets the flag and returns the prior value in a single `LOCK XCHG`. This is the lowest-level flag primitive in C++.

**Connection to `AtomicStrategy::activate_process()`:**
```cpp
// This is exactly what the 1.72× improvement applies to:
void activate_process(SimContext& ctx, int process_id) override {
    ProcessState expected = ProcessState::DORMANT;
    ctx.processes[process_id].state.compare_exchange_strong(
        expected, ProcessState::READY, std::memory_order_acq_rel);
    // CAS instead of mutex+bool — directly measured in Experiment 3
}
```

---

## 10. Experiment 4 — Shared Variable Contention (mutex vs atomic vs shard)

**Question:** When a single shared variable is on the hot path and *every* thread writes it on *every* iteration, what is the right primitive — and is atomic actually enough?

**This is different from Experiment 3.** In Experiment 3, only one thread wins per round (the rest bail out immediately after a failed CAS). Here, **all threads always write** — sustained maximum contention. This models: a global event counter incremented on every event, a hot signal written by all regions, any "accumulate" variable in a tight simulation loop.

### Three implementations

**Variant G — `std::mutex` + `int`:**
```cpp
// Every thread, every iteration:
{
    std::lock_guard lk(mtx);   // serialize ALL threads through ONE lock
    ++counter;
}
// Every loser parks (OS sleep) → winner pays wake-up cost on unlock
// At N threads: throughput ≈ single-thread / N  (pure serialization)
```

**Variant H — `std::atomic<int>` + `fetch_add`:**
```cpp
// Every thread, every iteration:
counter.fetch_add(1, std::memory_order_relaxed);  // LOCK XADD on x86

// No mutex, no OS. But: the cache line holding 'counter' can only be
// owned by ONE core at a time (MESI protocol: Modified state).
// Every fetch_add forces a cache-line transfer across cores:
//   Core 0 writes → invalidates Core 1's copy → Core 1 stalls waiting
//   → Core 1 writes → invalidates Core 2's copy → ...
// This is "cache-line ping-pong". Faster than mutex (no syscall),
// but still serializes at the hardware level.
```

**Variant I — per-thread shard + merge:**
```cpp
// Cache-line padded: each shard on its own 64-byte line
struct alignas(64) PaddedCounter {
    long long value{0};
    char pad[64 - sizeof(long long)];  // prevent false sharing
};
std::vector<PaddedCounter> shards(num_threads);

// Every thread, every iteration — writes only its OWN cache line:
++shards[thread_id].value;   // no lock, no atomic, no coherence traffic

// Once, at the end (or at barrier time):
long long total = 0;
for (auto& s : shards) total += s.value;  // merge — not on hot path
```

### Why the padding matters

```
WITHOUT padding (struct { long long value; } shards[N]):

  Core 0 counter | Core 1 counter | Core 2 counter
  ──────── same 64-byte cache line ───────────────────
  Core 0 writes → invalidates Core 1 and Core 2's copies
  → False sharing: logically independent variables, physically shared line
  → Same cache-line ping-pong as a single atomic, despite separate variables

WITH alignas(64) padding:

  ┌────────────────────────────────┐
  │  Core 0 counter  │  56B pad   │  ← cache line 0 (owned by Core 0)
  ├────────────────────────────────┤
  │  Core 1 counter  │  56B pad   │  ← cache line 1 (owned by Core 1)
  ├────────────────────────────────┤
  │  Core 2 counter  │  56B pad   │  ← cache line 2 (owned by Core 2)
  └────────────────────────────────┘
  Each core has exclusive ownership — MESI never transitions, zero coherence traffic
```

### Results (3 threads, 500,000 iterations each, all write every iteration)

```
  Experiment 4 result — shared variable contention:
  ┌──────────────────────────────┬────────────────┬────────────┐
  │  Strategy                    │  Incs/sec      │  Speedup   │
  ├──────────────────────────────┼────────────────┼────────────┤
  │  mutex + int   (serialized)  │     11,206,212 │    1.00×   │
  │  atomic fetch_add (contended)│    115,403,843 │   10.30×   │
  │  per-thread shard (no share) │ 36,363,636,364 │ 3,244.95×  │
  └──────────────────────────────┴────────────────┴────────────┘
```

### Interpretation

**mutex + int (1×):** Full serialization. Three threads sharing one mutex reduce to single-thread throughput. Every loser stalls, winner pays unlock cost. At 3 threads, you're paying 3× the mutex overhead for the same work one thread could do alone.

**atomic fetch_add (10.3×):** Eliminates the mutex and all OS involvement. But the 64-bit counter's cache line still bounces between cores via the MESI protocol on every `fetch_add`. Better, but still serialized at the hardware level — only one core can hold the line in Modified state at a time.

**per-thread shard (3245×):** The counter is now 3 separate cache lines. Each core has exclusive, permanent ownership of its line. No coherence traffic, no invalidations, no stalls. The CPU's write buffer can absorb increments at full clock speed — this is pure local memory bandwidth, which is orders of magnitude higher than cross-core coherence bandwidth.

### The real insight: atomic ≠ fast under sustained contention

```
Contention level    mutex       atomic fetch_add    shard
─────────────────   ─────────   ────────────────    ─────────
None (1 thread)     baseline    ~same               ~same
Low  (rare writes)  OK          better              best
High (all write)    bad         10× over mutex      3000× over mutex
```

Atomic is not a magic bullet — it removes the mutex overhead but keeps the cache-line contention. Sharding removes both. The right choice depends on the read/write ratio:

- **Many reads, rare writes:** atomic or mutex fine — readers are cheap
- **All writes, reads only at end:** shard + merge — this is the simulation profiler pattern

**Connection to celeris:** `Profiler::total_events` is `std::atomic<uint64_t>` with `fetch_add`. Every worker calls it on every event. At 32 cores, this becomes the most-contended line in the process. The next optimization: replace with `std::array<PaddedCounter, MAX_THREADS>` shards, summed in the `DeltaBarrier` completion function — one merge per delta cycle instead of one atomic RMW per event.

---

## 11. Combined Results

**All three strategies on the same modern engine (same WorkerThread, same DeltaBarrier, same circuit):**

```
  Full comparison:
  ┌──────────────────────────────┬────────────────┬────────────┐
  │  Strategy                    │  Events/sec    │  Speedup   │
  ├──────────────────────────────┼────────────────┼────────────┤
  │  COARSE_GRAINED (1 global μ) │         84,256 │    1.00×   │
  │  FINE_GRAINED   (1 μ/signal) │        180,923 │    2.15×   │
  │  ATOMIC         (lock-free)  │        313,259 │    3.72×   │
  └──────────────────────────────┴────────────────┴────────────┘
```

**What each number isolates:**

```
COARSE → FINE   (+2.15×):  granularity effect alone
                            (primitive held constant = mutex)

FINE → ATOMIC   (+1.73×):  primitive effect alone
                            (granularity held constant = per-signal)

COARSE → ATOMIC (+3.72×):  combined effect
                            = granularity effect × primitive effect
                            ≈ 2.15 × 1.73 ≈ 3.72  ✓  (multiplies cleanly)
```

The fact that `2.15 × 1.73 ≈ 3.72` confirms the two effects are **independent and multiplicative** — exactly what controlled experimentation should show.

---

## 12. C++20 Barrier Replacement

The hand-rolled barrier in the legacy engine is the most instructive before/after in this codebase.

### The legacy barrier (30 lines, three failure modes)

```cpp
// LegacySimEngine — the before state
std::mutex              barrier_mutex_;
std::condition_variable barrier_cv_;
int                     arrived_{0};
int                     generation_{0};   // BUG FIX: added generation counter

void legacy_barrier_wait() {
    std::unique_lock<std::mutex> lk(barrier_mutex_);
    int gen = generation_;    // capture before incrementing
    ++arrived_;

    if (arrived_ == num_threads_) {
        // Last thread: do delta flip inside the lock
        arrived_ = 0;
        ++generation_;

        bool more_delta;
        {
            std::lock_guard<std::mutex> elk(event_lock_);
            more_delta = delta_queue_.flip_delta();
        }
        profiler_.delta_cycles.fetch_add(1, std::memory_order_relaxed);

        if (!more_delta) {
            uint64_t next_t = time_wheel_.advance_time();
            if (next_t == UINT64_MAX) {
                active_.store(false, std::memory_order_release);
            } else {
                auto events = time_wheel_.drain_current_bucket();
                std::lock_guard<std::mutex> elk(event_lock_);
                for (auto& e : events) delta_queue_.push(e);
            }
        }
        barrier_cv_.notify_all();   // wake all waiting threads
    } else {
        // Phase-stable predicate (generation counter corrects the race)
        barrier_cv_.wait(lk, [this, gen]{
            return generation_ != gen || !active_.load();
        });
    }
}
```

**Three failure modes in the original `arrived_ == 0` predicate:**
1. Fast thread loops and increments `arrived_` before slow thread checks — slow thread sees `arrived_ != 0`, goes back to sleep — **deadlock**
2. If any thread throws or exits without calling `arrived_++`, barrier never reaches `num_threads_` — **permanent deadlock**
3. Completion function runs inside the lock — all threads blocked while delta flip happens — **unnecessary serialization**

### The C++20 replacement (1 line)

```cpp
// DeltaBarrier.hpp — the after state
std::barrier<std::function<void()>> barrier_(
    num_threads,
    [this]() noexcept { on_delta_complete(); }  // completion fn
);

// Worker:
void arrive_and_wait() {
    barrier_.arrive_and_wait();   // one line replaces 30
}
```

**What `std::barrier` guarantees by construction:**
- Completion function runs **exactly once** per phase, on the last arriving thread
- All N threads are released **simultaneously** after completion
- No spurious wakeups possible
- Thread count invariant maintained automatically
- `noexcept` completion function — if it throws, `std::terminate` (explicit failure, not silent deadlock)

**Other C++20 primitives used:**

| Primitive | Where | Replaces |
|-----------|-------|---------|
| `std::latch` | `SimulationEngine` startup gate | Manual `volatile bool` startup race |
| `std::counting_semaphore<2>` | Boundary signal bus limiter | Custom semaphore with mutex+CV |
| `atomic::wait()` | `AtomicStrategy` process wakeup | `condition_variable::wait()` with mutex |
| `atomic::notify_all()` | Signal change broadcast | `condition_variable::notify_all()` |

---

## 13. Strategy Design Pattern

```
ISyncStrategy (abstract interface)
    │
    ├── CoarseGrainedStrategy   (one global std::mutex)
    ├── FineGrainedStrategy     (per-signal std::shared_mutex)
    └── AtomicStrategy          (per-signal std::atomic, lock-free)

SyncStrategyFactory::create(SyncMode, num_signals, num_regions)
    → returns std::unique_ptr<ISyncStrategy>
```

**Interface:**
```cpp
class ISyncStrategy {
public:
    virtual LogicValue read_signal(const SimContext&, int signal_id)      = 0;
    virtual void write_signal(SimContext&, int signal_id, LogicValue val) = 0;
    virtual void activate_process(SimContext&, int process_id)            = 0;
    virtual void sync_boundary_signal(SimContext&, int signal_id)         = 0;
    virtual uint64_t contention_count() const                             = 0;
    virtual const char* name() const                                      = 0;
};
```

**Runtime swap — demonstrated in benchmark:**
```cpp
SimulationEngine engine(3, SyncMode::FINE_GRAINED);
engine.run_until(5000);           // run with fine-grained mutex

engine.set_strategy(SyncMode::ATOMIC);   // swap at runtime
engine.run_until(5000);           // same circuit, lock-free

// set_strategy() is a unique_ptr swap — zero changes to WorkerThread
// WorkerThread holds ISyncStrategy& → vtable dispatch → correct impl
```

**Factory + environment override:**
```cpp
// Select by enum:
auto s = SyncStrategyFactory::create(SyncMode::ATOMIC, num_signals, num_regions);

// Select by string (command-line arg):
auto mode = SyncStrategyFactory::mode_from_string("fine_grained");

// Select by environment variable:
// CELERIS_SYNC_MODE=ATOMIC ./sim
auto mode = SyncStrategyFactory::mode_from_env(SyncMode::FINE_GRAINED);
```

**Adding a new strategy** requires zero changes to any existing code:
1. Implement `ISyncStrategy` in a new file
2. Add one `case` to `SyncStrategyFactory::create()`
3. Done — `WorkerThread`, `DeltaBarrier`, `SimulationEngine` are untouched

---

## 14. C++20 Features Used

| Feature | File | Replaces | Why Better |
|---------|------|---------|-----------|
| `std::barrier<CompletionFn>` | `DeltaBarrier.hpp` | mutex + CV + counter (30 lines) | Correct by construction, no spurious wakeups, completion fn runs exactly once per phase |
| `std::latch` | `SimulationEngine.hpp` | `volatile bool started_` | One-shot, count-down gate; `arrive_and_wait()` is atomic decrement + block |
| `std::counting_semaphore<2>` | `SimulationEngine.hpp` | Custom semaphore with mutex | Limits boundary bus to 2 concurrent threads; more expressive than a raw mutex |
| `std::atomic::wait(old)` | `AtomicStrategy.hpp` | `condition_variable::wait()` | No mutex required; OS-efficient (uses futex on Linux, ulock on macOS) |
| `std::atomic::notify_all()` | `AtomicStrategy.hpp`, `FineGrainedStrategy.hpp` | `condition_variable::notify_all()` | Paired with `atomic::wait()`; no mutex needed |
| `std::jthread` (design intent) | `WorkerThread.hpp` | `std::thread` + manual join | Auto-join on destruction, `stop_token` for cooperative stop. Workaround on Apple clang: `std::thread + std::atomic<bool>` |
| Spaceship operator `<=>` | `Event.hpp` (SimTime) | Manual `<`, `>`, `==` operators | Single definition generates all six comparisons |
| CTAD for `std::lock_guard` | Throughout | `std::lock_guard<std::mutex>` | Deduced template argument, cleaner syntax |
| `std::barrier` completion `noexcept` | `DeltaBarrier.hpp` | — | Required by standard; `std::terminate` on throw rather than silent deadlock |

---

## 15. Legacy Engine — The Educational Bad Code

`include/legacy/` is the "before" state. Every bad practice is labeled `// LEGACY:` in the source with an explanation of why it is wrong and what the replacement is.

| File | What's wrong | Correct replacement |
|------|-------------|-------------------|
| `LegacySimEngine.hpp:event_lock_` | One mutex for ALL event queue ops | Per-bucket SpinLock in TimeWheel; separate insert/drain SpinLock in DeltaQueue |
| `LegacySimEngine.hpp:signal_lock_` | One mutex for ALL signal reads and writes | Per-signal `std::shared_mutex` (FineGrained) or `std::atomic` (Atomic) |
| `LegacySimEngine.hpp:legacy_barrier_wait()` | mutex + CV + counter, generation-counter race | `std::barrier<CompletionFn>` |
| `LegacySimEngine.hpp:stop_flag_` | `volatile bool` — data race with reading threads | `std::stop_token` via `std::jthread` |
| `LegacySimEngine.hpp` (constructor) | No startup synchronization | `std::latch startup_latch_` |
| `LegacyTimeWheel.hpp:global_lock_` | One mutex for 1024-bucket wheel | `std::array<SpinLock, 1024> bucket_locks_` |
| `LegacyDeltaQueue.hpp:global_lock_` | Insert and drain share one mutex | Separate `insert_lock_` and `drain_lock_` |

---

## 16. Build and Run

### Requirements

- C++20 compiler: GCC 11+, Clang 14+, Apple clang 17+
- pthreads

### Build

```bash
# Direct (no CMake):
mkdir -p build
c++ -std=c++20 -Wall -Wextra -O2 -Iinclude -o build/sim src/main.cpp -lpthread

# With CMake:
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Run

```bash
./build/sim               # 200 seed events (default)
./build/sim 1000          # 1000 seed events

CELERIS_SYNC_MODE=ATOMIC ./build/sim        # force atomic strategy
CELERIS_SYNC_MODE=FINE_GRAINED ./build/sim  # force fine-grained strategy
CELERIS_SYNC_MODE=COARSE_GRAINED ./build/sim
```

### Full output structure

```
Experiment 1: Lock Granularity
  Run A — COARSE_GRAINED  (1 global mutex)         → profiler report
  Run B — FINE_GRAINED    (1 mutex/signal)          → profiler report
  Table: granularity effect

Experiment 2: Sync Primitive
  Run B — FINE_GRAINED    (per-signal shared_mutex) → skipped (reuse A)
  Run C — ATOMIC          (per-signal atomic)       → profiler report
  Table: primitive effect

Experiment 3: Flag Primitive Microbenchmark
  Run D — mutex + bool                → activations/sec
  Run E — atomic<bool> + CAS          → activations/sec
  Run F — atomic_flag + test_and_set  → activations/sec
  Table: flag primitive effect

Combined Summary (all three strategies)
Strategy Swap Demo (runtime ISyncStrategy swap)
Legacy Engine (educational only, not a clean comparison)
```

---

## 17. Topics Covered

| Topic | Where in Code | Experiment |
|-------|--------------|-----------|
| `std::mutex`, lock contention | `CoarseGrainedStrategy`, `LegacySimEngine` | Exp 1 baseline |
| `std::shared_mutex` (reader-writer lock) | `FineGrainedStrategy::signal_locks_` | Exp 1 |
| Lock granularity and its effect on parallelism | Entire Exp 1 design | Exp 1 |
| `std::atomic` memory ordering (acquire/release) | `AtomicStrategy::write_signal` | Exp 2 |
| `compare_exchange_strong` (CAS) | `AtomicStrategy::activate_process` | Exp 3 |
| `std::atomic_flag::test_and_set` | `bench_atomic_flag_tas()` in main.cpp | Exp 3 |
| Mutex vs atomic overhead (even uncontested) | Exp 2 and Exp 3 both show ~1.7× | Exp 2, Exp 3 |
| `std::barrier<CompletionFn>` (C++20) | `DeltaBarrier.hpp` | All |
| `std::latch` (C++20 one-shot gate) | `SimulationEngine.hpp` | All |
| `std::counting_semaphore` (C++20) | `SimulationEngine.hpp` | All |
| `atomic::wait()` / `notify_all()` (C++20) | `AtomicStrategy.hpp` | Exp 2 |
| Hand-rolled barrier: generation-counter bug | `LegacySimEngine::legacy_barrier_wait()` | Educational |
| SpinLock with exponential backoff + `_mm_pause()` | `SpinLock.hpp` | Underlying TimeWheel/DeltaQueue |
| Ping-pong double buffer (producer-consumer) | `DeltaQueue.hpp` | All |
| Time wheel vs priority queue tradeoff | `TimeWheel.hpp` comments | All |
| Strategy design pattern | `ISyncStrategy` + `SyncStrategyFactory` | All |
| RAII lock guards | `SpinLockGuard`, `ReadLock`, `WriteLock` | All |
| Lock hierarchy (deadlock prevention) | `FineGrainedStrategy.hpp` comments | Exp 1 |
| False sharing (cache-line awareness) | `PaddedCounter` in `bench_shared_sharded()`, `FineGrainedStrategy.hpp` | Exp 1, Exp 4 |
| Cache-line ping-pong under sustained writes | `bench_shared_atomic()` — atomic still serializes at hardware level | Exp 4 |
| Per-thread sharding to eliminate shared state | `bench_shared_sharded()` — merge at barrier, not on hot path | Exp 4 |
| `std::jthread` / `std::stop_token` | `WorkerThread.hpp` (design + workaround) | All |
| IEEE 1800-2017 simulation scheduling regions | `EventScheduler.hpp` | All |
| Atomic profiling counters | `Profiler.hpp` | All |
| Controlled benchmarking (one variable at a time) | All three experiments | All |

---

*Built with C++20.*
