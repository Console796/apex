#include <doctest/doctest.h>

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QPlatformSurfaceEvent>
#include <QWindow>
#include <utility>
#include <vector>

#include "ui/pointer_lock.hpp"

#include "test_app.hpp"

using apex::ui_test::app;

namespace {
using apex::ui::PointerLock;
using apex::ui::PointerLockBackend;

class FakePointerLock final : public PointerLockBackend {
public:
    explicit FakePointerLock(QWindow& window) : window_(window) {}
    Motion lock(QPointF position, Delta delta, Cancel cancel) override {
        ++locks;
        restore = window_.mapToGlobal(position.toPoint());
        relative = std::move(delta);
        lost = std::move(cancel);
        return mode;
    }
    void unlock() override {
        ++unlocks;
        if (mode != Motion::Fallback) restored = restore;
    }
    void warp(QPoint global) override { warps.push_back(global); }

    Motion mode = Motion::Relative;
    int locks = 0, unlocks = 0;
    QPoint restore, restored;
    Delta relative;
    Cancel lost;
    std::vector<QPoint> warps;

private:
    QWindow& window_;
};
}  // namespace

TEST_CASE("pointer lock captures middle orbit/pan and right fly look until release") {
    app();
    QWindow window;
    window.resize(320, 200);
    window.setCursor(Qt::CrossCursor);
    auto backend = std::make_unique<FakePointerLock>(window);
    auto& fake = *backend;
    PointerLock lock(window, std::move(backend));
    for (auto button : {Qt::MiddleButton, Qt::RightButton}) {
        lock.press(button, {40, 70});
        CHECK(lock.active());
        CHECK(window.cursor().shape() == Qt::BlankCursor);
        const auto restore = fake.restore;
        lock.buttons(Qt::NoButton);
        CHECK_FALSE(lock.active());
        CHECK(fake.restored == restore);
        CHECK(window.cursor().shape() == Qt::CrossCursor);
    }
    CHECK(fake.locks == 2);
    CHECK(fake.unlocks == 2);
    lock.cancel();
    CHECK(fake.unlocks == 2);
    lock.press(Qt::LeftButton, {40, 70});
    CHECK_FALSE(lock.active());
    CHECK(fake.locks == 2);
}

TEST_CASE("pointer lock releases on focus loss hide Escape and surface destruction") {
    app();
    QWindow window;
    window.resize(320, 200);
    auto backend = std::make_unique<FakePointerLock>(window);
    auto& fake = *backend;
    int cancelled = 0;
    PointerLock lock(window, std::move(backend), [&] { ++cancelled; });
    QEvent focus(QEvent::FocusOut), hide(QEvent::Hide);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QPlatformSurfaceEvent surface(QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed);
    for (QEvent* event : {&focus, &hide, static_cast<QEvent*>(&escape), static_cast<QEvent*>(&surface)}) {
        lock.press(Qt::MiddleButton, {20, 30});
        QCoreApplication::sendEvent(&window, event);
        CHECK_FALSE(lock.active());
        CHECK(window.cursor().shape() != Qt::BlankCursor);
        CHECK(fake.restored == fake.restore);
    }
    CHECK(cancelled == 4);
    CHECK(fake.unlocks == 4);
    // A compositor can revoke a lock independently of Qt focus.
    lock.press(Qt::RightButton, {20, 30});
    fake.lost();
    CHECK_FALSE(lock.active());
    CHECK(cancelled == 5);
    CHECK(fake.unlocks == 5);
}

TEST_CASE("relative flicks accumulate beyond the pane without double counting absolute motion") {
    app();
    QWindow window;
    window.resize(320, 200);
    auto backend = std::make_unique<FakePointerLock>(window);
    auto& fake = *backend;
    PointerLock lock(window, std::move(backend));
    for (auto button : {Qt::MiddleButton, Qt::RightButton}) {
        lock.press(button, {20, 30});
        fake.relative({750.25, -80.5});
        fake.relative({900.5, 20.25});
        fake.relative({-100.0, 10.0});
        lock.move({319, 199});
        CHECK(lock.takeDelta() == QPointF(1550.75, -50.25));
        CHECK(lock.takeDelta().isNull());
        CHECK(fake.warps.empty());
        lock.buttons(Qt::NoButton);
        fake.relative({500, 500}); // A late event cannot navigate after release.
        CHECK(lock.takeDelta().isNull());
    }
}

TEST_CASE("relative mode uses Qt motion until its first native relative event") {
    app();
    QWindow window;
    window.resize(320, 200);
    auto backend = std::make_unique<FakePointerLock>(window);
    auto& fake = *backend;
    PointerLock lock(window, std::move(backend));
    lock.press(Qt::MiddleButton, {20, 30});
    lock.move({45, 65});
    CHECK(lock.takeDelta() == QPointF(25, 35));
    fake.relative({7, -3});
    lock.move({80, 90});
    CHECK(lock.takeDelta() == QPointF(7, -3));
    lock.buttons(Qt::NoButton);
}

TEST_CASE("warp backend recentres only near edges and keeps all flick deltas") {
    app();
    QWindow window;
    window.resize(320, 200);
    auto backend = std::make_unique<FakePointerLock>(window);
    auto& fake = *backend;
    fake.mode = PointerLockBackend::Motion::Warp;
    PointerLock lock(window, std::move(backend));
    for (auto button : {Qt::MiddleButton, Qt::RightButton}) {
        fake.warps.clear();
        lock.press(button, {100, 100});
        lock.move({160, 100});
        lock.move({200, 100});
        CHECK(fake.warps.empty());
        for (int i = 0; i < 4; ++i) {
            lock.move({310, 100});
            CHECK(fake.warps.size() == static_cast<std::size_t>(i + 1));
            CHECK(fake.warps.back() == window.mapToGlobal(QPoint(160, 100)));
            lock.move({160, 100}); // Synthetic warp notification adds no motion.
        }
        CHECK(lock.takeDelta() == QPointF(660, 0));
        lock.buttons(Qt::NoButton);
        CHECK(fake.restored == window.mapToGlobal(QPoint(100, 100)));
    }
}

TEST_CASE("fallback keeps local motion and lost-release recovery with mixed buttons") {
    app();
    QWindow window;
    auto backend = std::make_unique<FakePointerLock>(window);
    auto& fake = *backend;
    fake.mode = PointerLockBackend::Motion::Fallback;
    PointerLock lock(window, std::move(backend));
    lock.press(Qt::MiddleButton, {20, 30});
    CHECK(window.cursor().shape() != Qt::BlankCursor);
    lock.move({45, 65});
    CHECK(lock.takeDelta() == QPointF(25, 35));
    lock.press(Qt::RightButton, {45, 65});
    CHECK(fake.locks == 1);
    CHECK(window.cursor().shape() == Qt::BlankCursor);
    lock.buttons(Qt::RightButton);
    CHECK(lock.active());
    lock.buttons(Qt::NoButton); // The real button state repairs an undelivered release.
    CHECK_FALSE(lock.active());
    CHECK(fake.unlocks == 1);
    CHECK(window.cursor().shape() != Qt::BlankCursor);
    lock.buttons(Qt::NoButton);
    CHECK(fake.unlocks == 1);
    CHECK(fake.warps.empty());
}
