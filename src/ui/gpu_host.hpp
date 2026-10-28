#pragma once

#include <memory>
#include <map>
#include <string>
#include <set>

#include <QObject>
#include <QString>
#include <QTemporaryDir>

#include "assets/fgd.hpp"
#include "ui/model_pick.hpp"
#include "assets/game_profile.hpp"
#include "render/context.hpp"
#include "render/render_mirror.hpp"
#include "render/renderer.hpp"
#include "render/texture_heap.hpp"
#include "render/model_inspector.hpp"
#include "render/model_thumbnail_scheduler.hpp"

class QVulkanInstance;

namespace apex::document {
class Document;
}
namespace apex {
class JobPool;
}
namespace apex::assets {
class Vfs;
class MaterialCache;
}  // namespace apex::assets

namespace apex::ui {

// Process-wide GPU state shared by every viewport pane: the render Context (one VkDevice), the Qt
// wrapper around its VkInstance (so QWindow surfaces come from the same instance), texture heap,
// one Renderer per swapchain format, and the RenderMirror of the open document. Created lazily on
// the first pane; unavailable (with an explanation) on platforms without Vulkan surfaces, such as
// the offscreen QPA plugin, so the shell still runs headless.
class GpuHost : public QObject {
    Q_OBJECT
public:
    explicit GpuHost(QObject* parent = nullptr);
    ~GpuHost() override;

    // Idempotent. False when no usable Vulkan device/surface support exists (see error()).
    bool ensure();
    bool available() const { return context_ != nullptr; }
    const QString& error() const { return error_; }

    render::Context* context() { return context_.get(); }
    QVulkanInstance* vulkanInstance() { return qtInstance_.get(); }
    QString gpuName() const;

    // Binds the render mirror to `document` (null unbinds). The profile selects the material
    // search paths and FGDs; null falls back to an empty file system (checkerboard textures).
    // Without a GPU this only remembers the document.
    void setDocument(document::Document* document, std::shared_ptr<const assets::Vfs> vfs,
                     const assets::GameProfile* profile);
    document::Document* document() const { return document_; }

    render::Renderer* renderer(VkFormat colorFormat);
    ModelPickCache* modelPicks() { return modelPicks_.get(); }
    std::uint64_t modelPickRevision() const { return modelPickRevision_; }
    render::RenderMirror* mirror() { return mirror_.get(); }

    // Delivers finished mesh/texture jobs; returns true when the scene needs redrawing.
    bool pump();
    void requestModelThumbnail(const QObject* owner, const QString& path, bool visible);
    void clearModelThumbnails();
    void tickModelThumbnails(assets::ThumbnailCache* cache, bool interacting);
    // The owner's inspected model, loaded and rendered from `view` on demand. Null when there is no
    // GPU, document or model cache; an empty path releases the owner's model. Not rendered while
    // `interacting`. The result is valid until the next call.
    const render::ModelInspection* inspectModel(const QObject* owner, const QString& path,
                                                const render::ModelView& view, bool interacting);

signals:
    void gpuNameChanged(const QString& name);

private:
    void unbind();

    QString error_;
    std::unique_ptr<render::Context> context_;
    std::unique_ptr<QVulkanInstance> qtInstance_;
    std::unique_ptr<JobPool> jobs_;
    QTemporaryDir emptyGame_;
    std::unique_ptr<render::TextureHeap> heap_;
    std::shared_ptr<render::GlyphAtlas> glyphAtlas_;
    std::map<VkFormat, std::unique_ptr<render::Renderer>> renderers_;
    std::shared_ptr<const assets::Vfs> vfs_;
    std::unique_ptr<assets::MaterialCache> materials_;
    std::unique_ptr<assets::ModelCache> models_;
    std::unique_ptr<render::ModelThumbnailRenderer> thumbnailRenderer_;
    std::unique_ptr<render::ModelThumbnailScheduler> thumbnailScheduler_;
    std::map<const QObject*, std::unique_ptr<render::ModelInspector>> inspectors_;
    assets::ThumbnailCache* thumbnailCache_ = nullptr;
    std::map<const QObject*, std::set<std::string>> thumbnailRequests_;
    bool thumbnailTried_ = false;
    bool ensureThumbnailRenderer();
    std::unique_ptr<ModelPickCache> modelPicks_;
    std::uint64_t modelPickRevision_ = 0;
    std::unique_ptr<assets::EntityClassTable> classes_;
    std::unique_ptr<render::RenderMirror> mirror_;
    document::Document* document_ = nullptr;
    std::size_t observerToken_ = 0;
    bool tried_ = false;
};

}  // namespace apex::ui
