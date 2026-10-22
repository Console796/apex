#pragma once

// Pipelines and draw recording (plan 0.1, D13/0.1.9). Dynamic rendering only; one shared pipeline
// layout (set 0 = bindless TextureHeap, 128-byte push constants); reversed-Z (clear 0, compare
// GREATER_OR_EQUAL); culling off. Positions are camera-relative: the caller's view-projection is
// rotation-only, and each draw carries a double-precision world origin that the recorder rebases
// against the camera (view_math.hpp).
//
// Usage per frame, inside an already begun dynamic-rendering pass (RenderTarget::begin or
// SwapchainDepth::begin):
//     DrawList list; list.view = ...; list.meshes.push_back(...); renderer.record(commands, list);

#include "core/math.hpp"
#include "core/result.hpp"
#include "render/resources.hpp"
#include "render/texture_heap.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace apex::render {

enum class DisplayMode { Textured, Flat, Wireframe };

// Brush vertex, 36 bytes: camera-relative (draw-origin-relative) position, normal, uv, material.
struct BrushVertex {
    float position[3];
    float normal[3];
    float uv[2];
    std::uint32_t material; // Texture slot, used when MeshDraw::texture == kVertexMaterial.
};
static_assert(sizeof(BrushVertex) == 36);

// Per-instance data of an entity box, 40 bytes; corners relative to the draw origin.
struct BoxInstance {
    float mins[3];
    float maxs[3];
    float colour[4];
};
static_assert(sizeof(BoxInstance) == 40);

// Per-instance data of a sprite, 48 bytes.
struct SpriteInstance {
    float position[3]; // Relative to the draw origin.
    float halfSize;
    float colour[4];   // Multiplies the texture; alpha < 0.5 after multiplication is discarded.
    std::uint32_t texture;
    std::uint32_t pad[3]{};
};
static_assert(sizeof(SpriteInstance) == 48);

inline constexpr std::uint32_t kVertexMaterial = 0xFFFFFFFFu;

struct MeshDraw {
    const Buffer* vertices = nullptr;  // BrushVertex, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT.
    const Buffer* indices = nullptr;   // uint32 triangle list, INDEX_BUFFER_BIT.
    std::uint32_t indexCount = 0;
    const Buffer* edgeIndices = nullptr; // Optional uint32 line list into `vertices`.
    std::uint32_t edgeIndexCount = 0;
    DVec3 origin{0.0};                 // World position the vertices are relative to.
    TextureSlot texture = kCheckerboardSlot; // Or kVertexMaterial.
    Vec4 colour{1.0f};                 // Tint (Textured), fill (Flat), line colour (Wireframe).
    Vec4 highlight{0.0f};              // RGB tint blended by alpha, after material shading.
    std::uint32_t firstIndex = 0;
    std::uint32_t firstEdgeIndex = 0;
    bool outlineOnlyWire = false;      // Selected wire edges are emitted by the outline pass.
};

struct BoxDraw {
    const Buffer* instances = nullptr; // BoxInstance, VERTEX_BUFFER_BIT.
    std::uint32_t count = 0;
    DVec3 origin{0.0};
    bool solid = false;                // Shaded faces instead of wire edges.
    std::uint32_t firstInstance = 0;
    Vec4 tint{0.0f};                   // RGB tint blended by alpha; outline draws use RGBA directly.
    bool outlineOnlyWire = false;
};

struct SpriteDraw {
    const Buffer* instances = nullptr; // SpriteInstance, VERTEX_BUFFER_BIT.
    std::uint32_t count = 0;
    DVec3 origin{0.0};
    std::uint32_t firstInstance = 0;
    Vec4 tint{0.0f};
};

struct GridSettings {
    bool enabled = false;
    double planeZ = 0.0;        // World height of the grid plane.
    float spacing = 64.0f;      // Minor line spacing; a power of two, at most 1024.
    float fadeDistance = 4096.0f;
    Vec4 colour{0.6f, 0.6f, 0.65f, 0.8f}; // rgb line colour, a peak opacity.
};

struct FrameView {
    Mat4 viewProjection{1.0f};  // rotationOnlyView * projection (see view_math.hpp).
    DVec3 cameraPosition{0.0};
    Vec3 cameraRight{1.0f, 0.0f, 0.0f}; // World-space basis, for billboards.
    Vec3 cameraUp{0.0f, 0.0f, 1.0f};
};

struct DrawList {
    DisplayMode mode = DisplayMode::Textured;
    FrameView view;
    std::vector<MeshDraw> meshes;
    std::vector<BoxDraw> boxes;
    std::vector<SpriteDraw> sprites;
    // Recorded last: dim depth-disabled lines, then full-strength visible lines. No depth writes.
    std::vector<MeshDraw> outlines;
    std::vector<BoxDraw> boxOutlines;
    std::vector<SpriteDraw> spriteOutlines;
    float occludedAlpha = 0.25f;
    GridSettings grid;
    bool edgeOverlay = false;           // Textured/Flat: draw edgeIndices lines on top.
    Vec4 edgeColour{0.0f, 0.0f, 0.0f, 1.0f};
    float edgeDepthBias = 2.0f;         // Constant depth-bias factor pulling lines toward the camera.
};

// Push constants shared by all pipelines (mirrors shaders/common.inc).
struct PushConstants {
    Mat4 viewProjection;
    Vec4 colour;
    Vec4 offsetSlot; // xyz offset, w = texture slot bit-cast
    Vec4 p0;
    Vec4 p1;
};
static_assert(sizeof(PushConstants) == 128);

class Renderer {
  public:
    // Pipelines are built for these attachment formats; depthFormat is RenderTarget::kDepthFormat
    // for offscreen targets and SwapchainDepth::kFormat for swapchains.
    static Result<std::unique_ptr<Renderer>> create(Context& context, TextureHeap& heap,
                                                    VkFormat colorFormat, VkFormat depthFormat);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Records the list into the current rendering pass; viewport and scissor are expected to be
    // set already (RenderTarget::begin does).
    void record(VkCommandBuffer commands, const DrawList& list);

    // Debug builds: recompiles any shader under APEX_SHADER_DIR whose source changed (glslc on
    // PATH) and rebuilds the pipelines. Returns true if pipelines were rebuilt; a compile error
    // is logged and keeps the old shaders. Always false in release builds.
    // Call between frames (it waits for the device to go idle).
    Result<bool> reloadChangedShaders();

    bool wireframeFill() const { return wireFill_; } // fillModeNonSolid pipelines available.

  private:
    struct Impl;
    explicit Renderer(std::unique_ptr<Impl> impl, bool wireFill);
    std::unique_ptr<Impl> impl_;
    bool wireFill_ = false;
};

// Reversed-Z depth buffer matching a swapchain; begin() also starts dynamic rendering.
class SwapchainDepth {
  public:
    static constexpr VkFormat kFormat = VK_FORMAT_D32_SFLOAT;
    explicit SwapchainDepth(Context& context) : context_(&context) {}

    // Clears colour to `clearColor` and depth to 0, sets viewport and scissor. The colour image
    // must be in COLOR_ATTACHMENT_OPTIMAL (Swapchain::renderFrame guarantees it). Recreates the
    // depth image when the extent changes (waiting for the device idle first).
    Result<void> begin(VkCommandBuffer commands, const struct SwapchainFrame& frame, const float clearColor[4]);
    void end(VkCommandBuffer commands);

  private:
    Context* context_;
    Image depth_;
    bool depthReady_ = false;
};

// Creates a device-local buffer holding `data` (staged through the ring and flushed).
Result<Buffer> uploadBuffer(Context& context, UploadRing& ring, std::span<const std::byte> data,
                            VkBufferUsageFlags usage);

} // namespace apex::render
