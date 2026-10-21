#include "render/render_target.hpp"

#include <cstring>

namespace apex::render {

Result<RenderTarget> RenderTarget::create(Context& context, std::uint32_t width, std::uint32_t height) {
    RenderTarget target;
    target.context_ = &context;
    auto color = Image::create(context, {kColorFormat, width, height, 1, 1,
                                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                             VK_IMAGE_USAGE_SAMPLED_BIT});
    if (!color) return std::unexpected(color.error().withContext("render target colour"));
    auto depth = Image::create(context, {kDepthFormat, width, height, 1, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT});
    if (!depth) return std::unexpected(depth.error().withContext("render target depth"));
    target.color_ = std::move(*color);
    target.depth_ = std::move(*depth);
    return target;
}

void RenderTarget::begin(VkCommandBuffer commands, const float clearColor[4]) {
    transitionImage(commands, color_.handle(), VK_IMAGE_ASPECT_COLOR_BIT, colorLayout_,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transitionImage(commands, depth_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    colorLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkRenderingAttachmentInfo colorAttachment = vkStruct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    colorAttachment.imageView = color_.view();
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    std::memcpy(colorAttachment.clearValue.color.float32, clearColor, sizeof(float) * 4);
    VkRenderingAttachmentInfo depthAttachment = vkStruct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    depthAttachment.imageView = depth_.view();
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.clearValue.depthStencil = {0.0f, 0};

    VkRenderingInfo ri = vkStruct<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
    ri.renderArea = {{0, 0}, extent()};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &colorAttachment;
    ri.pDepthAttachment = &depthAttachment;
    vkCmdBeginRendering(commands, &ri);
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height()), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, extent()};
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &scissor);
}

void RenderTarget::end(VkCommandBuffer commands) {
    vkCmdEndRendering(commands);
    transitionImage(commands, color_.handle(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    colorLayout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}

Result<Rgba8Image> RenderTarget::readback() {
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width()) * height() * 4;
    auto buffer = Buffer::create(*context_, {bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::Readback});
    if (!buffer) return std::unexpected(buffer.error().withContext("readback buffer"));
    const VkImageLayout from = colorLayout_;
    auto done = context_->oneShot([&](VkCommandBuffer commands) {
        if (from != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            transitionImage(commands, color_.handle(), VK_IMAGE_ASPECT_COLOR_BIT, from,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        }
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width(), height(), 1};
        vkCmdCopyImageToBuffer(commands, color_.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer->handle(), 1, &region);
    });
    if (!done) return std::unexpected(done.error());
    colorLayout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    buffer->invalidate();
    Rgba8Image image;
    image.width = width();
    image.height = height();
    image.pixels.resize(bytes);
    std::memcpy(image.pixels.data(), buffer->mapped(), bytes);
    return image;
}

} // namespace apex::render
