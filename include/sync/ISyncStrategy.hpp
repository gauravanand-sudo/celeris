#pragma once
/**
 * ISyncStrategy.hpp — Strategy interface for synchronization hot paths.
 *
 * This is the core of the Strategy design pattern used across the engine.
 * The simulation engine holds a std::unique_ptr<ISyncStrategy> that can be
 * swapped at runtime to switch between three synchronization modes:
 *
 *   COARSE_GRAINED — single global std::mutex (legacy, bad, educational)
 *   FINE_GRAINED   — per-signal shared_mutex + per-bucket spinlocks
 *   ATOMIC         — lock-free atomics + C++20 atomic::wait/notify_all
 *
 * WHY a strategy pattern?
 *   In multicore simulation engines, the synchronization strategy is selected
 *   at elaboration time based on design topology (single-core vs multi-core,
 *   partition size, expected contention). The strategy pattern lets us:
 *     1. Benchmark all three approaches on the same workload.
 *     2. Switch mode without changing any call site (Open/Closed Principle).
 *     3. Demonstrate clearly which locking primitive is responsible for
 *        each synchronization decision.
 *
 * All hot-path methods are pure virtual with [[nodiscard]] on read operations.
 *
 * namespace celeris
 */

#include "../core/SimContext.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace celeris {

// Forward declarations
class EventScheduler;
struct Event;

// ---------------------------------------------------------------------------
// SyncMode — runtime selector for the strategy.
// ---------------------------------------------------------------------------
enum class SyncMode : uint8_t {
    COARSE_GRAINED = 0,
    FINE_GRAINED   = 1,
    ATOMIC         = 2
};

inline const char* sync_mode_name(SyncMode m) {
    switch (m) {
        case SyncMode::COARSE_GRAINED: return "COARSE_GRAINED";
        case SyncMode::FINE_GRAINED:   return "FINE_GRAINED";
        case SyncMode::ATOMIC:         return "ATOMIC";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// ISyncStrategy — abstract base for all synchronization strategies.
// ---------------------------------------------------------------------------
class ISyncStrategy {
public:
    virtual ~ISyncStrategy() = default;

    // -----------------------------------------------------------------------
    // Signal value operations — called on the HOT PATH (millions of times/sec)
    // -----------------------------------------------------------------------

    /// Read the current logic value of signal `signal_id`.
    /// Fine-grained: takes shared_lock on that signal.
    /// Atomic: loads with memory_order_acquire (no lock).
    [[nodiscard]] virtual LogicValue read_signal(
        const SimContext& ctx, int signal_id) = 0;

    /// Write a new logic value to signal `signal_id`.
    /// Fine-grained: takes unique_lock on that signal.
    /// Atomic: stores with memory_order_release + notify_all.
    virtual void write_signal(
        SimContext& ctx, int signal_id, LogicValue val) = 0;

    // -----------------------------------------------------------------------
    // Process activation — called when a signal change triggers sensitive procs
    // -----------------------------------------------------------------------

    /// Mark process `process_id` as READY (transition from DORMANT).
    /// Atomic: uses CAS (compare_exchange_strong) to avoid spurious double-activation.
    /// Fine-grained / Coarse: uses region_queue_lock.
    virtual void activate_process(SimContext& ctx, int process_id) = 0;

    // -----------------------------------------------------------------------
    // Cross-region boundary signal synchronization
    // -----------------------------------------------------------------------

    /// Flush the current value of a boundary signal to all other regions.
    /// This is the cross-core cache-coherency equivalent in simulation.
    virtual void sync_boundary_signal(SimContext& ctx, int signal_id) = 0;

    // -----------------------------------------------------------------------
    // Diagnostics
    // -----------------------------------------------------------------------

    [[nodiscard]] virtual SyncMode    mode()             const = 0;
    [[nodiscard]] virtual const char* name()             const = 0;

    /// Number of times a thread had to wait (mutex contention or CAS failure).
    /// Higher = more lock overhead. Used in the benchmark output.
    [[nodiscard]] virtual uint64_t contention_count()    const = 0;
};

} // namespace celeris
