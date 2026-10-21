#include "render/solid_mesh.hpp"

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
    auto geometry = geometry::solidGeometry(solidBlock);
    if (!geometry) return std::unexpected(geometry.error());
    const auto* polyhedron = &geometry->poly;

    SolidMesh mesh;
    mesh.bounds = polyhedron->bounds();
    mesh.origin = polyhedron->center();
    std::map<std::string, std::size_t> batchIndex;
    try {
    for (const geometry::Face& face : polyhedron->faces) {
        if (face.vertices.size() < 3) continue;
        const auto [it, inserted] = batchIndex.try_emplace(lower(face.material), mesh.batches.size());
        if (inserted) mesh.batches.push_back({it->first, {}, {}, {}});
        BatchMesh& batch = mesh.batches[it->second];
        const TextureSize size = sizeOf(it->first);
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
