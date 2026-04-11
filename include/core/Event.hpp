#pragma once
/**
 * Event.hpp — Core event types for the celeris multicore simulation engine.
 *
 * Models a hardware simulator event taxonomy:
 *   - SIGNAL_UPDATE  : a net/wire changes value (the most common event type)
 *   - PROCESS_ACTIVATE: a process is scheduled for evaluation
 *   - NBA            : non-blocking assignment (write deferred to NBA region)
 *   - POSTPONED      : $monitor / $strobe evaluation (end of time step)
 *   - MONITOR        : $monitor callbacks
 *
 * SimTime is a compound key: (simulation_time, delta_cycle).
 * Two events at the same wall-clock time but different delta cycles are ordered
 * by delta, matching the IEEE 1800 SystemVerilog scheduling semantics.
 *
 * namespace celeris
 */

#include <cstdint>
#include <compare>

namespace celeris {

// ---------------------------------------------------------------------------
// EventType — the region of the simulation scheduling algorithm this event
// belongs to.  Mirrors the five scheduling regions in IEEE 1800-2017 §4.
// ---------------------------------------------------------------------------
enum class EventType : uint8_t {
    SIGNAL_UPDATE    = 0,  ///< Net/wire value change — enters Active region
    PROCESS_ACTIVATE = 1,  ///< Process wake-up — evaluated in Active region
    NBA              = 2,  ///< Non-blocking assign update — NBA region
    POSTPONED        = 3,  ///< $strobe/$monitor — Postponed region
    MONITOR          = 4   ///< Monitor callbacks
};

// ---------------------------------------------------------------------------
// LogicValue — four-state logic matching std_logic / Verilog 1/0/X/Z
// Fits in 2 bits so it can be stored in std::atomic<uint8_t> cheaply.
// ---------------------------------------------------------------------------
enum class LogicValue : uint8_t {
    ZERO = 0,
    ONE  = 1,
    X    = 2,  ///< Unknown / uninitialized
    Z    = 3   ///< High-impedance
};

// ---------------------------------------------------------------------------
// SimTime — compound simulation timestamp.
// The primary ordering key is (time, delta): events at the same wall-clock
// time but later delta cycles must not affect earlier delta cycles.
// This matches the standard delta-time model used in IEEE 1800 simulators.
// ---------------------------------------------------------------------------
struct SimTime {
    uint64_t time{0};   ///< Simulation time in the design's time unit (e.g. ps)
    uint32_t delta{0};  ///< Delta cycle within the same time step

    [[nodiscard]] constexpr bool operator<(const SimTime& o) const noexcept {
        if (time != o.time) return time < o.time;
        return delta < o.delta;
    }
    [[nodiscard]] constexpr bool operator==(const SimTime& o) const noexcept {
        return time == o.time && delta == o.delta;
    }
    [[nodiscard]] constexpr bool operator<=(const SimTime& o) const noexcept {
        return !(o < *this);
    }
    [[nodiscard]] constexpr bool operator>(const SimTime& o) const noexcept {
        return o < *this;
    }

    // C++20 three-way comparison
    [[nodiscard]] constexpr auto operator<=>(const SimTime& o) const noexcept {
        if (time != o.time) return time <=> o.time;
        return delta <=> o.delta;
    }
};

// ---------------------------------------------------------------------------
// Event — the fundamental unit of work in the simulation event queue.
//
// Design note: kept POD-like (no vtable, no heap) so it can be stored in flat
// arrays and lock-free ring buffers without pointer chasing.  This matches
// This matches the typical POD event layout used in hardware simulators.
// ---------------------------------------------------------------------------
struct Event {
    SimTime     when;           ///< Scheduled simulation time + delta
    EventType   type;           ///< Which scheduling region this belongs to
    int         signal_id;      ///< Signal affected (-1 if process-only event)
    LogicValue  new_value;      ///< Value the signal will take (for SIGNAL_UPDATE)
    int         process_id;     ///< Process to wake (-1 = wake all sensitized procs)
    uint64_t    event_id{0};    ///< Monotonically increasing ID (assigned atomically)

    [[nodiscard]] constexpr bool operator<(const Event& o) const noexcept {
        return when < o.when;
    }
};

} // namespace celeris
