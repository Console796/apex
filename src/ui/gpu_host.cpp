#include "render/vk.hpp"  // volk first: Qt's Vulkan header must not bring in prototypes.

#include "ui/gpu_host.hpp"
#include "ui/bake_controller.hpp"
#include "ui/vrad_controller.hpp"
#include "render/lightmap_atlas.hpp"
#include "render/renderer.hpp"

#include <algorithm>

#include <QFile>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QGuiApplication>
#include <QVulkanInstance>

#include "assets/material_cache.hpp"
#include "assets/vfs.hpp"
#include "core/job_pool.hpp"
#include "core/log.hpp"
#include "document/document.hpp"

namespace apex::ui {
namespace {

render::GlyphAtlasData rasteriseGlyphs() {
    const QFont font = QGuiApplication::font();
    const QFontMetrics metrics(font);
    int cellWidth = 1;
    for (unsigned code = 32; code < 256; ++code)
        cellWidth = std::max(cellWidth, metrics.boundingRect(QChar(static_cast<char16_t>(code))).width());
    cellWidth += 2;
    const int cellHeight = metrics.height() + 2;
    QImage image(cellWidth * 16, cellHeight * 16, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.setRenderHint(QPainter::TextAntialiasing);
    render::GlyphAtlasData data;
    data.width = static_cast<std::uint32_t>(image.width());
    data.height = static_cast<std::uint32_t>(image.height());
    data.lineHeight = static_cast<float>(metrics.lineSpacing());
    for (unsigned code = 32; code < 256; ++code) {
        if (code >= 127 && code < 160) continue;
        const QChar ch(static_cast<char16_t>(code));
        const QRect bounds = metrics.boundingRect(ch);
        const int x = static_cast<int>(code % 16) * cellWidth + 1;
        const int y = static_cast<int>(code / 16) * cellHeight + 1;
        painter.drawText(QPoint(x - bounds.left(), y - bounds.top()), QString(ch));
        auto& glyph = data.glyphs[code];
        glyph.advance = static_cast<float>(metrics.horizontalAdvance(ch));
        glyph.bearing = Vec2(bounds.left(), bounds.top());
        glyph.rect = {static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                      static_cast<std::uint32_t>(bounds.width()), static_cast<std::uint32_t>(bounds.height())};
    }
    painter.end();
    data.pixels.resize(static_cast<std::size_t>(data.width) * data.height);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            data.pixels[static_cast<std::size_t>(y) * data.width + x] = std::byte(qAlpha(image.pixel(x, y)));
    return data;
}

} // namespace

GpuHost::GpuHost(QObject* parent) : QObject(parent) {}

GpuHost::~GpuHost() {
    unbind();
    if (context_) context_->waitIdle();
    // Members die in reverse order: mirror, caches, renderers, heap, jobs, Qt wrapper, context.
}

bool GpuHost::ensure() {
    if (context_) return true;
    if (tried_) return false;
    tried_ = true;

    const QString platform = QGuiApplication::platformName();
    render::ContextOptions options;
    std::vector<const char*> wanted = {VK_KHR_SURFACE_EXTENSION_NAME};
    if (platform.startsWith(QLatin1String("wayland"))) {
        wanted.push_back("VK_KHR_wayland_surface");
    } else if (platform == QLatin1String("xcb")) {
        wanted.push_back("VK_KHR_xcb_surface");
        wanted.push_back("VK_KHR_xlib_surface");
    } else if (platform == QLatin1String("windows")) {
        wanted.push_back("VK_KHR_win32_surface");
    } else {
        error_ = tr("The \"%1\" Qt platform has no Vulkan surfaces.").arg(platform);
        log::warn("viewports: {}", error_.toStdString());
        return false;
    }
    for (const char* name : wanted) {
        if (render::Context::instanceExtensionSupported(name)) options.instanceExtensions.emplace_back(name);
        else if (std::string_view(name) == VK_KHR_SURFACE_EXTENSION_NAME ||
                 std::string_view(name) == "VK_KHR_wayland_surface" ||
                 std::string_view(name) == "VK_KHR_xcb_surface" ||
                 std::string_view(name) == "VK_KHR_win32_surface") {
            error_ = tr("Vulkan instance extension %1 is missing.").arg(QString::fromLatin1(name));
            log::warn("viewports: {}", error_.toStdString());
            return false;
        }
    }

    auto created = render::Context::create(options);
    if (!created) {
        error_ = tr("No usable Vulkan device: %1").arg(QString::fromStdString(created.error().what()));
        log::warn("viewports: {}", error_.toStdString());
        return false;
    }
    auto context = std::move(*created);

    auto qt = std::make_unique<QVulkanInstance>();
    qt->setVkInstance(context->instance());
    if (!qt->create()) {
        error_ = tr("Qt could not wrap the Vulkan instance (error %1).").arg(static_cast<int>(qt->errorCode()));
        log::warn("viewports: {}", error_.toStdString());
        return false;
    }

    auto heap = render::TextureHeap::create(*context);
    if (!heap) {
        error_ = tr("Texture heap: %1").arg(QString::fromStdString(heap.error().what()));
        log::warn("viewports: {}", error_.toStdString());
        return false;
    }

    // Labels are optional: without an atlas the renderer skips the label pass.
    if (auto atlas = render::GlyphAtlas::create(*context, **heap, rasteriseGlyphs())) glyphAtlas_ = std::move(*atlas);
    else log::warn("viewports: glyph atlas: {}", atlas.error().what());
    jobs_ = std::make_unique<JobPool>(2);
    context_ = std::move(context);
    qtInstance_ = std::move(qt);
    heap_ = std::move(*heap);
    log::info("viewports: GPU {}{}", context_->info().name, context_->info().validation ? " (validation on)" : "");
    emit gpuNameChanged(gpuName());
    return true;
}

QString GpuHost::gpuName() const {
    return context_ ? QString::fromStdString(context_->info().name) : QString();
}

render::Renderer* GpuHost::renderer(VkFormat colorFormat) {
    if (!context_) return nullptr;
    if (auto it = renderers_.find(colorFormat); it != renderers_.end()) return it->second.get();
    auto created = render::Renderer::create(*context_, *heap_, colorFormat, render::SwapchainDepth::kFormat);
    if (!created) {
        log::error("viewports: renderer for format {}: {}", static_cast<int>(colorFormat), created.error().what());
        renderers_[colorFormat] = nullptr;
        return nullptr;
    }
    (*created)->setGlyphAtlas(glyphAtlas_);
    return (renderers_[colorFormat] = std::move(*created)).get();
}

void GpuHost::unbind() {
    if (document_ && observerToken_) document_->unobserve(observerToken_);
    observerToken_ = 0;
    if (context_) context_->waitIdle();
    clearModelThumbnails();
    bake_.reset();
    vrad_.reset();
    vradMaps_.reset();
    vradRadiance_.clear();
    source_ = LightmapSource::Apex;
    density_ = false;
    bakeInstances_.clear();
    mirror_.reset();
    modelPicks_.reset();
    if (models_) models_->clear();
    models_.reset();
    ++modelPickRevision_;
    classes_.reset();
    materials_.reset();
    vfs_.reset();
    document_ = nullptr;
}

void GpuHost::setDocument(document::Document* document, std::shared_ptr<const assets::Vfs> vfs,
                          const assets::GameProfile* profile) {
    unbind();
    document_ = document;
    if (!context_) return;

    assets::GameProfile effective;
    if (profile) effective = *profile;
    if (!vfs) {
        const QString info = emptyGame_.filePath(QStringLiteral("gameinfo.txt"));
        if (!QFile::exists(info)) {
            QFile f(info);
            if (f.open(QIODevice::WriteOnly)) {
                f.write("GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
            }
        }
        effective.gameDirectory = emptyGame_.path().toStdString();
        auto empty = assets::Vfs::load(effective, *jobs_);
        if (empty) vfs = std::make_shared<assets::Vfs>(std::move(*empty));
        else {
            log::error("viewports: cannot create an empty file system: {}", empty.error().what());
            document_ = nullptr;
            return;
        }
    }
    vfs_ = std::move(vfs);
    materials_ = std::make_unique<assets::MaterialCache>(*vfs_, *jobs_, 256ull * 1024 * 1024);
    if (profile && !profile->fgds.empty()) {
        if (auto table = assets::loadEntityClasses(profile->fgds)) {
            classes_ = std::make_unique<assets::EntityClassTable>(std::move(*table));
        } else {
            log::warn("viewports: FGD load failed: {}", table.error().what());
        }
    }
    models_ = std::make_unique<assets::ModelCache>(*vfs_, *jobs_);
    modelPicks_ = std::make_unique<ModelPickCache>(*models_);
    if (!document) return;
    mirror_ = std::make_unique<render::RenderMirror>(*context_, *heap_, *jobs_, *materials_, classes_.get(),
        models_.get(), [this](const std::string& path) { return vfs_->exists(path); });
    mirror_->rebuild(*document);
    bake_ = std::make_unique<BakeController>(*document, vfs_, *context_, *heap_, *mirror_, *jobs_);
    profile_ = effective;
    vrad_ = std::make_unique<VradController>(*document, vradRunner_, [this](render::VradLightmaps loaded) -> Result<void> {
        auto maps = render::BakedLightmaps::create(*context_, *heap_, std::move(loaded.atlas));
        if (!maps) return std::unexpected(maps.error());
        if (auto uploaded = (*maps)->upload(density_ ? render::densityRadiance((*maps)->atlas()) : loaded.radiance); !uploaded)
            return uploaded;
        vradMaps_ = std::move(*maps);
        vradRadiance_ = std::move(loaded.radiance);
        if (mirror_ && source_ == LightmapSource::Vrad) mirror_->setLightmaps(vradMaps_);
        return {};
    });
    observerToken_ = document->observe([this](const document::ChangeSet& changes) {
        if (mirror_ && document_) mirror_->apply(*document_, changes);
        // Conservative whole restart: keys include light targets, material and instance fixups.
        // Selection/view settings do not issue document ChangeSets.
        if (bake_ && !changes.empty()) bake_->invalidate();
    });
}

bool GpuHost::pump() {
    if (!mirror_) { if (jobs_) jobs_->drainCompletions(); return false; }
    mirror_->pump();
    if (mirror_->cubemapRebuildPending()) if (auto* capture = renderer(VK_FORMAT_R8G8B8A8_UNORM)) {
        if (auto rebuilt = mirror_->rebuildCubemaps(*capture); !rebuilt)
            log::error("cubemap rebuild: {}", rebuilt.error().what());
    }
    if (modelPicks_ && modelPicks_->pump()) ++modelPickRevision_;
    return mirror_->needsRedraw();
}

bool GpuHost::updateBake(bool active, DVec3 camera, const std::filesystem::path& map,
                         const std::optional<std::filesystem::path>& extraRoot,
                         const std::vector<document::InstanceContent>& instances) {
    if (!bake_) return false;
    std::vector<std::pair<std::shared_ptr<const document::Document>, DMat4>> identity;
    for (const auto& instance : instances) identity.emplace_back(instance.keys, instance.transform);
    if (identity != bakeInstances_) {
        bakeInstances_ = std::move(identity);
        bake_->invalidate(); // Includes external instance reloads without a parent document edit.
    }
    return bake_->update(active, camera, map, extraRoot);
}
QString GpuHost::bakeStatus() const {
    return bake_ ? bake_->status() + tr(" | Props/displacements: Lit (not baked)")
                 : tr("Baked: no document/GPU");
}
float GpuHost::bakeScale() const { return bake_ ? bake_->scale() : render::kDefaultBakedScale; }
float GpuHost::lightmapScale(bool vrad, bool density) const {
    if (density) return 1.0f;
    return vrad ? (vrad_ ? vrad_->scale() : render::kDefaultVradScale) : bakeScale();
}

void GpuHost::selectLightmaps(LightmapSource source, bool density) {
    if (!bake_ || !mirror_) return;
    if (source != source_) {
        source_ = source;
        if (source == LightmapSource::Vrad) { bake_->setDisplayed(false); mirror_->setLightmaps(vradMaps_); }
        else bake_->setDisplayed(true);
    }
    if (density != density_) {
        density_ = density;
        bake_->setDensity(density);
        if (vradMaps_) {
            if (auto uploaded = vradMaps_->upload(density ? render::densityRadiance(vradMaps_->atlas()) : vradRadiance_); !uploaded)
                log::error("viewports: lattice overlay upload: {}", uploaded.error().what());
        }
    }
}

bool GpuHost::updateVrad(bool active, const std::filesystem::path& map) {
    if (!vrad_) return false;
    editor::CompileGamePaths paths;
    paths.gameDirectory = profile_.gameDirectory;
    paths.toolDirectory = profile_.toolDirectory();
    paths.instanceRoot = profile_.instanceSearchRoot();
    paths.classes = classes_.get();
    paths.escapeMode = profile_.escapeMode;
    for (const auto& [variable, tool] : {std::pair{"bsp_exe", "vbsp"}, {"vis_exe", "vvis"}, {"light_exe", "vrad"}})
        paths.tools[variable] = profile_.toolPath(tool).string();
    return vrad_->update(active, map, paths);
}
void GpuHost::rerunVrad() { if (vrad_) vrad_->rerun(); }
void GpuHost::cancelVrad() { if (vrad_) vrad_->cancel(); }
QString GpuHost::vradStatus() const { return vrad_ ? vrad_->status() : tr("VRAD: no document/GPU"); }
QString GpuHost::vradDetail() const { return vrad_ && !vrad_->detail().isEmpty() ? vrad_->detail() : vradStatus(); }
bool GpuHost::vradFailed() const { return vrad_ && vrad_->phase() == VradController::Phase::Failed; }

void GpuHost::rebuildCubemaps() {
    if (mirror_) mirror_->requestCubemapRebuild();
}

void GpuHost::clearModelThumbnails() {
    inspectors_.clear();
    thumbnailScheduler_.reset();
    thumbnailCache_ = nullptr;
    thumbnailRequests_.clear();
}
void GpuHost::requestModelThumbnail(const QObject* owner, const QString& path, bool visible) {
    const auto name = path.toStdString();
    if (visible) thumbnailRequests_[owner].insert(name);
    else {
        if (auto found = thumbnailRequests_.find(owner); found != thumbnailRequests_.end()) {
            found->second.erase(name);
            if (found->second.empty()) thumbnailRequests_.erase(found);
        }
        if (thumbnailScheduler_ && std::ranges::none_of(thumbnailRequests_, [&](const auto& entry) {
            return entry.second.contains(name);
        })) thumbnailScheduler_->drop(name);
    }
}
bool GpuHost::ensureThumbnailRenderer() {
    if (!thumbnailTried_) {
        thumbnailTried_ = true;
        auto renderer = render::ModelThumbnailRenderer::create(*context_, *heap_);
        if (renderer) thumbnailRenderer_ = std::move(*renderer);
        else log::warn("model thumbnails: {}", renderer.error().what());
    }
    return thumbnailRenderer_ != nullptr;
}
const render::ModelInspection* GpuHost::inspectModel(const QObject* owner, const QString& path,
                                                    const render::ModelView& view, bool interacting) {
    if (path.isEmpty()) { inspectors_.erase(owner); return nullptr; }
    if (!context_ || !models_ || !materials_ || !vfs_ || !ensureThumbnailRenderer()) return nullptr;
    auto& inspector = inspectors_[owner];
    if (!inspector) inspector = std::make_unique<render::ModelInspector>(
        *models_, *materials_, [vfs = vfs_](const std::string& name) { return vfs->exists(name); }, *thumbnailRenderer_);
    return &inspector->update(path.toStdString(), view, interacting);
}
void GpuHost::tickModelThumbnails(assets::ThumbnailCache* cache, bool interacting) {
    if (thumbnailCache_ != cache) { thumbnailScheduler_.reset(); thumbnailCache_ = cache; }
    if (interacting || !context_ || !models_ || !cache || thumbnailRequests_.empty()) return;
    if (!ensureThumbnailRenderer()) return;
    if (!thumbnailScheduler_) {
        if (materials_ && vfs_) thumbnailScheduler_ = std::make_unique<render::ModelThumbnailScheduler>(
            *models_, *materials_, [vfs = vfs_](const std::string& path) { return vfs->exists(path); },
            *cache, *thumbnailRenderer_);
        else thumbnailScheduler_ = std::make_unique<render::ModelThumbnailScheduler>(
            *models_, *cache, *thumbnailRenderer_);
    }
    for (const auto& [owner, paths] : thumbnailRequests_) for (const auto& path : paths) {
        if (cache->state(assets::ThumbnailKind::Model, path, 128) == assets::ThumbnailState::Unloaded)
            thumbnailScheduler_->request(path, true);
    }
    thumbnailScheduler_->tick(interacting);
}

}  // namespace apex::ui
