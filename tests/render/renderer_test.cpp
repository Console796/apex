#include "render/context.hpp"
#include "shared_context.hpp"
#include "render/render_target.hpp"
#include "render/scene_target.hpp"
#include "render/renderer.hpp"
#include "render/texture_heap.hpp"
#include "render/view_math.hpp"
#include "golden_image.hpp"
#include "editor/transform.hpp"

#include <doctest/doctest.h>

#include <algorithm>
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
Cube makeCube(Fixture& f, float h, std::uint32_t material, const DMat4& transform = DMat4(1.0), bool interior = false) {
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
            if (transform != DMat4(1.0)) {
                const Vec3 p(transform * DVec4(face.c[i][0], face.c[i][1], face.c[i][2], 1));
                const Vec3 n(glm::normalize(glm::transpose(glm::inverse(DMat3(transform))) *
                                           DVec3(face.n[0], face.n[1], face.n[2])));
                std::memcpy(v.position, &p.x, sizeof v.position);
                std::memcpy(v.normal, &n.x, sizeof v.normal);
            }
            if (interior) for (float& n : v.normal) n = -n;
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

TEST_CASE("renderer preview entity boxes sprites and outlines match baked positions") {
    REQUIRE_GPU();
    auto& ring = gpu->heap->uploadRing();
    const BoxInstance box{{-16, -8, -12}, {16, 8, 12}, {0, 1, 0, 1}};
    const SpriteInstance sprite{{0, 0, 0}, 16, {1, 1, 1, 1}, gpu->checker, {}};
    const auto upload = [&](const auto& value) {
        auto result = uploadBuffer(*gpu->context, ring, std::as_bytes(std::span{&value, 1}), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        REQUIRE(result);
        return std::move(*result);
    };
    const auto boxes = upload(box);
    const auto sprites = upload(sprite);
    const DVec3 origin{65536, -65536, 32768};
    const auto move = editor::Transform::translate({0, 24, 8});
    for (const bool isSprite : {false, true}) {
        for (const bool wire : {false, true}) {
            CAPTURE(isSprite);
            CAPTURE(wire);
            DrawList list;
            list.mode = wire ? DisplayMode::Wireframe : DisplayMode::Flat;
            list.view = makeView(origin + DVec3(-128, 0, 0), {0, 0, 0});
            list.previews.push_back({move.matrix(), origin});
            if (isSprite) {
                list.sprites.push_back({&sprites, 1, origin, 0, Vec4(0), 1});
                list.spriteOutlines = list.sprites;
                list.spriteOutlines[0].tint = Vec4(1, 0, 1, 1);
            } else {
                list.boxes.push_back({&boxes, 1, origin, !wire, 0, Vec4(0), wire, 1});
                list.boxOutlines = list.boxes;
                list.boxOutlines[0].tint = Vec4(1, 0, 1, 1);
            }
            const auto preview = drawFrame(*gpu, list);
            list.previews.clear();
            if (isSprite) {
                list.sprites[0].previewIndex = list.spriteOutlines[0].previewIndex = 0;
                list.sprites[0].origin = list.spriteOutlines[0].origin = move.point(origin);
            } else {
                list.boxes[0].previewIndex = list.boxOutlines[0].previewIndex = 0;
                list.boxes[0].origin = list.boxOutlines[0].origin = move.point(origin);
            }
            CHECK(drawFrame(*gpu, list).pixels == preview.pixels);
            bool visible = false;
            for (std::uint32_t y = 0; y < kSize; ++y)
                for (std::uint32_t x = 0; x < kSize; ++x) visible = visible || !isBackground(at(preview, x, y));
            CHECK(visible);
        }
    }
    // Box shading must use the inverse transpose too. Bake an axis permutation with unequal
    // lengths and a reflection: its faces and normals match an axis-aligned reference box.
    const auto transform = editor::Transform::rotate({0, 0, 1}, 90, origin) *
                           editor::Transform::scale({-2, 0.5, 1.25}, DMat3(1), origin);
    const BoxInstance baked{{-4, -32, -15}, {4, 32, 15}, {0, 1, 0, 1}};
    const auto bakedBoxes = upload(baked);
    DrawList list;
    list.view = makeView(origin + DVec3(-128, 0, 0), {0, 0, 0});
    list.boxes.push_back({&boxes, 1, origin, true, 0, Vec4(0), false, 1});
    list.previews.push_back({transform.matrix(), origin});
    const auto preview = drawFrame(*gpu, list);
    list.previews.clear();
    list.boxes[0].instances = &bakedBoxes;
    list.boxes[0].previewIndex = 0;
    CHECK(drawFrame(*gpu, list).pixels == preview.pixels);

    // Scaling a billboard in its camera plane and mirroring its depth axis preserves the
    // billboard's facing/UV convention, while exercising the sprite's packed BDA/index words.
    const auto scale = editor::Transform::scale({-1, 1.5, 1.5}, DMat3(1), origin);
    auto largeSprite = sprite;
    largeSprite.halfSize = 24;
    const auto bakedSprite = upload(largeSprite);
    list.boxes.clear();
    list.sprites.push_back({&sprites, 1, origin, 0, Vec4(0), 1});
    list.previews.push_back({scale.matrix(), origin});
    const auto scaledPreview = drawFrame(*gpu, list);
    list.previews.clear();
    list.sprites[0].instances = &bakedSprite;
    list.sprites[0].previewIndex = 0;
    CHECK(drawFrame(*gpu, list).pixels == scaledPreview.pixels);
}

TEST_CASE("renderer preview frame buffers keep separate viewport recordings alive") {
    REQUIRE_GPU();
    const Cube cube = makeCube(*gpu, 16, 0);
    std::array<DrawList, 2> lists;
    std::array<Rgba8Image, 2> expected;
    std::array<RenderTarget, 2> targets;
    for (std::size_t i = 0; i < lists.size(); ++i) {
        lists[i] = cubeList(cube, DisplayMode::Flat, 0);
        lists[i].meshes[0].previewIndex = 1;
        lists[i].previews.push_back({editor::Transform::translate({0, i == 0 ? 24.0 : -24.0, 0}).matrix(), DVec3(0)});
        expected[i] = drawFrame(*gpu, lists[i]);
        auto target = RenderTarget::create(*gpu->context, kSize, kSize);
        REQUIRE(target);
        targets[i] = std::move(*target);
    }
    VkCommandBuffer commands[2]{};
    auto alloc = vkStruct<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    alloc.commandPool = gpu->context->commandPool();
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 2;
    REQUIRE(vkAllocateCommandBuffers(gpu->context->device(), &alloc, commands) == VK_SUCCESS);
    for (std::size_t i = 0; i < lists.size(); ++i) {
        const auto begin = vkStruct<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        REQUIRE(vkBeginCommandBuffer(commands[i], &begin) == VK_SUCCESS);
        targets[i].begin(commands[i], kClear);
        gpu->renderer->record(commands[i], lists[i]);
        targets[i].end(commands[i]);
        REQUIRE(vkEndCommandBuffer(commands[i]) == VK_SUCCESS);
    }
    // Submit only after both recordings, in reverse order; a shared upload buffer would fail.
    for (const std::size_t i : {1u, 0u}) {
        REQUIRE(gpu->context->submitAndWait(commands[i]));
        const auto image = targets[i].readback();
        REQUIRE(image);
        CHECK(image->pixels == expected[i].pixels);
    }
    vkFreeCommandBuffers(gpu->context->device(), gpu->context->commandPool(), 2, commands);
    CHECK(gpu->context->validation().errorCount() == 0);
}

TEST_CASE("renderer preview oblique nonuniform scale and mirror preserve lighting") {
    REQUIRE_GPU();
    const auto frame = editor::Transform::rotate({1, 2, 3}, 37).linear();
    const auto transform = editor::Transform::mirror({1, 2, 3}) *
                           editor::Transform::scale({2, 0.5, 1.25}, frame);
    const Cube original = makeCube(*gpu, 24, 0);
    const Cube baked = makeCube(*gpu, 24, 0, transform.matrix());
    auto list = cubeList(original, DisplayMode::Flat, 0);
    list.meshes[0].previewIndex = 1;
    list.previews.push_back({transform.matrix(), DVec3(0)});
    const auto preview = drawFrame(*gpu, list);
    const auto committed = drawFrame(*gpu, cubeList(baked, DisplayMode::Flat, 0));
    int lit = 0;
    for (std::uint32_t y = 20; y < 44; ++y) {
        for (std::uint32_t x = 20; x < 44; ++x) {
            const auto a = at(preview, x, y), b = at(committed, x, y);
            CHECK(nearColour(a, b, 1));
            if (!isBackground(b)) ++lit;
        }
    }
    CHECK(lit > 100);
}

TEST_CASE("renderer: overlay draws on top of geometry without depth and leaves other frames alone") {
    REQUIRE_GPU();
    const Cube cube = makeCube(*gpu, 32.0f, 0);
    DrawList list = cubeList(cube, DisplayMode::Flat, gpu->checker);
    list.viewportSize = Vec2(kSize);
    const auto baseline = drawFrame(*gpu, list);
    const auto again = drawFrame(*gpu, list);
    CHECK(baseline.pixels == again.pixels); // Empty overlay lists change nothing.

    // A segment buried inside the cube, 6 px wide; a triangle in a corner of the screen.
    OverlaySegment segment;
    segment.a = {20.0, -20.0, 0.0};
    segment.b = {20.0, 20.0, 0.0};
    segment.colour = Vec4(1.0f, 0.0f, 0.0f, 1.0f);
    segment.widthPixels = 6.0f;
    list.overlaySegments.push_back(segment);
    OverlayTriangle triangle;
    triangle.a = {0.0, 60.0, 60.0};
    triangle.b = {0.0, 40.0, 60.0};
    triangle.c = {0.0, 60.0, 40.0};
    triangle.colour = Vec4(0.0f, 1.0f, 0.0f, 1.0f);
    list.overlayTriangles.push_back(triangle);
    // Entirely behind the camera and straddling it: neither may draw nor fault.
    list.overlaySegments.push_back({{-300.0, 0.0, 0.0}, {-200.0, 10.0, 0.0}, Vec4(0, 0, 1, 1), 4.0f});
    list.overlaySegments.push_back({{-300.0, 0.0, 30.0}, {200.0, 0.0, 30.0}, Vec4(1, 1, 0, 1), 2.0f});
    const auto image = drawFrame(*gpu, list);

    int red = 0, green = 0, blue = 0, yellow = 0;
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const Px p = at(image, x, y);
            red += nearColour(p, {255, 0, 0}, 3);
            green += nearColour(p, {0, 255, 0}, 3);
            blue += nearColour(p, {0, 0, 255}, 3);
            yellow += nearColour(p, {255, 255, 0}, 3);
        }
    }
    CHECK(red >= 30);   // About 6 px wide and 8 px tall.
    CHECK(red < 120);
    CHECK(green >= 6);
    CHECK(blue == 0);
    CHECK(yellow >= 8);
    // Pixels outside the overlay are untouched.
    int changed = 0;
    for (std::uint32_t y = 0; y < kSize; ++y)
        for (std::uint32_t x = 0; x < kSize; ++x)
            changed += nearColour(at(image, x, y), at(baseline, x, y), 1) ? 0 : 1;
    CHECK(changed < 600);
}

namespace {

Rgba8Image drawLitFrame(Fixture& f, const DrawList& list, SceneTarget& scene, RenderTarget* target = nullptr) {
    RenderTarget& output = target ? *target : f.target;
    REQUIRE(scene.resize(output.width(), output.height()));
    auto done = f.context->oneShot([&](VkCommandBuffer commands) {
        f.renderer->prepare(commands, list, scene, kClear);
        output.begin(commands, kClear);
        f.renderer->record(commands, list);
        output.end(commands);
    });
    REQUIRE(done);
    auto image = output.readback();
    REQUIRE(image);
    for (const auto& e : f.context->validation().errors()) MESSAGE(e);
    CHECK(f.context->validation().errorCount() == 0);
    return std::move(*image);
}

void checkLitParity(const Rgba8Image& a, const Rgba8Image& b) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    REQUIRE(a.pixels.size() == b.pixels.size());
    int maxDifference = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i)
        maxDifference = std::max(maxDifference, std::abs(int(a.pixels[i]) - int(b.pixels[i])));
    CHECK(maxDifference <= 1);
}

} // namespace

TEST_CASE("renderer: Lit fullbright reproduces albedo without textured fake shading") {
    REQUIRE_GPU();
    SceneTarget scene(*gpu->context, *gpu->heap);
    const Cube cube = makeCube(*gpu, 32.0f, 0);
    DrawList list = cubeList(cube, DisplayMode::Textured, gpu->checker);
    SUBCASE("opaque") {}
    SUBCASE("alpha test") { list.meshes[0].alpha = AlphaMode::Test; }
    SUBCASE("preview and edge overlay") {
        DMat4 transform(1);
        transform[3][1] = 8;
        list.previews.push_back({transform, DVec3(0)});
        list.meshes[0].previewIndex = 1;
        list.edgeOverlay = true;
        list.edgeColour = Vec4(1);
    }
    SUBCASE("selection tint") { list.meshes[0].highlight = Vec4(1, 0.5f, 0, 0.4f); }
    SUBCASE("hover tint on alpha test") {
        list.meshes[0].alpha = AlphaMode::Test;
        list.meshes[0].highlight = Vec4(0, 0.5f, 1, 0.3f);
    }
    // The camera sees only -X. Cancel Textured's constant fake shade on that face to get
    // an albedo reference, including its existing alpha-test, highlight and edge behavior.
    const float shade = 0.65f + 0.35f * std::abs(glm::dot(Vec3(-1, 0, 0), glm::normalize(Vec3(0.3f, 0.5f, 0.8f))));
    list.meshes[0].colour = Vec4(Vec3(1.0f / shade), 1);
    const auto albedo = drawFrame(*gpu, list);
    list.meshes[0].colour = Vec4(1);
    list.mode = DisplayMode::Lit;
    list.fullbright = true;
    checkLitParity(albedo, drawLitFrame(*gpu, list, scene));
}

TEST_CASE("renderer: Lit copies scene depth before boxes and outlines") {
    REQUIRE_GPU();
    SceneTarget scene(*gpu->context, *gpu->heap);
    const Cube cube = makeCube(*gpu, 32.0f, 0);
    DrawList list = cubeList(cube, DisplayMode::Textured, gpu->checker);
    list.mode = DisplayMode::Lit;
    list.fullbright = true;
    const auto bare = drawLitFrame(*gpu, list, scene);
    BoxInstance box{{-16, -16, -16}, {16, 16, 16}, {1, 0, 1, 1}};
    auto instances = uploadBuffer(*gpu->context, gpu->heap->uploadRing(), std::as_bytes(std::span{&box, 1}),
                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    REQUIRE(instances);
    SUBCASE("solid box") { list.boxes.push_back({&*instances, 1, {64, 0, 0}, true}); }
    SUBCASE("outline with hidden lines disabled") {
        list.boxOutlines.push_back({&*instances, 1, {64, 0, 0}});
        list.boxOutlines.back().tint = Vec4(1, 0, 1, 1);
        list.occludedAlpha = 0;
    }
    CHECK(drawLitFrame(*gpu, list, scene).pixels == bare.pixels);
    // The helper occupies visible pixels without the occluder; equality above proves depth copying.
    list.meshes.clear();
    const auto helper = drawLitFrame(*gpu, list, scene);
    list.boxes.clear();
    list.boxOutlines.clear();
    CHECK(helper.pixels != drawLitFrame(*gpu, list, scene).pixels);
}

TEST_CASE("renderer: Lit exposure affects meshes before display-space helpers") {
    REQUIRE_GPU();
    SceneTarget scene(*gpu->context, *gpu->heap);
    const Cube cube = makeCube(*gpu, 32.0f, 0);
    DrawList list = cubeList(cube, DisplayMode::Textured, gpu->checker);
    list.mode = DisplayMode::Lit;
    list.fullbright = true;
    list.viewportSize = Vec2(kSize);
    list.overlayTriangles.push_back({{-96, -8, 8}, {-96, 8, 8}, {-96, 0, -8}, {1, 0, 1, 1}});
    const auto normal = drawLitFrame(*gpu, list, scene);
    list.exposure = 0;
    const auto dark = drawLitFrame(*gpu, list, scene);
    CHECK(nearColour(at(dark, 26, 38), {0, 0, 0}, 0));
    CHECK(nearColour(at(dark, 32, 32), {255, 0, 255}, 0));
    CHECK(nearColour(at(normal, 32, 32), at(dark, 32, 32), 0));
    // The selection tint is display-space: exposure does not reach it.
    list.meshes[0].highlight = Vec4(1, 0, 0, 0.5f);
    const auto selected = drawLitFrame(*gpu, list, scene);
    CHECK(nearColour(at(selected, 26, 38), {128, 0, 0}, 2));
}

TEST_CASE("renderer: scene target resize retires attachment views and reuses heap slots") {
    REQUIRE_GPU();
    auto heap = TextureHeap::create(*gpu->context, {6, 1u << 20});
    REQUIRE(heap);
    auto renderer = Renderer::create(*gpu->context, **heap, RenderTarget::kColorFormat, RenderTarget::kDepthFormat);
    REQUIRE(renderer);
    {
        SceneTarget scene(*gpu->context, **heap);
        DrawList list;
        list.mode = DisplayMode::Lit;
        for (std::uint32_t size : {16u, 32u, 24u, 16u, 32u, 24u}) {
            CAPTURE(size);
            const auto oldImage = scene.color().handle();
            const auto oldSlot = scene.colorSlot();
            gpu->context->waitIdle();
            REQUIRE(scene.resize(size, size));
            CHECK(scene.color().handle() != oldImage);
            CHECK(scene.colorSlot() != oldSlot);
            CHECK(scene.color().desc().format == SceneTarget::kColorFormat);
            CHECK(scene.depth().aspect() == VK_IMAGE_ASPECT_DEPTH_BIT);
            CHECK((*heap)->liveTextures() == 3);
            CHECK_FALSE((*heap)->registerAttachment(scene.depth(), VK_IMAGE_ASPECT_COLOR_BIT));
            auto output = RenderTarget::create(*gpu->context, size, size);
            REQUIRE(output);
            REQUIRE(gpu->context->oneShot([&](VkCommandBuffer commands) {
                (*renderer)->prepare(commands, list, scene, kClear);
                output->begin(commands, kClear);
                (*renderer)->record(commands, list);
                output->end(commands);
            }));
            auto image = output->readback();
            REQUIRE(image);
            CHECK(isBackground(at(*image, size / 2, size / 2)));
            const auto sameImage = scene.color().handle();
            REQUIRE(scene.resize(size, size));
            CHECK(scene.color().handle() == sameImage);
        }

    }
    CHECK((*heap)->liveTextures() == 1);
    for (const auto& e : gpu->context->validation().errors()) MESSAGE(e);
    CHECK(gpu->context->validation().errorCount() == 0);
}

TEST_CASE("renderer: thumbnail reservations cannot borrow live attachment slots") {
    REQUIRE_GPU();
    auto heap = TextureHeap::create(*gpu->context, {66, 1u << 20});
    REQUIRE(heap);
    {
        SceneTarget scene(*gpu->context, **heap);
        REQUIRE(scene.resize(16, 16));
        // 65 usable slots minus the scene's two leaves no 64-slot thumbnail range.
        CHECK_FALSE((*heap)->reserveThumbnailSlots());
    }
    CHECK((*heap)->reserveThumbnailSlots());
    CHECK(gpu->context->validation().errorCount() == 0);
}

TEST_CASE("renderer: scene target resizes in a heap with room for exactly one target") {
    REQUIRE_GPU();
    auto heap = TextureHeap::create(*gpu->context, {3, 1u << 20}); // checkerboard + colour + depth
    REQUIRE(heap);
    SceneTarget scene(*gpu->context, **heap);
    for (std::uint32_t size : {16u, 32u, 24u}) {
        CAPTURE(size);
        gpu->context->waitIdle();
        REQUIRE(scene.resize(size, size));
        CHECK(scene.extent().width == size);
        CHECK((*heap)->liveTextures() == 3);
    }
}

TEST_CASE("renderer: Lit box room evaluates point spot and sun in linear space") {
    REQUIRE_GPU();
    SceneTarget scene(*gpu->context, *gpu->heap);
    const Cube room = makeCube(*gpu, 48, 0, DMat4(1), true);
    const std::array<std::byte, 4> texel{std::byte{128}, std::byte{128}, std::byte{128}, std::byte{255}};
    const auto slot = gpu->heap->upload({1, 1, VK_FORMAT_R8G8B8A8_UNORM}, texel);
    REQUIRE(slot); REQUIRE(gpu->heap->flush());
    auto list = cubeList(room, DisplayMode::Lit, *slot);
    list.meshes[0].colour = Vec4(1);
    list.view = makeView({-24, 0, 0}, {0, 0, 0});
    lighting::Light point;
    point.origin = {-16, -20, 8}; point.intensity = {1, 0, 0}; point.constant = 1; point.quadratic = 0.0005;
    auto spot = point;
    spot.kind = lighting::LightKind::Spot; spot.origin = {-24, 0, 0}; spot.direction = {1, 0, 0};
    spot.intensity = {0, 1, 0}; spot.stopdot = std::cos(glm::radians(20.0)); spot.stopdot2 = std::cos(glm::radians(45.0));
    spot.exponent = 2;
    lighting::Light sun;
    sun.kind = lighting::LightKind::Sun; sun.direction = {0, 0, -1}; sun.intensity = {0, 0, 0.5};
    const auto black = drawLitFrame(*gpu, list, scene);
    for (unsigned y = 0; y < kSize; y += 4)
        for (unsigned x = 0; x < kSize; x += 4) CHECK(nearColour(at(black, x, y), {0, 0, 0}, 0));
    list.lights = {point, spot, sun};
    const auto image = drawLitFrame(*gpu, list, scene);
    golden::check("lit_room", image, *gpu->context);
    list.exposure = 0.25f;
    const auto dim = drawLitFrame(*gpu, list, scene);
    const auto inverse = glm::inverse(DMat4(list.view.viewProjection));
    int checked = 0, spotInside = 0, spotOutside = 0, sunFacing = 0, sunAway = 0;
    int minPoint = 255, maxPoint = 0;
    for (unsigned y = 2; y < kSize; y += 3) {
        for (unsigned x = 2; x < kSize; x += 3) {
            auto ray4 = inverse * DVec4(2.0 * (x + 0.5) / kSize - 1, 2.0 * (y + 0.5) / kSize - 1, 1, 1);
            const DVec3 ray = glm::normalize(DVec3(ray4) / ray4.w);
            double distance = 1e30;
            double secondDistance = 1e30;
            DVec3 normal(0);
            for (int axis = 0; axis < 3; ++axis) {
                if (std::abs(ray[axis]) < 1e-10) continue;
                const double sign = ray[axis] > 0 ? 1 : -1;
                const double t = (sign * 48 - list.view.cameraPosition[axis]) / ray[axis];
                if (t <= 0) continue;
                if (t < distance) {
                    secondDistance = distance;
                    distance = t;
                    normal = DVec3(0);
                    normal[axis] = -sign;
                } else secondDistance = std::min(secondDistance, t);
            }
            // A pixel exactly on a room edge has two valid face normals; raster ownership wins.
            if (secondDistance - distance < 1e-5) continue;
            const auto position = list.view.cameraPosition + ray * distance;
            DVec3 irradiance(0);
            for (const auto& light : list.lights) irradiance += lighting::irradiance(light, position, normal);
            const auto expected = [&](double exposure) {
                const auto encode = [&](double value) {
                    return static_cast<int>(std::lround(255 * std::pow(std::clamp(std::pow(128.0 / 255, 2.2) * value * exposure, 0.0, 1.0), 1 / 2.2)));
                };
                return Px{encode(irradiance.x), encode(irradiance.y), encode(irradiance.z)};
            };
            CAPTURE(x);
            CAPTURE(y);
            CHECK(nearColour(at(image, x, y), expected(1), 1));
            CHECK(nearColour(at(dim, x, y), expected(0.25), 1));
            minPoint = std::min(minPoint, at(image, x, y).r); maxPoint = std::max(maxPoint, at(image, x, y).r);
            spotInside += irradiance.y > 0.1; spotOutside += irradiance.y == 0;
            sunFacing += irradiance.z > 0; sunAway += irradiance.z == 0;
            ++checked;
        }
    }
    CHECK(checked > 400); CHECK(maxPoint - minPoint > 30);
    CHECK(spotInside > 5); CHECK(spotOutside > 5); CHECK(sunFacing > 5); CHECK(sunAway > 5);
    // Rebasing in double preserves the same scene far from the world origin.
    const DVec3 shift(1e10, -2e10, 3e10);
    list.exposure = 1;
    list.meshes[0].origin += shift; list.view.cameraPosition += shift;
    for (auto& light : list.lights) light.origin += shift;
    checkLitParity(image, drawLitFrame(*gpu, list, scene));
    gpu->heap->release(*slot);
}

TEST_CASE("renderer: Lit fullbright translucent albedo blends before gamma") {
    REQUIRE_GPU();
    SceneTarget scene(*gpu->context, *gpu->heap);
    const Cube cube = makeCube(*gpu, 32, 0);
    const std::array<std::byte, 4> texel{std::byte{128}, std::byte{64}, std::byte{192}, std::byte{255}};
    auto slot = gpu->heap->upload({1, 1, VK_FORMAT_R8G8B8A8_UNORM}, texel);
    REQUIRE(slot); REQUIRE(gpu->heap->flush());
    auto list = cubeList(cube, DisplayMode::Lit, *slot);
    list.fullbright = true;
    list.meshes[0].indexCount = 6; // Only the front face, so exactly one alpha blend.
    list.meshes[0].colour = Vec4(0.5f, 0.75f, 0.25f, 0.6f);
    list.meshes[0].alpha = AlphaMode::Blend;
    for (float exposure : {1.0f, 0.25f}) {
        list.exposure = exposure;
        const auto image = drawLitFrame(*gpu, list, scene);
        const auto encode = [&](double colour) {
            const double linear = 0.6 * std::pow(colour, 2.2) + 0.4 * std::pow(0.1, 2.2);
            return static_cast<int>(std::lround(255 * std::pow(linear * exposure, 1 / 2.2)));
        };
        CHECK(nearColour(at(image, 26, 38), {encode(128.0 / 255 * 0.5), encode(64.0 / 255 * 0.75),
                                           encode(192.0 / 255 * 0.25)}, 1));
    }
    gpu->heap->release(*slot);
}

TEST_CASE("renderer: Lit helpers ghosts and overlays stay display-space") {
    REQUIRE_GPU();
    SceneTarget scene(*gpu->context, *gpu->heap);
    const Cube room = makeCube(*gpu, 48, 0, DMat4(1), true);
    const Cube ghost = makeCube(*gpu, 4, 0);
    auto list = cubeList(room, DisplayMode::Lit, gpu->checker);
    list.view = makeView({-24, 0, 0}, {0, 0, 0});
    const auto bare = drawLitFrame(*gpu, list, scene); // Black room covers the whole display.
    MeshDraw ghostDraw;
    ghostDraw.vertices = &ghost.vertices; ghostDraw.indices = &ghost.indices; ghostDraw.indexCount = 36;
    ghostDraw.texture = gpu->checker; ghostDraw.colour = Vec4(1, 1, 1, 0.5f);
    ghostDraw.origin = {-8, 0, 0}; list.ghosts.push_back(ghostDraw);
    BoxInstance box{{-2, -2, -2}, {2, 2, 2}, {1, 0, 1, 1}};
    auto boxes = uploadBuffer(*gpu->context, gpu->heap->uploadRing(), std::as_bytes(std::span{&box, 1}), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    REQUIRE(boxes);
    list.boxes.push_back({&*boxes, 1, {-8, -10, 6}, true});
    SpriteInstance sprite{{-8, 10, 6}, 2, {1, 1, 1, 1}, gpu->checker};
    auto sprites = uploadBuffer(*gpu->context, gpu->heap->uploadRing(), std::as_bytes(std::span{&sprite, 1}), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    REQUIRE(sprites);
    list.sprites.push_back({&*sprites, 1});
    list.viewportSize = Vec2(kSize);
    list.overlaySegments.push_back({{-8, -5, -6}, {-8, 5, -6}, {0, 1, 1, 1}, 2});
    const auto normal = drawLitFrame(*gpu, list, scene);
    list.exposure = 8;
    const auto bright = drawLitFrame(*gpu, list, scene);
    CHECK(normal.pixels == bright.pixels);
    int visible = 0;
    for (std::size_t i = 0; i < normal.pixels.size(); i += 4)
        visible += normal.pixels[i] != bare.pixels[i] || normal.pixels[i + 1] != bare.pixels[i + 1] ||
                   normal.pixels[i + 2] != bare.pixels[i + 2];
    CHECK(visible > 100);
}

TEST_CASE("renderer: Lit frame slots keep lights and previews independent") {
    REQUIRE_GPU();
    const Cube cube = makeCube(*gpu, 16, 0);
    std::array<DrawList, 2> lists;
    std::array<Rgba8Image, 2> expected;
    std::array<RenderTarget, 2> targets;
    std::array<std::unique_ptr<SceneTarget>, 2> scenes;
    for (std::size_t i = 0; i < lists.size(); ++i) {
        scenes[i] = std::make_unique<SceneTarget>(*gpu->context, *gpu->heap);
        auto target = RenderTarget::create(*gpu->context, kSize, kSize);
        REQUIRE(target); targets[i] = std::move(*target);
        lists[i] = cubeList(cube, DisplayMode::Lit, gpu->checker);
        lists[i].meshes[0].colour = Vec4(1);
        lists[i].meshes[0].previewIndex = 1;
        DMat4 matrix(1); matrix[3][1] = i == 0 ? 12 : -12;
        lists[i].previews.push_back({matrix, DVec3(0)});
        lighting::Light sun;
        sun.kind = lighting::LightKind::Sun; sun.direction = {1, 0, 0};
        sun.intensity = i == 0 ? DVec3(0.5, 0, 0) : DVec3(0, 0, 0.25);
        lists[i].lights.push_back(sun);
        expected[i] = drawLitFrame(*gpu, lists[i], *scenes[i]);
    }
    VkCommandBuffer commands[2]{};
    auto alloc = vkStruct<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    alloc.commandPool = gpu->context->commandPool(); alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; alloc.commandBufferCount = 2;
    REQUIRE(vkAllocateCommandBuffers(gpu->context->device(), &alloc, commands) == VK_SUCCESS);
    for (std::size_t i = 0; i < lists.size(); ++i) {
        const auto begin = vkStruct<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        REQUIRE(vkBeginCommandBuffer(commands[i], &begin) == VK_SUCCESS);
        gpu->renderer->prepare(commands[i], lists[i], *scenes[i], kClear);
        targets[i].begin(commands[i], kClear);
        gpu->renderer->record(commands[i], lists[i]);
        targets[i].end(commands[i]);
        REQUIRE(vkEndCommandBuffer(commands[i]) == VK_SUCCESS);
    }
    for (const std::size_t i : {1u, 0u}) {
        REQUIRE(gpu->context->submitAndWait(commands[i]));
        auto image = targets[i].readback(); REQUIRE(image);
        CHECK(image->pixels == expected[i].pixels);
    }
    vkFreeCommandBuffers(gpu->context->device(), gpu->context->commandPool(), 2, commands);
    CHECK(gpu->context->validation().errorCount() == 0);
}

TEST_CASE("renderer: Lit fullbright matches UNORM and SRGB attachment encoding") {
    REQUIRE_GPU();
    Fixture srgb;
    srgb.context = gpu->context;
    auto renderer = Renderer::create(*gpu->context, *gpu->heap, VK_FORMAT_R8G8B8A8_SRGB, RenderTarget::kDepthFormat);
    REQUIRE(renderer);
    srgb.renderer = std::move(*renderer);
    auto target = RenderTarget::create(*gpu->context, kSize, kSize, VK_FORMAT_R8G8B8A8_SRGB);
    REQUIRE(target);
    srgb.target = std::move(*target);
    SceneTarget scene(*gpu->context, *gpu->heap);
    const Cube cube = makeCube(*gpu, 32, 0);
    auto list = cubeList(cube, DisplayMode::Lit, gpu->checker);
    list.fullbright = true;
    for (const Vec3 colour : {Vec3(0.01f, 0.08f, 0.2f), Vec3(0.25f, 0.5f, 0.75f), Vec3(1, 0.9f, 0.8f)}) {
        list.meshes[0].colour = Vec4(colour, 1);
        for (float exposure : {0.25f, 1.0f, 2.0f}) {
            CAPTURE(colour.x);
            CAPTURE(exposure);
            list.exposure = exposure;
            const auto unormImage = drawLitFrame(*gpu, list, scene);
            const auto srgbImage = drawLitFrame(srgb, list, scene);
            // Both store the same placeholder display curve; hardware SRGB conversion may
            // round to the neighbouring UNORM8 code compared with the shader's UNORM output.
            checkLitParity(unormImage, srgbImage);
        }
    }
}

TEST_CASE("renderer: Lit transformed preview matches point and spot CPU irradiance") {
    REQUIRE_GPU();
    SceneTarget scene(*gpu->context, *gpu->heap);
    GridSettings face;
    face.normal = glm::normalize(DVec3(-1, 0.2, 0.3));
    face.uAxis = glm::normalize(glm::cross(DVec3(0, 0, 1), face.normal));
    face.vAxis = glm::cross(face.normal, face.uAxis);
    face.bounds = GridRect{-40, 40, -40, 40};
    const Cube quad = makeGridFace(*gpu, face);
    const DVec3 origin(10000, 20000, -30000);
    const auto preview = (editor::Transform::translate({8, -5, 7}) *
                          editor::Transform::rotate({1, 2, 3}, 32, origin) *
                          editor::Transform::scale({1.2, 0.7, 1.6}, DMat3(1), origin)).matrix();
    const DVec3 centre(preview * DVec4(origin, 1));
    const DVec3 normal = glm::normalize(glm::transpose(glm::inverse(DMat3(preview))) * face.normal);
    const DVec3 u = glm::normalize(DMat3(preview) * face.uAxis);
    const DVec3 v = glm::normalize(DMat3(preview) * face.vAxis);
    const std::array<std::byte, 4> texel{std::byte{128}, std::byte{180}, std::byte{220}, std::byte{255}};
    const auto slot = gpu->heap->upload({1, 1, VK_FORMAT_R8G8B8A8_UNORM}, texel);
    REQUIRE(slot);
    REQUIRE(gpu->heap->flush());
    auto list = cubeList(quad, DisplayMode::Lit, *slot);
    list.view = makeView(centre + normal * 96.0, anglesFromForward(-normal, {0, 0, 1}));
    list.exposure = 0.75f;
    auto& mesh = list.meshes[0];
    mesh.indexCount = 6;
    mesh.origin = origin;
    mesh.colour = Vec4(0.8f, 0.6f, 0.9f, 1);
    mesh.previewIndex = 1;
    list.previews.push_back({preview, origin});
    lighting::Light point;
    point.origin = centre + normal * 48.0 + u * 12.0;
    point.intensity = {0.8, 0.2, 0.1};
    point.constant = 1;
    point.linear = 0.002;
    point.quadratic = 0.0002;
    lighting::Light spot;
    spot.kind = lighting::LightKind::Spot;
    spot.origin = centre + normal * 70.0 - u * 12.0 + v * 12.0;
    spot.direction = glm::normalize(centre - spot.origin);
    spot.intensity = {0.05, 0.7, 0.6};
    spot.constant = 1;
    spot.quadratic = 0.0001;
    spot.stopdot = std::cos(glm::radians(20.0));
    spot.stopdot2 = std::cos(glm::radians(45.0));
    spot.exponent = 2;
    list.lights = {point, spot};
    SUBCASE("point") { list.lights = {point}; }
    SUBCASE("spot") { list.lights = {spot}; }
    SUBCASE("point and spot") {}
    const auto image = drawLitFrame(*gpu, list, scene);
    const auto inverseView = glm::inverse(DMat4(list.view.viewProjection));
    const auto inversePreview = glm::inverse(preview);
    const DVec3 albedo = glm::pow(DVec3(128, 180, 220) / 255.0 * DVec3(mesh.colour), DVec3(2.2));
    for (unsigned y : {29u, 32u, 35u}) {
        for (unsigned x : {29u, 32u, 35u}) {
            CAPTURE(x);
            CAPTURE(y);
            const auto ray4 = inverseView * DVec4(2.0 * (x + 0.5) / kSize - 1,
                                                 2.0 * (y + 0.5) / kSize - 1, 1, 1);
            const auto ray = glm::normalize(DVec3(ray4) / ray4.w);
            const double distance = glm::dot(centre - list.view.cameraPosition, normal) / glm::dot(ray, normal);
            REQUIRE(distance > 0);
            const auto position = list.view.cameraPosition + ray * distance;
            const auto local = DVec3(inversePreview * DVec4(position, 1)) - origin;
            // Independently intersect the transformed plane and stay clear of rasterized edges.
            REQUIRE(std::abs(glm::dot(local, face.uAxis)) < 38);
            REQUIRE(std::abs(glm::dot(local, face.vAxis)) < 38);
            DVec3 irradiance(0);
            for (const auto& light : list.lights) irradiance += lighting::irradiance(light, position, normal);
            const DVec3 display = glm::pow(glm::clamp(albedo * irradiance * double(list.exposure), DVec3(0), DVec3(1)),
                                          DVec3(1 / 2.2));
            const Px expected{int(std::lround(display.r * 255)), int(std::lround(display.g * 255)),
                              int(std::lround(display.b * 255))};
            // Uniform albedo avoids filtering error. Float32 math and RGBA16F storage contribute
            // less than one UNORM8 code here, including the final half-code rounding error.
            CHECK(nearColour(at(image, x, y), expected, 1));
        }
    }
    gpu->heap->release(*slot);
}
