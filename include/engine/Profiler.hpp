#pragma once
/**
 * Profiler.hpp — Atomic performance counters for the simulation engine.
 *
 * All counters are std::atomic<uint64_t> so any worker thread can increment
 * them without a lock.  Relaxed memory ordering is sufficient — we don't
 * need the counter increments to be globally ordered relative to each other,
 * just eventually consistent for the final report.
 *
 * namespace celeris
 */

#include <atomic>
#include <chrono>
#include <ostream>
#include <iomanip>

namespace celeris {

struct Profiler {
    std::atomic<uint64_t> total_events{0};
    std::atomic<uint64_t> delta_cycles{0};
    std::atomic<uint64_t> time_steps{0};
    std::atomic<uint64_t> signal_updates{0};
    std::atomic<uint64_t> process_evaluations{0};
    std::atomic<uint64_t> barrier_waits{0};
    std::atomic<uint64_t> contention_events{0};
    std::atomic<uint64_t> nba_updates{0};

    void reset() {
        total_events.store(0);
        delta_cycles.store(0);
        time_steps.store(0);
        signal_updates.store(0);
        process_evaluations.store(0);
        barrier_waits.store(0);
        contention_events.store(0);
        nba_updates.store(0);
    }

    [[nodiscard]] double events_per_second(std::chrono::duration<double> elapsed) const
    {
        double secs = elapsed.count();
        if (secs <= 0.0) return 0.0;
        return static_cast<double>(total_events.load(std::memory_order_relaxed)) / secs;
    }

    void report(std::ostream& out, std::chrono::duration<double> elapsed,
                const char* strategy_name) const
    {
        out << "\n  ┌─────────────────────────────────────────────┐\n";
        out << "  │  Strategy: " << std::left << std::setw(31) << strategy_name << "  │\n";
        out << "  ├─────────────────────────────────────────────┤\n";
        out << "  │  Total events processed  : "
            << std::right << std::setw(14) << total_events.load() << "  │\n";
        out << "  │  Delta cycles            : "
            << std::setw(14) << delta_cycles.load() << "  │\n";
        out << "  │  Time steps              : "
            << std::setw(14) << time_steps.load() << "  │\n";
        out << "  │  Signal updates          : "
            << std::setw(14) << signal_updates.load() << "  │\n";
        out << "  │  Process evaluations     : "
            << std::setw(14) << process_evaluations.load() << "  │\n";
        out << "  │  NBA updates             : "
            << std::setw(14) << nba_updates.load() << "  │\n";
        out << "  │  Barrier waits           : "
            << std::setw(14) << barrier_waits.load() << "  │\n";
        out << "  │  Contention events       : "
            << std::setw(14) << contention_events.load() << "  │\n";
        out << "  │  Wall time (ms)          : "
            << std::setw(14) << std::fixed << std::setprecision(2)
            << elapsed.count() * 1000.0 << "  │\n";
        out << "  │  Events/sec              : "
            << std::setw(14) << std::fixed << std::setprecision(0)
            << events_per_second(elapsed) << "  │\n";
        out << "  └─────────────────────────────────────────────┘\n";
    }
};

} // namespace celeris
