#pragma once
/**
 * SyncStrategyFactory.hpp — Runtime factory for ISyncStrategy implementations.
 *
 * Allows the simulation engine to select a synchronization strategy at runtime:
 *   - By explicit SyncMode enum
 *   - By string (from command-line arg or environment variable CELERIS_SYNC_MODE)
 *
 * Usage:
 *   auto strategy = SyncStrategyFactory::create(SyncMode::ATOMIC, num_signals, num_regions);
 *   engine.set_strategy(std::move(strategy));
 *
 * This is the "factory" part of the Factory + Strategy combined pattern.
 * The factory centralizes construction so that main.cpp doesn't need to
 * #include all three strategy headers — just this one factory header.
 *
 * namespace celeris
 */

#include "ISyncStrategy.hpp"
#include "CoarseGrainedStrategy.hpp"
#include "FineGrainedStrategy.hpp"
#include "AtomicStrategy.hpp"
#include <memory>
#include <string_view>
#include <stdexcept>

namespace celeris {

class SyncStrategyFactory {
public:
    /// Create a strategy for the given mode.
    /// num_signals and num_regions are needed by FineGrainedStrategy to
    /// pre-allocate per-resource locks.
    [[nodiscard]] static std::unique_ptr<ISyncStrategy>
    create(SyncMode mode, int num_signals = 64, int num_regions = 4)
    {
        switch (mode) {
            case SyncMode::COARSE_GRAINED:
                return std::make_unique<CoarseGrainedStrategy>();
            case SyncMode::FINE_GRAINED:
                return std::make_unique<FineGrainedStrategy>(num_signals, num_regions);
            case SyncMode::ATOMIC:
                return std::make_unique<AtomicStrategy>();
        }
        throw std::invalid_argument("Unknown SyncMode");
    }

    /// Parse a SyncMode from a string (case-insensitive).
    /// Accepts: "coarse", "coarse_grained", "fine", "fine_grained", "atomic"
    [[nodiscard]] static SyncMode mode_from_string(std::string_view s)
    {
        if (s == "coarse" || s == "coarse_grained" || s == "COARSE_GRAINED")
            return SyncMode::COARSE_GRAINED;
        if (s == "fine" || s == "fine_grained" || s == "FINE_GRAINED")
            return SyncMode::FINE_GRAINED;
        if (s == "atomic" || s == "ATOMIC")
            return SyncMode::ATOMIC;
        throw std::invalid_argument(
            std::string("Unknown sync mode: ") + std::string(s));
    }

    /// Return the canonical name string for a SyncMode.
    [[nodiscard]] static const char* mode_name(SyncMode mode) noexcept
    {
        switch (mode) {
            case SyncMode::COARSE_GRAINED: return "COARSE_GRAINED";
            case SyncMode::FINE_GRAINED:   return "FINE_GRAINED";
            case SyncMode::ATOMIC:         return "ATOMIC";
        }
        return "UNKNOWN";
    }

    /// Read CELERIS_SYNC_MODE from environment; fall back to default_mode.
    [[nodiscard]] static SyncMode mode_from_env(SyncMode default_mode = SyncMode::FINE_GRAINED)
    {
        const char* env = std::getenv("CELERIS_SYNC_MODE");
        if (!env) return default_mode;
        try { return mode_from_string(env); }
        catch (...) { return default_mode; }
    }
};

} // namespace celeris
