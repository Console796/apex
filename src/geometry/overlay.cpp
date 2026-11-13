#include "geometry/overlay.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

namespace apex::geometry {
namespace {

bool finite(DVec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool finite(DVec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }
double cross2(DVec2 a, DVec2 b) { return a.x * b.y - a.y * b.x; }

Error invalid(std::string message) { return Error{ErrorCode::InvalidArgument, std::move(message)}; }

Result<DVec3> vectorKey(const kv1::Node& entity, std::string_view key, std::optional<DVec3> fallback) {
    const auto* node = entity.find(key);
    if (!node) {
        if (fallback) return *fallback;
        return std::unexpected(invalid(std::format("missing {}", key)));
    }
    // Doubles, not kv1's float Vec3: basis origins are world positions.
    const std::string text = node->decodedValue().value_or("");
    DVec3 value{0.0};
    const char* at = text.data();
    const char* end = text.data() + text.size();
    for (int axis = 0; axis < 3; ++axis) {
        while (at < end && (*at == ' ' || *at == '\t' || *at == '[' || *at == ']')) ++at;
        if (at < end && *at == '+') ++at;
        const auto [next, error] = std::from_chars(at, end, value[axis]);
        if (error != std::errc{}) return std::unexpected(invalid(std::format("{} is not a vector", key)));
        at = next;
    }
    if (!finite(value)) return std::unexpected(invalid(std::format("{} is not finite", key)));
    return value;
}

Result<double> numberKey(const kv1::Node& entity, std::string_view key, double fallback) {
    const auto* node = entity.find(key);
    if (!node) return fallback;
    const auto parsed = node->asDouble();
    if (!parsed || !std::isfinite(*parsed)) return std::unexpected(invalid(std::format("{} is not a number", key)));
    return *parsed;
}

std::string number(double value) {
    if (value == 0.0) return "0"; // Also folds -0.
    char buffer[64];
    const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return error == std::errc{} ? std::string(buffer, end) : std::string("0");
}
std::string vector(DVec3 v) { return number(v.x) + " " + number(v.y) + " " + number(v.z); }

DVec3 unit(DVec3 v) {
    const double length = glm::length(v);
    return length > 0.0 ? v / length : v;
}

// Sutherland-Hodgman: keeps the part of `subject` left of every edge of the convex,
// counter-clockwise `window`.
std::vector<DVec2> clip2(std::vector<DVec2> subject, std::span<const DVec2> window) {
    for (std::size_t i = 0; i < window.size() && !subject.empty(); ++i) {
        const DVec2 a = window[i], b = window[(i + 1) % window.size()];
        const DVec2 edge = b - a;
        const double scale = std::max(glm::length(edge), 1e-12);
        const auto side = [&](DVec2 p) { return cross2(edge, p - a) / scale; };
        std::vector<DVec2> out;
        out.reserve(subject.size() + 2);
        for (std::size_t j = 0; j < subject.size(); ++j) {
            const DVec2 p = subject[j], q = subject[(j + 1) % subject.size()];
            const double sp = side(p), sq = side(q);
            if (sp >= 0.0) out.push_back(p);
            if ((sp >= 0.0) != (sq >= 0.0)) out.push_back(p + (q - p) * (sp / (sp - sq)));
        }
        subject = std::move(out);
    }
    return subject;
}

double area2(std::span<const DVec2> polygon) {
    double area = 0.0;
    for (std::size_t i = 0; i < polygon.size(); ++i) area += cross2(polygon[i], polygon[(i + 1) % polygon.size()]);
    return area * 0.5;
}

// The quad as convex pieces: itself when convex, else two triangles split at the reflex corner.
std::vector<std::vector<DVec2>> convexPieces(const std::array<DVec2, 4>& points) {
    std::vector<DVec2> loop(points.begin(), points.end());
    if (area2(loop) < 0.0) std::ranges::reverse(loop);
    int reflex = -1;
    for (int i = 0; i < 4; ++i) {
        const DVec2 a = loop[(i + 3) % 4], b = loop[i], c = loop[(i + 1) % 4];
        if (cross2(b - a, c - b) < 0.0) reflex = i;
    }
    if (reflex < 0) return {loop};
    const auto at = [&](int i) { return loop[(reflex + i) % 4]; };
    return {{at(0), at(1), at(2)}, {at(0), at(2), at(3)}};
}

// Appends the clipped pieces on one planar polygon to `out`. `facing` is +1 to accept only faces
// whose normal is along quad.normal, -1 only against it, 0 either.
bool clipInto(const OverlayQuad& quad, std::span<const DVec3> polygon, DVec3 faceNormal, OverlayFragment& out,
              int facing = 0) {
    if (polygon.size() < 3) return false;
    const DVec3 n = unit(faceNormal);
    const double along = glm::dot(n, quad.normal);
    if (!(std::abs(along) > kOverlayMinCosine)) return false;
    if (facing != 0 && (along > 0.0) != (facing > 0)) return false;
    std::vector<DVec2> window;
    window.reserve(polygon.size());
    for (const auto& p : polygon) {
        const auto local = quad.local(p);
        if (!local) return false;
        window.push_back(*local);
    }
    if (area2(window) < 0.0) std::ranges::reverse(window);
    if (std::abs(area2(window)) <= 1e-9) return false;
    const double dist = glm::dot(n, polygon[0]);
    bool any = false;
    for (auto& piece : convexPieces(quad.points)) {
        const auto clipped = clip2(std::move(piece), window);
        if (clipped.size() < 3 || std::abs(area2(clipped)) <= 1e-9) continue;
        const auto first = static_cast<std::uint32_t>(out.vertices.size());
        for (const DVec2 p : clipped) {
            const DVec3 flat = quad.origin + quad.basisU * p.x + quad.basisV * p.y;
            const double t = (dist - glm::dot(n, flat)) / glm::dot(n, quad.normal);
            out.vertices.push_back({flat + quad.normal * t, quad.texCoord(p)});
        }
        // Wind each triangle counter-clockwise seen from outside the face.
        for (std::uint32_t i = 1; i + 1 < clipped.size(); ++i)
            out.triangles.push_back(along > 0.0 ? std::array{first, first + i, first + i + 1}
                                                : std::array{first, first + i + 1, first + i});
        any = true;
    }
    return any;
}

} // namespace

std::optional<DVec2> OverlayQuad::local(DVec3 world) const {
    const DMat3 basis(basisU, basisV, normal);
    const double det = glm::determinant(basis);
    if (!(std::abs(det) > 1e-12)) return std::nullopt;
    const DVec3 solved = glm::inverse(basis) * (world - origin);
    return DVec2{solved.x, solved.y};
}

DVec2 OverlayQuad::texCoord(DVec2 p) const {
    // Inverse bilinear (after Inigo Quilez): P(s, t) = a + e s + f t + g s t with a = corner 0,
    // corner 3 at s = 1 and corner 1 at t = 1.
    const DVec2 a = points[0], b = points[3], c = points[2], d = points[1];
    const DVec2 e = b - a, f = d - a, g = a - b + c - d, h = p - a;
    const double k2 = cross2(g, f), k1 = cross2(e, f) + cross2(h, g), k0 = cross2(h, e);
    const double scale = std::max(std::abs(cross2(e, f)), 1e-30);
    const auto sFor = [&](double t) {
        const DVec2 axis = e + g * t;
        const double length2 = glm::dot(axis, axis);
        return length2 > 0.0 ? glm::dot(h - f * t, axis) / length2 : 0.0;
    };
    double s = 0.0, t = 0.0;
    if (std::abs(k2) <= 1e-9 * scale) {
        t = std::abs(k1) > 0.0 ? -k0 / k1 : 0.0;
        s = sFor(t);
    } else {
        const double w = std::sqrt(std::max(k1 * k1 - 4.0 * k0 * k2, 0.0));
        const double t1 = (-k1 - w) / (2.0 * k2), t2 = (-k1 + w) / (2.0 * k2);
        const double s1 = sFor(t1), s2 = sFor(t2);
        const auto outside = [](double s, double t) {
            return std::max({-s, s - 1.0, -t, t - 1.0, 0.0});
        };
        if (outside(s1, t1) <= outside(s2, t2)) { s = s1; t = t1; }
        else { s = s2; t = t2; }
    }
    return {startU + s * (endU - startU), startV + t * (endV - startV)};
}

std::optional<std::string> OverlayQuad::problem() const {
    if (!finite(origin) || !finite(basisU) || !finite(basisV) || !finite(normal)) return "nonfinite basis";
    for (const auto& p : points) if (!finite(p)) return "nonfinite uv point";
    for (const double value : {startU, endU, startV, endV}) if (!std::isfinite(value)) return "nonfinite texture range";
    if (!(std::abs(glm::determinant(DMat3(basisU, basisV, normal))) > 1e-9)) return "degenerate basis";
    if (!(std::abs(area2(std::span<const DVec2>(points))) > 1e-9)) return "degenerate quad";
    return std::nullopt;
}

Result<OverlayQuad> parseOverlay(const kv1::Node& entity) {
    OverlayQuad quad;
    const auto origin = vectorKey(entity, "BasisOrigin", std::nullopt);
    if (!origin) return std::unexpected(origin.error());
    const auto normal = vectorKey(entity, "BasisNormal", std::nullopt);
    if (!normal) return std::unexpected(normal.error());
    const auto u = vectorKey(entity, "BasisU", std::nullopt);
    if (!u) return std::unexpected(u.error());
    quad.origin = *origin;
    quad.normal = unit(*normal);
    quad.basisU = unit(*u);
    const auto v = vectorKey(entity, "BasisV", glm::cross(quad.normal, quad.basisU));
    if (!v) return std::unexpected(v.error());
    quad.basisV = unit(*v);
    for (std::size_t i = 0; i < 4; ++i) {
        const auto point = vectorKey(entity, std::format("uv{}", i), std::nullopt);
        if (!point) return std::unexpected(point.error());
        quad.points[i] = {point->x, point->y};
    }
    const std::pair<const char*, double OverlayQuad::*> ranges[] = {
        {"StartU", &OverlayQuad::startU}, {"EndU", &OverlayQuad::endU},
        {"StartV", &OverlayQuad::startV}, {"EndV", &OverlayQuad::endV}};
    for (const auto& [key, member] : ranges) {
        const auto value = numberKey(entity, key, quad.*member);
        if (!value) return std::unexpected(value.error());
        quad.*member = *value;
    }
    if (const auto problem = quad.problem()) return std::unexpected(invalid(*problem));
    return quad;
}

std::vector<std::pair<std::string, std::string>> overlayKeys(const OverlayQuad& quad) {
    std::vector<std::pair<std::string, std::string>> keys{
        {"BasisOrigin", vector(quad.origin)}, {"BasisU", vector(quad.basisU)},
        {"BasisV", vector(quad.basisV)}, {"BasisNormal", vector(quad.normal)}};
    for (std::size_t i = 0; i < 4; ++i)
        keys.emplace_back(std::format("uv{}", i), vector(DVec3{quad.points[i], 0.0}));
    keys.emplace_back("StartU", number(quad.startU));
    keys.emplace_back("EndU", number(quad.endU));
    keys.emplace_back("StartV", number(quad.startV));
    keys.emplace_back("EndV", number(quad.endV));
    return keys;
}

std::vector<int> parseSideList(std::string_view text) {
    std::vector<int> sides;
    std::size_t at = 0;
    while (at < text.size()) {
        while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) ++at;
        std::size_t end = at;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))) ++end;
        int value = 0;
        const auto [ptr, error] = std::from_chars(text.data() + at, text.data() + end, value);
        if (end > at && error == std::errc{} && ptr == text.data() + end) sides.push_back(value);
        at = end;
    }
    return sides;
}

std::string sideListText(std::span<const int> sides) {
    std::string text;
    for (const int side : sides) {
        if (!text.empty()) text += ' ';
        text += std::to_string(side);
    }
    return text;
}

std::optional<OverlayFragment> clipOverlay(const OverlayQuad& quad, std::span<const DVec3> polygon, DVec3 faceNormal,
                                           std::uint32_t side) {
    OverlayFragment fragment;
    fragment.side = side;
    fragment.normal = unit(faceNormal);
    if (!clipInto(quad, polygon, faceNormal, fragment)) return std::nullopt;
    return fragment;
}

std::optional<OverlayFragment> clipOverlay(const OverlayQuad& quad, const DisplacementMesh& mesh, std::uint32_t side) {
    OverlayFragment fragment;
    fragment.side = side;
    // The surface takes the quad on the side it faces as a whole; triangles folded back against
    // it (overhangs) would cover the same ground twice.
    DVec3 total{0.0};
    for (const auto& triangle : mesh.triangles)
        total += glm::cross(mesh.positions.at(triangle[1]) - mesh.positions.at(triangle[0]),
                            mesh.positions.at(triangle[2]) - mesh.positions.at(triangle[0]));
    const int facing = glm::dot(total, quad.normal) >= 0.0 ? 1 : -1;
    DVec3 normalSum{0.0};
    for (const auto& triangle : mesh.triangles) {
        const std::array<DVec3, 3> corners{mesh.positions.at(triangle[0]), mesh.positions.at(triangle[1]),
                                           mesh.positions.at(triangle[2])};
        const DVec3 normal = glm::cross(corners[1] - corners[0], corners[2] - corners[0]);
        if (!(glm::length(normal) > 0.0)) continue;
        if (clipInto(quad, corners, normal, fragment, facing)) normalSum += normal;
    }
    if (fragment.triangles.empty()) return std::nullopt;
    fragment.normal = unit(normalSum);
    return fragment;
}

double fragmentArea(const OverlayFragment& fragment) {
    double area = 0.0;
    for (const auto& t : fragment.triangles) {
        const auto& a = fragment.vertices[t[0]].position;
        area += 0.5 * glm::length(glm::cross(fragment.vertices[t[1]].position - a, fragment.vertices[t[2]].position - a));
    }
    return area;
}

double quadArea(const OverlayQuad& quad) {
    double area = 0.0;
    for (std::size_t i = 1; i + 1 < 4; ++i)
        area += 0.5 * glm::length(glm::cross(quad.corner(i) - quad.corner(0), quad.corner(i + 1) - quad.corner(0)));
    return area;
}

OverlayQuad faceAlignedQuad(DVec3 centre, DVec3 normal, DVec3 textureU, DVec2 size) {
    OverlayQuad quad;
    quad.origin = centre;
    quad.normal = unit(normal);
    DVec3 u = textureU - quad.normal * glm::dot(textureU, quad.normal);
    if (!(glm::length(u) > 1e-6)) {
        // Any in-plane axis: the world axis least aligned with the normal, made perpendicular.
        const DVec3 a = std::abs(quad.normal.z) < 0.9 ? DVec3{0, 0, 1} : DVec3{1, 0, 0};
        u = glm::cross(a, quad.normal);
    }
    quad.basisU = unit(u);
    quad.basisV = glm::cross(quad.normal, quad.basisU);
    const DVec2 half = size * 0.5;
    quad.points = {DVec2{-half.x, -half.y}, DVec2{-half.x, half.y}, DVec2{half.x, half.y}, DVec2{half.x, -half.y}};
    quad.startU = 0.0;
    quad.endU = 1.0;
    quad.startV = 1.0; // Corner 0 is the bottom-left: the texture's bottom row.
    quad.endV = 0.0;
    return quad;
}

TransitionShape parseTransition(const kv1::Node& entity) {
    TransitionShape shape;
    const std::pair<const char*, double TransitionShape::*> keys[] = {
        {"Width1", &TransitionShape::landWidth}, {"Width2", &TransitionShape::waterWidth},
        {"LengthTexcoordStart", &TransitionShape::lengthStart}, {"LengthTexcoordEnd", &TransitionShape::lengthEnd},
        {"WidthTexcoordStart", &TransitionShape::widthStart}, {"WidthTexcoordEnd", &TransitionShape::widthEnd}};
    for (const auto& [key, member] : keys)
        if (const auto value = numberKey(entity, key, shape.*member)) shape.*member = *value;
    return shape;
}

std::vector<OverlayQuad> transitionQuads(const TransitionShape& shape, const Plane& water,
                                         std::span<const std::vector<DVec3>> landPolygons) {
    constexpr double kRepeat = 128.0; // World units per texture repeat along the shore.
    std::vector<OverlayQuad> quads;
    const DVec3 n = unit(water.normal);
    for (const auto& polygon : landPolygons) {
        std::vector<DVec3> crossings;
        DVec3 above{0.0};
        int aboveCount = 0;
        for (std::size_t i = 0; i < polygon.size(); ++i) {
            const DVec3 p = polygon[i], q = polygon[(i + 1) % polygon.size()];
            const double dp = water.signedDistance(p), dq = water.signedDistance(q);
            if (dp > 0.0) { above += p; ++aboveCount; }
            if ((dp > 0.0) != (dq > 0.0) && dp != dq) crossings.push_back(p + (q - p) * (dp / (dp - dq)));
        }
        if (crossings.size() < 2 || aboveCount == 0) continue;
        const DVec3 a = crossings.front(), b = crossings.back();
        const double length = glm::length(b - a);
        if (!(length > 1e-6)) continue;
        OverlayQuad quad;
        quad.origin = a;
        quad.normal = n;
        quad.basisU = (b - a) / length;
        DVec3 towardWater = glm::cross(n, quad.basisU);
        const DVec3 land = above / double(aboveCount);
        if (glm::dot(towardWater, land - a) > 0.0) towardWater = -towardWater;
        quad.basisV = towardWater;
        quad.points = {DVec2{0.0, -shape.landWidth}, DVec2{0.0, shape.waterWidth}, DVec2{length, shape.waterWidth},
                       DVec2{length, -shape.landWidth}};
        const double perUnit = (shape.lengthEnd - shape.lengthStart) / kRepeat;
        const double s0 = glm::dot(a, quad.basisU);
        quad.startU = shape.lengthStart + s0 * perUnit;
        quad.endU = shape.lengthStart + (s0 + length) * perUnit;
        quad.startV = shape.widthStart;
        quad.endV = shape.widthEnd;
        quads.push_back(quad);
    }
    return quads;
}

OverlayQuad transformOverlay(const OverlayQuad& quad, const DMat3& linear, DVec3 translation) {
    if (!(std::abs(glm::determinant(linear)) > 1e-12)) throw std::invalid_argument("singular overlay transform");
    // The engine ignores the stored BasisV and rebuilds it as BasisNormal x BasisU, so the moved
    // quad keeps that right-handed, orthonormal basis and re-expresses the moved corners in it.
    // A mirror or a skewing scale then still lands every corner where the map sends it.
    OverlayQuad out = quad;
    out.origin = linear * quad.origin + translation;
    out.normal = unit(glm::transpose(glm::inverse(linear)) * quad.normal);
    DVec3 u = linear * quad.basisU;
    u -= out.normal * glm::dot(u, out.normal);
    out.basisU = unit(u);
    out.basisV = glm::cross(out.normal, out.basisU);
    for (std::size_t i = 0; i < 4; ++i) {
        const DVec3 d = linear * quad.corner(i) + translation - out.origin;
        out.points[i] = {glm::dot(d, out.basisU), glm::dot(d, out.basisV)};
    }
    return out;
}

} // namespace apex::geometry
