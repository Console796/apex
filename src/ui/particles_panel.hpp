#pragma once

#include "ui/asset_browser_panel.hpp"
class QTreeWidget;

namespace apex::ui {
class ParticlesPanel final : public AssetBrowserPanel {
    Q_OBJECT
public:
    explicit ParticlesPanel(QWidget* parent = nullptr);

signals:
    void particleChosen(QString name);

protected:
    void rebuild() override;
    void recentActivated(const QString& name) override;

private:
    QTreeWidget* tree_ = nullptr;
};
} // namespace apex::ui
