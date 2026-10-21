#include "render/vk.hpp"  // volk before Qt's Vulkan header.

#include "ui/viewport_area.hpp"

#include <QSplitter>
#include <QVBoxLayout>
#include <algorithm>

#include "core/log.hpp"
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
    timer_.stop();
    // Panes (and their swapchains) must go before the GPU host.
    delete body_;
    body_ = nullptr;
}

void ViewportArea::setDocument(document::Document* document, const assets::GameProfile* profile) {
    document_ = document;
    host_->setDocument(document, profile);
    forceDraw_ = true;
    for (auto& [id, f] : frames_)
        if (f && f->window()) f->window()->requestRedraw();
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
    auto* splitter = new QSplitter(node.orientation == viewport::Orientation::Horizontal ? Qt::Horizontal : Qt::Vertical);
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

PaneFrame* ViewportArea::makeFrame(viewport::NodeId pane, bool maximized, bool canClose) {
    auto accessor = [this, pane]() -> viewport::Viewport* { return layout_.viewport(pane); };
    PaneWindow* window = nullptr;
    if (gpuReady_) {
        window = new PaneWindow(*host_, pane, accessor);
        float clear[4];
        clearColourFor(currentTokens(), clear);
        window->setClearColour(clear);
        window->setGridSpacing(static_cast<float>(gridSize_));
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

void ViewportArea::setGridSize(int units) {
    if (units < 1 || units == gridSize_) return;
    gridSize_ = units;
    for (auto& [id, f] : frames_)
        if (f && f->window()) f->window()->setGridSpacing(static_cast<float>(units));
    emit gridSizeChanged(units);
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
