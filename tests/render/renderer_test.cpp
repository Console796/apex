#include "render/context.hpp"
#include "shared_context.hpp"
#include "render/render_target.hpp"
#include "render/renderer.hpp"
#include "render/texture_heap.hpp"
#include "render/view_math.hpp"
#include "golden_image.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

using namespace apex;
using namespace apex::render;

namespace {

constexpr std::uint32_t kSize = 64;
const float kClear[4] = {0.1f, 0.1f, 0.1f, 1.0f};

struct Fixture {
    Context* context = nullptr; // Owned by testing::sharedContext().
    std::unique_ptr<TextureHeap> heap;
    std::unique_ptr<Renderer> renderer;
    RenderTarget target;
    TextureSlot checker = 0;
};

// Shared across test cases; null (with a message) when no usable device exists.
Fixture* fixture() {
    static std::unique_ptr<Fixture> shared = [] {
        auto f = std::make_unique<Fixture>();
        f->context = testing::sharedContext(); // Before anything GPU-owned, so it outlives this Fixture.
        if (!f->context) return std::unique_ptr<Fixture>();
        auto heap = TextureHeap::create(*f->context, {64, 1u << 20});
        REQUIRE(heap.has_value());
        f->heap = std::move(*heap);
        auto renderer = Renderer::create(*f->context, *f->heap, RenderTarget::kColorFormat, RenderTarget::kDepthFormat);
        if (!renderer) MESSAGE(renderer.error().what());
        REQUIRE(renderer.has_value());
        f->renderer = std::move(*renderer);
        auto target = RenderTarget::create(*f->context, kSize, kSize);
        REQUIRE(target.has_value());
        f->target = std::move(*target);

        // 16x16 texture of 8x8 cells: red, blue / green, white (top row first).
        std::vector<std::byte> pixels(16 * 16 * 4);
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                const bool right = x >= 8, bottom = y >= 8;
                const std::uint8_t rgb[3] = {static_cast<std::uint8_t>(!bottom && !right ? 255 : bottom && right ? 255 : 0),
                                             static_cast<std::uint8_t>(bottom ? 255 : 0),
                                             static_cast<std::uint8_t>(!bottom && right ? 255 : bottom && right ? 255 : 0)};
                std::byte* p = &pixels[(static_cast<std::size_t>(y) * 16 + x) * 4];
                for (int c = 0; c < 3; ++c) p[c] = std::byte{rgb[c]};
                p[3] = std::byte{255};
            }
        }
        auto slot = f->heap->upload({16, 16, VK_FORMAT_R8G8B8A8_UNORM}, pixels);
        REQUIRE(slot.has_value());
        f->checker = *slot;
        REQUIRE(f->heap->flush().has_value());
        return f;
    }();
    return shared.get();
}

#define REQUIRE_GPU()                                                                              \
    Fixture* gpu = fixture();                                                                      \
    if (!gpu) {                                                                                    \
        MESSAGE("Skipped: no Vulkan 1.3 device");                                                  \
        return;                                                                                    \
    }                                                                                              \
    gpu->context->validation().clear()

struct Cube {
    Buffer vertices, indices, edges;
};

// Axis-aligned cube of half-extent h around the origin; uv (0,0) is the top-left of each face as
// seen from outside for the -X face (the one the test camera sees).
Cube makeCube(Fixture& f, float h, std::uint32_t material) {
    struct Face {
        float n[3];
        float c[4][3];
    };
    const Face faces[6] = {
        {{-1, 0, 0}, {{-h, h, h}, {-h, -h, h}, {-h, -h, -h}, {-h, h, -h}}},
        {{1, 0, 0}, {{h, -h, h}, {h, h, h}, {h, h, -h}, {h, -h, -h}}},
        {{0, -1, 0}, {{-h, -h, h}, {h, -h, h}, {h, -h, -h}, {-h, -h, -h}}},
        {{0, 1, 0}, {{h, h, h}, {-h, h, h}, {-h, h, -h}, {h, h, -h}}},
        {{0, 0, -1}, {{-h, h, -h}, {-h, -h, -h}, {h, -h, -h}, {h, h, -h}}},
        {{0, 0, 1}, {{-h, h, h}, {h, h, h}, {h, -h, h}, {-h, -h, h}}},
    };
    const float uvs[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    std::vector<BrushVertex> vertices;
    std::vector<std::uint32_t> indices, edges;
    for (const Face& face : faces) {
        const auto base = static_cast<std::uint32_t>(vertices.size());
        for (int i = 0; i < 4; ++i) {
            BrushVertex v{};
            std::memcpy(v.position, face.c[i], sizeof v.position);
            std::memcpy(v.normal, face.n, sizeof v.normal);
            std::memcpy(v.uv, uvs[i], sizeof v.uv);
            v.material = material;
            vertices.push_back(v);
            edges.push_back(base + static_cast<std::uint32_t>(i));
            edges.push_back(base + static_cast<std::uint32_t>((i + 1) % 4));
        }
        for (std::uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) indices.push_back(base + i);
    }
    auto bytes = [](const auto& v) { return std::as_bytes(std::span{v}); };
    auto& ring = f.heap->uploadRing();
    auto vb = uploadBuffer(*f.context, ring, bytes(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    auto ib = uploadBuffer(*f.context, ring, bytes(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    auto eb = uploadBuffer(*f.context, ring, bytes(edges), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    REQUIRE(vb.has_value());
    REQUIRE(ib.has_value());
    REQUIRE(eb.has_value());
    return {std::move(*vb), std::move(*ib), std::move(*eb)};
}

Cube makeGridFace(Fixture& f, const GridSettings& grid) {
    const auto& b = *grid.bounds;
    const DVec2 corners[] = {{b.uMin, b.vMin}, {b.uMax, b.vMin}, {b.uMax, b.vMax}, {b.uMin, b.vMax}};
    std::array<BrushVertex, 4> vertices{};
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const Vec3 p(grid.uAxis * corners[i].x + grid.vAxis * corners[i].y), n(grid.normal);
        std::memcpy(vertices[i].position, &p.x, sizeof vertices[i].position);
        std::memcpy(vertices[i].normal, &n.x, sizeof vertices[i].normal);
    }
    const std::array<std::uint32_t, 6> indices{0, 1, 2, 0, 2, 3};
    auto& ring = f.heap->uploadRing();
    auto vb = uploadBuffer(*f.context, ring, std::as_bytes(std::span{vertices}), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    auto ib = uploadBuffer(*f.context, ring, std::as_bytes(std::span{indices}), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    REQUIRE(vb);
    REQUIRE(ib);
    return {std::move(*vb), std::move(*ib), {}};
}

FrameView makeView(DVec3 position, DVec3 anglesDeg) {
    const AngleVectors basis = angleVectors(anglesDeg);
    FrameView view;
    view.viewProjection = reversedZPerspective(glm::radians(90.0), 1.0, 1.0) * rotationOnlyView(basis);
    view.cameraPosition = position;
    view.cameraRight = Vec3(basis.right);
    view.cameraUp = Vec3(basis.up);
    return view;
}

Rgba8Image drawFrame(Fixture& f, const DrawList& list, RenderTarget* target = nullptr) {
    RenderTarget& output = target ? *target : f.target;
    auto done = f.context->oneShot([&](VkCommandBuffer commands) {
        output.begin(commands, kClear);
        f.renderer->record(commands, list);
        output.end(commands);
    });
    REQUIRE(done.has_value());
    auto image = output.readback();
    REQUIRE(image.has_value());
    CHECK(f.context->validation().errorCount() == 0);
    for (const auto& e : f.context->validation().errors()) MESSAGE(e);
    return std::move(*image);
}

struct Px {
    int r, g, b;
};
Px at(const Rgba8Image& image, std::uint32_t x, std::uint32_t y) {
    const std::uint8_t* p = &image.pixels[(static_cast<std::size_t>(y) * image.width + x) * 4];
    return {p[0], p[1], p[2]};
}
bool isBackground(Px p) {
    return p.r >= 24 && p.r <= 28 && p.g >= 24 && p.g <= 28 && p.b >= 24 && p.b <= 28;
}
bool nearColour(Px p, Px c, int tolerance) {
    return std::abs(p.r - c.r) <= tolerance && std::abs(p.g - c.g) <= tolerance && std::abs(p.b - c.b) <= tolerance;
}

DrawList cubeList(const Cube& cube, DisplayMode mode, TextureSlot slot) {
    DrawList list;
    list.mode = mode;
    list.view = makeView({-128.0, 0.0, 0.0}, {0.0, 0.0, 0.0});
    MeshDraw draw;
    draw.vertices = &cube.vertices;
    draw.indices = &cube.indices;
    draw.indexCount = 36;
    draw.edgeIndices = &cube.edges;
    draw.edgeIndexCount = 48;
    draw.texture = slot;
    draw.colour = mode == DisplayMode::Textured ? Vec4(1.0f) : Vec4(0.2f, 0.8f, 0.2f, 1.0f);
    list.meshes.push_back(draw);
    return list;
}

} // namespace

TEST_CASE("renderer: textured cube shows the bindless texture") {
    REQUIRE_GPU();
    const Cube cube = makeCube(*gpu, 32.0f, 0);
    const auto image = drawFrame(*gpu, cubeList(cube, DisplayMode::Textured, gpu->checker));
    CHECK(isBackground(at(image, 2, 2)));
    const Px topLeft = at(image, 26, 26), topRight = at(image, 38, 26);
    const Px bottomLeft = at(image, 26, 38), bottomRight = at(image, 38, 38);
    CHECK(topLeft.r > 150);
    CHECK(topLeft.b < 30);
    CHECK(topRight.b > 150);
    CHECK(topRight.r < 30);
    CHECK(bottomLeft.g > 150);
    CHECK(bottomLeft.r < 30);
    CHECK(bottomRight.r > 150);
    CHECK(bottomRight.g > 150);
    CHECK(bottomRight.b > 150);
}

TEST_CASE("renderer: per-vertex material index selects the texture") {
    REQUIRE_GPU();
    const Cube cube = makeCube(*gpu, 32.0f, gpu->checker);
    const auto image = drawFrame(*gpu, cubeList(cube, DisplayMode::Textured, kVertexMaterial));
    CHECK(at(image, 26, 26).r > 150);
    CHECK(at(image, 38, 26).b > 150);
}

TEST_CASE("renderer: flat cube with edge overlay") {
    REQUIRE_GPU();
    const Cube cube = makeCube(*gpu, 32.0f, 0);
    DrawList list = cubeList(cube, DisplayMode::Flat, 0);
    auto image = drawFrame(*gpu, list);
    CHECK(isBackground(at(image, 2, 2)));
    const Px face = at(image, 32, 32);
    CHECK(nearColour(face, {35, 140, 35}, 12));

    // Front face spans x in [21.3, 42.7]; the white edge overlay crosses row 32 near x = 21.
    list.edgeOverlay = true;
    list.edgeColour = Vec4(1.0f);
    image = drawFrame(*gpu, list);
    bool found = false;
    for (std::uint32_t x = 19; x <= 23; ++x) found = found || nearColour(at(image, x, 32), {255, 255, 255}, 8);
    CHECK(found);
    CHECK(nearColour(at(image, 32, 32), {35, 140, 35}, 12));
}

TEST_CASE("renderer: wireframe cube is hollow with edge lines") {
    REQUIRE_GPU();
    const Cube cube = makeCube(*gpu, 32.0f, 0);
    DrawList list = cubeList(cube, DisplayMode::Wireframe, 0);
    list.meshes[0].colour = Vec4(1.0f, 1.0f, 0.0f, 1.0f);
    auto image = drawFrame(*gpu, list);
    CHECK(isBackground(at(image, 32, 32)));
    bool found = false;
    for (std::uint32_t x = 19; x <= 23; ++x) found = found || nearColour(at(image, x, 32), {255, 255, 0}, 8);
    CHECK(found);

    // Without an edge buffer the polygon-mode fallback still draws something.
    list.meshes[0].edgeIndices = nullptr;
    list.meshes[0].edgeIndexCount = 0;
    image = drawFrame(*gpu, list);
    if (gpu->renderer->wireframeFill()) {
        bool any = false;
        for (std::uint32_t y = 0; y < kSize && !any; ++y)
            for (std::uint32_t x = 0; x < kSize; ++x) any = any || !isBackground(at(image, x, y));
        CHECK(any);
    }
}

TEST_CASE("renderer: entity boxes, sprites and grid") {
    REQUIRE_GPU();
    auto& ring = gpu->heap->uploadRing();
    const BoxInstance box{{-16, -16, -16}, {16, 16, 16}, {0.0f, 1.0f, 0.0f, 1.0f}};
    auto boxes = uploadBuffer(*gpu->context, ring, std::as_bytes(std::span{&box, 1}), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    REQUIRE(boxes.has_value());
    const SpriteInstance sprite{{0, 0, 0}, 16.0f, {1, 1, 1, 1}, gpu->checker, {}};
    auto sprites = uploadBuffer(*gpu->context, ring, std::as_bytes(std::span{&sprite, 1}), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    REQUIRE(sprites.has_value());

    SUBCASE("solid box") {
        DrawList list;
        list.view = makeView({-128.0, 0.0, 0.0}, {0.0, 0.0, 0.0});
        list.boxes.push_back({&*boxes, 1, {0.0, 0.0, 0.0}, true});
        const auto image = drawFrame(*gpu, list);
        CHECK(isBackground(at(image, 2, 2)));
        const Px centre = at(image, 32, 32);
        CHECK(centre.g > 120);
        CHECK(centre.r < 40);
    }
    SUBCASE("wire box") {
        DrawList list;
        list.view = makeView({-128.0, 0.0, 0.0}, {0.0, 0.0, 0.0});
        list.boxes.push_back({&*boxes, 1, {0.0, 0.0, 0.0}, false});
        const auto image = drawFrame(*gpu, list);
        CHECK(isBackground(at(image, 32, 32)));
        bool found = false;
        for (std::uint32_t y = 0; y < kSize; ++y)
            for (std::uint32_t x = 0; x < kSize; ++x) found = found || nearColour(at(image, x, y), {0, 255, 0}, 8);
        CHECK(found);
    }
    SUBCASE("sprite faces the camera") {
        DrawList list;
        list.view = makeView({-128.0, 0.0, 0.0}, {0.0, 20.0, 0.0}); // Yawed: billboard still fills the centre.
        list.sprites.push_back({&*sprites, 1, {-128.0 + 100.0 * std::cos(glm::radians(20.0)), 100.0 * std::sin(glm::radians(20.0)), 0.0}});
        const auto image = drawFrame(*gpu, list);
        CHECK(isBackground(at(image, 2, 2)));
        CHECK(!isBackground(at(image, 32, 32)));
    }
    SUBCASE("grid") {
        DrawList list;
        list.view = makeView({0.0, 0.0, 128.0}, {90.0, 0.0, 0.0}); // Looking straight down.
        list.grid.enabled = true;
        list.grid.spacing = 64.0f;
        list.grid.colour = Vec4(1.0f);
        const auto image = drawFrame(*gpu, list);
        int bright = 0;
        for (std::uint32_t y = 0; y < kSize; ++y)
            for (std::uint32_t x = 0; x < kSize; ++x) bright += at(image, x, y).r > 100 ? 1 : 0;
        CHECK(bright > 20);
        CHECK(bright < static_cast<int>(kSize * kSize) / 2);
        CHECK(isBackground(at(image, 4, 4)) == (at(image, 4, 4).r < 40));
    }
}

TEST_CASE("renderer: hot reload is a no-op when nothing changed") {
    REQUIRE_GPU();
    auto reloaded = gpu->renderer->reloadChangedShaders();
    REQUIRE(reloaded.has_value());
    CHECK(!*reloaded);
}

TEST_CASE("renderer: grid default ground golden") {
    REQUIRE_GPU();
    auto target = RenderTarget::create(*gpu->context, 256, 256);
    REQUIRE(target);
    DrawList list;
    list.view = makeView({0, 0, 128}, {90, 0, 0});
    list.grid.enabled = true;
    golden::check("grid_ground", drawFrame(*gpu, list, &*target), *gpu->context);
}

TEST_CASE("renderer: grid bounded wall and slope goldens with surface depth bias") {
    REQUIRE_GPU();
    auto target = RenderTarget::create(*gpu->context, 256, 256);
    REQUIRE(target);
    for (const bool slope : {false, true}) {
        CAPTURE(slope);
        DrawList list;
        list.mode = DisplayMode::Flat;
        list.grid.enabled = true;
        list.grid.normal = slope ? DVec3(0.6, 0, 0.8) : DVec3(1, 0, 0);
        list.grid.uAxis = slope ? DVec3(0.8, 0, -0.6) : DVec3(0, 1, 0);
        list.grid.vAxis = slope ? DVec3(0, 1, 0) : DVec3(0, 0, 1);
        list.grid.bounds = GridRect{-96, 96, -64, 64};
        list.grid.spacing = 16;
        list.grid.colour = Vec4(0.85f, 0.85f, 0.95f, 0.9f);
        // Projection is outside the rect; the nearest-point fade must keep this face visible.
        list.grid.fadeDistance = 256;
        const DVec3 camera = slope ? DVec3(230, -210, 230) : DVec3(220, -180, 130);
        list.view = makeView(camera, anglesFromForward(-camera, {0, 0, 1}));
        const Cube face = makeGridFace(*gpu, list.grid);
        MeshDraw mesh;
        mesh.vertices = &face.vertices;
        mesh.indices = &face.indices;
        mesh.indexCount = 6;
        mesh.colour = Vec4(0.18f, 0.24f, 0.32f, 1);
        list.meshes.push_back(mesh);
        const auto image = drawFrame(*gpu, list, &*target);
        golden::check(slope ? "grid_slope_bounded" : "grid_wall_bounded", image, *gpu->context);
        CHECK(drawFrame(*gpu, list, &*target).pixels == image.pixels);
        // At nearby camera angles, the coplanar grid must still cover the face without holes.
        for (const double yaw : {-0.1, 0.0, 0.1}) {
            CAPTURE(yaw);
            auto angles = anglesFromForward(-camera, {0, 0, 1});
            angles.y += yaw;
            list.view = makeView(camera, angles);
            list.grid.enabled = false;
            const auto bare = drawFrame(*gpu, list, &*target);
            list.grid.enabled = true;
            const auto overlaid = drawFrame(*gpu, list, &*target);
            DrawList onlyGrid = list;
            onlyGrid.meshes.clear();
            const auto unobstructed = drawFrame(*gpu, onlyGrid, &*target);
            int linesOnFace = 0;
            for (std::uint32_t y = 0; y < 256; ++y) {
                for (std::uint32_t x = 0; x < 256; ++x) {
                    linesOnFace += !isBackground(at(bare, x, y)) &&
                                   !nearColour(at(bare, x, y), at(overlaid, x, y), 3) ? 1 : 0;
                    if (isBackground(at(bare, x, y))) continue;
                    // The same lines must blend over the face as over the clear colour. Any
                    // lost depth tests leave holes, including at slightly different angles.
                    const double alpha = (at(unobstructed, x, y).r - 26.0) / (0.85 * 255.0 - 26.0);
                    const double expected = alpha * 0.85 * 255.0 + (1.0 - alpha) * at(bare, x, y).r;
                    CHECK(std::abs(at(overlaid, x, y).r - expected) < 3.0);
                }
            }
            CHECK(linesOnFace > 200);
        }
    }
}

TEST_CASE("renderer: grid spacing extremes goldens and subpixel suppression") {
    REQUIRE_GPU();
    auto target = RenderTarget::create(*gpu->context, 256, 256);
    REQUIRE(target);
    DrawList list;
    list.grid.enabled = true;
    list.grid.spacing = 0.125f;
    list.view = makeView({0.03125, -24.0625, 24}, {45, 90, 0});
    golden::check("grid_spacing_0125", drawFrame(*gpu, list, &*target), *gpu->context);

    // Two-pixel minors are gone while sixteen-pixel majors remain.
    const Mat4 down = rotationOnlyView({0, 0, -1}, {1, 0, 0}, {0, 1, 0});
    list.view.viewProjection = reversedZOrtho(8, 8, 1, 512) * down;
    list.view.cameraPosition = {0, 0, 128};
    const auto filtered = drawFrame(*gpu, list, &*target);
    CHECK(at(filtered, 127, 120).r > 50);
    CHECK(isBackground(at(filtered, 129, 120)));

    // At this zoom even the major pitch is one pixel: both sets must disappear completely.
    list.view.viewProjection = reversedZOrtho(128, 128, 1, 512) * rotationOnlyView(angleVectors({90, 0, 0}));
    list.view.cameraPosition = {0.03125, -0.0625, 128};
    const auto distant = drawFrame(*gpu, list, &*target);
    list.grid.enabled = false;
    CHECK(distant.pixels == drawFrame(*gpu, list, &*target).pixels);

    list.grid.enabled = true;
    list.grid.spacing = 1024;
    list.grid.fadeDistance = 32768;
    list.view = makeView({1536, -512, 8192}, {90, 0, 0});
    list.view.viewProjection = reversedZOrtho(8192, 8192, 1, 32768) * down;
    const auto coarse = drawFrame(*gpu, list, &*target);
    golden::check("grid_spacing_1024", coarse, *gpu->context);
    // The camera has crossed a 1024 tile: major lines must still be at u=0 and u=8192.
    CHECK(at(coarse, 104, 140).r > at(coarse, 120, 140).r + 20);
    CHECK(at(coarse, 232, 140).r > at(coarse, 120, 140).r + 20);
}

TEST_CASE("renderer: grid microspacing goldens match near and far plane origins") {
    REQUIRE_GPU();
    auto target = RenderTarget::create(*gpu->context, 256, 256);
    REQUIRE(target);
    DrawList list;
    list.grid.enabled = true;
    list.grid.spacing = 0.125f;
    const DVec3 localCamera{0.03125, -1.0625, 1.125};
    list.view = makeView(localCamera, {45, 90, 0});
    list.view.viewProjection = reversedZPerspective(glm::radians(90.0), 1, 0.01) *
                               rotationOnlyView(angleVectors({45, 90, 0}));
    const auto near = drawFrame(*gpu, list, &*target);
    golden::check("grid_near_origin", near, *gpu->context);
    for (const DVec3 shift : {DVec3(60000, 0, 0), DVec3(65536), DVec3(-65536)}) {
        CAPTURE(shift.x);
        list.grid.origin = shift;
        list.view.cameraPosition = localCamera + shift;
        const auto far = drawFrame(*gpu, list, &*target);
        CHECK(far.pixels == near.pixels);
        if (shift.x == 60000) golden::check("grid_far_origin", far, *gpu->context);
        // Also move along the same plane: wrapping the camera phase must preserve the
        // microgrid after crossing many minor and major cells, in either direction.
        list.grid.origin = {0, 0, shift.z};
        CHECK(drawFrame(*gpu, list, &*target).pixels == near.pixels);
    }
}

TEST_CASE("renderer: grid axis planes in ortho clip at expanded bounds and test depth") {
    REQUIRE_GPU();
    auto target = RenderTarget::create(*gpu->context, 256, 256);
    REQUIRE(target);
    std::vector<std::uint8_t> reference;
    for (const DVec3 normal : {DVec3(1, 0, 0), DVec3(-1, 0, 0), DVec3(0, 1, 0), DVec3(0, -1, 0)}) {
        DrawList list;
        list.mode = DisplayMode::Flat;
        list.grid.enabled = true;
        list.grid.normal = normal;
        list.grid.uAxis = normal.x != 0 ? DVec3(0, 1, 0) : DVec3(1, 0, 0);
        list.grid.vAxis = glm::cross(normal, list.grid.uAxis);
        list.grid.spacing = 16;
        list.grid.bounds = GridRect{-48, 48, -32, 32};
        list.view.cameraPosition = normal * 128.0;
        list.view.viewProjection = reversedZOrtho(128, 128, 1, 512) *
                                   rotationOnlyView(-normal, list.grid.uAxis, list.grid.vAxis);
        const auto image = drawFrame(*gpu, list, &*target);
        if (reference.empty()) {
            reference = image.pixels;
            golden::check("grid_wall_ortho", image, *gpu->context);
        }
        CHECK(image.pixels == reference);
        int inside = 0, margin = 0, outside = 0;
        for (std::uint32_t y = 0; y < 256; ++y) {
            for (std::uint32_t x = 0; x < 256; ++x) {
                if (isBackground(at(image, x, y))) continue;
                if (x < 64 || x >= 192 || y < 80 || y >= 176) ++outside;
                else if (x < 80 || x >= 176 || y < 96 || y >= 160) ++margin;
                else ++inside;
            }
        }
        CHECK(outside == 0);
        CHECK(margin > 100);
        CHECK(inside > 100);
        // A solid cube in front of the grid occludes its centre, including the major axes.
        const Cube cube = makeCube(*gpu, 16, 0);
        MeshDraw mesh;
        mesh.vertices = &cube.vertices;
        mesh.indices = &cube.indices;
        mesh.indexCount = 36;
        mesh.origin = normal * 32.0;
        mesh.colour = Vec4(0.2f, 0.5f, 0.2f, 1);
        list.meshes.push_back(mesh);
        const auto occluded = drawFrame(*gpu, list, &*target);
        list.grid.enabled = false;
        const auto bare = drawFrame(*gpu, list, &*target);
        for (std::uint32_t y = 114; y < 142; ++y)
            for (std::uint32_t x = 114; x < 142; ++x)
                CHECK(nearColour(at(occluded, x, y), at(bare, x, y), 0));
    }
}

TEST_CASE("renderer: outline is dim behind a surface and full strength in front") {
    REQUIRE_GPU();
    const Cube cube = makeCube(*gpu, 32.0f, 0);
    for (const auto mode : {DisplayMode::Textured, DisplayMode::Flat, DisplayMode::Wireframe}) {
        DrawList list = cubeList(cube, mode, gpu->checker);
        MeshDraw rear = list.meshes[0];
        rear.origin.x = 96;
        rear.colour = Vec4(1, 1, 1, 1);
        rear.outlineOnlyWire = true;
        list.meshes.push_back(rear);
        list.outlines.push_back(rear);
        list.occludedAlpha = 0.0f;
        const auto baseline = drawFrame(*gpu, list);
        list.occludedAlpha = 0.25f;
        const auto dim = drawFrame(*gpu, list);
        list.outlines[0].origin.x = -48; // In front of the wall, outside its silhouette.
        const auto visible = drawFrame(*gpu, list);
        int dimPixels = 0, brightPixels = 0;
        for (std::uint32_t y = 0; y < kSize; ++y) {
            for (std::uint32_t x = 0; x < kSize; ++x) {
                const Px p = at(dim, x, y), b = at(baseline, x, y);
                if (!nearColour(p, b, 2)) {
                    ++dimPixels;
                    CHECK_FALSE(nearColour(p, {255, 255, 255}, 2));
                }
                brightPixels += nearColour(at(visible, x, y), {255, 255, 255}, 2) ? 1 : 0;
            }
        }
        CHECK(dimPixels > 20);
        CHECK(brightPixels > 20);
        list.occludedAlpha = 0.0f;
        const auto noDim = drawFrame(*gpu, list);
        for (std::uint32_t y = 0; y < kSize; ++y)
            for (std::uint32_t x = 0; x < kSize; ++x)
                if (nearColour(at(visible, x, y), {255, 255, 255}, 2))
                    CHECK(nearColour(at(noDim, x, y), {255, 255, 255}, 2));
    }
}

TEST_CASE("renderer: box and sprite tints address a single existing instance") {
    REQUIRE_GPU();
    const BoxInstance boxes[] = {
        {{-16, 200, -16}, {16, 232, 16}, {1, 0, 0, 1}},
        {{-16, -16, -16}, {16, 16, 16}, {1, 0, 0, 1}}
    };
    const SpriteInstance sprites[] = {
        {{0, 216, 0}, 16, {1, 1, 1, 1}, gpu->checker, {}},
        {{0, 0, 0}, 16, {1, 1, 1, 1}, gpu->checker, {}}
    };
    auto& ring = gpu->heap->uploadRing();
    auto boxBuffer = uploadBuffer(*gpu->context, ring, std::as_bytes(std::span{boxes}), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    auto spriteBuffer = uploadBuffer(*gpu->context, ring, std::as_bytes(std::span{sprites}), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    REQUIRE(boxBuffer);
    REQUIRE(spriteBuffer);
    for (const bool sprite : {false, true}) {
        CAPTURE(sprite);
        DrawList list;
        list.view = makeView({-128, 0, 0}, {0, 0, 0});
        if (sprite) {
            list.sprites.push_back({&*spriteBuffer, 1, DVec3(0), 1, Vec4(0, 1, 1, 1)});
        } else {
            list.boxes.push_back({&*boxBuffer, 1, DVec3(0), true, 1, Vec4(0, 1, 1, 1)});
        }
        CHECK(nearColour(at(drawFrame(*gpu, list), 32, 32), {0, 255, 255}, 2));
        // Outline uses the instance's geometry with an independent RGBA colour.
        if (sprite) {
            list.spriteOutlines = list.sprites;
            list.spriteOutlines[0].tint = Vec4(1, 0, 1, 1);
        } else {
            list.boxOutlines = list.boxes;
            list.boxOutlines[0].tint = Vec4(1, 0, 1, 1);
        }
        const auto image = drawFrame(*gpu, list);
        int magenta = 0;
        for (std::uint32_t y = 0; y < kSize; ++y)
            for (std::uint32_t x = 0; x < kSize; ++x)
                magenta += nearColour(at(image, x, y), {255, 0, 255}, 2) ? 1 : 0;
        CHECK(magenta > 20);
    }
}

TEST_CASE("renderer: face draw ranges start within existing triangle and edge buffers") {
    REQUIRE_GPU();
    const Cube cube = makeCube(*gpu, 32, 0);
    DrawList list = cubeList(cube, DisplayMode::Flat, 0);
    list.view = makeView({128, 0, 0}, {0, 180, 0}); // The second face (+X).
    auto& face = list.meshes[0];
    face.firstIndex = 6;
    face.indexCount = 6;
    face.firstEdgeIndex = 8;
    face.edgeIndexCount = 8;
    CHECK(nearColour(at(drawFrame(*gpu, list), 32, 32), {35, 140, 35}, 12));
    list.outlines.push_back(face);
    list.outlines[0].colour = Vec4(1, 0, 1, 1);
    const auto image = drawFrame(*gpu, list);
    bool edge = false;
    for (std::uint32_t x = 19; x <= 23; ++x)
        edge = edge || nearColour(at(image, x, 32), {255, 0, 255}, 2);
    CHECK(edge);
}
