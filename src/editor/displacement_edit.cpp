#include "editor/displacement_edit.hpp"

#include "core/strings.hpp"
#include "document/displacement.hpp"
#include "editor/editor_session.hpp"
#include "editor/group_ops.hpp"
#include "geometry/solid_geometry.hpp"

#include <algorithm>
#include <set>

namespace apex::editor {
namespace {

using document::SideId;
using geometry::Displacement;

Error invalid(std::string message) { return {ErrorCode::InvalidArgument, std::move(message)}; }

std::string idList(const std::vector<SideId>& ids) {
    std::string text;
    for (std::size_t i = 0; i < ids.size() && i < 8; ++i) text += (i ? ", " : "") + std::to_string(ids[i]);
    if (ids.size() > 8) text += ", …";
    return text;
}

std::string plural(std::size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

using Writes = std::vector<std::pair<SideId, std::optional<Displacement>>>;

void commit(EditorSession& session, std::string label, const Writes& writes) {
    session.edit(std::move(label), [&](document::Transaction& tx) {
        for (const auto& [side, value] : writes) {
            if (value) tx.setDisplacement(side, *value);
            else tx.removeDisplacement(side);
        }
    });
}

// The displaced sides among `sides` that can be read and meshed, and the ones that cannot.
struct Displaced {
    std::vector<SideDisplacement> sides;
    std::vector<SideId> unreadable;
};
Displaced displaced(const document::Document& doc, std::span<const SideId> sides) {
    Displaced result;
    std::set<SideId> seen;
    for (const auto side : sides) {
        if (!seen.insert(side).second) continue;
        const auto found = document::displacement(doc, side);
        if (found && !found->has_value()) continue; // Flat.
        auto base = sideDisplacement(doc, side);
        if (!base || !base->disp) result.unreadable.push_back(side);
        else result.sides.push_back(std::move(*base));
    }
    return result;
}

std::string unreadableNote(const std::vector<SideId>& ids) {
    return ids.empty() ? "" : "; skipped " + plural(ids.size(), "displacement that cannot be read or meshed", "displacements that cannot be read or meshed") +
                                  " (" + idList(ids) + ")";
}

} // namespace

Result<SideDisplacement> sideDisplacement(const document::Document& doc, SideId side) {
    const auto location = doc.side(side);
    const auto* solid = location ? doc.get(location->solid) : nullptr;
    if (!solid || !solid->block) return std::unexpected(invalid("face " + std::to_string(side) + " is not in the map"));
    const auto geometry = geometry::solidGeometry(*solid->block);
    if (!geometry) return std::unexpected(geometry.error());
    const auto face = std::ranges::find(geometry->poly.faces, static_cast<std::uint32_t>(side), &geometry::Face::sideId);
    if (face == geometry->poly.faces.end()) return std::unexpected(invalid("face " + std::to_string(side) + " has no polygon"));
    const auto corners = geometry::displacementCorners(geometry->poly, *face);
    if (!corners) return std::unexpected(corners.error());
    auto disp = document::displacement(doc, side);
    if (!disp) return std::unexpected(disp.error());
    return SideDisplacement{side, *corners, std::move(*disp)};
}

std::vector<SideId> displacementTargets(const EditorSession& session) {
    std::vector<SideId> result;
    const auto* doc = session.document();
    if (!doc) return result;
    const auto& selection = session.selection();
    if (selection.mode() == SelectionMode::Faces) {
        for (const auto& target : selection.ordered())
            if (const auto* side = std::get_if<SideId>(&target)) result.push_back(*side);
        return result;
    }
    std::set<SideId> seen;
    const auto walk = [&](const auto& self, document::NodeId id) -> void {
        const auto handle = doc->handle(id);
        const auto* node = handle ? doc->get(*handle) : nullptr;
        if (!node) return;
        if (std::holds_alternative<Handle<document::Solid>>(*handle) && node->block) {
            for (const auto* side : node->block->findAll("side"))
                if (const auto* key = side->find("id"))
                    if (const auto value = key->asInt(); value && seen.insert(static_cast<SideId>(*value)).second)
                        result.push_back(static_cast<SideId>(*value));
        }
        for (const auto child : node->children) self(self, child);
    };
    for (const auto id : selectedNodes(selection)) walk(walk, id);
    return result;
}

Result<DisplacementEditReport> createDisplacements(EditorSession& session, std::span<const SideId> sides, int power,
                                                   double elevation) {
    const auto* doc = session.document();
    if (!doc) return std::unexpected(invalid("no map is open"));
    if (power < Displacement::kMinPower || power > Displacement::kMaxPower)
        return std::unexpected(invalid("displacement power must be 2, 3 or 4"));
    if (sides.empty()) return std::unexpected(invalid("Select the faces to displace (face selection mode)"));
    Writes writes;
    std::vector<SideId> nonQuad, already;
    std::set<SideId> seen;
    for (const auto side : sides) {
        if (!seen.insert(side).second) continue;
        const auto base = sideDisplacement(*doc, side);
        if (!base) {
            // displacement() parses even when the corners fail: a displaced non-quad is "already".
            if (const auto existing = document::displacement(*doc, side); !existing || existing->has_value())
                already.push_back(side);
            else nonQuad.push_back(side);
            continue;
        }
        if (base->disp) {
            already.push_back(side);
            continue;
        }
        auto created = geometry::createDisplacement(base->corners, power, elevation);
        if (!created) return std::unexpected(created.error());
        writes.emplace_back(side, std::move(*created));
    }
    std::string notes;
    if (!nonQuad.empty())
        notes += "; refused " + plural(nonQuad.size(), "face that is not a quad", "faces that are not quads") + " (" +
                 idList(nonQuad) + "): VBSP builds displacements only on four-sided faces";
    if (!already.empty())
        notes += "; skipped " + plural(already.size(), "face that is already displaced", "faces that are already displaced") +
                 " (" + idList(already) + ")";
    if (writes.empty()) return std::unexpected(invalid("Created no displacement" + notes));
    commit(session, "Create displacement", writes);
    return DisplacementEditReport{writes.size(), "Created " + plural(writes.size(), "displacement", "displacements") +
                                                     " at power " + std::to_string(power) + notes};
}

Result<DisplacementEditReport> destroyDisplacements(EditorSession& session, std::span<const SideId> sides) {
    const auto* doc = session.document();
    if (!doc) return std::unexpected(invalid("no map is open"));
    Writes writes;
    std::set<SideId> seen;
    for (const auto side : sides) {
        if (!seen.insert(side).second) continue;
        const auto location = doc->side(side);
        const auto* solid = location ? doc->get(location->solid) : nullptr;
        if (!solid || !solid->block) continue;
        std::size_t face = 0;
        for (const auto& child : solid->block->children()) {
            if (child.kind() != kv1::NodeKind::Block || !ciEquals(child.key(), "side")) continue;
            if (face++ != location->faceIndex) continue;
            // Unreadable dispinfo blocks are destroyed too: that is how a user gets rid of one.
            if (document::isDisplaced(child)) writes.emplace_back(side, std::nullopt);
            break;
        }
    }
    if (writes.empty()) return std::unexpected(invalid("No displaced face is selected"));
    commit(session, "Destroy displacement", writes);
    return DisplacementEditReport{writes.size(), "Destroyed " + plural(writes.size(), "displacement", "displacements") +
                                                     "; the faces are flat again"};
}

Result<DisplacementEditReport> sewDisplacements(EditorSession& session, std::span<const SideId> sides) {
    const auto* doc = session.document();
    if (!doc) return std::unexpected(invalid("no map is open"));
    const auto found = displaced(*doc, sides);
    if (found.sides.size() < 2) return std::unexpected(invalid("Select two or more displacements to sew" + unreadableNote(found.unreadable)));
    std::vector<geometry::DisplacementPatch> patches;
    for (const auto& side : found.sides) patches.push_back({side.corners, *side.disp});
    const auto changed = geometry::sewDisplacements(patches);
    if (!changed) return std::unexpected(changed.error());
    Writes writes;
    for (std::size_t i = 0; i < patches.size(); ++i)
        if (patches[i].disp != *found.sides[i].disp) writes.emplace_back(found.sides[i].side, patches[i].disp);
    if (writes.empty()) return DisplacementEditReport{0, "Already sewn" + unreadableNote(found.unreadable)};
    commit(session, "Sew displacements", writes);
    return DisplacementEditReport{writes.size(), "Sewed " + plural(writes.size(), "displacement", "displacements") +
                                                     unreadableNote(found.unreadable)};
}

Result<DisplacementEditReport> setDisplacementFlags(EditorSession& session, std::span<const SideId> sides, int mask,
                                                    bool on) {
    const auto* doc = session.document();
    if (!doc) return std::unexpected(invalid("no map is open"));
    Writes writes;
    std::set<SideId> seen;
    std::vector<SideId> unreadable;
    std::size_t displacedCount = 0;
    for (const auto side : sides) {
        if (!seen.insert(side).second) continue;
        // Flags need no base face: a displacement on a non-quad face can still have them set.
        const auto found = document::displacement(*doc, side);
        if (found && !found->has_value()) continue;
        ++displacedCount;
        if (!found) { unreadable.push_back(side); continue; }
        auto disp = **found;
        disp.flags = on ? disp.flags | mask : disp.flags & ~mask;
        if (disp.flags != (**found).flags) writes.emplace_back(side, std::move(disp));
    }
    if (displacedCount == 0) return std::unexpected(invalid("No displaced face is selected"));
    if (writes.empty()) return DisplacementEditReport{0, "Flags unchanged" + unreadableNote(unreadable)};
    commit(session, "Set displacement flags", writes);
    return DisplacementEditReport{writes.size(), "Set flags on " + plural(writes.size(), "displacement", "displacements") +
                                                     unreadableNote(unreadable)};
}

Result<DisplacementEditReport> setDisplacementPower(EditorSession& session, std::span<const SideId> sides, int power) {
    const auto* doc = session.document();
    if (!doc) return std::unexpected(invalid("no map is open"));
    if (power < Displacement::kMinPower || power > Displacement::kMaxPower)
        return std::unexpected(invalid("displacement power must be 2, 3 or 4"));
    const auto found = displaced(*doc, sides);
    if (found.sides.empty()) return std::unexpected(invalid("No displaced face is selected" + unreadableNote(found.unreadable)));
    std::size_t lowered = 0;
    Writes writes;
    for (const auto& side : found.sides) {
        const auto& old = *side.disp;
        if (old.power == power) continue;
        if (!geometry::displacementIsFlat(old)) {
            auto resampled = geometry::resampleDisplacement(side.corners, old, power);
            if (!resampled) return std::unexpected(resampled.error());
            if (power < old.power) ++lowered;
            writes.emplace_back(side.side, std::move(*resampled));
            continue;
        }
        auto rebuilt = geometry::createDisplacement(side.corners, power, old.elevation);
        if (!rebuilt) return std::unexpected(rebuilt.error());
        rebuilt->flags = old.flags;
        rebuilt->startPosition = old.startPosition;
        if (auto tagged = geometry::retagDisplacement(side.corners, *rebuilt); !tagged) return std::unexpected(tagged.error());
        writes.emplace_back(side.side, std::move(*rebuilt));
    }
    if (writes.empty()) return DisplacementEditReport{0, "Power unchanged" + unreadableNote(found.unreadable)};
    commit(session, "Set displacement power", writes);
    const std::string lost = lowered ? "; finer detail dropped on " + plural(lowered, "sculpted or painted displacement",
                                                                             "sculpted or painted displacements") : "";
    return DisplacementEditReport{writes.size(), "Set " + plural(writes.size(), "displacement", "displacements") +
                                                     " to power " + std::to_string(power) + lost +
                                                     unreadableNote(found.unreadable)};
}

bool displacementPowerLosesDetail(const document::Document& doc, std::span<const SideId> sides, int power) {
    const auto found = displaced(doc, sides);
    return std::ranges::any_of(found.sides, [&](const SideDisplacement& side) {
        return side.disp->power > power && !geometry::displacementIsFlat(*side.disp);
    });
}

} // namespace apex::editor
