#pragma once

// Plan 0.8 D5/D6: document edits behind the Displacement tool, Destroy displacement and the
// Face Properties displacement fields. Each call is one labelled, undoable transaction; nothing
// changes when it fails. The maths is in geometry/displacement_ops.hpp.

#include "document/document.hpp"
#include "geometry/displacement_ops.hpp"

#include <span>
#include <string>
#include <vector>

namespace apex::editor {

class EditorSession;

// A side's base corners (Face order) and its displacement, if any. Fails for an unknown side, a
// solid that does not build, or a base face that is not a quad.
struct SideDisplacement {
    document::SideId side = 0;
    std::array<DVec3, 4> corners{};
    std::optional<geometry::Displacement> disp;
};
Result<SideDisplacement> sideDisplacement(const document::Document& doc, document::SideId side);

// The sides a displacement command acts on: the selected faces in Faces mode, otherwise every
// side of the selected solids (including those inside selected entities and groups).
std::vector<document::SideId> displacementTargets(const EditorSession& session);

// What a command did: how many sides changed, and a line for the status bar that also names
// the sides it refused.
struct DisplacementEditReport {
    std::size_t changed = 0;
    std::string message;
};

// Create: a new flat displacement on each flat quad face. Non-quad faces are refused (VBSP
// cannot build them) and already displaced faces skipped, both named in the message; an error
// when no face qualifies.
Result<DisplacementEditReport> createDisplacements(EditorSession& session, std::span<const document::SideId> sides,
                                                   int power, double elevation);
// Destroy displacement: the displaced sides among `sides` turn flat (D5).
Result<DisplacementEditReport> destroyDisplacements(EditorSession& session, std::span<const document::SideId> sides);
// Sew the displaced sides among `sides` as one surface.
Result<DisplacementEditReport> sewDisplacements(EditorSession& session, std::span<const document::SideId> sides);
// Face Properties: set or clear flag bits (Displacement::kFlag*) on every displaced side.
Result<DisplacementEditReport> setDisplacementFlags(EditorSession& session, std::span<const document::SideId> sides,
                                                    int mask, bool on);
// Face Properties: a new power, as Hammer's attribute change. A flat, unpainted displacement is
// rebuilt as a new flat grid keeping its flags, elevation and start corner; a sculpted or painted
// one is resampled (geometry::resampleDisplacement), losslessly up and dropping detail down.
Result<DisplacementEditReport> setDisplacementPower(EditorSession& session, std::span<const document::SideId> sides,
                                                    int power);
// Whether setDisplacementPower would drop sculpted or painted detail (a lower power on a
// displacement that is not flat), so the caller can ask first.
bool displacementPowerLosesDetail(const document::Document& doc, std::span<const document::SideId> sides, int power);

} // namespace apex::editor
