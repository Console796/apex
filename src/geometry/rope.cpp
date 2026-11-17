#include "geometry/rope.hpp"

#include <algorithm>
#include <cmath>

namespace apex::geometry {
namespace {

constexpr int kMaxNodes = 10;     // ROPE_MAX_SEGMENTS
constexpr int kMaxSubdiv = 7;     // MAX_ROPE_SUBDIVS - 1
constexpr int kDefaultSubdiv = 2; // rope_subdiv
constexpr double kSlackFudge = -100.0;
constexpr double kStep = 1.0 / 50.0;
constexpr double kGravity = -1500.0;
constexpr double kKeep = 0.98;
constexpr int kPasses = 3;
constexpr int kHangSteps = 250;  // 5 s
constexpr int kMaxSteps = 15000; // 5 min: far past any rope's settling time
// Still: no node moves this far in a step.
constexpr double kRest = 1e-6;

} // namespace

std::vector<DVec3> ropeNodes(DVec3 start, DVec3 end, const RopeSettings& settings) {
    const int count = std::clamp(settings.nodes, 2, kMaxNodes);
    // The engine stores the length as an int, truncated.
    const double length = std::trunc(glm::length(end - start));
    const double rest = std::max((length + settings.slack + kSlackFudge) / (count - 1), 0.0);
    std::vector<DVec3> pos(count), prev(count);
    for (int i = 0; i < count; ++i) pos[i] = prev[i] = start + (end - start) * (double(i) / (count - 1));
    const double fall = kGravity * kStep * kStep * 0.5;
    for (int step = 0; step < kMaxSteps; ++step) {
        double moved = 0.0;
        for (int i = 0; i < count; ++i) {
            const DVec3 before = pos[i];
            pos[i] += (pos[i] - prev[i]) * kKeep + DVec3{0, 0, fall};
            prev[i] = before;
        }
        for (int pass = 0; pass < kPasses; ++pass) {
            for (int i = 0; i + 1 < count; ++i) {
                DVec3 to = pos[i] - pos[i + 1];
                const double dist = glm::length(to);
                if (dist > rest && dist > 0.0) {
                    to *= 1.0 - rest / dist;
                    pos[i] -= to * 0.5;
                    pos[i + 1] += to * 0.5;
                }
            }
            pos.front() = start;
            if (!settings.dangling) pos.back() = end;
        }
        for (int i = 0; i < count; ++i) moved = std::max(moved, glm::length(pos[i] - prev[i]));
        if (step >= kHangSteps && moved < kRest) break;
    }
    return pos;
}

std::vector<DVec3> ropeCurve(const std::vector<DVec3>& nodes, int subdiv) {
    if (subdiv == 255) subdiv = kDefaultSubdiv;
    subdiv = std::clamp(subdiv, 0, kMaxSubdiv);
    std::vector<DVec3> curve;
    if (nodes.empty()) return curve;
    const int last = int(nodes.size()) - 1;
    curve.reserve(nodes.size() + std::size_t(last) * subdiv);
    // As c_rope.cpp's BuildRope: the first span's previous point is the first node itself, and
    // the last spans repeat the last node.
    int previous = 0;
    for (int i = 0; i <= last; ++i) {
        curve.push_back(nodes[i]);
        if (i == last) break;
        const int next = i + 1, after = std::min(i + 2, last);
        const DVec3 p1 = nodes[previous], p2 = nodes[i], p3 = nodes[next], p4 = nodes[after];
        const DVec3 t3 = 0.5 * (-p1 + 3.0 * p2 - 3.0 * p3 + p4);
        const DVec3 t2 = 0.5 * (2.0 * p1 - 5.0 * p2 + 4.0 * p3 - p4);
        const DVec3 t1 = 0.5 * (-p1 + p3);
        for (int s = 0; s < subdiv; ++s) {
            const double t = double(s + 1) / (subdiv + 1);
            curve.push_back(p2 + t * t1 + t * t * t2 + t * t * t * t3);
        }
        previous = i;
    }
    return curve;
}

std::vector<DVec3> ropeShape(DVec3 start, DVec3 end, const RopeSettings& settings) {
    return ropeCurve(ropeNodes(start, end, settings), settings.subdiv);
}

RopeTube ropeTube(std::span<const DVec3> curve, double width) {
    RopeTube tube;
    if (!std::isfinite(width) || width <= 0.0) return tube;
    std::vector<DVec3> points;
    for (const auto p : curve) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return {};
        if (points.empty() || glm::length(p - points.back()) > 1e-9) points.push_back(p);
    }
    if (points.size() < 2) return tube;
    constexpr int sides = 8, ring = sides + 1;
    DVec3 normal{0.0};
    double distance = 0.0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto outgoing = i + 1 < points.size() ? glm::normalize(points[i + 1] - points[i])
                                                   : glm::normalize(points[i] - points[i - 1]);
        DVec3 tangent = outgoing;
        if (i > 0 && i + 1 < points.size()) {
            const auto sum = outgoing + glm::normalize(points[i] - points[i - 1]);
            if (glm::length(sum) > 1e-9) tangent = glm::normalize(sum);
        }
        normal -= tangent * glm::dot(normal, tangent);
        if (glm::length(normal) < 1e-9) {
            const auto axis = std::abs(tangent.z) < 0.9 ? DVec3{0, 0, 1} : DVec3{0, 1, 0};
            normal = glm::cross(tangent, axis);
        }
        normal = glm::normalize(normal);
        const auto binormal = glm::cross(tangent, normal);
        if (i) distance += glm::length(points[i] - points[i - 1]);
        for (int j = 0; j <= sides; ++j) {
            const double angle = glm::radians(360.0 * (j % sides) / sides);
            const auto radial = normal * std::cos(angle) + binormal * std::sin(angle);
            tube.vertices.push_back({points[i] + radial * (width * 0.5), radial,
                                     {distance / width, double(j) / sides}});
        }
        if (!i) continue;
        const auto base = static_cast<std::uint32_t>((i - 1) * ring);
        for (int j = 0; j < sides; ++j) {
            const auto a = base + static_cast<std::uint32_t>(j), b = a + ring;
            tube.indices.insert(tube.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    }
    return tube;
}

} // namespace apex::geometry
