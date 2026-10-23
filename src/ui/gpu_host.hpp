#pragma once

#include <memory>
#include <map>
#include <string>

#include <QObject>
#include <QString>
#include <QTemporaryDir>

#include "assets/fgd.hpp"
#include "assets/game_profile.hpp"
#include "render/context.hpp"
#include "render/render_mirror.hpp"
#include "render/renderer.hpp"
#include "render/texture_heap.hpp"

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
    void setDocument(document::Document* document, const assets::GameProfile* profile);
    document::Document* document() const { return document_; }

    render::Renderer* renderer(VkFormat colorFormat);
    render::RenderMirror* mirror() { return mirror_.get(); }

    // Delivers finished mesh/texture jobs; returns true when the scene needs redrawing.
    bool pump();

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
    std::unique_ptr<assets::Vfs> vfs_;
    std::unique_ptr<assets::MaterialCache> materials_;
    std::unique_ptr<assets::EntityClassTable> classes_;
    std::unique_ptr<render::RenderMirror> mirror_;
    document::Document* document_ = nullptr;
    std::size_t observerToken_ = 0;
    bool tried_ = false;
};

}  // namespace apex::ui
