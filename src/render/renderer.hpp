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
#include "render/glyph_atlas.hpp"
#include "render/texture_heap.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
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

// Per-instance data of an entity box, 88 bytes; local corners and draw-relative origin.
struct BoxInstance {
    float mins[3];
    float maxs[3];
    float colour[4];
    float origin[3] = {};
    float basis[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
};
static_assert(sizeof(BoxInstance) == 88);

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

// How the textured pass treats the texture's alpha channel. Opaque ignores it. Test discards
// below 0.5 in the opaque pass ($alphatest). Blend draws in the sorted translucent pass with no
// depth writes ($translucent); other display modes treat Blend draws as Opaque.
enum class AlphaMode : std::uint32_t { Opaque = 0, Test = 1, Blend = 2 };

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
    std::uint32_t previewIndex = 0;     // 0 = identity; otherwise DrawList::previews[index - 1].
    AlphaMode alpha = AlphaMode::Opaque; // Textured mode only.
    std::uint32_t instanceCount = 1;
    bool instancedTransforms = false; // Consecutive DrawList::previews, starting at previewIndex.
};

struct BoxDraw {
    const Buffer* instances = nullptr; // BoxInstance, VERTEX_BUFFER_BIT.
    std::uint32_t count = 0;
    DVec3 origin{0.0};
    bool solid = false;                // Shaded faces instead of wire edges.
    std::uint32_t firstInstance = 0;
    Vec4 tint{0.0f};                   // RGB tint blended by alpha; outline draws use RGBA directly.
    bool outlineOnlyWire = false;
    std::uint32_t previewIndex = 0;
};

struct SpriteDraw {
    const Buffer* instances = nullptr; // SpriteInstance, VERTEX_BUFFER_BIT.
    std::uint32_t count = 0;
    DVec3 origin{0.0};
    std::uint32_t firstInstance = 0;
    Vec4 tint{0.0f};
    std::uint32_t previewIndex = 0;
};

struct GridRect {
    double uMin, uMax, vMin, vMax; // Plane coordinates relative to GridSettings::origin.
};

struct GridSettings {
    bool enabled = false;
    DVec3 origin{0.0};
    DVec3 normal{0.0, 0.0, 1.0}; // Unit normal and orthonormal in-plane axes.
    DVec3 uAxis{1.0, 0.0, 0.0};
    DVec3 vAxis{0.0, 1.0, 0.0};
    std::optional<GridRect> bounds; // Unset: infinite camera-centred sheet.
    float margin = 16.0f;       // Expand bounds in plane space before drawing.
    float spacing = 64.0f;      // Minor line spacing; a power of two from 0.125 to 1024.
    float fadeDistance = 4096.0f;
    Vec4 colour{0.6f, 0.6f, 0.65f, 0.8f}; // rgb line colour, a peak opacity.
};

struct FrameView {
    Mat4 viewProjection{1.0f};  // rotationOnlyView * projection (see view_math.hpp).
    DVec3 cameraPosition{0.0};
    Vec3 cameraRight{1.0f, 0.0f, 0.0f}; // World-space basis, for billboards.
    Vec3 cameraUp{0.0f, 0.0f, 1.0f};
};

struct DrawPreview {
    DMat4 matrix{1.0};
    DVec3 origin{0.0}; // Same draw-local origin as the draws referring to this entry.
};

// Screen-space overlay primitives (editor gizmos): drawn after everything, no depth test, alpha
// blended. Positions are world space; segments are expanded to `widthPixels` quads in the renderer.
struct OverlaySegment {
    DVec3 a{0.0};
    DVec3 b{0.0};
    Vec4 colour{1.0f};
    float widthPixels = 2.0f;
};
struct OverlayTriangle {
    DVec3 a{0.0};
    DVec3 b{0.0};
    DVec3 c{0.0};
    Vec4 colour{1.0f};
};

struct OverlayPoint {
    DVec3 position{0.0};
    Vec4 colour{1.0f};
    float sizePixels = 7.0f;
};
struct Label {
    DVec3 position{0.0};
    std::string text; // UTF-8; ASCII/Latin-1 supported, other code points use '?'.
    Vec2 offset{0.0f}; // Baseline offset in pixels, y down.
    Vec4 colour{1.0f};
};

struct DrawList {
    DisplayMode mode = DisplayMode::Textured;
    FrameView view;
    std::vector<MeshDraw> meshes;
    std::vector<MeshDraw> ghosts; // Blended material pass, no depth writes.
    std::vector<BoxDraw> boxes;
    std::vector<SpriteDraw> sprites;
    // Recorded last: dim depth-disabled lines, then full-strength visible lines. No depth writes.
    std::vector<MeshDraw> outlines;
    std::vector<BoxDraw> boxOutlines;
    std::vector<SpriteDraw> spriteOutlines;
    std::vector<DrawPreview> previews;
    float occludedAlpha = 0.25f;
    GridSettings grid;
    bool edgeOverlay = false;           // Textured/Flat: draw edgeIndices lines on top.
    Vec4 edgeColour{0.0f, 0.0f, 0.0f, 1.0f};
    float edgeDepthBias = 2.0f;         // Constant depth-bias factor pulling lines toward the camera.
    // Empty by default, which leaves every other frame untouched.
    std::vector<OverlaySegment> overlaySegments;
    std::vector<OverlayTriangle> overlayTriangles;
    std::vector<OverlaySegment> worldSegments;
    std::vector<OverlayPoint> worldPoints;
    std::vector<OverlayTriangle> worldTriangles;
    std::vector<Label> labels;
    float labelScale = 1.0f; // Physical pixels per atlas pixel.
    Vec2 viewportSize{0.0f};            // Pixels; required when overlay primitives are present.
};

// Push constants shared by all pipelines (mirrors shaders/common.inc).
// Preview ABI, without growing the block:
//   brush/box: p1.xy = address low/high uint bits, p1.z = index uint bits;
//   instanced models: p1.z high bit selects consecutive transforms by gl_InstanceIndex.
//   sprite: offsetSlot.w / p1.w = address low/high, p0.w = (index << 1) | outline.
// Sprite outline used to occupy both p0.w and p1.w; packing it frees the third word.
// Grid retains every word of its existing layout. Index 0 skips all buffer reads.
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
    // set already (RenderTarget::begin does). Each command buffer is a reusable frame slot:
    // record once per recording, and finish its previous submission before reusing it (as
    // Swapchain does). Preview buffers are retained per slot, including across preview clears.
    void record(VkCommandBuffer commands, const DrawList& list);
    // The atlas may be shared by renderers with different attachment formats.
    void setGlyphAtlas(std::shared_ptr<const GlyphAtlas> atlas);

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
    // depth image when the extent changes. The caller must have completed the previous frame
    // using this depth image (Swapchain::renderFrame waits for its per-frame fence).
    Result<void> begin(VkCommandBuffer commands, const struct SwapchainFrame& frame, const float clearColor[4]);
    void end(VkCommandBuffer commands);

  private:
    Context* context_;
    Image depth_;
    bool depthReady_ = false;
};

// Creates a device-local buffer holding `data` (staged through the ring and flushed). With
// flush false the copy stays recorded in the ring; the caller flushes before the buffer is used.
Result<Buffer> uploadBuffer(Context& context, UploadRing& ring, std::span<const std::byte> data,
                            VkBufferUsageFlags usage, bool flush = true);

} // namespace apex::render
