#include "geometry/face_uv.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace apex::geometry {
namespace {
bool finite(DVec3 p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
void validate(const FaceUv& uv, int w = 1, int h = 1) {
    Face f;
    f.uv = uv;
    (void)uvAt(f, DVec3{0}, w, h);
}
FaceUv world(DVec3 n) {
    Face f;
    f.plane = {n, 0};
    return generateUv(f, 1, 1);
}
// Shifts are saved with six significant digits (Hammer's format), so one thousands of texels out
// loses whole texels. Whole textures change nothing on screen: keep computed shifts within one.
void wrapShifts(FaceUv& uv, int w, int h) {
    uv.shiftU = std::fmod(uv.shiftU, w);
    uv.shiftV = std::fmod(uv.shiftV, h);
}
DVec3 projected(DVec3 p, DVec3 n) { return p - n * glm::dot(p, n); }
DVec3 rotated(DVec3 p, DVec3 n, double radians) {
    return p * std::cos(radians) + glm::cross(n, p) * std::sin(radians) +
           n * glm::dot(n, p) * (1 - std::cos(radians));
}
}
FaceUv justify(FaceUv uv, std::span<const DVec3> points, Justify mode, int w, int h) {
    validate(uv, w, h);
    if (points.empty()) throw std::invalid_argument("UV bounds require points");
    DVec2 lo{INFINITY}, hi{-INFINITY};
    for (auto p : points) {
        Face f; f.uv = uv;
        const auto texel = uvAt(f, p, w, h) * DVec2{w, h};
        lo = glm::min(lo, texel); hi = glm::max(hi, texel);
    }
    if (mode == Justify::Fit) {
        const auto span = hi - lo;
        if (!(span.x > 0 && span.y > 0)) throw std::invalid_argument("UV fit requires nonzero bounds");
        uv.scaleU *= span.x / w;
        uv.scaleV *= span.y / h;
        uv.shiftU = 0; uv.shiftV = 0;
        uv = justify(uv, points, Justify::Left, w, h);
        return justify(uv, points, Justify::Top, w, h);
    }
    if (mode == Justify::Left) uv.shiftU -= lo.x;
    if (mode == Justify::Right) uv.shiftU += w - hi.x;
    if (mode == Justify::Top) uv.shiftV -= lo.y;
    if (mode == Justify::Bottom) uv.shiftV += h - hi.y;
    if (mode == Justify::Center) {
        uv.shiftU += (w - lo.x - hi.x) / 2;
        uv.shiftV += (h - lo.y - hi.y) / 2;
    }
    wrapShifts(uv, w, h);
    validate(uv, w, h);
    return uv;
}
FaceUv justify2(FaceUv uv, std::span<const DVec3> points, HorizontalAlignment horizontal,
                VerticalAlignment vertical, int w, int h) {
    validate(uv, w, h);
    if (points.empty()) throw std::invalid_argument("UV bounds require points");
    DVec2 lo{INFINITY}, hi{-INFINITY};
    for (const auto p : points) {
        Face f; f.uv = uv;
        const auto texel = uvAt(f, p, w, h) * DVec2{w, h};
        lo = glm::min(lo, texel); hi = glm::max(hi, texel);
    }
    if (horizontal == HorizontalAlignment::Left) uv.shiftU -= lo.x;
    if (horizontal == HorizontalAlignment::Right) uv.shiftU += w - hi.x;
    if (horizontal == HorizontalAlignment::Center) uv.shiftU += (w - lo.x - hi.x) / 2;
    if (vertical == VerticalAlignment::Top) uv.shiftV -= lo.y;
    if (vertical == VerticalAlignment::Bottom) uv.shiftV += h - hi.y;
    if (vertical == VerticalAlignment::Center) uv.shiftV += (h - lo.y - hi.y) / 2;
    wrapShifts(uv, w, h);
    validate(uv, w, h);
    return uv;
}
Alignment alignmentOf(const FaceUv& uv, std::span<const DVec3> points, int w, int h) {
    validate(uv, w, h);
    if (points.empty()) return {};
    DVec2 lo{INFINITY}, hi{-INFINITY};
    for (const auto p : points) {
        Face f; f.uv = uv;
        const auto texel = uvAt(f, p, w, h) * DVec2{w, h};
        lo = glm::min(lo, texel); hi = glm::max(hi, texel);
    }
    const auto edge = [](double value, double period) {
        const auto wrapped = std::fmod(value, period);
        return std::min(std::abs(wrapped), std::abs(std::abs(wrapped) - period));
    };
    Alignment result;
    if (edge(lo.x, w) <= 1e-3) result.horizontal = HorizontalAlignment::Left;
    else if (edge((lo.x + hi.x) / 2 - w / 2.0, w) <= 1e-3)
        result.horizontal = HorizontalAlignment::Center;
    else if (edge(hi.x - w, w) <= 1e-3) result.horizontal = HorizontalAlignment::Right;
    if (edge(lo.y, h) <= 1e-3) result.vertical = VerticalAlignment::Top;
    else if (edge((lo.y + hi.y) / 2 - h / 2.0, h) <= 1e-3)
        result.vertical = VerticalAlignment::Center;
    else if (edge(hi.y - h, h) <= 1e-3) result.vertical = VerticalAlignment::Bottom;
    return result;
}
FaceUv translateUv(FaceUv uv, double du, double dv, int w, int h) {
    validate(uv, w, h);
    if (!std::isfinite(du) || !std::isfinite(dv)) throw std::invalid_argument("UV translation must be finite");
    // Only the moved axis wraps, so a one-axis nudge leaves the other shift as written.
    if (du != 0) uv.shiftU = std::fmod(uv.shiftU + du, w);
    if (dv != 0) uv.shiftV = std::fmod(uv.shiftV + dv, h);
    return uv;
}
FaceUv alignWorld(FaceUv uv, DVec3 normal) {
    validate(uv);
    const auto axes = world(normal);
    uv.uaxis = axes.uaxis; uv.vaxis = axes.vaxis;
    return uv;
}
FaceUv alignFace(FaceUv uv, DVec3 normal) {
    uv = alignWorld(uv, normal);
    auto u = projected(uv.uaxis, normal);
    if (glm::length(u) < 1e-12) u = projected(uv.vaxis, normal);
    u = glm::normalize(u);
    auto v = glm::cross(normal, u);
    if (glm::dot(v, uv.vaxis) < 0) v = -v;
    uv.uaxis = u; uv.vaxis = v;
    return uv;
}
std::optional<FaceUv> projectUv(const FaceUv& source, DVec3 targetNormal) {
    validate(source);
    (void)world(targetNormal);
    constexpr double kMinimumProjection = 0.03489949670250097; // sin(2 degrees)
    for (const auto axis : {source.uaxis, source.vaxis}) {
        const auto length = glm::length(axis);
        if (!(length > 0) || glm::length(projected(axis, targetNormal)) / length < kMinimumProjection)
            return std::nullopt;
    }
    // Each axis can survive projection while the pair collapses onto one line.
    const auto joint = glm::dot(glm::cross(projected(source.uaxis, targetNormal), projected(source.vaxis, targetNormal)),
                                glm::normalize(targetNormal));
    if (std::abs(joint) / (glm::length(source.uaxis) * glm::length(source.vaxis)) < kMinimumProjection) return std::nullopt;
    return source;
}
FaceUv matchUv(const FaceUv& source, DVec3 sourceNormal, DVec3 targetNormal) {
    validate(source);
    const auto rotation = deriveRotation(source, sourceNormal);
    auto result = alignFace(source, targetNormal);
    return rotateUv(result, targetNormal, rotation);
}
FaceUv rotateUv(FaceUv uv, DVec3 normal, double degrees) {
    validate(uv); (void)world(normal);
    if (!std::isfinite(degrees)) throw std::invalid_argument("UV rotation must be finite");
    const auto angle = glm::radians(std::remainder(degrees, 360.0));
    uv.uaxis = rotated(uv.uaxis, normal, angle);
    uv.vaxis = rotated(uv.vaxis, normal, angle);
    return uv;
}
FaceUv rotateUvAbout(FaceUv uv, DVec3 normal, DVec3 pivot, double degrees) {
    validate(uv); (void)world(normal);
    if (!finite(pivot)) throw std::invalid_argument("UV pivot must be finite");
    const auto before = uv;
    uv = rotateUv(uv, normal, degrees);
    uv.shiftU += glm::dot(pivot, before.uaxis - uv.uaxis) / uv.scaleU;
    uv.shiftV += glm::dot(pivot, before.vaxis - uv.vaxis) / uv.scaleV;
    return uv;
}
FaceUv scaleUvAbout(FaceUv uv, DVec3 pivot, double factorU, double factorV) {
    validate(uv);
    if (!finite(pivot) || !std::isfinite(factorU) || !std::isfinite(factorV) || factorU <= 0 || factorV <= 0)
        throw std::invalid_argument("UV scale factors must be positive and finite");
    const auto before = uv;
    uv.scaleU *= factorU; uv.scaleV *= factorV;
    uv.shiftU += glm::dot(pivot, before.uaxis) * (1 / before.scaleU - 1 / uv.scaleU);
    uv.shiftV += glm::dot(pivot, before.vaxis) * (1 / before.scaleV - 1 / uv.scaleV);
    validate(uv);
    return uv;
}
FacePlaneFrame planeFrame(DVec3 normal, std::span<const DVec3> points) {
    (void)world(normal);
    if (points.empty()) throw std::invalid_argument("Face plane frame requires points");
    DVec3 origin{};
    for (const auto p : points) {
        if (!finite(p)) throw std::invalid_argument("Face plane frame requires finite points");
        origin += p;
    }
    origin /= static_cast<double>(points.size());
    const auto aligned = alignFace(world(normal), normal);
    // y = x × n makes x × y = -n: drawn x right, y down, the face is seen from outside.
    return {origin, aligned.uaxis, glm::cross(aligned.uaxis, glm::normalize(normal))};
}
double deriveRotation(const FaceUv& uv, DVec3 normal) {
    validate(uv);
    const auto base = projected(world(normal).uaxis, normal);
    const auto current = projected(uv.uaxis, normal);
    if (glm::length(current) < 1e-12) throw std::invalid_argument("UV U axis has no in-plane direction");
    auto degrees = glm::degrees(std::atan2(glm::dot(normal, glm::cross(base, current)), glm::dot(base, current)));
    degrees = std::fmod(degrees + 360.0, 360.0);
    degrees = std::round(degrees * 1e9) / 1e9;
    return degrees >= 360 ? 0 : degrees;
}
FaceUv wrapUv(const FaceUv& from, DVec3 fromNormal, DVec3 toNormal,
              DVec3 edgeA, DVec3 edgeB, int w, int h) {
    validate(from, w, h); (void)world(fromNormal); (void)world(toNormal);
    if (!finite(edgeA) || !finite(edgeB) || glm::length(edgeB - edgeA) < 1e-12)
        throw std::invalid_argument("UV wrap requires a finite shared edge");
    const auto edge = glm::normalize(edgeB - edgeA);
    if (std::abs(glm::dot(edge, fromNormal)) > 1e-6 || std::abs(glm::dot(edge, toNormal)) > 1e-6)
        throw std::invalid_argument("UV wrap edge must lie in both planes");
    const auto angle = std::atan2(glm::dot(edge, glm::cross(fromNormal, toNormal)), glm::dot(fromNormal, toNormal));
    auto result = from;
    if (std::abs(angle) > 1e-12) {
        result.uaxis = rotated(from.uaxis, edge, angle);
        result.vaxis = rotated(from.vaxis, edge, angle);
        result.shiftU += glm::dot(edgeA, from.uaxis - result.uaxis) / from.scaleU;
        result.shiftV += glm::dot(edgeA, from.vaxis - result.vaxis) / from.scaleV;
        wrapShifts(result, w, h);
    }
    validate(result, w, h);
    return result;
}
} // namespace apex::geometry
