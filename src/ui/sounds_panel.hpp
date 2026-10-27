#pragma once

#include "ui/asset_browser_panel.hpp"
#include "ui/sound_preview.hpp"

class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;

namespace apex::ui {
class SoundsPanel final : public AssetBrowserPanel {
    Q_OBJECT
public:
    explicit SoundsPanel(QWidget* parent = nullptr, SoundPreview::Output output = SoundPreview::Output::AudioDevice);
    SoundPreview& preview() { return *preview_; }

signals:
    void soundChosen(QString name);

protected:
    QString pickedValue(const QString& name) const override;
    void rebuild() override;
    void contentReset() override;
    void recentActivated(const QString& name) override;
    void hideEvent(QHideEvent* event) override;

private:
    void play();
    void updatePlayback();
    void connectPreview();
    QTreeWidget* tree_ = nullptr;
    QComboBox* type_ = nullptr;
    SoundPreview* preview_ = nullptr;
    QPushButton* play_ = nullptr;
    QPushButton* stop_ = nullptr;
    QPushButton* next_ = nullptr;
    QLabel* status_ = nullptr;
    QString script_;
    SoundPreview::Output output_;
    bool audioOutputReady_ = false;
};
} // namespace apex::ui
