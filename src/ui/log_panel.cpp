#include "ui/log_panel.hpp"

#include <QApplication>
#include <QMetaObject>
#include <QPointer>
#include <QScrollBar>
#include <QTextCharFormat>
#include <QTextCursor>

#include "ui/theme.hpp"

namespace apex::ui {

LogPanel::LogPanel(QWidget* parent) : QPlainTextEdit(parent) {
    setReadOnly(true);
    setMaximumBlockCount(5000);
    setLineWrapMode(NoWrap);
    QFont mono(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::Monospace);
    setFont(mono);
    QPointer<LogPanel> self(this);
    sink_ = log::addSink([self](log::Level level, std::string_view message) {
        if (!qApp) return;
        const QString line = QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size()));
        const int lv = static_cast<int>(level);
        QMetaObject::invokeMethod(
            qApp, [self, lv, line] { if (self) self->append(lv, line); }, Qt::QueuedConnection);
    });
}

LogPanel::~LogPanel() { log::removeSink(sink_); }

void LogPanel::append(int level, const QString& line) {
    const Tokens& t = currentTokens();
    QTextCharFormat fmt;
    switch (static_cast<log::Level>(level)) {
    case log::Level::Error: fmt.setForeground(t.error); break;
    case log::Level::Warn: fmt.setForeground(t.warn); break;
    case log::Level::Debug:
    case log::Level::Trace: fmt.setForeground(t.textDim); break;
    default: fmt.setForeground(t.text); break;
    }
    const bool atEnd = verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 2;
    QTextCursor c(document());
    c.movePosition(QTextCursor::End);
    if (!document()->isEmpty()) c.insertBlock();
    c.insertText(line, fmt);
    if (atEnd) verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

}  // namespace apex::ui
