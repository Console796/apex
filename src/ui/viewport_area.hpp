#pragma once

#include <QByteArray>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <map>

#include "viewport/layout.hpp"
#include "editor/editor_session.hpp"
#include "viewport/map_views.hpp"

namespace apex::render {
class RenderMirror;
}

class QSplitter;

namespace apex::assets {
struct GameProfile;
}
namespace apex::document {
class Document;
}

namespace apex::ui {

class GpuHost;
class PaneFrame;

// The central viewport area (plan 0.1, D15/D16): the viewport::ViewportLayout split tree mirrored
// into nested QSplitters, one PaneFrame (header + native Vulkan window) per leaf. It is a plain
// widget in the main window's central slot, never an ADS dock.
//
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
    viewport::MapViews captureMapViews() const;

    // The world ray under the mouse pointer when it is over a pane (Paste places at its work-plane
    // hit). The override stands in for a real pointer in tests and scripted input.
    std::optional<Ray> pointerRay() const;
    void setPointerRayOverride(std::optional<Ray> ray) { pointerOverride_ = ray; }

    // Runs queued rebuilds and one timer tick now (tests, screenshots).
    void flush();

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
    QWidget* build(const viewport::LayoutNode& node, bool canClose);
    PaneFrame* makeFrame(viewport::NodeId pane, bool maximized, bool canClose);
    void tick();
    void setActive(viewport::NodeId pane);
    void emitCamera();
    double pixelHeight() const;

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
