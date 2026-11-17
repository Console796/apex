#include "ui/plugin_manager.hpp"
#include "plugin_host/plugin_host.hpp"
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QVBoxLayout>
namespace apex::ui {
PluginManager::PluginManager(plugin_host::Host& host, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Plugins"));
    resize(560, 400);
    auto* layout = new QVBoxLayout(this);
    if (!host.staleBreadcrumb().empty()) {
        auto* warning = new QLabel(tr("The previous session stopped during plugin callback: %1").arg(QString::fromStdString(host.staleBreadcrumb())));
        warning->setWordWrap(true);
        layout->addWidget(warning);
    }
    layout->addWidget(new QLabel(tr("Changes to enabled plugins take effect after restart.")));
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto* contents = new QWidget(scroll);
    auto* rows = new QVBoxLayout(contents);
    for (const auto& plugin : host.plugins()) {
        auto* enabled = new QCheckBox(QString::fromStdString(plugin.name.empty() ? plugin.id : plugin.name), contents);
        enabled->setChecked(plugin.enabled);
        enabled->setToolTip(QString::fromStdString(plugin.id));
        connect(enabled, &QCheckBox::toggled, this, [&host, enabled, id = plugin.id](bool checked) {
            try { host.setEnabled(id, checked); }
            catch (const std::exception& e) {
                const QSignalBlocker blocker(enabled);
                enabled->setChecked(!checked);
                QMessageBox::warning(enabled, QObject::tr("Plugins"), QString::fromUtf8(e.what()));
            }
        });
        rows->addWidget(enabled);
        if (!plugin.error.empty()) {
            auto* error = new QLabel(QString::fromStdString(plugin.error), contents);
            error->setWordWrap(true);
            rows->addWidget(error);
        }
    }
    rows->addStretch();
    scroll->setWidget(contents);
    layout->addWidget(scroll);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(close);
}
}
