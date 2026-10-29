#pragma once

#include "render/texture_heap.hpp"

namespace apex::render {

// Viewport-sized HDR scene and reversed-Z depth, both sampled by the display pass. The heap and
// context must outlive the target. Resize requires completion of every prior use: the UI calls it
// inside Swapchain::renderFrame after its one-frame-in-flight fence; tests use oneShot/waitIdle.
class SceneTarget {
  public:
    static constexpr VkFormat kColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

    SceneTarget(Context& context, TextureHeap& heap) : context_(&context), heap_(&heap) {}
    ~SceneTarget(); // Waits idle before releasing registrations and images.
    SceneTarget(const SceneTarget&) = delete;
    SceneTarget& operator=(const SceneTarget&) = delete;

    Result<void> resize(std::uint32_t width, std::uint32_t height);
    VkExtent2D extent() const { return color_.extent(); }
    const Image& color() const { return color_; }
    const Image& depth() const { return depth_; }
    TextureSlot colorSlot() const { return colorSlot_; }
    TextureSlot depthSlot() const { return depthSlot_; }

    // Outside any active rendering pass. Clears colour and depth (0), sets viewport/scissor.
    void begin(VkCommandBuffer commands, const float clearColor[4]);
    // Ends rendering and transitions both images to SHADER_READ_ONLY_OPTIMAL.
    void end(VkCommandBuffer commands);

  private:
    Context* context_;
    TextureHeap* heap_;
    Image color_, depth_;
    TextureSlot colorSlot_ = kCheckerboardSlot;
    TextureSlot depthSlot_ = kCheckerboardSlot;
};

} // namespace apex::render
