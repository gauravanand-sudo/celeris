#pragma once
/**
 * EventScheduler.hpp — Composes TimeWheel + DeltaQueue into the unified scheduler.
 *
 * The EventScheduler is the central dispatch table for the simulation engine.
 * It exactly mirrors the scheduling algorithm described in IEEE 1800-2017 §4:
 *
 *   ┌─────────────────────────────────────────────────────┐
 *   │  IEEE 1800 Simulation Scheduling Algorithm          │
 *   │                                                     │
 *   │  while (events exist):                              │
 *   │    advance to smallest T with active events         │
 *   │    repeat:                                          │
 *   │      run Active region (signal updates, processes)  │
 *   │      run NBA region (non-blocking assign updates)   │
 *   │      [more events at same T?] → new delta cycle     │
 *   │    advance T                                        │
 *   └─────────────────────────────────────────────────────┘
 *
 * schedule_now(e)  → DeltaQueue (same time, next delta)
 * schedule_at(e,t) → TimeWheel  (future time)
 * advance_delta()  → flip DeltaQueue ping-pong buffers
 * advance_time()   → move TimeWheel pointer to next non-empty bucket
 *
 * namespace celeris
 */

#include "TimeWheel.hpp"
#include "DeltaQueue.hpp"
#include <optional>

namespace celeris {

class EventScheduler {
public:
    explicit EventScheduler(SimContext& ctx) : ctx_(ctx) {}

    // -----------------------------------------------------------------------
    // schedule_now — event at the current time, next delta.
    // This is the hot path for combinational logic: a gate evaluates, its
    // output changes, and the output change enters the delta queue for the
    // next delta cycle.
    // -----------------------------------------------------------------------
    void schedule_now(Event e)
    {
        e.when.time  = ctx_.current_time.load(std::memory_order_relaxed);
        e.when.delta = ctx_.current_delta.load(std::memory_order_relaxed) + 1;
        e.event_id   = ctx_.next_event_id();
        delta_queue_.push(e);
    }

    // -----------------------------------------------------------------------
    // schedule_at — event at a specific future simulation time.
    // -----------------------------------------------------------------------
    void schedule_at(Event e, uint64_t sim_time)
    {
        e.when.time  = sim_time;
        e.when.delta = 0;
        e.event_id   = ctx_.next_event_id();
        time_wheel_.schedule(e);
    }

    // -----------------------------------------------------------------------
    // schedule_nba — non-blocking assignment: deferred to NBA region.
    // NBA events are delta events of type EventType::NBA.
    // -----------------------------------------------------------------------
    void schedule_nba(Event e)
    {
        e.type       = EventType::NBA;
        e.when.time  = ctx_.current_time.load(std::memory_order_relaxed);
        e.when.delta = ctx_.current_delta.load(std::memory_order_relaxed) + 1;
        e.event_id   = ctx_.next_event_id();
        delta_queue_.push(e);
    }

    // -----------------------------------------------------------------------
    // drain_delta — pull all events from the active delta buffer.
    // Called at the start of each delta cycle by workers.
    // -----------------------------------------------------------------------
    [[nodiscard]] std::vector<Event> drain_delta()
    {
        return delta_queue_.drain();
    }

    // -----------------------------------------------------------------------
    // advance_delta — flip the DeltaQueue buffers.
    // Called by the DeltaBarrier completion function (once per delta, on the
    // last thread to arrive at the barrier).
    // Returns true if more delta events exist (another delta cycle needed).
    // -----------------------------------------------------------------------
    bool advance_delta()
    {
        bool more = delta_queue_.flip_delta();
        if (more) {
            ctx_.current_delta.fetch_add(1, std::memory_order_release);
        }
        return more;
    }

    // -----------------------------------------------------------------------
    // advance_time — move to the next non-empty time step.
    // Returns true if more timed events remain.
    // -----------------------------------------------------------------------
    bool advance_time()
    {
        // First migrate timed events at current_time into delta queue
        auto timed_events = time_wheel_.drain_current_bucket();

        for (auto& e : timed_events) {
            e.when.delta = 0;
            delta_queue_.push(e);
        }

        if (!timed_events.empty()) return true;

        // No events at current time: advance the wheel
        uint64_t next = time_wheel_.advance_time();
        if (next == UINT64_MAX) return false;

        ctx_.current_time.store(next, std::memory_order_release);
        ctx_.current_delta.store(0, std::memory_order_release);

        // Pull those events into delta queue
        auto next_events = time_wheel_.drain_current_bucket();
        for (auto& e : next_events) {
            e.when.delta = 0;
            delta_queue_.push(e);
        }
        return !next_events.empty() || delta_queue_.has_pending();
    }

    // -----------------------------------------------------------------------
    // has_any_events — fast-path check: is there anything left to simulate?
    // -----------------------------------------------------------------------
    [[nodiscard]] bool has_any_events() const noexcept
    {
        return delta_queue_.has_pending() || time_wheel_.has_events();
    }

    [[nodiscard]] bool has_delta_events() const noexcept
    {
        return delta_queue_.has_pending();
    }

    [[nodiscard]] TimeWheel&   time_wheel()  noexcept { return time_wheel_; }
    [[nodiscard]] DeltaQueue&  delta_queue() noexcept { return delta_queue_; }

    void reset()
    {
        time_wheel_.reset();
        delta_queue_.reset();
    }

private:
    TimeWheel      time_wheel_;
    DeltaQueue     delta_queue_;
    SimContext&    ctx_;
};

} // namespace celeris
