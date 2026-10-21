#include "geometry/solid_geometry.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>

namespace apex::geometry {
namespace {

std::unexpected<Error> fail(std::string message) {
    return std::unexpected(Error{ErrorCode::ParseError, std::move(message)});
}

// VMF plane points and UV axes have punctuation absent from core's vector parser.
class Numbers {
public:
    explicit Numbers(std::string_view text) : text_(text) {}
    bool consume(char c) {
        trim();
        if (text_.empty() || text_.front() != c) return false;
        text_.remove_prefix(1);
        return true;
    }
    bool real(double& value) {
        trim();
        if (text_.empty()) return false;
        if (text_.front() == '+') text_.remove_prefix(1);
        const auto [end, error] = std::from_chars(text_.data(), text_.data() + text_.size(), value);
        if (error != std::errc{} || !std::isfinite(value)) return false;
        if (end != text_.data() + text_.size() &&
            std::string_view(" \t\r\n)]").find(*end) == std::string_view::npos) return false;
        text_.remove_prefix(static_cast<std::size_t>(end - text_.data()));
        return true;
    }
    bool vector(DVec3& value) { return real(value.x) && real(value.y) && real(value.z); }
    bool empty() {
        trim();
        return text_.empty();
    }
private:
    void trim() {
        const auto first = text_.find_first_not_of(" \t\r\n");
        text_.remove_prefix(first == std::string_view::npos ? text_.size() : first);
    }
    std::string_view text_;
};

Result<Plane> readPlane(const kv1::Node& node) {
    auto text = node.decodedValue();
    if (!text) return std::unexpected(text.error());
    Numbers input(*text);
    std::array<DVec3, 3> points;
    for (auto& point : points) {
        if (!input.consume('(') || !input.vector(point) || !input.consume(')'))
            return fail("expected three parenthesized plane points");
    }
    if (!input.empty()) return fail("extra text after plane");
    auto plane = planePointsFromSource(points[0], points[1], points[2]);
    if (!plane) return fail("side with degenerate plane");
    return *plane;
}

Result<void> readAxis(const kv1::Node& node, DVec3& axis, double& shift, double& scale) {
    auto text = node.decodedValue();
    if (!text) return std::unexpected(text.error());
    Numbers input(*text);
    if (!input.consume('[') || !input.vector(axis) || !input.real(shift) ||
        !input.consume(']') || !input.real(scale) || !input.empty())
        return fail("expected [x y z shift] scale");
    return {};
}

Result<std::uint32_t> smoothingGroups(const kv1::Node& node) {
    auto decoded = node.decodedValue();
    if (!decoded) return std::unexpected(decoded.error());
    auto text = std::string_view(*decoded);
    const auto first = text.find_first_not_of(" \t\r\n");
    text.remove_prefix(first == std::string_view::npos ? text.size() : first);
    const auto last = text.find_last_not_of(" \t\r\n");
    if (last != std::string_view::npos) text = text.substr(0, last + 1);
    if (text.empty()) return fail("invalid smoothing_groups bitmask");
    if (text.front() == '+') text.remove_prefix(1);
    if (text.empty()) return fail("invalid smoothing_groups bitmask");
    if (text.front() == '-') {
        std::int64_t value{};
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size())
            return fail("invalid smoothing_groups bitmask");
        return static_cast<std::uint32_t>(value);
    }
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        return fail("invalid smoothing_groups bitmask");
    return static_cast<std::uint32_t>(value);
}

Result<PointDataSide> readSide(const kv1::Node& side) {
    PointDataSide value;
    const auto* plane = side.find("plane");
    const auto* id = side.find("id");
    if (!plane || !id) return fail("side without plane/id");
    auto parsedPlane = readPlane(*plane);
    if (!parsedPlane) return std::unexpected(parsedPlane.error());
    value.plane = *parsedPlane;
    auto sideId = id->asInt();
    if (!sideId) return std::unexpected(sideId.error());
    value.sideId = static_cast<std::uint32_t>(*sideId);
    if (const auto* material = side.find("material")) {
        auto text = material->decodedValue();
        if (!text) return std::unexpected(text.error());
        value.material = std::move(*text);
    }
    if (const auto* u = side.find("uaxis")) {
        auto result = readAxis(*u, value.uv.uaxis, value.uv.shiftU, value.uv.scaleU);
        if (!result) return std::unexpected(result.error());
    }
    if (const auto* v = side.find("vaxis")) {
        auto result = readAxis(*v, value.uv.vaxis, value.uv.shiftV, value.uv.scaleV);
        if (!result) return std::unexpected(result.error());
    }
    if (const auto* scale = side.find("lightmapscale")) {
        auto parsed = scale->asInt();
        if (!parsed) return std::unexpected(parsed.error());
        value.lightmapScale = *parsed;
    }
    if (const auto* groups = side.find("smoothing_groups")) {
        auto parsed = smoothingGroups(*groups);
        if (!parsed) return std::unexpected(parsed.error());
        value.smoothingGroups = *parsed;
    }
    if (const auto* data = side.find("point_data")) {
        for (const auto* point : data->findAll("point")) {
            auto text = point->decodedValue();
            if (!text) return std::unexpected(text.error());
            Numbers input(*text);
            double index;
            DVec3 xyz;
            if (!input.real(index) || !input.vector(xyz) || !input.empty())
                return fail("expected point index and three components");
            value.points.push_back(xyz);
        }
        // Saved point_data follows Source's clockwise plane winding.
        std::reverse(value.points.begin(), value.points.end());
    }
    return value;
}

} // namespace

Result<SolidGeometry> solidGeometry(const kv1::Node& solid) {
    SolidGeometry result;
    for (const auto* side : solid.findAll("side")) {
        auto parsed = readSide(*side);
        if (!parsed) return std::unexpected(parsed.error().withContext("solid side"));
        result.sides.push_back(std::move(*parsed));
    }
    auto poly = ConvexPolyhedron::fromPointData(result.sides);
    if (!poly) {
        std::vector<PlaneSide> planes(result.sides.begin(), result.sides.end());
        auto planePoly = ConvexPolyhedron::fromPlanes(planes);
        if (!planePoly) {
            std::string message = "solid is not a valid convex polyhedron";
            if (!poly.error().empty()) message += ": point_data: " + poly.error().front().message;
            if (!planePoly.error().empty()) message += "; planes: " + planePoly.error().front().message;
            return fail(std::move(message));
        }
        result.poly = std::move(*planePoly);
        result.usedPlaneFallback = true;
        return result;
    }
    result.poly = std::move(*poly);
    return result;
}

} // namespace apex::geometry
