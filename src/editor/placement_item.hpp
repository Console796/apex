#pragma once

#include "core/result.hpp"
#include "editor/placement.hpp"

#include <functional>
#include <optional>
#include <string>
#include <variant>

namespace apex::editor {

class EditorSession;

// What the Entity tool puts down (0.4 D13). Choosing an item never changes the tool.
//
// An entity class is placed as a bare point entity. A model is placed as a point entity of the
// session's model placement class (`prop_static` by default) with that `model` key. A prefab is
// opaque here: the Entity tool resolves where it lands and calls `place`.
struct ClassItem {
    std::string classname;
};
struct ModelItem {
    std::string model; // The indexed path, for example "models/props/barrel.mdl".
};
// `place` receives the surface placement the ghost showed (origin, and angles only when the
// session aligns to the surface) and runs one undoable edit that puts the prefab's contents
// there. `bounds` is the prefab's local box about its origin; it sizes the ghost and the
// resting support on the surface, and an absent box falls back to the usual 8-unit one.
using PrefabPlacer = std::function<Result<void>(EditorSession&, const EntityPlacement&)>;
struct PrefabItem {
    std::string path;
    std::optional<AABB> bounds;
    PrefabPlacer place;
};
using PlacementItem = std::variant<ClassItem, ModelItem, PrefabItem>;

// Where an item lands for one hover or click: the placement, and the box the ghost draws.
struct PlacementPlan {
    EntityPlacement placement;
    AABB bounds;
};

} // namespace apex::editor
