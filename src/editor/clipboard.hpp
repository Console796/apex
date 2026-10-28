#pragma once

#include "editor/group_ops.hpp"
#include "editor/transform.hpp"

#include <string_view>
#include <vector>

namespace apex::editor {

// Clipboard codec (D12). The clipboard carries a world-less VMF fragment: top-level
// `visgroups`, `group`, `solid` and `entity` blocks with their full contents, so it also
// pastes into other maps and other tools. Qt and the system clipboard live in the UI layer.

enum class NamePolicy { None, Prefix, Suffix };

// Paste Special. Copy i (1..copies) is the previous copy (the placed paste for i = 1) moved
// by `offset` after rotating by `rotationDeg` (degrees about X, then Y, then Z, through the
// previous copy's bounds centre), so offsets and rotations accumulate like a stair or ring.
struct PasteSpecial {
    int copies = 1; // 1..kMaxCopies
    DVec3 offset{0.0};
    DVec3 rotationDeg{0.0};
    // One new group per copy holds that copy's top-level objects (nested above any groups
    // the fragment already carries).
    bool groupCopies = false;
    // Entity names: the affix is added in front of or behind every targetname of the pasted
    // set. "%d" in the affix becomes the copy number; with several copies and no "%d" the
    // number is added (prefix "copy_" gives "copy_2_door", suffix "_copy" gives "door_copy2").
    // A result that matches an existing name gets "_2", "_3", ... until it is unique.
    NamePolicy names = NamePolicy::None;
    std::string affix;
    // Off by default: `connections` and other references stay verbatim, so they keep
    // pointing at the original entities. On: references to an entity renamed in the same
    // copy (exact targetname matches in any key but id/classname/origin/angles/model/
    // spawnflags, and the target token of each connections entry) follow the rename.
    bool rewriteReferences = false;
    static constexpr int kMaxCopies = 256;
};

struct PasteOptions {
    // Keep the copied coordinates exactly.
    bool inPlace = false;
    // Otherwise the pasted bounds rest on `plane`, centred on `at` (default: the plane
    // origin), snapped like a translate drag. Both default to the session's work plane.
    std::optional<DVec3> at;
    std::optional<SurfacePlane> plane;
    std::optional<SnapSettings> snap;
    std::optional<bool> textureLock; // Default: the session setting, else on.
    std::optional<PasteSpecial> special;
    const assets::EntityClassTable* classes = nullptr;
};

// Copy: the selected groups (with every member), entities (with their solids) and solids.
// A brush entity's solids copy as the entity only when every one of its solids is selected;
// otherwise they copy as loose world solids. Faces mode is an error.
Result<std::string> serializeSelection(const document::Document& doc, const GroupIndex& groups,
                                       const Selection& selection);

// What a fragment looks like, for previews: the bounds that paste rests on its plane (solids and
// entity origins), and the face polygons of every solid (counter-clockwise from outside).
struct FragmentShape {
    AABB bounds;
    std::vector<std::vector<DVec3>> polygons;
};
Result<FragmentShape> fragmentShape(std::string_view text);

// Paste into the caller's open transaction: fresh object, side, group and visgroup IDs,
// `groupid`s remapped, visgroups matched by name (missing ones created), `connections`
// verbatim. Returns what was created, for the selection afterwards. A failure leaves the
// transaction half-written, so the caller drops it. A document without a `world` node
// receives pasted solids and groups as roots.
Result<EditedObjects> stagePaste(const document::Document& doc, document::Transaction& tx,
                                 std::string_view text, const PasteOptions& options);

} // namespace apex::editor
