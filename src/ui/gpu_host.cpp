#include "render/vk.hpp"  // volk first: Qt's Vulkan header must not bring in prototypes.

#include "ui/gpu_host.hpp"

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
    } else {
        error_ = tr("The \"%1\" Qt platform has no Vulkan surfaces.").arg(platform);
        log::warn("viewports: {}", error_.toStdString());
        return false;
    }
    for (const char* name : wanted) {
        if (render::Context::instanceExtensionSupported(name)) options.instanceExtensions.emplace_back(name);
        else if (std::string_view(name) == VK_KHR_SURFACE_EXTENSION_NAME ||
                 std::string_view(name) == "VK_KHR_wayland_surface" ||
                 std::string_view(name) == "VK_KHR_xcb_surface") {
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
    mirror_.reset();
    classes_.reset();
    materials_.reset();
    vfs_.reset();
    document_ = nullptr;
}

void GpuHost::setDocument(document::Document* document, const assets::GameProfile* profile) {
    unbind();
    document_ = document;
    if (!document || !context_) return;

    assets::GameProfile effective;
    if (profile) effective = *profile;
    auto loadVfs = [&](const assets::GameProfile& p) { return assets::Vfs::load(p, *jobs_); };
    auto vfs = profile ? loadVfs(effective) : Result<assets::Vfs>(std::unexpected(Error{ErrorCode::NotFound, "no profile"}));
    if (!vfs) {
        if (profile) log::warn("viewports: game files unavailable ({}); using placeholder textures", vfs.error().what());
        const QString info = emptyGame_.filePath(QStringLiteral("gameinfo.txt"));
        if (!QFile::exists(info)) {
            QFile f(info);
            if (f.open(QIODevice::WriteOnly)) {
                f.write("GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
            }
        }
        assets::GameProfile empty;
        empty.gameDirectory = emptyGame_.path().toStdString();
        vfs = loadVfs(empty);
    }
    if (!vfs) {
        log::error("viewports: cannot create a file system: {}", vfs.error().what());
        document_ = nullptr;
        return;
    }
    const auto mounted = std::ranges::count_if(vfs->addons().entries, [](const auto& entry) {
        return entry.mounted && !entry.resolved.empty();
    });
    log::info("viewports: mounted {} addons", mounted);
    for (const auto& problem : vfs->addons().problems) log::warn("viewports: {}", problem);
    vfs_ = std::make_unique<assets::Vfs>(std::move(*vfs));
    materials_ = std::make_unique<assets::MaterialCache>(*vfs_, *jobs_, 256ull * 1024 * 1024,
                                                         effective.escapeMode);
    if (profile && !profile->fgds.empty()) {
        if (auto table = assets::loadEntityClasses(profile->fgds)) {
            classes_ = std::make_unique<assets::EntityClassTable>(std::move(*table));
        } else {
            log::warn("viewports: FGD load failed: {}", table.error().what());
        }
    }
    mirror_ = std::make_unique<render::RenderMirror>(*context_, *heap_, *jobs_, *materials_, classes_.get());
    mirror_->rebuild(*document);
    observerToken_ = document->observe([this](const document::ChangeSet& changes) {
        if (mirror_ && document_) mirror_->apply(*document_, changes);
    });
}

bool GpuHost::pump() {
    if (!mirror_) return false;
    mirror_->pump();
    return mirror_->needsRedraw();
}

}  // namespace apex::ui
