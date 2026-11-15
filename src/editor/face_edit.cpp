#include "editor/face_edit.hpp"
#include "editor/editor_session.hpp"
#include "core/strings.hpp"
#include "document/displacement.hpp"
#include "document/vmf.hpp"
#include "editor/map_problems.hpp"
#include "geometry/solid_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <climits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace apex::editor {
std::optional<FaceSnapshot> faceSnapshot(const document::Document& doc, document::SideId id) {
    const auto location = doc.side(id);
    const auto* solid = location ? doc.get(location->solid) : nullptr;
    if (!solid || !solid->block) return {};
    const auto geometry = geometry::solidGeometry(*solid->block);
    if (!geometry) return {};
    const auto f = std::ranges::find(geometry->poly.faces, static_cast<std::uint32_t>(id), &geometry::Face::sideId);
    if (f == geometry->poly.faces.end()) return {};
    std::size_t index = 0;
    for (const auto& child : solid->block->children()) {
        if (child.kind() != kv1::NodeKind::Block || !ciEquals(child.key(), "side")) continue;
        if (index++ != location->faceIndex) continue;
        FaceSnapshot result{*f, {}, child};
        for (auto v : f->vertices) result.points.push_back(geometry->poly.vertices[v]);
        return result;
    }
    return {};
}
std::optional<std::string> derivedRotationText(const geometry::FaceUv& uv, DVec3 normal, const kv1::Node* stored) {
    double rotation = 0;
    try { rotation = geometry::deriveRotation(uv, normal); } catch (const std::invalid_argument&) { return {}; }
    if (stored && stored->asDouble().value_or(NAN) == rotation) return {};
    auto node = kv1::Node::value("", "");
    vmf::Value(&node).set(rotation).value();
    return node.decodedValue().value();
}
FaceProperties summarize(const document::Document& doc, std::span<const document::SideId> sides) {
    FaceProperties result;
    std::set<document::SideId> seen;
    bool rotationUnknown = false;
    for (auto id : sides) {
        if (!seen.insert(id).second) continue;
        const auto snapshot = faceSnapshot(doc, id);
        if (!snapshot) continue;
        const auto& f = snapshot->face;
        const auto merge = [&](auto& field, const auto& value) {
            if (!result.count) field = value;
            else if (field && *field != value) field.reset();
        };
        merge(result.material, f.material);
        merge(result.shiftU, f.uv.shiftU); merge(result.shiftV, f.uv.shiftV);
        merge(result.scaleU, f.uv.scaleU); merge(result.scaleV, f.uv.scaleV);
        // A U axis along the normal has no rotation; the field then shows as mixed.
        try { merge(result.rotation, geometry::deriveRotation(f.uv, f.plane.normal)); }
        catch (const std::invalid_argument&) { rotationUnknown = true; }
        merge(result.lightmapScale, f.lightmapScale); merge(result.smoothingGroups, f.smoothingGroups);
        ++result.count;
    }
    if (rotationUnknown) result.rotation.reset();
    return result;
}
namespace {
struct Write { document::SideId side; std::string key, value; };
using Writes = std::vector<Write>;
std::string axis(DVec3 a, double shift, double scale) {
    auto node = kv1::Node::value("", "");
    vmf::Value(&node).setGeometry(vmf::UvAxis{{a.x, a.y, a.z}, shift, scale}).value();
    return node.decodedValue().value();
}
void write(Writes& writes, const FaceSnapshot& old, std::string key, std::string value) {
    const auto* existing = old.keys.find(key);
    if (existing && existing->decodedValue().value_or("") == value) return;
    writes.push_back({static_cast<int>(old.face.sideId), std::move(key), std::move(value)});
}
void axes(Writes& writes, const FaceSnapshot& old, const geometry::FaceUv& uv) {
    const auto& before = old.face.uv;
    const bool u = uv.uaxis != before.uaxis || uv.shiftU != before.shiftU || uv.scaleU != before.scaleU;
    const bool v = uv.vaxis != before.vaxis || uv.shiftV != before.shiftV || uv.scaleV != before.scaleV;
    const auto start = writes.size();
    if (u) write(writes, old, "uaxis", axis(uv.uaxis, uv.shiftU, uv.scaleU));
    if (v) write(writes, old, "vaxis", axis(uv.vaxis, uv.shiftV, uv.scaleV));
    if (writes.size() == start) return;
    if (auto rotation = derivedRotationText(uv, old.face.plane.normal, old.keys.find("rotation")))
        write(writes, old, "rotation", std::move(*rotation));
}
std::size_t commit(EditorSession& session, const Writes& writes, std::string label, std::string token = {}) {
    if (writes.empty()) return 0;
    std::set<document::SideId> changed;
    session.edit(std::move(label), [&](document::Transaction& tx) {
        for (const auto& w : writes) { tx.setSideKeyValue(w.side, w.key, w.value); changed.insert(w.side); }
    }, std::move(token));
    return changed.size();
}
std::vector<FaceSnapshot> selected(const EditorSession& session) {
    std::vector<FaceSnapshot> faces;
    if (!session.document() || session.selection().mode() != SelectionMode::Faces) return faces;
    for (auto target : session.selection().ordered())
        if (auto f = faceSnapshot(*session.document(), std::get<document::SideId>(target))) faces.push_back(std::move(*f));
    return faces;
}
Error invalid(const std::exception& e) { return {ErrorCode::InvalidArgument, e.what()}; }
double numeric(const FaceValue& value) {
    return std::visit([](const auto& v) -> double {
        if constexpr (std::is_arithmetic_v<std::decay_t<decltype(v)>>) return static_cast<double>(v);
        else throw std::invalid_argument("Expected a numeric face value");
    }, value);
}
}
Result<std::size_t> EditorSession::editFaceGesture(FaceGesture gesture, double u, double v,
                                                    DVec3 activePivot, std::string token) {
    try {
        if (!std::isfinite(u) || !std::isfinite(v) ||
            (gesture == FaceGesture::Scale && (u <= 0 || v <= 0)))
            throw std::invalid_argument("Invalid face gesture value");
        const auto faces = selected(*this);
        const auto active = selection_.active();
        Writes writes;
        for (const auto& f : faces) {
            const auto [w, h] = faceTextureSize(f.face.material);
            DVec3 centroid{};
            for (const auto p : f.points) centroid += p;
            if (!f.points.empty()) centroid /= static_cast<double>(f.points.size());
            const auto pivot = active && std::get<document::SideId>(*active) == f.face.sideId ? activePivot : centroid;
            auto uv = f.face.uv;
            if (gesture == FaceGesture::Translate) uv = geometry::translateUv(uv, u, v, w, h);
            if (gesture == FaceGesture::Rotate) uv = geometry::rotateUvAbout(uv, f.face.plane.normal, pivot, u);
            if (gesture == FaceGesture::Scale) uv = geometry::scaleUvAbout(uv, pivot, u, v);
            axes(writes, f, uv);
        }
        return commit(*this, writes, "Edit face UVs", token.empty() ? std::string{} : "face:gesture:" + token);
    } catch (const std::exception& e) { return std::unexpected(invalid(e)); }
}
Result<std::size_t> EditorSession::setFaceField(FaceField field, FaceValue value, std::string token) {
    try {
        Writes writes;
        const auto faces = selected(*this);
        std::string key = token.empty() ? "" : "face:" + token + ":" + std::to_string(static_cast<int>(field)) + ":";
        for (const auto& f : faces) {
            if (!token.empty()) key += std::to_string(f.face.sideId) + ",";
            auto uv = f.face.uv;
            if (field == FaceField::Material) {
                const auto* material = std::get_if<std::string>(&value);
                if (!material || material->empty()) throw std::invalid_argument("Expected a material name");
                if (!ciEquals(*material, f.face.material)) write(writes, f, "material", *material);
                continue;
            }
            const double n = numeric(value);
            if (!std::isfinite(n)) throw std::invalid_argument("Face value must be finite");
            switch (field) {
            case FaceField::ShiftU: uv.shiftU = n; break;
            case FaceField::ShiftV: uv.shiftV = n; break;
            case FaceField::ScaleU: if (n == 0) throw std::invalid_argument("Scale must be nonzero"); uv.scaleU = n; break;
            case FaceField::ScaleV: if (n == 0) throw std::invalid_argument("Scale must be nonzero"); uv.scaleV = n; break;
            case FaceField::Rotation: {
                const auto delta = std::remainder(n - geometry::deriveRotation(uv, f.face.plane.normal), 360.0);
                if (std::abs(delta) > 1e-9) uv = geometry::rotateUv(uv, f.face.plane.normal, delta);
                break;
            }
            case FaceField::LightmapScale:
                if (n < 1 || n > INT_MAX || n != std::floor(n)) throw std::invalid_argument("Invalid lightmap scale");
                if (n != f.face.lightmapScale) write(writes, f, "lightmapscale", std::to_string(static_cast<int>(n)));
                break;
            case FaceField::SmoothingGroups:
                if (n < 0 || n > UINT32_MAX || n != std::floor(n)) throw std::invalid_argument("Invalid smoothing groups");
                if (n != f.face.smoothingGroups) write(writes, f, "smoothing_groups", std::to_string(static_cast<std::uint32_t>(n)));
                break;
            case FaceField::Material: break;
            }
            axes(writes, f, uv);
        }
        return commit(*this, writes, "Edit face field", key);
    } catch (const std::exception& e) { return std::unexpected(invalid(e)); }
}
Result<std::size_t> EditorSession::toggleFaceSmoothingGroup(std::uint32_t bit, bool enabled) {
    if (bit == 0 || (bit & (bit - 1)) != 0)
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Expected one smoothing-group bit"});
    Writes writes;
    for (const auto& f : selected(*this)) {
        const auto next = enabled ? f.face.smoothingGroups | bit : f.face.smoothingGroups & ~bit;
        if (next != f.face.smoothingGroups)
            write(writes, f, "smoothing_groups", std::to_string(next));
    }
    return commit(*this, writes, "Edit smoothing groups");
}
Result<std::size_t> EditorSession::justifyFaces(geometry::Justify mode, bool treatAsOne) {
    try {
        const auto faces = selected(*this);
        std::vector<DVec3> combined;
        if (treatAsOne) for (const auto& f : faces) combined.insert(combined.end(), f.points.begin(), f.points.end());
        Writes writes;
        for (const auto& f : faces) {
            const auto [w, h] = faceTextureSize(f.face.material);
            axes(writes, f, geometry::justify(f.face.uv, treatAsOne ? combined : f.points, mode, w, h));
        }
        return commit(*this, writes, "Justify faces");
    } catch (const std::exception& e) { return std::unexpected(invalid(e)); }
}
Result<std::size_t> EditorSession::justifyFaces(geometry::HorizontalAlignment horizontal,
                                                geometry::VerticalAlignment vertical, bool treatAsOne) {
    try {
        const auto faces = selected(*this);
        std::vector<DVec3> combined;
        if (treatAsOne) for (const auto& f : faces) combined.insert(combined.end(), f.points.begin(), f.points.end());
        Writes writes;
        for (const auto& f : faces) {
            const auto [w, h] = faceTextureSize(f.face.material);
            axes(writes, f, geometry::justify2(f.face.uv, treatAsOne ? combined : f.points,
                                                horizontal, vertical, w, h));
        }
        return commit(*this, writes, "Align faces");
    } catch (const std::exception& e) { return std::unexpected(invalid(e)); }
}
namespace {
// One face's seed from the click's: a hotspot click over several faces makes independent choices.
std::uint64_t faceSeed(std::uint64_t click, document::SideId side) {
    return click * 0x9e3779b97f4a7c15ull ^ (std::uint64_t(side) + 0x632be59bd9b4e019ull);
}
}
Result<std::size_t> EditorSession::hotspotFaces(bool alternate) {
    const auto faces = selected(*this);
    if (faces.empty()) return std::unexpected(Error{ErrorCode::InvalidArgument, "Select the faces to hotspot (face selection mode)"});
    const auto click = ++hotspotSeed_;
    Writes writes;
    std::size_t fitted = 0;
    std::string lastError;
    for (const auto& f : faces) {
        const auto rects = faceHotspots(f.face.material);
        if (rects.empty()) { lastError = f.face.material + " has no hotspot rects"; continue; }
        const auto fit = geometry::fitHotspot(f.face.uv, f.face.plane.normal, f.points, rects,
                                              {newSolidStyle_.textureScale, alternate, faceSeed(click, f.face.sideId)});
        if (!fit) { lastError = fit.error().what(); continue; }
        axes(writes, f, fit->uv);
        ++fitted;
    }
    if (fitted == 0) return std::unexpected(Error{ErrorCode::InvalidArgument, lastError});
    commit(*this, writes, "Hotspot faces");
    return fitted;
}
Result<bool> EditorSession::applyHotspotAt(Ray ray, const std::string& material, bool alternate) {
    if (!document_ || material.empty()) return false;
    const auto hit = picks().first(ray);
    const auto f = hit && hit->side ? faceSnapshot(*document_, *hit->side) : std::nullopt;
    if (!f) return false;
    const auto rects = faceHotspots(material);
    if (rects.empty()) return std::unexpected(Error{ErrorCode::InvalidArgument, material + " has no hotspot rects"});
    const auto fit = geometry::fitHotspot(f->face.uv, f->face.plane.normal, f->points, rects,
                                          {newSolidStyle_.textureScale, alternate, faceSeed(++hotspotSeed_, f->face.sideId)});
    if (!fit) return std::unexpected(fit.error());
    Writes writes;
    if (!ciEquals(material, f->face.material)) write(writes, *f, "material", material);
    axes(writes, *f, fit->uv);
    commit(*this, writes, "Apply hotspot");
    return true;
}
Result<std::size_t> EditorSession::alignFaces(FaceAlignment alignment) {
    try {
        Writes writes;
        for (const auto& f : selected(*this)) axes(writes, f, alignment == FaceAlignment::World ?
            geometry::alignWorld(f.face.uv, f.face.plane.normal) : geometry::alignFace(f.face.uv, f.face.plane.normal));
        return commit(*this, writes, "Align faces");
    } catch (const std::exception& e) { return std::unexpected(invalid(e)); }
}
Result<std::size_t> EditorSession::rotateFaces(double degrees, std::string token) {
    return setFaceField(FaceField::Rotation, degrees, std::move(token));
}
bool EditorSession::liftMaterialAt(Ray ray, bool alignSource) {
    if (!document_) return false;
    const auto hit = picks().first(ray);
    const auto f = hit && hit->side ? faceSnapshot(*document_, *hit->side) : std::nullopt;
    if (!f) return false;
    setNewSolidMaterial(f->face.material);
    if (alignSource) {
        selection_.setMode(SelectionMode::Faces);
        selection_.select(static_cast<document::SideId>(f->face.sideId));
    }
    return true;
}
Result<FaceApplyResult> EditorSession::applyMaterialWrapped(Ray ray) {
    if (!document_) return FaceApplyResult{};
    const auto hit = picks().first(ray);
    if (!hit || !hit->side) return FaceApplyResult{};
    const auto target = faceSnapshot(*document_, *hit->side);
    const auto active = selection_.mode() == SelectionMode::Faces ? selection_.active() : std::nullopt;
    const auto source = active ? faceSnapshot(*document_, std::get<document::SideId>(*active)) : std::nullopt;
    if (!target) return FaceApplyResult{};
    // Nothing to wrap from: a plain apply, as right click does.
    if (!source || source->face.sideId == target->face.sideId)
        return FaceApplyResult{applyMaterialAt(ray, newSolidStyle_.material), false};
    // The hinge is the line where the two planes meet, so faces needn't share an edge (Hammer's
    // apply with alignment works across a room). Parallel faces copy the alignment.
    const auto n1 = source->face.plane.normal, n2 = target->face.plane.normal;
    const auto direction = glm::cross(n1, n2);
    std::optional<std::pair<DVec3, DVec3>> hinge;
    if (glm::length(direction) > 1e-9) {
        const auto d1 = glm::dot(n1, source->points.front()), d2 = glm::dot(n2, target->points.front());
        const auto point = (glm::cross(n2, direction) * d1 + glm::cross(direction, n1) * d2) / glm::dot(direction, direction);
        hinge = std::pair{point, point + glm::normalize(direction) * 64.0};
    }
    try {
        const auto [w, h] = faceTextureSize(newSolidStyle_.material);
        bool fallback = false;
        geometry::FaceUv uv;
        if (faceExtrapolation_ == FaceExtrapolation::Projected) {
            if (const auto projected = geometry::projectUv(source->face.uv, n2)) uv = *projected;
            else {
                fallback = true;
                uv = hinge ? geometry::wrapUv(source->face.uv, n1, n2, hinge->first, hinge->second, w, h)
                           : source->face.uv;
            }
        } else if (faceExtrapolation_ == FaceExtrapolation::Match) {
            uv = geometry::matchUv(source->face.uv, n1, n2);
        } else {
            uv = hinge ? geometry::wrapUv(source->face.uv, n1, n2, hinge->first, hinge->second, w, h)
                       : source->face.uv;
        }
        Writes writes;
        axes(writes, *target, uv);
        if (!ciEquals(target->face.material, newSolidStyle_.material)) write(writes, *target, "material", newSolidStyle_.material);
        commit(*this, writes, "Apply material with alignment");
        return FaceApplyResult{true, fallback};
    } catch (const std::exception& e) { return std::unexpected(invalid(e)); }
}
DisplacementSummary summarizeDisplacements(const document::Document& doc, std::span<const document::SideId> sides) {
    DisplacementSummary summary;
    bool first = true;
    const auto merge = [](auto& field, auto value, bool initial) {
        if (initial) field = value;
        else if (field && *field != value) field.reset();
    };
    std::map<document::NodeId, std::optional<geometry::SolidGeometry>> solids;
    for (const auto id : sides) {
        const auto location = doc.side(id);
        const auto* solid = location ? doc.get(location->solid) : nullptr;
        if (!solid || !solid->block) continue;
        const kv1::Node* side = nullptr;
        std::size_t face = 0;
        for (const auto& child : solid->block->children()) {
            if (child.kind() != kv1::NodeKind::Block || !ciEquals(child.key(), "side")) continue;
            if (face++ == location->faceIndex) { side = &child; break; }
        }
        if (!side || !document::isDisplaced(*side)) continue;
        ++summary.displaced;
        auto [cached, inserted] = solids.try_emplace(solid->id);
        if (inserted) if (auto built = geometry::solidGeometry(*solid->block)) cached->second = std::move(*built);
        const auto* poly = cached->second ? &cached->second->poly : nullptr;
        for (auto& problem : displacementProblems(*solid->block, *side, poly, solid->id))
            summary.problems.push_back(std::move(problem.text));
        const auto disp = document::parseDisplacement(*side->find("dispinfo"));
        if (!disp) continue;
        ++summary.readable;
        merge(summary.power, disp->power, first);
        merge(summary.flags, disp->flags, first);
        summary.flagsAll = first ? disp->flags : summary.flagsAll & disp->flags;
        summary.flagsAny |= disp->flags;
        merge(summary.elevation, disp->elevation, first);
        for (const auto alpha : disp->alphas) {
            summary.alphaMin = summary.alphaMin ? std::min(*summary.alphaMin, alpha) : alpha;
            summary.alphaMax = summary.alphaMax ? std::max(*summary.alphaMax, alpha) : alpha;
        }
        first = false;
    }
    return summary;
}

std::vector<std::string> displacementFlagNames(int flags) {
    std::vector<std::string> names;
    const auto bits = static_cast<unsigned>(flags);
    for (unsigned bit = 0; bit < 32; ++bit) {
        const unsigned mask = 1u << bit;
        if (!(bits & mask)) continue;
        if (mask == unsigned(geometry::Displacement::kFlagNoPhys)) names.emplace_back("NoPhys");
        else if (mask == unsigned(geometry::Displacement::kFlagNoHull)) names.emplace_back("NoHull");
        else if (mask == unsigned(geometry::Displacement::kFlagNoRay)) names.emplace_back("NoRay");
        else {
            std::ostringstream text;
            text << "0x" << std::hex << mask;
            names.push_back(text.str());
        }
    }
    return names;
}

} // namespace apex::editor
