#include "render/scene_target.hpp"

#include <cassert>
#include <cstring>

namespace apex::render {

SceneTarget::~SceneTarget() {
    if (color_ || depth_) context_->waitIdle();
    heap_->releaseAttachment(colorSlot_);
    heap_->releaseAttachment(depthSlot_);
}

Result<void> SceneTarget::resize(std::uint32_t width, std::uint32_t height) {
    if (color_ && color_.desc().width == width && color_.desc().height == height) return {};
    auto color = Image::create(*context_, {kColorFormat, width, height, 1, 1,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
    if (!color) return std::unexpected(color.error().withContext("scene colour"));
    auto depth = Image::create(*context_, {kDepthFormat, width, height, 1, 1,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
    if (!depth) return std::unexpected(depth.error().withContext("scene depth"));
    // The caller has completed the previous frame, so the old slots can be reused at once: a heap
    // with room for one target can always resize it. On failure the target is left empty.
    heap_->releaseAttachment(colorSlot_);
    heap_->releaseAttachment(depthSlot_);
    colorSlot_ = depthSlot_ = kCheckerboardSlot;
    color_ = Image();
    depth_ = Image();
    auto colorSlot = heap_->registerAttachment(*color, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!colorSlot) return std::unexpected(colorSlot.error());
    auto depthSlot = heap_->registerAttachment(*depth, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (!depthSlot) {
        heap_->releaseAttachment(*colorSlot);
        return std::unexpected(depthSlot.error());
    }
    color_ = std::move(*color);
    depth_ = std::move(*depth);
    colorSlot_ = *colorSlot;
    depthSlot_ = *depthSlot;
    return {};
}

void SceneTarget::begin(VkCommandBuffer commands, const float clearColor[4]) {
    assert(color_ && depth_);
    // Both are cleared every frame, so the old contents (and layout) never matter. This also keeps a
    // recording that was discarded unsubmitted from leaving a wrong layout behind.
    transitionImage(commands, color_.handle(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transitionImage(commands, depth_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color = vkStruct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    color.imageView = color_.view();
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    std::memcpy(color.clearValue.color.float32, clearColor, sizeof(float) * 4);
    VkRenderingAttachmentInfo depth = vkStruct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    depth.imageView = depth_.view();
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {0.0f, 0};
    VkRenderingInfo info = vkStruct<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
    info.renderArea = {{0, 0}, extent()};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &color;
    info.pDepthAttachment = &depth;
    vkCmdBeginRendering(commands, &info);
    const VkViewport viewport{0, 0, static_cast<float>(extent().width), static_cast<float>(extent().height), 0, 1};
    const VkRect2D scissor{{0, 0}, extent()};
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &scissor);
}

void SceneTarget::end(VkCommandBuffer commands) {
    vkCmdEndRendering(commands);
    transitionImage(commands, color_.handle(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transitionImage(commands, depth_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

} // namespace apex::render
