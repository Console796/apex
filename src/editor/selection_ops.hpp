#pragma once

#include "editor/group_index.hpp"
#include "editor/pick_index.hpp"
#include "editor/selection.hpp"

namespace apex::editor {

struct Modifiers { bool ctrl = false, shift = false; };
using MarqueeMode = editor::Mode;

// Caller-owned gesture state. sameSpot certifies the pixel tolerance and unchanged
// camera; no screen coordinates are interpreted here. Candidate/mode/policy changes
// restart cycling. Targets are deduplicated so multiple brushes of one entity/group
// occupy one depth slot. Selected targets are skipped when another is available.
struct CycleState {
    std::optional<Ray> ray;
    std::vector<Target> candidates;
    std::size_t cursor = 0;
    SelectionMode mode = SelectionMode::Groups;
    bool ignoreGroups = false;
};

// Objects/Groups promote brushes through structural wrappers to the owning non-world
// entity. Groups then promote to the outermost group, represented by its own NodeId.
// Solids excludes point entities; Faces requires a hit SideId. Group policy applies
// only in Groups mode. Inputs normally come from the visible PickIndex.
std::optional<Target> resolveClick(const Hit& hit, SelectionMode mode,
                                   const GroupIndex& groups, bool ignoreGroups = false);
void click(Selection& selection, const PickIndex& picks, const GroupIndex& groups,
           Ray ray, Modifiers modifiers = {}, CycleState* cycle = nullptr, bool sameSpot = false);
std::optional<Target> hover(const PickIndex& picks, const GroupIndex& groups,
                            Ray ray, SelectionMode mode, bool ignoreGroups = false);

// Enclosed requires all visible brushes/point boxes resolving to an object/group to be
// inside; Touching accepts any. Hidden members do not veto; malformed visible brushes
// veto Enclosed because their geometry cannot be certified. Empty/unpickable groups
// are never marquee targets. Faces mode selects every side of each accepted brush;
// face-polygon marquee clipping is deferred. Ctrl wins over Shift when both are set.
void marquee(Selection& selection, const PickIndex& picks, const GroupIndex& groups,
             std::span<const Plane> frustum, MarqueeMode mode, Modifiers modifiers = {});
// Enumerate document objects rather than only valid pick geometry: select-all also
// includes visible malformed brushes. Faces enumerates their parseable side IDs.
// Invert replaces the selection with the visible universe minus selected targets.
void selectAll(Selection& selection, const PickIndex& picks, const GroupIndex& groups);
void selectNone(Selection& selection);
void invert(Selection& selection, const PickIndex& picks, const GroupIndex& groups);

} // namespace apex::editor
