#include "render/solid_mesh.hpp"
#include "render/lightmap_atlas.hpp"

#include "geometry/solid_geometry.hpp"

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

double loopArea(std::span<const DVec3> loop) {
    DVec3 sum{0.0};
    for (std::size_t i = 1; i + 1 < loop.size(); ++i) sum += glm::cross(loop[i] - loop[0], loop[i + 1] - loop[0]);
    return 0.5 * glm::length(sum);
}

// A placed face cannot be compared chart-for-chart: the baker charted the rotated plane. The baked
// fragments must instead lie on the face moved by the bake's transform and cover its area. Compiled
// fragments (vbsp's CSG removes the parts of a side that touch other brushes) may cover less, never more.
bool placedMatch(const geometry::Face& face, const geometry::ConvexPolyhedron& poly, const LightmapAtlas& atlas,
                 std::span<const std::size_t> fragments, const DMat4& toWorld) {
    std::vector<DVec3> loop;
    AABB bounds;
    for (const auto index : face.vertices) {
        loop.push_back(DVec3(toWorld * DVec4(poly.vertices[index], 1.0)));
        bounds.expand(loop.back());
    }
    const auto normal = glm::normalize(DMat3(toWorld) * face.plane.normal);
    constexpr double kTolerance = 0.05;
    double area = 0.0;
    for (const auto fragment : fragments) {
        const auto& vertices = atlas.faces[fragment].vertices;
        for (const auto& p : vertices)
            if (std::abs(glm::dot(normal, p - loop[0])) > kTolerance ||
                glm::any(glm::lessThan(p, bounds.min - kTolerance)) ||
                glm::any(glm::greaterThan(p, bounds.max + kTolerance))) return false;
        area += loopArea(vertices);
    }
    const auto expected = loopArea(loop);
    if (atlas.compiled) return area <= expected * 1.01 + kTolerance;
    return std::abs(area - expected) <= 0.01 * expected + kTolerance;
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
                            const std::function<TextureSize(std::string_view)>& sizeOf,
                            const LightmapAtlas* lightmaps, std::uint64_t solidId,
                            const LightmapPlacement* placement) {
    auto geometry = geometry::solidGeometry(solidBlock);
    if (!geometry) return std::unexpected(geometry.error());
    const document::InstanceSolidOrigin* origin = nullptr;
    if (placement) {
        // A placed template solid without baked sides (props, other entities) stays unmapped.
        const auto* id = solidBlock.find("id");
        const auto found = id ? placement->find(int(id->asInt().value_or(0))) : placement->end();
        if (found == placement->end()) lightmaps = nullptr;
        else origin = &found->second;
    }
    auto mesh = meshSolid(geometry->poly, sizeOf, lightmaps, solidId, origin);
    if (!mesh) return mesh;
    std::set<document::SideId> displacements;
    for (const auto* side : solidBlock.findAll("side"))
        if (const auto* id = side->find("id"); id && side->find("dispinfo"))
            if (const auto value = id->asInt()) displacements.insert(static_cast<document::SideId>(*value));
    for (auto& batch : mesh->batches) for (auto& face : batch.faces)
        face.displacement = displacements.contains(face.side);
    return mesh;
}

Result<SolidMesh> meshSolid(const geometry::ConvexPolyhedron& poly,
                            const std::function<TextureSize(std::string_view)>& sizeOf,
                            const LightmapAtlas* lightmaps, std::uint64_t solidId,
                            const document::InstanceSolidOrigin* origin) {
    const auto problems = poly.validate();
    if (!problems.empty()) return fail(problems.front().message);
    const auto* polyhedron = &poly;

    SolidMesh mesh;
    mesh.bounds = polyhedron->bounds();
    mesh.origin = polyhedron->center();
    std::map<std::string, std::size_t> batchIndex;
    try {
    for (const geometry::Face& face : polyhedron->faces) {
        if (face.vertices.size() < 3) continue;
        const auto [it, inserted] = batchIndex.try_emplace(lower(face.material), mesh.batches.size());
        if (inserted) mesh.batches.push_back({it->first, {}, {}, {}, {}});
        BatchMesh& batch = mesh.batches[it->second];
        const TextureSize size = sizeOf(it->first);
        std::vector<std::size_t> fragments;
        std::pair<std::uint64_t, std::uint32_t> key{solidId, face.sideId};
        if (origin) {
            const auto side = origin->sides.find(int(face.sideId));
            key = side == origin->sides.end() ? std::pair<std::uint64_t, std::uint32_t>{0, 0} :
                std::pair<std::uint64_t, std::uint32_t>{origin->solid, side->second};
        }
        if (lightmaps) if (const auto found = lightmaps->fragments.find(key);
                          found != lightmaps->fragments.end()) fragments = found->second;
        std::optional<std::size_t> retained;
        if (!fragments.empty() && (origin || lightmaps->compiled)) {
            // A compiled lattice is the compiler's own, so it is checked against the face only by position.
            if (!placedMatch(face, *polyhedron, *lightmaps, fragments, origin ? origin->transform : DMat4(1.0)))
                fragments.clear();
        } else if (!fragments.empty()) {
            // A stale bake must never substitute old geometry after a document edit.
            const auto current = lighting::chartFace(face, polyhedron->vertices);
            bool matches = current.fragments.size() == fragments.size();
            for (std::size_t i = 0; matches && i < fragments.size(); ++i) {
                const auto& a = current.fragments[i];
                const auto& b = lightmaps->faces[fragments[i]];
                matches = a.vertices == b.vertices && a.chart.dimensions == b.chart.dimensions &&
                    a.chart.mins == b.chart.mins && a.chart.offset == b.chart.offset &&
                    a.chart.worldToLuxel == b.chart.worldToLuxel;
            }
            if (!matches) {
                if (lightmaps->retainStale) retained = fragments.front();
                fragments.clear();
            }
        }
        // Use the exact baker loops, so subdivision boundaries cannot interpolate across atlas rects.
        // `world` (placed faces) is the loop in the baked snapshot's space, for the lightmap lookup only.
        const auto emit = [&](std::span<const DVec3> loop, std::optional<std::size_t> fragment, bool triangles = true,
                              std::span<const DVec3> world = {}) {
            const auto base = static_cast<std::uint32_t>(batch.vertices.size());
            for (std::size_t vertex = 0; vertex < loop.size(); ++vertex) {
                const DVec3 p = loop[vertex];
                const DVec2 uv = DVec2(size.uvTransform * DVec3{geometry::uvAt(face, p, size.width, size.height), 1.0});
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
                if (fragment) {
                    auto lm = lightmaps->uv(*fragment, world.empty() ? p : world[vertex]);
                    if (retained) {
                        const auto& rect = lightmaps->rects[*fragment];
                        lm = glm::clamp(lm, (DVec2(rect.x, rect.y) + 0.5) / double(lightmaps->size),
                            (DVec2(rect.x + rect.width, rect.y + rect.height) - 0.5) / double(lightmaps->size));
                    }
                    v.lightmapUv[0] = static_cast<float>(lm.x);
                    v.lightmapUv[1] = static_cast<float>(lm.y);
                }
                batch.vertices.push_back(v);
            }
            const auto count = static_cast<std::uint32_t>(loop.size());
            if (triangles) for (std::uint32_t i = 1; i + 1 < count; ++i)
                batch.indices.insert(batch.indices.end(), {base, base + i, base + i + 1});
            return base;
        };
        FaceRange range;
        for (const auto index : face.vertices) range.centre += polyhedron->vertices[index];
        range.centre /= static_cast<double>(face.vertices.size());
        range.side = static_cast<document::SideId>(face.sideId);
        range.firstIndex = static_cast<std::uint32_t>(batch.indices.size());
        range.firstEdgeIndex = static_cast<std::uint32_t>(batch.edges.size());
        std::vector<DVec3> loop;
        for (const auto index : face.vertices) loop.push_back(polyhedron->vertices[index]);
        std::uint32_t edgeBase = 0;
        if (fragments.empty()) edgeBase = emit(loop, retained);
        else {
            for (const auto fragment : fragments) {
                const auto& baked = lightmaps->faces[fragment].vertices;
                if (!origin) { emit(baked, fragment); continue; }
                // Template-local positions for the instance's own transform at draw time.
                const auto inverse = glm::inverse(origin->transform);
                std::vector<DVec3> local;
                for (const DVec3 p : baked) local.push_back(DVec3(inverse * DVec4(p, 1.0)));
                emit(local, fragment, true, baked);
            }
            edgeBase = emit(loop, {}, false); // Original perimeter only; no subdivision seams in wire/overlays.
        }
        const auto count = static_cast<std::uint32_t>(loop.size());
        for (std::uint32_t i = 0; i < count; ++i)
            batch.edges.insert(batch.edges.end(), {edgeBase + i, edgeBase + (i + 1) % count});
        range.indexCount = static_cast<std::uint32_t>(batch.indices.size()) - range.firstIndex;
        range.edgeIndexCount = static_cast<std::uint32_t>(batch.edges.size()) - range.firstEdgeIndex;
        batch.faces.push_back(range);
    }
    } catch (const std::exception& e) { // uvAt rejects invalid UV data.
        return fail(std::string("invalid face data: ") + e.what());
    }
    return mesh;
}

} // namespace apex::render
