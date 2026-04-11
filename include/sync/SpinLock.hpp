#pragma once
/**
 * SpinLock.hpp — Test-and-set spinlock with exponential backoff.
 *
 * Used as the lightweight per-half lock inside DeltaQueue and as a building
 * block inside FineGrainedStrategy for hot-path critical sections that are
 * expected to be held for only a handful of instructions.
 *
 * Exponential backoff: on contention we double the pause count up to a cap.
 * This reduces cache-line invalidation traffic on the flag, matching the
 * strategy used in Intel's TBB and the LKMM's qspinlock.
 *
 * We use std::atomic_flag (the only atomic guaranteed to be lock-free) with
 * test_and_set/clear rather than compare_exchange so that this works even on
 * platforms that don't have hardware CAS.
 *
 * namespace celeris
 */

#include <atomic>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#  include <immintrin.h>
#  define CELERIS_PAUSE() _mm_pause()
#else
#  define CELERIS_PAUSE() std::this_thread::yield()
#endif

namespace celeris {

class SpinLock {
public:
    SpinLock() = default;

    // Non-copyable, non-movable (atomic_flag).
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    /// Acquire the spinlock.  Spins with exponential backoff to reduce
    /// cache-coherency traffic when the lock is heavily contended.
    void lock() noexcept {
        int backoff = 1;
        // test() without acquiring — optimistic read to avoid coherence traffic
        while (flag_.test_and_set(std::memory_order_acquire)) {
            // Busy-wait with pauses
            for (int i = 0; i < backoff; ++i) {
                CELERIS_PAUSE();
            }
            // Double the backoff, cap at 1024 pause iterations
            if (backoff < 1024) backoff *= 2;
        }
    }

    /// Try to acquire without blocking.  Returns true on success.
    [[nodiscard]] bool try_lock() noexcept {
        return !flag_.test_and_set(std::memory_order_acquire);
    }

    /// Release the spinlock.
    void unlock() noexcept {
        flag_.clear(std::memory_order_release);
    }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

// ---------------------------------------------------------------------------
// RAII guard for SpinLock — matches std::lock_guard interface so it works
// with structured bindings and scoped regions.
// ---------------------------------------------------------------------------
class SpinLockGuard {
public:
    explicit SpinLockGuard(SpinLock& sl) noexcept : sl_(sl) { sl_.lock(); }
    ~SpinLockGuard() noexcept { sl_.unlock(); }
    SpinLockGuard(const SpinLockGuard&) = delete;
    SpinLockGuard& operator=(const SpinLockGuard&) = delete;
private:
    SpinLock& sl_;
};

} // namespace celeris
