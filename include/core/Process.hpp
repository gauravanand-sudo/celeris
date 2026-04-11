#pragma once
/**
 * Process.hpp — Simulation process abstraction for the celeris engine.
 *
 * A Process corresponds to one "always" block, "initial" block, or continuous
 * assign in the design under test.  In compiled-simulation engines the
 * equivalent is an "event node" in the compiled simulation graph.
 *
 * Lifecycle:
 *   DORMANT  → (signal in sensitivity list changes)  → READY
 *   READY    → (worker thread picks it up)            → RUNNING
 *   RUNNING  → (evaluate() completes)                 → DORMANT
 *   BLOCKED  → used when a process is waiting on an explicit @(posedge clk)
 *              inside a fork..join_none (future extension)
 *
 * Thread safety:
 *   state is std::atomic<ProcessState> so that a worker can CAS it from
 *   DORMANT → READY without holding any additional lock (AtomicStrategy path).
 *   activation_count is only written by the owning worker thread — no lock
 *   needed for that field.
 *
 * namespace celeris
 */

#include "Event.hpp"
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace celeris {

// Forward declaration so Process can hold a reference to SimContext.
struct SimContext;

// ---------------------------------------------------------------------------
// ProcessState — lifecycle state of a simulation process.
// ---------------------------------------------------------------------------
enum class ProcessState : uint8_t {
    DORMANT  = 0,   ///< Not sensitive / waiting for trigger
    READY    = 1,   ///< Has been triggered, queued for evaluation
    RUNNING  = 2,   ///< Currently being evaluated by a worker thread
    BLOCKED  = 3    ///< Explicitly waiting (fork/join, future use)
};

// ---------------------------------------------------------------------------
// Process — the executable unit of simulation work.
// ---------------------------------------------------------------------------
struct Process {
    int         id{-1};
    std::string name;

    /// Signal IDs that trigger this process.  Immutable after construction.
    std::vector<int> sensitivity_list;

    /// Current lifecycle state.  Modified atomically so AtomicStrategy can do
    /// a CAS from DORMANT→READY without a mutex.
    std::atomic<ProcessState> state{ProcessState::DORMANT};

    /// The actual simulation logic.  Called with the current SimContext so the
    /// process can read signals and schedule new events.
    std::function<void(SimContext&)> evaluate;

    /// Cumulative count of times this process has been activated (profiling).
    uint64_t activation_count{0};

    // Non-copyable (atomic + function member).
    // Custom move constructor: std::atomic<ProcessState> is not movable by default.
    Process() = default;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    Process(Process&& o) noexcept
        : id(o.id)
        , name(std::move(o.name))
        , sensitivity_list(std::move(o.sensitivity_list))
        , state(o.state.load(std::memory_order_relaxed))
        , evaluate(std::move(o.evaluate))
        , activation_count(o.activation_count)
    {}

    Process& operator=(Process&& o) noexcept {
        if (this != &o) {
            id = o.id;
            name = std::move(o.name);
            sensitivity_list = std::move(o.sensitivity_list);
            state.store(o.state.load(std::memory_order_relaxed), std::memory_order_relaxed);
            evaluate = std::move(o.evaluate);
            activation_count = o.activation_count;
        }
        return *this;
    }
};

} // namespace celeris
