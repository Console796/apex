#include "editor/vertex_tool.hpp"

#include "editor/editor_session.hpp"
#include "editor/selection_expand.hpp"
#include "geometry/solid_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace apex::editor {
namespace {
bool displacement(const kv1::Node& block) {
    return std::ranges::any_of(block.findAll("side"), [](const auto* side) { return side->find("dispinfo"); });
}
Vec4 colour(float r, float g, float b) { return {r,g,b,1.0f}; }
}

std::map<NodeId, geometry::SolidGeometry> VertexTool::selectedSolids() const {
    std::map<NodeId, geometry::SolidGeometry> result;
    if (!session_.document() || !session_.groups_) return result;
    const auto expanded = expandSelection(*session_.document(), *session_.groups_, session_.selection());
    std::set<NodeId> nodes(expanded.solidsAndEntities.begin(), expanded.solidsAndEntities.end());
    for (const auto side : expanded.sides) if (const auto location = session_.document()->side(side))
        if (const auto* solid = session_.document()->get(location->solid)) nodes.insert(solid->id);
    for (const auto node : nodes) {
        const auto handle = session_.document()->handle(node);
        if (!handle || !std::holds_alternative<Handle<document::Solid>>(*handle)) continue;
        const auto* solid = session_.document()->get(std::get<Handle<document::Solid>>(*handle));
        if (!solid || displacement(*solid->block)) continue;
        const auto geometry = geometry::solidGeometry(*solid->block);
        if (geometry) result.emplace(node, *geometry);
    }
    return result;
}

std::optional<DVec3> VertexTool::elementPoint(const ElementHandle& element,
    const std::map<NodeId, geometry::SolidGeometry>& solids) const {
    const auto found = solids.find(element.solid);
    if (found == solids.end()) return {};
    const auto& poly = found->second.poly;
    if (element.kind == ElementKind::Vertex) {
        if (element.a < poly.vertices.size()) return poly.vertices[element.a];
    } else if (element.kind == ElementKind::Edge) {
        if (element.a < poly.vertices.size() && element.b < poly.vertices.size())
            return (poly.vertices[element.a] + poly.vertices[element.b]) * 0.5;
    } else {
        const auto face = std::ranges::find(poly.faces, element.side, &geometry::Face::sideId);
        if (face != poly.faces.end() && !face->vertices.empty()) {
            DVec3 centre{0.0};
            for (const auto index : face->vertices) centre += poly.vertices[index];
            return centre / static_cast<double>(face->vertices.size());
        }
    }
    return {};
}

std::optional<ElementHandle> VertexTool::hitElement(const InputEvent& event, const ToolView& view) const {
    if (!session_.document() || !session_.picks_) return {};
    const auto solids = selectedSolids();
    if (mode_ == ElementMode::Face) {
        const auto hit = session_.picks_->first(view.cursorRay);
        if (!hit || !hit->side || !solids.contains(hit->node)) return {};
        return ElementHandle{hit->node, ElementKind::Face, 0, 0, *hit->side};
    }
    const auto features = session_.picks_->nearestFeature(view.cursorRay, 8.0, view);
    const auto eligible = [&](const FeatureHit& hit) { return solids.contains(hit.node); };
    if (mode_ == ElementMode::Vertex && features.vertex && eligible(*features.vertex))
        return ElementHandle{features.vertex->node, ElementKind::Vertex, features.vertex->feature.a};
    if (mode_ == ElementMode::Edge && features.midpoint && eligible(*features.midpoint))
        return ElementHandle{features.midpoint->node, ElementKind::Edge,
            features.midpoint->feature.a, features.midpoint->feature.b};
    (void)event;
    return {};
}

bool VertexTool::selectElement(ElementHandle element, bool add, bool toggle) {
    const auto found = std::ranges::find(elements_, element);
    if (!add && !toggle) elements_.clear();
    if (toggle && found != elements_.end()) {
        elements_.erase(std::ranges::find(elements_, element));
        return true;
    }
    if (std::ranges::find(elements_, element) == elements_.end()) elements_.push_back(element);
    return true;
}

std::optional<GizmoContext> VertexTool::gizmoContext() const {
    if (elements_.empty()) return {};
    const auto solids = selectedSolids();
    AABB bounds;
    std::optional<DVec3> active;
    for (const auto& element : elements_) if (const auto point = elementPoint(element, solids)) {
        bounds.expand(*point);
        if (!active) active = point;
    }
    if (bounds.empty()) return {};
    GizmoContext context;
    context.bounds = bounds;
    context.pivot = session_.pivotMode() == PivotMode::Custom ? session_.customPivot()
        : session_.pivotMode() == PivotMode::ActiveOrigin && active ? *active : bounds.center();
    context.workPlane = session_.workPlane().resolve()
        ? session_.workPlane().resolve()->plane : session_.fallbackPlane();
    context.snap = session_.snapSettings();
    context.snap.gridSize = session_.gridSize();
    return context;
}

std::vector<FeatureRef> VertexTool::excludedFeatures() const {
    std::vector<FeatureRef> excluded;
    std::map<NodeId, geometry::SolidGeometry> solids;
    for (const auto& [node, source] : sources_) {
        (void)source;
        const auto handle = session_.document()->handle(node);
        if (handle) if (const auto* solid = session_.document()->get(*handle))
            if (const auto geometry = geometry::solidGeometry(*solid->block)) solids.emplace(node, *geometry);
    }
    for (const auto& [node, geometry] : solids) {
        std::set<std::size_t> selected;
        for (const auto& element : elements_) if (element.solid == node) {
            if (element.kind == ElementKind::Vertex) selected.insert(element.a);
            else if (element.kind == ElementKind::Edge) { selected.insert(element.a); selected.insert(element.b); }
            else if (const auto face = std::ranges::find(geometry.poly.faces, element.side, &geometry::Face::sideId);
                     face != geometry.poly.faces.end()) selected.insert(face->vertices.begin(), face->vertices.end());
        }
        for (const auto index : selected) excluded.push_back({node, FeatureKind::Vertex, index, 0});
        std::set<std::pair<std::size_t,std::size_t>> edges;
        for (const auto& face : geometry.poly.faces) for (std::size_t i = 0; i < face.vertices.size(); ++i) {
            const auto a = face.vertices[i], b = face.vertices[(i+1)%face.vertices.size()];
            if (selected.contains(a) || selected.contains(b)) edges.emplace(std::min(a,b), std::max(a,b));
        }
        for (const auto& [a,b] : edges) excluded.push_back({node, FeatureKind::Edge, a, b});
    }
    return excluded;
}

bool VertexTool::beginMove(GizmoMode mode, const GizmoView& view, Ray ray, GizmoHandle handle,
                           std::optional<DVec3> anchor) {
    if (transformActive() || elements_.empty() || !session_.document()) return false;
    sources_.clear();
    selectionRevision_ = session_.selection().changed();
    for (const auto& [node, geometry] : selectedSolids()) {
        Source source;
        source.node = node;
        source.poly = geometry.poly;
        const auto docHandle = session_.document()->handle(node);
        if (!docHandle || !std::holds_alternative<Handle<document::Solid>>(*docHandle)) continue;
        source.handle = std::get<Handle<document::Solid>>(*docHandle);
        source.block = session_.document()->get(source.handle)->block.get();
        for (const auto& element : elements_) if (element.solid == node) {
            if (element.kind == ElementKind::Vertex) source.vertices.push_back(element.a);
            else if (element.kind == ElementKind::Edge) { source.vertices.push_back(element.a); source.vertices.push_back(element.b); }
            else if (const auto face = std::ranges::find(source.poly.faces, element.side, &geometry::Face::sideId);
                     face != source.poly.faces.end()) source.vertices.insert(source.vertices.end(), face->vertices.begin(), face->vertices.end());
        }
        std::ranges::sort(source.vertices);
        source.vertices.erase(std::unique(source.vertices.begin(), source.vertices.end()), source.vertices.end());
        if (!source.vertices.empty()) sources_.emplace(node, std::move(source));
    }
    const auto context = gizmoContext();
    if (!context || sources_.empty()) return false;
    const auto hitView = view;
    if (!drag_.begin(mode, handle, *context, hitView, ray)) { sources_.clear(); return false; }
    moving_ = true;
    modalActive_ = false;
    awaitingMerge_ = false;
    activeMode_ = mode;
    activeHandle_ = handle;
    if (anchor) snapAnchor_ = *anchor;
    else {
        snapAnchor_ = context->pivot;
        for (const auto& element : elements_) if (const auto point = elementPoint(element, selectedSolids())) { snapAnchor_ = *point; break; }
    }
    owner_.reset();
    publish(Transform{});
    return true;
}

bool VertexTool::beginGizmoDrag(GizmoHandle handle, const GizmoView& view, Ray ray) {
    const auto context = gizmoContext();
    if (!context) return false;
    const auto pivot = context->pivot;
    if (!beginMove(session_.gizmoMode(), view, ray, handle, pivot)) return false;
    return true;
}

bool VertexTool::beginModal(GizmoMode mode, const ToolView& view, const InputEvent& event) {
    if (elements_.empty() || transformActive()) return false;
    const auto context = gizmoContext();
    if (!context) return false;
    sources_.clear();
    selectionRevision_ = session_.selection().changed();
    for (const auto& [node, geometry] : selectedSolids()) {
        const auto docHandle = session_.document()->handle(node);
        if (!docHandle || !std::holds_alternative<Handle<document::Solid>>(*docHandle)) continue;
        Source source{std::get<Handle<document::Solid>>(*docHandle), node,
            session_.document()->get(*docHandle)->block.get(), geometry.poly, {}, {}};
        for (const auto& element : elements_) if (element.solid == node) {
            if (element.kind == ElementKind::Vertex) source.vertices.push_back(element.a);
            else if (element.kind == ElementKind::Edge) { source.vertices.push_back(element.a); source.vertices.push_back(element.b); }
            else if (const auto face = std::ranges::find(source.poly.faces, element.side, &geometry::Face::sideId);
                     face != source.poly.faces.end()) source.vertices.insert(source.vertices.end(), face->vertices.begin(), face->vertices.end());
        }
        std::ranges::sort(source.vertices);
        source.vertices.erase(std::unique(source.vertices.begin(), source.vertices.end()), source.vertices.end());
        if (!source.vertices.empty()) sources_.emplace(node, std::move(source));
    }
    if (sources_.empty() || !modal_.start(mode, *context, gizmoViewFor(view, context->pivot), view.cursorRay, event.cursor)) {
        sources_.clear(); return false;
    }
    moving_ = false;
    modalActive_ = true;
    awaitingMerge_ = false;
    activeMode_ = mode;
    activeHandle_ = GizmoHandle::Screen;
    snapAnchor_ = context->pivot;
    owner_ = event.source;
    publish(modal_.value().transform);
    return true;
}

bool VertexTool::publish(const Transform& transform, const ToolView* view, const InputEvent* event) {
    Transform actual = transform;
    if (view && activeMode_ == GizmoMode::Translate &&
        !(modalActive_ && !modal_.typedValue().empty())) {
        const auto snapped = session_.snap(view->cursorRay, *view,
            event && transformModifiers(session_.preset(), *event).suspend, {}, true, excludedFeatures());
        if (snapped.kind == SnapKind::Vertex || snapped.kind == SnapKind::EdgeMidpoint ||
            snapped.kind == SnapKind::Edge) {
            auto delta = snapped.point - snapAnchor_;
            const auto context = gizmoContext();
            const auto handle = modalActive_ ? modal_.constraint() : activeHandle_;
            if (context && (handle == GizmoHandle::X || handle == GizmoHandle::Y || handle == GizmoHandle::Z)) {
                const auto axis = context->frame[handle == GizmoHandle::X ? 0 : handle == GizmoHandle::Y ? 1 : 2];
                delta = axis * glm::dot(delta, axis);
            }
            actual = Transform::translate(delta);
        }
    }
    std::map<NodeId, geometry::ConvexPolyhedron> replacements;
    absorbedCount_ = mergeCount_ = splitCount_ = 0;
    status_.clear();
    for (auto& [node, source] : sources_) {
        auto evaluated = evaluateVertexEdit(source.poly, source.vertices, actual);
        if (!evaluated) {
            preview_.clear();
            previewValid_ = false;
            status_ = std::string("Invalid move: ") + evaluated.error().what();
            session_.notify();
            return false;
        }
        source.result = std::move(*evaluated);
        absorbedCount_ += source.result.absorbedVertices.size();
        mergeCount_ += source.result.mergedVertices.size();
        splitCount_ += source.result.splitFaces.size();
        replacements.emplace(node, source.result.poly);
    }
    preview_.setReplacements(replacements);
    previewValid_ = true;
    const auto appendStatus = [&](std::string message) {
        if (!status_.empty()) status_ += "; ";
        status_ += std::move(message);
    };
    if (absorbedCount_) appendStatus(std::to_string(absorbedCount_) + " vertices will be absorbed");
    if (mergeCount_) appendStatus(std::to_string(mergeCount_) + " vertex merge(s) need confirmation");
    if (splitCount_) appendStatus(std::to_string(splitCount_) + " new face(s)");
    session_.notify();
    return true;
}

bool VertexTool::updateMove(Ray ray, const ToolView& view, TransformModifiers modifiers,
                            const InputEvent* event) {
    if (!transformActive()) return false;
    const auto value = modalActive_ ? modal_.update(ray, modifiers).transform : drag_.update(ray, modifiers).transform;
    return publish(value, &view, event);
}

bool VertexTool::updateGizmoDrag(Ray ray, TransformModifiers modifiers) {
    if (!moving_) return false;
    return publish(drag_.update(ray, modifiers).transform);
}

Handled VertexTool::input(const InputEvent& event, const ToolView& view) {
    auto& pane = panes_[event.source];
    pane.cursor = event.cursor;
    if (event.kind == InputKind::Cancel) {
        if (owns(event.source)) cancelTransform();
        return false;
    }
    if (event.kind == InputKind::CancelPointer) return false;
    if (event.kind == InputKind::Leave) {
        pane.hovered.reset();
        return false;
    }
    if (!session_.document() || view.size.x <= 0 || view.size.y <= 0) return false;
    const bool key = event.kind == InputKind::KeyDown || event.kind == InputKind::KeyUp;
    const auto modifiers = transformModifiers(session_.preset(), event);
    const auto update = [&] { return updateMove(view.cursorRay, view, modifiers, &event); };
    if (owns(event.source)) {
        if (key) {
            if (event.modifierKey) { update(); return false; }
            if (event.kind == InputKind::KeyUp) return modalActive_;
            if (awaitingMerge_) {
                if (event.key == "Escape") cancelTransform();
                else if (event.key == "Enter") commit();
                return true;
            }
            if (!modalActive_) {
                if (event.key == "Escape") { cancelTransform(); return true; }
                return false;
            }
            const auto result = modal_.key(event.key, event.shift);
            if (result == ModalEvent::Updated) publish(modal_.value().transform, &view, &event);
            else if (result == ModalEvent::Commit) { publish(modal_.value().transform); commit(); }
            else if (result == ModalEvent::Cancel) cancelTransform();
            return true;
        }
        if (event.kind == InputKind::Move) update();
        else if (event.kind == InputKind::Press && modalActive_) {
            update();
            if (event.button == PointerButton::Left) modal_.key("LeftClick");
            else if (event.button == PointerButton::Right) modal_.key("RightClick");
        } else if (event.kind == InputKind::Press && event.button == PointerButton::Right) cancelTransform();
        else if (event.kind == InputKind::Release && !modalActive_ && event.button == PointerButton::Left) {
            update();
            endGizmoDrag();
        }
        return true;
    }
    if (pane.marqueeStart && event.kind == InputKind::Move && event.held()) {
        pane.cursor = event.cursor;
        return true;
    }
    if (pane.marqueeStart && event.kind == InputKind::Release && event.button == PointerButton::Left) {
        const auto start = *pane.marqueeStart;
        const DVec2 minimum{std::min(start.x,event.cursor.x), std::min(start.y,event.cursor.y)};
        const DVec2 maximum{std::max(start.x,event.cursor.x), std::max(start.y,event.cursor.y)};
        const auto solids = selectedSolids();
        for (const auto& [node, geometry] : solids) {
            std::vector<ElementHandle> candidates;
            if (mode_ == ElementMode::Vertex) for (std::size_t i=0;i<geometry.poly.vertices.size();++i)
                candidates.push_back({node,ElementKind::Vertex,i});
            else if (mode_ == ElementMode::Edge) {
                std::set<std::pair<std::size_t,std::size_t>> edges;
                for (const auto& face : geometry.poly.faces) for (std::size_t i=0;i<face.vertices.size();++i)
                    edges.emplace(std::min(face.vertices[i],face.vertices[(i+1)%face.vertices.size()]),
                                  std::max(face.vertices[i],face.vertices[(i+1)%face.vertices.size()]));
                for (const auto& [a,b] : edges) candidates.push_back({node,ElementKind::Edge,a,b});
            } else for (const auto& face : geometry.poly.faces)
                candidates.push_back({node,ElementKind::Face,0,0,static_cast<SideId>(face.sideId)});
            for (const auto& candidate : candidates) if (const auto point=elementPoint(candidate,solids))
                if (const auto pixel=worldToPixel(view,*point); pixel && pixel->x>=minimum.x && pixel->x<=maximum.x &&
                    pixel->y>=minimum.y && pixel->y<=maximum.y)
                    selectElement(candidate,true,pane.marqueeToggle);
        }
        pane.marqueeStart.reset();
        session_.notify();
        return true;
    }
    if (key) {
        if (event.kind != InputKind::KeyDown || event.autoRepeat || event.navigating || transformActive() || !event.chord)
            return false;
        const auto action = session_.keymap().lookup(*event.chord, BindingContext::forTool(id()));
        if (!action || !action->starts_with("transform.modal.")) return false;
        const auto mode = *action == "transform.modal.grab" ? GizmoMode::Translate
                        : *action == "transform.modal.rotate" ? GizmoMode::Rotate : GizmoMode::Scale;
        const auto context = gizmoContext();
        if (!context) return true;
        if (beginModal(mode, view, event)) update();
        return true;
    }
    if (event.kind == InputKind::Move && !event.held()) {
        const auto context = gizmoContext();
        const auto gizmo = context ? hitTestGizmo(session_.gizmoMode(), *context,
            gizmoViewFor(view, context->pivot), view.cursorRay) : std::nullopt;
        pane.hovered = gizmo ? std::optional<GizmoHandle>(gizmo->handle) : std::nullopt;
        if (gizmo) return true;
        const auto element = hitElement(event, view);
        if (element) return true;
    }
    if (event.kind == InputKind::Press && event.button == PointerButton::Left) {
        if (elements_.empty()) {
            const auto expanded = expandSelection(*session_.document(), *session_.groups_, session_.selection());
            for (const auto node : expanded.solidsAndEntities) {
                const auto handle = session_.document()->handle(node);
                if (!handle || !std::holds_alternative<Handle<document::Solid>>(*handle)) continue;
                const auto* solid = session_.document()->get(std::get<Handle<document::Solid>>(*handle));
                if (solid && displacement(*solid->block)) {
                    status_ = "Displacement solids are read-only in Vertex mode";
                    session_.notify();
                    return true;
                }
            }
        }
        const auto context = gizmoContext();
        const auto gizmo = context ? hitTestGizmo(session_.gizmoMode(), *context,
            gizmoViewFor(view, context->pivot), view.cursorRay) : std::nullopt;
        if (gizmo && beginGizmoDrag(gizmo->handle, gizmoViewFor(view, context->pivot), view.cursorRay)) {
            owner_ = event.source; pane.dragged = gizmo->handle; pane.hovered.reset(); return true;
        }
        if (const auto element = hitElement(event, view)) {
            selectElement(*element, event.shift, event.ctrl);
            session_.notify();
            return true;
        }
        if (!event.shift && !event.ctrl) { elements_.clear(); session_.notify(); }
        pane.marqueeStart = event.cursor;
        pane.cursor = event.cursor;
        pane.marqueeAdd = event.shift;
        pane.marqueeToggle = event.ctrl;
        return true;
    }
    return false;
}

void VertexTool::updateSelectionAfterCommit(const std::map<NodeId, VertexEditResult>& results) {
    std::vector<ElementHandle> updated;
    std::map<NodeId, geometry::SolidGeometry> committed;
    for (const auto& element : elements_) {
        const auto source = sources_.find(element.solid);
        const auto output = results.find(element.solid);
        if (source == sources_.end() || output == results.end()) { updated.push_back(element); continue; }
        if (element.kind == ElementKind::Vertex) {
            for (std::size_t n = 0; n < output->second.vertexSources.size(); ++n)
                if (std::ranges::find(output->second.vertexSources[n], element.a) != output->second.vertexSources[n].end())
                    updated.push_back({element.solid, ElementKind::Vertex, n});
        } else if (element.kind == ElementKind::Edge) {
            std::optional<std::size_t> a, b;
            for (std::size_t n = 0; n < output->second.vertexSources.size(); ++n) {
                const auto& origins = output->second.vertexSources[n];
                if (std::ranges::find(origins, element.a) != origins.end()) a = n;
                if (std::ranges::find(origins, element.b) != origins.end()) b = n;
            }
            if (a && b && *a != *b) updated.push_back({element.solid, ElementKind::Edge, std::min(*a,*b), std::max(*a,*b)});
        } else if (session_.document()->side(element.side)) updated.push_back(element);
    }
    std::ranges::sort(updated);
    updated.erase(std::unique(updated.begin(), updated.end()), updated.end());
    elements_ = std::move(updated);
}

bool VertexTool::commit() {
    if (!transformActive()) return false;
    if (!previewValid_) {
        const auto reason = status_.empty() ? std::string("Invalid move") : status_;
        clearMove();
        status_ = reason;
        session_.notify();
        return false;
    }
    if (!session_.document() || session_.selection().changed() != selectionRevision_) {
        status_ = "Selection changed during the move; move cancelled";
        cancelTransform();
        return false;
    }
    for (const auto& [node, source] : sources_) {
        const auto* current = session_.document()->get(source.handle);
        if (!current || current->block.get() != source.block) {
            status_ = "Solid changed during the move; move cancelled";
            cancelTransform();
            return false;
        }
    }
    if (mergeCount_ && !awaitingMerge_) {
        awaitingMerge_ = true;
        status_ = "Merge vertices? Enter confirms; Esc cancels";
        session_.notify();
        return false;
    }
    std::map<NodeId, VertexEditResult> results;
    for (const auto& [node, source] : sources_) results.emplace(node, source.result);
    committing_ = true;
    try {
        session_.edit("Move elements", [&](document::Transaction& tx) {
            for (const auto& [node, source] : sources_) {
                (void)node;
                tx.rebuildSolid(source.handle, source.result.poly, source.result.correspondence);
            }
        });
    } catch (const std::exception& error) {
        committing_ = false;
        preview_.clear();
        previewValid_ = false;
        status_ = std::string("Invalid move: ") + error.what();
        session_.notify();
        return false;
    }
    committing_ = false;
    updateSelectionAfterCommit(results);
    clearMove();
    session_.notify();
    return true;
}

void VertexTool::clearMove() {
    drag_.cancel(); modal_.cancel();
    moving_ = modalActive_ = awaitingMerge_ = false;
    sources_.clear(); owner_.reset();
    for (auto& [source, pane] : panes_) pane.dragged.reset();
    preview_.clear();
    previewValid_ = false;
    status_.clear();
}
bool VertexTool::endGizmoDrag() { return commit(); }

void VertexTool::cancelTransform() {
    if (!transformActive() && !awaitingMerge_ && preview_.empty()) return;
    clearMove();
    if (status_.empty()) status_ = "Move cancelled";
    session_.notify();
}
void VertexTool::endTransform() { clearMove(); }
void VertexTool::documentChanged() {
    if (committing_) return;
    if (transformActive() || awaitingMerge_) {
        drag_.cancel(); modal_.cancel();
        moving_ = modalActive_ = awaitingMerge_ = false;
        sources_.clear(); owner_.reset(); preview_.clear();
        previewValid_ = false;
        status_ = "Document changed; move cancelled";
    }
    if (!elements_.empty()) {
        elements_.clear();
        if (status_.empty()) status_ = "Document changed; element selection cleared";
    }
}
void VertexTool::cancel() {
    cancelTransform();
    elements_.clear(); panes_.clear();
}
void VertexTool::forget(InputSource source) {
    if (owner_ == source) { clearMove(); status_ = "Move cancelled"; }
    panes_.erase(source);
}
bool VertexTool::owns(InputSource source) const { return owner_ == source && transformActive(); }
std::optional<GizmoHandle> VertexTool::hovered(InputSource source) const {
    const auto it = panes_.find(source); return it == panes_.end() ? std::nullopt : it->second.hovered;
}
std::optional<GizmoHandle> VertexTool::dragged(InputSource source) const {
    const auto it = panes_.find(source); return it == panes_.end() ? std::nullopt : it->second.dragged;
}

bool VertexTool::perform(const ActionId& action) {
    if (action == "vertex.mode.vertex" || action == "vertex.mode.edge" || action == "vertex.mode.face") {
        mode_ = action == "vertex.mode.vertex" ? ElementMode::Vertex
              : action == "vertex.mode.edge" ? ElementMode::Edge : ElementMode::Face;
        elements_.clear();
        for (auto& [source, pane] : panes_) { (void)source; pane.marqueeStart.reset(); }
        status_.clear();
        session_.notify();
        return true;
    }
    return false;
}

ToolReadout VertexTool::readout() const {
    const char* mode = mode_ == ElementMode::Vertex ? "Vertex" : mode_ == ElementMode::Edge ? "Edge" : "Face";
    std::string status = status_;
    if (status.empty()) status = elements_.empty() ? "Select elements" : std::to_string(elements_.size()) + " element(s) selected";
    return {{"Vertex", mode}, {}, status, status_.starts_with("Invalid") ||
        status_.find("collapse") != std::string::npos};
}

ToolOverlay VertexTool::overlay(const ToolView& view, InputSource source) const {
    ToolOverlay overlay;
    const auto solids = selectedSolids();
    for (const auto& [node, geometry] : solids) {
        const auto& poly = geometry.poly;
        std::set<std::pair<std::size_t,std::size_t>> edges;
        for (const auto& face : poly.faces) {
            for (std::size_t i = 0; i < face.vertices.size(); ++i) {
                const auto a = face.vertices[i], b = face.vertices[(i+1)%face.vertices.size()];
                edges.emplace(std::min(a,b), std::max(a,b));
                overlay.worldSegments.push_back({poly.vertices[a], poly.vertices[b], colour(0.35f,0.55f,0.75f), 1.0f});
            }
        }
        if (mode_ == ElementMode::Vertex) for (std::size_t i = 0; i < poly.vertices.size(); ++i) {
            const ElementHandle handle{node,ElementKind::Vertex,i};
            const bool selected = std::ranges::find(elements_, handle) != elements_.end();
            auto point = poly.vertices[i];
            Vec4 c = selected ? colour(1.0f,0.85f,0.2f) : colour(0.25f,0.85f,1.0f);
            if (transformActive() && selected) {
                for (const auto& [id, src] : sources_) if (id == node) {
                    if (const auto found = std::ranges::find(src.vertices, i); found != src.vertices.end()) {
                        if (preview_.empty()) {
                            const auto transform = modalActive_ ? modal_.value().transform : drag_.value().transform;
                            point = transform.point(point);
                            c = colour(1.0f,0.15f,0.15f);
                        }
                        else if (const auto replacement = preview_.replacements().find(node); replacement != preview_.replacements().end() &&
                                 std::ranges::find(src.result.absorbedVertices, i) != src.result.absorbedVertices.end()) {
                            point = modalActive_ ? modal_.value().transform.point(point) : drag_.value().transform.point(point);
                            c = colour(1.0f,0.15f,0.15f);
                        }
                    }
                }
            }
            overlay.worldPoints.push_back({point,c,selected ? 10.0f : 7.0f});
        }
        if (mode_ == ElementMode::Edge) for (const auto& [a,b] : edges) {
            const ElementHandle handle{node,ElementKind::Edge,a,b};
            const bool selected = std::ranges::find(elements_, handle) != elements_.end();
            overlay.worldPoints.push_back({(poly.vertices[a]+poly.vertices[b])*0.5,
                selected ? colour(1.0f,0.85f,0.2f) : colour(0.25f,0.85f,1.0f), selected ? 10.0f : 7.0f});
        }
        if (mode_ == ElementMode::Face) for (const auto& face : poly.faces) {
            DVec3 centre{0.0}; for (const auto index : face.vertices) centre += poly.vertices[index];
            centre /= static_cast<double>(face.vertices.size());
            const ElementHandle handle{node,ElementKind::Face,0,0,static_cast<SideId>(face.sideId)};
            const bool selected = std::ranges::find(elements_, handle) != elements_.end();
            overlay.worldPoints.push_back({centre, selected ? colour(1.0f,0.85f,0.2f) : colour(0.25f,0.85f,1.0f), selected ? 10.0f : 7.0f});
        }
        if (mode_ != ElementMode::Vertex) for (std::size_t i = 0; i < poly.vertices.size(); ++i)
            overlay.worldPoints.push_back({poly.vertices[i], colour(0.4f,0.58f,0.68f), 4.0f});
        if (mode_ != ElementMode::Edge) for (const auto& [a,b] : edges)
            overlay.worldPoints.push_back({(poly.vertices[a]+poly.vertices[b])*0.5, colour(0.4f,0.58f,0.68f), 4.0f});
        if (mode_ != ElementMode::Face) for (const auto& face : poly.faces) {
            DVec3 centre{0.0}; for (const auto index : face.vertices) centre += poly.vertices[index];
            centre /= static_cast<double>(face.vertices.size());
            overlay.worldPoints.push_back({centre, colour(0.4f,0.58f,0.68f), 4.0f});
        }
        if (transformActive()) if (const auto sourceIt = sources_.find(node); sourceIt != sources_.end()) {
            for (const auto faceIndex : sourceIt->second.result.splitFaces) {
                if (faceIndex >= sourceIt->second.result.poly.faces.size()) continue;
                const auto& split = sourceIt->second.result.poly.faces[faceIndex];
                for (std::size_t i = 0; i < split.vertices.size(); ++i) {
                    const auto a = sourceIt->second.result.poly.vertices[split.vertices[i]];
                    const auto b = sourceIt->second.result.poly.vertices[split.vertices[(i+1)%split.vertices.size()]];
                    overlay.worldSegments.push_back({a,b,colour(1.0f,0.45f,0.08f),2.5f});
                }
            }
        }
    }
    if (!modalActive_) if (const auto context = gizmoContext(); context && view.size.x > 0 && view.size.y > 0) {
        const auto gizmoView = gizmoViewFor(view, context->pivot);
        const auto active = dragged(source) ? dragged(source) : hovered(source);
        for (const auto& primitive : gizmoDescription(session_.gizmoMode(), *context, gizmoView)) {
            const Vec4 c = active && *active == primitive.handle ? colour(1.0f,0.9f,0.2f) :
                primitive.handle == GizmoHandle::X ? colour(0.95f,0.25f,0.25f) :
                primitive.handle == GizmoHandle::Y ? colour(0.35f,0.85f,0.3f) : colour(0.3f,0.55f,1.0f);
            if (primitive.shape == GizmoShape::Segment) overlay.segments.push_back({primitive.points[0],primitive.points[1],c,2.5f});
            else if (primitive.shape == GizmoShape::Quad) for (int i=0;i<4;++i)
                overlay.segments.push_back({primitive.points[i],primitive.points[(i+1)%4],c,2.0f});
            else {
                const auto n = glm::normalize(primitive.normal);
                DVec3 u = glm::normalize(glm::cross(n, std::abs(n.x)<0.8 ? DVec3{1,0,0} : DVec3{0,1,0}));
                const DVec3 v = glm::cross(n,u);
                DVec3 previous = primitive.centre + u*primitive.radius;
                for (int i=1;i<=48;++i) {
                    const double angle = 2*std::numbers::pi*i/48;
                    const DVec3 next = primitive.centre + (u*std::cos(angle)+v*std::sin(angle))*primitive.radius;
                    overlay.segments.push_back({previous,next,c,2.0f}); previous=next;
                }
            }
        }
    }
    if (const auto it = panes_.find(source); it != panes_.end() && it->second.marqueeStart)
        overlay.rectangle = {{*it->second.marqueeStart, it->second.cursor}};
    return overlay;
}

} // namespace apex::editor

