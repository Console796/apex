#pragma once

#include <QApplication>
#include <QTemporaryDir>

namespace apex::ui_test {
inline QApplication& app() {
    static int argc = 1;
    static char name[] = "apex_ui_tests";
    static char* argv[] = {name, nullptr};
    static QTemporaryDir config;
    static QTemporaryDir state;
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    qputenv("APEX_AUTOSAVE_DIR", state.path().toUtf8());
    qputenv("APEX_SKIP_RECOVERY", "1");
    QCoreApplication::setOrganizationName(QStringLiteral("ApexTest"));
    QCoreApplication::setApplicationName(QStringLiteral("ApexTest"));
    static QApplication application(argc, argv);
    return application;
}
} // namespace apex::ui_test
