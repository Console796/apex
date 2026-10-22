#pragma once

#include <QCursor>
#include <QObject>
#include <QPointF>
#include <functional>
#include <memory>

class QWindow;

namespace apex::ui {

// Native code stays behind this interface; headless input tests supply a backend.
class PointerLockBackend {
public:
    enum class Motion { Fallback, Warp, Relative };
    using Delta = std::function<void(QPointF)>;
    using Cancel = std::function<void()>;
    virtual ~PointerLockBackend() = default;
    virtual Motion lock(QPointF position, Delta delta, Cancel cancel) = 0;
    virtual void unlock() = 0;
    virtual void warp(QPoint global) = 0;
};

std::unique_ptr<PointerLockBackend> makePointerLockBackend(QWindow& window);

// One navigation gesture, shared by middle orbit/pan and right fly look.
class PointerLock final : public QObject {
public:
    PointerLock(QWindow& window, std::unique_ptr<PointerLockBackend> backend,
                std::function<void()> cancelled = {});
    ~PointerLock() override;
    void press(Qt::MouseButton button, QPointF position);
    void buttons(Qt::MouseButtons buttons);
    void move(QPointF position);
    void cancel();
    bool active() const { return buttons_ != Qt::NoButton; }
    QPointF takeDelta();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void finish(bool notify);
    void hideCursor();
    QWindow& window_;
    std::unique_ptr<PointerLockBackend> backend_;
    std::function<void()> cancelled_;
    PointerLockBackend::Motion motion_ = PointerLockBackend::Motion::Fallback;
    Qt::MouseButtons buttons_;
    QPointF last_, delta_;
    QCursor cursor_;
    bool hidden_ = false;
};

}  // namespace apex::ui
