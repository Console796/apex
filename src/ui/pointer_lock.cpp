#include "ui/pointer_lock.hpp"

#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPlatformSurfaceEvent>
#include <QWindow>
#include <algorithm>
#include <utility>

#include "ui/input_log.hpp"

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace apex::ui {

#ifdef APEX_HAS_WAYLAND_POINTER_LOCK
std::unique_ptr<PointerLockBackend> makeWaylandPointerLockBackend(QWindow& window);
#endif

namespace {
class DesktopPointerLock final : public PointerLockBackend {
public:
    explicit DesktopPointerLock(QWindow& window) : window_(window) {}
    Motion lock(QPointF position, Delta, Cancel) override {
        restore_ = window_.mapToGlobal(position.toPoint());
#ifdef Q_OS_WIN
        if (inputLogEnabled()) inputTraceEvent("pointer_lock", {{"event", "lock"}, {"motion", "warp"},
                                                               {"reason", "windows-cursor-warp"}});
        return Motion::Warp;
#else
        const bool warp = QGuiApplication::platformName() == QLatin1String("xcb");
        const char* reason = warp ? "xcb-cursor-warp" :
            QGuiApplication::platformName().startsWith(QLatin1String("wayland")) ?
                "wayland-native-backend-not-built" : "no-native-backend-for-platform";
        if (inputLogEnabled())
            inputTraceEvent("pointer_lock", {{"event", "lock"}, {"motion", warp ? "warp" : "fallback"},
                                             {"reason", reason}, {"platform", QGuiApplication::platformName()}});
        return warp ? Motion::Warp : Motion::Fallback;
#endif
    }
    void unlock() override {
#ifdef Q_OS_WIN
        warp(restore_);
#else
        if (QGuiApplication::platformName() == QLatin1String("xcb")) warp(restore_);
#endif
    }
    void warp(QPoint global) override {
#ifdef Q_OS_WIN
        // Qt positions are logical; SetCursorPos expects native screen pixels.
        const QPoint local = window_.mapFromGlobal(global);
        POINT point{qRound(local.x() * window_.devicePixelRatio()),
                    qRound(local.y() * window_.devicePixelRatio())};
        if (ClientToScreen(reinterpret_cast<HWND>(window_.winId()), &point))
            SetCursorPos(point.x, point.y);
#else
        QCursor::setPos(global);
#endif
    }

private:
    QWindow& window_;
    QPoint restore_;
};
}  // namespace

std::unique_ptr<PointerLockBackend> makePointerLockBackend(QWindow& window) {
#ifdef APEX_HAS_WAYLAND_POINTER_LOCK
    if (QGuiApplication::platformName().startsWith(QLatin1String("wayland")))
        return makeWaylandPointerLockBackend(window);
#endif
    return std::make_unique<DesktopPointerLock>(window);
}

PointerLock::PointerLock(QWindow& window, std::unique_ptr<PointerLockBackend> backend,
                        std::function<void()> cancelled)
    : window_(window), backend_(std::move(backend)), cancelled_(std::move(cancelled)) {
    window_.installEventFilter(this);
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive) cancel();
    });
}

PointerLock::~PointerLock() {
    cancelled_ = {};
    cancel();
}

void PointerLock::press(Qt::MouseButton button, QPointF position) {
    if (button != Qt::MiddleButton && button != Qt::RightButton) return;
    if (active()) {
        buttons_ |= button;
        if (button == Qt::RightButton && !hidden_) hideCursor();
        return;
    }
    buttons_ = button;
    last_ = position;
    delta_ = {};
    relativeSeen_ = false;
    if (inputLogEnabled())
        inputTraceEvent("pointer_lock", {{"event", "press"},
                                         {"window_id", QString::number(reinterpret_cast<quintptr>(&window_), 16)},
                                         {"platform", QGuiApplication::platformName()},
                                         {"button", static_cast<qint64>(button)},
                                         {"local", QJsonObject{{"x", position.x()}, {"y", position.y()}}}});
    motion_ = backend_->lock(position, [this](QPointF delta) {
        if (!active()) return;
        if (!relativeSeen_ && inputLogEnabled())
            inputTraceEvent("pointer_lock", {{"event", "relative_source_active"},
                                             {"reason", "first-relative-event"}});
        relativeSeen_ = true;
        delta_ += delta;
    }, [this] { cancel(); });
    if (inputLogEnabled())
        inputTraceEvent("pointer_lock", {{"event", "lock_return"},
            {"window_id", QString::number(reinterpret_cast<quintptr>(&window_), 16)},
            {"motion", motion_ == PointerLockBackend::Motion::Relative ? "relative" :
                       motion_ == PointerLockBackend::Motion::Warp ? "warp" : "fallback"},
            {"gesture_active", active()}, {"reason", "backend-returned"}});
    // Native discovery can dispatch a focus loss before lock() returns.
    if (!active()) {
        backend_->unlock();
        return;
    }
    if (motion_ != PointerLockBackend::Motion::Fallback || button == Qt::RightButton) hideCursor();
}

void PointerLock::hideCursor() {
    cursor_ = window_.cursor();
    window_.setCursor(Qt::BlankCursor);
    hidden_ = true;
}

void PointerLock::buttons(Qt::MouseButtons buttons) {
    if (!active()) return;
    const auto remaining = buttons_ & buttons;
    if (remaining == Qt::NoButton) finish(false);
    else buttons_ = remaining;
}

void PointerLock::move(QPointF position) {
    if (!active() || (motion_ == PointerLockBackend::Motion::Relative && relativeSeen_)) return;
    delta_ += position - last_;
    last_ = position;
    if (motion_ == PointerLockBackend::Motion::Relative) return; // Pending lock: temporary Qt-motion fallback.
    if (motion_ != PointerLockBackend::Motion::Warp) return;
    const double margin = std::min({32.0, window_.width() / 4.0, window_.height() / 4.0});
    if (position.x() > margin && position.y() > margin &&
        position.x() < window_.width() - margin && position.y() < window_.height() - margin) return;
    last_ = QPointF(window_.width() / 2, window_.height() / 2);
    backend_->warp(window_.mapToGlobal(last_.toPoint()));
}

void PointerLock::cancel() { finish(true); }

void PointerLock::finish(bool notify) {
    if (!active()) return;
    buttons_ = Qt::NoButton;
    backend_->unlock();
    if (hidden_) window_.setCursor(cursor_);
    hidden_ = false;
    relativeSeen_ = false;
    delta_ = {};
    if (notify && cancelled_) cancelled_();
}

QPointF PointerLock::takeDelta() { return std::exchange(delta_, {}); }

bool PointerLock::eventFilter(QObject*, QEvent* event) {
    if (!active()) return false;
    switch (event->type()) {
    case QEvent::FocusOut:
    case QEvent::Hide:
        cancel();
        break;
    case QEvent::KeyPress:
        if (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            cancel();
            event->accept();
            return true;
        }
        break;
    case QEvent::PlatformSurface:
        if (static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() ==
            QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) cancel();
        break;
    default: break;
    }
    return false;
}

}  // namespace apex::ui
