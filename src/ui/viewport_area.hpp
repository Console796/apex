#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QPointF>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <map>
#include <optional>
#include <vector>

#include "viewport/layout.hpp"
#include "editor/editor_session.hpp"
#include "viewport/map_views.hpp"

namespace apex::render {
class RenderMirror;
}

class QSplitter;
class QSplitterHandle;
class QMouseEvent;
class QWindow;

namespace apex::assets {
struct GameProfile;
}
namespace apex::document {
class Document;
}

namespace apex::ui {

class GpuHost;
class PaneFrame;
class AreaSplitter;
class FloatingPanel;

// The window's area tree (plan 0.1, D15/D16; area roles 0.2.12): the viewport::ViewportLayout split
// tree mirrored into nested QSplitters. Each leaf takes a role, as in Blender: a Viewport leaf is a
// PaneFrame (header + native Vulkan window), any other role a PanelFrame around a panel widget,
// and a collapsed leaf a CollapsedStrip. Panels can also open in their own tool windows.
//
// Panel widgets are plain widgets and survive rebuilds (they are kept per leaf while its role
// stays), so a split or a collapse elsewhere keeps an outliner's expansion and scroll position.
// Structural changes (split, close, maximize, restoring a layout) rebuild the whole widget tree
// on the next event-loop turn instead of reparenting: native windows must not be reparented or
// hidden (the swapchain surface is destroyed silently), and rebuilding keeps the tree exact.
// Without Vulkan (offscreen platform, no device) the panes show an explanatory label instead.
class ViewportArea : public QWidget {
    Q_OBJECT
public:
    explicit ViewportArea(QWidget* parent = nullptr);
    ~ViewportArea() override;

    // Not owned; may be null. The profile (may be null) selects materials and FGDs.
    void setDocument(document::Document* document, const assets::GameProfile* profile = nullptr);
    document::Document* document() const { return document_; }
    void setEditorSession(editor::EditorSession* session);
    void updateHighlight();
    void updateModeBadges();

    // Area roles and collapse (rebuild on the next event-loop turn).
    void setRole(viewport::NodeId pane, viewport::AreaRole role);
    void setCollapsed(viewport::NodeId pane, bool collapsed);
    // Reveals a role: its first area (expanded, un-maximizing others), or, with `window` or when the
    // layout has none, its floating window. Returns the panel widget shown.
    QWidget* showRole(viewport::AreaRole role, bool window);
    FloatingPanel* popOut(viewport::AreaRole role);
    FloatingPanel* floating(viewport::AreaRole role) const;
    // The panel widget of a leaf (null for viewports, collapsed leaves keep theirs once created).
    QWidget* panel(viewport::NodeId pane) const;

    const viewport::ViewportLayout& viewportLayout() const { return layout_; }
    viewport::ViewportLayout& mutableLayout() { return layout_; }
    // Replaces the layout and rebuilds immediately.
    void setLayout(viewport::ViewportLayout layout);
    std::size_t paneCount() const { return layout_.paneCount(); }
    // The pane widget of a layout node (null while a rebuild is pending, or when hidden by maximize).
    PaneFrame* frame(viewport::NodeId pane) const;
    GpuHost& host() { return *host_; }

    // Layout persistence (the whole tree, per-pane cameras, active and maximized pane) as JSON.
    QByteArray layoutJson() const;
    bool restoreLayoutJson(const QByteArray& json);

    // Per-map views: the first four panes follow v0..v3 of the VMF's viewsettings.
    void applyMapViews(const viewport::MapViews& views);
    // Frames every viewport on `bounds`, keeping each camera's orientation.
    void centerOn(const AABB& bounds);
    viewport::MapViews captureMapViews() const;

    // The world ray under the mouse pointer when it is over a pane (Paste places at its work-plane
    // hit). The override stands in for a real pointer in tests and scripted input.
    std::optional<Ray> pointerRay() const;
    void setPointerRayOverride(std::optional<Ray> ray) { pointerOverride_ = ray; }

    // Runs queued rebuilds and one timer tick now (tests, screenshots).
    void flush();

    // Splitter handle drags (called by the area's own handles). The application event filter maps
    // each receiver's local coordinates and coalesces moves to the 16 ms tick; Wayland panes pass
    // input through during the drag.
    // Opposed splits of a quad move together (Shift keeps them independent), and a press
    // on the crossing of two bars drags both axes at once.
    bool beginHandleDrag(AreaSplitter* splitter, QSplitterHandle* handle, const QMouseEvent& event);
    // The perpendicular child splitters whose bar crosses `handle` near `local` (empty: no junction).
    std::vector<AreaSplitter*> junctionAt(AreaSplitter* splitter, QSplitterHandle* handle, QPointF local) const;
    bool handleDragging() const { return drag_.has_value(); }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

public slots:
    void resetCameras();   // Every pane back to the preset of its slot, centred on the origin.
    void resetLayout();    // Back to the quad layout.
    void setThemeChanged();

signals:
    // Status bar feeds: world-space camera position of the active viewport, frame time in
    // milliseconds, and the GPU name once a device is chosen.
    void cameraPositionChanged(double x, double y, double z);
    void frameTimeChanged(double milliseconds);
    void gpuNameChanged(const QString& name);
    void layoutChanged();  // Structure or viewport settings changed (a persistence hint).

private:
    void syncPreview(render::RenderMirror& mirror);
    void scheduleRebuild();
    void rebuild();
    QWidget* build(const viewport::LayoutNode& node, bool canClose, bool verticalStrip);
    QWidget* makeArea(const viewport::LayoutNode& node, bool maximized, bool canClose, bool verticalStrip);
    PaneFrame* makeFrame(viewport::NodeId pane, bool maximized, bool canClose);
    QWidget* panelFor(viewport::NodeId pane, viewport::AreaRole role);
    template <typename Frame> void connectArea(Frame* frame);  // Defined in the .cpp; used there only.
    void tick();
    void setActive(viewport::NodeId pane);
    void emitCamera();
    double pixelHeight() const;
    struct HandleDragInput {
        QPointF global;
        QJsonObject trace;
    };
    std::optional<HandleDragInput> captureHandleDrag(QObject* receiver, const QMouseEvent& event) const;
    void applyHandleDrag(HandleDragInput input, std::uint32_t coalesced = 1);
    void endHandleDrag(const char* reason = "rebuild-or-destroy");
    void syncRatio(AreaSplitter* splitter);
    void alignRatio(AreaSplitter* splitter, double ratio);

    struct HandleDrag {
        QPointer<AreaSplitter> splitter;
        int index = 1;
        int startPos = 0;
        QPointF startGlobal;
        std::vector<QPointer<AreaSplitter>> crossers;  // Junction drag: bars moved along the other axis.
        std::vector<int> crosserStart;
        QPointer<AreaSplitter> partner;                // Aligned sibling bar of a quad.
        std::vector<QPointer<QWindow>> inputTransparent; // Only windows whose flag we changed.
        std::optional<HandleDragInput> pending;
        std::uint32_t pendingMoves = 0;
    };
    std::optional<HandleDrag> drag_;

    editor::EditorSession* session_ = nullptr;
    static constexpr std::uint64_t kNoPreview = ~std::uint64_t{0};
    std::uint64_t previewRevision_ = kNoPreview;
    bool previewEmpty_ = true;
    std::size_t sessionObserver_ = 0;
    document::Document* document_ = nullptr;
    viewport::ViewportLayout layout_ = viewport::ViewportLayout::quad();
    GpuHost* host_ = nullptr;
    QWidget* root_ = nullptr;
    QWidget* body_ = nullptr;
    std::map<viewport::NodeId, QPointer<PaneFrame>> frames_;
    std::map<viewport::NodeId, QPointer<QWidget>> panels_;
    std::map<viewport::AreaRole, QPointer<FloatingPanel>> floating_;
    QTimer timer_;
    bool rebuildPending_ = false;
    bool gpuTried_ = false;
    bool gpuReady_ = false;
    QString gpuReason_;
    bool lastValid_ = false;
    double lastCam_[3] = {0, 0, 0};
    bool forceDraw_ = false;
    bool gpuAnnounced_ = false;
    std::optional<Ray> pointerOverride_;
};

}  // namespace apex::ui
