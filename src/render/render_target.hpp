#pragma once

// Offscreen colour + reversed-Z depth target with CPU readback, for golden-image tests and
// screenshots (plan 0.1, 0.1.9 / D20).

#include "core/result.hpp"
#include "render/resources.hpp"

#include <cstdint>
#include <vector>

namespace apex::render {

struct Rgba8Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels; // width * height * 4, row-major, top row first.
};

class RenderTarget {
  public:
    static constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

    // RGBA8 UNORM or SRGB; readback returns the stored display bytes in either case.
    static Result<RenderTarget> create(Context& context, std::uint32_t width, std::uint32_t height,
                                        VkFormat colorFormat = kColorFormat);

    RenderTarget() = default;
    RenderTarget(RenderTarget&&) noexcept = default;
    RenderTarget& operator=(RenderTarget&&) noexcept = default;

    std::uint32_t width() const { return color_.desc().width; }
    std::uint32_t height() const { return color_.desc().height; }
    VkExtent2D extent() const { return color_.extent(); }
    const Image& color() const { return color_; }
    const Image& depth() const { return depth_; }

    // Transitions both attachments, clears (colour to `clearColor`, depth to 0 for reversed-Z),
    // sets viewport and scissor to the full target and calls vkCmdBeginRendering. Pipelines should
    // use color().desc().format / kDepthFormat and depth compare GREATER_OR_EQUAL.
    void begin(VkCommandBuffer commands, const float clearColor[4]);
    // vkCmdEndRendering, then leaves colour in TRANSFER_SRC_OPTIMAL (ready for readback).
    void end(VkCommandBuffer commands);

    // Copies the colour image to the CPU (blocking). Call after the commands that rendered into
    // the target have completed (or after Context::waitIdle()).
    Result<Rgba8Image> readback();

  private:
    Context* context_ = nullptr;
    Image color_;
    Image depth_;
    VkImageLayout colorLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
};

} // namespace apex::render
