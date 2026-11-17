#include <QApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QMessageBox>
#include <QTimer>
#include <filesystem>

#include "plugin_host/angelscript_engine.hpp"
#include "plugin_host/plugin_host.hpp"
#include "editor/actions.hpp"
#include "editor/tool.hpp"
#include "editor/pie_menus.hpp"
#include "editor/map_problems.hpp"
#include "viewport/layout.hpp"
#include "ui/plugin_manager.hpp"

#include "core/log.hpp"
#include "ui/main_window.hpp"
#include "ui/theme.hpp"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Apex"));
    QApplication::setOrganizationName(QStringLiteral("Apex"));
    QApplication::setWindowIcon(apex::ui::appIcon());  // Dialogs and the taskbar, not just the main window.
    apex::log::init();
    QStringList themeErrors;
    apex::ui::loadCustomThemes(apex::ui::themesDir(), &themeErrors);
    for (const QString& e : themeErrors) apex::log::warn("theme: {}", e.toStdString());
    apex::ui::applyThemeId(app, QSettings().value(QStringLiteral("ui/theme")).toString());
#ifdef _WIN32
    const auto data = std::filesystem::path(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation).toStdU16String()) / "Apex";
#else
    const auto data = std::filesystem::path(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation).toStdString()) / "apex";
#endif
#ifdef _WIN32
    const auto bundled = std::filesystem::path(QApplication::applicationDirPath().toStdU16String()) / "plugins";
#else
    const auto bundled = std::filesystem::path(QApplication::applicationDirPath().toStdString()) / "plugins";
#endif
    apex::plugin_host::Host plugins(data / "plugin-state.txt", data / "plugin-breadcrumb");
    plugins.setPanelRegistrar([](std::string id, std::string title) {
        return apex::viewport::mutableAreaRoleRegistry().add({std::move(id), std::move(title)});
    });
    plugins.setScriptEngineFactory(apex::plugin_host::defaultScriptEngineFactory());
    plugins.discover({data / "plugins", bundled});
    bool safeMode = app.arguments().contains(QStringLiteral("--no-plugins"));
    if (!plugins.staleBreadcrumb().empty()) {
        const auto message = QObject::tr("The previous session stopped in %1. Start without plugins for this session? You can disable the plugin in the manager.")
            .arg(QString::fromStdString(plugins.staleBreadcrumb()));
        safeMode = QMessageBox::question(nullptr, QObject::tr("Plugin crash"), message,
                                         QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) == QMessageBox::Yes || safeMode;
    }
    plugins.activate(safeMode);
    {
        // Plugin themes are data files in <plugin folder>/themes, registered for enabled, loaded plugins only
        // (never under --no-plugins or after a crash prompt). The saved theme is applied again, since it may be
        // one of them and was unknown when the first applyThemeId ran.
        std::vector<apex::ui::PluginThemeSource> sources;
        for (const auto& plugin : plugins.plugins()) {
            if (!plugin.enabled || !plugin.loaded) continue;
            const auto dir = (plugin.folder / "themes").generic_u8string();
            sources.push_back({QString::fromStdString(plugin.id), QString::fromStdString(plugin.name),
                               QString::fromUtf8(reinterpret_cast<const char*>(dir.data()), static_cast<qsizetype>(dir.size()))});
        }
        QStringList pluginThemeErrors;
        apex::ui::setPluginThemes(sources, &pluginThemeErrors);
        for (const QString& e : pluginThemeErrors) apex::log::warn("plugin theme: {}", e.toStdString());
        apex::ui::applyThemeId(app, QSettings().value(QStringLiteral("ui/theme")).toString());
    }
    apex::editor::mutableActionRegistry().freeze();
    apex::editor::mutableToolRegistry().freeze();
    apex::editor::mutablePieRegistry().freeze();
    apex::editor::mutableProblemRegistry().freeze();
    apex::viewport::mutableAreaRoleRegistry().freeze();
    apex::ui::MainWindow window;
    window.setPluginHost(&plugins);
    if (!plugins.staleBreadcrumb().empty()) QTimer::singleShot(0, &window, [&window, &plugins] { apex::ui::PluginManager(plugins, &window).exec(); });
    window.reloadProfiles();
    window.show();
    for (int i = 1; i < argc; ++i) {
        const auto argument = QString::fromLocal8Bit(argv[i]);
        if (argument != QStringLiteral("--no-plugins")) { window.openFile(argument); break; }
    }
    const int result = QApplication::exec();
    plugins.setDocument(nullptr);
    return result;
}
