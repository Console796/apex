#include "geometry/solid_block.hpp"

#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace apex::geometry {
namespace {

std::string number(double value) {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return text.str();
}
std::string vector(DVec3 p) {
    return number(p.x) + " " + number(p.y) + " " + number(p.z);
}
std::string axis(DVec3 direction, double scale) {
    return "[" + vector(direction) + " 0] " + number(scale);
}
void append(kv1::Node& block, std::string key, std::string value) {
    block.append(kv1::Node::value(std::move(key), std::move(value))).value();
}

} // namespace

kv1::Node solidBlock(const ConvexPolyhedron& poly, const NewSolidStyle& style) {
    if (!poly.isValid() || !std::isfinite(style.textureScale) ||
        style.textureScale <= 0.0 || style.lightmapScale <= 0)
        throw std::invalid_argument("New solid requires valid geometry and positive scales");
    auto solid = kv1::Node::block("solid");
    append(solid, "id", "0");
    for (const auto& face : poly.faces) {
        auto side = kv1::Node::block("side");
        append(side, "id", "0");
        const auto points = poly.sourcePlanePoints(face).value();
        append(side, "plane", "(" + vector(points[0]) + ") (" + vector(points[1]) +
                              ") (" + vector(points[2]) + ")");
        // Hammer saves point_data straight after plane (probe e_control, g_side_order).
        auto data = kv1::Node::block("point_data");
        append(data, "numpts", std::to_string(face.vertices.size()));
        std::size_t index = 0;
        for (auto v = face.vertices.rbegin(); v != face.vertices.rend(); ++v)
            append(data, "point", std::to_string(index++) + " " + vector(poly.vertices[*v]));
        side.append(std::move(data)).value();
        append(side, "material", style.material);
        auto uv = generateUv(face, 1, 1);
        if (style.alignment == UvAlignment::Face) {
            const auto n = face.plane.normal;
            uv.uaxis = glm::normalize(uv.uaxis - n * glm::dot(n, uv.uaxis));
            // Keep Hammer's V direction, with an orthonormal basis on sloping faces.
            uv.vaxis = glm::normalize(uv.vaxis - n * glm::dot(n, uv.vaxis) -
                                      uv.uaxis * glm::dot(uv.uaxis, uv.vaxis));
        }
        append(side, "uaxis", axis(uv.uaxis, style.textureScale));
        append(side, "vaxis", axis(uv.vaxis, style.textureScale));
        append(side, "rotation", "0");
        append(side, "lightmapscale", std::to_string(style.lightmapScale));
        append(side, "smoothing_groups", "0");
        solid.append(std::move(side)).value();
    }
    return solid;
}

} // namespace apex::geometry
