#include "render/bake_setup.hpp"

#include "assets/vmt.hpp"
#include "assets/vtf.hpp"
#include "core/strings.hpp"
#include "document/document.hpp"
#include "lighting/bake_casters.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace apex::render {
using namespace lighting;
namespace {
template <typename T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().what());
    return std::move(*result);
}
std::string text(const kv1::Node& node, std::string_view key) {
    const auto* value = node.find(key);
    return value ? value->decodedValue().value_or("") : "";
}
}
BakeSetup setupBake(const document::Document& doc, const assets::Vfs* vfs) {
    std::map<std::string, BakeMaterial> materials;
    std::size_t failedMaterials = 0, failedTextures = 0;
    const auto material = [&](std::string_view name) -> BakeMaterial {
        const auto key = toLowerAscii(name);
        if (const auto found = materials.find(key); found != materials.end()) return found->second;
        BakeMaterial result;
        result.sky = key.starts_with("tools/toolsskybox");
        result.nodraw = key == "tools/toolsnodraw";
        result.noLight = key.starts_with("tools/") && !result.sky && !result.nodraw;
        if (vfs) {
            auto vmt = assets::loadMaterial(*vfs, key);
            if (!vmt) {
                ++failedMaterials;
                std::cerr << "material-failed " << key << ": " << vmt.error().what() << " (using fallback)\n";
            } else {
                const auto flag = [&](const char* name) {
                    const auto found = vmt->compileFlags.find(name);
                    return found != vmt->compileFlags.end() && !found->second.empty() && found->second != "0";
                };
                result.sky = result.sky || flag("%compilesky");
                result.nodraw = result.nodraw || flag("%compilenodraw");
                result.noLight = result.noLight || flag("%compilenolight");
                if (!vmt->baseTexture.empty()) {
                    auto texture = assets::loadTexture(*vfs, vmt->baseTexture);
                    if (texture) {
                        for (int c = 0; c < 3; ++c) result.albedo[c] = std::clamp(texture->reflectivity[c], 0.0f, 1.0f);
                    } else {
                        ++failedTextures;
                        std::cerr << "texture-failed " << vmt->baseTexture << ": " << texture.error().what() << " (albedo 0.5)\n";
                    }
                }
            }
        }
        materials.emplace(key, result);
        return result;
    };
    auto scene = take(bakeSceneFromDocument(doc, {}, material));
    std::set<document::NodeId> solids, entities;
    const auto scope = [&](const auto& self, document::NodeId id, bool eligible) -> void {
        const auto handle = *doc.handle(id);
        const auto* node = doc.get(handle);
        if (ciEquals(node->block->key(), "hidden")) return;
        if (std::holds_alternative<Handle<document::Entity>>(handle)) {
            entities.insert(id);
            const auto cls = toLowerAscii(text(*node->block, "classname"));
            eligible = cls == "worldspawn" || cls == "func_detail";
        }
        if (eligible && std::holds_alternative<Handle<document::Solid>>(handle)) solids.insert(id);
        for (const auto child : node->children) self(self, child, eligible);
    };
    for (const auto root : doc.roots()) scope(scope, root, true);
    std::erase_if(scene.faces, [&](const auto& face) { return !solids.contains(face.solidId); });
    std::erase_if(scene.lights, [&](const auto& light) { return !entities.contains(light.id >> 3); });
    scene.occlusion = take(bakeCastersFromDocument(doc, scene.faces, material));
    std::cout << "assets=" << (vfs ? "game VFS + addons" : "DISABLED: flat linear albedo 0.5, name flags")
              << " materials=" << materials.size() << " failed-materials=" << failedMaterials << " failed-textures=" << failedTextures << '\n';
    return {std::move(scene), failedMaterials, failedTextures};
}

} // namespace apex::render
