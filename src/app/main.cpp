#include <QApplication>
#include <QSettings>

#include "core/log.hpp"
#include "ui/main_window.hpp"
#include "ui/theme.hpp"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Apex"));
    QApplication::setOrganizationName(QStringLiteral("Apex"));
    apex::log::init();
    QStringList themeErrors;
    apex::ui::loadCustomThemes(apex::ui::themesDir(), &themeErrors);
    for (const QString& e : themeErrors) apex::log::warn("theme: {}", e.toStdString());
    apex::ui::applyThemeId(app, QSettings().value(QStringLiteral("ui/theme")).toString());
    apex::ui::MainWindow window;
    window.reloadProfiles();
    window.show();
    if (argc > 1) window.openFile(QString::fromLocal8Bit(argv[1]));  // apex [map.vmf]
    return QApplication::exec();
}
