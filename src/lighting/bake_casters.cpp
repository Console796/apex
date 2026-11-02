#include "lighting/bake_casters.hpp"

#include "core/strings.hpp"
#include "document/document.hpp"

#include <map>

namespace apex::lighting {

Result<BakeOcclusion> bakeCastersFromDocument(
    const document::Document& document, std::span<const BakeFace> faces,
    const std::function<BakeMaterial(std::string_view)>& material) {
    struct Primitive { bool sky; std::optional<std::size_t> face; };
    auto primitives = std::make_shared<std::vector<Primitive>>();
    TriangleMesh mesh;
    const auto append = [&](std::span<const DVec3> loop, bool sky, std::optional<std::size_t> face) {
        const auto base = static_cast<std::uint32_t>(mesh.positions.size());
        for (const auto p : loop)
            mesh.positions.push_back({static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)});
        for (std::uint32_t i = 1; i + 1 < loop.size(); ++i) {
            mesh.indices.insert(mesh.indices.end(), {base, base + i, base + i + 1});
            primitives->push_back({sky, face});
        }
    };
    std::map<std::pair<std::uint64_t, std::uint32_t>, std::vector<std::size_t>> fragments;
    for (std::size_t i = 0; i < faces.size(); ++i)
        fragments[{faces[i].solidId, faces[i].sideId}].push_back(i);
    const auto walk = [&](const auto& self, document::NodeId id, bool eligible) -> Result<void> {
        const auto handle = *document.handle(id);
        const auto* node = document.get(handle);
        if (ciEquals(node->block->key(), "hidden")) return {};
        if (std::holds_alternative<Handle<document::Entity>>(handle)) {
            const auto* cls = node->block->find("classname");
            const auto name = cls ? toLowerAscii(cls->decodedValue().value_or("")) : "";
            eligible = name == "worldspawn" || name == "func_detail";
        }
        if (eligible && std::holds_alternative<Handle<document::Solid>>(handle)) {
            auto shape = geometry::solidGeometry(*node->block);
            if (!shape) return std::unexpected(shape.error());
            for (const auto& face : shape->poly.faces) {
                const auto name = toLowerAscii(face.material);
                const auto flags = material ? material(name) : BakeMaterial{};
                const bool sky = flags.sky || name.starts_with("tools/toolsskybox");
                const bool nodraw = flags.nodraw || name == "tools/toolsnodraw";
                if (name.starts_with("tools/") && !sky && !nodraw) continue;
                bool displacement = false;
                for (const auto* side : node->block->findAll("side"))
                    if (const auto* value = side->find("id"); value &&
                        value->asInt().value_or(-1) == static_cast<int>(face.sideId) && side->find("dispinfo"))
                        displacement = true;
                if (displacement) continue;
                const auto found = fragments.find({id, face.sideId});
                if (found != fragments.end()) {
                    for (const auto index : found->second) append(faces[index].vertices, sky, index);
                } else {
                    std::vector<DVec3> loop;
                    for (const auto index : face.vertices) loop.push_back(shape->poly.vertices[index]);
                    append(loop, sky, {});
                }
            }
        }
        for (const auto child : node->children)
            if (auto result = self(self, child, eligible); !result) return result;
        return {};
    };
    for (const auto root : document.roots())
        if (auto result = walk(walk, root, true); !result) return std::unexpected(result.error());
    std::vector<CpuInstance> instances;
    if (!mesh.indices.empty()) {
        auto blas = CpuBlas::build(mesh.positions, mesh.indices);
        if (!blas) return std::unexpected(blas.error());
        instances.push_back({*blas, {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}}, 0});
    }
    auto accel = CpuAccel::build(instances);
    if (!accel) return std::unexpected(accel.error());
    return BakeOcclusion{std::shared_ptr<const CpuAccel>(std::move(*accel)),
        [primitives](const RayHit& hit) { return primitives->at(hit.primitive).sky; },
        [primitives](const RayHit& hit) { return primitives->at(hit.primitive).face; }};
}

} // namespace apex::lighting
