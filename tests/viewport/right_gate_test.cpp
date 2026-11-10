#include <doctest/doctest.h>

#include "viewport/right_gate.hpp"

using apex::viewport::RightGate;
using Outcome = RightGate::Outcome;
using State = RightGate::State;

TEST_CASE("right gate: a release without movement or keys is a tap") {
    RightGate gate;
    CHECK(gate.state() == State::Idle);
    gate.press(100, 100);
    CHECK(gate.pending());
    CHECK_FALSE(gate.move(102, 101)); // Hand jitter stays under the slop.
    CHECK(gate.pending());
    CHECK(gate.release() == Outcome::Tap);
    CHECK(gate.state() == State::Idle);
}

TEST_CASE("right gate: movement past the slop commits to flying") {
    RightGate gate(4.0);
    gate.press(50, 50);
    CHECK_FALSE(gate.move(53, 50));       // 3 px
    CHECK(gate.move(55, 50));             // 5 px from the press, not from the last move
    CHECK(gate.flying());
    CHECK_FALSE(gate.move(200, 200));     // Only the committing move reports true.
    CHECK(gate.release() == Outcome::EndFly);
    CHECK(gate.state() == State::Idle);
}

TEST_CASE("right gate: slop is measured from the press so slow drift still commits") {
    RightGate gate(4.0);
    gate.press(0, 0);
    CHECK_FALSE(gate.move(1, 0));
    CHECK_FALSE(gate.move(2, 0));
    CHECK_FALSE(gate.move(3, 0));
    CHECK(gate.move(5, 0));
}

TEST_CASE("right gate: a fly key commits a pending press") {
    RightGate gate;
    gate.press(10, 10);
    CHECK(gate.key());
    CHECK(gate.flying());
    CHECK_FALSE(gate.key()); // Already flying.
    CHECK(gate.release() == Outcome::EndFly);
}

TEST_CASE("right gate: idle gate ignores input and a cancelled press is not a tap") {
    RightGate gate;
    CHECK_FALSE(gate.move(500, 500));
    CHECK_FALSE(gate.key());
    CHECK(gate.release() == Outcome::None);
    gate.press(1, 1);
    gate.cancel();
    CHECK_FALSE(gate.active());
    CHECK(gate.release() == Outcome::None);
}

TEST_CASE("right gate: remembers where the press landed") {
    RightGate gate;
    gate.press(12.5, 40.0);
    CHECK(gate.originX() == doctest::Approx(12.5));
    CHECK(gate.originY() == doctest::Approx(40.0));
}
