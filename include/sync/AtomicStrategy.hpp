#pragma once
/**
 * AtomicStrategy.hpp — Lock-free synchronization for signal hot paths.
 *
 * REFACTOR: From FineGrainedStrategy (per-signal mutex) to AtomicStrategy
 *           (no mutex at all on the critical signal read/write path).
 *
 * CORE IDEA:
 *   Signal values are uint8_t (LogicValue fits in 2 bits).
 *   std::atomic<uint8_t> operations are natively lock-free on every modern
 *   ISA (x86, ARM, RISC-V).  There is NO mutex acquisition, NO OS syscall,
 *   NO thread context switch — just a single load/store instruction with the
 *   appropriate memory barrier baked in.
 *
 * C++20 additions used here:
 *   std::atomic<T>::wait(old)      — efficient OS-level futex wait; the thread
 *                                    sleeps until the value changes from `old`.
 *                                    Replaces condition_variable for single-value
 *                                    watching (no spurious wakeups!).
 *   std::atomic<T>::notify_all()   — wake all threads waiting on this atomic.
 *
 * Process activation via CAS (compare-and-swap):
 *   We want exactly ONE thread to activate a process, even if multiple signals
 *   in its sensitivity list change simultaneously.
 *   CAS from DORMANT → READY: the first thread wins; all others see the CAS
 *   fail (process already READY) and skip — no double-activation, no mutex.
 *
 * Memory ordering:
 *   write_signal → memory_order_release  (all prior writes visible to readers)
 *   read_signal  → memory_order_acquire  (sees all writes that happened-before)
 *   This is the minimal correct ordering for producer-consumer.
 *
 * Contention counting:
 *   We count CAS failures (process already READY when we try to activate it).
 *   Unlike mutex contention this is NOT a blocking event — the thread continues
 *   immediately.  Fewer CAS failures = more efficient process scheduling.
 *
 * namespace celeris
 */

#include "ISyncStrategy.hpp"
#include <atomic>

namespace celeris {

class AtomicStrategy final : public ISyncStrategy {
public:
    // -----------------------------------------------------------------------
    // Signal read — ZERO LOCKS.  Single acquire-load instruction.
    // On x86 this compiles to a plain MOV (loads are acquire by default).
    // REFACTOR vs FineGrained: no shared_mutex overhead, no OS primitives.
    // -----------------------------------------------------------------------
    [[nodiscard]] LogicValue read_signal(
            const SimContext& ctx, int signal_id) override
    {
        // atomic_value stores the same data as current_value, kept in sync by
        // write_signal.  The acquire fence ensures we see any preceding writes.
        return static_cast<LogicValue>(
            ctx.signals[signal_id].atomic_value.load(std::memory_order_acquire));
    }

    // -----------------------------------------------------------------------
    // Signal write — ZERO LOCKS.  Single release-store + notify.
    // -----------------------------------------------------------------------
    void write_signal(
            SimContext& ctx, int signal_id, LogicValue val) override
    {
        // Keep the non-atomic copy in sync (used by FineGrained if strategy switches).
        ctx.signals[signal_id].current_value = val;

        // Release-store: all writes prior to this point are visible to any
        // thread that subsequently does an acquire-load on this atomic.
        ctx.signals[signal_id].atomic_value.store(
            static_cast<uint8_t>(val), std::memory_order_release);

        // C++20: wake all threads blocked in atomic_value.wait().
        // This is a lightweight futex-based notification — far cheaper than
        // condition_variable::notify_all() which requires a mutex acquisition.
        ctx.signals[signal_id].atomic_value.notify_all();
    }

    // -----------------------------------------------------------------------
    // Process activation — CAS from DORMANT → READY (no mutex).
    // If multiple signals change at once, exactly one activation wins.
    // -----------------------------------------------------------------------
    void activate_process(SimContext& ctx, int process_id) override
    {
        auto& proc = ctx.processes[process_id];
        ProcessState expected = ProcessState::DORMANT;

        // compare_exchange_strong: atomically checks that state == DORMANT,
        // if so sets it to READY and returns true.
        // If state is already READY or RUNNING, returns false (contention count++).
        bool activated = proc.state.compare_exchange_strong(
            expected,
            ProcessState::READY,
            std::memory_order_acq_rel,   // success: full fence
            std::memory_order_relaxed);  // failure: no fence needed

        if (!activated) {
            // Process was already READY or RUNNING — count as a (non-blocking) contention.
            contentions_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // -----------------------------------------------------------------------
    // Boundary signal sync — no extra work needed.
    // write_signal already did release-store + notify_all.
    // Any thread waiting with atomic_value.wait() is already unblocked.
    // -----------------------------------------------------------------------
    void sync_boundary_signal(SimContext& ctx, int signal_id) override
    {
        // Ensure the atomic value is current (in case it was only written via
        // the non-atomic path by a different strategy earlier).
        auto cur = static_cast<uint8_t>(ctx.signals[signal_id].current_value);
        ctx.signals[signal_id].atomic_value.store(cur, std::memory_order_release);
        ctx.signals[signal_id].atomic_value.notify_all();
    }

    // -----------------------------------------------------------------------
    // C++20 wait / notify helper — lets a process block until a signal changes.
    // Used by WorkerThread when a process has no work at the current delta.
    // -----------------------------------------------------------------------
    void wait_for_signal_change(const SimContext& ctx, int signal_id,
                                LogicValue current) const
    {
        // std::atomic::wait(old): blocks until value != old.
        // Implementation uses OS futex (Linux) or WaitOnAddress (Windows).
        // Far more efficient than spinning or using a condition_variable.
        ctx.signals[signal_id].atomic_value.wait(
            static_cast<uint8_t>(current), std::memory_order_acquire);
    }

    [[nodiscard]] SyncMode    mode()             const override { return SyncMode::ATOMIC; }
    [[nodiscard]] const char* name()             const override { return "ATOMIC"; }
    [[nodiscard]] uint64_t    contention_count() const override {
        return contentions_.load(std::memory_order_relaxed);
    }

private:
    // CAS failure counter — non-blocking contention (process already activated).
    mutable std::atomic<uint64_t> contentions_{0};
};

} // namespace celeris
