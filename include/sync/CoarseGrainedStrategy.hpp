#pragma once
/**
 * CoarseGrainedStrategy.hpp — Single global mutex for ALL synchronization.
 *
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║  LEGACY / BAD CODE — intentionally suboptimal for comparison.       ║
 * ║  This is the "before" state that we refactor away from.             ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 *
 * WHY THIS IS BAD:
 *   Every operation — reading a signal, writing a signal, activating a
 *   process, scheduling an event — takes the SAME global_lock.
 *
 *   Consequences:
 *   1. ZERO parallelism: only one thread executes at a time.
 *   2. A thread reading signal A blocks all other threads trying to read
 *      signal B — even though there is no data dependency between them.
 *   3. A thread scheduling a future event blocks all process evaluations.
 *   4. Lock contention explodes with thread count — adding more cores
 *      actually slows the simulation due to OS scheduling overhead.
 *   5. Cache thrashing: every core repeatedly invalidates the same
 *      cache line (the mutex's internal state word).
 *
 * This mirrors the common early state of multicore simulators before
 * fine-grained refactoring, where adding a second thread degrades throughput
 * due to lock serialization outweighing parallelism gains.
 *
 * namespace celeris
 */

#include "ISyncStrategy.hpp"
#include <mutex>
#include <atomic>

namespace celeris {

class CoarseGrainedStrategy final : public ISyncStrategy {
public:
    // -----------------------------------------------------------------------
    // Signal read — COARSE: takes the GLOBAL lock even for a pure read.
    // Multiple readers that could safely run concurrently are fully serialized.
    // -----------------------------------------------------------------------
    [[nodiscard]] LogicValue read_signal(
            const SimContext& ctx, int signal_id) override
    {
        // COARSE: ONE lock for everything. Reading signal 0 blocks a write to signal 7.
        std::lock_guard<std::mutex> lk(global_lock_);
        contentions_.fetch_add(1, std::memory_order_relaxed); // always counts as contention
        return ctx.signals[signal_id].current_value;
    }

    // -----------------------------------------------------------------------
    // Signal write — COARSE: same global lock as the read.
    // -----------------------------------------------------------------------
    void write_signal(
            SimContext& ctx, int signal_id, LogicValue val) override
    {
        // COARSE: write blocks ALL readers on ALL signals.
        std::lock_guard<std::mutex> lk(global_lock_);
        ctx.signals[signal_id].current_value = val;

        // Also update atomic_value so AtomicStrategy benchmarks see correct data
        // if we ever switch strategy mid-run.
        ctx.signals[signal_id].atomic_value.store(
            static_cast<uint8_t>(val), std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------------
    // Process activation — COARSE: global lock guards the state transition.
    // -----------------------------------------------------------------------
    void activate_process(SimContext& ctx, int process_id) override
    {
        std::lock_guard<std::mutex> lk(global_lock_);
        auto& proc = ctx.processes[process_id];
        // No CAS — just overwrite (could cause double activation under races,
        // but the lock prevents races here anyway).
        proc.state.store(ProcessState::READY, std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------------
    // Boundary signal sync — COARSE: no per-boundary logic; everything already
    // serialized by the global lock.
    // -----------------------------------------------------------------------
    void sync_boundary_signal(SimContext& /*ctx*/, int /*signal_id*/) override
    {
        // Nothing additional needed — the global lock already ensures
        // that any write to this signal happened-before any subsequent read
        // by any thread.  This "correctness by over-synchronization" is the
        // hallmark of coarse-grained locking.
        std::lock_guard<std::mutex> lk(global_lock_);
        // no-op body, but we still pay the lock/unlock cost.
    }

    // -----------------------------------------------------------------------
    // Diagnostics
    // -----------------------------------------------------------------------
    [[nodiscard]] SyncMode    mode()             const override { return SyncMode::COARSE_GRAINED; }
    [[nodiscard]] const char* name()             const override { return "COARSE_GRAINED"; }
    [[nodiscard]] uint64_t    contention_count() const override {
        return contentions_.load(std::memory_order_relaxed);
    }

private:
    // THE single global mutex.  All threads fight over this one lock.
    // On a 16-core machine running a 100k-event simulation, this becomes
    // the most-contended cache line in the process.
    mutable std::mutex global_lock_;

    // We count every operation as a "contention" here since under coarse-grain
    // every operation *could* have blocked another.
    mutable std::atomic<uint64_t> contentions_{0};
};

} // namespace celeris
