#pragma once
/**
 * LegacyDeltaQueue.hpp — Delta queue with a single mutex for both buffers.
 *
 * ╔══════════════════════════════════════════════════════════════════╗
 * ║  LEGACY / BAD CODE                                               ║
 * ║                                                                  ║
 * ║  COARSE: insert and drain both contend on global_lock_.          ║
 * ║  A thread inserting a new delta event blocks a thread draining   ║
 * ║  the active buffer — even though they touch different buffers.   ║
 * ║                                                                  ║
 * ║  REFACTOR → DeltaQueue.hpp uses separate insert_lock_ and        ║
 * ║             drain_lock_ (SpinLocks), so insert/drain never       ║
 * ║             contend each other.                                  ║
 * ╚══════════════════════════════════════════════════════════════════╝
 *
 * namespace celeris::legacy
 */

#include "LegacyEvent.hpp"
#include <array>
#include <vector>
#include <mutex>
#include <atomic>

namespace celeris {
namespace legacy {

class LegacyDeltaQueue {
public:
    // LEGACY: push and drain share the same mutex.
    void push(const LegacyEvent& e)
    {
        std::lock_guard<std::mutex> lk(global_lock_); // COARSE: blocks drain()
        int pending = 1 - active_idx_;
        buffers_[pending].push_back(e);
        pending_count_++;
    }

    // LEGACY: drain holds the same lock as push.
    // A concurrent push() must wait until drain() finishes — unnecessary.
    std::vector<LegacyEvent> drain()
    {
        std::lock_guard<std::mutex> lk(global_lock_); // COARSE: blocks push()
        std::vector<LegacyEvent> out = std::move(buffers_[active_idx_]);
        buffers_[active_idx_].clear();
        return out;
    }

    // LEGACY: flip also holds the global lock.
    bool flip_delta()
    {
        std::lock_guard<std::mutex> lk(global_lock_);
        active_idx_ = 1 - active_idx_;
        bool has_more = (pending_count_ > 0);
        pending_count_ = 0;
        return has_more;
    }

    bool has_pending() const
    {
        std::lock_guard<std::mutex> lk(global_lock_); // COARSE: even a read takes the lock
        return pending_count_ > 0;
    }

    void reset()
    {
        std::lock_guard<std::mutex> lk(global_lock_);
        buffers_[0].clear();
        buffers_[1].clear();
        active_idx_ = 0;
        pending_count_ = 0;
    }

private:
    // LEGACY: ONE mutex for insert AND drain.  No true ping-pong isolation.
    mutable std::mutex global_lock_;

    std::array<std::vector<LegacyEvent>, 2> buffers_;
    int    active_idx_{0};
    size_t pending_count_{0};
};

} // namespace legacy
} // namespace celeris
