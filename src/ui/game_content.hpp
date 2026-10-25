#pragma once

#include <QObject>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "assets/game_profile.hpp"
#include "assets/material_index.hpp"
#include "assets/thumbnail_cache.hpp"

class QTimer;

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
    assets::ThumbnailCache* thumbnails() const { return thumbnails_.get(); }
    bool hasProfile() const { return profile_.has_value(); }
    const std::optional<assets::GameProfile>& profile() const { return profile_; }
    QString addonName(std::size_t index) const;
    QString error() const { return error_; }
    std::pair<int, int> faceTextureSize(std::string_view material) const;

signals:
    void contentChanged();
    void indexProgress(std::size_t done, std::size_t total);
    void indexReady();
    void thumbnailsReady();

private slots:
    void pump();

private:
    std::unique_ptr<JobPool> jobs_;
    std::shared_ptr<const assets::Vfs> vfs_;
    std::shared_ptr<const assets::MaterialIndexSnapshot> snapshot_;
    std::unique_ptr<assets::MaterialIndex> index_;
    std::unique_ptr<assets::ThumbnailCache> thumbnails_;
    std::optional<assets::GameProfile> profile_;
    QTimer* timer_ = nullptr;
    QString error_;
    mutable std::unordered_map<std::string, std::pair<int, int>> textureSizes_;
    std::size_t lastDone_ = static_cast<std::size_t>(-1), lastTotal_ = static_cast<std::size_t>(-1);
};

} // namespace apex::ui
