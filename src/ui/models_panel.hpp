#pragma once

#include "ui/asset_browser_panel.hpp"
#include <functional>

class QListView;
class QSlider;
class QComboBox;

namespace apex::ui {
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
    QListView* grid_ = nullptr;
    QSlider* size_ = nullptr;
    QComboBox* mode_ = nullptr;
    QCheckBox* staticOnly_ = nullptr;
    class GridModel;
    GridModel* model_ = nullptr;
    ThumbnailRequest request_;
    std::set<QString> requested_;
    std::size_t requests_ = 0;
    QMetaObject::Connection thumbnailsConnection_;
};
} // namespace apex::ui
