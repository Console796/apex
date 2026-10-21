#pragma once

#include "render/context.hpp"

#include <vulkan/vulkan.h>

#include <iostream>
#include <memory>

namespace apex::render::testing {

// The single Vulkan Context for the whole test binary. volk binds its global vk* entry points to the
// most recently created instance/device, so two Contexts alive at once break each other (and abort
// inside the validation layer at teardown). Every render test must go through this accessor.
// Fixtures that hold GPU objects must call it before creating them so their statics are destroyed
// before the Context. Returns nullptr (with a message) when no usable device exists.
inline Context* sharedContext() {
    static std::unique_ptr<Context> context = [] {
        ContextOptions options;
        // Instance-level extensions the swapchain test needs; harmless for the rest.
        for (const char* name : {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME}) {
            if (!Context::instanceExtensionSupported(name)) {
                options.instanceExtensions.clear();
                break;
            }
            options.instanceExtensions.emplace_back(name);
        }
        auto created = Context::create(options);
        if (!created) {
            std::cerr << "[render tests] no Vulkan device: " << created.error().what() << '\n';
            return std::unique_ptr<Context>();
        }
        return std::move(*created);
    }();
    return context.get();
}

} // namespace apex::render::testing
