#pragma once

#include <QMainWindow>

namespace apex::ui {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
};

}  // namespace apex::ui
