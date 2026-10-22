#include "ui/pointer_lock.hpp"

#include <QGuiApplication>
#include <QWindow>
#include <qpa/qplatformnativeinterface.h>
#include <wayland-client.h>
#include <cstring>
#include <utility>

#include "ui/input_log.hpp"

#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"

namespace apex::ui {
namespace {

// All proxies use Qt's display and default queue: Qt continues to dispatch input.
// Qt owns the connection; discovery needs just one initial registry round trip.
class WaylandPointerLock final : public PointerLockBackend {
public:
    explicit WaylandPointerLock(QWindow& window) : window_(window) {}
    ~WaylandPointerLock() override {
        unlock();
        if (constraints_) zwp_pointer_constraints_v1_destroy(constraints_);
        if (manager_) zwp_relative_pointer_manager_v1_destroy(manager_);
        if (registry_) wl_registry_destroy(registry_);
    }

    Motion lock(QPointF position, Delta delta, Cancel cancel) override {
        auto* native = QGuiApplication::platformNativeInterface();
        if (!native) return fallback("qt-native-interface-unavailable");
        if (!window_.handle()) return fallback("window-has-no-platform-handle");
        QWindow* target = &window_;
        while (target->parent()) target = target->parent();
        if (!target->handle()) return fallback("toplevel-window-has-no-platform-handle");
        auto* display = static_cast<wl_display*>(native->nativeResourceForIntegration("wl_display"));
        auto* surface = static_cast<wl_surface*>(native->nativeResourceForWindow("surface", target));
        auto* pointer = static_cast<wl_pointer*>(native->nativeResourceForIntegration("wl_pointer"));
        // Qt 6.10.2's nativeResourceForIntegration supports "wl_pointer" (also
        // "QtWayland::wl_pointer"). Borrow it; never replace Qt's listener or seat.
        if (!display) return fallback("qt-wl_display-unavailable");
        if (!surface) return fallback("toplevel-wl_surface-unavailable");
        if (!pointer) return fallback("qt-wl_pointer-unavailable");
        if (!registry_) {
            display_ = display;
            registry_ = wl_display_get_registry(display_);
            static const wl_registry_listener listener{global, globalRemoved};
            wl_registry_add_listener(registry_, &listener, this);
            // Discover globals once; do not pump a nested Qt event loop.
            if (wl_display_roundtrip(display_) < 0) return fallback("registry-roundtrip-failed");
        }
        if (wl_display_get_error(display)) return fallback("wayland-display-error");
        if (!constraints_) return fallback("compositor-missing-zwp_pointer_constraints_v1");
        if (!manager_) return fallback("compositor-missing-zwp_relative_pointer_manager_v1");
        surface_ = surface;
        targetWindow_ = target;
        surfaceTarget_ = target == &window_ ? "pane" : "toplevel";
        restore_ = target->mapFromGlobal(window_.mapToGlobal(position));
        delta_ = std::move(delta);
        cancel_ = std::move(cancel);
        relative_ = zwp_relative_pointer_manager_v1_get_relative_pointer(manager_, pointer);
        static const zwp_relative_pointer_v1_listener relativeListener{relativeMotion};
        zwp_relative_pointer_v1_add_listener(relative_, &relativeListener, this);
        locked_ = zwp_pointer_constraints_v1_lock_pointer(
            constraints_, surface_, pointer, nullptr, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
        static const zwp_locked_pointer_v1_listener lockListener{locked, unlocked};
        zwp_locked_pointer_v1_add_listener(locked_, &lockListener, this);
        // The hint is surface-local, double buffered, and must be committed before
        // destroying the lock. The pointer otherwise remains fixed at the press.
        zwp_locked_pointer_v1_set_cursor_position_hint(
            locked_, wl_fixed_from_double(restore_.x()), wl_fixed_from_double(restore_.y()));
        wl_surface_commit(surface_);
        wl_display_flush(display_);
        if (inputLogEnabled()) {
            const auto paneGlobal = window_.mapToGlobal(QPointF(0, 0));
            const auto targetGlobal = target->mapToGlobal(QPointF(0, 0));
            inputTraceEvent("pointer_lock", {{"event", "request"}, {"surface_target", surfaceTarget_},
                {"pane_window_id", QString::number(reinterpret_cast<quintptr>(&window_), 16)},
                {"target_window_id", QString::number(reinterpret_cast<quintptr>(target), 16)},
                {"pane_origin", QJsonObject{{"x", paneGlobal.x()}, {"y", paneGlobal.y()}}},
                {"target_origin", QJsonObject{{"x", targetGlobal.x()}, {"y", targetGlobal.y()}}},
                {"hint", QJsonObject{{"x", restore_.x()}, {"y", restore_.y()}}},
                {"lifetime", "oneshot"}, {"surface_commit", true}});
        }
        decision("relative-pending", "lock-request-sent-awaiting-compositor");
        return Motion::Relative;
    }

    void unlock() override {
        const bool hadRequest = locked_ || relative_ || delta_ || cancel_;
        if (hadRequest && inputLogEnabled())
            inputTraceEvent("pointer_lock", {{"event", "unlock"}, {"locked_confirmed", lockedConfirmed_},
                                             {"relative_events", static_cast<qint64>(relativeEvents_)}});
        lockedConfirmed_ = false;
        relativeEvents_ = 0;
        delta_ = {};
        cancel_ = {};
        if (locked_) {
            // The hint is applied when the lock is destroyed. Re-submit it on the
            // constrained top-level surface so an active lock restores the pointer
            // to the pane press position when the gesture ends.
            if (surface_) {
                zwp_locked_pointer_v1_set_cursor_position_hint(
                    locked_, wl_fixed_from_double(restore_.x()), wl_fixed_from_double(restore_.y()));
                wl_surface_commit(surface_);
            }
            zwp_locked_pointer_v1_destroy(locked_);
            locked_ = nullptr;
        }
        if (relative_) {
            zwp_relative_pointer_v1_destroy(relative_);
            relative_ = nullptr;
        }
        surface_ = nullptr;
        targetWindow_ = nullptr;
        surfaceTarget_ = "unresolved";
        if (display_) wl_display_flush(display_);
    }

    void warp(QPoint) override {}  // Wayland only permits the lock's position hint.

private:
    void decision(const char* result, const char* reason) const {
        if (inputLogEnabled())
            inputTraceEvent("pointer_lock", {{"event", result}, {"reason", reason},
                {"window_id", QString::number(reinterpret_cast<quintptr>(&window_), 16)},
                {"surface_target", surfaceTarget_},
                {"target_window_id", QString::number(reinterpret_cast<quintptr>(targetWindow_), 16)},
                {"locked_confirmed", lockedConfirmed_},
                {"relative_events", static_cast<qint64>(relativeEvents_)}});
    }

    Motion fallback(const char* reason) const {
        decision("fallback", reason);
        return Motion::Fallback;
    }

    static void global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
        auto& self = *static_cast<WaylandPointerLock*>(data);
        if (std::strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) {
            self.constraints_ = static_cast<zwp_pointer_constraints_v1*>(
                wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1));
            self.constraintsName_ = name;
        } else if (std::strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
            self.manager_ = static_cast<zwp_relative_pointer_manager_v1*>(
                wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1));
            self.managerName_ = name;
        }
    }

    static void globalRemoved(void* data, wl_registry*, uint32_t name) {
        auto& self = *static_cast<WaylandPointerLock*>(data);
        if (name != self.constraintsName_ && name != self.managerName_) return;
        self.decision("cancel", "compositor-removed-pointer-protocol");
        if (self.cancel_) {
            const auto cancel = self.cancel_;
            cancel();
        }
        if (name == self.constraintsName_ && self.constraints_) {
            zwp_pointer_constraints_v1_destroy(self.constraints_);
            self.constraints_ = nullptr;
        }
        if (name == self.managerName_ && self.manager_) {
            zwp_relative_pointer_manager_v1_destroy(self.manager_);
            self.manager_ = nullptr;
        }
    }

    static void locked(void* data, zwp_locked_pointer_v1*) {
        auto& self = *static_cast<WaylandPointerLock*>(data);
        self.lockedConfirmed_ = true;
        self.decision("locked", "compositor-confirmed-pointer-lock");
    }

    static void unlocked(void* data, zwp_locked_pointer_v1*) {
        auto& self = *static_cast<WaylandPointerLock*>(data);
        self.lockedConfirmed_ = false;
        self.decision("unlocked", "compositor-released-pointer-lock");
        // Copy: cancellation destroys the proxies and clears the stored callback.
        const auto cancel = self.cancel_;
        if (cancel) cancel();
    }

    static void relativeMotion(void* data, zwp_relative_pointer_v1*, uint32_t hi, uint32_t lo,
                               wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t dxUnaccel, wl_fixed_t dyUnaccel) {
        auto& self = *static_cast<WaylandPointerLock*>(data);
        const QPointF accelerated(wl_fixed_to_double(dx), wl_fixed_to_double(dy));
        ++self.relativeEvents_;
        if (inputLogEnabled())
            inputTraceEvent("relative", {{"window_id", QString::number(reinterpret_cast<quintptr>(&self.window_), 16)},
                {"n", static_cast<qint64>(self.relativeEvents_)}, {"locked_confirmed", self.lockedConfirmed_},
                {"time_us", static_cast<double>((static_cast<uint64_t>(hi) << 32) | lo)},
                {"delta", QJsonObject{{"x", accelerated.x()}, {"y", accelerated.y()}}},
                {"unaccelerated", QJsonObject{{"x", wl_fixed_to_double(dxUnaccel)},
                                               {"y", wl_fixed_to_double(dyUnaccel)}}}});
        // relative_pointer events are valid independently of the constraint's locked
        // notification. Keep navigation alive while the gesture owns this proxy.
        if (self.relative_ && self.delta_) self.delta_(accelerated);
    }

    QWindow& window_;
    wl_display* display_ = nullptr;  // Borrowed from Qt, as is surface_.
    wl_surface* surface_ = nullptr;
    QWindow* targetWindow_ = nullptr;
    wl_registry* registry_ = nullptr;
    zwp_pointer_constraints_v1* constraints_ = nullptr;
    zwp_relative_pointer_manager_v1* manager_ = nullptr;
    zwp_locked_pointer_v1* locked_ = nullptr;
    zwp_relative_pointer_v1* relative_ = nullptr;
    uint32_t constraintsName_ = 0, managerName_ = 0;
    QPointF restore_;
    Delta delta_;
    Cancel cancel_;
    std::uint64_t relativeEvents_ = 0;
    bool lockedConfirmed_ = false;
    const char* surfaceTarget_ = "unresolved";
};

}  // namespace

std::unique_ptr<PointerLockBackend> makeWaylandPointerLockBackend(QWindow& window) {
    return std::make_unique<WaylandPointerLock>(window);
}

}  // namespace apex::ui
