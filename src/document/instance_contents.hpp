#pragma once

#include "document/document.hpp"
#include "document/instance_fixup.hpp"

#include <filesystem>
#include <map>
#include <vector>

namespace apex::document {

// Hammer's viewsettings/nInstanceVisibility, also srctools.vmf.StrataInstanceVisibility.
enum class InstanceVisibility { Hidden = 0, Tinted = 1, Normal = 2 };

// Identity of one placed instance, stable between the editor's InstanceExpansion and the bake's
// expansion: the top-level func_instance's NodeId in the map document, then the VMF entity ids of
// the func_instances nested inside each template, outermost first. Two placements of one file differ.
struct InstancePlacement {
    NodeId owner = 0;
    std::vector<int> nested;
    auto operator<=>(const InstancePlacement&) const = default;
};

// A solid of an expanded (collapsed) snapshot and the placement/template solid it was made from.
// Template ids are the VMF ids in the template file; `sides` maps template side id -> expanded SideId.
struct InstanceSolidOrigin {
    InstancePlacement placement;
    int templateSolid = 0;
    NodeId solid = 0;
    std::map<int, SideId> sides;
    DMat4 transform{1.0}; // Template-local to the expanded snapshot's space, as it was baked.
};

// Separate documents, never handles into the parent. Keys are fixed on an immutable copy;
// geometry stays template-local so placements can share the same GPU buffers.
struct InstanceContent {
    NodeId instance = 0;
    std::shared_ptr<const Document> templ;
    std::shared_ptr<const Document> keys;
    DMat4 transform{1.0};
    InstanceFixup fixup;
    std::size_t depth = 0;
    std::filesystem::path path;
    std::vector<int> nested; // Entity ids of the enclosing nested func_instances; empty at depth 1.
    InstancePlacement placement() const { return {instance, nested}; }
};

} // namespace apex::document
