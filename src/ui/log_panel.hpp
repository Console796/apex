#pragma once

#include <QPlainTextEdit>
#include <cstdint>

#include "core/log.hpp"

namespace apex::ui {

// Log console: a core/log sink whose lines are copied and marshalled to the UI thread.
class LogPanel : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit LogPanel(QWidget* parent = nullptr);
    ~LogPanel() override;

public slots:
    void append(int level, const QString& line);

private:
    log::SinkId sink_ = 0;
};

}  // namespace apex::ui
