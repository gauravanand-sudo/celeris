#pragma once
/**
 * LegacySimEngine.hpp — Coarse-grained multicore simulation engine.
 *
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║  LEGACY / BAD CODE — the "before" state that motivated the          ║
 * ║  multicore synchronization refactoring.                             ║
 * ║                                                                     ║
 * ║  Every bad practice is labeled // LEGACY: with an explanation.     ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 *
 * What is wrong here:
 *  1. TWO global mutexes: event_lock_ (for queues) and signal_lock_
 *     (for signal values).  Any thread doing ANY event or signal op
 *     serializes with ALL other threads.
 *  2. Hand-rolled barrier: mutex + condition_variable + int counter.
 *     Fragile: if one thread throws, the barrier deadlocks.
 *     Does NOT guarantee all threads are released simultaneously.
 *     C++20 std::barrier replaces this correctly.
 *  3. std::thread with a manual stop_flag_ bool — no cooperative
 *     cancellation semantics, data race if not carefully ordered.
 *     C++20 std::jthread + std::stop_token replaces this.
 *  4. No startup gate: threads start processing events before all
 *     setup is complete → undefined behavior on startup.
 *     C++20 std::latch replaces this.
 *
 * namespace celeris::legacy
 */

#include "LegacyTimeWheel.hpp"
#include "LegacyDeltaQueue.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <iostream>
#include <chrono>

namespace celeris {
namespace legacy {

// ── Simple signal model for the legacy engine ───────────────────────────────
struct LegacySignal {
    int               id;
    LegacyLogicValue  value{LegacyLogicValue::X};
    std::vector<int>  sensitive_processes;
};

struct LegacyProcess {
    int  id;
    bool ready{false};
    std::function<void()> evaluate;
    uint64_t activations{0};
};

// ── Profiling counters ───────────────────────────────────────────────────────
struct LegacyProfiler {
    std::atomic<uint64_t> total_events{0};
    std::atomic<uint64_t> delta_cycles{0};
    std::atomic<uint64_t> signal_updates{0};
    std::atomic<uint64_t> process_evals{0};

    void reset() {
        total_events.store(0); delta_cycles.store(0);
        signal_updates.store(0); process_evals.store(0);
    }
};

// ── Legacy simulation engine ─────────────────────────────────────────────────
class LegacySimEngine {
public:
    explicit LegacySimEngine(int num_threads)
        : num_threads_(num_threads)
    {}

    ~LegacySimEngine() { stop(); }

    void add_signal(LegacySignal s) { signals_.push_back(s); }
    void add_process(LegacyProcess p) { processes_.push_back(p); }

    void add_event(LegacyEvent e)
    {
        // LEGACY: event_lock_ must be taken even for the very first event.
        std::lock_guard<std::mutex> lk(event_lock_);
        if (e.sim_time == 0) {
            delta_queue_.push(e);
        } else {
            time_wheel_.schedule(e);
        }
    }

    // -----------------------------------------------------------------------
    // run_all — launches threads and runs until queues drain.
    // -----------------------------------------------------------------------
    void run_all()
    {
        active_.store(true, std::memory_order_release);
        stop_flag_ = false; // LEGACY: plain bool, not stop_token
        arrived_   = 0;     // LEGACY: manual counter for barrier

        // Seed delta queue from time wheel at t=0
        {
            // LEGACY: must take event_lock_ to touch the queue
            std::lock_guard<std::mutex> lk(event_lock_);
            auto events = time_wheel_.drain_current_bucket();
            for (auto& e : events) delta_queue_.push(e);
        }

        // LEGACY: std::thread — no stop_token, no auto-join.
        threads_.clear();
        for (int i = 0; i < num_threads_; ++i) {
            threads_.emplace_back([this, i]{ worker_loop(i); });
        }

        // Wait for all threads.  LEGACY: manual join loop.
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
        threads_.clear();
    }

    void stop()
    {
        stop_flag_ = true; // LEGACY: non-atomic write — potential data race
        active_.store(false, std::memory_order_release);
        // LEGACY: must notify barrier CV to unblock waiting threads.
        barrier_cv_.notify_all();
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
        threads_.clear();
    }

    LegacyProfiler& profiler() { return profiler_; }

private:
    // -----------------------------------------------------------------------
    // worker_loop — main loop for each thread.
    // LEGACY: no startup latch; threads begin immediately and may race with
    //         the engine still calling add_event().
    // -----------------------------------------------------------------------
    void worker_loop(int /*tid*/)
    {
        // LEGACY: no std::latch. Threads just start — potential startup race.
        while (active_.load(std::memory_order_acquire) && !stop_flag_) {
            process_delta_events();
            legacy_barrier_wait(); // LEGACY: hand-rolled barrier
            if (!active_.load(std::memory_order_relaxed)) break;
        }
    }

    // -----------------------------------------------------------------------
    // process_delta_events — drain and apply delta events.
    // LEGACY: signal_lock_ and event_lock_ are both taken sequentially,
    //         causing maximum serialization.
    // -----------------------------------------------------------------------
    void process_delta_events()
    {
        std::vector<LegacyEvent> events;
        {
            // LEGACY: event_lock_ taken to drain delta queue.
            // While held, no other thread can push new events.
            std::lock_guard<std::mutex> lk(event_lock_);
            events = delta_queue_.drain();
        }

        for (auto& e : events) {
            if (e.type == LegacyEventType::SIGNAL_UPDATE && e.signal_id >= 0) {
                {
                    // LEGACY: signal_lock_ taken to update ONE signal value.
                    // All other threads blocked from reading ANY signal.
                    std::lock_guard<std::mutex> lk(signal_lock_);
                    signals_[e.signal_id].value = e.new_value;
                }

                // Activate sensitized processes.
                for (int pid : signals_[e.signal_id].sensitive_processes) {
                    std::lock_guard<std::mutex> lk(signal_lock_); // LEGACY: take again
                    processes_[pid].ready = true;
                }

                profiler_.signal_updates.fetch_add(1, std::memory_order_relaxed);
            }
            profiler_.total_events.fetch_add(1, std::memory_order_relaxed);
        }

        // Evaluate ready processes.
        for (auto& proc : processes_) {
            bool should_run = false;
            {
                std::lock_guard<std::mutex> lk(signal_lock_); // LEGACY: lock to read .ready
                if (proc.ready) { proc.ready = false; should_run = true; }
            }
            if (should_run && proc.evaluate) {
                proc.evaluate();
                ++proc.activations;
                profiler_.process_evals.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // -----------------------------------------------------------------------
    // legacy_barrier_wait — hand-rolled barrier using mutex + condvar + counter.
    //
    // LEGACY: This is exactly what C++20 std::barrier replaces.
    // Problems:
    //   - Spurious wakeups require the lambda predicate — easy to get wrong.
    //   - If one thread is killed, arrived_ is never incremented → deadlock.
    //   - Completion logic (flip_delta) runs inside the lock → others wait.
    //   - Extra mutex acquisition even for threads that are just waiting.
    //
    // REFACTOR → DeltaBarrier.hpp using std::barrier<CompletionFn>
    // -----------------------------------------------------------------------
    void legacy_barrier_wait()
    {
        std::unique_lock<std::mutex> lk(barrier_mutex_); // LEGACY: barrier mutex
        int gen = generation_;  // capture current generation BEFORE incrementing arrived_
        ++arrived_;

        if (arrived_ == num_threads_) {
            // Last thread to arrive: do delta flip.
            arrived_ = 0;
            ++generation_;  // advance generation so waiting threads can distinguish phases

            // LEGACY: flip and advance inside the lock — blocks all other threads.
            bool more_delta;
            {
                std::lock_guard<std::mutex> elk(event_lock_);
                more_delta = delta_queue_.flip_delta();
            }
            profiler_.delta_cycles.fetch_add(1, std::memory_order_relaxed);

            if (!more_delta) {
                // Try to advance time.
                uint64_t next_t = time_wheel_.advance_time();
                if (next_t == UINT64_MAX) {
                    active_.store(false, std::memory_order_release);
                } else {
                    auto events = time_wheel_.drain_current_bucket();
                    std::lock_guard<std::mutex> elk(event_lock_);
                    for (auto& e : events) delta_queue_.push(e);
                }
            }
            // LEGACY: notify_all to unblock ALL waiting threads.
            // (notify_one is wrong here — other threads may remain blocked.)
            barrier_cv_.notify_all();
        } else {
            // LEGACY: wait with generation-based predicate.
            // BUG FIXED: arrived_ == 0 predicate races when a fast thread re-enters
            // the next phase and bumps arrived_ before slow threads check it.
            // The generation counter (gen != generation_) is phase-stable.
            // C++20 std::barrier eliminates this entirely.
            barrier_cv_.wait(lk, [this, gen]{ return generation_ != gen || !active_.load(); });
        }
    }

    int num_threads_;

    // LEGACY: two coarse global mutexes.
    mutable std::mutex event_lock_;   // protects ALL event queue operations
    mutable std::mutex signal_lock_;  // protects ALL signal value reads and writes

    // LEGACY: hand-rolled barrier state.
    // BUG: arrived_ == 0 predicate has a race — if a fast thread re-enters the
    // next phase and increments arrived_ before a slow thread checks the predicate,
    // the slow thread sees arrived_ != 0 and goes back to sleep indefinitely.
    // Fix: use a generation counter (generation_ != captured_gen) as the predicate.
    // C++20 std::barrier avoids this entire class of bugs by construction.
    std::mutex              barrier_mutex_;
    std::condition_variable barrier_cv_;
    int                     arrived_{0};
    int                     generation_{0};  // LEGACY BUG FIX: generation counter

    // LEGACY: plain bool stop flag — potential data race with threads.
    // (volatile bool would be slightly better; std::stop_token is correct.)
    volatile bool stop_flag_{false};

    std::atomic<bool>       active_{false};

    // LEGACY: std::thread — no auto-join, no cooperative stop.
    std::vector<std::thread> threads_;

    LegacyTimeWheel   time_wheel_;
    LegacyDeltaQueue  delta_queue_;

    std::vector<LegacySignal>  signals_;
    std::vector<LegacyProcess> processes_;

    LegacyProfiler profiler_;
};

} // namespace legacy
} // namespace celeris
