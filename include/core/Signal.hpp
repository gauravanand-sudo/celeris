#pragma once
/**
 * Signal.hpp — Net/Wire abstraction for the celeris simulation engine.
 *
 * A Signal represents a single net (wire) in the simulated design.
 * It has:
 *   - A current value (readable by all sensitive processes)
 *   - A scheduled NBA value (for non-blocking assignments; written at end of
 *     Active region, read in NBA region — IEEE 1800 §4.9.4)
 *   - A list of process IDs that are sensitive to this signal
 *   - An atomic value field used by AtomicStrategy for the lock-free hot path
 *
 * Synchronization note:
 *   FineGrainedStrategy protects current_value with a per-signal shared_mutex.
 *   AtomicStrategy uses atomic_value directly (no lock needed).
 *   CoarseGrainedStrategy uses the single global mutex in that class.
 *
 * namespace celeris
 */

#include "Event.hpp"
#include <atomic>
#include <string>
#include <vector>

namespace celeris {

struct Signal {
    int         id{-1};
    std::string name;

    /// Current value of the signal — the value that drives all loads right now.
    /// Protected by strategy (shared_mutex for fine-grained, global mutex for coarse).
    LogicValue  current_value{LogicValue::X};

    /// Scheduled value for a non-blocking assignment (NBA region).
    /// Written during Active evaluation, applied at the NBA fence.
    LogicValue  scheduled_value{LogicValue::X};

    /// True when a non-blocking assignment is pending for this signal.
    bool        has_nba_pending{false};

    /// IDs of processes that evaluate whenever this signal changes.
    /// Immutable after construction — safe to read without locks.
    std::vector<int> sensitive_processes;

    /// Atomic copy of the signal value, used by AtomicStrategy.
    /// Storing as uint8_t because std::atomic<LogicValue> requires
    /// LogicValue to be trivially copyable (it is, but some older ABIs
    /// are finicky); casting on read/write is cheap.
    std::atomic<uint8_t> atomic_value{static_cast<uint8_t>(LogicValue::X)};

    // Non-copyable because of atomic member.
    // Custom move constructor: atomics are not movable by default; we load the value.
    Signal() = default;
    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;

    Signal(Signal&& o) noexcept
        : id(o.id)
        , name(std::move(o.name))
        , current_value(o.current_value)
        , scheduled_value(o.scheduled_value)
        , has_nba_pending(o.has_nba_pending)
        , sensitive_processes(std::move(o.sensitive_processes))
        , atomic_value(o.atomic_value.load(std::memory_order_relaxed))
    {}

    Signal& operator=(Signal&& o) noexcept {
        if (this != &o) {
            id = o.id;
            name = std::move(o.name);
            current_value = o.current_value;
            scheduled_value = o.scheduled_value;
            has_nba_pending = o.has_nba_pending;
            sensitive_processes = std::move(o.sensitive_processes);
            atomic_value.store(o.atomic_value.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
        }
        return *this;
    }
};

} // namespace celeris
