#include "ui/game_content.hpp"

#include <QTimer>

#include <algorithm>

#include "assets/vfs.hpp"
#include "assets/vmt.hpp"
#include "core/strings.hpp"
#include "core/job_pool.hpp"
#include "core/log.hpp"

namespace apex::ui {
GameContent::GameContent(QObject* parent) : QObject(parent), jobs_(std::make_unique<JobPool>(2)) {
    timer_ = new QTimer(this);
    timer_->setInterval(8);
    connect(timer_, &QTimer::timeout, this, &GameContent::pump);
}
GameContent::~GameContent() { clear(); }
void GameContent::clear() {
    if (timer_) timer_->stop();
    thumbnails_.reset();
    index_.reset();
    vfs_.reset();
    snapshot_.reset();
    profile_.reset();
    error_.clear();
    textureSizes_.clear();
}
void GameContent::reload(const assets::GameProfile* profile) {
    if (timer_) timer_->stop();
    thumbnails_.reset();
    index_.reset();
    vfs_.reset();
    snapshot_.reset();
    profile_ = profile ? std::optional(*profile) : std::nullopt;
    error_.clear();
    textureSizes_.clear();
    lastDone_ = lastTotal_ = static_cast<std::size_t>(-1);
    if (profile_) {
        auto loaded = assets::Vfs::load(*profile_, *jobs_);
        if (!loaded) {
            error_ = QString::fromStdString(loaded.error().what());
            log::warn("game content: {}", loaded.error().what());
        } else {
            vfs_ = std::make_shared<assets::Vfs>(std::move(*loaded));
            const auto mounted = std::ranges::count_if(vfs_->addons().entries, [](const auto& entry) {
                return entry.mounted && !entry.resolved.empty();
            });
            log::info("game content: mounted {} addons", mounted);
            for (const auto& problem : vfs_->addons().problems) log::warn("game content: {}", problem);
            index_ = std::make_unique<assets::MaterialIndex>(*vfs_, *jobs_, profile_->escapeMode);
            timer_->start();
        }
    }
    emit contentChanged();
    if (!index_) emit indexProgress(0, 0);
}
void GameContent::pump() {
    if (index_) {
        index_->step();
        const auto progress = index_->progress();
        if (progress.done != lastDone_ || progress.total != lastTotal_) {
            lastDone_ = progress.done;
            lastTotal_ = progress.total;
            emit indexProgress(progress.done, progress.total);
        }
        if (progress.finished) {
            snapshot_ = index_->snapshot();
            thumbnails_ = std::make_unique<assets::ThumbnailCache>(*vfs_, *jobs_, snapshot_);
            index_.reset();
            emit indexReady();
        }
    }
    if (thumbnails_ && thumbnails_->takeChanged()) emit thumbnailsReady();
    if (jobs_) jobs_->drainCompletions();
    if (!index_ && !thumbnails_ && timer_) timer_->stop();
}
std::shared_ptr<const assets::MaterialIndexSnapshot> GameContent::snapshot() const {
    return snapshot_;
}

std::pair<int, int> GameContent::faceTextureSize(std::string_view material) const {
    constexpr std::pair<int, int> fallback{256, 256};
    const auto key = toLowerAscii(material);
    if (const auto cached = textureSizes_.find(key); cached != textureSizes_.end()) return cached->second;
    const auto remember = [&](std::pair<int, int> size) {
        textureSizes_.emplace(key, size);
        return size;
    };
    if (!snapshot_ || !vfs_) return fallback;
    const auto* entry = snapshot_->find(material);
    if (!entry || entry->previewTexture.empty()) return remember(fallback);
    // Vfs exposes whole-file reads. Parse only the fixed VTF header fields; image data is untouched.
    const auto bytes = vfs_->open(assets::texturePath(entry->previewTexture));
    if (!bytes || bytes->size() < 20 ||
        (*bytes)[0] != std::byte{'V'} || (*bytes)[1] != std::byte{'T'} ||
        (*bytes)[2] != std::byte{'F'} || (*bytes)[3] != std::byte{0}) return remember(fallback);
    const auto u16 = [&](std::size_t at) {
        return int(std::to_integer<unsigned char>((*bytes)[at])) |
               (int(std::to_integer<unsigned char>((*bytes)[at + 1])) << 8);
    };
    const int width = u16(16), height = u16(18);
    return remember(width > 0 && height > 0 ? std::pair{width, height} : fallback);
}

QString GameContent::addonName(std::size_t index) const {
    if (!vfs_ || index >= vfs_->addons().entries.size()) return {};
    const auto& addon = vfs_->addons().entries[index];
    return QString::fromStdString(addon.name.empty() ? addon.dir : addon.name);
}
} // namespace apex::ui
