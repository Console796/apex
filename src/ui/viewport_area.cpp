#include "render/vk.hpp"  // volk before Qt's Vulkan header.

#include "ui/viewport_area.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QJsonArray>
#include <QMouseEvent>
#include <QSplitter>
#include <QSplitterHandle>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <utility>

#ifdef APEX_HAS_WAYLAND_POINTER_LOCK
#include <qpa/qplatformnativeinterface.h>
#include <wayland-client.h>
#endif

#include "core/log.hpp"
#include "editor/selection_expand.hpp"
#include "ui/area_frame.hpp"
#include "ui/gpu_host.hpp"
#include "ui/input_log.hpp"
#include "ui/theme.hpp"
#include "ui/viewport_pane.hpp"
#include "ui/materials_panel.hpp"
#include "ui/face_properties_panel.hpp"
#include "ui/game_content.hpp"
#include "ui/inspector_panel.hpp"

namespace apex::ui {
namespace {

// The event's reported global position can have a different synthetic origin on Wayland.
// Map before resizing any pane: its receiving native window may move during this step.
std::optional<QPointF> receiverGlobal(QObject* receiver, QPointF local) {
    if (auto* window = qobject_cast<QWindow*>(receiver)) return window->mapToGlobal(local);
    if (auto* widget = qobject_cast<QWidget*>(receiver)) return widget->mapToGlobal(local);
    return std::nullopt;
}

void setInputTransparent(QWindow& window, bool transparent) {
    window.setFlag(Qt::WindowTransparentForInput, transparent);
    bool committed = false;
#ifdef APEX_HAS_WAYLAND_POINTER_LOCK
    // Qt 6.10 updates wl_surface's pending input region in setWindowFlags, but does not
    // commit it. Do so now: throttled Vulkan presents must not delay input passthrough.
    if (auto* native = QGuiApplication::platformNativeInterface()) {
        auto* surface = static_cast<wl_surface*>(native->nativeResourceForWindow("surface", &window));
        if (surface) {
            wl_surface_commit(surface);
            committed = true;
            if (auto* display = static_cast<wl_display*>(native->nativeResourceForIntegration("wl_display")))
                wl_display_flush(display);
        }
    }
#endif
    if (inputLogEnabled())
        inputTraceEvent("splitter_input", {{"window_id", QString::number(reinterpret_cast<quintptr>(&window), 16)},
                                            {"transparent", transparent}, {"wayland_commit", committed}});
}

void clearColourFor(const Tokens& t, float out[4]) {
    out[0] = static_cast<float>(t.base.redF());
    out[1] = static_cast<float>(t.base.greenF());
    out[2] = static_cast<float>(t.base.blueF());
    out[3] = 1.0f;
}

bool fullyCollapsed(const viewport::LayoutNode& node) {
    if (node.leaf) return node.collapsed;
    return std::ranges::all_of(node.children, fullyCollapsed);
}

}  // namespace

class AreaSplitter final : public QSplitter {
public:
    AreaSplitter(Qt::Orientation orientation, ViewportArea* area) : QSplitter(orientation), area_(area) {}
    using QSplitter::moveSplitter;
    ViewportArea* area() const { return area_; }
    viewport::NodeId node = 0;
    bool locked = false;  // One side is a collapsed strip: the bar stays put.

protected:
    QSplitterHandle* createHandle() override;

private:
    ViewportArea* area_;
};

class SplitHandle final : public QSplitterHandle {
public:
    SplitHandle(Qt::Orientation orientation, AreaSplitter* splitter) : QSplitterHandle(orientation, splitter), splitter_(splitter) {
        setMouseTracking(true);
    }

protected:
    void mousePressEvent(QMouseEvent* e) override {
        if (splitter_->locked) {
            e->accept();
            return;
        }
        if (e->button() == Qt::LeftButton &&
            splitter_->area()->beginHandleDrag(splitter_, this, *e)) {
            e->accept();
            return;
        }
        QSplitterHandle::mousePressEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (splitter_->area()->handleDragging() || splitter_->locked) return;
        if (e->buttons() == Qt::NoButton) {
            const bool junction = !splitter_->area()->junctionAt(splitter_, this, e->position()).empty();
            setCursor(junction ? Qt::SizeAllCursor : orientation() == Qt::Horizontal ? Qt::SplitHCursor : Qt::SplitVCursor);
        }
        QSplitterHandle::mouseMoveEvent(e);
    }

private:
    AreaSplitter* splitter_;
};

QSplitterHandle* AreaSplitter::createHandle() { return new SplitHandle(orientation(), this); }

ViewportArea::ViewportArea(QWidget* parent) : QWidget(parent) {
    inputTraceInitialize();
    setObjectName(QStringLiteral("ViewportArea"));
    setMinimumSize(320, 240);
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    host_ = new GpuHost(this);
    connect(host_, &GpuHost::gpuNameChanged, this, &ViewportArea::gpuNameChanged);
    timer_.setInterval(16);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &ViewportArea::tick);
    timer_.start();
    rebuild();
}

ViewportArea::~ViewportArea() {
    endHandleDrag();
    setEditorSession(nullptr);
    timer_.stop();
    for (auto& [role, window] : floating_) delete window.data();
    // Panes (and their swapchains) must go before the GPU host.
    delete body_;
    body_ = nullptr;
}

void ViewportArea::setDocument(document::Document* document, const assets::GameProfile* profile) {
    document_ = document;
    host_->setDocument(document, gameContent_ ? gameContent_->vfs() : nullptr, profile);
    if (session_) {
        session_->setEntityBoundsProvider([host = host_](const kv1::Node& entity) {
            return host->modelPicks() ? host->modelPicks()->shape(entity) : std::nullopt;
        });
    }
    previewRevision_ = kNoPreview;  // The mirror may be new.
    updateHighlight();
    forceDraw_ = true;
    for (auto& [id, f] : frames_)
        if (f && f->window()) f->window()->requestRedraw();
    for (auto& [id, p] : panels_) setPanelDocument(p, document_);
    for (auto& [role, w] : floating_)
        if (w) setPanelDocument(w->panel(), document_);
}

void ViewportArea::setEditorSession(editor::EditorSession* session) {
    if (session_) {
        session_->unobserve(sessionObserver_);
        session_->setEntityBoundsProvider({});
    }
    session_ = session;
    previewRevision_ = kNoPreview;
    if (session_) session_->setEntityBoundsProvider([host = host_](const kv1::Node& entity) {
        return host->modelPicks() ? host->modelPicks()->shape(entity) : std::nullopt;
    });
    if (session_) sessionObserver_ = session_->observe([this] {
        updateHighlight();
        updateModeBadges();
    });
    for (auto& [id, frame] : frames_)
        if (frame && frame->window()) frame->window()->setEditorSession(session_);
    for (auto& [id, p] : panels_) setPanelSession(p, session_);
    for (auto& [role, w] : floating_)
        if (w) setPanelSession(w->panel(), session_);
    updateHighlight();
    updateModeBadges();
}

void ViewportArea::setGameContent(GameContent* content) {
    gameContent_ = content;
    for (auto& [id, panel] : panels_) setPanelGameContent(panel, content);
    for (auto& [role, window] : floating_) if (window) setPanelGameContent(window->panel(), content);
}
void ViewportArea::setMaterialBrowser(std::function<void(QString, QString, std::function<void(QString)>)> browser) {
    materialBrowser_ = std::move(browser);
    for (auto& [id, panel] : panels_) configurePanel(panel);
    for (auto& [role, window] : floating_) if (window) configurePanel(window->panel());
}
void ViewportArea::setOpenInstanceHandler(std::function<bool(document::NodeId)> resolved,
                                          std::function<void(document::NodeId)> open) {
    instanceResolved_ = std::move(resolved);
    openInstance_ = std::move(open);
    for (auto& [id, panel] : panels_) configurePanel(panel);
    for (auto& [role, window] : floating_) if (window) configurePanel(window->panel());
}
void ViewportArea::configurePanel(QWidget* panel) {
    if (auto* materials = qobject_cast<MaterialsPanel*>(panel)) {
        connect(materials, &MaterialsPanel::activeMaterialRequested, this,
                &ViewportArea::activeMaterialRequested, Qt::UniqueConnection);
        connect(materials, &MaterialsPanel::statusMessage, this,
                &ViewportArea::materialStatusMessage, Qt::UniqueConnection);
    }
    if (auto* inspector = qobject_cast<InspectorPanel*>(panel)) {
        inspector->setMaterialBrowser(materialBrowser_);
        inspector->setOpenInstanceHandler(instanceResolved_, openInstance_);
    }
    if (auto* properties = qobject_cast<FacePropertiesPanel*>(panel)) {
        properties->setMaterialBrowser(materialBrowser_);
        properties->setGameContent(gameContent_);
        connect(properties, &FacePropertiesPanel::statusMessage, this,
                &ViewportArea::materialStatusMessage, Qt::UniqueConnection);
    }
}
QWidget* ViewportArea::makePanel(viewport::AreaRole role) {
    auto* panel = createPanel(role, document_, session_, gameContent_);
    configurePanel(panel);
    return panel;
}

void ViewportArea::setRole(viewport::NodeId pane, viewport::AreaRole role) {
    if (layout_.role(pane) == role || !layout_.setRole(pane, role)) return;
    scheduleRebuild();
    emit layoutChanged();
}

void ViewportArea::setCollapsed(viewport::NodeId pane, bool collapsed) {
    if (layout_.collapsed(pane) == collapsed) return;
    if (!layout_.setCollapsed(pane, collapsed)) {
        log::info("areas: the last expanded area cannot be collapsed");
        return;
    }
    scheduleRebuild();
    emit layoutChanged();
}

QWidget* ViewportArea::panel(viewport::NodeId pane) const {
    const auto it = panels_.find(pane);
    return it == panels_.end() ? nullptr : it->second.data();
}

FloatingPanel* ViewportArea::floating(viewport::AreaRole role) const {
    const auto it = floating_.find(role);
    return it == floating_.end() ? nullptr : it->second.data();
}

QWidget* ViewportArea::showRole(viewport::AreaRole role, bool window) {
    if (role == viewport::AreaRole::Viewport) return nullptr;
    if (const auto ids = layout_.paneIds(role); !window && !ids.empty()) {
        const auto pane = ids.front();
        bool changed = false;
        if (layout_.collapsed(pane)) changed = layout_.setCollapsed(pane, false).has_value();
        if (const auto max = layout_.maximized(); max && *max != pane) {
            layout_.restore();
            changed = true;
        }
        if (changed) {
            rebuild();
            emit layoutChanged();
        }
        auto* shown = panel(pane);
        if (shown) shown->setFocus(Qt::OtherFocusReason);
        return shown;
    }
    auto* w = popOut(role);
    return w ? w->panel() : nullptr;
}

FloatingPanel* ViewportArea::popOut(viewport::AreaRole role) {
    if (role == viewport::AreaRole::Viewport) return nullptr;  // Native Vulkan windows stay docked (D15).
    auto& slot = floating_[role];
    if (!slot) slot = new FloatingPanel(role, makePanel(role), window());
    slot->show();
    slot->raise();
    slot->activateWindow();
    return slot;
}

void ViewportArea::updateModeBadges() {
    const QString text = session_ ? modeSummary(*session_) : QString();
    const bool entity = session_ && session_->activeToolObject().readout().warn;
    for (auto& [id, frame] : frames_)
        if (frame) {
            frame->setModeText(text, entity);
            frame->setEditorSession(session_);
            if (session_) frame->setTool(session_->activeToolObject());
        }
}

void ViewportArea::updateHighlight() {
    render::Highlight highlight;
    if (session_ && session_->document()) {
        auto expanded = editor::expandSelection(*session_->document(), session_->groups(), session_->selection());
        highlight.selected = std::move(expanded.solidsAndEntities);
        highlight.selectedSides = std::move(expanded.sides);
        // E1 has one hover drawable: use the brush under the pointer, including
        // in Groups/Faces mode, rather than an arbitrary member of its selection.
        highlight.hover = session_->hoverDrawable();
    }
    if (auto* mirror = host_->mirror()) {
        if (session_) mirror->setInstanceContents(session_->instances().contents(), session_->instanceVisibility());
        mirror->setHighlight(highlight);
        syncPreview(*mirror);
    }
    forceDraw_ = true;
    for (auto& [id, frame] : frames_)
        if (frame && frame->window()) frame->window()->requestRedraw();
}

// The session owns the transient preview channel; the mirror only draws it. Equal revisions
// are skipped, and the session clearing it (commit, cancel) clears the mirror in the same call.
void ViewportArea::syncPreview(render::RenderMirror& mirror) {
    if (!session_) {
        if (previewRevision_ != kNoPreview) mirror.clearPreview();
        previewRevision_ = kNoPreview;
        return;
    }
    const auto& preview = session_->preview();
    const bool changed = preview.revision() != previewRevision_ || previewEmpty_ != preview.empty();
    if (!changed) return;
    if (preview.empty()) mirror.clearPreview();
    else {
        render::PreviewGeometry geometry;
        geometry.replacements = preview.replacements();
        for (const auto& ghost : preview.ghosts())
            geometry.ghosts.push_back({ghost.poly, ghost.material, ghost.opacity, ghost.outline});
        mirror.setPreview(preview.transforms(), geometry, preview.revision());
    }
    previewRevision_ = preview.revision();
    previewEmpty_ = preview.empty();
}

void ViewportArea::setLayout(viewport::ViewportLayout layout) {
    layout_ = std::move(layout);
    rebuild();
    emit layoutChanged();
}

PaneFrame* ViewportArea::frame(viewport::NodeId pane) const {
    const auto it = frames_.find(pane);
    return it == frames_.end() ? nullptr : it->second.data();
}

std::optional<Ray> ViewportArea::pointerRay() const {
    if (pointerOverride_) return pointerOverride_;
    for (const auto& [id, f] : frames_) {
        if (f && f->window()) {
            if (const auto ray = f->window()->pointerRay()) return ray;
        }
    }
    return std::nullopt;
}

void ViewportArea::flush() {
    if (rebuildPending_) rebuild();
    tick();
}

void ViewportArea::scheduleRebuild() {
    if (rebuildPending_) return;
    rebuildPending_ = true;
    QMetaObject::invokeMethod(this, [this] { if (rebuildPending_) rebuild(); }, Qt::QueuedConnection);
}

double ViewportArea::pixelHeight() const {
    if (const auto* f = frame(layout_.active()); f && f->container()->height() > 0)
        return f->container()->height();
    for (const auto& [id, f] : frames_)
        if (f && f->container()->height() > 0) return f->container()->height();
    return 768.0;
}

void ViewportArea::rebuild() {
    endHandleDrag();
    rebuildPending_ = false;
    if (!gpuTried_) {
        gpuTried_ = true;
        gpuReady_ = host_->ensure();
        gpuReason_ = gpuReady_ ? QString() : host_->error();
        if (!gpuReady_) log::warn("viewports: no GPU rendering ({})", gpuReason_.toStdString());
    }
    // Take the panels back before the frames go; drop those whose leaf is gone or changed role.
    for (auto it = panels_.begin(); it != panels_.end();) {
        auto* p = it->second.data();
        const bool keep = p && layout_.hasPane(it->first) &&
                          p->property("areaRole").toString() == QLatin1String(viewport::nameOf(layout_.role(it->first)));
        if (p) {
            p->hide();
            p->setParent(this);
        }
        if (!keep) {
            delete p;
            it = panels_.erase(it);
        } else {
            ++it;
        }
    }
    // Clear first: destroying panes hides their windows, which cancels tool input and notifies
    // session observers; updateModeBadges must not reach frames that are mid-destruction.
    frames_.clear();
    delete body_;
    body_ = nullptr;

    const auto tree = layout_.tree();
    if (const auto max = layout_.maximized(); max && layout_.hasPane(*max)) {
        const auto find = [](const auto& self, const viewport::LayoutNode& n, viewport::NodeId id) -> const viewport::LayoutNode* {
            if (n.id == id) return &n;
            for (const auto& c : n.children)
                if (const auto* hit = self(self, c, id)) return hit;
            return nullptr;
        };
        body_ = makeArea(*find(find, tree, *max), true, true, false);
    } else {
        body_ = build(tree, layout_.paneCount() > 1, false);
    }
    layout()->addWidget(body_);
    body_->show();
    setActive(layout_.active());
    forceDraw_ = true;
}

QWidget* ViewportArea::build(const viewport::LayoutNode& node, bool canClose, bool verticalStrip) {
    if (node.leaf) return makeArea(node, false, canClose, verticalStrip);
    const bool horizontal = node.orientation == viewport::Orientation::Horizontal;
    auto* splitter = new AreaSplitter(horizontal ? Qt::Horizontal : Qt::Vertical, this);
    splitter->node = node.id;
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(currentTokens().splitterWidth);
    const viewport::NodeId id = node.id;
    const bool folded[2] = {fullyCollapsed(node.children[0]), fullyCollapsed(node.children[1])};
    for (int i = 0; i < 2; ++i) {
        // A side that collapses entirely, beside one that does not, folds along this split's axis.
        const bool strip = folded[i] && !folded[1 - i] ? horizontal : verticalStrip;
        splitter->addWidget(build(node.children[static_cast<std::size_t>(i)], canClose, strip));
    }
    const int total = 10000;
    const int first = std::clamp(static_cast<int>(node.ratio * total), 1, total - 1);
    if (folded[0] != folded[1]) {
        splitter->locked = true;  // The ratio is kept for when the side expands again.
        splitter->setSizes(folded[0] ? QList<int>{0, total} : QList<int>{total, 0});
        return splitter;
    }
    splitter->setSizes({first, total - first});
    connect(splitter, &QSplitter::splitterMoved, this, [this, splitter, id] {
        const auto sizes = splitter->sizes();
        if (sizes.size() != 2 || sizes[0] + sizes[1] <= 0) return;
        const double ratio = static_cast<double>(sizes[0]) / (sizes[0] + sizes[1]);
        if (layout_.setRatio(id, ratio)) emit layoutChanged();
        forceDraw_ = true;
    });
    return splitter;
}

namespace {
int along(const QSplitter* s, QPointF p) { return qRound(s->orientation() == Qt::Horizontal ? p.x() : p.y()); }
int handleStart(const QSplitter* s, int index) {
    const auto* h = s->handle(index);
    return s->orientation() == Qt::Horizontal ? h->x() : h->y();
}
}  // namespace

std::vector<AreaSplitter*> ViewportArea::junctionAt(AreaSplitter* splitter, QSplitterHandle* handle, QPointF local) const {
    constexpr int kReach = 8;
    std::vector<AreaSplitter*> out, others;
    // The bar of `splitter` runs across its stacking axis; a child bar crosses it where the child's
    // own handle sits, measured along the bar.
    const bool useX = splitter->orientation() == Qt::Vertical;
    const QPoint pointer = handle->mapTo(splitter, local.toPoint());
    for (int i = 0; i < splitter->count(); ++i) {
        auto* child = dynamic_cast<AreaSplitter*>(splitter->widget(i));
        if (!child || child->orientation() == splitter->orientation() || child->count() < 2) continue;
        const QPoint bar = child->mapTo(splitter, child->handle(1)->geometry().center());
        const bool near = std::abs((useX ? pointer.x() : pointer.y()) - (useX ? bar.x() : bar.y())) <= kReach;
        if (near && out.empty()) out.push_back(child);
        else if (near) out.insert(out.begin(), child);
        else others.push_back(child);
    }
    // Grabbing one crossing takes every bar across this one: they all end up aligned.
    if (!out.empty()) out.insert(out.end(), others.begin(), others.end());
    return out;
}

bool ViewportArea::beginHandleDrag(AreaSplitter* splitter, QSplitterHandle* handle, const QMouseEvent& event) {
    if (drag_ || !splitter || splitter->count() != 2) return false;
    HandleDrag drag;
    drag.splitter = splitter;
    drag.index = splitter->indexOf(handle);
    if (drag.index <= 0) return false;
    drag.startPos = handleStart(splitter, drag.index);
    drag.startGlobal = handle->mapToGlobal(event.position());
    for (auto* cross : junctionAt(splitter, handle, event.position())) {
        drag.crossers.push_back(cross);
        drag.crosserStart.push_back(handleStart(cross, 1));
    }
    if (drag.crossers.empty() && !event.modifiers().testFlag(Qt::ShiftModifier)) {
        // Bars of opposed splits stay aligned, as in a Hammer quad.
        if (auto* parent = dynamic_cast<AreaSplitter*>(splitter->parentWidget());
            parent && parent->orientation() != splitter->orientation()) {
            for (int i = 0; i < parent->count(); ++i) {
                auto* sibling = dynamic_cast<AreaSplitter*>(parent->widget(i));
                if (sibling && sibling != splitter && sibling->orientation() == splitter->orientation() && sibling->count() == 2)
                    drag.partner = sibling;
            }
        }
    }
    drag_ = std::move(drag);
    const bool wayland = QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
    for (auto& [id, frame] : frames_) {
        if (!frame || !frame->window()) continue;
        auto* window = frame->window();
        window->setResizeDragging(true);
        // No non-popup grab exists on Wayland. Keep the press surface receiving motion
        // over the native children, without hiding/reparenting their Vulkan surfaces.
        if (wayland && !window->flags().testFlag(Qt::WindowTransparentForInput)) {
            drag_->inputTransparent.push_back(window);
            setInputTransparent(*window, true);
        }
    }
    qApp->installEventFilter(this);
    if (auto input = captureHandleDrag(handle, event))
        applyHandleDrag(std::move(*input)); // Also records the press and initial legal range.
    return true;
}

void ViewportArea::syncRatio(AreaSplitter* splitter) {
    const auto sizes = splitter->sizes();
    if (sizes.size() != 2 || sizes[0] + sizes[1] <= 0) return;
    if (layout_.setRatio(splitter->node, static_cast<double>(sizes[0]) / (sizes[0] + sizes[1]))) emit layoutChanged();
    forceDraw_ = true;
}

void ViewportArea::alignRatio(AreaSplitter* splitter, double ratio) {
    const int total = 10000;
    const int first = std::clamp(static_cast<int>(ratio * total), 1, total - 1);
    splitter->setSizes({first, total - first});
    syncRatio(splitter);
}

std::optional<ViewportArea::HandleDragInput> ViewportArea::captureHandleDrag(
    QObject* receiver, const QMouseEvent& event) const {
    const auto global = receiverGlobal(receiver, event.position());
    if (!global) return std::nullopt;
    HandleDragInput input{*global, {}};
    const bool logging = inputLogEnabled();
    if (logging) {
        const auto origin = receiverGlobal(receiver, {}).value_or(QPointF());
        auto* window = qobject_cast<QWindow*>(receiver);
        if (auto* widget = qobject_cast<QWidget*>(receiver)) window = widget->window()->windowHandle();
        const auto windowOrigin = window ? window->mapToGlobal(QPointF()) : origin;
        const auto xy = [](QPointF p) { return QJsonObject{{"x", p.x()}, {"y", p.y()}}; };
        input.trace = {{"event", event.type() == QEvent::MouseButtonPress ? "press" :
                                  event.type() == QEvent::MouseButtonRelease ? "release" : "move"},
                       {"event_ms", static_cast<qint64>(event.timestamp())},
                       {"receiver_class", QString::fromLatin1(receiver->metaObject()->className())},
                       {"receiver_name", receiver->objectName()},
                       {"receiver_id", QString::number(reinterpret_cast<quintptr>(receiver), 16)},
                       {"global", xy(event.globalPosition())}, {"local", xy(event.position())},
                       {"receiver_origin", xy(origin)}, {"window_origin", xy(windowOrigin)},
                       {"mapped_global", xy(*global)}};
    }
    return input;
}

void ViewportArea::applyHandleDrag(HandleDragInput input, std::uint32_t coalesced) {
    auto& drag = *drag_;
    if (!drag.splitter) return;
    QElapsedTimer applyTimer;
    applyTimer.start();
    const bool logging = inputLogEnabled();
    struct Move { AreaSplitter* splitter; int index; int requested; int result; int minimum; int maximum; };
    std::vector<Move> moves;
    const auto move = [&](AreaSplitter* splitter, int index, int requested) {
        splitter->moveSplitter(requested, index);
        if (logging) {
            int minimum = 0, maximum = 0;
            splitter->getRange(index, &minimum, &maximum);
            moves.push_back({splitter, index, requested, handleStart(splitter, index), minimum, maximum});
        }
    };
    const QPointF delta = input.global - drag.startGlobal;
    move(drag.splitter, drag.index, drag.startPos + along(drag.splitter, delta));
    syncRatio(drag.splitter);
    for (std::size_t i = 0; i < drag.crossers.size(); ++i) {
        auto* cross = drag.crossers[i].data();
        if (!cross) continue;
        move(cross, 1, drag.crosserStart[i] + along(cross, delta));
        syncRatio(cross);
        if (i != 0 && drag.crossers[0]) {
            const auto sizes = drag.crossers[0]->sizes();
            if (sizes.size() == 2 && sizes[0] + sizes[1] > 0)
                alignRatio(cross, static_cast<double>(sizes[0]) / (sizes[0] + sizes[1]));
        }
    }
    if (drag.partner) {
        const auto sizes = drag.splitter->sizes();
        if (sizes.size() == 2 && sizes[0] + sizes[1] > 0)
            alignRatio(drag.partner, static_cast<double>(sizes[0]) / (sizes[0] + sizes[1]));
    }
    if (logging) {
        QJsonArray loggedMoves;
        for (const auto& m : moves) {
            loggedMoves.append(QJsonObject{{"splitter_id", QString::number(reinterpret_cast<quintptr>(m.splitter), 16)},
                {"axis", m.splitter->orientation() == Qt::Horizontal ? "x" : "y"}, {"index", m.index},
                {"requested", m.requested}, {"result", m.result},
                {"range", QJsonArray{m.minimum, m.maximum}}});
        }
        input.trace["moves"] = loggedMoves;
        input.trace["coalesced"] = static_cast<qint64>(coalesced);
        input.trace["apply_ms"] = static_cast<double>(applyTimer.nsecsElapsed()) / 1.0e6;
        inputTraceEvent("splitter", std::move(input.trace));
    }
}

void ViewportArea::endHandleDrag(const char* reason) {
    if (!drag_) return;
    if (inputLogEnabled()) inputTraceEvent("splitter_end", {{"reason", reason}});
    for (const auto& window : drag_->inputTransparent)
        if (window) setInputTransparent(*window, false);
    drag_.reset();
    for (auto& [id, frame] : frames_)
        if (frame && frame->window()) frame->window()->setResizeDragging(false);
    forceDraw_ = true;
    qApp->removeEventFilter(this);
    emit layoutChanged();
}

bool ViewportArea::eventFilter(QObject* watched, QEvent* event) {
    if (!drag_) return QWidget::eventFilter(watched, event);
    switch (event->type()) {
    case QEvent::MouseMove: {
        const auto* move = static_cast<QMouseEvent*>(event);
        if (!move->buttons().testFlag(Qt::LeftButton)) {  // The release was lost: stop following.
            endHandleDrag("left-button-lost");
            return false;
        }
        if (inputLogEnabled()) inputTraceMouse(watched, *move); // This filter consumes the move.
        if (auto input = captureHandleDrag(watched, *move)) {
            drag_->pending = std::move(*input);
            ++drag_->pendingMoves;
        }
        return true;
    }
    case QEvent::MouseButtonRelease:
        if (const auto* release = static_cast<QMouseEvent*>(event); release->button() == Qt::LeftButton) {
            // A coalesced final move may arrive only as the release position.
            const auto coalesced = std::exchange(drag_->pendingMoves, 0) + 1;
            drag_->pending.reset();
            if (auto input = captureHandleDrag(watched, *release)) applyHandleDrag(std::move(*input), coalesced);
            endHandleDrag("release");
        }
        return false;  // Still delivered, so Qt's own press tracking ends cleanly.
    case QEvent::ApplicationDeactivate:
        endHandleDrag("application-deactivated");
        return false;
    case QEvent::Hide:
        if (watched == this || watched == window() || watched == window()->windowHandle())
            endHandleDrag("area-hidden");
        return false;
    case QEvent::KeyPress:
        if (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            endHandleDrag("escape");
            return true;
        }
        return false;
    default:
        return false;
    }
}

// The area signals PaneFrame and PanelFrame share.
template <typename Frame> void ViewportArea::connectArea(Frame* frame) {
    connect(frame, &Frame::splitRequested, this, [this](viewport::NodeId p, viewport::Orientation o) {
        if (layout_.split(p, o)) {
            scheduleRebuild();
            emit layoutChanged();
        }
    });
    connect(frame, &Frame::closeRequested, this, [this](viewport::NodeId p) {
        if (layout_.close(p)) {
            scheduleRebuild();
            emit layoutChanged();
        }
    });
    connect(frame, &Frame::maximizeToggled, this, [this](viewport::NodeId p) {
        layout_.toggleMaximize(p);
        scheduleRebuild();
        emit layoutChanged();
    });
    connect(frame, &Frame::roleRequested, this, &ViewportArea::setRole);
    connect(frame, &Frame::collapseRequested, this, [this](viewport::NodeId p) { setCollapsed(p, true); });
}

QWidget* ViewportArea::makeArea(const viewport::LayoutNode& node, bool maximized, bool canClose, bool verticalStrip) {
    if (node.collapsed && !maximized) {
        if (node.role != viewport::AreaRole::Viewport) (void)panelFor(node.id, node.role);  // Kept hidden.
        auto* strip = new CollapsedStrip(node.id, node.role, verticalStrip);
        connect(strip, &CollapsedStrip::expandRequested, this, [this](viewport::NodeId p) { setCollapsed(p, false); });
        return strip;
    }
    if (node.role == viewport::AreaRole::Viewport) return makeFrame(node.id, maximized, canClose);
    auto* frame = new PanelFrame(node.id, node.role, panelFor(node.id, node.role), maximized, canClose);
    connectArea(frame);
    connect(frame, &PanelFrame::popOutRequested, this, [this](viewport::AreaRole role) { popOut(role); });
    return frame;
}

QWidget* ViewportArea::panelFor(viewport::NodeId pane, viewport::AreaRole role) {
    auto& slot = panels_[pane];
    if (!slot) slot = makePanel(role);
    return slot;
}

PaneFrame* ViewportArea::makeFrame(viewport::NodeId pane, bool maximized, bool canClose) {
    auto accessor = [this, pane]() -> viewport::Viewport* { return layout_.viewport(pane); };
    PaneWindow* window = nullptr;
    if (gpuReady_) {
        window = new PaneWindow(*host_, pane, accessor);
        window->setEditorSession(session_);
        float clear[4];
        clearColourFor(currentTokens(), clear);
        window->setClearColour(clear);
    }
    auto* frame = new PaneFrame(pane, accessor, window, gpuReason_, maximized, canClose);
    frame->setEditorSession(session_);
    frames_[pane] = frame;
    if (session_) {
        frame->setModeText(modeSummary(*session_), session_->activeToolObject().readout().warn);
        frame->setTool(session_->activeToolObject());
    }
    connect(frame, &PaneFrame::activated, this, &ViewportArea::setActive);
    connect(frame, &PaneFrame::edited, this, [this](viewport::NodeId) { emit layoutChanged(); });
    connectArea(frame);
    if (window) connect(window, &PaneWindow::cameraChanged, this, &ViewportArea::emitCamera);
    return frame;
}

void ViewportArea::setActive(viewport::NodeId pane) {
    if (layout_.hasPane(pane)) (void)layout_.setActive(pane);
    for (auto& [id, f] : frames_)
        if (f) f->setActive(id == layout_.active());
    lastValid_ = false;
    emitCamera();
}

void ViewportArea::emitCamera() {
    const auto* vp = layout_.viewport(layout_.active());
    if (!vp) return;
    const auto& p = vp->camera.position;
    if (lastValid_ && lastCam_[0] == p.x && lastCam_[1] == p.y && lastCam_[2] == p.z) return;
    lastValid_ = true;
    lastCam_[0] = p.x; lastCam_[1] = p.y; lastCam_[2] = p.z;
    emit cameraPositionChanged(p.x, p.y, p.z);
}

void ViewportArea::tick() {
    if (rebuildPending_) return;
    if (drag_ && drag_->pending) {
        auto input = std::move(*drag_->pending);
        const auto coalesced = std::exchange(drag_->pendingMoves, 0);
        drag_->pending.reset();
        applyHandleDrag(std::move(input), coalesced);
    }
    if (gpuReady_ && !gpuAnnounced_) {
        gpuAnnounced_ = true;
        emit gpuNameChanged(host_->gpuName());
    }
    const auto pickRevision = host_->modelPickRevision();
    if (gpuReady_ && host_->pump()) forceDraw_ = true;
    if (session_ && pickRevision != host_->modelPickRevision()) session_->refreshEntityBounds();
    const bool force = forceDraw_ || drag_.has_value();
    forceDraw_ = false;
    for (auto& [id, f] : frames_) {
        if (!f || !f->window()) continue;
        if (force) f->window()->requestRedraw();
        const double ms = f->window()->tick(false);
        if (ms >= 0.0 && id == layout_.active()) emit frameTimeChanged(ms);
    }
    emitCamera();
}

void ViewportArea::resetCameras() {
    const viewport::Preset presets[] = {viewport::Preset::Camera, viewport::Preset::Top, viewport::Preset::Front,
                                      viewport::Preset::Side};
    std::size_t i = 0;
    for (const auto id : layout_.viewportIds()) {
        if (auto* vp = layout_.viewport(id)) *vp = viewport::makePreset(presets[i % 4]);
        ++i;
    }
    for (auto& [id, f] : frames_) {
        if (!f) continue;
        f->refreshTitle();
        if (f->window()) f->window()->requestRedraw();
    }
    lastValid_ = false;
    emitCamera();
    emit layoutChanged();
}

void ViewportArea::resetLayout() { setLayout(viewport::ViewportLayout::quad()); }

void ViewportArea::setThemeChanged() {
    float clear[4];
    clearColourFor(currentTokens(), clear);
    for (auto& [id, f] : frames_) {
        if (!f) continue;
        f->restyle();
        if (f->window()) f->window()->setClearColour(clear);
    }
    update();
}

QByteArray ViewportArea::layoutJson() const {
    const std::string text = json::write(layout_.toJson());
    return QByteArray(text.data(), static_cast<qsizetype>(text.size()));
}

bool ViewportArea::restoreLayoutJson(const QByteArray& bytes) {
    if (bytes.isEmpty()) return false;
    auto parsed = json::parse(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
    if (!parsed) {
        log::warn("viewports: saved layout unreadable: {}", parsed.error().what());
        return false;
    }
    auto restored = viewport::ViewportLayout::fromJson(*parsed);
    if (!restored) {
        log::warn("viewports: saved layout rejected: {}", restored.error().what());
        return false;
    }
    setLayout(std::move(*restored));
    return true;
}

void ViewportArea::applyMapViews(const viewport::MapViews& views) {
    viewport::applyMapViews(layout_, views, pixelHeight());
    for (auto& [id, f] : frames_) {
        if (!f) continue;
        f->refreshTitle();
        if (f->window()) f->window()->requestRedraw();
    }
    lastValid_ = false;
    emitCamera();
}

void ViewportArea::centerOn(const AABB& bounds) {
    for (const auto pane : layout_.viewportIds()) {
        auto* view = layout_.viewport(pane);
        if (!view) continue;
        double aspect = 1.0;
        if (const auto* f = frame(pane); f && f->container()->height() > 0)
            aspect = double(f->container()->width()) / f->container()->height();
        viewport::frameBounds(view->camera, bounds, aspect);
    }
    for (auto& [id, f] : frames_) {
        if (f && f->window()) f->window()->requestRedraw();
    }
    lastValid_ = false;
    emitCamera();
}

viewport::MapViews ViewportArea::captureMapViews() const {
    return viewport::captureMapViews(layout_, pixelHeight());
}

}  // namespace apex::ui
