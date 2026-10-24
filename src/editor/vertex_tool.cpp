#include "editor/vertex_tool.hpp"

#include "editor/editor_session.hpp"
#include "editor/selection_expand.hpp"
#include "geometry/solid_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <sstream>

namespace apex::editor {
namespace {
bool displacement(const kv1::Node& block) {
    return std::ranges::any_of(block.findAll("side"), [](const auto* side) { return side->find("dispinfo"); });
}
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

bool VertexTool::collectSources() {
    sources_.clear();
    if (elements_.empty() || !session_.document()) return false;
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
    return !sources_.empty();
}

bool VertexTool::beginPush(const ElementHandle& face, Ray ray, InputSource source) {
    if (transformActive() || !collectSources()) return false;
    const auto found = sources_.find(face.solid);
    if (found == sources_.end()) { sources_.clear(); return false; }
    const auto& poly = found->second.poly;
    const auto pressed = std::ranges::find(poly.faces, face.side, &geometry::Face::sideId);
    if (pressed == poly.faces.end() || pressed->vertices.empty()) { sources_.clear(); return false; }
    DVec3 centre{0.0};
    for (const auto index : pressed->vertices) centre += poly.vertices[index];
    centre /= static_cast<double>(pressed->vertices.size());
    const auto hit = session_.picks_ ? session_.picks_->first(ray) : std::nullopt;
    pushAxis_ = glm::normalize(pressed->plane.normal);
    pushStart_ = hit && hit->node == face.solid ? hit->point : centre;
    pushDistance_ = 0.0;
    pushing_ = moving_ = true;
    modalActive_ = awaitingMerge_ = false;
    activeMode_ = GizmoMode::Translate;
    activeHandle_ = GizmoHandle::Screen;
    snapAnchor_ = pushStart_;
    owner_ = source;
    publish(Transform{});
    return true;
}

Transform VertexTool::pushTransform(Ray ray, TransformModifiers modifiers) const {
    // Closest point on the push line to the cursor ray; hold the last value when looking along it.
    const auto q = ray.origin - pushStart_;
    const double a = glm::dot(ray.dir, ray.dir), b = glm::dot(ray.dir, pushAxis_);
    const double det = a - b * b;
    if (det <= 1e-9 * a) return Transform::translate(pushAxis_ * pushDistance_);
    const double t = (b * glm::dot(q, pushAxis_) - glm::dot(q, ray.dir)) / det;
    double distance = glm::dot(ray.at(t) - pushStart_, pushAxis_);
    const double step = session_.gridSize();
    if (!modifiers.suspend && !session_.snapSettings().suspended && step > 0.0)
        distance = std::round(distance / step) * step;
    return Transform::translate(pushAxis_ * distance);
}

bool VertexTool::beginMove(GizmoMode mode, const GizmoView& view, Ray ray, GizmoHandle handle,
                           std::optional<DVec3> anchor) {
    if (transformActive() || !collectSources()) return false;
    const auto context = gizmoContext();
    if (!context) { sources_.clear(); return false; }
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
    if (!collectSources() || !modal_.start(mode, *context, gizmoViewFor(view, context->pivot), view.cursorRay, event.cursor)) {
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
            if (pushing_) delta = pushAxis_ * glm::dot(delta, pushAxis_);
            else if (context && (handle == GizmoHandle::X || handle == GizmoHandle::Y || handle == GizmoHandle::Z)) {
                const auto axis = context->frame[handle == GizmoHandle::X ? 0 : handle == GizmoHandle::Y ? 1 : 2];
                delta = axis * glm::dot(delta, axis);
            }
            actual = Transform::translate(delta);
        }
    }
    if (pushing_) pushDistance_ = glm::dot(actual.point(DVec3{0.0}), pushAxis_);
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
    if (pushing_) {
        std::ostringstream distance;
        distance << "Push/pull " << pushDistance_ << " units";
        appendStatus(distance.str());
    }
    if (absorbedCount_) appendStatus(std::to_string(absorbedCount_) + " vertices will be absorbed");
    if (mergeCount_) appendStatus(std::to_string(mergeCount_) + " vertex merge(s) need confirmation");
    if (splitCount_) appendStatus(std::to_string(splitCount_) + " new face(s)");
    session_.notify();
    return true;
}

bool VertexTool::updateMove(Ray ray, const ToolView& view, TransformModifiers modifiers,
                            const InputEvent* event) {
    if (!transformActive()) return false;
    if (pushing_) return publish(pushTransform(ray, modifiers), &view, event);
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
        if (pane.hoveredFace) { pane.hoveredFace.reset(); session_.notify(); }
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
            if (pushing_ && std::abs(pushDistance_) < 1e-9) {
                const auto collapse = pushCollapse_;
                clearMove();
                if (collapse) selectElement(*collapse, false, false);
                session_.notify();
            } else endGizmoDrag();
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
        const auto element = hitElement(event, view);
        // In face mode a face under the cursor beats the gizmo; the gizmo keeps presses off the face.
        const bool facePush = mode_ == ElementMode::Face && element;
        const auto face = facePush ? element : std::nullopt;
        if (face != pane.hoveredFace) { pane.hoveredFace = face; session_.notify(); }
        pane.hovered = gizmo && !facePush ? std::optional<GizmoHandle>(gizmo->handle) : std::nullopt;
        if (gizmo || element) return true;
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
        const auto pressed = hitElement(event, view);
        const bool facePush = mode_ == ElementMode::Face && pressed && !event.shift && !event.ctrl;
        if (gizmo && !facePush &&
            beginGizmoDrag(gizmo->handle, gizmoViewFor(view, context->pivot), view.cursorRay)) {
            owner_ = event.source; pane.dragged = gizmo->handle; pane.hovered.reset(); return true;
        }
        if (const auto element = pressed) {
            if (facePush) {
                const bool wasSelected = std::ranges::find(elements_, *element) != elements_.end();
                if (!wasSelected) selectElement(*element, false, false);
                if (beginPush(*element, view.cursorRay, event.source)) {
                    pushCollapse_ = wasSelected && elements_.size() > 1 ? element : std::nullopt;
                    pane.hovered.reset();
                    pane.hoveredFace.reset();
                    return true;
                }
                session_.notify();
                return true;
            }
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
    moving_ = modalActive_ = awaitingMerge_ = pushing_ = false;
    pushCollapse_.reset();
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
        moving_ = modalActive_ = awaitingMerge_ = pushing_ = false;
        pushCollapse_.reset();
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
    const auto& palette = session_.overlayPalette();
    const auto solids = selectedSolids();
    for (const auto& [node, geometry] : solids) {
        const auto& poly = geometry.poly;
        std::set<std::pair<std::size_t,std::size_t>> edges;
        for (const auto& face : poly.faces) {
            for (std::size_t i = 0; i < face.vertices.size(); ++i) {
                const auto a = face.vertices[i], b = face.vertices[(i+1)%face.vertices.size()];
                edges.emplace(std::min(a,b), std::max(a,b));
                overlay.worldSegments.push_back({poly.vertices[a], poly.vertices[b], palette.vertexEdge, 1.0f});
            }
        }
        if (mode_ == ElementMode::Vertex) for (std::size_t i = 0; i < poly.vertices.size(); ++i) {
            const ElementHandle handle{node,ElementKind::Vertex,i};
            const bool selected = std::ranges::find(elements_, handle) != elements_.end();
            auto point = poly.vertices[i];
            Vec4 c = selected ? palette.vertexSelected : palette.vertexHandle;
            if (transformActive() && selected) {
                for (const auto& [id, src] : sources_) if (id == node) {
                    if (const auto found = std::ranges::find(src.vertices, i); found != src.vertices.end()) {
                        if (preview_.empty()) {
                            const auto transform = modalActive_ ? modal_.value().transform : drag_.value().transform;
                            point = transform.point(point);
                            c = palette.vertexInvalid;
                        }
                        else if (const auto replacement = preview_.replacements().find(node); replacement != preview_.replacements().end() &&
                                 std::ranges::find(src.result.absorbedVertices, i) != src.result.absorbedVertices.end()) {
                            point = modalActive_ ? modal_.value().transform.point(point) : drag_.value().transform.point(point);
                            c = palette.vertexInvalid;
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
                selected ? palette.vertexSelected : palette.vertexHandle, selected ? 10.0f : 7.0f});
        }
        if (mode_ == ElementMode::Face) for (const auto& face : poly.faces) {
            DVec3 centre{0.0}; for (const auto index : face.vertices) centre += poly.vertices[index];
            centre /= static_cast<double>(face.vertices.size());
            const ElementHandle handle{node,ElementKind::Face,0,0,static_cast<SideId>(face.sideId)};
            const bool selected = std::ranges::find(elements_, handle) != elements_.end();
            overlay.worldPoints.push_back({centre, selected ? palette.vertexSelected : palette.vertexHandle, selected ? 10.0f : 7.0f});
        }
        if (mode_ != ElementMode::Vertex) for (std::size_t i = 0; i < poly.vertices.size(); ++i)
            overlay.worldPoints.push_back({poly.vertices[i], palette.vertexInactive, 4.0f});
        if (mode_ == ElementMode::Vertex) for (const auto& [a,b] : edges)
            overlay.worldPoints.push_back({(poly.vertices[a]+poly.vertices[b])*0.5, palette.vertexInactive, 4.0f});
        if (mode_ != ElementMode::Face) for (const auto& face : poly.faces) {
            DVec3 centre{0.0}; for (const auto index : face.vertices) centre += poly.vertices[index];
            centre /= static_cast<double>(face.vertices.size());
            overlay.worldPoints.push_back({centre, palette.vertexInactive, 4.0f});
        }
        if (const auto pane = panes_.find(source); !transformActive() && pane != panes_.end() &&
            pane->second.hoveredFace && pane->second.hoveredFace->solid == node) {
            const auto face = std::ranges::find(poly.faces, pane->second.hoveredFace->side, &geometry::Face::sideId);
            if (face != poly.faces.end() && face->vertices.size() >= 3) {
                const Vec4 outline = palette.faceHover;
                Vec4 fill = outline;
                fill.a = 0.22f;
                const auto& v = face->vertices;
                for (std::size_t i = 1; i + 1 < v.size(); ++i)
                    overlay.worldTriangles.push_back({poly.vertices[v[0]], poly.vertices[v[i]], poly.vertices[v[i+1]], fill});
                for (std::size_t i = 0; i < v.size(); ++i)
                    overlay.worldSegments.push_back({poly.vertices[v[i]], poly.vertices[v[(i+1)%v.size()]], outline, 2.5f});
            }
        }
        if (transformActive()) if (const auto sourceIt = sources_.find(node); sourceIt != sources_.end()) {
            for (const auto faceIndex : sourceIt->second.result.splitFaces) {
                if (faceIndex >= sourceIt->second.result.poly.faces.size()) continue;
                const auto& split = sourceIt->second.result.poly.faces[faceIndex];
                for (std::size_t i = 0; i < split.vertices.size(); ++i) {
                    const auto a = sourceIt->second.result.poly.vertices[split.vertices[i]];
                    const auto b = sourceIt->second.result.poly.vertices[split.vertices[(i+1)%split.vertices.size()]];
                    overlay.worldSegments.push_back({a,b,palette.vertexSplit,2.5f});
                }
            }
        }
    }
    if (!modalActive_ && !pushing_) if (const auto context = gizmoContext(); context && view.size.x > 0 && view.size.y > 0) {
        const auto gizmoView = gizmoViewFor(view, context->pivot);
        const auto active = dragged(source) ? dragged(source) : hovered(source);
        for (const auto& primitive : gizmoDescription(session_.gizmoMode(), *context, gizmoView)) {
            const Vec4 c = gizmoHandleColour(palette, primitive.handle, active && *active == primitive.handle);
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

