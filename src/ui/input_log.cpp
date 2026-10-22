#include "ui/input_log.hpp"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QScreen>
#include <QWidget>
#include <QWindow>

namespace apex::ui {
namespace {
QJsonObject point(QPointF p) { return {{"x", p.x()}, {"y", p.y()}}; }
QJsonObject rect(QRect r) {
    return {{"x", r.x()}, {"y", r.y()}, {"w", r.width()}, {"h", r.height()}};
}

class InputTrace final : public QObject {
public:
    InputTrace() {
        const auto path = qEnvironmentVariable("APEX_INPUT_LOG");
        if (path.isEmpty()) return;
        file_.setFileName(path);
        if (!file_.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            qWarning().noquote() << "APEX_INPUT_LOG:" << file_.errorString() << path;
            return;
        }
        clock_.start();
        QJsonObject start{{"executable", QCoreApplication::applicationFilePath()},
                          {"qt", QString::fromLatin1(qVersion())},
                          {"platform", QGuiApplication::platformName()},
                          {"pid", static_cast<qint64>(QCoreApplication::applicationPid())}};
#ifdef Q_OS_LINUX
        start["fd_count"] = fdCount(true);
#endif
        if (const auto* screen = QGuiApplication::primaryScreen()) {
            start["scale"] = screen->devicePixelRatio();
            start["screen_geometry"] = rect(screen->geometry());
        }
        write("start", std::move(start));
    }

    bool enabled() const { return file_.isOpen(); }

    void initialize() {
        if (!enabled() || installed_) return;
        qApp->installEventFilter(this);
        installed_ = true;
    }

    void write(const char* type, QJsonObject fields) {
        if (!enabled()) return;
        const double milliseconds = static_cast<double>(clock_.nsecsElapsed()) / 1.0e6;
        fields["type"] = QLatin1String(type);
        fields["t_ms"] = milliseconds;
        file_.write(QJsonDocument(fields).toJson(QJsonDocument::Compact));
        file_.putChar('\n');
        // Bound data loss without forcing a filesystem flush for every mouse move.
        if (milliseconds - lastFlushMs_ >= 100.0 || qstrcmp(type, "marker") == 0) {
            file_.flush();
            lastFlushMs_ = milliseconds;
        }
    }

    void setState(QJsonObject state) { if (enabled()) state_ = std::move(state); }

    int fdCount(bool refresh) {
#ifdef Q_OS_LINUX
        if (!enabled()) return -1;
        const auto elapsed = clock_.isValid() ? clock_.elapsed() : 0;
        if (refresh || fdCount_ < 0 || elapsed - fdCountAtMs_ >= 100) {
            fdCount_ = static_cast<int>(QDir(QStringLiteral("/proc/self/fd"))
                                            .entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size());
            fdCountAtMs_ = elapsed;
        }
        return fdCount_;
#else
        Q_UNUSED(refresh);
        return -1;
#endif
    }

    int marker() {
        if (!enabled()) return 0;
        QJsonObject fields = state_;
        fields["marker"] = QStringLiteral("felt wrong");
        fields["n"] = ++markers_;
        fields["buttons"] = static_cast<qint64>(QGuiApplication::mouseButtons().toInt());
        fields["global"] = point(QCursor::pos());
        write("marker", std::move(fields));
        qInfo().noquote() << QStringLiteral("Input trace marker %1: felt wrong").arg(markers_);
        return markers_;
    }

protected:
    bool eventFilter(QObject* receiver, QEvent* event) override {
        if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseMove &&
            event->type() != QEvent::MouseButtonRelease) return false;
        mouse(receiver, *static_cast<QMouseEvent*>(event));
        return false;
    }

public:
    void mouse(QObject* receiver, const QMouseEvent& event) {
        if (!enabled()) return;
        QJsonObject fields{{"event", event.type() == QEvent::MouseButtonPress ? "press" :
                                     event.type() == QEvent::MouseButtonRelease ? "release" : "move"},
                           {"button", static_cast<qint64>(event.button())},
                           {"buttons", static_cast<qint64>(event.buttons().toInt())},
                           {"local", point(event.position())}, {"global", point(event.globalPosition())},
                           {"receiver_class", QString::fromLatin1(receiver->metaObject()->className())},
                           {"receiver_name", receiver->objectName()},
                           {"receiver_id", QString::number(reinterpret_cast<quintptr>(receiver), 16)}};
        QWindow* window = qobject_cast<QWindow*>(receiver);
        if (auto* widget = qobject_cast<QWidget*>(receiver)) window = widget->window()->windowHandle();
        if (window) {
            fields["window_geometry"] = rect(window->geometry());
            fields["scale"] = window->devicePixelRatio();
            fields["window_id"] = QString::number(reinterpret_cast<quintptr>(window), 16);
        }
        write("mouse", std::move(fields));
    }

private:
    QFile file_;
    QElapsedTimer clock_;
    QJsonObject state_;
    int markers_ = 0;
    bool installed_ = false;
    double lastFlushMs_ = -100.0;
    qint64 fdCountAtMs_ = -100;
    int fdCount_ = -1;
};

InputTrace& trace() {
    static InputTrace instance;
    return instance;
}
}  // namespace

bool inputLogEnabled() { return trace().enabled(); }
void inputTraceInitialize() { trace().initialize(); }
void inputTraceEvent(const char* type, QJsonObject fields) { trace().write(type, std::move(fields)); }
void inputTraceSetState(QJsonObject state) { trace().setState(std::move(state)); }
int inputTraceMarker() { return trace().marker(); }
int inputTraceFdCount(bool refresh) { return trace().fdCount(refresh); }

void inputTraceWindowStart(const QWidget& window) {
    if (!inputLogEnabled()) return;
    QJsonObject fields{{"geometry", rect(window.geometry())}, {"scale", window.devicePixelRatioF()}};
    if (window.windowHandle()) {
        fields["native_geometry"] = rect(window.windowHandle()->geometry());
        fields["native_scale"] = window.windowHandle()->devicePixelRatio();
    }
    inputTraceEvent("window", std::move(fields));
}

void inputTraceMouse(QObject* receiver, const QMouseEvent& event) { trace().mouse(receiver, event); }

}  // namespace apex::ui
