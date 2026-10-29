#pragma once

#include <QApplication>
#include <QTemporaryDir>
#include <QSettings>

namespace apex::ui_test {
inline QApplication& app() {
    static int argc = 1;
    static char name[] = "apex_ui_tests";
    static char* argv[] = {name, nullptr};
    static QTemporaryDir config;
    static QTemporaryDir state;
    // XDG_CONFIG_HOME does not redirect the Windows registry backend. Isolate both platforms
    // explicitly, so a previous run's Prefabs tab or layout cannot change this run's tests.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, config.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, config.path());
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    qputenv("APEX_AUTOSAVE_DIR", state.path().toUtf8());
    qputenv("APEX_SKIP_RECOVERY", "1");
#ifdef _WIN32
    // On Windows the offscreen platform uses Qt's FreeType font database, which looks only in
    // QT_QPA_FONTDIR (default <Qt>/lib/fonts, which Qt no longer ships). With no fonts every
    // glyph is drawn as an em-wide box, roughly doubling text widths: pane headers then claim
    // over half of an 800 px quad and the splitter range inverts. Point it at the system fonts
    // so widget metrics match a real desktop, as fontconfig already does on Linux.
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR"))
        qputenv("QT_QPA_FONTDIR", (qEnvironmentVariable("WINDIR", QStringLiteral("C:/Windows")) + QStringLiteral("/Fonts")).toUtf8());
#endif
    QCoreApplication::setOrganizationName(QStringLiteral("ApexTest"));
    QCoreApplication::setApplicationName(QStringLiteral("ApexTest"));
    static QApplication application(argc, argv);
    return application;
}
} // namespace apex::ui_test
