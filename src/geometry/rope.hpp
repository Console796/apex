#pragma once

// Plan 0.8 D14: a rope's resting shape by the engine's own rule. Source SDK 2013's rope code
// (rope.cpp, c_rope.cpp, rope_physics.cpp, simple_physics.cpp) sets it up as below, and this
// reproduces it without wind or world collision:
// - The physics has `nodes` points: Type 0 gives 10, 1 gives 4, anything else 2, and an entity
//   without Type has 5. They start on the straight line between the ends.
// - The rest length between neighbours is (int(distance) + Slack - 100) / (nodes - 1), at least
//   zero. The -100 is ROPESLACK_FUDGEFACTOR, so a Slack of 0 hangs tighter than taut.
// - Verlet steps of 1/50 s with gravity -1500 and 0.98 of the last step's motion kept, then three
//   passes that only pull neighbours closer than the rest length together, re-locking the ends
//   after each pass. Dangling unlocks the far end.
// - The engine hangs the rope for 5 s at spawn and keeps simulating after. A still rope is that
//   simulation's rest state, so this runs the 5 s and then on until nothing moves.
// - It draws a Catmull-Rom curve through the nodes, with Subdiv points between each pair
//   (255 means the rope_subdiv default, 2; at most 7).

#include "core/math.hpp"

#include <vector>

namespace apex::geometry {

struct RopeSettings {
    int slack = 0;
    int nodes = 5;
    int subdiv = 2;
    bool dangling = false;
    bool operator==(const RopeSettings&) const = default;
};

// The physics nodes at rest, from `start` to `end`.
std::vector<DVec3> ropeNodes(DVec3 start, DVec3 end, const RopeSettings& settings);
// The drawn curve through the nodes: each node, then `subdiv` Catmull-Rom points to the next.
std::vector<DVec3> ropeCurve(const std::vector<DVec3>& nodes, int subdiv);
// Both: the curve of the rope at rest.
std::vector<DVec3> ropeShape(DVec3 start, DVec3 end, const RopeSettings& settings);

} // namespace apex::geometry
