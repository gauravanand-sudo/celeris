#pragma once
/**
 * DeltaQueue.hpp — Ping-pong double-buffer for zero-delay (delta) events.
 *
 * A delta event is one scheduled at the SAME simulation time but a later
 * delta cycle.  Example: a combinational gate reads its inputs (delta N)
 * and writes its output (delta N) — but the output change is not visible
 * to downstream gates until delta N+1.
 *
 * Ping-pong double buffer:
 * ┌──────────────┐     draining      ┌──────────────┐
 * │  active_buf  │ ←── workers read  │  pending_buf │ ←── workers write
 * │  (read-only) │                   │  (write-only)│
 * └──────────────┘                   └──────────────┘
 *         ↑_______________swap at delta boundary_______________↑
 *
 * At the end of each delta cycle (triggered by the DeltaBarrier):
 *   1. Swap active_idx (0↔1) — pending becomes active, active becomes empty.
 *   2. Increment SimContext::current_delta.
 *   3. Return: is the new active buffer non-empty? (more deltas needed?)
 *
 * Locking strategy:
 *   - drain_lock_   : SpinLock protecting reads from the ACTIVE buffer.
 *                     Only one reader-group drains at a time.
 *   - insert_lock_  : SpinLock protecting writes to the PENDING buffer.
 *                     Fine-grained: insert and drain never contend each other
 *                     (they operate on different buffers).
 *
 * pending_count_ is an atomic counter for the fast-path check "any delta events?".
 * Workers check this without taking any lock.
 *
 * namespace celeris
 */

#include "../core/Event.hpp"
#include "../sync/SpinLock.hpp"
#include <array>
#include <vector>
#include <atomic>

namespace celeris {

class DeltaQueue {
public:
    DeltaQueue() = default;

    // -----------------------------------------------------------------------
    // push — add a delta event to the PENDING buffer.
    // Thread-safe: only insert_lock_ is taken.
    // Does NOT block drain operations on the ACTIVE buffer.
    // -----------------------------------------------------------------------
    void push(Event e)
    {
        SpinLockGuard lk(insert_lock_);
        int pending = 1 - active_idx_.load(std::memory_order_relaxed);
        buffers_[pending].push_back(e);
        pending_count_.fetch_add(1, std::memory_order_release);
    }

    // -----------------------------------------------------------------------
    // drain — extract all events from the ACTIVE buffer.
    // Returns the events; the active buffer is left empty.
    // -----------------------------------------------------------------------
    std::vector<Event> drain()
    {
        SpinLockGuard lk(drain_lock_);
        int active = active_idx_.load(std::memory_order_acquire);
        std::vector<Event> out = std::move(buffers_[active]);
        buffers_[active].clear();
        return out;
    }

    // -----------------------------------------------------------------------
    // flip_delta — called by DeltaBarrier completion function.
    // Swaps active and pending buffers.
    // Returns true if the new active buffer (previously pending) has events.
    // This determines whether another delta cycle is needed.
    // -----------------------------------------------------------------------
    bool flip_delta()
    {
        // After flip: active becomes the old pending buffer.
        // pending_count_ now represents the count in the new active buffer.
        int old_active = active_idx_.load(std::memory_order_relaxed);
        active_idx_.store(1 - old_active, std::memory_order_release);

        uint64_t cnt = pending_count_.exchange(0, std::memory_order_acq_rel);
        return cnt > 0;
    }

    // -----------------------------------------------------------------------
    // has_pending — fast-path check without any lock.
    // -----------------------------------------------------------------------
    [[nodiscard]] bool has_pending() const noexcept
    {
        return pending_count_.load(std::memory_order_acquire) > 0;
    }

    // -----------------------------------------------------------------------
    // size — number of events currently in pending buffer (approximate).
    // -----------------------------------------------------------------------
    [[nodiscard]] size_t pending_size() const noexcept
    {
        return static_cast<size_t>(
            pending_count_.load(std::memory_order_relaxed));
    }

    void reset()
    {
        buffers_[0].clear();
        buffers_[1].clear();
        active_idx_.store(0, std::memory_order_relaxed);
        pending_count_.store(0, std::memory_order_relaxed);
    }

private:
    // Two event buffers — ping and pong.
    std::array<std::vector<Event>, 2> buffers_;

    // Index of the currently ACTIVE (draining) buffer.
    // The PENDING (accepting inserts) buffer is always 1 - active_idx_.
    std::atomic<int>    active_idx_{0};

    // Number of events in the PENDING buffer.  Written under insert_lock_;
    // read lock-free by has_pending() for fast-path checks.
    std::atomic<uint64_t> pending_count_{0};

    // Fine-grained: drain and insert never contend each other.
    SpinLock drain_lock_;
    SpinLock insert_lock_;
};

} // namespace celeris
