#pragma once

#include "ui/asset_browser_panel.hpp"
#include <functional>

class QListView;
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

signals:
    void modelActivated(QString path);
    void modelChosen(QString path);

protected:
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;
    void rebuild() override;
    void contentReset() override;
    void recentActivated(const QString& name) override;

private:
    void updateGridSize();
    void releaseThumbnails();
    void inspect(const QString& name);
    void syncInspector(bool interacting);
    void releaseInspector();
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
