#pragma once

// Common Vulkan plumbing for apex::render: the volk header and VkResult -> Error conversion.
// Every render header that mentions Vulkan types includes this instead of <vulkan/vulkan.h>, so
// that no translation unit links the Vulkan loader statically (volk resolves everything).

#include "core/result.hpp"

#include <volk.h>

#include <string_view>

namespace apex::render {

// "VK_ERROR_DEVICE_LOST" style name; "VkResult(n)" for unknown values.
const char* vkResultName(VkResult result);

// ErrorCode::Unsupported for missing features/extensions/drivers, Internal otherwise.
Error vkError(VkResult result, std::string_view what);

// Zero-initialised Vulkan structure with sType set (avoids -Wmissing-field-initializers).
template <typename T> T vkStruct(VkStructureType type) {
    T value{};
    value.sType = type;
    return value;
}

} // namespace apex::render

// Returns std::unexpected(vkError(...)) from the enclosing function when call != VK_SUCCESS.
#define APEX_VK_TRY(call, what)                                                                    \
    do {                                                                                           \
        if (const VkResult apexVkResult_ = (call); apexVkResult_ != VK_SUCCESS)                    \
            return std::unexpected(::apex::render::vkError(apexVkResult_, (what)));                \
    } while (false)
