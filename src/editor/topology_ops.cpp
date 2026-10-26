#include "editor/topology_ops.hpp"

#include "editor/editor_session.hpp"
#include "editor/face_edit.hpp"
#include "editor/group_ops.hpp"
#include "editor/selection_expand.hpp"
#include "editor/visibility.hpp"
#include "geometry/solid_geometry.hpp"
#include "geometry/face_matching.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <iomanip>

namespace apex::editor {
namespace {
using Poly = geometry::ConvexPolyhedron;
using SolidHandle = Handle<document::Solid>;
auto failure(std::string message) { return std::unexpected(Error{ErrorCode::InvalidArgument, std::move(message)}); }
struct Input { SolidHandle handle; Poly poly; };
Result<std::vector<Input>> inputs(const EditorSession& session) {
    if (!session.document()) return failure("No document");
    const auto& doc = *session.document();
    auto nodes = expandSelection(doc, session.groups(), session.selection()).solidsAndEntities;
    std::vector<Input> result;
    for (const auto node : nodes) {
        const auto h = doc.handle(node);
        if (!h || !std::holds_alternative<SolidHandle>(*h)) continue;
        const auto handle = std::get<SolidHandle>(*h);
        const auto& block = *doc.get(handle)->block;
        if (hasDisplacement(block)) return failure("Displacement solids are read-only for topology tools");
        const auto poly = geometry::solidGeometry(block);
        if (!poly) return failure("Cannot edit invalid solid geometry");
        result.push_back({handle, poly->poly});
    }
    if (result.empty()) return failure("Select solids first");
    return result;
}
void capAttributes(Poly& piece, const Poly& source) {
    for (auto& face : piece.faces) if (face.isCap) {
        const auto old = face.carverFace ? source.faces.begin() + *face.carverFace :
            std::ranges::max_element(source.faces, {}, [&](const auto& f) { return glm::dot(face.plane.normal, f.plane.normal); });
        static_cast<geometry::FaceAttributes&>(face) = *old;
        face.sideId = 0;
        face.carverFace.reset();
    }
}
std::vector<Poly> subtract(const std::vector<Poly>& pieces, const Poly& carver) {
    std::vector<Poly> result;
    for (const auto& piece : pieces) {
        auto fragments = geometry::subtract(piece, carver);
        for (auto& fragment : fragments) {
            // Only newly cut faces belong to this carver.
            for (auto& face : fragment.faces) if (face.carverFace) {
                static_cast<geometry::FaceAttributes&>(face) = carver.faces[*face.carverFace];
                face.sideId = 0;
                face.carverFace.reset();
            }
            result.push_back(std::move(fragment));
        }
    }
    return result;
}
std::string axisText(DVec3 axis, double shift, double scale) {
    std::ostringstream out;
    out << std::setprecision(17) << '[' << axis.x << ' ' << axis.y << ' ' << axis.z << ' ' << shift << "] " << scale;
    return out.str();
}
// Rebuild preserves source side blocks. Copy explicit operation attributes after it has
// allocated side IDs, matching by plane so source side ordering is immaterial.
void attributes(document::Transaction& tx, const document::Document& doc, SolidHandle solid, const Poly& poly) {
    const auto written = geometry::solidGeometry(*doc.get(solid)->block).value();
    auto matched = poly;
    const auto matches = geometry::matchFaces(written.poly, matched);
    for (const auto& match : matches.matched) {
        const auto& side = written.poly.faces[match.oldIndex];
        const auto* face = &poly.faces[match.newIndex];
        if (face->material != side.material) tx.setSideKeyValue(static_cast<document::SideId>(side.sideId), "material", face->material);
        if (face->lightmapScale != side.lightmapScale)
            tx.setSideKeyValue(static_cast<document::SideId>(side.sideId), "lightmapscale", std::to_string(face->lightmapScale));
        if (face->smoothingGroups != side.smoothingGroups)
            tx.setSideKeyValue(static_cast<document::SideId>(side.sideId), "smoothing_groups", std::to_string(face->smoothingGroups));
        const auto& uv = face->uv;
        if (uv.uaxis != side.uv.uaxis || uv.shiftU != side.uv.shiftU || uv.scaleU != side.uv.scaleU)
            tx.setSideKeyValue(static_cast<document::SideId>(side.sideId), "uaxis", axisText(uv.uaxis, uv.shiftU, uv.scaleU));
        const bool u = uv.uaxis != side.uv.uaxis || uv.shiftU != side.uv.shiftU || uv.scaleU != side.uv.scaleU;
        const bool v = uv.vaxis != side.uv.vaxis || uv.shiftV != side.uv.shiftV || uv.scaleV != side.uv.scaleV;
        if (v) tx.setSideKeyValue(static_cast<document::SideId>(side.sideId), "vaxis", axisText(uv.vaxis, uv.shiftV, uv.scaleV));
        // D15: `rotation` follows the axes whenever they're written.
        if (u || v) if (auto rotation = derivedRotationText(uv, face->plane.normal, nullptr))
            tx.setSideKeyValue(static_cast<document::SideId>(side.sideId), "rotation", std::move(*rotation));
    }
}
void replace(document::Transaction& tx, const document::Document& doc, SolidHandle source, const std::vector<Poly>& pieces) {
    if (pieces.empty()) { tx.destroy(source); return; }
    for (std::size_t i = 1; i < pieces.size(); ++i) {
        const auto fragment = tx.createFragment(source, pieces[i]);
        attributes(tx, doc, fragment, pieces[i]);
    }
    tx.rebuildSolid(source, pieces.front());
    attributes(tx, doc, source, pieces.front());
}
}
bool hasDisplacement(const kv1::Node& block) {
    return std::ranges::any_of(block.findAll("side"), [](const auto* side) { return side->find("dispinfo"); });
}
Result<void> carve(EditorSession& session) {
    const auto carvers = inputs(session);
    if (!carvers) return std::unexpected(carvers.error());
    auto& doc = *session.document();
    std::set<document::NodeId> selected;
    for (const auto& input : *carvers) selected.insert(doc.get(input.handle)->id);
    std::vector<std::pair<SolidHandle, std::vector<Poly>>> edits;
    try {
        const auto walk = [&](const auto& self, document::NodeId node) -> Result<void> {
            const auto h = *doc.handle(node);
            if (const auto* solid = std::get_if<SolidHandle>(&h); solid && !selected.contains(node) && isVisible(doc, node, session.viewState())) {
                const auto source = geometry::solidGeometry(*doc.get(*solid)->block);
                if (!source) return failure("Cannot carve invalid solid geometry");
                std::vector<Poly> pieces{source->poly};
                for (const auto& carver : *carvers) pieces = subtract(pieces, carver.poly);
                double remaining = 0;
                for (const auto& p : pieces) remaining += p.volume();
                if (source->poly.volume() - remaining > 1e-6) {
                    if (hasDisplacement(*doc.get(*solid)->block)) return failure("Cannot carve a displacement target: topology is read-only");
                    edits.emplace_back(*solid, std::move(pieces));
                }
            }
            for (const auto child : doc.get(h)->children) if (auto r = self(self, child); !r) return r;
            return {};
        };
        for (const auto root : doc.roots()) if (auto r = walk(walk, root); !r) return r;
    } catch (const std::invalid_argument&) { return failure("Cannot carve: resulting geometry is degenerate"); }
    if (edits.empty()) return failure("No visible solids intersect the carvers");
    session.edit("Carve solids", [&](document::Transaction& tx) {
        for (const auto& [source, pieces] : edits) replace(tx, doc, source, pieces);
    });
    return {};
}
Result<void> hollow(EditorSession& session, double thickness) {
    if (!std::isfinite(thickness) || thickness == 0) return failure("Hollow thickness must be finite and nonzero");
    const auto selected = inputs(session);
    if (!selected) return std::unexpected(selected.error());
    std::vector<std::vector<Poly>> walls;
    try {
        for (const auto& input : *selected) {
            std::vector<geometry::PlaneSide> inner, outer;
            for (const auto& face : input.poly.faces) {
                geometry::PlaneSide side;
                static_cast<geometry::FaceAttributes&>(side) = face;
                side.plane = face.plane;
                outer.push_back(side);
                inner.push_back(side);
                if (thickness > 0) inner.back().plane.dist -= thickness;
                else outer.back().plane.dist -= thickness;
            }
            auto in = Poly::fromPlanes(inner), out = Poly::fromPlanes(outer);
            if (!in || !out) return failure("Hollow thickness leaves no valid interior");
            auto pieces = geometry::subtract(*out, *in);
            for (auto& piece : pieces) capAttributes(piece, *in); // carverFace indexes the inner hull.
            walls.push_back(std::move(pieces));
        }
    } catch (const std::invalid_argument&) { return failure("Cannot hollow: resulting geometry is degenerate"); }
    session.edit("Hollow solids", [&](document::Transaction& tx) {
        std::vector<Target> selection;
        for (std::size_t i = 0; i < selected->size(); ++i) {
            auto block = kv1::Node::block("group");
            block.append(kv1::Node::value("id", "0")).value();
            const auto group = tx.create(std::move(block), worldHandle(*session.document()));
            const auto node = session.document()->get(group)->id;
            const auto groupId = vmfId(*session.document(), node);
            tx.setEditor(group, document::EditorEdit::setColor(groupColor(*groupId)));
            const auto source = (*selected)[i].handle;
            for (std::size_t n = 1; n < walls[i].size(); ++n) {
                const auto fragment = tx.createFragment(source, walls[i][n]);
                attributes(tx, *session.document(), fragment, walls[i][n]);
                tx.setEditor(fragment, document::EditorEdit::setGroup(groupId));
            }
            tx.rebuildSolid(source, walls[i].front());
            attributes(tx, *session.document(), source, walls[i].front());
            tx.setEditor(source, document::EditorEdit::setGroup(groupId));
            selection.emplace_back(node);
        }
        session.selection().setMode(SelectionMode::Groups);
        session.selection().replace(selection);
    });
    return {};
}
Result<MergePlan> planMerge(const EditorSession& session) {
    const auto selected = inputs(session);
    if (!selected) return std::unexpected(selected.error());
    if (selected->size() < 2) return failure("Select at least two solids to merge");
    std::vector<DVec3> points;
    for (const auto& input : *selected) points.insert(points.end(), input.poly.vertices.begin(), input.poly.vertices.end());
    auto hull = geometry::convexHull(points);
    if (!hull) return failure("Cannot build a valid merge hull");
    std::set<std::size_t> inherited;
    for (const auto& input : *selected) {
        auto matched = *hull;
        const auto matches = geometry::matchFaces(input.poly, matched);
        for (const auto& match : matches.matched) if (inherited.insert(match.newIndex).second)
            static_cast<geometry::FaceAttributes&>(hull->faces[match.newIndex]) = matched.faces[match.newIndex];
    }
    for (std::size_t i = 0; i < hull->faces.size(); ++i) if (!inherited.contains(i)) {
        const auto donor = std::ranges::max_element(selected->front().poly.faces, {}, [&](const auto& f) {
            return glm::dot(hull->faces[i].plane.normal, f.plane.normal);
        });
        static_cast<geometry::FaceAttributes&>(hull->faces[i]) = *donor;
    }
    MergePlan plan;
    plan.hull = std::move(*hull);
    plan.ghosts = {plan.hull};
    try {
        for (const auto& input : *selected) {
            plan.inputs.push_back(input.handle);
            plan.ghosts = subtract(plan.ghosts, input.poly);
        }
    } catch (const std::invalid_argument&) { return failure("Cannot merge: resulting geometry is degenerate"); }
    std::erase_if(plan.ghosts, [](const auto& p) { return p.volume() <= 1e-6; });
    for (const auto& piece : plan.ghosts) plan.addedVolume += piece.volume();
    return plan;
}
Result<void> commitMerge(EditorSession& session, const MergePlan& plan) {
    if (!session.document() || plan.inputs.empty()) return failure("No merge inputs");
    for (const auto h : plan.inputs) {
        const auto* solid = session.document()->get(h);
        if (!solid || hasDisplacement(*solid->block)) return failure("Merge inputs changed or contain a displacement");
    }
    session.edit("Merge solids", [&](document::Transaction& tx) {
        tx.rebuildSolid(plan.inputs.front(), plan.hull);
        attributes(tx, *session.document(), plan.inputs.front(), plan.hull);
        for (std::size_t i = 1; i < plan.inputs.size(); ++i) tx.destroy(plan.inputs[i]);
        session.selection().setMode(SelectionMode::Solids);
        session.selection().select(session.document()->get(plan.inputs.front())->id);
    });
    return {};
}
} // namespace apex::editor
