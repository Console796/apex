#pragma once

// Vulkan instance, physical device, logical device, graphics queue and VMA allocator (plan 0.1,
// D13). One Context per process (D12: one VkDevice, one graphics queue, main thread only). All
// other render objects (Buffer, Image, Swapchain, TextureHeap, RenderTarget) borrow the Context
// and must be destroyed before it.

#include "core/result.hpp"
#include "render/vk.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct VmaAllocator_T;

namespace apex::render {

// Collects validation-layer messages of severity error and warning. Thread-safe. Tests should
// assert errorCount() == 0 after rendering.
class ValidationLog {
  public:
    std::size_t errorCount() const;
    std::size_t warningCount() const;
    // Up to the first 64 error messages.
    std::vector<std::string> errors() const;
    void clear();
    void record(bool isError, std::string message); // Called by the debug messenger.

  private:
    mutable std::mutex mutex_;
    std::size_t errorCount_ = 0;
    std::size_t warningCount_ = 0;
    std::vector<std::string> errors_;
};

struct ContextOptions {
    // Extra instance extensions, e.g. VK_KHR_surface plus the platform surface extension for UI
    // use (QVulkanInstance::supportedExtensions / required extensions). Leave empty for headless.
    // A missing extension fails creation.
    std::vector<std::string> instanceExtensions;
    // Validation layer + debug messenger. Default: on in debug builds, off otherwise; the
    // environment variable APEX_VK_VALIDATION=1/0 overrides. When the layer is not installed
    // creation still succeeds and DeviceInfo::validation is false.
    bool validation =
#ifdef NDEBUG
        false;
#else
        true;
#endif
    // Substring (case-insensitive) of the device name to force, e.g. "llvmpipe". The environment
    // variable APEX_VK_DEVICE, when set, takes precedence. No match is an error.
    std::string deviceNameFilter;
};

struct DeviceInfo {
    std::string name;
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    std::uint32_t apiVersion = 0;
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    bool rayQuery = false;              // VK_KHR_ray_query available (reported, not enabled).
    bool accelerationStructure = false; // VK_KHR_acceleration_structure available (ditto).
    bool swapchain = false;             // VK_KHR_swapchain enabled.
    bool anisotropy = false;            // samplerAnisotropy enabled.
    bool fillModeNonSolid = false;      // Wireframe polygon mode enabled.
    bool validation = false;            // Validation layer active on the instance.
};

// Only one Context may be alive at a time: volk binds its global vk* pointers to the latest
// instance/device, so a second Context breaks the first (tests share one via shared_context.hpp).
class Context {
  public:
    // Required device baseline: Vulkan 1.3 with dynamicRendering, synchronization2,
    // descriptorIndexing (runtimeDescriptorArray, partially bound and update-after-bind sampled
    // images, non-uniform sampled image indexing), bufferDeviceAddress and textureCompressionBC.
    static Result<std::unique_ptr<Context>> create(const ContextOptions& options = {});
    static bool instanceExtensionSupported(const char* name); // Loads the loader on first use.

    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    VkInstance instance() const { return instance_; } // For ui/ to wrap (QVulkanInstance).
    VkPhysicalDevice physicalDevice() const { return physical_; }
    VkDevice device() const { return device_; }
    VkQueue graphicsQueue() const { return queue_; }
    std::uint32_t graphicsFamily() const { return family_; }
    VmaAllocator_T* allocator() const { return allocator_; }
    const DeviceInfo& info() const { return info_; }
    ValidationLog& validation() const { return *validation_; }

    // Blocking helpers for setup, uploads and tests (not the frame path).
    // Records into a fresh primary command buffer, submits and waits.
    Result<void> oneShot(const std::function<void(VkCommandBuffer)>& record);
    VkCommandPool commandPool() const { return pool_; } // Resettable; main thread only.
    // Submits a recorded command buffer and waits for completion.
    Result<void> submitAndWait(VkCommandBuffer commands);
    void waitIdle() const;

  private:
    Context() = default;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t family_ = 0;
    VmaAllocator_T* allocator_ = nullptr;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    DeviceInfo info_;
    std::unique_ptr<ValidationLog> validation_;
};

} // namespace apex::render
