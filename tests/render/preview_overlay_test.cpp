#include "../assets/fixture.hpp"
#include "../editor/fixtures.hpp"
#include "bitmap_font.hpp"
#include "golden_image.hpp"
#include "shared_context.hpp"
#include "render/render_mirror.hpp"
#include "render/render_target.hpp"
#include "render/view_math.hpp"
#include "editor/compile_overlay.hpp"
#include "editor/editor_session.hpp"
#include "../editor/fixtures.hpp"
#include "editor/vertex_tool.hpp"

#include <vtfpp/VTF.h>

#include <cstdlib>
#include <iomanip>
#include <iostream>

using namespace apex;
using namespace apex::render;

namespace {

constexpr unsigned kSize = 128;

// Solid colour, so the textured golden carries no sampler-dependent checker aliasing.
void writeVtf(const std::filesystem::path& path, std::array<std::uint8_t, 3> colour) {
    std::vector<std::byte> pixels(16 * 16 * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        for (int c = 0; c < 3; ++c) pixels[i + c] = std::byte{colour[c]};
        pixels[i + 3] = std::byte{255};
    }
    vtfpp::VTF::CreationOptions options;
    options.version = 6;
    options.outputFormat = vtfpp::ImageFormat::RGBA8888;
    options.computeThumbnail = false;
    options.computeReflectivity = false;
    auto vtf = vtfpp::VTF::create(pixels, vtfpp::ImageFormat::RGBA8888, 16, 16, options);
    REQUIRE(vtf.bake(path));
}
const float kClear[] = {0.1f, 0.1f, 0.1f, 1};

struct Gpu {
    Context* context = testing::sharedContext();
    GameTree tree;
    JobPool jobs{2};
    std::unique_ptr<assets::Vfs> vfs;
    std::unique_ptr<assets::MaterialCache> cache;
    std::unique_ptr<TextureHeap> heap;
    std::unique_ptr<Renderer> renderer;
    RenderTarget target;

    Gpu() {
        if (!context) return;
        context->validation().clear();
        tree.put("gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
        tree.put("materials/test/preview.vmt", "UnlitGeneric { $basetexture test/preview }");
        tree.put("materials/test/ghost.vmt", "UnlitGeneric { $basetexture test/ghost }");
        writeVtf(tree.root / "materials/test/preview.vtf", {200, 160, 60});
        writeVtf(tree.root / "materials/test/ghost.vtf", {40, 120, 255});
        assets::GameProfile profile;
        profile.gameDirectory = tree.root;
        auto loaded = assets::Vfs::load(profile, jobs);
        REQUIRE(loaded);
        vfs = std::make_unique<assets::Vfs>(std::move(*loaded));
        cache = std::make_unique<assets::MaterialCache>(*vfs, jobs);
        auto h = TextureHeap::create(*context, {256, 4u << 20});
        REQUIRE(h);
        heap = std::move(*h);
        auto r = Renderer::create(*context, *heap, RenderTarget::kColorFormat, RenderTarget::kDepthFormat);
        REQUIRE(r);
        renderer = std::move(*r);
        auto t = RenderTarget::create(*context, kSize, kSize);
        REQUIRE(t);
        target = std::move(*t);
    }
};

#define REQUIRE_GPU() Gpu g; if (!g.context) { MESSAGE("Skipped: no Vulkan 1.3 device"); return; }

MirrorViewOptions options(DVec3 offset = DVec3(0)) {
    MirrorViewOptions result;
    result.mode = DisplayMode::Flat;
    result.view.cameraPosition = DVec3(-128, 0, 0) + offset;
    result.view.viewProjection = reversedZOrtho(64, 64, 1, 512) * rotationOnlyView(angleVectors({0, 0, 0}));
    return result;
}

Rgba8Image frame(Gpu& g, DrawList list) {
    list.viewportSize = Vec2(kSize);
    REQUIRE(g.context->oneShot([&](VkCommandBuffer commands) {
        g.target.begin(commands, kClear);
        g.renderer->record(commands, list);
        g.target.end(commands);
    }));
    auto image = g.target.readback();
    REQUIRE(image);
    CHECK(g.context->validation().errorCount() == 0);
    return std::move(*image);
}

// Budget tests use a missing material: a texture arriving mid-test changes sizes and remeshes.
geometry::ConvexPolyhedron box(DVec3 lo, DVec3 hi, const std::string& material = "test/preview") {
    auto poly = geometry_test::build(geometry_test::boxSides(lo, hi));
    for (auto& face : poly.faces) face.material = material;
    return poly;
}

int channel(const Rgba8Image& image, unsigned x, unsigned y, unsigned c) {
    return image.pixels[(y * image.width + x) * 4 + c];
}

} // namespace

TEST_CASE("[measure] 0.3 release preview ticks for 500 vertex-selected solids") {
    const auto* enabled = std::getenv("APEX_MEASURE");
    if (!enabled || std::string_view(enabled) != "1") {
        MESSAGE("APEX_MEASURE=1 is required; skipping release measurements");
        return;
    }
    REQUIRE_GPU();
    document::Document doc(editor_test::parsed());
    std::vector<document::NodeId> ids;
    ids.reserve(500);
    for (std::size_t i = 0; i < 500; ++i) {
        const DVec3 lo{double(i % 100) * 80.0, double(i / 100) * 80.0, 0};
        const auto handle = editor_test::addBox(doc, lo, lo + DVec3(64.0));
        ids.push_back(doc.get(std::get<Handle<document::Solid>>(handle))->id);
    }
    editor::EditorSession session;
    session.attach(doc);
    session.setPreset(editor_test::blenderPreset());
    session.selection().setMode(editor::SelectionMode::Solids);
    session.selection().setIgnoreGroups(true);
    std::vector<editor::Target> selection;
    for (const auto id : ids) selection.emplace_back(id);
    session.selection().replace(selection);
    REQUIRE(session.perform("tool.vertex"));

    editor::ToolView camera;
    camera.size = {1600, 900};
    camera.cameraPosition = {4000, -8000, 6000};
    camera.cameraForward = glm::normalize(DVec3{4000, 360, 32} - camera.cameraPosition);
    camera.cameraUp = {0, 0, 1};
    camera.fovYDegrees = 90;
    camera.cursorRay = {camera.cameraPosition, camera.cameraForward};
    auto input = [&](editor::InputKind kind, DVec2 cursor, bool held = false) {
        editor::InputEvent e;
        e.kind = kind;
        e.source = 1;
        e.cursor = cursor;
        e.left = held || kind == editor::InputKind::Press;
        if (kind == editor::InputKind::Press || kind == editor::InputKind::Release)
            e.button = editor::PointerButton::Left;
        return e;
    };
    camera.cursorRay = {camera.cameraPosition, DVec3{0, 0, 1}}; // Begin the marquee over empty space.
    auto press = input(editor::InputKind::Press, {0, 0});
    REQUIRE(session.input(press, camera));
    auto move = input(editor::InputKind::Move, camera.size, true);
    REQUIRE(session.input(move, camera));
    auto release = input(editor::InputKind::Release, camera.size);
    REQUIRE(session.input(release, camera));
    auto& tool = static_cast<editor::VertexTool&>(session.activeToolObject());
    std::set<document::NodeId> vertexSolids;
    for (const auto& element : tool.elements()) vertexSolids.insert(element.solid);
    REQUIRE(vertexSolids.size() == 500);
    editor::InputEvent grab;
    grab.kind = editor::InputKind::KeyDown;
    grab.source = 1;
    grab.key = "G";
    grab.chord = editor::KeyChord{"G"};
    camera.cursorRay = {camera.cameraPosition, camera.cameraForward};
    REQUIRE(session.input(grab, camera));

    RenderMirror mirror(*g.context, *g.heap, g.jobs, *g.cache);
    mirror.rebuild(doc);
    REQUIRE(mirror.settle());
    std::vector<double> tickMs, handoffMs;
    std::size_t maxTicksForMove = 0;
    std::optional<std::uint64_t> lastRevision;
    // Eight moves, each far enough to change the snapped preview, so every one is a new revision.
    for (int sample = 0; sample < 8; ++sample) {
        const DVec3 target{4000, 360, 32};
        const DVec3 offset{double((sample + 1) * 128), 0, double((sample % 2 ? -1 : 1) * 64)};
        const auto direction = glm::normalize(target + offset - camera.cameraPosition);
        camera.cursorRay = {camera.cameraPosition, direction};
        move.cursor = {800.0 + (sample + 1) * 40.0, 450.0 + (sample % 2 ? -1 : 1) * 30.0};
        move.left = false; // A modal grab follows the bare pointer, as in the vertex tool tests.
        REQUIRE(session.input(move, camera));
        const auto& preview = session.preview();
        REQUIRE(preview.replacements().size() == 500);
        CAPTURE(sample);
        REQUIRE(preview.revision() != lastRevision);
        lastRevision = preview.revision();
        PreviewGeometry geometry;
        geometry.replacements = preview.replacements();
        PreviewTransforms transforms = preview.transforms();
        std::size_t ticks = 0;
        do {
            // setPreview's hand-off (copying the new revision, retiring old meshes) is timed apart
            // from pump(), where D12's budgeted meshing and upload happen.
            const auto start = std::chrono::steady_clock::now();
            mirror.setPreview(transforms, geometry, preview.revision());
            const auto handed = std::chrono::steady_clock::now();
            mirror.pump();
            const auto end = std::chrono::steady_clock::now();
            if (ticks == 0) handoffMs.push_back(std::chrono::duration<double, std::milli>(handed - start).count());
            tickMs.push_back(std::chrono::duration<double, std::milli>(end - handed).count());
            ++ticks;
        } while (mirror.pendingPreviewCount() > 0);
        maxTicksForMove = std::max(maxTicksForMove, ticks);
    }
    std::ranges::sort(tickMs);
    const double medianTick = tickMs[tickMs.size() / 2];
    const double maxTick = tickMs.back();
    std::ostringstream report;
    report << std::fixed << std::setprecision(3)
           << "Vertex preview 500 solids | per-tick median " << medianTick << " ms, max " << maxTick
           << " ms vs D12 2.000 ms | setPreview hand-off max " << std::ranges::max(handoffMs)
           << " ms | max drain " << maxTicksForMove << " ticks/move";
    MESSAGE(report.str());
    std::cout << report.str() << '\n';
    CHECK(g.context->validation().errorCount() == 0);
}

TEST_CASE("preview geometry: tiny budget continues across ticks and revision restarts") {
    REQUIRE_GPU();
    auto file = vmf::parse("");
    REQUIRE(file);
    document::Document doc(*file);
    RenderMirror mirror(*g.context, *g.heap, g.jobs, *g.cache);
    mirror.rebuild(doc);
    mirror.setPreviewBudget(std::chrono::nanoseconds(1));
    PreviewGeometry preview;
    for (int i = 0; i < 12; ++i)
        preview.ghosts.push_back({box({0, double(i * 4), 0}, {8, double(i * 4 + 2), 8}, "test/missing"), "test/missing"});
    mirror.setPreview({}, preview, 1);
    CHECK(mirror.pendingPreviewCount() == 12); // Meshing waits for the tick's pump.
    mirror.pump();
    CHECK(mirror.pendingPreviewCount() == 11);
    auto list = mirror.buildDrawList(options());
    CHECK(list.ghosts.size() == 1);
    CHECK(list.worldSegments.size() == 11 * 24);
    CHECK(mirror.pending());
    mirror.setPreview({}, preview, 1);
    CHECK(mirror.pendingPreviewCount() == 11);
    mirror.pump();
    CHECK(mirror.pendingPreviewCount() == 10);
    list = mirror.buildDrawList(options());
    CHECK(list.ghosts.size() == 2);
    CHECK(list.worldSegments.size() == 10 * 24);
    // A new snapshot discards both the completed old buffers and the unfinished tail.
    preview.ghosts.resize(4);
    preview.ghosts[0].poly = box({0, -60, 0}, {8, -52, 8}, "test/missing");
    mirror.setPreview({}, preview, 2);
    mirror.setPreview({}, preview, 3); // Revisions between ticks share one budget.
    CHECK(mirror.pendingPreviewCount() == 4);
    mirror.pump();
    CHECK(mirror.pendingPreviewCount() == 3);
    list = mirror.buildDrawList(options());
    CHECK(list.ghosts.size() == 1);
    CHECK(list.worldSegments.size() == 3 * 24);
    CHECK(list.ghosts[0].origin.y == -56);
    for (std::size_t pending = 3; pending > 0; --pending) {
        mirror.pump();
        CHECK(mirror.pendingPreviewCount() == pending - 1);
    }
    list = mirror.buildDrawList(options());
    CHECK(list.ghosts.size() == 4);
    CHECK(list.worldSegments.empty());
    mirror.clearPreview();
    CHECK(mirror.pendingPreviewCount() == 0);
    CHECK(mirror.buildDrawList(options()).ghosts.empty());
    mirror.setPreview({}, preview, 3); // The same revision is valid after a clear.
    CHECK(mirror.pendingPreviewCount() == 4);
    mirror.clearPreview();
    CHECK(g.context->validation().errorCount() == 0);
}

TEST_CASE("preview geometry: replacement hides original even while waiting for mesh") {
    REQUIRE_GPU();
    document::Document doc(editor_test::parsed());
    const auto first = editor_test::addBox(doc, {0, -24, -24}, {32, 24, 24});
    const auto second = editor_test::addBox(doc, {0, 30, 30}, {8, 40, 40});
    RenderMirror mirror(*g.context, *g.heap, g.jobs, *g.cache);
    mirror.rebuild(doc);
    REQUIRE(mirror.settle());
    const auto committed = mirror.buildDrawList(options());
    REQUIRE(committed.meshes.size() == 2);
    mirror.setPreviewBudget(std::chrono::nanoseconds(1));
    PreviewGeometry preview;
    preview.replacements.emplace(doc.get(first)->id, box({0, -12, -12}, {32, 12, 12}, "test/missing"));
    preview.replacements.emplace(doc.get(second)->id, box({0, 44, 44}, {8, 52, 52}, "test/missing"));
    mirror.setPreview({}, preview, 1);
    mirror.pump();
    auto list = mirror.buildDrawList(options());
    CHECK(mirror.pendingPreviewCount() == 1);
    CHECK(list.meshes.size() == 1);
    CHECK(list.worldSegments.size() == 24);
    CHECK(list.meshes[0].vertices != committed.meshes[0].vertices);
    CHECK(list.meshes[0].vertices != committed.meshes[1].vertices);
    mirror.pump();
    list = mirror.buildDrawList(options());
    CHECK(list.meshes.size() == 2);
    CHECK(list.worldSegments.empty());
    mirror.clearPreview();
    list = mirror.buildDrawList(options());
    REQUIRE(list.meshes.size() == 2);
    CHECK(list.meshes[0].vertices == committed.meshes[0].vertices);
}

TEST_CASE("world overlays: partly occluded lines points triangles and Latin-1 label golden") {
    REQUIRE_GPU();
    document::Document doc(editor_test::parsed());
    editor_test::addBox(doc, {0, -24, -24}, {32, 24, 24});
    RenderMirror mirror(*g.context, *g.heap, g.jobs, *g.cache);
    mirror.rebuild(doc);
    REQUIRE(mirror.settle());
    const auto font = testing::bitmapFont();
    auto atlas = GlyphAtlas::create(*g.context, *g.heap, font);
    REQUIRE(atlas);
    g.renderer->setGlyphAtlas(*atlas);
    const auto textures = g.heap->liveTextures();
    auto list = mirror.buildDrawList(options());
    const auto base = frame(g, list);
    list.worldSegments.push_back({{64, -50, 12}, {64, 50, 12}, {1, 0, 0, 1}, 3});
    list.worldPoints.push_back({{64, 0, 0}, {0, 1, 0, 1}, 9});
    list.worldPoints.push_back({{64, -40, 0}, {0, 1, 0, 1}, 9});
    list.worldTriangles.push_back({{64, -50, -16}, {64, 50, -16}, {64, 0, -46}, {0, 0.5f, 1, 0.5f}});
    list.labels.push_back({{64, 44, 44}, "Apex \xC3\xA9", {2, 0}, {1, 1, 1, 1}});
    const auto image = frame(g, list);
    golden::check("world_overlays_label", image, *g.context);
    CHECK(channel(image, 104, 64, 1) == 255); // Visible point, full strength.
    CHECK(std::abs(channel(image, 64, 64, 1) - int(channel(base, 64, 64, 1) * 0.75 + 255 * 0.25)) <= 1);
    CHECK(channel(image, 104, 52, 0) == 255); // Visible line.
    CHECK(std::abs(channel(image, 64, 52, 0) - int(channel(base, 64, 52, 0) * 0.75 + 255 * 0.25)) <= 1);
    CHECK(g.heap->liveTextures() == textures); // Labels do not upload per frame.
    // Camera-relative subtraction must precede float narrowing for every primitive and label.
    const DVec3 far{1e12, -2e12, 3e12};
    list.view.cameraPosition += far;
    for (auto& draw : list.meshes) draw.origin += far;
    for (auto& p : list.worldSegments) { p.a += far; p.b += far; }
    for (auto& p : list.worldPoints) p.position += far;
    for (auto& p : list.worldTriangles) { p.a += far; p.b += far; p.c += far; }
    for (auto& p : list.labels) p.position += far;
    CHECK(frame(g, list).pixels == image.pixels);
}

TEST_CASE("world overlays: compile leak polyline and start marker draw, grey when stale") {
    REQUIRE_GPU();
    const auto font = testing::bitmapFont();
    auto atlas = GlyphAtlas::create(*g.context, *g.heap, font);
    REQUIRE(atlas);
    g.renderer->setGlyphAtlas(*atlas);
    editor::CompileOverlay overlay;
    overlay.leak = {{64, -40, 0}, {64, 0, 0}, {64, 0, 30}};
    auto drawn = [&](bool stale) {
        DrawList list;
        list.view = options().view;
        list.mode = options().mode;
        const auto primitives = editor::compileOverlayPrimitives(overlay, stale, true, true);
        for (const auto& p : primitives.worldSegments) list.worldSegments.push_back({p.a, p.b, p.colour, p.width});
        for (const auto& p : primitives.worldPoints) list.worldPoints.push_back({p.position, p.colour, p.size});
        for (const auto& p : primitives.labels) list.labels.push_back({p.position, p.text, Vec2(p.offset), p.colour});
        return frame(g, list);
    };
    const auto live = drawn(false);
    // Pixel column = 64 - y, row = 64 - z in this view; the first segment runs along row 64.
    auto reddest = [&](const Rgba8Image& image, unsigned x, unsigned y) {
        int best = 0;
        for (unsigned row = y - 2; row <= y + 2; ++row) best = std::max(best, channel(image, x, row, 0) - channel(image, x, row, 1));
        return best;
    };
    CHECK(reddest(live, 84, 64) > 150);
    CHECK(channel(live, 64, 40, 0) == 255); // the vertical segment up to z = 30 (row 34)
    const auto grey = drawn(true);
    CHECK(reddest(grey, 84, 64) == 0);
    int bright = 0;
    for (unsigned row = 62; row <= 66; ++row) bright = std::max(bright, channel(grey, 84, row, 0));
    CHECK(bright > 60); // still drawn
    CHECK(channel(live, 100, 100, 0) == channel(grey, 100, 100, 0)); // untouched background
}

TEST_CASE("world overlays: square points keep pixel size with distance and projection") {
    REQUIRE_GPU();
    for (const bool perspective : {false, true}) {
        DrawList list;
        list.view = options().view;
        if (perspective)
            list.view.viewProjection = reversedZPerspective(glm::radians(75.0), 1, 1) * rotationOnlyView(angleVectors({0, 0, 0}));
        list.worldPoints.push_back({{0, 0, 0}, {1, 1, 1, 1}, 8});
        const auto near = frame(g, list);
        list.worldPoints[0].position.x = 300; // Inside the ortho far plane (512 from the camera).
        CHECK(frame(g, list).pixels == near.pixels);
        unsigned bright = 0;
        for (std::size_t i = 0; i < near.pixels.size(); i += 4) bright += near.pixels[i] == 255;
        CHECK(bright == 64);
    }
}

TEST_CASE("preview geometry: material ghost and per-face replacement golden") {
    REQUIRE_GPU();
    document::Document doc(editor_test::parsed());
    const auto handle = editor_test::addBox(doc, {0, -24, -24}, {32, 24, 24});
    RenderMirror mirror(*g.context, *g.heap, g.jobs, *g.cache);
    mirror.rebuild(doc);
    REQUIRE(mirror.settle());
    PreviewGeometry preview;
    preview.replacements.emplace(doc.get(handle)->id, box({0, -8, -16}, {32, 28, 28}));
    preview.ghosts.push_back({box({40, -52, -32}, {64, -4, 16}), "test/ghost", 0.35f});
    mirror.setPreview({}, preview, 1);
    REQUIRE(mirror.settle());
    auto view = options();
    view.mode = DisplayMode::Textured;
    const auto list = mirror.buildDrawList(view);
    REQUIRE(list.meshes.size() == 1);
    REQUIRE(list.ghosts.size() == 1);
    CHECK(list.outlines.size() == 1);
    CHECK(list.ghosts[0].colour.a == doctest::Approx(0.35f));
    golden::check("preview_ghost_replacement", frame(g, list), *g.context);
}

TEST_CASE("glyph atlas: invalid byte counts and glyph rectangles fail before upload") {
    REQUIRE_GPU();
    auto font = testing::bitmapFont();
    const auto textures = g.heap->liveTextures();
    font.pixels.pop_back();
    CHECK_FALSE(GlyphAtlas::create(*g.context, *g.heap, font));
    font = testing::bitmapFont();
    font.glyphs['A'].rect[0] = font.width;
    CHECK_FALSE(GlyphAtlas::create(*g.context, *g.heap, font));
    CHECK(g.heap->liveTextures() == textures);
}
