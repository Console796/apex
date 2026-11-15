#include "editor/displacement_tool.hpp"

#include "core/strings.hpp"
#include "document/displacement.hpp"
#include "editor/displacement_edit.hpp"
#include "editor/editor_session.hpp"
#include "geometry/displacement.hpp"
#include "geometry/solid_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>

namespace apex::editor {
namespace {

using document::SideId;
using geometry::BrushFalloff;
using geometry::SculptBrush;
using geometry::SculptDirection;

constexpr int kModes = 4;
const std::vector<std::string> kModeNames{"Create", "Sew", "Sculpt", "Paint alpha"};
const std::vector<std::string> kBrushNames{"Raise/Lower", "Smooth", "Flatten", "Set height", "Noise"};
const std::vector<std::string> kFalloffNames{"Constant", "Linear", "Smooth"};
const std::vector<std::string> kDirectionNames{"Vertex normal", "Face normal", "X axis", "Y axis", "Z axis"};
constexpr double kMaxRadius = 4096.0;
constexpr Vec4 kBrushColour{1.0f, 0.65f, 0.15f, 1.0f};

int choice(double value, std::size_t count) {
    return std::clamp(static_cast<int>(std::lround(value)), 0, static_cast<int>(count) - 1);
}

bool brushing(DisplacementTool::Mode mode) {
    return mode == DisplacementTool::Mode::Sculpt || mode == DisplacementTool::Mode::PaintAlpha;
}

// Every solid in the document, in hierarchy order.
template <typename F> void forEachSolid(const document::Document& doc, F&& visit) {
    const auto walk = [&](const auto& self, NodeId id) -> void {
        const auto handle = doc.handle(id);
        const auto* node = handle ? doc.get(*handle) : nullptr;
        if (!node) return;
        if (std::holds_alternative<Handle<document::Solid>>(*handle) && node->block) visit(id, *node->block);
        for (const auto child : node->children) self(self, child);
    };
    for (const auto root : doc.roots()) walk(walk, root);
}

} // namespace

void DisplacementTool::activate() { session_.selection().setMode(SelectionMode::Faces); }

void DisplacementTool::setMode(Mode mode) {
    if (mode_ == mode) return;
    cancel();
    mode_ = mode;
    notice_.clear();
    cursor_.reset();
    session_.publishToolStatus();
}

void DisplacementTool::setNotice(std::string notice) {
    notice_ = std::move(notice);
    session_.publishToolStatus();
}

geometry::SculptSettings DisplacementTool::sculptSettings() const {
    geometry::SculptSettings settings;
    settings.brush = brush_;
    settings.falloff = falloff_;
    settings.direction = direction_;
    settings.radius = radius_;
    settings.amount = amount_;
    settings.strength = strength_;
    settings.height = height_;
    settings.seed = static_cast<std::uint32_t>(seed_);
    return settings;
}

geometry::PaintSettings DisplacementTool::paintSettings() const {
    return {falloff_, radius_, strength_, alpha_};
}

double DisplacementTool::dabSpacing(double radius) { return std::max(radius * 0.25, 1.0); }

bool DisplacementTool::setParameter(std::string_view id, double value) {
    if (!std::isfinite(value)) return false;
    if (id == "mode") {
        setMode(static_cast<Mode>(choice(value, kModes)));
        return true;
    }
    if (id == "power") power_ = std::clamp(static_cast<int>(std::lround(value)), geometry::Displacement::kMinPower,
                                           geometry::Displacement::kMaxPower);
    else if (id == "elevation") elevation_ = value;
    else if (id == "brush") brush_ = static_cast<SculptBrush>(choice(value, kBrushNames.size()));
    else if (id == "falloff") falloff_ = static_cast<BrushFalloff>(choice(value, kFalloffNames.size()));
    else if (id == "direction") direction_ = static_cast<SculptDirection>(choice(value, kDirectionNames.size()));
    else if (id == "radius") radius_ = std::clamp(value, 1.0, kMaxRadius);
    else if (id == "amount") amount_ = std::clamp(value, -1024.0, 1024.0);
    else if (id == "strength") strength_ = std::clamp(value, 0.0, 1.0);
    else if (id == "height") height_ = std::clamp(value, -65536.0, 65536.0);
    else if (id == "seed") seed_ = std::clamp(static_cast<int>(std::lround(value)), 0, 1 << 30);
    else if (id == "alpha") alpha_ = std::clamp(std::round(value), 0.0, 255.0);
    else return false;
    session_.publishToolStatus();
    return true;
}

bool DisplacementTool::perform(const ActionId& action) {
    if (action != "displacement.apply") return false;
    apply();
    return true;
}

bool DisplacementTool::apply() {
    if (!session_.document()) return false;
    if (brushing(mode_)) {
        setNotice("Sculpt and Paint alpha work by dragging over displacements");
        return false;
    }
    if (session_.selection().mode() != SelectionMode::Faces) {
        setNotice("Select faces first (face selection mode)");
        return false;
    }
    const auto sides = displacementTargets(session_);
    Result<DisplacementEditReport> report;
    switch (mode_) {
    case Mode::Create: report = createDisplacements(session_, sides, power_, elevation_); break;
    case Mode::Sew: report = sewDisplacements(session_, sides); break;
    default: return false;
    }
    if (!report) {
        setNotice(report.error().what());
        return false;
    }
    setNotice(report->message);
    return report->changed != 0;
}

bool DisplacementTool::prepare() {
    const auto* doc = session_.document();
    if (!doc) return false;
    if (stroke_ && preparedDocument_ == doc && preparedState_ == doc->historyState()) return true;
    stroke_.reset();
    surfaces_.clear();
    solidSurfaces_.clear();
    polys_.clear();
    std::vector<geometry::DisplacementPatch> patches;
    // Visible displacements only (hidden solids are not in the pick index). A dispinfo that does
    // not parse, or a base face that is not a quad, cannot be brushed and is left out.
    forEachSolid(*doc, [&](NodeId id, const kv1::Node& block) {
        const auto displaced = document::displacedSides(block);
        if (displaced.empty() || !session_.picks().bounds(id)) return;
        const auto geometry = geometry::solidGeometry(block);
        if (!geometry) return;
        bool used = false;
        for (const auto* side : block.findAll("side")) {
            const auto* key = side->find("id");
            const auto value = key ? key->asInt() : Result<int>(std::unexpected(Error{ErrorCode::ParseError, "no id"}));
            if (!value || !displaced.contains(static_cast<SideId>(*value))) continue;
            const auto face = std::ranges::find(geometry->poly.faces, static_cast<std::uint32_t>(*value), &geometry::Face::sideId);
            if (face == geometry->poly.faces.end()) continue;
            const auto corners = geometry::displacementCorners(geometry->poly, *face);
            auto disp = document::parseDisplacement(*side->find("dispinfo"));
            if (!corners || !disp || disp->problem()) continue;
            solidSurfaces_[id].push_back(surfaces_.size());
            surfaces_.push_back({static_cast<SideId>(*value), id});
            patches.push_back({*corners, std::move(*disp)});
            used = true;
        }
        if (used) polys_.emplace(id, geometry->poly);
    });
    auto stroke = geometry::DisplacementStroke::create(std::move(patches));
    if (!stroke) {
        surfaces_.clear();
        solidSurfaces_.clear();
        polys_.clear();
        setNotice(stroke.error().what());
        return false;
    }
    stroke_ = std::move(*stroke);
    preparedDocument_ = doc;
    preparedState_ = doc->historyState();
    return true;
}

void DisplacementTool::dab(DVec3 centre) {
    if (mode_ == Mode::Sculpt) {
        auto settings = sculptSettings();
        if (invert_ && settings.brush == SculptBrush::RaiseLower) settings.amount = -settings.amount;
        stroke_->sculpt(settings, centre);
    } else {
        auto settings = paintSettings();
        if (invert_) settings.alpha = 255.0 - settings.alpha;
        stroke_->paint(settings, centre);
    }
}

void DisplacementTool::publishPreview() {
    Preview::Replacements replacements;
    Preview::Displacements displacements;
    std::set<NodeId> solids;
    for (const auto p : stroke_->touched()) solids.insert(surfaces_[p].solid);
    for (const auto solid : solids) {
        replacements.emplace(solid, polys_.at(solid));
        auto& sides = displacements[solid];
        for (const auto p : solidSurfaces_.at(solid)) sides.emplace(surfaces_[p].side, stroke_->patch(p).disp);
    }
    preview_.setReplacements(replacements, displacements);
    // Viewports sync the preview when the session notifies, so the mesh follows the stroke.
    session_.publishToolStatus();
}

void DisplacementTool::commit() {
    stroking_ = false;
    auto* doc = session_.document();
    const auto touched = stroke_->touched();
    if (touched.empty() || !doc) {
        stroke_->revert();
        preview_.clear();
        return;
    }
    // The stroke's patches are this document state's; anything else would write the wrong sides.
    if (doc != preparedDocument_ || doc->historyState() != preparedState_) {
        stroke_.reset();
        preview_.clear();
        setNotice("The map changed during the stroke; it was not applied");
        return;
    }
    if (auto tagged = stroke_->finish(); !tagged) {
        stroke_->revert();
        preview_.clear();
        setNotice(tagged.error().what());
        return;
    }
    const bool sculpt = mode_ == Mode::Sculpt;
    session_.edit(sculpt ? "Sculpt displacement" : "Paint displacement alpha", [&](document::Transaction& tx) {
        for (const auto p : touched) tx.setDisplacement(surfaces_[p].side, stroke_->patch(p).disp);
    });
    preview_.clear();
    // Keep the working set for the next stroke: re-read only what this one wrote, as the document
    // now holds it, so the next stroke starts from the committed text.
    preparedState_ = doc->historyState();
    for (const auto p : touched) {
        auto written = document::displacement(*doc, surfaces_[p].side);
        if (!written || !*written || !stroke_->reset(p, std::move(**written))) {
            stroke_.reset();
            break;
        }
    }
    if (stroke_) stroke_->begin();
    notice_.clear();
    session_.publishToolStatus();
}

void DisplacementTool::cancel() {
    if (!stroking_) return;
    stroking_ = false;
    if (stroke_) stroke_->revert();
    preview_.clear();
    session_.publishToolStatus();
}

Handled DisplacementTool::strokeInput(const InputEvent& event, const ToolView& view) {
    // The viewport sends CancelPointer on every press a tool consumes (it drops selection gestures
    // only), so it must not end the stroke; Cancel and Esc do.
    if (event.kind == InputKind::CancelPointer) return false;
    if (event.kind == InputKind::Cancel || (event.kind == InputKind::KeyDown && event.key == "Escape")) {
        if (!stroking_) return false;
        cancel();
        return true;
    }
    if (event.kind == InputKind::Leave) {
        if (!cursor_) return false;
        cursor_.reset();
        return true;
    }
    if (event.kind == InputKind::Press && event.button == PointerButton::Left) {
        if (stroking_ || !prepare()) return true;
        const auto hit = stroke_->raycast(view.cursorRay);
        cursor_ = hit;
        if (!hit) {
            setNotice("Point at a displacement to " + std::string(mode_ == Mode::Sculpt ? "sculpt" : "paint"));
            return true;
        }
        // Selected displaced faces narrow the brush to them; with none, every displacement is brushed.
        std::set<SideId> selected;
        if (session_.selection().mode() == SelectionMode::Faces)
            for (const auto& target : session_.selection().ordered())
                if (const auto* side = std::get_if<SideId>(&target)) selected.insert(*side);
        std::vector<bool> editable(surfaces_.size(), true);
        if (std::ranges::any_of(surfaces_, [&](const Surface& s) { return selected.contains(s.side); }))
            for (std::size_t p = 0; p < surfaces_.size(); ++p) editable[p] = selected.contains(surfaces_[p].side);
        stroke_->setEditable(editable);
        stroke_->begin();
        stroking_ = true;
        invert_ = event.ctrl;
        strokeSource_ = event.source;
        last_ = hit->point;
        carry_ = 0.0;
        dab(hit->point);
        notice_.clear();
        publishPreview();
        return true;
    }
    if (event.kind == InputKind::Release && event.button == PointerButton::Left) {
        if (!stroking_ || event.source != strokeSource_) return stroking_;
        commit();
        return true;
    }
    if (event.kind != InputKind::Move) return false;
    if (!stroking_) {
        // The brush outline follows the cursor over displacements.
        if (event.held() || !prepare()) return false;
        const auto hit = stroke_->raycast(view.cursorRay);
        if (!hit && !cursor_) return false;
        cursor_ = hit;
        return true;
    }
    if (event.source != strokeSource_) return true;
    const auto hit = stroke_->raycast(view.cursorRay);
    if (!hit) return true; // Off the surface: the stroke resumes where the cursor comes back.
    cursor_ = hit;
    const double spacing = dabSpacing(radius_);
    const DVec3 from = last_;
    const double length = glm::distance(from, hit->point);
    double at = spacing - carry_;
    bool dabbed = false;
    for (; at <= length; at += spacing) {
        dab(from + (hit->point - from) * (at / length));
        dabbed = true;
    }
    carry_ = length - (at - spacing);
    last_ = hit->point;
    if (dabbed) publishPreview();
    return true;
}

Handled DisplacementTool::input(const InputEvent& event, const ToolView& view) {
    if (!session_.document() || event.navigating) return false;
    if (brushing(mode_)) return strokeInput(event, view);
    if (event.kind == InputKind::KeyDown && !event.autoRepeat && event.key == "Enter") {
        apply();
        return true;
    }
    if (event.kind != InputKind::Press || event.button != PointerButton::Left) return false;
    const auto hit = session_.picks().first(view.cursorRay);
    auto& selection = session_.selection();
    selection.setMode(SelectionMode::Faces);
    if (hit && hit->side) {
        if (event.ctrl) selection.toggle(*hit->side);
        else if (event.shift) selection.add(*hit->side);
        else selection.select(*hit->side);
    } else if (!event.ctrl && !event.shift) selection.clear();
    notice_.clear();
    session_.publishToolStatus();
    return true;
}

ToolOverlay DisplacementTool::overlay(const ToolView&, InputSource) const {
    ToolOverlay overlay;
    if (!brushing(mode_) || !cursor_) return overlay;
    // The brush's circle on the surface under the cursor.
    const DVec3 normal = cursor_->normal;
    const DVec3 helper = std::abs(normal.z) < 0.9 ? DVec3{0, 0, 1} : DVec3{1, 0, 0};
    const DVec3 u = glm::normalize(glm::cross(normal, helper)), v = glm::cross(normal, u);
    constexpr int kSegments = 48;
    const auto point = [&](int i) {
        const double angle = 2.0 * std::numbers::pi * i / kSegments;
        return cursor_->point + (u * std::cos(angle) + v * std::sin(angle)) * radius_;
    };
    for (int i = 0; i < kSegments; ++i) overlay.worldSegments.push_back({point(i), point(i + 1), kBrushColour, 1.5f});
    overlay.worldPoints.push_back({cursor_->point, kBrushColour, 5.0f});
    return overlay;
}

ToolReadout DisplacementTool::readout() const {
    ToolReadout readout;
    readout.badge = {"Displacement"};
    readout.detail = kModeNames[static_cast<std::size_t>(mode_)];
    readout.parameters.push_back({"mode", "Mode", ToolParameter::Kind::Choice, static_cast<double>(mode_), 0.0,
                                  double(kModes - 1), kModeNames});
    const auto number = [&](const char* id, const char* label, double value, double low, double high) {
        readout.parameters.push_back({id, label, ToolParameter::Kind::Number, value, low, high, {}});
    };
    const auto list = [&](const char* id, const char* label, int value, const std::vector<std::string>& names) {
        readout.parameters.push_back({id, label, ToolParameter::Kind::Choice, double(value), 0.0,
                                      double(names.size() - 1), names});
    };
    if (mode_ == Mode::Create) {
        readout.parameters.push_back({"power", "Power", ToolParameter::Kind::Integer, double(power_),
                                      double(geometry::Displacement::kMinPower), double(geometry::Displacement::kMaxPower), {}});
        number("elevation", "Elevation", elevation_, -65536.0, 65536.0);
    }
    if (mode_ == Mode::Sculpt) {
        list("brush", "Brush", static_cast<int>(brush_), kBrushNames);
        list("direction", "Direction", static_cast<int>(direction_), kDirectionNames);
    }
    if (brushing(mode_)) {
        number("radius", "Radius", radius_, 1.0, kMaxRadius);
        list("falloff", "Falloff", static_cast<int>(falloff_), kFalloffNames);
    }
    if (mode_ == Mode::Sculpt) {
        if (brush_ == SculptBrush::RaiseLower || brush_ == SculptBrush::Noise)
            number("amount", "Amount", amount_, -1024.0, 1024.0);
        else number("strength", "Strength", strength_, 0.0, 1.0);
        if (brush_ == SculptBrush::SetHeight) number("height", "Height", height_, -65536.0, 65536.0);
        if (brush_ == SculptBrush::Noise)
            readout.parameters.push_back({"seed", "Seed", ToolParameter::Kind::Integer, double(seed_), 0.0, double(1 << 30), {}});
    }
    if (mode_ == Mode::PaintAlpha) {
        number("strength", "Strength", strength_, 0.0, 1.0);
        readout.parameters.push_back({"alpha", "Alpha", ToolParameter::Kind::Integer, alpha_, 0.0, 255.0, {}});
    }
    std::string help;
    switch (mode_) {
    case Mode::Create: help = "Click faces (Ctrl toggles, Shift adds), then Enter to make them displacements; quad faces only."; break;
    case Mode::Sew: help = "Select neighbouring displacements, then Enter to sew their shared edges."; break;
    case Mode::Sculpt:
        help = std::string("Drag over displacements to sculpt") +
               (brush_ == SculptBrush::RaiseLower ? " (Ctrl lowers)" : "") +
               "; only the selected ones when displaced faces are selected. Esc cancels the stroke.";
        break;
    case Mode::PaintAlpha:
        help = "Drag over displacements to paint alpha (Ctrl paints 255 - alpha); only the selected ones when "
               "displaced faces are selected. Esc cancels the stroke.";
        break;
    }
    readout.status = notice_.empty() ? help : notice_ + " · " + help;
    return readout;
}

} // namespace apex::editor
