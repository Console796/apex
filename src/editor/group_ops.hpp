#pragma once

#include "editor/group_index.hpp"
#include "editor/selection.hpp"

#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace apex::editor {

// Structural editing on top of Document: delete, group, ungroup, tie to entity and move to
// world. Each stage* function writes into a caller-owned open transaction, so a failure
// leaves the document untouched once the transaction is dropped. The GroupIndex must reflect
// the document as it was before the transaction began; nothing here reads it mid-edit.
// Groups stay flat VMF `group` blocks in `world`; membership is `editor.groupid` (D10).

// What an edit created or reshaped, used to compute the selection afterwards.
struct EditedObjects {
    struct Item {
        enum class Kind { Solid, Entity, Group };
        NodeId node = 0;
        Kind kind = Kind::Solid;
        std::vector<NodeId> solids;  // An entity's solids, or the solid itself.
        std::optional<NodeId> group; // Immediate group.
    };
    std::vector<Item> items;
    // Parent group of every group an item can reach (nullopt for an outermost group).
    std::map<NodeId, std::optional<NodeId>> groupParents;
};

// The selection to show after an edit, in the current selection's mode: Groups (without
// ignore-groups) selects each item's outermost group or the item itself, Objects selects
// entities and world solids, Solids selects every solid, and Faces falls back to Objects.
Selection selectionFor(const Selection& current, const EditedObjects& objects);

struct TieParams {
    // Class of the new brush entity when `entity` is empty. func_detail is the default.
    std::string className = "func_detail";
    // Tie to this existing entity instead of creating one. The caller checks that its class
    // takes brushes; nothing here has the FGD.
    std::optional<NodeId> entity;
    // Extra key values for a new entity (classname is set from className).
    std::vector<std::pair<std::string, std::string>> keyValues;
};

// Selection targets that are document nodes (empty in Faces mode).
std::vector<NodeId> selectedNodes(const Selection& selection);

// Deletes the selected groups (with every member, recursively), entities and solids. A brush
// entity left without solids, and a group left without members, are deleted with them.
Result<EditedObjects> stageDelete(const document::Document& doc, const GroupIndex& groups,
                                  const Selection& selection, document::Transaction& tx);
// Wraps the selection in one new group. Existing groups join as members unless the
// selection ignores groups, where the selected objects themselves move into the new group.
Result<EditedObjects> stageGroup(const document::Document& doc, const GroupIndex& groups,
                                 const Selection& selection, document::Transaction& tx);
// Dissolves one level: each selected group (or the outermost group of each selected object).
Result<EditedObjects> stageUngroup(const document::Document& doc, const GroupIndex& groups,
                                   const Selection& selection, document::Transaction& tx);
// Moves the selected solids into a new or existing brush entity.
Result<EditedObjects> stageTieToEntity(const document::Document& doc, const GroupIndex& groups,
                                       const Selection& selection, const TieParams& params,
                                       document::Transaction& tx);
// Moves the selected brush-entity solids (or all solids of selected brush entities) into
// the world, deleting entities that end up empty.
Result<EditedObjects> stageMoveToWorld(const document::Document& doc, const GroupIndex& groups,
                                       const Selection& selection, document::Transaction& tx);

Result<EditedObjects> stageMoveNodesInto(const document::Document& doc, const GroupIndex& groups,
                                          std::vector<NodeId> nodes, NodeId container,
                                          document::Transaction& tx);
Result<EditedObjects> stageRenameEntity(const document::Document& doc, NodeId entity,
                                        std::string_view targetname, document::Transaction& tx);

// Shared with the clipboard codec.
std::string groupColor(int vmfId);
// The non-world entity that owns a solid, or nullopt for a world solid.
std::optional<NodeId> brushEntityOf(const document::Document& doc, NodeId solid);
std::optional<document::NodeHandle> worldHandle(const document::Document& doc);
std::optional<int> vmfId(const document::Document& doc, NodeId node);
// Solids below a node, including those inside hidden wrappers.
std::vector<NodeId> solidsOf(const document::Document& doc, NodeId node);

} // namespace apex::editor
