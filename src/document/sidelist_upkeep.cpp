#include "document/sidelist_upkeep.hpp"

#include "core/strings.hpp"
#include "document/overlay_fragments.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace apex::document {
namespace {

constexpr double kCoplanarCos = 1.0 - 1e-6;
constexpr double kCoplanarDistance = 0.01;

} // namespace

bool isSideListKey(const InstanceKeyTypes& types, std::string_view classname, std::string_view key) {
    if (types && types(classname, key) == InstanceKeyType::SideList) return true;
    if (ciEquals(key, "sides"))
        return ciEquals(classname, "info_overlay") || ciEquals(classname, "info_overlay_transition") ||
               ciEquals(classname, "env_cubemap");
    return ciEquals(key, "sides2") && ciEquals(classname, "info_overlay_transition");
}

std::vector<SideListEdit> sideListUpkeep(const Document& doc, const InstanceKeyTypes& types,
                                         std::span<const SideId> vanished,
                                         const std::map<SideId, SideLineage>& lineage) {
    std::vector<SideListEdit> edits;
    if (vanished.empty() && lineage.empty()) return edits;
    const std::set<SideId> gone(vanished.begin(), vanished.end());
    SurfaceLookup surfaces(doc);
    std::map<SideId, std::optional<OverlaySurface>> resolved;
    const auto surfaceOf = [&](SideId side) -> const std::optional<OverlaySurface>& {
        auto [it, inserted] = resolved.try_emplace(side);
        if (inserted) it->second = surfaces.surface(side);
        return it->second;
    };
    for (const auto id : entityNodes(doc)) {
        const auto handle = doc.handle(id);
        const auto* node = handle ? doc.get(*handle) : nullptr;
        if (!node || !node->block) continue;
        const auto& block = *node->block;
        const auto* classKey = block.find("classname");
        const std::string classname = classKey ? classKey->decodedValue().value_or("") : "";
        std::optional<Result<geometry::OverlayQuad>> quad; // Parsed on first need.
        for (const auto& value : block.children()) {
            if (value.kind() != kv1::NodeKind::Value || !isSideListKey(types, classname, value.key())) continue;
            // setKeyValue refuses a duplicated key; leave such a list as written.
            if (block.findAll(value.key()).size() != 1) continue;
            const auto listed = geometry::parseSideList(value.decodedValue().value_or(""));
            auto out = listed;
            std::set<int> present(listed.begin(), listed.end());
            for (const auto& [side, entry] : lineage) {
                if (present.contains(side)) continue;
                std::optional<Plane> plane;
                SideId current = side;
                for (int depth = 0; depth < 64; ++depth) {
                    const auto found = lineage.find(current);
                    if (found == lineage.end()) break;
                    if (present.contains(found->second.parent)) {
                        plane = found->second.parentPlane;
                        break;
                    }
                    current = found->second.parent;
                }
                if (!plane) continue;
                const auto& surface = surfaceOf(side);
                if (!surface || surface->polygon.empty()) continue;
                if (glm::dot(surface->normal, plane->normal) < kCoplanarCos ||
                    std::abs(glm::dot(plane->normal, surface->polygon[0]) - plane->dist) > kCoplanarDistance)
                    continue; // A cap or other new face: not a piece of the listed one.
                if (overlayKind(classname) == OverlayKind::Overlay) {
                    if (!quad) quad = geometry::parseOverlay(block);
                    if (*quad && !clipToSurface(**quad, *surface)) continue; // Outside the footprint.
                }
                out.push_back(side);
                present.insert(side);
            }
            std::erase_if(out, [&](int side) { return gone.contains(side); });
            if (out == listed) continue;
            edits.push_back({*handle, std::string(value.key()), geometry::sideListText(out)});
        }
    }
    return edits;
}

} // namespace apex::document
