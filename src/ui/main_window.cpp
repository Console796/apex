#include "ui/main_window.hpp"

#include <QString>

#include "core/version.hpp"

namespace apex::ui {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    const auto& v = version();
    setWindowTitle(QStringLiteral("Apex %1").arg(QString::fromUtf8(v.string.data(), static_cast<qsizetype>(v.string.size()))));
    resize(1280, 800);
}

}  // namespace apex::ui
