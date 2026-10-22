#include "render/vk.hpp"  // volk before Qt's Vulkan header.

#include "ui/viewport_area.hpp"

#include <QApplication>
#include <QMouseEvent>
#include <QSplitter>
#include <QSplitterHandle>
#include <QVBoxLayout>
#include <algorithm>

#include "core/log.hpp"
#include "editor/selection_expand.hpp"
#include "ui/gpu_host.hpp"
#include "ui/theme.hpp"
#include "ui/viewport_pane.hpp"

namespace apex::ui {
namespace {

void clearColourFor(const Tokens& t, float out[4]) {
    out[0] = static_cast<float>(t.base.redF());
    out[1] = static_cast<float>(t.base.greenF());
    out[2] = static_cast<float>(t.base.blueF());
    out[3] = 1.0f;
}

}  // namespace

class AreaSplitter final : public QSplitter {
public:
    AreaSplitter(Qt::Orientation orientation, ViewportArea* area) : QSplitter(orientation), area_(area) {}
    using QSplitter::moveSplitter;
    ViewportArea* area() const { return area_; }
    viewport::NodeId node = 0;

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
        if (e->button() == Qt::LeftButton &&
            splitter_->area()->beginHandleDrag(splitter_, this, e->position(), e->globalPosition(), e->modifiers())) {
            e->accept();
            return;
        }
        QSplitterHandle::mousePressEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (splitter_->area()->handleDragging()) return;
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
    setObjectName(QStringLiteral("ViewportArea"));
    setMinimumSize(320, 240);
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    host_ = new GpuHost(this);
    connect(host_, &GpuHost::gpuNameChanged, this, &ViewportArea::gpuNameChanged);
    timer_.setInterval(16);
    connect(&timer_, &QTimer::timeout, this, &ViewportArea::tick);
    timer_.start();
    rebuild();
}

ViewportArea::~ViewportArea() {
    setEditorSession(nullptr);
    timer_.stop();
    // Panes (and their swapchains) must go before the GPU host.
    delete body_;
    body_ = nullptr;
}

void ViewportArea::setDocument(document::Document* document, const assets::GameProfile* profile) {
    document_ = document;
    host_->setDocument(document, profile);
    previewRevision_ = kNoPreview;  // The mirror may be new.
    updateHighlight();
    forceDraw_ = true;
    for (auto& [id, f] : frames_)
        if (f && f->window()) f->window()->requestRedraw();
}

void ViewportArea::setEditorSession(editor::EditorSession* session) {
    if (session_) session_->unobserve(sessionObserver_);
    session_ = session;
    previewRevision_ = kNoPreview;
    if (session_) sessionObserver_ = session_->observe([this] { updateHighlight(); });
    for (auto& [id, frame] : frames_)
        if (frame && frame->window()) frame->window()->setEditorSession(session_);
    updateHighlight();
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
        mirror->setHighlight(highlight);
        syncPreview(*mirror);
    }
    forceDraw_ = true;
    for (auto& [id, frame] : frames_)
        if (frame && frame->window()) frame->window()->requestRedraw();
}

// The session owns the transient transform channel; the mirror only draws it. Equal revisions
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
    else mirror.setPreview(preview.transforms());
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
    delete body_;
    body_ = nullptr;
    frames_.clear();

    const auto tree = layout_.tree();
    if (const auto max = layout_.maximized(); max && layout_.hasPane(*max)) {
        body_ = makeFrame(*max, true, true);
    } else {
        body_ = build(tree, layout_.paneCount() > 1);
    }
    layout()->addWidget(body_);
    body_->show();
    setActive(layout_.active());
    forceDraw_ = true;
}

QWidget* ViewportArea::build(const viewport::LayoutNode& node, bool canClose) {
    if (node.leaf) return makeFrame(node.id, false, canClose);
    auto* splitter = new AreaSplitter(node.orientation == viewport::Orientation::Horizontal ? Qt::Horizontal : Qt::Vertical, this);
    splitter->node = node.id;
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(currentTokens().splitterWidth);
    const viewport::NodeId id = node.id;
    for (const auto& child : node.children) splitter->addWidget(build(child, canClose));
    const int total = 10000;
    const int first = std::clamp(static_cast<int>(node.ratio * total), 1, total - 1);
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

bool ViewportArea::beginHandleDrag(AreaSplitter* splitter, QSplitterHandle* handle, QPointF local, QPointF global,
                                   Qt::KeyboardModifiers modifiers) {
    if (drag_ || !splitter || splitter->count() != 2) return false;
    HandleDrag drag;
    drag.splitter = splitter;
    drag.index = splitter->indexOf(handle);
    if (drag.index <= 0) return false;
    drag.startPos = handleStart(splitter, drag.index);
    drag.startGlobal = global;
    for (auto* cross : junctionAt(splitter, handle, local)) {
        drag.crossers.push_back(cross);
        drag.crosserStart.push_back(handleStart(cross, 1));
    }
    if (drag.crossers.empty() && !modifiers.testFlag(Qt::ShiftModifier)) {
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
    for (auto& [id, frame] : frames_)
        if (frame && frame->window()) frame->window()->setResizeDragging(true);
    qApp->installEventFilter(this);
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

void ViewportArea::applyHandleDrag(QPointF global) {
    auto& drag = *drag_;
    if (!drag.splitter) return;
    const QPointF delta = global - drag.startGlobal;
    drag.splitter->moveSplitter(drag.startPos + along(drag.splitter, delta), drag.index);
    syncRatio(drag.splitter);
    for (std::size_t i = 0; i < drag.crossers.size(); ++i) {
        auto* cross = drag.crossers[i].data();
        if (!cross) continue;
        cross->moveSplitter(drag.crosserStart[i] + along(cross, delta), 1);
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
}

void ViewportArea::endHandleDrag() {
    if (!drag_) return;
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
            endHandleDrag();
            return false;
        }
        applyHandleDrag(move->globalPosition());
        return true;
    }
    case QEvent::MouseButtonRelease:
        if (const auto* release = static_cast<QMouseEvent*>(event); release->button() == Qt::LeftButton) {
            // A coalesced final move may arrive only as the release position.
            applyHandleDrag(release->globalPosition());
            endHandleDrag();
        }
        return false;  // Still delivered, so Qt's own press tracking ends cleanly.
    default:
        return false;
    }
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
    frames_[pane] = frame;
    connect(frame, &PaneFrame::activated, this, &ViewportArea::setActive);
    connect(frame, &PaneFrame::edited, this, [this](viewport::NodeId) { emit layoutChanged(); });
    connect(frame, &PaneFrame::splitRequested, this, [this](viewport::NodeId p, viewport::Orientation o) {
        if (layout_.split(p, o)) {
            scheduleRebuild();
            emit layoutChanged();
        }
    });
    connect(frame, &PaneFrame::closeRequested, this, [this](viewport::NodeId p) {
        if (layout_.close(p)) {
            scheduleRebuild();
            emit layoutChanged();
        }
    });
    connect(frame, &PaneFrame::maximizeToggled, this, [this](viewport::NodeId p) {
        layout_.toggleMaximize(p);
        scheduleRebuild();
        emit layoutChanged();
    });
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
    if (gpuReady_ && !gpuAnnounced_) {
        gpuAnnounced_ = true;
        emit gpuNameChanged(host_->gpuName());
    }
    if (gpuReady_ && host_->pump()) forceDraw_ = true;
    const bool force = forceDraw_;
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
    for (const auto id : layout_.paneIds()) {
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

viewport::MapViews ViewportArea::captureMapViews() const {
    return viewport::captureMapViews(layout_, pixelHeight());
}

}  // namespace apex::ui
