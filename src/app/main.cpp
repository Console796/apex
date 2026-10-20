#include <QApplication>

#include "ui/main_window.hpp"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Apex"));
    apex::ui::MainWindow window;
    window.show();
    return QApplication::exec();
}
