#pragma once

#include <cmath>

namespace apex::viewport {

// Decides whether a right press is a click for the active tool or the start of an FPV fly. The
// press is held back; moving past `slop` pixels or pressing a fly key commits it to flying (the
// tool never sees that press), and a release before either is a tap the tool receives as a click.
// Qt-free so the split is unit tested without a window.
class RightGate {
public:
    enum class State { Idle, Pending, Flying };
    enum class Outcome { None, Tap, EndFly };

    explicit RightGate(double slopPixels = 4.0) : slop_(slopPixels) {}

    State state() const { return state_; }
    bool pending() const { return state_ == State::Pending; }
    bool flying() const { return state_ == State::Flying; }
    bool active() const { return state_ != State::Idle; }
    double originX() const { return ox_; }
    double originY() const { return oy_; }

    void press(double x, double y) {
        state_ = State::Pending;
        ox_ = x;
        oy_ = y;
    }
    // True when this move commits a pending press to flying.
    bool move(double x, double y) {
        if (state_ != State::Pending) return false;
        if (std::hypot(x - ox_, y - oy_) <= slop_) return false;
        state_ = State::Flying;
        return true;
    }
    // A fly key (WASD, Q, E, arrows). True when it commits a pending press to flying.
    bool key() {
        if (state_ != State::Pending) return false;
        state_ = State::Flying;
        return true;
    }
    Outcome release() {
        const auto was = state_;
        state_ = State::Idle;
        return was == State::Pending ? Outcome::Tap : was == State::Flying ? Outcome::EndFly : Outcome::None;
    }
    void cancel() { state_ = State::Idle; }

private:
    State state_ = State::Idle;
    double slop_;
    double ox_ = 0.0, oy_ = 0.0;
};

} // namespace apex::viewport
