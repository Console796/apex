#include "editor/gizmo.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace apex::editor {
namespace {
constexpr double eps = 1e-10;
bool finite(DVec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
int axisIndex(GizmoHandle h) {
    return h == GizmoHandle::X ? 0 : h == GizmoHandle::Y ? 1 : h == GizmoHandle::Z ? 2 : -1;
}
DVec3 mask(GizmoHandle h) {
    switch (h) {
    case GizmoHandle::X: return {1, 0, 0};
    case GizmoHandle::Y: return {0, 1, 0};
    case GizmoHandle::Z: return {0, 0, 1};
    case GizmoHandle::XY: return {1, 1, 0};
    case GizmoHandle::YZ: return {0, 1, 1};
    case GizmoHandle::ZX: return {1, 0, 1};
    default: return DVec3{1.0};
    }
}
DVec3 planeNormal(GizmoHandle h, const DMat3& f, DVec3 screen) {
    if (h == GizmoHandle::XY) return f[2];
    if (h == GizmoHandle::YZ) return f[0];
    if (h == GizmoHandle::ZX) return f[1];
    const int index = axisIndex(h);
    return index >= 0 ? f[index] : screen;
}
DVec3 screenNormal(const GizmoContext& c, const GizmoView& v) {
    const auto d = v.cameraPosition - c.pivot;
    return glm::length(d) > eps ? glm::normalize(d) : DVec3{0, 0, 1};
}
std::pair<DVec3, DVec3> screenAxes(DVec3 n, DVec3 up) {
    if (glm::length(glm::cross(up, n)) < eps) up = std::abs(n.z) < 0.9 ? DVec3{0, 0, 1} : DVec3{0, 1, 0};
    const auto u = glm::normalize(glm::cross(up, n));
    return {u, glm::cross(n, u)};
}
double size(const GizmoView& v) {
    if (!std::isfinite(v.pixelsPerWorldUnit) || v.pixelsPerWorldUnit <= 0 ||
        !std::isfinite(v.sizePixels) || v.sizePixels <= 0) throw std::invalid_argument("invalid gizmo size");
    return v.sizePixels / v.pixelsPerWorldUnit;
}
std::optional<DVec3> intersectPlane(Ray ray, DVec3 centre, DVec3 normal) {
    const double divisor = glm::dot(ray.dir, normal);
    if (std::abs(divisor) <= eps * glm::length(ray.dir)) return {};
    const double t = glm::dot(centre - ray.origin, normal) / divisor;
    if (t < 0 || !std::isfinite(t)) return {};
    return ray.at(t);
}
// Closest point on an infinite axis to a forward ray. Parallel drags are undefined.
std::optional<DVec3> axisPoint(Ray ray, DVec3 centre, DVec3 axis) {
    const auto q = ray.origin - centre;
    const double a = glm::dot(ray.dir, ray.dir), b = glm::dot(ray.dir, axis);
    const double det = a - b * b;
    if (det <= eps * a) return {};
    const double t = (b * glm::dot(q, axis) - glm::dot(q, ray.dir)) / det;
    if (t < 0) return {};
    return centre + axis * glm::dot(ray.at(t) - centre, axis);
}
struct Distance { double t, distance; };
Distance segmentDistance(Ray ray, DVec3 a, DVec3 b) {
    const auto edge = b - a, q = ray.origin - a;
    const double rr = glm::dot(ray.dir, ray.dir), ee = glm::dot(edge, edge);
    const double re = glm::dot(ray.dir, edge), rq = glm::dot(ray.dir, q), eq = glm::dot(edge, q);
    double s = ee > eps ? std::clamp(eq / ee, 0.0, 1.0) : 0.0;
    const double det = rr * ee - re * re;
    if (det > eps * rr * ee) s = std::clamp((rr * eq - re * rq) / det, 0.0, 1.0);
    double t = std::max(0.0, (re * s - rq) / rr);
    if (t == 0 && ee > eps) s = std::clamp(eq / ee, 0.0, 1.0);
    return {t, glm::length(ray.at(t) - (a + edge * s))};
}
// Isolate all stationary roots on a finite interval using derivative roots; the
// polynomial is degree <= 4, so this remains constant work for each analytic ring.
using Polynomial = std::vector<double>; // Ascending coefficients.
double polynomial(const Polynomial& p, double x) {
    double result = 0;
    for (auto it = p.rbegin(); it != p.rend(); ++it) result = result * x + *it;
    return result;
}
std::vector<double> roots(Polynomial p, double lo, double hi) {
    while (p.size() > 1 && std::abs(p.back()) < 1e-14) p.pop_back();
    if (p.size() < 2) return {};
    if (p.size() == 2) {
        const double r = -p[0] / p[1];
        return r >= lo && r <= hi ? std::vector<double>{r} : std::vector<double>{};
    }
    Polynomial derivative;
    for (std::size_t i = 1; i < p.size(); ++i) derivative.push_back(p[i] * static_cast<double>(i));
    auto cuts = roots(derivative, lo, hi);
    cuts.insert(cuts.begin(), lo);
    cuts.push_back(hi);
    std::vector<double> result;
    for (const auto x : cuts) if (std::abs(polynomial(p, x)) < 1e-9) result.push_back(x);
    for (std::size_t i = 1; i < cuts.size(); ++i) {
        double a = cuts[i - 1], b = cuts[i];
        double fa = polynomial(p, a);
        if ((fa < 0) == (polynomial(p, b) < 0)) continue;
        for (int j = 0; j < 64; ++j) {
            const double mid = (a + b) * 0.5, fm = polynomial(p, mid);
            if ((fa < 0) == (fm < 0)) { a = mid; fa = fm; } else b = mid;
        }
        result.push_back((a + b) * 0.5);
    }
    return result;
}
Distance ringDistance(Ray ray, const GizmoPrimitive& p) {
    // Shift the ray parameter near the circle before forming the quartic to avoid
    // cancellation for distant cameras. Normalize world units by the ring radius.
    const double rayLength = glm::length(ray.dir);
    ray.dir /= rayLength;
    const double offset = std::max(0.0, glm::dot(p.centre - ray.origin, ray.dir) - 2 * p.radius);
    const auto q = (ray.at(offset) - p.centre) / p.radius;
    const auto d = ray.dir;
    const auto planarQ = q - p.normal * glm::dot(q, p.normal);
    const auto planarD = d - p.normal * glm::dot(d, p.normal);
    const double a = glm::dot(d, d), b = glm::dot(q, d);
    const double c = glm::dot(planarD, planarD), e = glm::dot(planarQ, planarD), f = glm::dot(planarQ, planarQ);
    Polynomial poly{b*b*f-e*e, 2*b*b*e+2*a*b*f-2*c*e,
                    b*b*c+4*a*b*e+a*a*f-c*c, 2*a*b*c+2*a*a*e, a*a*c};
    const double upper = std::max(0.0, -b + 2.0);
    auto candidates = roots(poly, 0, upper);
    candidates.push_back(0);
    candidates.push_back(upper);
    // Include the cusp when the ray crosses the circle axis.
    if (c > eps) candidates.push_back(std::clamp(-e / c, 0.0, upper));
    Distance best{0, std::numeric_limits<double>::infinity()};
    for (const double t : candidates) {
        const auto point = q + t*d;
        const double h = glm::dot(point, p.normal);
        const double radial = glm::length(point - p.normal*h);
        const double distance = std::hypot(h, radial - 1.0) * p.radius;
        const double parameter = (offset + t * p.radius) / rayLength;
        if (distance < best.distance - eps || (std::abs(distance - best.distance) <= eps && parameter < best.t))
            best = {parameter, distance};
    }
    return best;
}
AABB frameBounds(const AABB& bounds, DMat3 frame) {
    AABB result;
    for (int i = 0; i < 8; ++i) result.expand(glm::transpose(frame) * DVec3{
        i & 1 ? bounds.max.x : bounds.min.x, i & 2 ? bounds.max.y : bounds.min.y,
        i & 4 ? bounds.max.z : bounds.min.z});
    return result;
}
std::string vectorText(std::string_view prefix, DVec3 v) {
    std::ostringstream s;
    s << std::setprecision(8) << prefix << v.x << ", " << v.y << ", " << v.z;
    return s.str();
}
} // namespace

std::vector<GizmoPrimitive> gizmoDescription(GizmoMode mode, const GizmoContext& c, const GizmoView& v) {
    const double length = size(v);
    const auto n = screenNormal(c, v);
    const auto [u, w] = screenAxes(n, v.cameraUp);
    std::vector<GizmoPrimitive> result;
    for (int i = 0; i < 3; ++i) {
        GizmoPrimitive p{};
        p.handle = static_cast<GizmoHandle>(i);
        p.centre = c.pivot;
        if (mode == GizmoMode::Rotate) {
            p.shape = GizmoShape::Ring; p.normal = c.frame[i]; p.radius = length;
        } else {
            p.shape = GizmoShape::Segment;
            p.points[0] = c.pivot + c.frame[i] * (length * 0.15);
            p.points[1] = c.pivot + c.frame[i] * length;
        }
        result.push_back(p);
    }
    if (mode == GizmoMode::Translate) {
        for (int i = 0; i < 3; ++i) {
            const int j = (i + 1) % 3;
            GizmoPrimitive p{};
            p.handle = i == 0 ? GizmoHandle::XY : i == 1 ? GizmoHandle::YZ : GizmoHandle::ZX;
            p.shape = GizmoShape::Quad;
            p.normal = glm::cross(c.frame[i], c.frame[j]);
            p.centre = c.pivot + (c.frame[i] + c.frame[j]) * (length * 0.325);
            p.points = {c.pivot + (c.frame[i]+c.frame[j])*(length*0.2),
                c.pivot + (c.frame[i]*0.45+c.frame[j]*0.2)*length,
                c.pivot + (c.frame[i]+c.frame[j])*(length*0.45),
                c.pivot + (c.frame[i]*0.2+c.frame[j]*0.45)*length};
            result.push_back(p);
        }
    }
    GizmoPrimitive p{};
    p.handle = mode == GizmoMode::Scale ? GizmoHandle::Uniform : GizmoHandle::Screen;
    p.centre = c.pivot; p.normal = n;
    if (mode == GizmoMode::Rotate) { p.shape = GizmoShape::Ring; p.radius = length * 1.2; }
    else {
        p.shape = GizmoShape::Quad;
        const double r = length * 0.08;
        p.points = {c.pivot-(u+w)*r, c.pivot+(u-w)*r, c.pivot+(u+w)*r, c.pivot+(-u+w)*r};
    }
    result.push_back(p);
    return result;
}

std::optional<GizmoHit> hitTestGizmo(GizmoMode mode, const GizmoContext& c, const GizmoView& v,
                                    Ray ray, double tolerancePixels) {
    if (!finite(ray.origin) || !finite(ray.dir) || glm::length(ray.dir) <= eps ||
        !std::isfinite(tolerancePixels) || tolerancePixels < 0) return {};
    std::optional<GizmoHit> best;
    for (const auto& p : gizmoDescription(mode, c, v)) {
        Distance d{0, std::numeric_limits<double>::infinity()};
        if (p.shape == GizmoShape::Segment) d = segmentDistance(ray, p.points[0], p.points[1]);
        else if (p.shape == GizmoShape::Ring) d = ringDistance(ray, p);
        else {
            if (const auto point = intersectPlane(ray, p.centre, p.normal)) {
                const auto a = p.points[1]-p.points[0], b = p.points[3]-p.points[0], q = *point-p.points[0];
                const double x = glm::dot(q,a)/glm::dot(a,a), y = glm::dot(q,b)/glm::dot(b,b);
                if (x >= 0 && x <= 1 && y >= 0 && y <= 1)
                    d = {glm::dot(*point-ray.origin,ray.dir)/glm::dot(ray.dir,ray.dir), 0};
            }
            for (int i = 0; i < 4; ++i) {
                const auto edge = segmentDistance(ray, p.points[i], p.points[(i+1)%4]);
                if (edge.distance < d.distance) d = edge;
            }
        }
        const double pixels = d.distance * v.pixelsPerWorldUnit;
        if (pixels > tolerancePixels + eps) continue;
        if (!best || d.t < best->rayParameter-eps ||
            (std::abs(d.t-best->rayParameter) <= eps && pixels < best->distancePixels))
            best = GizmoHit{p.handle, d.t, pixels};
    }
    return best;
}

std::optional<TransformReadout> evaluateTransform(GizmoMode mode, GizmoHandle handle,
    const GizmoContext& c, DVec3 grab, DVec3 raw, TransformModifiers modifiers, bool exact, DVec3 screen) {
    if (!finite(raw)) return {};
    auto snap = c.snap;
    snap.suspended = snap.suspended || modifiers.suspend || exact;
    snap.fine = snap.fine || modifiers.fine;
    TransformReadout result;
    const auto active = mask(handle);
    const auto transpose = glm::transpose(c.frame);
    try {
        if (mode == GizmoMode::Translate) {
            auto delta = c.frame * ((transpose * raw) * active);
            auto plane = c.workPlane;
            // Axis/plane constraints define a snap frame containing the active axes.
            if (handle != GizmoHandle::Screen) {
                int first = 0;
                while (active[first] == 0) ++first;
                int second = (first+1)%3;
                if (active[second] == 0 && active[(first+2)%3] != 0) second = (first+2)%3;
                plane.uAxis = c.frame[first]; plane.vAxis = c.frame[second];
                plane.normal = glm::cross(plane.uAxis, plane.vAxis);
            }
            delta = snapTranslate(delta, c.bounds, grab, plane, snap);
            // A screen-plane drag in perspective leaves the work plane, so the component along
            // the plane normal needs the grid too (an ortho drag has none and keeps its depth).
            if (handle == GizmoHandle::Screen && std::abs(glm::dot(delta, plane.normal)) > 1e-9) {
                auto across = plane;
                across.uAxis = plane.normal;
                across.vAxis = glm::cross(plane.normal, plane.uAxis);
                delta = snapTranslate(delta, c.bounds, grab, across, snap);
            }
            result.delta = c.frame * ((transpose * delta) * active);
            result.transform = Transform::translate(result.delta);
            result.text = vectorText("Move: ", result.delta);
        } else if (mode == GizmoMode::Rotate) {
            const int index = axisIndex(handle);
            const double start = index >= 0 ? c.rotationStartDeg[index] : 0.0;
            result.angleDeg = snapRotate(raw.x, start, snap);
            result.transform = Transform::rotate(planeNormal(handle, c.frame, screen), result.angleDeg, c.pivot);
            std::ostringstream s; s << std::setprecision(8) << "Rotate: " << result.angleDeg << " deg";
            result.text = s.str();
        } else {
            auto factors = DVec3{1.0} + (raw-DVec3{1.0})*active;
            factors = snapScale(factors, frameBounds(c.bounds, c.frame), transpose*c.pivot, snap);
            if (handle == GizmoHandle::Uniform) {
                const auto bounds = frameBounds(c.bounds, c.frame);
                const auto reach = glm::max(glm::abs(bounds.min-transpose*c.pivot), glm::abs(bounds.max-transpose*c.pivot));
                int index = reach.y > reach.x ? 1 : 0;
                if (reach.z > reach[index]) index = 2;
                factors = DVec3{factors[index]};
            }
            result.factors = factors;
            result.transform = Transform::scale(factors, c.frame, c.pivot);
            result.text = vectorText("Scale: ", factors);
        }
    } catch (const std::invalid_argument&) { return {}; }
    return result;
}

std::optional<DVec3> GizmoDrag::position(Ray ray) const {
    if (!finite(ray.origin) || !finite(ray.dir) || glm::length(ray.dir) < eps) return {};
    if (axisIndex(handle_) >= 0 && mode_ != GizmoMode::Rotate)
        return axisPoint(ray, context_.pivot, axis_);
    return intersectPlane(ray, context_.pivot, normal_);
}
bool GizmoDrag::begin(GizmoMode mode, GizmoHandle handle, const GizmoContext& c,
                       const GizmoView& v, Ray ray) {
    active_ = false;
    // Reject handles absent from this mode, including accidental plane scales.
    const auto description = gizmoDescription(mode, c, v);
    if (std::ranges::none_of(description, [handle](const auto& p) { return p.handle == handle; })) return false;
    mode_ = mode; handle_ = handle; context_ = c;
    const int index = axisIndex(handle);
    axis_ = index >= 0 ? c.frame[index] : DVec3{1, 0, 0};
    normal_ = planeNormal(handle, c.frame, screenNormal(c, v));
    const auto start = position(ray);
    if (!start) return false;
    start_ = *start;
    if (mode != GizmoMode::Translate && glm::length(start_-c.pivot) < eps) {
        // Uniform centre remains draggable when pressed exactly at the pivot.
        const auto [u, unused] = screenAxes(normal_, v.cameraUp);
        start_ += u * size(v);
    }
    previousAngle_ = accumulatedAngle_ = 0;
    value_ = {};
    active_ = true;
    return true;
}
const TransformReadout& GizmoDrag::update(Ray ray, TransformModifiers modifiers) {
    if (!active_) return value_;
    const auto point = position(ray);
    if (!point) return value_;
    DVec3 raw = *point-start_;
    if (mode_ == GizmoMode::Rotate) {
        const auto a = start_-context_.pivot, b = *point-context_.pivot;
        if (glm::length(b) < eps) return value_;
        const double angle = glm::degrees(std::atan2(glm::dot(normal_,glm::cross(a,b)),glm::dot(a,b)));
        accumulatedAngle_ += std::remainder(angle-previousAngle_, 360.0);
        previousAngle_ = angle;
        raw = {accumulatedAngle_, 0, 0};
    } else if (mode_ == GizmoMode::Scale) {
        const auto a = start_-context_.pivot;
        const double factor = 1.0 + glm::dot(*point-start_, a)/glm::dot(a,a);
        raw = DVec3{factor};
    }
    if (auto value = evaluateTransform(mode_,handle_,context_,start_,raw,modifiers,false,normal_)) value_ = std::move(*value);
    return value_;
}
} // namespace apex::editor
