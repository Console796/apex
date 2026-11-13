#include "render/solid_mesh.hpp"
#include "render/lightmap_atlas.hpp"

#include "document/displacement.hpp"
#include "geometry/displacement.hpp"
#include "geometry/solid_geometry.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <map>
#include <set>
#include <utility>

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

// A solid's displaced sides: every side document::isDisplaced flags, and the ones that parsed.
struct Displacements {
    std::set<document::SideId> sides;
    std::map<document::SideId, geometry::Displacement> parsed;
};

// Index segments of a batch, concatenated in this order (BatchMesh's layout).
enum Segment : std::size_t { kDisplaced, kFlat, kCasterOnly, kSegments };

Result<SolidMesh> meshPolyhedron(const geometry::ConvexPolyhedron& poly,
                                 const std::function<TextureSize(std::string_view)>& sizeOf,
                                 const LightmapAtlas* lightmaps, std::uint64_t solidId,
                                 const document::InstanceSolidOrigin* origin, const Displacements& displacements) {
    const auto problems = poly.validate();
    if (!problems.empty()) return fail(problems.front().message);
    const auto* polyhedron = &poly;

    SolidMesh mesh;
    mesh.bounds = polyhedron->bounds();
    mesh.origin = polyhedron->center();
    std::map<std::string, std::size_t> batchIndex;
    std::vector<std::array<std::vector<std::uint32_t>, kSegments>> segments; // Parallel to mesh.batches.
    std::vector<std::vector<Segment>> rangeSegments;                          // Parallel to each batch's faces.
    try {
    for (const geometry::Face& face : polyhedron->faces) {
        if (face.vertices.size() < 3) continue;
        const auto [it, inserted] = batchIndex.try_emplace(lower(face.material), mesh.batches.size());
        if (inserted) {
            mesh.batches.push_back({it->first, {}, {}, {}, {}});
            segments.emplace_back();
            rangeSegments.emplace_back();
        }
        BatchMesh& batch = mesh.batches[it->second];
        auto& indices = segments[it->second];
        const TextureSize size = sizeOf(it->first);
        const auto sideId = static_cast<document::SideId>(face.sideId);

        // D2 mesh of a displaced side; nullopt draws the flat face (bad dispinfo, non-quad base).
        const geometry::Displacement* disp = nullptr;
        std::optional<geometry::DisplacementMesh> displaced;
        if (const auto found = displacements.parsed.find(sideId); found != displacements.parsed.end()) {
            if (const auto corners = geometry::displacementCorners(*polyhedron, face))
                if (auto built = geometry::displacementMesh(*corners, found->second)) {
                    disp = &found->second;
                    displaced = std::move(*built);
                }
        }

        std::vector<std::size_t> fragments;
        std::pair<std::uint64_t, std::uint32_t> key{solidId, face.sideId};
        if (origin) {
            const auto side = origin->sides.find(int(face.sideId));
            key = side == origin->sides.end() ? std::pair<std::uint64_t, std::uint32_t>{0, 0} :
                std::pair<std::uint64_t, std::uint32_t>{origin->solid, side->second};
        }
        // No displacement lightmaps until 0.8.5: a displaced face never takes a flat chart.
        if (lightmaps && !displaced) if (const auto found = lightmaps->fragments.find(key);
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
        const auto vertexAt = [&](DVec3 p, DVec3 uvPoint, DVec3 normal) {
            const DVec2 uv = DVec2(size.uvTransform * DVec3{geometry::uvAt(face, uvPoint, size.width, size.height), 1.0});
            const DVec3 relative = p - mesh.origin;
            BrushVertex v{};
            v.position[0] = static_cast<float>(relative.x);
            v.position[1] = static_cast<float>(relative.y);
            v.position[2] = static_cast<float>(relative.z);
            v.normal[0] = static_cast<float>(normal.x);
            v.normal[1] = static_cast<float>(normal.y);
            v.normal[2] = static_cast<float>(normal.z);
            v.uv[0] = static_cast<float>(uv.x);
            v.uv[1] = static_cast<float>(uv.y);
            v.material = 0;
            return v;
        };
        // Use the exact baker loops, so subdivision boundaries cannot interpolate across atlas rects.
        // `world` (placed faces) is the loop in the baked snapshot's space, for the lightmap lookup only.
        const auto emit = [&](std::span<const DVec3> loop, std::optional<std::size_t> fragment, bool triangles = true,
                              std::span<const DVec3> world = {}, Segment segment = kFlat) {
            const auto base = static_cast<std::uint32_t>(batch.vertices.size());
            for (std::size_t vertex = 0; vertex < loop.size(); ++vertex) {
                const DVec3 p = loop[vertex];
                BrushVertex v = vertexAt(p, p, face.plane.normal);
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
                indices[segment].insert(indices[segment].end(), {base, base + i, base + i + 1});
            return base;
        };
        FaceRange range;
        for (const auto index : face.vertices) range.centre += polyhedron->vertices[index];
        range.centre /= static_cast<double>(face.vertices.size());
        range.normal = face.plane.normal;
        range.side = sideId;
        range.displacement = displacements.sides.contains(sideId);
        const Segment segment = displaced ? kDisplaced : kFlat;
        range.firstIndex = static_cast<std::uint32_t>(indices[segment].size());
        range.firstEdgeIndex = static_cast<std::uint32_t>(batch.edges.size());
        std::vector<DVec3> loop;
        for (const auto index : face.vertices) loop.push_back(polyhedron->vertices[index]);
        if (displaced) {
            // Smooth normals, area-weighted: each triangle adds its unnormalised cross product.
            const auto& positions = displaced->positions;
            std::vector<DVec3> normals(positions.size(), DVec3(0.0));
            for (const auto& t : displaced->triangles) {
                const DVec3 n = glm::cross(positions[t[1]] - positions[t[0]], positions[t[2]] - positions[t[0]]);
                for (const auto k : t) normals[k] += n;
            }
            const auto base = static_cast<std::uint32_t>(batch.vertices.size());
            for (std::size_t i = 0; i < positions.size(); ++i) {
                // VBSP and the engine interpolate the base face's corner texcoords over the grid, which
                // equals the side's texture projection at the flat point: cliffs stretch, as in game.
                const DVec3 flat = positions[i] - disp->normals[i] * disp->distances[i] - disp->offsets[i];
                const double length = glm::length(normals[i]);
                BrushVertex v = vertexAt(positions[i], flat, length > 0.0 ? normals[i] / length : face.plane.normal);
                v.alpha = static_cast<float>(std::clamp(displaced->alphas[i], 0.0, 255.0) / 255.0);
                batch.vertices.push_back(v);
                mesh.bounds.expand(positions[i]);
            }
            std::set<std::pair<std::uint32_t, std::uint32_t>> edges;
            for (const auto& t : displaced->triangles) {
                indices[kDisplaced].insert(indices[kDisplaced].end(), {base + t[0], base + t[1], base + t[2]});
                for (std::size_t k = 0; k < 3; ++k) {
                    const auto a = t[k], b = t[(k + 1) % 3];
                    edges.insert({std::min(a, b), std::max(a, b)});
                }
            }
            for (const auto& [a, b] : edges) batch.edges.insert(batch.edges.end(), {base + a, base + b});
            // The flat base face stays a caster (and in the scene BVH) only; it is never drawn.
            emit(loop, {}, true, {}, kCasterOnly);
        } else {
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
        }
        range.indexCount = static_cast<std::uint32_t>(indices[segment].size()) - range.firstIndex;
        range.edgeIndexCount = static_cast<std::uint32_t>(batch.edges.size()) - range.firstEdgeIndex;
        batch.faces.push_back(range);
        rangeSegments[it->second].push_back(segment);
    }
    } catch (const std::exception& e) { // uvAt rejects invalid UV data.
        return fail(std::string("invalid face data: ") + e.what());
    }
    for (std::size_t b = 0; b < mesh.batches.size(); ++b) {
        BatchMesh& batch = mesh.batches[b];
        const auto& parts = segments[b];
        const auto displacedCount = static_cast<std::uint32_t>(parts[kDisplaced].size());
        for (std::size_t f = 0; f < batch.faces.size(); ++f)
            if (rangeSegments[b][f] == kFlat) batch.faces[f].firstIndex += displacedCount;
        for (const auto& part : parts) batch.indices.insert(batch.indices.end(), part.begin(), part.end());
        batch.drawIndexCount = displacedCount + static_cast<std::uint32_t>(parts[kFlat].size());
        batch.casterFirstIndex = displacedCount;
    }
    return mesh;
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
    Displacements displacements;
    displacements.sides = document::displacedSides(solidBlock);
    for (const auto* side : solidBlock.findAll("side")) {
        if (!document::isDisplaced(*side)) continue;
        const auto* id = side->find("id");
        const auto value = id ? id->asInt() : Result<int>(std::unexpected(Error{ErrorCode::ParseError, "no id"}));
        if (!value) continue;
        // A dispinfo that does not parse draws flat; it must not fail the solid.
        if (auto parsed = document::parseDisplacement(*side->find("dispinfo")))
            displacements.parsed.emplace(static_cast<document::SideId>(*value), std::move(*parsed));
    }
    return meshPolyhedron(geometry->poly, sizeOf, lightmaps, solidId, origin, displacements);
}

Result<SolidMesh> meshSolid(const geometry::ConvexPolyhedron& poly,
                            const std::function<TextureSize(std::string_view)>& sizeOf,
                            const LightmapAtlas* lightmaps, std::uint64_t solidId,
                            const document::InstanceSolidOrigin* origin) {
    return meshPolyhedron(poly, sizeOf, lightmaps, solidId, origin, {});
}

} // namespace apex::render
