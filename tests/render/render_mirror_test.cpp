#include "../assets/fixture.hpp"
#include "golden_image.hpp"
#include "render/context.hpp"
#include "shared_context.hpp"
#include "render/render_mirror.hpp"
#include "render/render_target.hpp"
#include "render/renderer.hpp"
#include "render/solid_mesh.hpp"
#include "render/texture_heap.hpp"
#include "render/view_math.hpp"
#include "editor/preview.hpp"
#include "editor/pick_index.hpp"

#include <doctest/doctest.h>
#include <vtfpp/VTF.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>

using namespace apex;
using namespace apex::render;

namespace {

constexpr std::uint32_t kSize = 128;
const float kClear[4] = {0.1f, 0.1f, 0.1f, 1.0f};

std::string readFixture(const char* name) {
    std::ifstream stream(std::filesystem::path(APEX_FIXTURE_DIR) / "vmf" / name, std::ios::binary);
    REQUIRE(stream.good());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::unique_ptr<document::Document> loadMap() {
    auto file = vmf::parse(readFixture("render_map.vmf"));
    REQUIRE(file.has_value());
    return std::make_unique<document::Document>(*file);
}

void writeVtf(const std::filesystem::path& path, bool checker, std::array<std::uint8_t, 3> colour,
              std::uint8_t alpha = 255) {
    std::vector<std::byte> pixels(16 * 16 * 4);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const bool alt = checker && (((x / 4) + (y / 4)) % 2 == 1);
            std::byte* p = &pixels[(static_cast<std::size_t>(y) * 16 + x) * 4];
            for (int c = 0; c < 3; ++c) {
                p[c] = std::byte{alt ? static_cast<std::uint8_t>(colour[c] / 2) : colour[c]};
            }
            p[3] = std::byte{alpha};
        }
    }
    vtfpp::VTF::CreationOptions options;
    options.version = 6;
    options.outputFormat = vtfpp::ImageFormat::RGBA8888;
    options.computeThumbnail = false;
    options.computeReflectivity = false;
    auto vtf = vtfpp::VTF::create(pixels, vtfpp::ImageFormat::RGBA8888, 16, 16, options);
    REQUIRE(vtf.bake(path));
}

struct Gpu {
    GameTree tree;
    JobPool jobs{2};
    std::unique_ptr<assets::Vfs> vfs;
    std::unique_ptr<assets::MaterialCache> cache;
    Context* context = nullptr; // Owned by testing::sharedContext().
    std::unique_ptr<TextureHeap> heap;
    std::unique_ptr<Renderer> renderer;
    RenderTarget target;
};

// Shared across cases; null (with a message) when no usable device exists.
Gpu* gpu() {
    static std::unique_ptr<Gpu> shared = [] {
        auto g = std::make_unique<Gpu>();
        g->context = testing::sharedContext(); // Before anything GPU-owned, so it outlives this Gpu.
        if (!g->context) return std::unique_ptr<Gpu>();
        g->tree.put("gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
        for (const char* name : {"floor", "red", "green", "blue"}) {
            g->tree.put(std::string("materials/test/") + name + ".vmt",
                        std::string("UnlitGeneric { $basetexture test/") + name + " }");
        }
        writeVtf(g->tree.root / "materials/test/floor.vtf", true, {200, 200, 200});
        writeVtf(g->tree.root / "materials/test/red.vtf", false, {255, 0, 0});
        writeVtf(g->tree.root / "materials/test/green.vtf", false, {0, 255, 0});
        writeVtf(g->tree.root / "materials/test/blue.vtf", false, {0, 0, 255});
        // Alpha-channel materials: half-transparent green glass, a fully transparent cutout, and
        // two nodraw spellings (by name, and by %compilenodraw alone).
        g->tree.put("materials/test/glass.vmt", "UnlitGeneric { $basetexture test/glass $translucent 1 }");
        g->tree.put("materials/test/cutout.vmt", "UnlitGeneric { $basetexture test/cutout $alphatest 1 }");
        g->tree.put("materials/test/skipped.vmt", "UnlitGeneric { $basetexture test/blue %compilenodraw 1 }");
        g->tree.put("materials/tools/toolsnodraw.vmt", "UnlitGeneric { $basetexture test/blue }");
        writeVtf(g->tree.root / "materials/test/glass.vtf", false, {0, 255, 0}, 128);
        writeVtf(g->tree.root / "materials/test/cutout.vtf", false, {0, 255, 0}, 0);
        assets::GameProfile profile;
        profile.gameDirectory = g->tree.root;
        auto vfs = assets::Vfs::load(profile, g->jobs);
        REQUIRE(vfs.has_value());
        g->vfs = std::make_unique<assets::Vfs>(std::move(*vfs));
        g->cache = std::make_unique<assets::MaterialCache>(*g->vfs, g->jobs);
        auto heap = TextureHeap::create(*g->context, {256, 4u << 20});
        REQUIRE(heap.has_value());
        g->heap = std::move(*heap);
        auto renderer = Renderer::create(*g->context, *g->heap, RenderTarget::kColorFormat, RenderTarget::kDepthFormat);
        REQUIRE(renderer.has_value());
        g->renderer = std::move(*renderer);
        auto target = RenderTarget::create(*g->context, kSize, kSize);
        REQUIRE(target.has_value());
        g->target = std::move(*target);
        return g;
    }();
    return shared.get();
}

#define REQUIRE_GPU()                                                                              \
    Gpu* g = gpu();                                                                                \
    if (!g) {                                                                                      \
        MESSAGE("Skipped: no Vulkan 1.3 device");                                                  \
        return;                                                                                    \
    }                                                                                              \
    g->context->validation().clear()

// A mirror bound to a document through the observer API, as the app will do.
struct Scene {
    std::unique_ptr<document::Document> doc;
    RenderMirror mirror;
    std::size_t token = 0;

    explicit Scene(Gpu& g, std::unique_ptr<document::Document> document = loadMap())
        : doc(std::move(document)), mirror(*g.context, *g.heap, g.jobs, *g.cache) {
        mirror.rebuild(*doc);
        REQUIRE(mirror.settle());
        token = doc->observe([this](const document::ChangeSet& c) { mirror.apply(*doc, c); });
    }
    ~Scene() { doc->unobserve(token); }
};

std::optional<document::NodeHandle> findSolid(const document::Document& doc, std::string_view material) {
    std::optional<document::NodeHandle> found;
    const auto walk = [&](const auto& self, document::NodeId id) -> void {
        const auto handle = doc.handle(id);
        if (!handle) return;
        const document::Node* node = doc.get(*handle);
        if (std::holds_alternative<Handle<document::Solid>>(*handle) && node->block) {
            const auto side = node->block->find("side");
            const auto m = side ? side->find("material") : nullptr;
            if (m && m->decodedValue() && *m->decodedValue() == material) found = *handle;
        }
        for (const auto child : node->children) self(self, child);
    };
    for (const auto root : doc.roots()) walk(walk, root);
    return found;
}

std::optional<document::NodeHandle> findEntity(const document::Document& doc, std::string_view classname) {
    std::optional<document::NodeHandle> found;
    const auto walk = [&](const auto& self, document::NodeId id) -> void {
        const auto handle = doc.handle(id);
        if (!handle) return;
        const document::Node* node = doc.get(*handle);
        if (std::holds_alternative<Handle<document::Entity>>(*handle) && node->block) {
            const auto c = node->block->find("classname");
            if (c && c->decodedValue() && *c->decodedValue() == classname) found = *handle;
        }
        for (const auto child : node->children) self(self, child);
    };
    for (const auto root : doc.roots()) walk(walk, root);
    return found;
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

Rgba8Image drawFrame(Gpu& g, RenderMirror& mirror, DisplayMode mode, bool edges = false,
                     std::optional<FrameView> view = std::nullopt) {
    MirrorViewOptions options;
    options.mode = mode;
    options.view = view ? *view : makeView({-220.0, -170.0, 130.0}, {23.0, 38.0, 0.0});
    options.edgeOverlay = edges;
    const DrawList list = mirror.buildDrawList(options);
    auto done = g.context->oneShot([&](VkCommandBuffer commands) {
        g.target.begin(commands, kClear);
        g.renderer->record(commands, list);
        g.target.end(commands);
    });
    REQUIRE(done.has_value());
    auto image = g.target.readback();
    REQUIRE(image.has_value());
    CHECK(g.context->validation().errorCount() == 0);
    for (const auto& e : g.context->validation().errors()) MESSAGE(e);
    return std::move(*image);
}

struct Px {
    int r, g, b;
};
Px at(const Rgba8Image& image, std::uint32_t x, std::uint32_t y) {
    const std::uint8_t* p = &image.pixels[(static_cast<std::size_t>(y) * image.width + x) * 4];
    return {p[0], p[1], p[2]};
}
bool isBackground(Px p) { return p.r >= 24 && p.r <= 28 && p.g >= 24 && p.g <= 28 && p.b >= 24 && p.b <= 28; }
template <typename F> std::size_t count(const Rgba8Image& image, F&& predicate) {
    std::size_t n = 0;
    for (std::uint32_t y = 0; y < image.height; ++y)
        for (std::uint32_t x = 0; x < image.width; ++x) n += predicate(at(image, x, y)) ? 1 : 0;
    return n;
}
bool reddish(Px p) { return p.r > 120 && p.g < 60 && p.b < 60; }
bool greenish(Px p) { return p.g > 120 && p.r < 60 && p.b < 60; }
bool bluish(Px p) { return p.b > 120 && p.r < 60 && p.g < 60; }

} // namespace

TEST_CASE("solid meshing: a box becomes one batch with expected counts and UVs") {
    const auto doc = loadMap();
    const auto red = findSolid(*doc, "test/red");
    REQUIRE(red.has_value());
    const auto& block = *doc->get(*red)->block;
    CHECK(solidMaterials(block) == std::vector<std::string>{"test/red"});
    auto mesh = meshSolid(block, [](std::string_view) { return TextureSize{16, 16}; });
    REQUIRE(mesh.has_value());
    REQUIRE(mesh->batches.size() == 1);
    CHECK(mesh->batches[0].vertices.size() == 24);
    CHECK(mesh->batches[0].indices.size() == 36);
    CHECK(mesh->batches[0].edges.size() == 48);
    CHECK(mesh->origin.x == doctest::Approx(-24.0));
    CHECK(mesh->bounds.max.z == doctest::Approx(48.0));
    // Top face: u = x / 4 / 16 texels; the vertex at x = 0, y = 0 has u = 0, v = 0.
    bool found = false;
    for (const BrushVertex& v : mesh->batches[0].vertices) {
        if (v.normal[2] > 0.5f && std::abs(v.position[0] - 24.0f) < 1e-3f && std::abs(v.position[1] - 24.0f) < 1e-3f) {
            CHECK(v.uv[0] == doctest::Approx(0.0f).epsilon(1e-4));
            CHECK(v.uv[1] == doctest::Approx(0.0f).epsilon(1e-4));
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("solid meshing: invalid solids fail cleanly") {
    auto parsed = kv1::parse("solid { \"id\" \"1\" side { \"id\" \"1\" \"plane\" \"(0 0 0) (0 1 0) (1 1 0)\" } }");
    REQUIRE(parsed.has_value());
    const auto result = meshSolid(parsed->root().children()[0], [](std::string_view) { return TextureSize{}; });
    CHECK(!result.has_value());
}

TEST_CASE("mirror: initial load meshes every solid once the textures are in") {
    REQUIRE_GPU();
    Scene scene(*g);
    CHECK(scene.mirror.solidCount() == 4);
    CHECK(scene.mirror.meshedSolidCount() == 4);
    CHECK(scene.mirror.batchCount() == 4);
    CHECK(scene.mirror.entityBoxCount() == 2); // light and info_target; worldspawn and func_detail excluded.
    CHECK(scene.mirror.stats().meshFailures == 0);
    CHECK(!scene.mirror.pending());
}

TEST_CASE("mirror: a change touching one solid re-meshes only that solid") {
    REQUIRE_GPU();
    Scene scene(*g);
    auto& mirror = scene.mirror;
    mirror.resetStats();

    const auto red = findSolid(*scene.doc, "test/red");
    REQUIRE(red.has_value());
    const auto side = std::get<Handle<document::Solid>>(*red);
    const document::Solid* solid = scene.doc->get(side);
    const auto id = solid->block->find("side")->find("id")->asInt();
    REQUIRE(id.has_value());

    SUBCASE("material edit") {
        auto tx = scene.doc->begin("retexture");
        tx.setSideKeyValue(*id, "material", "test/blue");
        tx.commit();
        CHECK(mirror.needsRedraw());
        REQUIRE(mirror.settle());
        CHECK(mirror.stats().jobsSubmitted == 1);
        CHECK(mirror.stats().solidsRemeshed == 1);
        CHECK(mirror.batchCount() == 5); // One face now in a second material.
    }
    SUBCASE("entity edit does not re-mesh any solid") {
        const auto light = findEntity(*scene.doc, "light");
        REQUIRE(light.has_value());
        auto tx = scene.doc->begin("move light");
        tx.setKeyValue(*light, "origin", "0 0 200");
        tx.commit();
        REQUIRE(mirror.settle());
        CHECK(mirror.stats().solidsRemeshed == 0);
        CHECK(mirror.stats().jobsSubmitted == 0);
        CHECK(mirror.needsRedraw());
    }
    SUBCASE("undo re-meshes the same single solid") {
        {
            auto tx = scene.doc->begin("retexture");
            tx.setSideKeyValue(*id, "material", "test/green");
            tx.commit();
        }
        REQUIRE(mirror.settle());
        mirror.resetStats();
        REQUIRE(scene.doc->undo());
        REQUIRE(mirror.settle());
        CHECK(mirror.stats().solidsRemeshed == 1);
        CHECK(mirror.batchCount() == 4);
    }
}

TEST_CASE("mirror: adding and removing a solid") {
    REQUIRE_GPU();
    Scene scene(*g);
    auto& mirror = scene.mirror;
    mirror.resetStats();

    const auto red = findSolid(*scene.doc, "test/red");
    const auto world = findEntity(*scene.doc, "worldspawn");
    REQUIRE(red.has_value());
    REQUIRE(world.has_value());
    const kv1::Node copy = *scene.doc->get(*red)->block;

    document::NodeHandle created;
    {
        auto tx = scene.doc->begin("add");
        created = tx.create(copy, world);
        tx.commit();
    }
    REQUIRE(mirror.settle());
    CHECK(mirror.solidCount() == 5);
    CHECK(mirror.meshedSolidCount() == 5);
    CHECK(mirror.stats().solidsRemeshed == 1);
    CHECK(mirror.stats().jobsSubmitted == 1);

    mirror.resetStats();
    {
        auto tx = scene.doc->begin("remove");
        tx.destroy(created);
        tx.commit();
    }
    REQUIRE(mirror.settle());
    CHECK(mirror.solidCount() == 4);
    CHECK(mirror.stats().solidsFreed == 1);
    CHECK(mirror.stats().solidsRemeshed == 0);
    CHECK(mirror.batchCount() == 4);

    // Undo brings it back under a new handle: one more mesh, nothing else.
    REQUIRE(scene.doc->undo());
    REQUIRE(mirror.settle());
    CHECK(mirror.solidCount() == 5);
    CHECK(mirror.stats().solidsRemeshed == 1);
    CHECK(g->context->validation().errorCount() == 0);
}

TEST_CASE("mirror: materials stream in with a placeholder and a pending redraw") {
    REQUIRE_GPU();
    // A fresh cache so the textures are not already resident.
    assets::MaterialCache cache(*g->vfs, g->jobs);
    auto doc = loadMap();
    RenderMirror mirror(*g->context, *g->heap, g->jobs, cache);
    mirror.rebuild(*doc);
    CHECK(mirror.needsRedraw());
    CHECK(mirror.pending());
    // Drawing mid-load is legal: unloaded materials show the checkerboard slot.
    const Rgba8Image early = drawFrame(*g, mirror, DisplayMode::Textured);
    CHECK(early.width == kSize);
    REQUIRE(mirror.settle());
    CHECK(mirror.needsRedraw()); // Meshes and textures arrived since the last draw.
    const Rgba8Image late = drawFrame(*g, mirror, DisplayMode::Textured);
    CHECK(!mirror.needsRedraw());
    CHECK(count(late, reddish) > 20);
    CHECK(early.pixels != late.pixels);
}

TEST_CASE("mirror: render in each display mode") {
    REQUIRE_GPU();
    Scene scene(*g);
    SUBCASE("textured") {
        const auto image = drawFrame(*g, scene.mirror, DisplayMode::Textured);
        CHECK(isBackground(at(image, 1, 1)));
        CHECK(count(image, reddish) > 20);
        CHECK(count(image, bluish) > 5);
        CHECK(count(image, greenish) > 10);
        CHECK(count(image, [](Px p) { return !isBackground(p); }) > 1500);
    }
    SUBCASE("flat") {
        const auto image = drawFrame(*g, scene.mirror, DisplayMode::Flat);
        CHECK(isBackground(at(image, 1, 1)));
        CHECK(count(image, [](Px p) { return !isBackground(p); }) > 1500);
        CHECK(count(image, reddish) == 0); // Flat colours are material-hash pastels, not texture colours.
    }
    SUBCASE("wireframe") {
        const auto image = drawFrame(*g, scene.mirror, DisplayMode::Wireframe);
        CHECK(isBackground(at(image, 1, 1)));
        const auto lines = count(image, [](Px p) { return !isBackground(p); });
        CHECK(lines > 100);
        CHECK(lines < kSize * kSize / 4);
    }
}

TEST_CASE("mirror: golden images of the fixture map") {
    REQUIRE_GPU();
    Scene scene(*g);
    golden::check("render_map_textured", drawFrame(*g, scene.mirror, DisplayMode::Textured), *g->context);
    golden::check("render_map_flat", drawFrame(*g, scene.mirror, DisplayMode::Flat, true), *g->context);
    golden::check("render_map_wireframe", drawFrame(*g, scene.mirror, DisplayMode::Wireframe), *g->context);
}

TEST_CASE("mirror: grid plane settings pass through to the draw list") {
    REQUIRE_GPU();
    Scene scene(*g);
    MirrorViewOptions options;
    options.grid.enabled = true;
    options.grid.origin = {60000, -65536, 128};
    options.grid.normal = {1, 0, 0};
    options.grid.uAxis = {0, 1, 0};
    options.grid.vAxis = {0, 0, 1};
    options.grid.bounds = GridRect{-32, 64, -16, 48};
    options.grid.margin = 8;
    options.grid.spacing = 0.125f;
    options.grid.fadeDistance = 512;
    options.grid.colour = Vec4(0.2f, 0.4f, 0.8f, 0.6f);
    const auto list = scene.mirror.buildDrawList(options);
    CHECK(list.grid.enabled);
    CHECK(list.grid.origin == options.grid.origin);
    CHECK(list.grid.normal == options.grid.normal);
    CHECK(list.grid.uAxis == options.grid.uAxis);
    CHECK(list.grid.vAxis == options.grid.vAxis);
    REQUIRE(list.grid.bounds);
    CHECK(list.grid.bounds->uMin == -32);
    CHECK(list.grid.bounds->uMax == 64);
    CHECK(list.grid.bounds->vMin == -16);
    CHECK(list.grid.bounds->vMax == 48);
    CHECK(list.grid.margin == 8.0f);
    CHECK(list.grid.spacing == 0.125f);
    CHECK(list.grid.fadeDistance == 512.0f);
    CHECK(list.grid.colour == options.grid.colour);
}

TEST_CASE("solid meshing: face ranges retain SideIds and cover triangles and polygon edges") {
    const auto doc = loadMap();
    const auto red = findSolid(*doc, "test/red");
    REQUIRE(red);
    const auto& block = *doc->get(*red)->block;
    const auto mesh = meshSolid(block, [](std::string_view) { return TextureSize{}; });
    REQUIRE(mesh);
    const auto& batch = mesh->batches[0];
    REQUIRE(batch.faces.size() == 6);
    std::uint32_t triangles = 0, edges = 0;
    for (const FaceRange& face : batch.faces) {
        CHECK(doc->side(face.side).has_value());
        CHECK(face.firstIndex == triangles);
        CHECK(face.firstEdgeIndex == edges);
        CHECK(face.indexCount == 6);
        CHECK(face.edgeIndexCount == 8);
        triangles += face.indexCount;
        edges += face.edgeIndexCount;
    }
    CHECK(triangles == batch.indices.size());
    CHECK(edges == batch.edges.size());
}

TEST_CASE("mirror: highlight changes only draw state in every display mode") {
    REQUIRE_GPU();
    Scene scene(*g);
    const auto red = findSolid(*scene.doc, "test/red");
    const auto blue = findSolid(*scene.doc, "test/blue");
    const auto light = findEntity(*scene.doc, "light");
    REQUIRE(red);
    REQUIRE(blue);
    REQUIRE(light);
    const auto redId = scene.doc->get(*red)->id;
    const auto blueId = scene.doc->get(*blue)->id;
    const auto lightId = scene.doc->get(*light)->id;
    const auto sideId = *scene.doc->get(*red)->block->find("side")->find("id")->asInt();
    for (const auto mode : {DisplayMode::Textured, DisplayMode::Flat, DisplayMode::Wireframe}) {
        MirrorViewOptions options;
        options.mode = mode;
        scene.mirror.setHighlight({});
        const auto before = scene.mirror.buildDrawList(options);
        CHECK_FALSE(scene.mirror.needsRedraw());
        scene.mirror.resetStats();
        Highlight h;
        h.selected = {redId, lightId, 9999999, redId};
        h.hover = blueId;
        scene.mirror.setHighlight(h);
        CHECK(scene.mirror.needsRedraw());
        const auto list = scene.mirror.buildDrawList(options);
        REQUIRE(list.meshes.size() == before.meshes.size());
        REQUIRE(list.outlines.size() == 1);
        REQUIRE(list.boxOutlines.size() == 1);
        CHECK(list.outlines[0].origin == DVec3(-24, -24, 24));
        for (std::size_t i = 0; i < list.meshes.size(); ++i) {
            const auto& draw = list.meshes[i];
            CHECK(draw.vertices == before.meshes[i].vertices);
            CHECK(draw.indices == before.meshes[i].indices);
            CHECK(draw.edgeIndices == before.meshes[i].edgeIndices);
            const Vec4 expected = draw.origin == DVec3(-24, -24, 24) ? h.selectTint :
                                  draw.origin == DVec3(40, 24, 16) ? h.hoverTint : Vec4(0);
            CHECK(draw.highlight == expected);
        }
        CHECK(list.boxes[list.boxOutlines[0].firstInstance].tint == h.selectTint);
        CHECK_FALSE(scene.mirror.needsRedraw());
        scene.mirror.setHighlight(h);
        CHECK_FALSE(scene.mirror.needsRedraw());
        h.hover = redId; // Selection wins over hover.
        scene.mirror.setHighlight(h);
        const auto selected = scene.mirror.buildDrawList(options);
        for (const auto& draw : selected.meshes)
            if (draw.origin == DVec3(-24, -24, 24)) CHECK(draw.highlight == h.selectTint);

        h.selected.clear();
        h.hover = lightId;
        scene.mirror.setHighlight(h);
        const auto hoveredEntity = scene.mirror.buildDrawList(options);
        CHECK(hoveredEntity.boxOutlines.empty());
        CHECK(hoveredEntity.boxes[list.boxOutlines[0].firstInstance].tint == h.hoverTint);
        h.hover = 9999999;
        h.selectedSides = {sideId, -1};
        scene.mirror.setHighlight(h);
        const auto faces = scene.mirror.buildDrawList(options);
        REQUIRE(faces.outlines.size() == 1);
        CHECK(faces.outlines[0].edgeIndexCount == 8);
        CHECK(faces.outlines[0].indexCount == 6);
        CHECK(faces.meshes.size() == before.meshes.size() + 5);
        CHECK(std::ranges::count_if(faces.meshes, [&](const MeshDraw& d) { return d.highlight == h.selectTint; }) == 1);
        options.showEntities = false;
        h.selected = {lightId};
        h.selectedSides.clear();
        scene.mirror.setHighlight(h);
        const auto noEntities = scene.mirror.buildDrawList(options);
        CHECK(noEntities.boxes.empty());
        CHECK(noEntities.boxOutlines.empty());
        CHECK(noEntities.outlines.empty());
        scene.mirror.pump();
        const auto& stats = scene.mirror.stats();
        CHECK(stats.jobsSubmitted == 0);
        CHECK(stats.solidsRemeshed == 0);
        CHECK(stats.meshFailures == 0);
        CHECK(stats.solidsFreed == 0);
        CHECK(stats.texturesUploaded == 0);
    }
}

TEST_CASE("mirror: hidden solids and point entities never highlight") {
    REQUIRE_GPU();
    Scene scene(*g);
    const auto red = findSolid(*scene.doc, "test/red");
    const auto light = findEntity(*scene.doc, "light");
    REQUIRE(red);
    REQUIRE(light);
    Highlight h;
    h.selected = {scene.doc->get(*red)->id, scene.doc->get(*light)->id};
    h.hover = h.selected[0];
    h.selectedSides = {*scene.doc->get(*red)->block->find("side")->find("id")->asInt()};
    auto parsed = kv1::parse("hidden {}");
    REQUIRE(parsed);
    auto tx = scene.doc->begin("hide");
    const auto wrapper = tx.create(parsed->root().children()[0]);
    const auto groupBlock = kv1::parse("group {}");
    REQUIRE(groupBlock);
    const auto group = tx.create(groupBlock->root().children()[0]);
    tx.reparent(*red, group);
    tx.reparent(*light, group);
    tx.commit();
    REQUIRE(scene.mirror.settle());
    auto hide = scene.doc->begin("hide group");
    hide.reparent(group, wrapper);
    hide.commit();
    REQUIRE(scene.mirror.settle());
    scene.mirror.setHighlight(h);
    const auto list = scene.mirror.buildDrawList({});
    CHECK(list.meshes.size() == 3);
    CHECK(list.boxes.size() == 1);
    CHECK(list.outlines.empty());
    CHECK(list.boxOutlines.empty());
    REQUIRE(scene.doc->undo());
    REQUIRE(scene.mirror.settle());
    const auto restored = scene.mirror.buildDrawList({});
    CHECK(restored.meshes.size() == 4);
    CHECK(restored.outlines.size() == 1);
    CHECK(restored.boxOutlines.size() == 1);
}

TEST_CASE("mirror: selecting ten thousand solids reuses all uploaded geometry") {
    REQUIRE_GPU();
    Scene scene(*g);
    const auto red = findSolid(*scene.doc, "test/red");
    const auto world = findEntity(*scene.doc, "worldspawn");
    REQUIRE(red);
    REQUIRE(world);
    const auto block = *scene.doc->get(*red)->block;
    Highlight h;
    auto tx = scene.doc->begin("many solids");
    for (std::size_t i = 0; i < 10000; ++i) {
        const auto solid = tx.create(block, world);
        h.selected.push_back(scene.doc->get(solid)->id);
    }
    tx.commit();
    REQUIRE(scene.mirror.settle(std::chrono::seconds(120)));
    const auto before = scene.mirror.buildDrawList({});
    scene.mirror.resetStats();
    scene.mirror.setHighlight(h);
    CHECK(scene.mirror.needsRedraw());
    scene.mirror.pump();
    const auto after = scene.mirror.buildDrawList({});
    CHECK_FALSE(scene.mirror.needsRedraw());
    CHECK(after.outlines.size() == 10000);
    REQUIRE(after.meshes.size() == before.meshes.size());
    for (std::size_t i = 0; i < after.meshes.size(); ++i) {
        CHECK(after.meshes[i].vertices == before.meshes[i].vertices);
        CHECK(after.meshes[i].indices == before.meshes[i].indices);
        CHECK(after.meshes[i].edgeIndices == before.meshes[i].edgeIndices);
    }
    const auto& stats = scene.mirror.stats();
    CHECK(stats.jobsSubmitted == 0);
    CHECK(stats.solidsRemeshed == 0);
    CHECK(stats.meshFailures == 0);
    CHECK(stats.solidsFreed == 0);
    CHECK(stats.texturesUploaded == 0);
}

TEST_CASE("mirror: highlight golden images in every display mode") {
    REQUIRE_GPU();
    for (const auto mode : {DisplayMode::Textured, DisplayMode::Flat, DisplayMode::Wireframe}) {
        const std::string suffix = mode == DisplayMode::Textured ? "textured" : mode == DisplayMode::Flat ? "flat" : "wireframe";
        for (const std::string scenario : {"selected", "hovered", "occluded", "entity", "face"}) {
            CAPTURE(suffix);
            CAPTURE(scenario);
            Scene scene(*g);
            const auto red = findSolid(*scene.doc, "test/red");
            const auto light = findEntity(*scene.doc, "light");
            REQUIRE(red);
            REQUIRE(light);
            Highlight h;
            MirrorViewOptions options;
            options.mode = mode;
            options.view = makeView({-150, -110, 100}, {27, 33, 0});
            const auto redId = scene.doc->get(*red)->id;
            if (scenario == "selected") h.selected = {redId};
            if (scenario == "hovered") h.hover = redId;
            if (scenario == "face") h.selectedSides = {*scene.doc->get(*red)->block->find("side")->find("id")->asInt()};
            if (scenario == "entity") {
                h.selected = {scene.doc->get(*light)->id};
                options.view = makeView({-50, -40, 125}, {24, 38, 0});
            }
            if (scenario == "occluded") {
                // A same-sized brush behind the red wall is entirely hidden in this view.
                auto tx = scene.doc->begin("rear brush");
                const auto rear = tx.create(*scene.doc->get(*red)->block);
                auto geometry = geometry::solidGeometry(*scene.doc->get(rear)->block);
                REQUIRE(geometry);
                for (auto& p : geometry->poly.vertices) p.x += 96;
                for (auto& face : geometry->poly.faces) face.plane = geometry->poly.derivePlane(face).value();
                tx.setSolidGeometry(std::get<Handle<document::Solid>>(rear), *geometry);
                tx.commit();
                REQUIRE(scene.mirror.settle());
                h.selected = {scene.doc->get(rear)->id};
                options.view = makeView({-152, -24, 24}, {0, 0, 0});
            }
            scene.mirror.setHighlight(h);
            const auto list = scene.mirror.buildDrawList(options);
            REQUIRE(g->context->oneShot([&](VkCommandBuffer commands) {
                g->target.begin(commands, kClear);
                g->renderer->record(commands, list);
                g->target.end(commands);
            }));
            const auto image = g->target.readback();
            REQUIRE(image);
            CHECK(g->context->validation().errorCount() == 0);
            const std::string name = "highlight_" + scenario + "_" + suffix;
            golden::check(name, *image, *g->context);
        }
    }
}

TEST_CASE("mirror preview changes draw state only and cancel restores the image") {
    REQUIRE_GPU();
    Scene scene(*g);
    const auto red = findSolid(*scene.doc, "test/red");
    const auto light = findEntity(*scene.doc, "light");
    REQUIRE(red);
    REQUIRE(light);
    const auto redId = scene.doc->get(*red)->id, lightId = scene.doc->get(*light)->id;
    const std::array nodes{redId, lightId};
    const auto preview = editor::fromTransform(nodes, editor::Transform::translate({0, 80, 32}));
    editor::PickIndex picks(*scene.doc);
    picks.attach(*scene.doc);
    const auto bounds = picks.bounds(redId);
    const auto bytes = scene.doc->write().value();
    const auto history = scene.doc->undoCount();
    int notifications = 0;
    scene.doc->observe([&](const document::ChangeSet&) { ++notifications; });
    Highlight highlight;
    highlight.selected = {redId, lightId};
    scene.mirror.setHighlight(highlight);
    const auto image = drawFrame(*g, scene.mirror, DisplayMode::Flat, true);
    const auto before = scene.mirror.buildDrawList({});
    scene.mirror.resetStats();
    scene.mirror.setPreview(preview.transforms());
    CHECK(scene.mirror.needsRedraw());
    CHECK_FALSE(scene.mirror.pending());
    const auto after = scene.mirror.buildDrawList({});
    CHECK_FALSE(scene.mirror.needsRedraw());
    scene.mirror.setPreview(preview.transforms());
    CHECK_FALSE(scene.mirror.needsRedraw());
    REQUIRE(after.previews.size() == 2);
    REQUIRE(after.outlines.size() == 1);
    REQUIRE(after.boxOutlines.size() == 1);
    CHECK(after.outlines[0].previewIndex != 0);
    CHECK(after.boxOutlines[0].previewIndex != 0);
    REQUIRE(after.meshes.size() == before.meshes.size());
    for (std::size_t i = 0; i < after.meshes.size(); ++i) {
        CHECK(after.meshes[i].vertices == before.meshes[i].vertices);
        CHECK(after.meshes[i].indices == before.meshes[i].indices);
        CHECK(after.meshes[i].edgeIndices == before.meshes[i].edgeIndices);
        CHECK(after.meshes[i].origin == before.meshes[i].origin);
    }
    CHECK(drawFrame(*g, scene.mirror, DisplayMode::Flat, true).pixels != image.pixels);
    scene.mirror.clearPreview();
    CHECK(scene.mirror.needsRedraw());
    CHECK(drawFrame(*g, scene.mirror, DisplayMode::Flat, true).pixels == image.pixels);
    CHECK_FALSE(scene.mirror.needsRedraw());
    scene.mirror.clearPreview();
    CHECK_FALSE(scene.mirror.needsRedraw());
    CHECK(scene.doc->write().value() == bytes);
    CHECK(scene.doc->undoCount() == history);
    CHECK(notifications == 0);
    REQUIRE(bounds);
    REQUIRE(picks.bounds(redId));
    CHECK(picks.bounds(redId)->min == bounds->min);
    CHECK(picks.bounds(redId)->max == bounds->max);
    CHECK(scene.mirror.stats().jobsSubmitted == 0);
    CHECK(scene.mirror.stats().solidsRemeshed == 0);
    CHECK(scene.mirror.stats().texturesUploaded == 0);
}

TEST_CASE("mirror preview Bite 9 brush images exactly match committed transforms") {
    REQUIRE_GPU();
    const DVec3 pivot{-24, -24, 24};
    const std::array transforms{
        editor::Transform::translate({8, 48, 16}),
        editor::Transform::rotate({0, 0, 1}, 90, pivot),
        editor::Transform::scale({2, 0.5, 1.25}, DMat3(1), pivot),
        editor::Transform::mirror({1, 0, 0}, {-12, -24, 24})};
    for (std::size_t t = 0; t < transforms.size(); ++t) {
        CAPTURE(t);
        Scene scene(*g);
        const auto red = findSolid(*scene.doc, "test/red");
        REQUIRE(red);
        const std::array nodes{scene.doc->get(*red)->id};
        Highlight highlight;
        highlight.selected = {nodes[0]};
        scene.mirror.setHighlight(highlight);
        std::vector<Rgba8Image> images;
        scene.mirror.setPreview(editor::fromTransform(nodes, transforms[t]).transforms());
        for (const auto mode : {DisplayMode::Textured, DisplayMode::Flat, DisplayMode::Wireframe})
            images.push_back(drawFrame(*g, scene.mirror, mode, true));
        REQUIRE(editor::commitTransform(*scene.doc, nodes, transforms[t], {}, "Transform"));
        scene.mirror.clearPreview();
        REQUIRE(scene.mirror.settle());
        std::size_t i = 0;
        for (const auto mode : {DisplayMode::Textured, DisplayMode::Flat, DisplayMode::Wireframe}) {
            CAPTURE(mode);
            const auto committed = drawFrame(*g, scene.mirror, mode, true);
            CHECK(images[i++].pixels == committed.pixels);
        }
    }
}

TEST_CASE("mirror preview containers cover descendants with child overrides and no double transform") {
    REQUIRE_GPU();
    Scene scene(*g);
    const auto brush = findEntity(*scene.doc, "func_detail");
    REQUIRE(brush);
    const auto brushId = scene.doc->get(*brush)->id;
    const auto& children = scene.doc->get(*brush)->children;
    REQUIRE_FALSE(children.empty());
    const auto child = children[0];
    const auto move = editor::Transform::translate({32, 0, 0}).matrix();
    scene.mirror.setPreview({{brushId, move}});
    const auto inherited = scene.mirror.buildDrawList({});
    REQUIRE(inherited.previews.size() == 1);
    CHECK(inherited.previews[0].matrix == move);
    const auto childMove = editor::Transform::translate({0, 64, 0}).matrix();
    scene.mirror.setPreview({{brushId, move}, {child, childMove}, {99999999, move}});
    const auto overridden = scene.mirror.buildDrawList({});
    REQUIRE(overridden.previews.size() == 1);
    CHECK(overridden.previews[0].matrix == childMove);
    const auto red = findSolid(*scene.doc, "test/red");
    REQUIRE(red);
    auto tx = scene.doc->begin("reparent during preview");
    tx.reparent(*red, *brush);
    tx.commit();
    REQUIRE(scene.mirror.settle());
    CHECK(scene.mirror.buildDrawList({}).previews.size() == 2);
    REQUIRE(scene.doc->undo());
    REQUIRE(scene.mirror.settle());
    CHECK(scene.mirror.buildDrawList({}).previews.size() == 1);
}

TEST_CASE("mirror preview 2000 solids builds linear draw state without meshing") {
    REQUIRE_GPU();
    Scene scene(*g);
    const auto red = findSolid(*scene.doc, "test/red");
    const auto world = findEntity(*scene.doc, "worldspawn");
    REQUIRE(red);
    REQUIRE(world);
    const auto block = *scene.doc->get(*red)->block;
    PreviewTransforms preview;
    const auto matrix = editor::Transform::translate({1, 2, 3}).matrix();
    auto tx = scene.doc->begin("2000 solids");
    for (std::size_t i = 0; i < 2000; ++i) {
        const auto solid = tx.create(block, world);
        preview.emplace(scene.doc->get(solid)->id, matrix);
    }
    tx.commit();
    REQUIRE(scene.mirror.settle(std::chrono::seconds(120)));
    const auto before = scene.mirror.buildDrawList({});
    scene.mirror.resetStats();
    const auto start = std::chrono::steady_clock::now();
    scene.mirror.setPreview(preview);
    for (int frame = 0; frame < 20; ++frame) {
        const auto after = scene.mirror.buildDrawList({});
        REQUIRE(after.previews.size() == 2000);
        REQUIRE(after.meshes.size() == before.meshes.size());
        for (std::size_t i = 0; i < after.meshes.size(); ++i) {
            CHECK(after.meshes[i].vertices == before.meshes[i].vertices);
            CHECK(after.meshes[i].indices == before.meshes[i].indices);
            CHECK(after.meshes[i].edgeIndices == before.meshes[i].edgeIndices);
        }
    }
    // Loose debug/ASan guard against accidental quadratic scans, not a frame-time benchmark.
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    CHECK_FALSE(scene.mirror.pending());
    scene.mirror.pump();
    CHECK(scene.mirror.stats().jobsSubmitted == 0);
    CHECK(scene.mirror.stats().solidsRemeshed == 0);
    CHECK(scene.mirror.stats().texturesUploaded == 0);
}

namespace {

// An axis-aligned box solid, one material on every side. Plane windings put normals outward.
std::string boxSolid(int id, std::array<double, 3> lo, std::array<double, 3> hi, const std::string& material) {
    const auto [x0, y0, z0] = lo;
    const auto [x1, y1, z1] = hi;
    const auto pt = [](double x, double y, double z) { return "(" + std::to_string(x) + " " + std::to_string(y) + " " + std::to_string(z) + ") "; };
    const std::string planes[6] = {
        pt(x0, y1, z1) + pt(x1, y1, z1) + pt(x1, y0, z1), pt(x0, y0, z0) + pt(x1, y0, z0) + pt(x1, y1, z0),
        pt(x0, y1, z1) + pt(x0, y0, z1) + pt(x0, y0, z0), pt(x1, y1, z0) + pt(x1, y0, z0) + pt(x1, y0, z1),
        pt(x1, y0, z0) + pt(x0, y0, z0) + pt(x0, y0, z1), pt(x0, y1, z0) + pt(x1, y1, z0) + pt(x1, y1, z1)};
    std::string out = "solid { \"id\" \"" + std::to_string(id) + "\"\n";
    for (int i = 0; i < 6; ++i) {
        out += "side { \"id\" \"" + std::to_string(id * 10 + i) + "\" \"plane\" \"" + planes[i] + "\" \"material\" \"" + material +
               "\" \"uaxis\" \"[1 0 0 0] 0.25\" \"vaxis\" \"[0 -1 0 0] 0.25\" \"rotation\" \"0\" "
               "\"lightmapscale\" \"16\" \"smoothing_groups\" \"0\" }\n";
    }
    return out + "}\n";
}

std::unique_ptr<document::Document> boxesDocument(const std::vector<std::pair<std::string, std::array<double, 6>>>& boxes) {
    std::string text = "versioninfo { \"editorextension\" \"strata\" }\nworld { \"id\" \"1\" \"classname\" \"worldspawn\"\n";
    int id = 2;
    for (const auto& [material, b] : boxes) text += boxSolid(id++, {b[0], b[1], b[2]}, {b[3], b[4], b[5]}, material);
    text += "}\n";
    auto file = vmf::parse(text);
    REQUIRE(file.has_value());
    return std::make_unique<document::Document>(*file);
}

std::size_t countAlpha(const DrawList& list, AlphaMode mode) {
    return static_cast<std::size_t>(std::ranges::count(list.meshes, mode, &MeshDraw::alpha));
}

} // namespace

TEST_CASE("mirror: material alpha flags select the draw's alpha mode") {
    REQUIRE_GPU();
    Scene scene(*g, boxesDocument({{"test/red", {0, 0, 0, 32, 32, 32}},
                                   {"test/glass", {64, 0, 0, 96, 32, 32}},
                                   {"test/cutout", {128, 0, 0, 160, 32, 32}}}));
    MirrorViewOptions options;
    options.mode = DisplayMode::Textured;
    auto list = scene.mirror.buildDrawList(options);
    REQUIRE(list.meshes.size() == 3);
    CHECK(countAlpha(list, AlphaMode::Opaque) == 1);
    CHECK(countAlpha(list, AlphaMode::Blend) == 1);
    CHECK(countAlpha(list, AlphaMode::Test) == 1);
    // Only Textured mode samples the texture's alpha.
    for (const auto mode : {DisplayMode::Flat, DisplayMode::Wireframe}) {
        options.mode = mode;
        list = scene.mirror.buildDrawList(options);
        CHECK(countAlpha(list, AlphaMode::Opaque) == 3);
    }
}

TEST_CASE("mirror: showNodraw filters nodraw fills by name or %compilenodraw") {
    REQUIRE_GPU();
    const auto document = [] {
        return boxesDocument({{"test/red", {0, 0, 0, 32, 32, 32}},
                              {"TOOLS/ToolsNoDraw", {64, 0, 0, 96, 32, 32}},
                              {"test/skipped", {128, 0, 0, 160, 32, 32}}});
    };
    Scene scene(*g, document());
    MirrorViewOptions options;
    for (const auto mode : {DisplayMode::Textured, DisplayMode::Flat, DisplayMode::Wireframe}) {
        options.mode = mode;
        options.showNodraw = true;
        CHECK(scene.mirror.buildDrawList(options).meshes.size() == 3);
        options.showNodraw = false;
        // Wireframe keeps every brush edge so 2D views stay complete.
        CHECK(scene.mirror.buildDrawList(options).meshes.size() == (mode == DisplayMode::Wireframe ? 3u : 1u));
    }

    // A hidden nodraw solid still shows its selection outline.
    const auto nodraw = findSolid(*scene.doc, "TOOLS/ToolsNoDraw");
    REQUIRE(nodraw);
    Highlight highlight;
    highlight.selected = {scene.doc->get(*nodraw)->id};
    scene.mirror.setHighlight(highlight);
    options.mode = DisplayMode::Textured;
    options.showNodraw = false;
    const auto list = scene.mirror.buildDrawList(options);
    CHECK(list.meshes.size() == 1);
    CHECK(list.outlines.size() == 1);
}

TEST_CASE("mirror: translucent and alpha-tested textures render through the pass") {
    REQUIRE_GPU();
    // Camera at x = -100 looking down +x; the red wall is behind a front box in the middle.
    const auto view = makeView({-100.0, 0.0, 0.0}, {0.0, 0.0, 0.0});
    const auto centre = [&](const std::string& front) {
        Scene scene(*g, boxesDocument({{"test/red", {200, -200, -200, 300, 200, 200}},
                                       {front, {0, -50, -50, 50, 50, 50}}}));
        const auto image = drawFrame(*g, scene.mirror, DisplayMode::Textured, false, view);
        return at(image, kSize / 2, kSize / 2);
    };
    const Px opaque = centre("test/green");
    CHECK(opaque.g > 100);
    CHECK(opaque.r < 10);

    const Px glass = centre("test/glass"); // Green at 50% over the red wall.
    CHECK(glass.g > 40);
    CHECK(glass.r > 40);
    CHECK(glass.g < opaque.g);

    const Px cutout = centre("test/cutout"); // Alpha 0 everywhere: the wall shows through.
    CHECK(cutout.r > 100);
    CHECK(cutout.g < 10);
}

TEST_CASE("mirror: translucent faces win against a coplanar opaque face") {
    REQUIRE_GPU();
    // The glass front sits 0.004 units behind the red wall's face, inside the layer bias at this
    // distance (about 0.006), so the glass must still blend over it. Without the bias the wall
    // would hide the glass entirely.
    const auto view = makeView({-100.0, 0.0, 0.0}, {0.0, 0.0, 0.0});
    Scene scene(*g, boxesDocument({{"test/red", {200, -200, -200, 300, 200, 200}},
                                   {"test/glass", {200.004, -50, -50, 250, 50, 50}}}));
    const auto image = drawFrame(*g, scene.mirror, DisplayMode::Textured, false, view);
    const Px centre = at(image, kSize / 2, kSize / 2);
    CHECK(centre.g > 40);
    CHECK(centre.r > 40);
}
