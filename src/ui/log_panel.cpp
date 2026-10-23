#include "ui/log_panel.hpp"

#include <QApplication>
#include <QMetaObject>
#include <QPointer>
#include <QScrollBar>
#include <QTextCharFormat>
#include <QTextCursor>
#include <deque>
#include <utility>
#include <vector>

#include "core/log.hpp"

#include "ui/theme.hpp"

namespace apex::ui {
namespace {

constexpr int kKeep = 5000;

struct Feed {
    std::deque<std::pair<int, QString>> recent;
    std::vector<QPointer<LogPanel>> panels;
};

Feed& feed() {
    static Feed* instance = [] {
        auto* f = new Feed;  // Lives for the process: the sink may fire during shutdown.
        log::addSink([](log::Level level, std::string_view message) {
            if (!qApp) return;
            const QString line = QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size()));
            const int lv = static_cast<int>(level);
            QMetaObject::invokeMethod(qApp, [lv, line] {
                auto& fd = feed();
                fd.recent.emplace_back(lv, line);
                if (fd.recent.size() > kKeep) fd.recent.pop_front();
                std::erase_if(fd.panels, [](const QPointer<LogPanel>& p) { return !p; });
                for (auto& p : fd.panels) p->append(lv, line);
            }, Qt::QueuedConnection);
        });
        return f;
    }();
    return *instance;
}

}  // namespace

LogPanel::LogPanel(QWidget* parent) : QPlainTextEdit(parent) {
    setReadOnly(true);
    setMaximumBlockCount(5000);
    setLineWrapMode(NoWrap);
    QFont mono(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::Monospace);
    setFont(mono);
    auto& fd = feed();
    for (const auto& [level, line] : fd.recent) append(level, line);
    fd.panels.emplace_back(this);
}

LogPanel::~LogPanel() = default;

void LogPanel::startCapture() { (void)feed(); }

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
