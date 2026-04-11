#pragma once
/**
 * Region.hpp — Design partition / domain for the celeris multicore engine.
 *
 * A Region is a static partition of the design netlist assigned to one or more
 * worker threads.  The partitioning mirrors a multi-domain simulator model
 * where the tool statically analyzes the design and groups logic that
 * communicates frequently into the same region to reduce synchronization.
 *
 * Intra-region signals: read/written by the owning thread only → no sync
 * needed beyond the per-signal lock when FineGrainedStrategy is active.
 *
 * Boundary signals: driven in one region but read in another.  These require
 * explicit cross-region synchronization via ISyncStrategy::sync_boundary_signal
 * and are serialized through the counting_semaphore in SimulationEngine.
 *
 * region_mutex: only used by CoarseGrainedStrategy to show how coarse locking
 * degenerates to per-region serialization even within the "improved" version.
 *
 * namespace celeris
 */

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace celeris {

struct Region {
    int id{-1};

    /// Process IDs that belong to this region.
    std::vector<int> process_ids;

    /// Signal IDs that are owned (driven) by this region.
    std::vector<int> signal_ids;

    /// Signals whose values are consumed by processes in other regions.
    /// These are the synchronization boundary: writing them requires
    /// notification so readers in other regions see the update.
    std::vector<int> boundary_signals;

    /// Number of events currently queued for this region's processes.
    /// Used by workers to quickly check if there is work to do.
    std::atomic<int> pending_events{0};

    /// Per-region mutex — ONLY used by CoarseGrainedStrategy.
    /// Stored as unique_ptr so Region remains movable (std::mutex is not movable).
    std::unique_ptr<std::mutex> region_mutex{std::make_unique<std::mutex>()};

    // Non-copyable; custom move (atomic + unique_ptr members).
    Region() = default;
    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;

    Region(Region&& o) noexcept
        : id(o.id)
        , process_ids(std::move(o.process_ids))
        , signal_ids(std::move(o.signal_ids))
        , boundary_signals(std::move(o.boundary_signals))
        , pending_events(o.pending_events.load(std::memory_order_relaxed))
        , region_mutex(std::move(o.region_mutex))
    {}

    Region& operator=(Region&& o) noexcept {
        if (this != &o) {
            id = o.id;
            process_ids = std::move(o.process_ids);
            signal_ids = std::move(o.signal_ids);
            boundary_signals = std::move(o.boundary_signals);
            pending_events.store(o.pending_events.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
            region_mutex = std::move(o.region_mutex);
        }
        return *this;
    }
};

} // namespace celeris
