#pragma once

#include "ui/asset_browser_panel.hpp"
#include <QIcon>
#include <filesystem>
#include <functional>
#include <map>

class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QTabBar;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QSplitter;
class QSlider;
class QComboBox;

namespace apex::render { struct ModelInspection; struct ModelView; }

namespace apex::ui {
class ModelInspectView;
class ModelsPanel final : public AssetBrowserPanel {
    Q_OBJECT
public:
    explicit ModelsPanel(QWidget* parent = nullptr);
    ~ModelsPanel() override;
    // The owner combines requests from all visible panels before ticking the GPU scheduler.
    using ThumbnailRequest = std::function<void(const QString&, bool)>;
    void setThumbnailSink(ThumbnailRequest request);
    void syncThumbnails(bool interacting);
    std::size_t thumbnailRequestCount() const { return requests_; }
    // The inspect area's source of renders. Called from syncThumbnails for the selected model, and
    // with an empty path when the area stops needing one. A null result means no preview is possible.
    using InspectSink = std::function<const render::ModelInspection*(const QString&, const render::ModelView&, bool)>;
    void setInspectSink(InspectSink sink);
    QString inspectedModel() const { return inspected_; }

    // The Prefabs tab (0.4.7): the prefab folder's files grouped by subfolder. Clicking one arms it as the
    // Entity tool's placement item (the owner handles prefabActivated). Icons are CPU renders of the
    // prefab's solids, a few per syncThumbnails tick.
    bool prefabsShown() const;
    void showPrefabs(bool shown);
    void refreshPrefabs();
    std::size_t prefabCount() const { return prefabCount_; }
    std::size_t pendingPrefabIcons() const { return prefabQueue_.size(); }

signals:
    void prefabActivated(QString file);
    void prefabCreateRequested();
    void modelActivated(QString path);
    void modelChosen(QString path);

protected:
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;
    void rebuild() override;
    void contentReset() override;
    void recentActivated(const QString& name) override;
    void pickModeChanged(bool enabled) override;

private:
    void updateGridSize();
    void releaseThumbnails();
    void inspect(const QString& name);
    void syncInspector(bool interacting);
    void releaseInspector();
    void renderPrefabIcons(int budget);
    void watchPrefabs(const std::filesystem::path& folder, const std::vector<std::filesystem::path>& directories);
    QTabBar* tabs_ = nullptr;
    QWidget* prefabsPage_ = nullptr;
    QLineEdit* prefabFilter_ = nullptr;
    QLabel* prefabStatus_ = nullptr;
    QPushButton* prefabCreate_ = nullptr;
    QTreeWidget* prefabTree_ = nullptr;
    QFileSystemWatcher* prefabWatcher_ = nullptr;
    QTimer* prefabRefresh_ = nullptr;
    struct PrefabIcon { QString stamp; QIcon icon; };
    std::map<QString, PrefabIcon> prefabIcons_;
    std::vector<QString> prefabQueue_;
    std::map<QString, QTreeWidgetItem*> prefabItems_;
    std::size_t prefabCount_ = 0;
    std::vector<QWidget*> modelWidgets_;
    QMetaObject::Connection prefabsConnection_;
    QListView* grid_ = nullptr;
    QSlider* size_ = nullptr;
    QComboBox* mode_ = nullptr;
    QSplitter* split_ = nullptr;
    ModelInspectView* inspect_ = nullptr;
    QCheckBox* inspectToggle_ = nullptr;
    InspectSink inspectSink_;
    QString inspected_;
    bool inspecting_ = false;
    QCheckBox* staticOnly_ = nullptr;
    class GridModel;
    GridModel* model_ = nullptr;
    ThumbnailRequest request_;
    std::set<QString> requested_;
    std::size_t requests_ = 0;
    QMetaObject::Connection thumbnailsConnection_;
};
} // namespace apex::ui
