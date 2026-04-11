#pragma once
/**
 * TimeWheel.hpp — Circular bucket time wheel for O(1) event scheduling.
 *
 * This is the primary event-time data structure in hardware simulation schedulers.
 * It outperforms a sorted priority queue for the common case where events are
 * scheduled within a bounded time window (e.g., within 1024 time units of now).
 *
 * ┌─────────────────────────────────────────────────────────────────────┐
 * │  Time Wheel — Circular array of event buckets                       │
 * │                                                                     │
 * │  bucket[0]  bucket[1]  ...  bucket[1023]  bucket[0]  bucket[1] ... │
 * │  t=0        t=1             t=1023         t=1024      t=1025        │
 * │                                                                     │
 * │  current_bucket → points to "now".  Events here are due this step. │
 * │  bucket_index = simulation_time & (WHEEL_SIZE - 1)  (fast modulo)  │
 * └─────────────────────────────────────────────────────────────────────┘
 *
 * Overflow list: events beyond current_time + WHEEL_SIZE are stored in a
 * sorted std::vector (overflow_heap_) and migrated into buckets when the
 * wheel pointer gets close enough.
 *
 * Fine-grained sync: each bucket has its own SpinLock so threads scheduling
 * events into different buckets do not contend.
 *
 * Fast-path check: bucket_nonempty_bitmask_ is an atomic bitmask (64 bits
 * covering 64 buckets at a time) so workers can quickly check "any events
 * in the next N steps?" without taking any lock.
 *
 * namespace celeris
 */

#include "../core/Event.hpp"
#include "../sync/SpinLock.hpp"
#include <array>
#include <vector>
#include <optional>
#include <algorithm>
#include <atomic>
#include <cassert>

namespace celeris {

class TimeWheel {
public:
    static constexpr int WHEEL_SIZE = 1024; // must be power of 2

    TimeWheel() = default;

    // -----------------------------------------------------------------------
    // schedule — insert an event into the wheel or overflow heap.
    // O(1) for near-future events; O(log n) for overflow.
    // Fine-grained: only locks the target bucket.
    // -----------------------------------------------------------------------
    void schedule(const Event& e)
    {
        uint64_t t = e.when.time;
        uint64_t now = current_time_.load(std::memory_order_relaxed);

        if (t < now) {
            // Past event — schedule at "now" (shouldn't happen in correct sim)
            schedule_in_bucket(e, now);
        } else if (t < now + WHEEL_SIZE) {
            schedule_in_bucket(e, t);
        } else {
            // Overflow: beyond wheel range
            std::lock_guard lk(overflow_lock_);
            overflow_heap_.push_back(e);
            std::push_heap(overflow_heap_.begin(), overflow_heap_.end(),
                [](const Event& a, const Event& b){ return a.when > b.when; });
        }
    }

    // -----------------------------------------------------------------------
    // drain_current_bucket — extract all events at current_time.
    // Called by the scheduler at the start of each time step.
    // -----------------------------------------------------------------------
    std::vector<Event> drain_current_bucket()
    {
        uint64_t now = current_time_.load(std::memory_order_acquire);
        int idx = bucket_index(now);

        SpinLockGuard lk(bucket_locks_[idx]);
        std::vector<Event> out = std::move(buckets_[idx]);
        buckets_[idx].clear();

        // Update bitmask — clear this bucket's bit
        if (out.empty()) return out;
        // Recheck after drain
        if (buckets_[idx].empty()) {
            bucket_nonempty_bitmask_.fetch_and(
                ~(uint64_t(1) << (idx & 63)), std::memory_order_relaxed);
        }
        return out;
    }

    // -----------------------------------------------------------------------
    // advance_time — move the wheel pointer forward to the next non-empty bucket.
    // Returns the new simulation time, or UINT64_MAX if no more events.
    // -----------------------------------------------------------------------
    uint64_t advance_time()
    {
        uint64_t now = current_time_.load(std::memory_order_relaxed);
        migrate_overflow(now);

        // Scan forward to find next non-empty bucket
        for (int step = 1; step < WHEEL_SIZE; ++step) {
            uint64_t candidate = now + step;
            int idx = bucket_index(candidate);
            SpinLockGuard lk(bucket_locks_[idx]);
            if (!buckets_[idx].empty()) {
                current_time_.store(candidate, std::memory_order_release);
                return candidate;
            }
        }

        // Check overflow heap
        {
            std::lock_guard lk(overflow_lock_);
            if (!overflow_heap_.empty()) {
                uint64_t next_t = overflow_heap_.front().when.time;
                current_time_.store(next_t, std::memory_order_release);
                migrate_overflow(next_t);
                return next_t;
            }
        }
        return UINT64_MAX; // no more events
    }

    // -----------------------------------------------------------------------
    // has_events — quick non-blocking check using bitmask.
    // -----------------------------------------------------------------------
    [[nodiscard]] bool has_events() const noexcept
    {
        if (bucket_nonempty_bitmask_.load(std::memory_order_relaxed) != 0)
            return true;
        std::lock_guard lk(overflow_lock_);
        return !overflow_heap_.empty();
    }

    [[nodiscard]] uint64_t current_time() const noexcept {
        return current_time_.load(std::memory_order_acquire);
    }

    void reset(uint64_t start_time = 0) {
        current_time_.store(start_time, std::memory_order_release);
        for (auto& b : buckets_) b.clear();
        std::lock_guard lk(overflow_lock_);
        overflow_heap_.clear();
        bucket_nonempty_bitmask_.store(0, std::memory_order_relaxed);
    }

private:
    static int bucket_index(uint64_t t) noexcept {
        return static_cast<int>(t & (WHEEL_SIZE - 1));
    }

    void schedule_in_bucket(const Event& e, uint64_t t)
    {
        int idx = bucket_index(t);
        SpinLockGuard lk(bucket_locks_[idx]);
        buckets_[idx].push_back(e);
        // Set corresponding bit in bitmask
        bucket_nonempty_bitmask_.fetch_or(
            uint64_t(1) << (idx & 63), std::memory_order_relaxed);
    }

    // Move overflow events that now fit within the wheel into their buckets.
    void migrate_overflow(uint64_t now)
    {
        std::lock_guard lk(overflow_lock_);
        while (!overflow_heap_.empty()) {
            const Event& top = overflow_heap_.front();
            if (top.when.time >= now + WHEEL_SIZE) break;
            Event e = top;
            std::pop_heap(overflow_heap_.begin(), overflow_heap_.end(),
                [](const Event& a, const Event& b){ return a.when > b.when; });
            overflow_heap_.pop_back();
            // Re-schedule into bucket (without holding overflow_lock again — safe
            // because we hold overflow_lock_ and schedule_in_bucket only takes bucket_locks_)
            schedule_in_bucket(e, e.when.time);
        }
    }

    // Circular array of event buckets
    std::array<std::vector<Event>, WHEEL_SIZE> buckets_;

    // Per-bucket spinlock (fine-grained)
    std::array<SpinLock, WHEEL_SIZE> bucket_locks_;

    // Fast-path bitmask: bit i set if bucket (i mod 64) has events.
    // Allows O(1) "any events?" check without taking any lock.
    std::atomic<uint64_t> bucket_nonempty_bitmask_{0};

    // Overflow: events beyond wheel range, stored as a min-heap by time.
    std::vector<Event>    overflow_heap_;
    mutable std::mutex    overflow_lock_;

    // Current simulation time (wheel's "now" pointer)
    std::atomic<uint64_t> current_time_{0};
};

} // namespace celeris
