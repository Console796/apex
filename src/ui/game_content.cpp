#include "ui/game_content.hpp"

#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QSignalBlocker>

#include <algorithm>
#include <exception>

#include "assets/vfs.hpp"
#include "assets/unmounted_addons.hpp"
#include "assets/fgd.hpp"
#include "assets/vmt.hpp"
#include "core/strings.hpp"
#include "core/job_pool.hpp"
#include "core/log.hpp"
#include "editor/prefab.hpp"

namespace apex::ui {
GameContent::GameContent(QObject* parent) : QObject(parent), jobs_(std::make_unique<JobPool>(2)) {
    timer_ = new QTimer(this);
    timer_->setInterval(8);
    watcher_ = new QFileSystemWatcher(this);
    watchTimer_ = new QTimer(this);
    watchTimer_->setSingleShot(true);
    watchTimer_->setInterval(150);
    connect(watcher_, &QFileSystemWatcher::fileChanged, this, [this] { watchTimer_->start(); });
    connect(watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { watchTimer_->start(); });
    connect(watchTimer_, &QTimer::timeout, this, [this] {
        if (watchedFile_.isEmpty()) return;
        if (QFile::exists(watchedFile_) && !watcher_->files().contains(watchedFile_)) watcher_->addPath(watchedFile_);
        QFile file(watchedFile_);
        if (!file.open(QIODevice::ReadOnly)) { watchTimer_->start(); return; }
        const auto bytes = file.readAll();
        if (bytes == watchedBytes_) return;
        watchedBytes_ = bytes;
        if (profile_) { auto profile = *profile_; reload(&profile); }
    });
    connect(timer_, &QTimer::timeout, this, &GameContent::pump);
}
GameContent::~GameContent() { const QSignalBlocker blocker(this); clear(); }
void GameContent::clear() {
    if (timer_) timer_->stop();
    reloadJob_.cancel();
    loading_ = false;
    unmounted_.reset();
    emit contentAboutToChange();
    classesJob_.cancel();
    classes_.reset();
    modelIndex_.reset(); soundIndex_.reset(); particleIndex_.reset();
    modelSnapshot_.reset(); soundSnapshot_.reset(); particleSnapshot_.reset();
    thumbnails_.reset();
    index_.reset();
    vfs_.reset();
    snapshot_.reset();
    profile_.reset();
    watchTimer_->stop();
    if (!watcher_->files().empty()) watcher_->removePaths(watcher_->files());
    if (!watcher_->directories().empty()) watcher_->removePaths(watcher_->directories());
    watchedFile_.clear(); watchedBytes_.clear();
    error_.clear();
    textureSizes_.clear();
    emit contentChanged();
}
std::filesystem::path GameContent::prefabFolder() const {
    if (prefabUserFolder_.empty() && !profile_) return {};
    return editor::prefabFolder(profile_ ? profile_->gameDirectory : std::filesystem::path{}, prefabUserFolder_);
}
void GameContent::setPrefabUserFolder(std::filesystem::path folder) {
    if (folder == prefabUserFolder_) return;
    prefabUserFolder_ = std::move(folder);
    emit prefabsChanged();
}
void GameContent::reload(const assets::GameProfile* profile) {
    const auto next = profile ? std::optional(*profile) : std::nullopt;
    if (!next || !profile_ || next->gameDirectory != profile_->gameDirectory ||
        next->addons.list != profile_->addons.list || next->addons.enabled != profile_->addons.enabled) {
        watchTimer_->stop();
        if (!watcher_->files().empty()) watcher_->removePaths(watcher_->files());
        if (!watcher_->directories().empty()) watcher_->removePaths(watcher_->directories());
        watchedFile_.clear(); watchedBytes_.clear();
    }
    if (timer_) timer_->stop();
    reloadJob_.cancel();
    loading_ = false;
    unmounted_.reset();
    emit contentAboutToChange();
    classesJob_.cancel();
    classes_.reset();
    modelIndex_.reset(); soundIndex_.reset(); particleIndex_.reset();
    modelSnapshot_.reset(); soundSnapshot_.reset(); particleSnapshot_.reset();
    thumbnails_.reset();
    index_.reset();
    vfs_.reset();
    snapshot_.reset();
    profile_ = next;
    error_.clear();
    textureSizes_.clear();
    lastDone_ = lastTotal_ = static_cast<std::size_t>(-1);
    if (!profile_) { clear(); return; }
    loading_ = true;
    timer_->start();
    reloadJob_ = jobs_->submit(Priority::Normal, [profile = *profile_](CancelToken token) {
        // Vfs::load waits on its loose-file indexing job, so give that work its own pool.
        using Loaded = std::pair<std::shared_ptr<const assets::Vfs>, std::unique_ptr<assets::UnmountedAddonIndex>>;
        try {
            JobPool workers(1);
            auto loaded = assets::Vfs::load(profile, workers);
            if (!loaded) return Result<Loaded>(std::unexpected(loaded.error()));
            if (token.cancelled()) return Result<Loaded>(std::unexpected(Error{ErrorCode::Internal, "Content reload cancelled"}));
            auto vfs = std::make_shared<assets::Vfs>(std::move(*loaded));
            auto unmounted = std::make_unique<assets::UnmountedAddonIndex>(vfs->addons(), token);
            return Result<Loaded>(Loaded{std::move(vfs), std::move(unmounted)});
        } catch (const std::exception& error) {
            return Result<Loaded>(std::unexpected(Error{ErrorCode::IoError, error.what()}));
        }
    }, [this](auto loaded) {
        loading_ = false;
        if (!loaded) {
            error_ = QString::fromStdString(loaded.error().what());
            log::warn("game content: {}", loaded.error().what());
        } else {
            vfs_ = std::move(loaded->first);
            unmounted_ = std::move(loaded->second);
            for (const auto& problem : vfs_->addons().problems) log::warn("game content: {}", problem);
            for (const auto& problem : unmounted_->problems()) log::warn("game content: {}", problem);
            index_ = std::make_unique<assets::MaterialIndex>(*vfs_, *jobs_, profile_->escapeMode);
            modelIndex_ = std::make_unique<assets::ModelIndex>(*vfs_, *jobs_, profile_->escapeMode);
            soundIndex_ = std::make_unique<assets::SoundIndex>(*vfs_, *jobs_, profile_->escapeMode);
            particleIndex_ = std::make_unique<assets::ParticleIndex>(*vfs_, *jobs_, profile_->escapeMode);
            classesJob_ = jobs_->submit(Priority::Normal, [files = profile_->fgds](CancelToken) {
                return assets::loadEntityClasses(files);
            }, [this](Result<assets::EntityClassTable> table) {
                if (table) classes_ = std::make_unique<assets::EntityClassTable>(std::move(*table));
                emit classesReady();
            });
            watchAddonList();
        }
        emit contentChanged();
    });
    emit contentChanged();
}
void GameContent::watchAddonList() {
    auto file = vfs_ ? QString::fromStdString(vfs_->addons().file.string()) : QString{};
    // Keep watching through a temporarily invalid or absent list, so a later save can repair it.
    if (file.isEmpty()) file = watchedFile_;
    if (!watcher_->files().empty()) watcher_->removePaths(watcher_->files());
    if (!watcher_->directories().empty()) watcher_->removePaths(watcher_->directories());
    watchedFile_ = file;
    if (file.isEmpty()) return;
    if (vfs_ && !vfs_->addons().file.empty()) watchedBytes_ = QByteArray::fromStdString(vfs_->addons().sourceBytes);
    watcher_->addPath(QFileInfo(file).absolutePath());
    if (QFile::exists(file)) watcher_->addPath(file);
    watchTimer_->start(); // An edit during the worker's load must not be swallowed by re-arming.
}
Result<void> GameContent::setAddonMounted(std::size_t index, bool mounted) {
    if (!vfs_) return std::unexpected(Error{ErrorCode::InvalidArgument, "No addon list loaded"});
    auto result = assets::setAddonMounted(vfs_->addons(), index, mounted);
    if (!result) { error_ = QString::fromStdString(result.error().what()); return result; }
    auto profile = profile_;
    reload(profile ? &*profile : nullptr);
    return {};
}
std::optional<std::size_t> GameContent::unmountedAddon(std::string_view path) const {
    return unmounted_ ? unmounted_->provider(path) : std::nullopt;
}
QString GameContent::missingAssetTooltip(std::string_view path) const {
    if (assetExists(path)) return {};
    auto tip = tr("Missing asset: %1").arg(QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size())));
    if (auto addon = unmountedAddon(path)) tip += tr("\nProvided by unmounted addon: %1").arg(addonName(*addon));
    return tip;
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
    } else if (modelIndex_) {
        modelIndex_->step();
        const auto progress = modelIndex_->progress();
        emit modelIndexProgress(progress.done, progress.total);
        if (progress.finished) {
            modelSnapshot_ = modelIndex_->snapshot(); modelIndex_.reset(); emit modelIndexReady();
        }
    } else if (soundIndex_) {
        soundIndex_->step();
        const auto progress = soundIndex_->progress();
        emit soundIndexProgress(progress.done, progress.total);
        if (progress.finished) {
            soundSnapshot_ = soundIndex_->snapshot(); soundIndex_.reset(); emit soundIndexReady();
        }
    } else if (particleIndex_) {
        particleIndex_->step();
        const auto progress = particleIndex_->progress();
        emit particleIndexProgress(progress.done, progress.total);
        if (progress.finished) {
            particleSnapshot_ = particleIndex_->snapshot(); particleIndex_.reset(); emit particleIndexReady();
        }
    }
    if (thumbnails_ && thumbnails_->takeChanged()) emit thumbnailsReady();
    if (jobs_) jobs_->drainCompletions();
    if (!loading_ && !index_ && !modelIndex_ && !soundIndex_ && !particleIndex_ && !thumbnails_ && timer_) timer_->stop();
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
bool GameContent::assetExists(std::string_view path) const {
    return vfs_ && vfs_->exists(path);
}
std::optional<std::string> GameContent::assetProviderName(std::string_view path) const {
    if (!vfs_) return {};
    const auto index = vfs_->provider(path);
    if (!index) return {};
    const auto name = addonName(*index).toStdString();
    return name.empty() ? std::nullopt : std::optional(name);
}
} // namespace apex::ui
