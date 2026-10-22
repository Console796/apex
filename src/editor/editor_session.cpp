#include "editor/editor_session.hpp"

#include "core/strings.hpp"
#include "editor/selection_expand.hpp"
#include "editor/selection_undo.hpp"

namespace apex::editor {

EditorSession::EditorSession() : preset_(hammerPreset()), keymap_(preset_) {
    selection_.observe([this](const Selection&) { notify(); });
}
EditorSession::~EditorSession() { detach(); }

void EditorSession::attach(document::Document& doc) {
    detach();
    document_ = &doc;
    picks_ = std::make_unique<PickIndex>(doc);
    groups_ = std::make_unique<GroupIndex>(doc);
    documentObserver_ = doc.observe([this](const document::ChangeSet& changes) {
        picks_->apply(changes);
        groups_->apply(changes);
        cycle_ = {};
        hover_.reset();
        hoverDrawable_.reset();
        hoverFace_.reset();
        workPlane_.setHovered(std::nullopt);
        selection_.prune(*document_);
        notify();
    });
    notify();
}
void EditorSession::detach() {
    if (document_) document_->unobserve(documentObserver_);
    document_ = nullptr;
    picks_.reset();
    groups_.reset();
    cycle_ = {};
    view_ = {};
    hover_.reset();
    hoverDrawable_.reset();
    hoverFace_.reset();
    workPlane_ = {};
    selection_.clear();
    notify();
}
void EditorSession::setViewState(ViewState view) {
    view_ = std::move(view);
    if (picks_) picks_->setViewState(view_);
    cycle_ = {};
    hover_.reset();
    hoverDrawable_.reset();
    hoverFace_.reset();
    workPlane_.setHovered(std::nullopt);
    notify();
}
void EditorSession::clearHover() {
    if (!hover_ && !hoverDrawable_ && !hoverFace_) return;
    hover_.reset();
    hoverDrawable_.reset();
    hoverFace_.reset();
    workPlane_.setHovered(std::nullopt);
    notify();
}
void EditorSession::pointerHover(Ray ray) {
    std::optional<Target> target;
    std::optional<NodeId> drawable;
    std::optional<std::pair<NodeId, SideId>> face;
    if (picks_) {
        if (const auto hit = picks_->first(ray); hit && hit->side) face = {{hit->node, *hit->side}};
        for (const auto& hit : picks_->pick(ray)) {
            target = resolveClick(hit, selection_.mode(), *groups_, selection_.ignoreGroups());
            if (target) { drawable = hit.node; break; }
        }
    }
    const bool faceChanged = face != hoverFace_;
    if (target == hover_ && drawable == hoverDrawable_ && !faceChanged) return;
    hover_ = target;
    hoverDrawable_ = drawable;
    if (faceChanged) {
        hoverFace_ = face;
        workPlane_.setHovered(face ? planeFromFace(*document_, face->first, face->second) : std::nullopt);
    }
    notify();
}
void EditorSession::pinWorkPlane() {
    if (!hoverFace_) return;
    workPlane_.pinHovered();
    notify();
}
void EditorSession::pinAxisWorkPlane(Axis axis, DVec3 through) {
    workPlane_.pinAxisPlane(axis, through);
    notify();
}
void EditorSession::unpinWorkPlane() {
    if (!workPlane_.pinned()) return;
    workPlane_.unpin();
    notify();
}
void EditorSession::setGridSize(double units) {
    units = GridLadder::clamp(units);
    if (units == gridSize_) return;
    gridSize_ = units;
    notify();
}
void EditorSession::pointerClick(Ray ray, Modifiers modifiers, bool sameSpot) {
    if (picks_) click(selection_, *picks_, *groups_, ray, modifiers, &cycle_, sameSpot);
}
void EditorSession::marquee(std::span<const Plane> planes, Modifiers modifiers) {
    cycle_ = {};
    if (picks_) editor::marquee(selection_, *picks_, *groups_, planes, marqueeMode_, modifiers);
}
void EditorSession::selectNode(NodeId node, Modifiers modifiers) {
    if (!document_ || !document_->handle(node)) return;
    cycle_ = {};
    std::vector<Target> targets;
    const auto handle = *document_->handle(node);
    if (std::holds_alternative<Handle<document::Group>>(handle) &&
        selection_.mode() == SelectionMode::Groups && !selection_.ignoreGroups()) {
        targets.emplace_back(groups_->outermostGroup(node).value_or(node));
    } else {
        Selection source(SelectionMode::Objects);
        source.select(node);
        for (const auto drawable : expandSelection(*document_, *groups_, source).solidsAndEntities) {
            if (selection_.mode() == SelectionMode::Faces) {
                const auto drawableHandle = *document_->handle(drawable);
                if (!std::holds_alternative<Handle<document::Solid>>(drawableHandle)) continue;
                const auto* solid = document_->get(drawableHandle);
                for (const auto& side : solid->block->children()) {
                    if (!ciEquals(side.key(), "side")) continue;
                    if (const auto* number = side.find("id")) {
                        if (const auto id = number->asInt(); id && *id >= 0) targets.emplace_back(SideId{*id});
                    }
                }
            } else if (const auto target = resolveClick({drawable, {}, {}, {}, 0}, selection_.mode(),
                                                       *groups_, selection_.ignoreGroups())) {
                targets.push_back(*target);
            }
        }
    }
    if (!modifiers.ctrl && !modifiers.shift) selection_.replace(targets);
    else for (const auto target : targets) {
        if (modifiers.ctrl) selection_.toggle(target);
        else selection_.add(target);
    }
}
void EditorSession::setMarqueeMode(MarqueeMode mode) {
    if (marqueeMode_ == mode) return;
    marqueeMode_ = mode;
    notify();
}
void EditorSession::setPreset(const Preset& preset) {
    Keymap keymap(preset);
    preset_ = preset;
    keymap_ = std::move(keymap);
    notify();
}
bool EditorSession::perform(const ActionId& action) {
    if (!document_) return false;
    if (action == "grid.larger" || action == "grid.smaller") {
        setGridSize(action == "grid.larger" ? GridLadder::larger(gridSize_) : GridLadder::smaller(gridSize_));
        return true;
    }
    if (action == "workplane.pin") {
        pinWorkPlane();
        return true;
    }
    if (action == "workplane.axis_x" || action == "workplane.axis_y" || action == "workplane.axis_z") {
        const auto expanded = expandSelection(*document_, *groups_, selection_);
        AABB bounds;
        for (const auto node : expanded.solidsAndEntities) {
            if (const auto box = picks_->bounds(node)) bounds.expand(*box);
        }
        for (const auto side : expanded.sides) {
            if (const auto location = document_->side(side)) {
                if (const auto box = picks_->bounds(document_->get(location->solid)->id, side)) bounds.expand(*box);
            }
        }
        const auto axis = action == "workplane.axis_x" ? Axis::X : action == "workplane.axis_y" ? Axis::Y : Axis::Z;
        pinAxisWorkPlane(axis, bounds.empty() ? DVec3{0.0} : bounds.center());
        return true;
    }
    if (action == "selection.none" && workPlane_.pinned()) {
        unpinWorkPlane();
        return true;
    }
    if (action == "edit.undo" || action == "edit.redo") {
        const bool changed = action == "edit.undo" ? document_->undo() : document_->redo();
        if (changed) restoreFromUndo(*document_, selection_);
        return changed;
    }
    if (action == "selection.all") selectAll(selection_, *picks_, *groups_);
    else if (action == "selection.none") selection_.clear();
    else if (action == "selection.invert") invert(selection_, *picks_, *groups_);
    else if (action == "selection.ignore_groups") selection_.setIgnoreGroups(!selection_.ignoreGroups());
    else if (action == "selection.marquee_toggle") {
        setMarqueeMode(marqueeMode_ == MarqueeMode::Enclosed ? MarqueeMode::Touching : MarqueeMode::Enclosed);
    } else {
        const std::pair<const char*, SelectionMode> modes[] = {
            {"selection.mode.groups", SelectionMode::Groups}, {"selection.mode.objects", SelectionMode::Objects},
            {"selection.mode.solids", SelectionMode::Solids}, {"selection.mode.faces", SelectionMode::Faces}};
        bool found = false;
        for (const auto& [id, mode] : modes) {
            if (action == id) { selection_.setMode(mode); found = true; break; }
        }
        if (!found) return false;
    }
    cycle_ = {};
    clearHover();
    return true;
}
void EditorSession::edit(std::string label, const std::function<void(document::Transaction&)>& operation) {
    if (!document_) return;
    const Selection before(selection_);
    auto transaction = document_->begin(std::move(label));
    operation(transaction);
    Selection after(selection_);
    after.prune(*document_);
    recordSelection(transaction, before, after);
    transaction.commit();
}
std::size_t EditorSession::observe(Observer observer) {
    const auto token = nextObserver_++;
    observers_.emplace(token, std::move(observer));
    return token;
}
void EditorSession::unobserve(std::size_t token) { observers_.erase(token); }
void EditorSession::notify() {
    const auto snapshot = observers_;
    for (const auto& [token, callback] : snapshot) {
        if (observers_.contains(token)) callback();
    }
}

} // namespace apex::editor
