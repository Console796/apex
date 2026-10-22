#pragma once

// One swapchain per VkSurfaceKHR with on-demand presentation (plan 0.1, D13 and the 0.1.1 spike).
// The surface comes from ui/ (QVulkanInstance::surfaceForWindow) and stays owned there; destroy
// the Swapchain before the window/surface. Never rely on Qt's SurfaceAboutToBeDestroyed, and do
// not reparent viewport windows after creation (see D13 spike result).
//
// Frames are rendered only when asked: renderFrame() acquires an image, runs the callback to
// record commands, submits, and presents. A size change or VK_ERROR_OUT_OF_DATE_KHR /
// VK_SUBOPTIMAL_KHR recreates the swapchain transparently. One frame in flight.

#include "core/result.hpp"
#include "render/context.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace apex::render {

struct SwapchainFrame {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::uint32_t index = 0;
};

enum class FrameStatus {
    Presented,
    Skipped, // Zero-sized, out of date, or between resize-drag intervals: nothing was drawn.
             // Retry on the next expose/update.
};

class Swapchain {
  public:
    // The callback records into `commands` with the image in COLOR_ATTACHMENT_OPTIMAL and must
    // leave it in that layout. Pipelines should target frame.format (B8G8R8A8_UNORM or
    // R8G8B8A8_UNORM when the surface offers it; no sRGB encoding is applied by hardware).
    using RecordFn = std::function<void(VkCommandBuffer commands, const SwapchainFrame& frame)>;

    // Fails with Unsupported if the device has no swapchain support or the graphics queue cannot
    // present to the surface. `pixelSize` is used when the surface does not dictate an extent.
    static Result<std::unique_ptr<Swapchain>> create(Context& context, VkSurfaceKHR surface,
                                                     VkExtent2D pixelSize);
    ~Swapchain();
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // Draws and presents one frame at the window's current pixel size.
    using Clock = std::chrono::steady_clock;
    Result<FrameStatus> renderFrame(VkExtent2D pixelSize, const RecordFn& record,
                                    Clock::time_point now = Clock::now());
    // The UI's precise 16 ms tick controls drag cadence. Release forces a final resize.
    void setResizeDragging(bool dragging, Clock::time_point now = Clock::now());
    bool hasRetiredSwapchains() const { return !retired_.empty(); }
    std::size_t retiredSwapchainCount() const { return retired_.size(); }

    VkFormat format() const { return format_; }
    VkExtent2D extent() const { return extent_; }
    std::uint32_t imageCount() const { return static_cast<std::uint32_t>(images_.size()); }
    std::uint32_t recreateCount() const { return recreates_; } // For tests and diagnostics.
    std::uint32_t forcedRetirementCount() const { return forcedRetirements_; }
    double lastRecreateMilliseconds() const { return lastRecreateMilliseconds_; }

  private:
    Swapchain() = default;
    Result<bool> recreate(VkExtent2D pixelSize); // false: surface has zero size.
    void destroyImages();
    void destroyRetired();
    struct Retired {
        VkSwapchainKHR swapchain;
        std::vector<VkImageView> views;
        std::vector<VkSemaphore> renderDone;
    };
    std::vector<Retired> retired_;
    // Reacquiring the first presented image of the replacement, then completing its frame
    // fence (which waits on acquisition), proves that the old presentation has finished.
    // See docs.vulkan.org/samples/latest/samples/api/swapchain_recreation/README.html.
    // Continuous resizing can defer retirement until release; the UI keeps ticking to collect it.
    std::uint32_t firstPresented_ = UINT32_MAX;
    bool retirementReady_ = false;
    bool resizeDragging_ = false;

    Context* context_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkSemaphore> renderDone_; // One per image: presentation holds them past the frame.
    VkSemaphore imageAvailable_ = VK_NULL_HANDLE;
    VkFence inFlight_ = VK_NULL_HANDLE;
    VkCommandBuffer commands_ = VK_NULL_HANDLE;
    bool needsRecreate_ = true;
    std::uint32_t recreates_ = 0;
    std::uint32_t forcedRetirements_ = 0;
    double lastRecreateMilliseconds_ = 0.0;
};

} // namespace apex::render
