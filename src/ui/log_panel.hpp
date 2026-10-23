#pragma once

#include <QPlainTextEdit>

namespace apex::ui {

// Log console. One process-wide core/log sink copies lines to the UI thread and keeps the recent
// ones, so any number of Log areas (and one opened later) show the same history.
class LogPanel : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit LogPanel(QWidget* parent = nullptr);
    ~LogPanel() override;
    // Starts keeping lines before the first panel exists (the main window calls it first thing).
    static void startCapture();

public slots:
    void append(int level, const QString& line);
};

}  // namespace apex::ui
