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
    const bool light = QSettings().value(QStringLiteral("ui/theme")).toString() == QStringLiteral("light");
    apex::ui::applyTheme(app, light ? apex::ui::Theme::Light : apex::ui::Theme::Dark);
    apex::ui::MainWindow window;
    window.reloadProfiles();
    window.show();
    if (argc > 1) window.openFile(QString::fromLocal8Bit(argv[1]));  // apex [map.vmf]
    return QApplication::exec();
}
