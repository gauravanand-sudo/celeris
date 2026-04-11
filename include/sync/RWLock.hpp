#pragma once
/**
 * RWLock.hpp — RAII wrappers around std::shared_mutex for reader-writer locking.
 *
 * Used by FineGrainedStrategy for per-signal locks:
 *   - Multiple threads can READ a signal simultaneously (shared lock).
 *   - Only ONE thread can WRITE a signal at a time (unique lock).
 *
 * In a simulation workload, reads vastly outnumber writes (a signal is read by
 * every process in its sensitivity list but written only by its driver).
 * Shared-exclusive locking gives significant concurrency gains over a plain mutex.
 *
 * namespace celeris
 */

#include <shared_mutex>

namespace celeris {

/// RAII read-lock (shared) — multiple readers allowed simultaneously.
class ReadLock {
public:
    explicit ReadLock(std::shared_mutex& m) : m_(m) { m_.lock_shared(); }
    ~ReadLock() { m_.unlock_shared(); }
    ReadLock(const ReadLock&) = delete;
    ReadLock& operator=(const ReadLock&) = delete;
private:
    std::shared_mutex& m_;
};

/// RAII write-lock (exclusive) — only one writer, no concurrent readers.
class WriteLock {
public:
    explicit WriteLock(std::shared_mutex& m) : m_(m) { m_.lock(); }
    ~WriteLock() { m_.unlock(); }
    WriteLock(const WriteLock&) = delete;
    WriteLock& operator=(const WriteLock&) = delete;
private:
    std::shared_mutex& m_;
};

} // namespace celeris
