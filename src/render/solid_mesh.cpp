#include "render/solid_mesh.hpp"

#include "document/vmf.hpp"
#include "geometry/convex_polyhedron.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <exception>
#include <set>

namespace apex::render {
namespace {

std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::unexpected<Error> fail(std::string message) {
    return std::unexpected(Error{ErrorCode::ParseError, std::move(message)});
}

} // namespace

std::vector<std::string> solidMaterials(const kv1::Node& solidBlock) {
    std::set<std::string> names;
    for (const kv1::Node* side : solidBlock.findAll("side")) {
        if (const kv1::Node* material = side->find("material")) {
            if (auto text = material->decodedValue()) names.insert(lower(*text));
        }
    }
    return {names.begin(), names.end()};
}

Result<SolidMesh> meshSolid(const kv1::Node& solidBlock,
                            const std::function<TextureSize(std::string_view)>& sizeOf) {
    kv1::Node copy = solidBlock; // vmf::Value wraps mutable nodes; this is a private copy.
    std::vector<geometry::PlaneSide> sides;
    for (kv1::Node* side : copy.findAll("side")) {
        geometry::PlaneSide value;
        vmf::Value plane(side->find("plane"));
        if (!plane) return fail("side without plane");
        auto points = plane.asPlane();
        if (!points) return std::unexpected(points.error());
        const auto vec = [](vmf::Vec3 v) { return DVec3{v[0], v[1], v[2]}; };
        const auto derived = geometry::planePointsFromSource(vec(points->points[0]), vec(points->points[1]),
                                                             vec(points->points[2]));
        if (!derived) return fail("side with degenerate plane");
        value.plane = *derived;
        if (vmf::Value id(side->find("id")); id) {
            if (auto v = id.asInt()) value.sideId = static_cast<std::uint32_t>(*v);
        }
        if (vmf::Value material(side->find("material")); material) {
            if (auto v = material.text()) value.material = lower(*v);
        }
        if (vmf::Value u(side->find("uaxis")); u) {
            if (auto v = u.asUvAxis()) {
                value.uv.uaxis = vec(v->axis);
                value.uv.shiftU = v->shift;
                value.uv.scaleU = v->scale;
            }
        }
        if (vmf::Value v(side->find("vaxis")); v) {
            if (auto a = v.asUvAxis()) {
                value.uv.vaxis = vec(a->axis);
                value.uv.shiftV = a->shift;
                value.uv.scaleV = a->scale;
            }
        }
        sides.push_back(std::move(value));
    }
    if (sides.size() < 4) return fail("solid with fewer than four sides");
    auto polyhedron = geometry::ConvexPolyhedron::fromPlanes(sides);
    if (!polyhedron) {
        std::string message = "solid is not a valid convex polyhedron";
        if (!polyhedron.error().empty()) message += ": " + polyhedron.error().front().message;
        return fail(std::move(message));
    }

    SolidMesh mesh;
    mesh.bounds = polyhedron->bounds();
    mesh.origin = polyhedron->center();
    std::map<std::string, std::size_t> batchIndex;
    try {
    for (const geometry::Face& face : polyhedron->faces) {
        if (face.vertices.size() < 3) continue;
        const auto [it, inserted] = batchIndex.try_emplace(face.material, mesh.batches.size());
        if (inserted) mesh.batches.push_back({face.material, {}, {}, {}});
        BatchMesh& batch = mesh.batches[it->second];
        const TextureSize size = sizeOf(face.material);
        const auto base = static_cast<std::uint32_t>(batch.vertices.size());
        for (const std::size_t index : face.vertices) {
            const DVec3 p = polyhedron->vertices[index];
            const DVec2 uv = geometry::uvAt(face, p, size.width, size.height);
            const DVec3 relative = p - mesh.origin;
            BrushVertex v{};
            v.position[0] = static_cast<float>(relative.x);
            v.position[1] = static_cast<float>(relative.y);
            v.position[2] = static_cast<float>(relative.z);
            v.normal[0] = static_cast<float>(face.plane.normal.x);
            v.normal[1] = static_cast<float>(face.plane.normal.y);
            v.normal[2] = static_cast<float>(face.plane.normal.z);
            v.uv[0] = static_cast<float>(uv.x);
            v.uv[1] = static_cast<float>(uv.y);
            v.material = 0;
            batch.vertices.push_back(v);
        }
        const auto count = static_cast<std::uint32_t>(face.vertices.size());
        for (std::uint32_t i = 1; i + 1 < count; ++i) {
            batch.indices.insert(batch.indices.end(), {base, base + i, base + i + 1});
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            batch.edges.insert(batch.edges.end(), {base + i, base + (i + 1) % count});
        }
    }
    } catch (const std::exception& e) { // uvAt rejects invalid UV data.
        return fail(std::string("invalid face data: ") + e.what());
    }
    return mesh;
}

} // namespace apex::render
