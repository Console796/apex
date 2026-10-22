#include "ui/pointer_lock.hpp"

#include <QGuiApplication>
#include <QWindow>
#include <qpa/qplatformnativeinterface.h>
#include <wayland-client.h>
#include <cstring>
#include <utility>

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
        if (!native || !window_.handle()) return Motion::Fallback;
        auto* display = static_cast<wl_display*>(native->nativeResourceForIntegration("wl_display"));
        auto* surface = static_cast<wl_surface*>(native->nativeResourceForWindow("surface", &window_));
        auto* pointer = static_cast<wl_pointer*>(native->nativeResourceForIntegration("wl_pointer"));
        if (!display || !surface || !pointer) return Motion::Fallback;
        if (!registry_) {
            display_ = display;
            registry_ = wl_display_get_registry(display_);
            static const wl_registry_listener listener{global, globalRemoved};
            wl_registry_add_listener(registry_, &listener, this);
            // Discover globals once; do not pump a nested Qt event loop.
            if (wl_display_roundtrip(display_) < 0) return Motion::Fallback;
        }
        if (!constraints_ || !manager_) return Motion::Fallback;
        surface_ = surface;
        restore_ = position;
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
        return Motion::Relative;
    }

    void unlock() override {
        active_ = false;
        delta_ = {};
        cancel_ = {};
        if (locked_) {
            zwp_locked_pointer_v1_destroy(locked_);
            locked_ = nullptr;
        }
        if (relative_) {
            zwp_relative_pointer_v1_destroy(relative_);
            relative_ = nullptr;
        }
        surface_ = nullptr;
        if (display_) wl_display_flush(display_);
    }

    void warp(QPoint) override {}  // Wayland only permits the lock's position hint.

private:
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
        static_cast<WaylandPointerLock*>(data)->active_ = true;
    }

    static void unlocked(void* data, zwp_locked_pointer_v1*) {
        auto& self = *static_cast<WaylandPointerLock*>(data);
        self.active_ = false;
        // Copy: cancellation destroys the proxies and clears the stored callback.
        const auto cancel = self.cancel_;
        if (cancel) cancel();
    }

    static void relativeMotion(void* data, zwp_relative_pointer_v1*, uint32_t, uint32_t,
                               wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t, wl_fixed_t) {
        auto& self = *static_cast<WaylandPointerLock*>(data);
        if (self.active_ && self.delta_)
            self.delta_(QPointF(wl_fixed_to_double(dx), wl_fixed_to_double(dy)));
    }

    QWindow& window_;
    wl_display* display_ = nullptr;  // Borrowed from Qt, as is surface_.
    wl_surface* surface_ = nullptr;
    wl_registry* registry_ = nullptr;
    zwp_pointer_constraints_v1* constraints_ = nullptr;
    zwp_relative_pointer_manager_v1* manager_ = nullptr;
    zwp_locked_pointer_v1* locked_ = nullptr;
    zwp_relative_pointer_v1* relative_ = nullptr;
    uint32_t constraintsName_ = 0, managerName_ = 0;
    QPointF restore_;
    Delta delta_;
    Cancel cancel_;
    bool active_ = false;
};

}  // namespace

std::unique_ptr<PointerLockBackend> makeWaylandPointerLockBackend(QWindow& window) {
    return std::make_unique<WaylandPointerLock>(window);
}

}  // namespace apex::ui
