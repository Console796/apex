#include "render/swapchain.hpp"

#include "render/resources.hpp"

#include <algorithm>
#include <limits>

namespace apex::render {

namespace {

// The surface's own extent when it dictates one, otherwise the window size clamped to limits.
VkExtent2D surfaceExtent(const VkSurfaceCapabilitiesKHR& caps, VkExtent2D pixelSize) {
    if (caps.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) return caps.currentExtent;
    return {std::clamp(pixelSize.width, caps.minImageExtent.width, caps.maxImageExtent.width),
            std::clamp(pixelSize.height, caps.minImageExtent.height, caps.maxImageExtent.height)};
}

} // namespace

Result<std::unique_ptr<Swapchain>> Swapchain::create(Context& context, VkSurfaceKHR surface,
                                                     VkExtent2D pixelSize) {
    if (!context.info().swapchain) {
        return std::unexpected(Error{ErrorCode::Unsupported, "device has no VK_KHR_swapchain"});
    }
    VkBool32 present = VK_FALSE;
    APEX_VK_TRY(vkGetPhysicalDeviceSurfaceSupportKHR(context.physicalDevice(), context.graphicsFamily(),
                                                     surface, &present),
                "vkGetPhysicalDeviceSurfaceSupportKHR");
    if (!present) {
        return std::unexpected(Error{ErrorCode::Unsupported, "graphics queue cannot present to this surface"});
    }
    std::unique_ptr<Swapchain> sc(new Swapchain());
    sc->context_ = &context;
    sc->surface_ = surface;

    VkSemaphoreCreateInfo si = vkStruct<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
    APEX_VK_TRY(vkCreateSemaphore(context.device(), &si, nullptr, &sc->imageAvailable_), "vkCreateSemaphore");
    VkFenceCreateInfo fi = vkStruct<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    APEX_VK_TRY(vkCreateFence(context.device(), &fi, nullptr, &sc->inFlight_), "vkCreateFence");
    VkCommandBufferAllocateInfo ai = vkStruct<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    ai.commandPool = context.commandPool();
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    APEX_VK_TRY(vkAllocateCommandBuffers(context.device(), &ai, &sc->commands_), "vkAllocateCommandBuffers");

    auto created = sc->recreate(pixelSize);
    if (!created) return std::unexpected(created.error());
    return sc;
}

Swapchain::~Swapchain() {
    if (!context_) return;
    VkDevice device = context_->device();
    vkDeviceWaitIdle(device);
    destroyImages();
    destroyRetired();
    if (swapchain_) vkDestroySwapchainKHR(device, swapchain_, nullptr);
    if (imageAvailable_) vkDestroySemaphore(device, imageAvailable_, nullptr);
    if (inFlight_) vkDestroyFence(device, inFlight_, nullptr);
    if (commands_) vkFreeCommandBuffers(device, context_->commandPool(), 1, &commands_);
}

void Swapchain::destroyImages() {
    VkDevice device = context_->device();
    for (VkImageView v : views_) vkDestroyImageView(device, v, nullptr);
    for (VkSemaphore s : renderDone_) vkDestroySemaphore(device, s, nullptr);
    views_.clear();
    renderDone_.clear();
    images_.clear();
}

void Swapchain::destroyRetired() {
    const VkDevice device = context_->device();
    for (const auto& old : retired_) {
        for (VkImageView view : old.views) vkDestroyImageView(device, view, nullptr);
        for (VkSemaphore done : old.renderDone) vkDestroySemaphore(device, done, nullptr);
        vkDestroySwapchainKHR(device, old.swapchain, nullptr);
    }
    retired_.clear();
}

void Swapchain::setResizeDragging(bool dragging, Clock::time_point now) {
    if (dragging == resizeDragging_) return;
    resizeDragging_ = dragging;
    lastDragFrame_ = now;
    if (!dragging) needsRecreate_ = true;
}

Result<bool> Swapchain::recreate(VkExtent2D pixelSize) {
    VkDevice device = context_->device();
    VkPhysicalDevice physical = context_->physicalDevice();
    // renderFrame has waited for this pane's frame fence; other panes keep running.

    VkSurfaceCapabilitiesKHR caps{};
    APEX_VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface_, &caps), "surface capabilities");
    const VkExtent2D extent = surfaceExtent(caps, pixelSize);
    if (extent.width == 0 || extent.height == 0) return false;

    std::uint32_t count = 0;
    APEX_VK_TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface_, &count, nullptr), "surface formats");
    std::vector<VkSurfaceFormatKHR> formats(count);
    APEX_VK_TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface_, &count, formats.data()), "surface formats");
    if (formats.empty()) return std::unexpected(Error{ErrorCode::Unsupported, "surface has no formats"});
    VkSurfaceFormatKHR chosen = formats.front();
    for (const VkFormat wanted : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
        const auto it = std::ranges::find_if(formats, [&](const VkSurfaceFormatKHR& f) {
            return f.format == wanted && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        if (it != formats.end()) {
            chosen = *it;
            break;
        }
    }
    if (chosen.format == VK_FORMAT_UNDEFINED) chosen.format = VK_FORMAT_B8G8R8A8_UNORM;

    std::uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount) imageCount = std::min(imageCount, caps.maxImageCount);
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (const auto candidate : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                 VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR}) {
        if (caps.supportedCompositeAlpha & candidate) {
            alpha = candidate;
            break;
        }
    }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        return std::unexpected(Error{ErrorCode::Unsupported, "surface does not support colour attachments"});
    }

    VkSwapchainCreateInfoKHR info = vkStruct<VkSwapchainCreateInfoKHR>(VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);
    info.surface = surface_;
    info.minImageCount = imageCount;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = alpha;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR; // Always available; frames are on demand anyway.
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain_;
    VkSwapchainKHR replacement = VK_NULL_HANDLE;
    APEX_VK_TRY(vkCreateSwapchainKHR(device, &info, nullptr, &replacement), "vkCreateSwapchainKHR");
    if (swapchain_) retired_.push_back({swapchain_, std::move(views_), std::move(renderDone_)});
    views_.clear();
    renderDone_.clear();
    images_.clear();
    firstPresented_ = UINT32_MAX;
    swapchain_ = replacement;
    format_ = chosen.format;
    extent_ = extent;

    APEX_VK_TRY(vkGetSwapchainImagesKHR(device, swapchain_, &count, nullptr), "swapchain images");
    images_.resize(count);
    APEX_VK_TRY(vkGetSwapchainImagesKHR(device, swapchain_, &count, images_.data()), "swapchain images");
    for (VkImage image : images_) {
        VkImageViewCreateInfo vi = vkStruct<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        APEX_VK_TRY(vkCreateImageView(device, &vi, nullptr, &view), "swapchain view");
        views_.push_back(view);
        VkSemaphoreCreateInfo si = vkStruct<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
        VkSemaphore done = VK_NULL_HANDLE;
        APEX_VK_TRY(vkCreateSemaphore(device, &si, nullptr, &done), "swapchain semaphore");
        renderDone_.push_back(done);
    }
    needsRecreate_ = false;
    ++recreates_;
    return true;
}

Result<FrameStatus> Swapchain::renderFrame(VkExtent2D pixelSize, const RecordFn& record, Clock::time_point now) {
    if (!frameDue(now)) return FrameStatus::Skipped;
    if (resizeDragging_) lastDragFrame_ = now;
    VkDevice device = context_->device();
    APEX_VK_TRY(vkWaitForFences(device, 1, &inFlight_, VK_TRUE, UINT64_MAX), "wait for frame");
    if (retirementReady_) {
        destroyRetired();
        retirementReady_ = false;
    }
    if (!needsRecreate_) {
        VkSurfaceCapabilitiesKHR caps{};
        APEX_VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(context_->physicalDevice(), surface_, &caps),
                    "surface capabilities");
        const VkExtent2D wanted = surfaceExtent(caps, pixelSize);
        needsRecreate_ = wanted.width != extent_.width || wanted.height != extent_.height;
    }
    if (needsRecreate_) {
        auto sized = recreate(pixelSize);
        if (!sized) return std::unexpected(sized.error());
        if (!*sized) return FrameStatus::Skipped;
    }

    std::uint32_t index = 0;
    VkResult acquired = vkAcquireNextImageKHR(device, swapchain_, UINT64_MAX, imageAvailable_, VK_NULL_HANDLE, &index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        needsRecreate_ = true;
        return FrameStatus::Skipped; // Retry at the next interval; never recreate twice in one step.
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        return std::unexpected(vkError(acquired, "vkAcquireNextImageKHR"));
    }
    if (index == firstPresented_) retirementReady_ = true;
    vkResetFences(device, 1, &inFlight_);

    vkResetCommandBuffer(commands_, 0);
    VkCommandBufferBeginInfo bi = vkStruct<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    APEX_VK_TRY(vkBeginCommandBuffer(commands_, &bi), "begin frame");
    transitionImage(commands_, images_[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    record(commands_, SwapchainFrame{images_[index], views_[index], format_, extent_, index});
    transitionImage(commands_, images_[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    APEX_VK_TRY(vkEndCommandBuffer(commands_), "end frame");

    VkSemaphoreSubmitInfo wait = vkStruct<VkSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO);
    wait.semaphore = imageAvailable_;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal = vkStruct<VkSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO);
    signal.semaphore = renderDone_[index];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cb = vkStruct<VkCommandBufferSubmitInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO);
    cb.commandBuffer = commands_;
    VkSubmitInfo2 submit = vkStruct<VkSubmitInfo2>(VK_STRUCTURE_TYPE_SUBMIT_INFO_2);
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cb;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    APEX_VK_TRY(vkQueueSubmit2(context_->graphicsQueue(), 1, &submit, inFlight_), "submit frame");

    VkPresentInfoKHR pi = vkStruct<VkPresentInfoKHR>(VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderDone_[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    const VkResult presented = vkQueuePresentKHR(context_->graphicsQueue(), &pi);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR || acquired == VK_SUBOPTIMAL_KHR) {
        needsRecreate_ = true; // The frame itself was drawn; the next one gets a fresh swapchain.
    } else if (presented != VK_SUCCESS) {
        return std::unexpected(vkError(presented, "vkQueuePresentKHR"));
    }
    if (firstPresented_ == UINT32_MAX) firstPresented_ = index;
    return FrameStatus::Presented;
}

} // namespace apex::render
