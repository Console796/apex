#pragma once

#include <QObject>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "assets/game_profile.hpp"
#include <QByteArray>
#include "assets/material_index.hpp"
#include "assets/model_index.hpp"
#include "assets/sound_index.hpp"
#include "assets/particle_index.hpp"
#include "core/entity_class.hpp"
#include "assets/thumbnail_cache.hpp"

class QTimer;
class QFileSystemWatcher;

namespace apex::assets { class UnmountedAddonIndex; }

namespace apex::ui {

// Main-thread owner for mounted assets and their UI-facing asynchronous work. It exists even
// when Vulkan is unavailable. Destroy/reset the index and cache before replacing the Vfs.
class GameContent final : public QObject {
    Q_OBJECT
public:
    explicit GameContent(QObject* parent = nullptr);
    ~GameContent() override;
    void reload(const assets::GameProfile* profile);
    void clear();
    std::shared_ptr<const assets::Vfs> vfs() const { return vfs_; }
    std::shared_ptr<const assets::MaterialIndexSnapshot> snapshot() const;
    std::shared_ptr<const assets::ModelIndexSnapshot> modelSnapshot() const { return modelSnapshot_; }
    std::shared_ptr<const assets::SoundIndexSnapshot> soundSnapshot() const { return soundSnapshot_; }
    std::shared_ptr<const assets::ParticleIndexSnapshot> particleSnapshot() const { return particleSnapshot_; }
    const assets::EntityClassTable* classes() const { return classes_.get(); }
    assets::ThumbnailCache* thumbnails() const { return thumbnails_.get(); }
    bool hasProfile() const { return profile_.has_value(); }
    const std::optional<assets::GameProfile>& profile() const { return profile_; }
    // The prefab folder (0.4 D12): the user's folder when set, else `<game>/../hammer/prefabs` of the
    // current profile; empty with neither. Changing the user folder, or a prefab written by Apex,
    // emits prefabsChanged (a folder watcher in the browser covers outside edits).
    std::filesystem::path prefabFolder() const;
    const std::filesystem::path& prefabUserFolder() const { return prefabUserFolder_; }
    void setPrefabUserFolder(std::filesystem::path folder);
    void notifyPrefabsChanged() { emit prefabsChanged(); }
    QString addonName(std::size_t index) const;
    // UI bridge for lightweight reports; assets types remain behind this QObject.
    bool assetExists(std::string_view path) const;
    std::optional<std::string> assetProviderName(std::string_view path) const;
    std::optional<std::size_t> unmountedAddon(std::string_view path) const;
    QString missingAssetTooltip(std::string_view path) const;
    Result<void> setAddonMounted(std::size_t index, bool mounted);
    bool loading() const { return loading_; }
    QString error() const { return error_; }
    std::pair<int, int> faceTextureSize(std::string_view material) const;

signals:
    void contentAboutToChange(); // Release consumers borrowing the thumbnail store before reset.
    void contentChanged();
    void modelIndexProgress(std::size_t done, std::size_t total);
    void soundIndexProgress(std::size_t done, std::size_t total);
    void particleIndexProgress(std::size_t done, std::size_t total);
    void modelIndexReady();
    void soundIndexReady();
    void particleIndexReady();
    void classesReady();
    void indexProgress(std::size_t done, std::size_t total);
    void indexReady();
    void thumbnailsReady();
    void prefabsChanged();

private slots:
    void pump();

private:
    void watchAddonList();
    std::unique_ptr<JobPool> jobs_;
    JobHandle reloadJob_;
    bool loading_ = false;
    std::unique_ptr<assets::UnmountedAddonIndex> unmounted_;
    QFileSystemWatcher* watcher_ = nullptr;
    QTimer* watchTimer_ = nullptr;
    QString watchedFile_;
    QByteArray watchedBytes_;
    std::shared_ptr<const assets::Vfs> vfs_;
    std::shared_ptr<const assets::MaterialIndexSnapshot> snapshot_;
    std::unique_ptr<assets::MaterialIndex> index_;
    std::unique_ptr<assets::ModelIndex> modelIndex_;
    std::unique_ptr<assets::SoundIndex> soundIndex_;
    std::unique_ptr<assets::ParticleIndex> particleIndex_;
    std::shared_ptr<const assets::ModelIndexSnapshot> modelSnapshot_;
    std::shared_ptr<const assets::SoundIndexSnapshot> soundSnapshot_;
    std::shared_ptr<const assets::ParticleIndexSnapshot> particleSnapshot_;
    std::unique_ptr<assets::EntityClassTable> classes_;
    JobHandle classesJob_;
    std::unique_ptr<assets::ThumbnailCache> thumbnails_;
    std::optional<assets::GameProfile> profile_;
    std::filesystem::path prefabUserFolder_;
    QTimer* timer_ = nullptr;
    QString error_;
    mutable std::unordered_map<std::string, std::pair<int, int>> textureSizes_;
    std::size_t lastDone_ = static_cast<std::size_t>(-1), lastTotal_ = static_cast<std::size_t>(-1);
};

} // namespace apex::ui
