#pragma once

// Plan 0.8 D14: Duplicate strings keyframe chains (rope keyframes by NextKey, path tracks by
// target: classes with the keyframe() or animator() helper), so a rope is extended by
// duplicating its last node and moving the copy.
// - One keyframe entity duplicated: the copy goes after the original in its chain. The original
//   points at the copy, and the copy at whatever the original pointed at. An unnamed original is
//   named <classname>_1 (or the next free number) first.
// - Several: the copies are a chain of their own. Each named copy gets the next free name after
//   its original's ("rope_3" -> "rope_4", "wire" -> "wire_2"), and copies that pointed at a
//   copied original point at its copy.

#include "core/entity_class.hpp"
#include "editor/group_ops.hpp"

#include <span>
#include <string>

namespace apex::editor {

// Inside Duplicate's transaction, after its paste. `originals` are the selected nodes that were
// copied (entities found below them too), `copies` what the paste created, and `names` the
// targetnames in use before the paste.
Result<void> stageKeyframeChain(const document::Document& doc, document::Transaction& tx,
                                const assets::EntityClassTable& classes, std::span<const NodeId> originals,
                                const EditedObjects& copies, std::span<const std::string> names);

} // namespace apex::editor
