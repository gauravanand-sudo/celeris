#pragma once
/**
 * LegacyEvent.hpp — Event types for the coarse-grained legacy engine.
 *
 * ╔══════════════════════════════════════════════════════════════════╗
 * ║  LEGACY CODE — preserved to show the "before" state.            ║
 * ║  This is identical in semantics to Event.hpp but is used only   ║
 * ║  by the LegacySimEngine to keep the two implementations cleanly  ║
 * ║  separated.                                                      ║
 * ╚══════════════════════════════════════════════════════════════════╝
 *
 * namespace celeris::legacy
 */

#include <cstdint>

namespace celeris {
namespace legacy {

enum class LegacyEventType : uint8_t {
    SIGNAL_UPDATE    = 0,
    PROCESS_ACTIVATE = 1,
    NBA              = 2,
    POSTPONED        = 3
};

enum class LegacyLogicValue : uint8_t { ZERO=0, ONE=1, X=2, Z=3 };

struct LegacyEvent {
    uint64_t         sim_time{0};
    uint32_t         delta{0};
    LegacyEventType  type{LegacyEventType::SIGNAL_UPDATE};
    int              signal_id{-1};
    LegacyLogicValue new_value{LegacyLogicValue::X};
    int              process_id{-1};
    uint64_t         event_id{0};

    bool operator<(const LegacyEvent& o) const noexcept {
        if (sim_time != o.sim_time) return sim_time < o.sim_time;
        return delta < o.delta;
    }
};

} // namespace legacy
} // namespace celeris
