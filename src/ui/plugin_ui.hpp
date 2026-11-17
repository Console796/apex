#pragma once
#include "plugin_host/plugin_host.hpp"
#include "ui/plugin_form.hpp"
#include <QObject>
#include <QPointer>
#include <QScrollArea>
#include <QSet>
#include <QString>
#include <QWidget>
#include <functional>
#include <optional>

class QDialog;
class QVBoxLayout;
namespace apex::ui {
class PluginFormView;

// Qt side of apex.ui/1: owns nothing of the plugins, only turns their form descriptions into widgets.
// Refresh requests are queued to the event loop and coalesced, so a plugin may ask from inside a callback.
class PluginUi final : public QObject, public plugin_host::UiBackend {
    Q_OBJECT
public:
    explicit PluginUi(plugin_host::Host& host, QObject* parent = nullptr);
    ~PluginUi() override;
    plugin_host::Host& host() const { return host_; }
    bool hasPanel(const QString& id) const;
    // A scrollable dockable view of the plugin's panel, or null when the plugin is not loaded.
    QWidget* createPanel(const QString& id, QWidget* parent = nullptr);
    // Runs the queued refreshes now (tests).
    void flush();
    // Tests: replaces the blocking dialog loop. The hook gets the built dialog and returns whether to accept.
    void setDialogHook(std::function<bool(QDialog&)> hook) { dialogHook_ = std::move(hook); }
    void formChanged(std::string_view id) override;
    bool runDialog(std::string_view id, std::string_view title) override;
signals:
    void formRefreshed(const QString& id);
private:
    plugin_host::Host& host_;
    QSet<QString> queued_;
    std::function<bool(QDialog&)> dialogHook_;
};

// One described form bound to a panel, section or dialog entry (id), re-described when the plugin asks.
class PluginFormView final : public QWidget {
    Q_OBJECT
public:
    PluginFormView(PluginUi& ui, QString id, QWidget* parent = nullptr);
    void setContext(std::optional<plugin_host::UiContext> context);
    bool reload();
    PluginForm* form() const { return form_; }
    const QString& entryId() const { return id_; }
private:
    QPointer<PluginUi> ui_;
    QString id_;
    PluginForm* form_;
    std::optional<plugin_host::UiContext> context_;
};

// The plugin sections of one host panel (materials, inspector, face_properties), shown under the host content.
class PluginSections final : public QWidget {
    Q_OBJECT
public:
    explicit PluginSections(QString target, QWidget* parent = nullptr);
    void setPluginUi(PluginUi* ui);
    // The context the host panel names for its current selection.
    void setContext(plugin_host::UiContext context);
    const plugin_host::UiContext& context() const { return context_; }
private:
    void rebuild();
    QString target_;
    QPointer<PluginUi> ui_;
    plugin_host::UiContext context_;
    QVBoxLayout* layout_;
};
}
