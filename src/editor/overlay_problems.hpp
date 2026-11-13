#pragma once

#include "editor/map_problems.hpp"

namespace apex::editor {

// Plan 0.8 D11 rule 4: an info_overlay or info_overlay_transition whose side lists name no face of
// the map is kept (its keys may still matter, and undo may bring faces back) and listed here as a
// warning, so the author can re-attach or delete it. VBSP drops such an overlay from the compile.
void overlayProblems(const ProblemContext& context, std::vector<Problem>& out);

} // namespace apex::editor
