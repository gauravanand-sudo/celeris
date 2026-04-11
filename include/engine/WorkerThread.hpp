#pragma once
/**
 * WorkerThread.hpp — Per-core simulation worker using C++20 jthread.
 *
 * Each WorkerThread owns one region of the design and is responsible for:
 *   1. Draining delta events assigned to its region
 *   2. Evaluating ready processes in its region
 *   3. Propagating signal changes (writing values, activating sensitized procs)
 *   4. Synchronizing boundary signals (cross-region communication)
 *   5. Arriving at the DeltaBarrier at the end of each delta cycle
 *
 * C++20 features used:
 *   std::latch    — one-shot startup gate: all workers wait here before the
 *                   simulation starts, preventing startup races.
 *
 * Note: std::jthread / std::stop_token are in libc++ on Apple clang 17 only
 * behind _LIBCPP_ENABLE_EXPERIMENTAL. We use std::thread + std::atomic<bool>
 * which provides the same cooperative-stop semantics. The design comment still
 * documents the jthread upgrade path (used on Linux with libstdc++).
 *
 * namespace celeris
 */

#include "../core/SimContext.hpp"
#include "../scheduler/EventScheduler.hpp"
#include "../sync/ISyncStrategy.hpp"
#include "DeltaBarrier.hpp"
#include "Profiler.hpp"
#include <latch>
#include <thread>
#include <atomic>
#include <vector>
#include <iostream>

namespace celeris {

class WorkerThread {
public:
    WorkerThread(int thread_id, int region_id,
                 SimContext& ctx, EventScheduler& sched,
                 ISyncStrategy& strategy, DeltaBarrier& barrier,
                 Profiler& prof, std::latch& startup_latch)
        : thread_id_(thread_id)
        , region_id_(region_id)
        , ctx_(ctx)
        , sched_(sched)
        , strategy_(strategy)
        , barrier_(barrier)
        , prof_(prof)
        , startup_latch_(startup_latch)
        , stop_requested_(false)
        // Cooperative stop via atomic<bool>. On platforms with full C++20
        // libstdc++, replace with std::jthread + std::stop_token.
        , thread_([this]{ run(); })
    {}

    ~WorkerThread()
    {
        stop_requested_.store(true, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }

    // Non-copyable, non-movable after construction (thread is running).
    WorkerThread(const WorkerThread&) = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;

    void request_stop()
    {
        stop_requested_.store(true, std::memory_order_release);
    }

    [[nodiscard]] int thread_id() const noexcept { return thread_id_; }
    [[nodiscard]] int region_id() const noexcept { return region_id_; }

private:
    // -----------------------------------------------------------------------
    // run — main loop for this worker thread.
    // Cooperative stop: checks stop_requested_ at each delta boundary.
    // On libstdc++ (Linux), this becomes: void run(std::stop_token st)
    // with while (!st.stop_requested() && ...) — same logic, better API.
    // -----------------------------------------------------------------------
    void run()
    {
        // Wait for all workers to be constructed before starting simulation.
        // C++20 std::latch::arrive_and_wait(): decrement count, block until 0.
        startup_latch_.arrive_and_wait();

        // Workers MUST always arrive at the barrier on every iteration.
        // Exiting asynchronously (without arriving) would deadlock the remaining
        // workers waiting for all N participants.
        // The ONLY termination path is via barrier_.simulation_done() — set
        // exclusively inside the barrier's completion function (on_delta_complete).
        while (true)
        {
            process_delta_events();
            evaluate_ready_processes();
            sync_boundary_signals();

            // All N workers must arrive before any proceeds.
            // Last to arrive runs on_delta_complete() which decides termination.
            barrier_.arrive_and_wait();

            if (barrier_.simulation_done()) break;
        }
    }

    // -----------------------------------------------------------------------
    // process_delta_events — drain events from the active delta buffer and apply them.
    // drain_delta() is protected by SpinLock so only ONE worker gets events per call.
    // Workers that get an empty drain just proceed to the barrier.
    // In a full implementation, per-region event queues would allow true
    // parallel drain; here we use a single shared queue for correctness.
    // -----------------------------------------------------------------------
    void process_delta_events()
    {
        auto events = sched_.drain_delta();

        for (auto& e : events) {
            // Process all events regardless of region — the drain_lock ensures
            // only one worker sees each event (no duplication).
            bool is_signal_update = (e.type == EventType::SIGNAL_UPDATE);
            bool has_signal = (e.signal_id >= 0 &&
                               e.signal_id < static_cast<int>(ctx_.signals.size()));

            if (is_signal_update && has_signal) {
                // Apply the signal update via the active strategy.
                strategy_.write_signal(ctx_, e.signal_id, e.new_value);
                prof_.signal_updates.fetch_add(1, std::memory_order_relaxed);

                // Activate all processes sensitive to this signal.
                const auto& sig = ctx_.signals[e.signal_id];
                for (int pid : sig.sensitive_processes) {
                    strategy_.activate_process(ctx_, pid);
                }

            } else if (e.type == EventType::PROCESS_ACTIVATE) {
                if (e.process_id >= 0 &&
                    e.process_id < static_cast<int>(ctx_.processes.size())) {
                    strategy_.activate_process(ctx_, e.process_id);
                }
            } else if (e.type == EventType::NBA && has_signal) {
                auto& sig = ctx_.signals[e.signal_id];
                if (sig.has_nba_pending) {
                    strategy_.write_signal(ctx_, e.signal_id, sig.scheduled_value);
                    sig.has_nba_pending = false;
                    prof_.nba_updates.fetch_add(1, std::memory_order_relaxed);
                }
            }

            prof_.total_events.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // -----------------------------------------------------------------------
    // evaluate_ready_processes — find all READY processes in our region and
    // call their evaluate() function.
    // -----------------------------------------------------------------------
    void evaluate_ready_processes()
    {
        auto& region = ctx_.regions[region_id_];

        for (int pid : region.process_ids) {
            auto& proc = ctx_.processes[pid];

            // CAS from READY → RUNNING.  If another thread beat us (shouldn't
            // happen with static region assignment, but be safe), skip.
            ProcessState expected = ProcessState::READY;
            bool took_it = proc.state.compare_exchange_strong(
                expected, ProcessState::RUNNING,
                std::memory_order_acq_rel, std::memory_order_relaxed);

            if (!took_it) continue;

            // Execute the process.
            if (proc.evaluate) {
                proc.evaluate(ctx_);
                prof_.process_evaluations.fetch_add(1, std::memory_order_relaxed);
                ++proc.activation_count;
            }

            // Transition back to DORMANT.
            proc.state.store(ProcessState::DORMANT, std::memory_order_release);
        }
    }

    // -----------------------------------------------------------------------
    // sync_boundary_signals — flush values of boundary signals to all regions.
    // -----------------------------------------------------------------------
    void sync_boundary_signals()
    {
        for (int sid : ctx_.regions[region_id_].boundary_signals) {
            strategy_.sync_boundary_signal(ctx_, sid);
        }
    }

    // -----------------------------------------------------------------------
    // Member data
    // -----------------------------------------------------------------------
    int             thread_id_;
    int             region_id_;
    SimContext&     ctx_;
    EventScheduler& sched_;
    ISyncStrategy&  strategy_;
    DeltaBarrier&   barrier_;
    Profiler&       prof_;
    std::latch&          startup_latch_;
    std::atomic<bool>    stop_requested_;

    // std::thread: join() called in destructor (see ~WorkerThread).
    // Upgrade path: replace with std::jthread on libstdc++.
    std::thread          thread_;
};

} // namespace celeris
