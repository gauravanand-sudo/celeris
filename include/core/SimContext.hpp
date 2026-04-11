#pragma once
/**
 * SimContext.hpp — Global simulation state container for the celeris engine.
 *
 * SimContext is the single source of truth shared by all worker threads.
 * All mutable shared state is protected either:
 *   - by the active ISyncStrategy (signal values, event queues), or
 *   - by being std::atomic (current_time, current_delta, simulation_active, counters)
 *
 * Design note: we intentionally do NOT use a single struct lock here.
 * The whole point of the engine is to show that fine-grained or atomic
 * synchronization drastically outperforms a single global lock.
 *
 * next_event_id() issues unique monotonically increasing IDs for events using
 * a relaxed fetch_add — ordering is not required for uniqueness.
 *
 * namespace celeris
 */

#include "Signal.hpp"
#include "Process.hpp"
#include "Region.hpp"
#include <atomic>
#include <climits>
#include <cstdint>
#include <vector>

namespace celeris {

struct SimContext {
    // -----------------------------------------------------------------------
    // Time state — written only by the barrier completion function,
    // read by all workers.  Acquire/release semantics pair correctly with the
    // std::barrier completion callback.
    // -----------------------------------------------------------------------
    std::atomic<uint64_t> current_time{0};
    std::atomic<uint32_t> current_delta{0};

    // -----------------------------------------------------------------------
    // Lifecycle flag — set true by SimulationEngine::run(), cleared by stop()
    // or by the barrier completion when the event queues drain.
    // -----------------------------------------------------------------------
    std::atomic<bool>     simulation_active{false};

    // -----------------------------------------------------------------------
    // Profiling counters — updated by workers and the barrier.
    // -----------------------------------------------------------------------
    std::atomic<uint64_t> total_events_processed{0};
    std::atomic<uint64_t> event_id_counter{0};

    // -----------------------------------------------------------------------
    // Static configuration — written once before workers start; thereafter
    // immutable (no synchronization needed).
    // -----------------------------------------------------------------------
    int num_threads{1};
    int num_regions{1};
    std::atomic<uint64_t> end_time{UINT64_MAX}; // simulation stops at this time

    // -----------------------------------------------------------------------
    // Design data — populated before simulation starts.
    // Immutable structural data (ids, names, sensitivity lists) needs no lock.
    // Mutable value data (current_value, atomic_value) is protected by strategy.
    // -----------------------------------------------------------------------
    std::vector<Signal>  signals;
    std::vector<Process> processes;
    std::vector<Region>  regions;

    // -----------------------------------------------------------------------
    // Atomic event-ID generator.
    // Relaxed ordering is sufficient: we only need uniqueness, not ordering
    // relative to other operations on these IDs.
    // -----------------------------------------------------------------------
    [[nodiscard]] uint64_t next_event_id() noexcept {
        return event_id_counter.fetch_add(1, std::memory_order_relaxed);
    }

    // Non-copyable.
    SimContext() = default;
    SimContext(const SimContext&) = delete;
    SimContext& operator=(const SimContext&) = delete;
};

} // namespace celeris
