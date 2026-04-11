#pragma once
/**
 * LegacyTimeWheel.hpp — Time wheel with a SINGLE global mutex.
 *
 * ╔══════════════════════════════════════════════════════════════════╗
 * ║  LEGACY / BAD CODE — single lock protects the ENTIRE wheel.    ║
 * ║                                                                  ║
 * ║  COARSE: Every schedule() and drain() call takes global_lock_.  ║
 * ║  Two threads scheduling into bucket 0 and bucket 512 (which     ║
 * ║  share no data) still block each other.                         ║
 * ║                                                                  ║
 * ║  REFACTOR → TimeWheel.hpp uses per-bucket SpinLock[].           ║
 * ╚══════════════════════════════════════════════════════════════════╝
 *
 * namespace celeris::legacy
 */

#include "LegacyEvent.hpp"
#include <array>
#include <vector>
#include <mutex>
#include <cstdint>
#include <algorithm>

namespace celeris {
namespace legacy {

class LegacyTimeWheel {
public:
    static constexpr int WHEEL_SIZE = 1024;

    // LEGACY: schedule() takes the ONE global lock regardless of which bucket.
    void schedule(const LegacyEvent& e)
    {
        std::lock_guard<std::mutex> lk(global_lock_); // COARSE: blocks ALL other threads
        uint64_t now = current_time_;
        if (e.sim_time < now + WHEEL_SIZE) {
            int idx = static_cast<int>(e.sim_time & (WHEEL_SIZE - 1));
            buckets_[idx].push_back(e);
        } else {
            overflow_.push_back(e);
            std::push_heap(overflow_.begin(), overflow_.end(),
                [](const LegacyEvent& a, const LegacyEvent& b){ return a.sim_time > b.sim_time; });
        }
    }

    // LEGACY: drain_current_bucket() takes the same global lock.
    // While draining, no other thread can schedule ANY new event.
    std::vector<LegacyEvent> drain_current_bucket()
    {
        std::lock_guard<std::mutex> lk(global_lock_); // COARSE: blocks all schedulers
        int idx = static_cast<int>(current_time_ & (WHEEL_SIZE - 1));
        std::vector<LegacyEvent> out = std::move(buckets_[idx]);
        buckets_[idx].clear();
        return out;
    }

    uint64_t advance_time()
    {
        std::lock_guard<std::mutex> lk(global_lock_); // COARSE: yet another global lock
        // Migrate overflow events
        while (!overflow_.empty()) {
            if (overflow_.front().sim_time < current_time_ + WHEEL_SIZE) {
                LegacyEvent e = overflow_.front();
                std::pop_heap(overflow_.begin(), overflow_.end(),
                    [](const LegacyEvent& a, const LegacyEvent& b){ return a.sim_time > b.sim_time; });
                overflow_.pop_back();
                int idx = static_cast<int>(e.sim_time & (WHEEL_SIZE - 1));
                buckets_[idx].push_back(e);
            } else break;
        }
        // Find next non-empty bucket
        for (int step = 1; step < WHEEL_SIZE; ++step) {
            uint64_t candidate = current_time_ + step;
            int idx = static_cast<int>(candidate & (WHEEL_SIZE - 1));
            if (!buckets_[idx].empty()) {
                current_time_ = candidate;
                return candidate;
            }
        }
        if (!overflow_.empty()) {
            current_time_ = overflow_.front().sim_time;
            return current_time_;
        }
        return UINT64_MAX;
    }

    bool has_events() const
    {
        std::lock_guard<std::mutex> lk(global_lock_); // COARSE: even a read takes the lock
        for (auto& b : buckets_) if (!b.empty()) return true;
        return !overflow_.empty();
    }

    uint64_t current_time() const
    {
        std::lock_guard<std::mutex> lk(global_lock_);
        return current_time_;
    }

private:
    // LEGACY: ONE mutex for everything.  This is the root cause of poor scalability.
    mutable std::mutex global_lock_;

    std::array<std::vector<LegacyEvent>, WHEEL_SIZE> buckets_;
    std::vector<LegacyEvent> overflow_;
    uint64_t current_time_{0};
};

} // namespace legacy
} // namespace celeris
