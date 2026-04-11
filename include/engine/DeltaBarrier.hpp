#pragma once
/**
 * DeltaBarrier.hpp — C++20 std::barrier wrapping delta-cycle synchronization.
 *
 * In a multicore simulation, every worker thread processes some subset of the
 * events in the current delta cycle.  Before any thread starts the NEXT delta
 * cycle, ALL threads must have finished the current one — otherwise a thread
 * starting delta N+1 might read a signal that another thread has not yet
 * written in delta N.
 *
 * This is exactly the synchronization problem std::barrier solves:
 *   "All N threads arrive at the barrier; the last one to arrive runs the
 *    completion function; then all N are released simultaneously."
 *
 * REFACTOR vs legacy (LegacySimEngine):
 *   Legacy uses:  std::mutex + std::condition_variable + int counter
 *     → 3 separate objects, manual wait loop, spurious wakeup handling,
 *       easy to deadlock if one thread fails to decrement the counter.
 *
 *   C++20 uses: std::barrier<CompletionFn>
 *     → Single object, correct by construction, no manual counter,
 *       no spurious wakeups, completion function guaranteed to run exactly
 *       once per phase, on the last arriving thread.
 *
 * CompletionFn (runs once per delta, on the last thread to arrive):
 *   1. Flips the DeltaQueue ping-pong buffers (advance_delta).
 *   2. If no more delta events AND no more timed events: sets simulation_done.
 *   3. Increments Profiler::delta_cycles.
 *
 * namespace celeris
 */

#include "../core/SimContext.hpp"
#include "../scheduler/EventScheduler.hpp"
#include "../engine/Profiler.hpp"
#include <barrier>
#include <atomic>
#include <functional>

namespace celeris {

class DeltaBarrier {
public:
    DeltaBarrier(int num_threads, SimContext& ctx,
                 EventScheduler& sched, Profiler& prof)
        : ctx_(ctx)
        , sched_(sched)
        , prof_(prof)
        , done_(false)
        // C++20 std::barrier: num_threads participants, completion lambda runs
        // on the last thread to call arrive_and_wait().
        , barrier_(num_threads, [this]() noexcept { on_delta_complete(); })
    {}

    // -----------------------------------------------------------------------
    // arrive_and_wait — called by each worker at the end of a delta cycle.
    //
    // All N workers call this.  The last one to arrive runs on_delta_complete().
    // All N are then released to start the next delta.
    //
    // REFACTOR: In legacy code this was:
    //   {
    //     std::unique_lock lk(barrier_mutex_);
    //     ++arrived_;
    //     if (arrived_ == num_threads_) {
    //         arrived_ = 0;
    //         flip_delta();
    //         cv_.notify_all();  // wake everyone
    //     } else {
    //         cv_.wait(lk, [&]{ return arrived_ == 0; }); // wait with spurious wake check
    //     }
    //   }
    // With C++20:  one line.
    // -----------------------------------------------------------------------
    void arrive_and_wait()
    {
        prof_.barrier_waits.fetch_add(1, std::memory_order_relaxed);
        barrier_.arrive_and_wait();  // C++20: blocks until all threads arrive
    }

    [[nodiscard]] bool simulation_done() const noexcept
    {
        return done_.load(std::memory_order_acquire);
    }

    void reset()
    {
        done_.store(false, std::memory_order_relaxed);
    }

private:
    // -----------------------------------------------------------------------
    // on_delta_complete — runs ONCE per delta cycle, on the last arriving thread.
    // All other threads are blocked in barrier_.arrive_and_wait().
    // This function must be noexcept (std::barrier requirement).
    // -----------------------------------------------------------------------
    void on_delta_complete() noexcept
    {
        prof_.delta_cycles.fetch_add(1, std::memory_order_relaxed);

        // Check end_time guard (set by run_until before workers start).
        uint64_t t   = ctx_.current_time.load(std::memory_order_relaxed);
        uint64_t end = ctx_.end_time.load(std::memory_order_relaxed);
        if (t >= end) {
            ctx_.simulation_active.store(false, std::memory_order_release);
            done_.store(true, std::memory_order_release);
            return;
        }

        // Flip ping-pong buffers.  Returns true if more delta events were queued.
        bool more_delta = sched_.advance_delta();

        if (!more_delta) {
            // No more events at this time step.  Try to advance simulation time.
            bool more_time = sched_.advance_time();
            if (more_time) {
                prof_.time_steps.fetch_add(1, std::memory_order_relaxed);
            } else {
                // No more events at all — simulation is complete.
                ctx_.simulation_active.store(false, std::memory_order_release);
                done_.store(true, std::memory_order_release);
            }
        }
    }

    SimContext&     ctx_;
    EventScheduler& sched_;
    Profiler&       prof_;
    std::atomic<bool> done_{false};

    // C++20 std::barrier with a completion function stored by value.
    std::barrier<std::function<void()>> barrier_;
};

} // namespace celeris
