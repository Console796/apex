#pragma once

#include <QElapsedTimer>
#include <QFrame>
#include <QPointF>
#include <QWindow>
#include <functional>
#include <memory>

#include "editor/editor_session.hpp"
#include "render/renderer.hpp"
#include "render/swapchain.hpp"
#include "ui/pointer_lock.hpp"
#include "ui/transform_input.hpp"
#include "viewport/controllers.hpp"
#include "viewport/layout.hpp"

class QLabel;
class QToolButton;
class QMenu;

namespace apex::ui {

class GpuHost;

render::GridSettings makeGridSettings(const viewport::Viewport& view, const editor::EditorSession* session);
// Red X, green Y and blue Z from the world origin while the grid overlay is on.
void appendWorldAxes(render::DrawList& list, const viewport::Viewport& view);

// The native Vulkan child window of one pane (plan 0.1, D15; pattern from spikes/wayland-vulkan).
// Renders on demand into its own swapchain through the shared Renderer and RenderMirror, and turns
// mouse and key input into viewport::NavInput for viewport::navigate.
//
// Never reparent or hide it casually: Qt destroys the VkSurfaceKHR silently on some platforms.
// The area rebuilds panes instead of moving them.
class PaneWindow final : public QWindow {
    Q_OBJECT
public:
    using ViewportAccessor = std::function<viewport::Viewport*()>;

    PaneWindow(GpuHost& host, viewport::NodeId pane, ViewportAccessor accessor);
    ~PaneWindow() override;

    viewport::NodeId paneId() const { return pane_; }
    void requestRedraw() { dirty_ = true; }
    void setClearColour(const float rgba[4]);
    void setResizeDragging(bool dragging);
    // One timer tick: applies queued navigation input, then draws when dirty (or `forceDraw`).
    // Returns the milliseconds a draw took, or a negative value when nothing was drawn.
    double tick(bool forceDraw);
    std::uint64_t frames() const { return frames_; }
    bool failed() const { return failed_; }
    std::uint32_t recreateCount() const { return swapchain_ ? swapchain_->recreateCount() : 0; }

    void releaseCapture();
    void setEditorSession(editor::EditorSession* session) {
        transform_.setSession(session);
        session_ = session;
        dirty_ = true;
    }
    TransformInput& transformInput() { return transform_; }
    // World-space ray under the pointer while it is over this pane; empty otherwise.
    std::optional<Ray> pointerRay() const;

signals:
    void pressed();             // Any mouse press: the pane becomes active and takes focus.
    void cameraChanged();       // Navigation moved the camera.

protected:
    bool event(QEvent* event) override;

private:
    bool handleMouse(QMouseEvent* event);
    void cancelSelectionGesture();
    void drawMarquee(VkCommandBuffer commands, VkExtent2D extent);
    bool handleKey(QKeyEvent* event);
    void applyNavigation(double dt);
    QSize pixelSize() const;
    void destroySwapchain();
    double draw();

    editor::EditorSession* session_ = nullptr;
    TransformInput transform_;
    bool pointerInside_ = false;

    GpuHost& host_;
    viewport::NodeId pane_;
    ViewportAccessor accessor_;
    PointerLock pointerLock_;

    std::unique_ptr<render::Swapchain> swapchain_;
    render::SwapchainDepth depth_;
    float clear_[4] = {0.1f, 0.1f, 0.1f, 1.0f};
    bool resizeDragging_ = false;
    bool dirty_ = true;
    bool failed_ = false;
    std::uint64_t frames_ = 0;

    // Input state accumulated between ticks.
    Qt::MouseButtons buttons_;
    Qt::KeyboardModifiers modifiers_;
    QPointF cursor_, last_;
    QPointF delta_;
    double scroll_ = 0.0;
    bool forward_ = false, back_ = false, left_ = false, right_ = false, up_ = false, down_ = false;
    bool grabbed_ = false;
    QElapsedTimer sinceTick_;
    QElapsedTimer traceClock_;
    qint64 lastCameraTraceMs_ = -100;
    std::uint32_t lastTraceRecreates_ = 0;
};

// "Select · Objects · Move" / "Entity · info_player_start": the editor's current mode, shown in
// the pane badge and the status bar.
QString modeSummary(const editor::EditorSession& session);

// A viewport area in the splitter tree: a header strip (role switcher, title, mode badge, collapse
// and the pane menu) above the Vulkan container. When no GPU is available the body is an
// explanatory label instead.
class PaneFrame final : public QFrame {
    Q_OBJECT
public:
    using ViewportAccessor = PaneWindow::ViewportAccessor;

    // `window` may be null (no GPU); the frame takes ownership of the window via its container.
    PaneFrame(viewport::NodeId pane, ViewportAccessor accessor, PaneWindow* window,
              const QString& unavailableReason, bool maximized, bool canClose, QWidget* parent = nullptr);

    viewport::NodeId paneId() const { return pane_; }
    PaneWindow* window() const { return window_; }
    QWidget* container() const { return container_; }
    void setActive(bool active);
    void refreshTitle();
    void setModeText(const QString& text, bool entityTool);
    void restyle();
    QMenu* menu() const { return menu_; }

signals:
    void edited(viewport::NodeId pane);  // The viewport settings changed (not the tree).
    void splitRequested(viewport::NodeId pane, viewport::Orientation orientation);
    void closeRequested(viewport::NodeId pane);
    void maximizeToggled(viewport::NodeId pane);
    void activated(viewport::NodeId pane);
    void roleRequested(viewport::NodeId pane, viewport::AreaRole role);
    void collapseRequested(viewport::NodeId pane);

private:
    void buildMenu();

    viewport::NodeId pane_;
    ViewportAccessor accessor_;
    PaneWindow* window_;
    QWidget* container_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* mode_ = nullptr;
    QToolButton* menuButton_ = nullptr;
    QMenu* menu_ = nullptr;
    bool active_ = false;
    bool maximized_;
    bool canClose_;
};

}  // namespace apex::ui
