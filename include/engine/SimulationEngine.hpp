#pragma once
/**
 * SimulationEngine.hpp — Top-level multicore simulation engine.
 *
 * This is the orchestrator that assembles all components:
 *   SimContext       — shared simulation state
 *   EventScheduler   — TimeWheel + DeltaQueue
 *   ISyncStrategy    — pluggable synchronization (Coarse/Fine/Atomic)
 *   DeltaBarrier     — C++20 std::barrier for inter-delta sync
 *   WorkerThread[]   — per-core workers (C++20 jthread)
 *   std::latch       — startup gate (C++20)
 *   counting_semaphore — boundary signal bus access limiter (C++20)
 *   Profiler         — atomic performance counters
 *
 * Usage:
 *   SimulationEngine engine(4, SyncMode::ATOMIC);
 *   engine.add_signal(sig);
 *   engine.add_process(proc);
 *   engine.add_event(initial_event);
 *   engine.run_until(1000);
 *   engine.profiler().report(std::cout, elapsed, "ATOMIC");
 *
 * namespace celeris
 */

#include "../core/SimContext.hpp"
#include "../scheduler/EventScheduler.hpp"
#include "../sync/ISyncStrategy.hpp"
#include "../sync/SyncStrategyFactory.hpp"
#include "DeltaBarrier.hpp"
#include "WorkerThread.hpp"
#include "Profiler.hpp"
#include <latch>
#include <semaphore>
#include <memory>
#include <vector>
#include <stdexcept>

namespace celeris {

class SimulationEngine {
public:
    explicit SimulationEngine(int num_threads, SyncMode mode = SyncMode::FINE_GRAINED)
        : num_threads_(num_threads)
        , scheduler_(ctx_)
        , startup_latch_(num_threads)     // C++20 latch: all workers wait here
        // C++20 counting_semaphore: max 2 threads can access boundary signal bus concurrently.
        // Models the cross-region interconnect bandwidth constraint.
        , boundary_semaphore_(2)
        , barrier_(num_threads, ctx_, scheduler_, profiler_)
    {
        ctx_.num_threads = num_threads;
        // Strategy is created with actual signal/region counts in run_until()
        // after add_signal/add_region calls. Store mode for deferred creation.
        mode_ = mode;
        // Create a placeholder (COARSE needs no counts; will be replaced in run_until)
        strategy_ = std::make_unique<CoarseGrainedStrategy>();
    }

    // -----------------------------------------------------------------------
    // Design population — called before run().
    // After these calls, structural data is immutable.
    // -----------------------------------------------------------------------
    void add_signal(Signal s)
    {
        ctx_.signals.push_back(std::move(s));
    }

    void add_process(Process p)
    {
        ctx_.processes.push_back(std::move(p));
    }

    void add_region(Region&& r)
    {
        ctx_.regions.push_back(std::move(r));
        ctx_.num_regions = static_cast<int>(ctx_.regions.size());
    }

    void add_event(Event e)
    {
        e.event_id = ctx_.next_event_id();
        if (e.when.time == 0 && e.when.delta == 0) {
            scheduler_.schedule_now(e);
        } else {
            scheduler_.schedule_at(e, e.when.time);
        }
    }

    // -----------------------------------------------------------------------
    // set_strategy — swap the synchronization strategy at runtime.
    // Can be called between run() calls to benchmark different strategies.
    // -----------------------------------------------------------------------
    void set_strategy(std::unique_ptr<ISyncStrategy> s)
    {
        strategy_ = std::move(s);
    }

    void set_strategy(SyncMode mode)
    {
        mode_ = mode;
        strategy_ = SyncStrategyFactory::create(mode,
            static_cast<int>(ctx_.signals.size()),
            static_cast<int>(ctx_.regions.size()));
    }

    // -----------------------------------------------------------------------
    // run_until — run simulation from current time until end_time.
    // -----------------------------------------------------------------------
    void run_until(uint64_t end_time)
    {
        // Store end_time in context so DeltaBarrier's completion function can
        // check it.  Termination is ALWAYS decided inside the completion function,
        // never asynchronously from the main thread (which would cause workers to
        // exit without arriving at the barrier → deadlock).
        // (Re)create the strategy now that all signals and regions are known.
        // FineGrainedStrategy needs the exact signal count to pre-allocate locks.
        strategy_ = SyncStrategyFactory::create(mode_,
            static_cast<int>(ctx_.signals.size()),
            static_cast<int>(ctx_.regions.size()));

        ctx_.end_time.store(end_time, std::memory_order_release);
        ctx_.simulation_active.store(true, std::memory_order_release);
        barrier_.reset();

        workers_.clear();
        workers_.reserve(num_threads_);
        for (int i = 0; i < num_threads_; ++i) {
            int region_id = (ctx_.regions.empty() ? 0 : i % static_cast<int>(ctx_.regions.size()));
            workers_.emplace_back(std::make_unique<WorkerThread>(
                i, region_id, ctx_, scheduler_, *strategy_,
                barrier_, profiler_, startup_latch_));
        }

        // Wait for all workers to finish.  Workers exit via barrier_.simulation_done().
        // The main thread just waits — no asynchronous modification of simulation_active.
        workers_.clear();  // WorkerThread destructors join threads
    }

    // -----------------------------------------------------------------------
    // run_all — run until all events are consumed.
    // -----------------------------------------------------------------------
    void run_all()
    {
        run_until(UINT64_MAX);
    }

    void stop()
    {
        ctx_.simulation_active.store(false, std::memory_order_release);
        workers_.clear();
    }

    void reset_profiler() { profiler_.reset(); }

    [[nodiscard]] const Profiler&    profiler()  const noexcept { return profiler_; }
    [[nodiscard]] const SimContext&  context()   const noexcept { return ctx_; }
    [[nodiscard]] ISyncStrategy&     strategy()  noexcept       { return *strategy_; }
    [[nodiscard]] EventScheduler&    scheduler() noexcept       { return scheduler_; }

private:
    int              num_threads_;
    SyncMode         mode_{SyncMode::FINE_GRAINED};
    SimContext       ctx_;
    EventScheduler   scheduler_;
    Profiler         profiler_;
    std::unique_ptr<ISyncStrategy> strategy_;

    // C++20 startup synchronization
    std::latch                startup_latch_;
    std::counting_semaphore<2> boundary_semaphore_;

    DeltaBarrier              barrier_;
    std::vector<std::unique_ptr<WorkerThread>> workers_;
};

} // namespace celeris
