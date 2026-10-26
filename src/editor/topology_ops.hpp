#pragma once

#include "core/result.hpp"
#include "editor/selection.hpp"
#include "geometry/convex_polyhedron.hpp"

namespace apex::editor {
class EditorSession;

bool hasDisplacement(const kv1::Node& block);
struct MergePlan {
    geometry::ConvexPolyhedron hull;
    std::vector<geometry::ConvexPolyhedron> ghosts;
    std::vector<Handle<document::Solid>> inputs;
    double addedVolume = 0;
    bool convex() const { return ghosts.empty(); }
};
Result<void> carve(EditorSession& session);
Result<void> hollow(EditorSession& session, double thickness);
Result<MergePlan> planMerge(const EditorSession& session);
Result<void> commitMerge(EditorSession& session, const MergePlan& plan);
} // namespace apex::editor
