#include "lighting/bake_scene.hpp"

#include "core/strings.hpp"
#include "document/document.hpp"
#include "document/entity_transform.hpp"

#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace apex::lighting {

bool BakeOcclusion::reachesSky(DVec3 point, DVec3 direction) const {
    if (!accel) return true;
    const auto hit = accel->closest(Vec3(point), Vec3(direction), std::numeric_limits<float>::max());
    return !hit || (isSky && isSky(*hit));
}

bool BakeOcclusion::visible(DVec3 point, DVec3 endpoint) const {
    const DVec3 delta = endpoint - point;
    const double distance = glm::length(delta);
    if (!accel || distance <= 0) return true;
    // Do not include the emitter endpoint in the segment. Samples already have the VRAD unit lift.
    const float reach = std::nextafter(static_cast<float>(distance), 0.0f);
    return !accel->occluded(Vec3(point), Vec3(delta / distance), reach);
}

std::vector<BakeFace> bakeFaces(const geometry::Face& face, std::span<const DVec3> vertices,
                                std::uint32_t material, Vec3 albedo) {
    std::vector<BakeFace> result;
    for (auto& fragment : chartFace(face, vertices).fragments) {
        BakeFace built;
        built.vertices = std::move(fragment.vertices);
        built.normal = fragment.face.plane.normal;
        built.chart = fragment.chart;
        built.material = material;
        built.albedo = albedo;
        built.sideId = face.sideId;
        result.push_back(std::move(built));
    }
    return result;
}

Result<BakeScene> bakeSceneFromDocument(
    const document::Document& document, BakeOcclusion occlusion,
    const std::function<BakeMaterial(std::string_view)>& material,
    const assets::EntityClassTable* classes) {
    BakeScene scene;
    scene.occlusion = std::move(occlusion);
    std::map<std::string, std::uint32_t> materials;
    std::map<document::NodeId, const document::Entity*> entities;
    std::vector<const document::Solid*> solids;
    const auto walk = [&](const auto& self, document::NodeId id) -> void {
        const auto handle = document.handle(id);
        if (!handle) return;
        if (const auto* entity = std::get_if<Handle<document::Entity>>(&*handle))
            entities.emplace(id, document.get(*entity));
        if (const auto* solid = std::get_if<Handle<document::Solid>>(&*handle))
            solids.push_back(document.get(*solid));
        for (const auto child : document.get(*handle)->children) self(self, child);
    };
    for (const auto root : document.roots()) walk(walk, root);

    for (const auto* solid : solids) {
        auto shape = geometry::solidGeometry(*solid->block);
        if (!shape) return std::unexpected(shape.error().withContext("bake solid " + std::to_string(solid->id)));
        std::set<std::uint32_t> displacements;
        for (const auto* side : solid->block->findAll("side")) {
            if (const auto* id = side->find("id"); id && side->find("dispinfo"))
                if (const auto value = id->asInt()) displacements.insert(static_cast<std::uint32_t>(*value));
        }
        for (const auto& face : shape->poly.faces) {
            const auto name = toLowerAscii(face.material);
            const auto flags = material ? material(name) : BakeMaterial{};
            if (flags.sky || flags.nodraw || flags.noLight || name.starts_with("tools/toolsskybox") ||
                name == "tools/toolsnodraw" || name == "tools/toolstrigger" ||
                name == "tools/toolsclip" || name == "tools/toolsplayerclip" ||
                name == "tools/toolsnpcclip" || displacements.contains(face.sideId)) continue;
            const auto [it, inserted] = materials.try_emplace(name, static_cast<std::uint32_t>(scene.materials.size()));
            if (inserted) scene.materials.push_back(name);
            try {
                auto faces = bakeFaces(face, shape->poly.vertices, it->second, flags.albedo);
                for (auto& built : faces) {
                    built.solidId = solid->id;
                    scene.faces.push_back(std::move(built));
                }
            } catch (const std::invalid_argument& error) {
                return std::unexpected(Error{ErrorCode::InvalidArgument, error.what()}
                    .withContext("bake side " + std::to_string(face.sideId)));
            }
        }
    }

    std::map<std::string, DVec3> targets;
    const auto text = [](const kv1::Node& block, std::string_view key) {
        const auto* value = block.find(key);
        return value ? value->decodedValue().value_or("") : std::string{};
    };
    for (const auto& [id, entity] : entities) {
        const auto name = toLowerAscii(text(*entity->block, "targetname"));
        if (!name.empty()) targets.try_emplace(name, document::entityPose(*entity->block).origin);
    }
    for (const auto& [id, entity] : entities) {
        const auto& block = *entity->block;
        const auto target = targets.find(toLowerAscii(text(block, "target")));
        const auto position = target == targets.end() ? std::nullopt : std::optional(target->second);
        const auto* cls = classes ? classes->find(text(block, "classname")) : nullptr;
        auto lights = cls ? lightsFromEntity(block, *cls, position) : lightsFromEntity(block, position);
        std::uint64_t part = 0;
        for (auto& light : lights) {
            light.id = (id << 3) | ++part;
            scene.lights.push_back(std::move(light));
        }
    }
    return scene;
}

} // namespace apex::lighting
