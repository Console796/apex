#include "render/vk.hpp"  // volk before Qt's Vulkan header.

#include "ui/viewport_pane.hpp"

#include <QActionGroup>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVulkanInstance>
#include <algorithm>
#include <cmath>

#include "core/log.hpp"
#include "render/render_mirror.hpp"
#include "render/view_math.hpp"
#include "ui/gpu_host.hpp"
#include "ui/key_chord.hpp"
#include "viewport/pick_math.hpp"
#include "viewport/view_context.hpp"
#include "ui/theme.hpp"

namespace apex::ui {
namespace {

bool sameCamera(const viewport::Camera& a, const viewport::Camera& b) {
    return a.position == b.position && a.angles == b.angles && a.projection == b.projection &&
           a.fovYDegrees == b.fovYDegrees && a.orthoHeight == b.orthoHeight;
}

constexpr double kMaxDt = 0.1;  // Seconds; a hitch must not fling the fly camera.

QString viewName(const viewport::Viewport& vp) {
    const auto& c = vp.camera;
    if (!c.isOrtho()) return c.axisLocked() ? QStringLiteral("Perspective") : QStringLiteral("Camera");
    switch (c.axis) {
    case viewport::ViewAxis::NegZ: return QStringLiteral("Top");
    case viewport::ViewAxis::PosZ: return QStringLiteral("Bottom");
    case viewport::ViewAxis::PosY: return QStringLiteral("Front");
    case viewport::ViewAxis::NegY: return QStringLiteral("Back");
    case viewport::ViewAxis::NegX: return QStringLiteral("Side");
    case viewport::ViewAxis::PosX: return QStringLiteral("Side (rev)");
    case viewport::ViewAxis::Free: break;
    }
    return QStringLiteral("Ortho");
}

QString modeName(viewport::DisplayMode mode) {
    switch (mode) {
    case viewport::DisplayMode::Wireframe: return QStringLiteral("Wireframe");
    case viewport::DisplayMode::Flat: return QStringLiteral("Flat");
    case viewport::DisplayMode::Textured: return QStringLiteral("Textured");
    case viewport::DisplayMode::Lit: return QStringLiteral("Lit");
    case viewport::DisplayMode::Baked: return QStringLiteral("Baked");
    }
    return {};
}

render::DisplayMode renderMode(viewport::DisplayMode mode) {
    switch (mode) {
    case viewport::DisplayMode::Wireframe: return render::DisplayMode::Wireframe;
    case viewport::DisplayMode::Flat: return render::DisplayMode::Flat;
    default: return render::DisplayMode::Textured;  // Lit and Baked arrive in 0.6 / 0.7.
    }
}

// Container keys go to the pane window: on Wayland the embedded window never sees a key itself.
class KeyForwarder final : public QObject {
public:
    KeyForwarder(QWidget* container, PaneWindow* window) : QObject(container), window_(window) {
        container->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::FocusOut) window_->releaseCapture();
        if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease &&
            event->type() != QEvent::ShortcutOverride) return false;
        const auto* key = static_cast<QKeyEvent*>(event);
        QKeyEvent copy(key->type(), key->key(), key->modifiers(), key->text(), key->isAutoRepeat());
        copy.ignore(); // A fresh QKeyEvent starts accepted; only an explicit accept may claim the key.
        QCoreApplication::sendEvent(window_, &copy);
        if (event->type() == QEvent::ShortcutOverride) {
            if (copy.isAccepted()) event->accept();
            return copy.isAccepted();
        }
        return copy.isAccepted();
    }

private:
    PaneWindow* window_;
};

}  // namespace

render::GridSettings makeGridSettings(const viewport::Viewport& view, const editor::EditorSession* session) {
    render::GridSettings grid;
    grid.enabled = view.overlays.grid;
    grid.spacing = static_cast<float>(session ? session->gridSize() : editor::GridLadder::kDefault);
    const auto resolved = session ? session->resolvedWorkPlane() : std::nullopt;
    const bool axisOrtho = view.camera.isOrtho() && view.camera.axisLocked();
    // Hover anchoring is a perspective-view concept. Axis-locked ortho panes keep
    // their screen-aligned grid unless the shared session plane is pinned.
    if (resolved && (!axisOrtho || resolved->source == editor::WorkPlaneSource::Pinned)) {
        const auto& plane = resolved->plane;
        grid.origin = plane.origin;
        grid.normal = plane.normal;
        grid.uAxis = plane.uAxis;
        grid.vAxis = plane.vAxis;
        if (plane.bounds) {
            grid.bounds = render::GridRect{plane.bounds->min.x, plane.bounds->max.x,
                                          plane.bounds->min.y, plane.bounds->max.y};
        }
    } else if (axisOrtho) {
        const auto plane = viewport::makeWorkPlane(view.camera);
        grid.origin = plane.origin;
        grid.normal = plane.normal;
        grid.uAxis = plane.uAxis;
        grid.vAxis = plane.vAxis;
    }
    return grid;
}

// ---- PaneWindow ----

PaneWindow::PaneWindow(GpuHost& host, viewport::NodeId pane, ViewportAccessor accessor)
    : host_(host), pane_(pane), accessor_(std::move(accessor)),
      pointerLock_(*this, makePointerLockBackend(*this), [this] { releaseCapture(); }),
      depth_(*host.context()) {
    setSurfaceType(QSurface::VulkanSurface);
    setVulkanInstance(host.vulkanInstance());
    sinceTick_.start();
}

PaneWindow::~PaneWindow() {
    releaseCapture();
    destroySwapchain();
}

void PaneWindow::setClearColour(const float rgba[4]) {
    std::copy(rgba, rgba + 4, clear_);
    dirty_ = true;
}

void PaneWindow::setResizeDragging(bool dragging) {
    resizeDragging_ = dragging;
    if (swapchain_) swapchain_->setResizeDragging(dragging);
    dirty_ = true; // Release always renders the final extent, including a lost release.
}

QSize PaneWindow::pixelSize() const {
    return QSize(qRound(width() * devicePixelRatio()), qRound(height() * devicePixelRatio()));
}

void PaneWindow::destroySwapchain() {
    if (!swapchain_) {
        return;
    }
    host_.context()->waitIdle();
    swapchain_.reset();
    depth_ = render::SwapchainDepth(*host_.context());
}

void PaneWindow::cancelSelectionGesture() {
    selectionPending_ = marqueeDragging_ = false;
    gestureCamera_.reset();
    dirty_ = true;
}

void PaneWindow::releaseCapture() {
    cancelSelectionGesture();
    clickCamera_.reset();
    if (grabbed_) setMouseGrabEnabled(false);
    grabbed_ = false;
    pointerLock_.cancel();
    buttons_ = Qt::NoButton;
    delta_ = {};
    forward_ = back_ = left_ = right_ = up_ = down_ = false;
}

bool PaneWindow::event(QEvent* e) {
    switch (e->type()) {
    case QEvent::Expose:
    case QEvent::Resize:
    case QEvent::DevicePixelRatioChange:
        dirty_ = true;
        break;
    case QEvent::FocusOut:
    case QEvent::Hide:
        if (e->type() == QEvent::Hide) transform_.cancel();
        releaseCapture();
        forward_ = back_ = left_ = right_ = up_ = down_ = false;
        break;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseMove:
        return handleMouse(static_cast<QMouseEvent*>(e));
    case QEvent::Leave:
        pointerInside_ = false;
        if (session_ && !transform_.owns()) session_->clearHover();
        transform_.leave();
        if (transform_.takeDirty()) dirty_ = true;
        break;
    case QEvent::ShortcutOverride:
        // A gizmo drag or modal transform owns the keyboard: Esc, Enter, digits and axis keys
        // must not trigger window shortcuts.
        if (transform_.owns() || buttons_.testFlag(Qt::RightButton) || buttons_.testFlag(Qt::MiddleButton)) {
            e->accept();
            return true;
        }
        break;
    case QEvent::Wheel: {
        cancelSelectionGesture();
        clickCamera_.reset();
        if (session_) session_->clearHover();
        const auto* w = static_cast<QWheelEvent*>(e);
        scroll_ += w->angleDelta().y() / 120.0;
        modifiers_ = w->modifiers();
        cursor_ = w->position();
        return true;
    }
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
        return handleKey(static_cast<QKeyEvent*>(e));
    case QEvent::PlatformSurface:
        // Release the swapchain before Qt takes the surface away (hide/destroy).
        if (static_cast<QPlatformSurfaceEvent*>(e)->surfaceEventType() ==
            QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            transform_.cancel();
            releaseCapture();
            destroySwapchain();
        }
        break;
    default:
        break;
    }
    return QWindow::event(e);
}

std::optional<Ray> PaneWindow::pointerRay() const {
    const auto* vp = accessor_();
    if (!pointerInside_ || !vp || width() <= 0 || height() <= 0) return std::nullopt;
    return viewport::pixelRay(vp->camera, static_cast<double>(width()) / height(), cursor_.x(), cursor_.y(), width(), height());
}

bool PaneWindow::handleMouse(QMouseEvent* e) {
    modifiers_ = e->modifiers();
    const bool press = e->type() == QEvent::MouseButtonPress;
    const bool release = e->type() == QEvent::MouseButtonRelease;
    cursor_ = e->position();
    pointerInside_ = true;
    if (!press && !release && buttons_ != Qt::NoButton && (buttons_ & ~e->buttons()) != Qt::NoButton) {
        // A release outside the window is never delivered here: follow the real button state, or
        // the view would keep navigating when the pointer comes back.
        const auto lost = buttons_ & ~e->buttons();
        buttons_ &= e->buttons();
        pointerLock_.buttons(buttons_);
        if (lost.testFlag(Qt::LeftButton)) cancelSelectionGesture();
        if (buttons_ == Qt::NoButton && grabbed_) {
            setMouseGrabEnabled(false);
            grabbed_ = false;
        }
        last_ = e->position();
    }
    bool consumed = false;
    if (const auto* vp = accessor_(); vp && session_)
        consumed = transform_.mouse(*e, vp->camera, QSizeF(width(), height()));
    if (transform_.takeDirty()) dirty_ = true;
    if (consumed) {
        // The gizmo or a modal transform owns this event: never selection, marquee or navigation.
        cancelSelectionGesture();
        if (press) {
            requestActivate();
            emit pressed();
            if (!grabbed_) grabbed_ = setMouseGrabEnabled(true);
        } else if (release && e->buttons() == Qt::NoButton && grabbed_ && buttons_ == Qt::NoButton) {
            setMouseGrabEnabled(false);
            grabbed_ = false;
        }
        return true;
    }
    selectionMouse(e);
    if (press) {
        requestActivate();
        emit pressed();
        buttons_ = e->buttons();
        last_ = e->position();
        if (!grabbed_) grabbed_ = setMouseGrabEnabled(true);
        const auto* vp = accessor_();
        if (e->button() == Qt::MiddleButton ||
            (e->button() == Qt::RightButton && vp && !vp->camera.isOrtho()))
            pointerLock_.press(e->button(), e->position());
    } else if (release) {
        // A fast flick can finish between ticks. Apply its last relative events
        // while the navigation button still identifies orbit, pan or fly look.
        if (pointerLock_.active()) applyNavigation(0.0);
        buttons_ = e->buttons();
        pointerLock_.buttons(buttons_);
        if (buttons_ == Qt::NoButton && grabbed_) {
            setMouseGrabEnabled(false);
            grabbed_ = false;
        }
    } else if (buttons_ != Qt::NoButton) {
        if (pointerLock_.active()) pointerLock_.move(e->position());
        else delta_ += e->position() - last_;
        last_ = e->position();
    }
    return true;
}

void PaneWindow::selectionMouse(QMouseEvent* e) {
    auto* vp = accessor_();
    if (!session_ || !session_->document() || !vp || width() <= 0 || height() <= 0) return;
    const auto buttons = e->buttons();
    if (buttons.testFlag(Qt::RightButton) || buttons.testFlag(Qt::MiddleButton)) {
        cancelSelectionGesture();
        clickCamera_.reset();
        session_->clearHover();
        return;
    }
    // Releasing navigation must not become a hover or selection gesture.
    if (e->type() == QEvent::MouseButtonRelease && e->button() != Qt::LeftButton) return;
    const double aspect = static_cast<double>(width()) / height();
    const auto ray = viewport::pixelRay(vp->camera, aspect, cursor_.x(), cursor_.y(), width(), height());
    if (e->type() == QEvent::MouseButtonPress && e->button() == Qt::LeftButton) {
        selectionPending_ = true;
        selectionStart_ = cursor_;
        selectionModifiers_ = {e->modifiers().testFlag(Qt::ControlModifier), e->modifiers().testFlag(Qt::ShiftModifier)};
        gestureCamera_ = vp->camera;
        session_->pointerHover(ray);
        marqueeAllowed_ = !session_->hoverTarget() || selectionModifiers_.ctrl || selectionModifiers_.shift;
    } else if (e->type() == QEvent::MouseMove && selectionPending_) {
        if (!sameCamera(*gestureCamera_, vp->camera)) { cancelSelectionGesture(); return; }
        const auto delta = cursor_ - selectionStart_;
        if (marqueeAllowed_ && QPointF::dotProduct(delta, delta) > 9) {
            marqueeDragging_ = true;
            session_->clearHover();
            dirty_ = true;
        }
    } else if (e->type() == QEvent::MouseButtonRelease && selectionPending_) {
        if (sameCamera(*gestureCamera_, vp->camera)) {
            if (marqueeDragging_) {
                session_->marquee(viewport::rectFrustum(vp->camera, aspect, selectionStart_.x(), selectionStart_.y(),
                                                        cursor_.x(), cursor_.y(), width(), height()), selectionModifiers_);
                clickCamera_.reset();
            } else {
                const auto delta = cursor_ - lastClick_;
                const bool sameSpot = clickCamera_ && sameCamera(*clickCamera_, vp->camera) &&
                                      lastClickSize_ == size() && QPointF::dotProduct(delta, delta) <= 9;
                session_->pointerClick(ray, selectionModifiers_, sameSpot);
                lastClick_ = cursor_;
                lastClickSize_ = size();
                clickCamera_ = vp->camera;
            }
        }
        cancelSelectionGesture();
    } else if (e->type() == QEvent::MouseMove && buttons == Qt::NoButton) session_->pointerHover(ray);
}

void PaneWindow::drawMarquee(VkCommandBuffer commands, VkExtent2D extent) {
    if (!marqueeDragging_) return;
    const double scale = devicePixelRatio();
    const auto rect = QRectF(selectionStart_, cursor_).normalized();
    const int x0 = std::clamp(qRound(rect.left() * scale), 0, static_cast<int>(extent.width) - 1);
    const int x1 = std::clamp(qRound(rect.right() * scale), x0, static_cast<int>(extent.width) - 1);
    const int y0 = std::clamp(qRound(rect.top() * scale), 0, static_cast<int>(extent.height) - 1);
    const int y1 = std::clamp(qRound(rect.bottom() * scale), y0, static_cast<int>(extent.height) - 1);
    const auto w = static_cast<std::uint32_t>(x1 - x0 + 1), h = static_cast<std::uint32_t>(y1 - y0 + 1);
    const VkClearRect borders[] = {{{{x0, y0}, {w, 1}}, 0, 1}, {{{x0, y1}, {w, 1}}, 0, 1},
                                   {{{x0, y0}, {1, h}}, 0, 1}, {{{x1, y0}, {1, h}}, 0, 1}};
    VkClearAttachment colour{};
    colour.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    colour.clearValue.color = {{0.2f, 0.85f, 1.0f, 1.0f}};
    vkCmdClearAttachments(commands, 1, &colour, 4, borders);
}

bool PaneWindow::handleKey(QKeyEvent* e) {
    modifiers_ = e->modifiers();
    const bool down = e->type() == QEvent::KeyPress;
    const bool navigating = buttons_.testFlag(Qt::RightButton) || buttons_.testFlag(Qt::MiddleButton);
    if (session_) {
        const auto* vp = accessor_();
        const bool consumed = vp && transform_.key(*e, vp->camera, QSizeF(width(), height()), !navigating);
        if (transform_.takeDirty()) dirty_ = true;
        if (consumed) return true;
    }
    if (!navigating) {
        if (session_ && down && !e->isAutoRepeat()) {
            if (const auto chord = keyChord(*e)) {
                if (const auto action = session_->keymap().lookup(*chord, editor::BindingContext::SelectToolActive)) {
                    if (session_->perform(*action)) return true;
                    e->ignore(); // Window-level actions (cut, copy, paste) still get their shortcut.
                    return false;
                }
            }
        }
        forward_ = back_ = left_ = right_ = up_ = down_ = false;
        e->ignore();
        return false;
    }
    if (e->isAutoRepeat()) return true;
    switch (e->key()) {
    case Qt::Key_W: case Qt::Key_Up: forward_ = down; return true;
    case Qt::Key_S: case Qt::Key_Down: back_ = down; return true;
    case Qt::Key_A: case Qt::Key_Left: left_ = down; return true;
    case Qt::Key_D: case Qt::Key_Right: right_ = down; return true;
    case Qt::Key_E: up_ = down; return true;
    case Qt::Key_Q: down_ = down; return true;
    default: return false;
    }
}

void PaneWindow::applyNavigation(double dt) {
    delta_ += pointerLock_.takeDelta();
    if (auto* vp = accessor_()) {
        const bool keys = forward_ || back_ || left_ || right_ || up_ || down_;
        if (buttons_ != Qt::NoButton || keys || scroll_ != 0.0) {
            viewport::NavInput in;
            in.cursor = {cursor_.x(), cursor_.y()};
            in.cursorDelta = {delta_.x(), delta_.y()};
            in.viewportSize = {std::max(1, width()), std::max(1, height())};
            in.scroll = scroll_;
            in.dt = dt;
            in.left = buttons_.testFlag(Qt::LeftButton);
            in.middle = buttons_.testFlag(Qt::MiddleButton);
            in.right = buttons_.testFlag(Qt::RightButton);
            in.shift = modifiers_.testFlag(Qt::ShiftModifier);
            in.ctrl = modifiers_.testFlag(Qt::ControlModifier);
            in.alt = modifiers_.testFlag(Qt::AltModifier);
            in.moveForward = forward_; in.moveBack = back_; in.moveLeft = left_; in.moveRight = right_;
            in.moveUp = up_; in.moveDown = down_;
            if (viewport::navigate(vp->camera, in)) {
                cancelSelectionGesture();
                clickCamera_.reset();
                if (session_) session_->clearHover();
                dirty_ = true;
                emit cameraChanged();
            }
        }
    }
    delta_ = {};
    scroll_ = 0.0;
}

double PaneWindow::tick(bool forceDraw) {
    const double dt = std::min(sinceTick_.restart() / 1000.0, kMaxDt);
    applyNavigation(dt);
    if ((resizeDragging_ && !swapchain_) || (swapchain_ && !swapchain_->frameDue())) return -1.0;
    const bool retiring = swapchain_ && swapchain_->hasRetiredSwapchains();
    if ((!dirty_ && !forceDraw && !retiring) || !isExposed() || failed_) return -1.0;
    return draw();
}

double PaneWindow::draw() {
    auto* vp = accessor_();
    const QSize px = pixelSize();
    if (!vp || px.isEmpty()) return -1.0;
    render::Context& ctx = *host_.context();
    const VkExtent2D extent{static_cast<std::uint32_t>(px.width()), static_cast<std::uint32_t>(px.height())};
    if (!swapchain_) {
        const VkSurfaceKHR surface = QVulkanInstance::surfaceForWindow(this);
        if (!surface) return -1.0;  // Not created yet; retry on the next tick.
        auto created = render::Swapchain::create(ctx, surface, extent);
        if (!created) {
            log::error("viewports: swapchain for pane {}: {}", pane_, created.error().what());
            failed_ = true;
            return -1.0;
        }
        swapchain_ = std::move(*created);
        swapchain_->setResizeDragging(resizeDragging_);
    }
    render::Renderer* renderer = host_.renderer(swapchain_->format());
    if (!renderer) {
        failed_ = true;
        return -1.0;
    }

    const auto& camera = vp->camera;
    const double aspect = static_cast<double>(px.width()) / px.height();
    render::MirrorViewOptions options;
    options.mode = renderMode(vp->mode);
    const auto basis = camera.basis();
    options.view.viewProjection = Mat4(camera.projectionMatrix(aspect) * camera.viewMatrix());
    options.view.cameraPosition = camera.position;
    options.view.cameraRight = Vec3(basis.right);
    options.view.cameraUp = Vec3(basis.up);
    const bool lightBackground = (clear_[0] + clear_[1] + clear_[2]) / 3.0f > 0.5f;
    const float line = lightBackground ? 0.1f : 0.85f;
    options.wireColour = Vec4(line, line, line + (lightBackground ? 0.05f : 0.05f), 1.0f);
    options.edgeColour = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
    options.edgeOverlay = vp->mode == viewport::DisplayMode::Flat;
    options.showEntities = vp->overlays.entityHelpers;
    options.grid = makeGridSettings(*vp, session_);
    if (lightBackground) options.grid.colour = Vec4(0.25f, 0.25f, 0.3f, 0.8f);

    render::DrawList list;
    if (auto* mirror = host_.mirror()) {
        list = mirror->buildDrawList(options);
    } else {
        list.mode = options.mode;
        list.view = options.view;
        list.grid = options.grid;
    }
    transform_.appendOverlay(list, camera, QSizeF(width(), height()), devicePixelRatio());

    QElapsedTimer timer;
    timer.start();
    auto status = swapchain_->renderFrame(extent, [&](VkCommandBuffer cb, const render::SwapchainFrame& frame) {
        if (auto begun = depth_.begin(cb, frame, clear_); !begun) {
            log::error("viewports: depth target: {}", begun.error().what());
            failed_ = true;
            return;
        }
        renderer->record(cb, list);
        drawMarquee(cb, extent);
        depth_.end(cb);
    });
    if (!status) {
        log::error("viewports: frame failed on pane {}: {}", pane_, status.error().what());
        failed_ = true;
        return -1.0;
    }
    if (*status == render::FrameStatus::Skipped) return -1.0;  // Retry; dirty_ stays set.
    dirty_ = false;
    ++frames_;
    return timer.nsecsElapsed() / 1.0e6;
}

// ---- PaneFrame ----

PaneFrame::PaneFrame(viewport::NodeId pane, ViewportAccessor accessor, PaneWindow* window,
                     const QString& unavailableReason, bool maximized, bool canClose, QWidget* parent)
    : QFrame(parent), pane_(pane), accessor_(std::move(accessor)), window_(window),
      maximized_(maximized), canClose_(canClose) {
    setObjectName(QStringLiteral("ViewportPane"));
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(1, 1, 1, 1);
    column->setSpacing(0);

    auto* header = new QWidget;
    header->setObjectName(QStringLiteral("PaneHeader"));
    auto* row = new QHBoxLayout(header);
    row->setContentsMargins(8, 2, 2, 2);
    title_ = new QLabel;
    title_->setObjectName(QStringLiteral("PaneTitle"));
    title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);  // A long title must not set the pane's minimum width.
    row->addWidget(title_, 1);
    menuButton_ = new QToolButton;
    menuButton_->setObjectName(QStringLiteral("PaneMenuButton"));
    menuButton_->setText(QString(QChar(0x2630)));
    menuButton_->setToolTip(tr("Viewport options"));
    menuButton_->setPopupMode(QToolButton::InstantPopup);
    menuButton_->setAutoRaise(true);
    menu_ = new QMenu(menuButton_);
    menu_->setObjectName(QStringLiteral("PaneMenu"));
    menuButton_->setMenu(menu_);
    connect(menu_, &QMenu::aboutToShow, this, &PaneFrame::buildMenu);
    row->addWidget(menuButton_);
    column->addWidget(header);

    if (window_) {
        container_ = QWidget::createWindowContainer(window_, this);
        container_->setObjectName(QStringLiteral("PaneContainer"));
        container_->setFocusPolicy(Qt::StrongFocus);
        container_->setMinimumSize(80, 60);
        new KeyForwarder(container_, window_);
        connect(window_, &PaneWindow::pressed, this, [this] {
            container_->setFocus(Qt::MouseFocusReason);
            emit activated(pane_);
        });
        column->addWidget(container_, 1);
    } else {
        auto* label = new QLabel(unavailableReason.isEmpty() ? tr("Viewport unavailable") : unavailableReason);
        label->setObjectName(QStringLiteral("PaneUnavailable"));
        label->setAlignment(Qt::AlignCenter);
        label->setWordWrap(true);
        label->setMinimumSize(80, 60);
        container_ = label;
        column->addWidget(label, 1);
    }
    restyle();
    refreshTitle();
}

void PaneFrame::setActive(bool active) {
    if (active_ == active) return;
    active_ = active;
    restyle();
}

void PaneFrame::restyle() {
    const Tokens& t = currentTokens();
    setStyleSheet(QStringLiteral(
        "#ViewportPane { border: 1px solid %1; background: %2; }"
        "#PaneHeader { background: %2; }"
        "#PaneTitle { color: %3; }"
        "#PaneUnavailable { color: %3; background: %4; }")
        .arg((active_ ? t.accent : t.border).name(), t.panel.name(), (active_ ? t.text : t.textDim).name(),
             t.base.name()));
}

void PaneFrame::refreshTitle() {
    const auto* vp = accessor_();
    if (!vp) return;
    QString text = viewName(*vp) + QStringLiteral("  |  ") + modeName(vp->mode);
    if (maximized_) text += tr("  (maximized)");
    title_->setText(text);
}

void PaneFrame::buildMenu() {
    menu_->clear();
    auto* vp = accessor_();
    if (!vp) return;
    const auto edit = [this](auto&& change) {
        if (auto* v = accessor_()) {
            change(*v);
            refreshTitle();
            if (window_) window_->requestRedraw();
            emit edited(pane_);
        }
    };

    auto* preset = menu_->addMenu(tr("Preset"));
    for (const auto p : {viewport::Preset::Camera, viewport::Preset::Top, viewport::Preset::Front, viewport::Preset::Side}) {
        preset->addAction(QString::fromLatin1(viewport::nameOf(p)), this, [edit, p] {
            edit([p](viewport::Viewport& v) { viewport::applyPreset(v, p); });
        });
    }

    auto* projection = menu_->addMenu(tr("Projection"));
    auto* projGroup = new QActionGroup(projection);
    for (const auto kind : {viewport::ProjectionKind::Perspective, viewport::ProjectionKind::Orthographic}) {
        auto* a = projection->addAction(kind == viewport::ProjectionKind::Perspective ? tr("Perspective") : tr("Orthographic"),
                                        this, [edit, kind] {
            edit([kind](viewport::Viewport& v) {
                v.camera.projection = kind;
                v.camera.controller = kind == viewport::ProjectionKind::Orthographic ? viewport::ControllerKind::Pan2D
                                                                                       : viewport::ControllerKind::Fly;
            });
        });
        a->setCheckable(true);
        a->setChecked(vp->camera.projection == kind);
        projGroup->addAction(a);
    }

    auto* axis = menu_->addMenu(tr("Axis"));
    auto* axisGroup = new QActionGroup(axis);
    struct AxisEntry { viewport::ViewAxis axis; const char* label; };
    const AxisEntry axes[] = {{viewport::ViewAxis::Free, "Free"}, {viewport::ViewAxis::NegZ, "Top (-Z)"},
                              {viewport::ViewAxis::PosZ, "Bottom (+Z)"}, {viewport::ViewAxis::PosY, "Front (+Y)"},
                              {viewport::ViewAxis::NegY, "Back (-Y)"}, {viewport::ViewAxis::NegX, "Side (-X)"},
                              {viewport::ViewAxis::PosX, "Side (+X)"}};
    for (const auto& entry : axes) {
        auto* a = axis->addAction(tr(entry.label), this, [edit, ax = entry.axis] {
            edit([ax](viewport::Viewport& v) { viewport::setAxis(v.camera, ax); });
        });
        a->setCheckable(true);
        a->setChecked(vp->camera.axis == entry.axis);
        axisGroup->addAction(a);
    }

    auto* mode = menu_->addMenu(tr("Display mode"));
    auto* modeGroup = new QActionGroup(mode);
    for (const auto m : {viewport::DisplayMode::Wireframe, viewport::DisplayMode::Flat, viewport::DisplayMode::Textured}) {
        auto* a = mode->addAction(modeName(m), this, [edit, m] { edit([m](viewport::Viewport& v) { v.mode = m; }); });
        a->setCheckable(true);
        a->setChecked(vp->mode == m);
        modeGroup->addAction(a);
    }
    for (const auto m : {viewport::DisplayMode::Lit, viewport::DisplayMode::Baked}) {
        mode->addAction(modeName(m) + tr(" (later)"))->setEnabled(false);
    }

    auto* overlays = menu_->addMenu(tr("Overlays"));
    struct OverlayEntry { const char* label; bool viewport::Overlays::*field; };
    const OverlayEntry entries[] = {{"Grid", &viewport::Overlays::grid},
                                    {"Entity helpers", &viewport::Overlays::entityHelpers},
                                    {"Bounds", &viewport::Overlays::bounds},
                                    {"Normals", &viewport::Overlays::normals}};
    for (const auto& entry : entries) {
        auto* a = overlays->addAction(tr(entry.label));
        a->setCheckable(true);
        a->setChecked(vp->overlays.*(entry.field));
        connect(a, &QAction::toggled, this, [edit, field = entry.field](bool on) {
            edit([field, on](viewport::Viewport& v) { v.overlays.*field = on; });
        });
    }

    menu_->addSeparator();
    menu_->addAction(tr("Split horizontally (side by side)"), this,
                     [this] { emit splitRequested(pane_, viewport::Orientation::Horizontal); });
    menu_->addAction(tr("Split vertically (stacked)"), this,
                     [this] { emit splitRequested(pane_, viewport::Orientation::Vertical); });
    menu_->addSeparator();
    menu_->addAction(maximized_ ? tr("Restore") : tr("Maximize"), this, [this] { emit maximizeToggled(pane_); });
    auto* close = menu_->addAction(tr("Close pane"), this, [this] { emit closeRequested(pane_); });
    close->setEnabled(canClose_);
}

}  // namespace apex::ui
