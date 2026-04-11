#pragma once
/**
 * FineGrainedStrategy.hpp — Per-resource locking for maximum concurrency.
 *
 * REFACTOR: From CoarseGrainedStrategy (one lock for everything) to
 *           FineGrainedStrategy (one lock per resource).
 *
 * KEY INSIGHT:
 *   Two threads touching different signals have NO data dependency.
 *   There is no reason they should serialize on the same lock.
 *   Fine-grained locking assigns one std::shared_mutex per signal so:
 *     - Thread A reading signal 3 and Thread B reading signal 7 run concurrently.
 *     - Thread A reading signal 3 and Thread B writing signal 3 still serialize
 *       (correct: write-read ordering required), but only on signal 3.
 *
 * Lock hierarchy (prevents deadlock):
 *   1. signal_locks_[id]  — always acquired via signal ID order (ascending)
 *                           to prevent ABBA deadlock when multiple signals
 *                           must be updated atomically.
 *   2. region_queue_locks_[region_id] — taken after signal lock if needed.
 *   NEVER acquire region lock while holding a signal lock from a different region.
 *
 * std::shared_mutex (reader-writer lock):
 *   - read_signal   → std::shared_lock   (many readers, no blocking each other)
 *   - write_signal  → std::unique_lock   (exclusive: one writer, no readers)
 *   This matches the simulation access pattern: 1 driver, N sensitive readers.
 *
 * namespace celeris
 */

#include "ISyncStrategy.hpp"
#include "RWLock.hpp"
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <atomic>

namespace celeris {

class FineGrainedStrategy final : public ISyncStrategy {
public:
    explicit FineGrainedStrategy(int num_signals, int num_regions)
        : signal_locks_(num_signals)
        , region_queue_locks_(num_regions)
    {}

    // -----------------------------------------------------------------------
    // Signal read — shared lock: concurrent reads on the SAME signal are allowed.
    // REFACTOR vs CoarseGrained: zero contention between reads of DIFFERENT signals.
    // -----------------------------------------------------------------------
    [[nodiscard]] LogicValue read_signal(
            const SimContext& ctx, int signal_id) override
    {
        // shared_lock: many threads may hold this simultaneously.
        std::shared_lock lk(signal_locks_[signal_id]);
        return ctx.signals[signal_id].current_value;
    }

    // -----------------------------------------------------------------------
    // Signal write — exclusive lock: only one writer, no concurrent readers.
    // REFACTOR: only blocks readers/writers of THIS signal, not all signals.
    // -----------------------------------------------------------------------
    void write_signal(
            SimContext& ctx, int signal_id, LogicValue val) override
    {
        {
            std::unique_lock lk(signal_locks_[signal_id]);
            ctx.signals[signal_id].current_value = val;
            ctx.signals[signal_id].atomic_value.store(
                static_cast<uint8_t>(val), std::memory_order_release);
        }
        // Notify any threads using AtomicStrategy::wait() on this signal's
        // atomic_value (cross-strategy compatibility).
        ctx.signals[signal_id].atomic_value.notify_all();
    }

    // -----------------------------------------------------------------------
    // Process activation — per-region queue lock.
    // REFACTOR: processes in region 0 and region 1 can be activated concurrently.
    // -----------------------------------------------------------------------
    void activate_process(SimContext& ctx, int process_id) override
    {
        auto& proc = ctx.processes[process_id];

        // Find which region this process belongs to.
        int region_id = 0;
        for (auto& r : ctx.regions) {
            for (int pid : r.process_ids) {
                if (pid == process_id) { region_id = r.id; goto found; }
            }
        }
        found:

        {
            std::lock_guard lk(region_queue_locks_[region_id]);
            ProcessState expected = ProcessState::DORMANT;
            // Only transition DORMANT → READY; ignore if already READY or RUNNING.
            proc.state.compare_exchange_strong(
                expected, ProcessState::READY, std::memory_order_acq_rel);
        }

        if (proc.state.load(std::memory_order_relaxed) != ProcessState::READY) {
            contentions_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // -----------------------------------------------------------------------
    // Boundary signal sync — flush the written value across region boundaries.
    // Fine-grained: only this signal's lock is taken, not a global lock.
    // -----------------------------------------------------------------------
    void sync_boundary_signal(SimContext& ctx, int signal_id) override
    {
        // Read under shared lock, then notify waiters.
        LogicValue val;
        {
            std::shared_lock lk(signal_locks_[signal_id]);
            val = ctx.signals[signal_id].current_value;
        }
        // Store into atomic so atomic-strategy waiters wake up.
        ctx.signals[signal_id].atomic_value.store(
            static_cast<uint8_t>(val), std::memory_order_release);
        ctx.signals[signal_id].atomic_value.notify_all();
    }

    [[nodiscard]] SyncMode    mode()             const override { return SyncMode::FINE_GRAINED; }
    [[nodiscard]] const char* name()             const override { return "FINE_GRAINED"; }
    [[nodiscard]] uint64_t    contention_count() const override {
        return contentions_.load(std::memory_order_relaxed);
    }

private:
    // One shared_mutex per signal.  Stored in a vector indexed by signal_id.
    // Each mutex is on its own cache line (shared_mutex is typically 40+ bytes
    // on Linux, so no false-sharing between adjacent signals on x86).
    std::vector<std::shared_mutex> signal_locks_;

    // One mutex per region for process-queue operations.
    std::vector<std::mutex> region_queue_locks_;

    mutable std::atomic<uint64_t> contentions_{0};
};

} // namespace celeris
