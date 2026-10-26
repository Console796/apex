#include "../assets/fixture.hpp"
#include "golden_image.hpp"
#include "shared_context.hpp"
#include "render/render_mirror.hpp"
#include "render/view_math.hpp"
#include "ui/model_pick.hpp"
#include "editor/editor_session.hpp"

#include <doctest/doctest.h>
#include <vtfpp/VTF.h>

#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>

using namespace apex;
using namespace apex::render;

namespace {
// Minimal v48 files, including a second choice beyond the uint16 vertex boundary.
struct Bytes {
    std::vector<std::byte> data;
    explicit Bytes(std::size_t size) : data(size) {}
    template<class T> void put(std::size_t offset, T value) { std::memcpy(data.data() + offset, &value, sizeof(value)); }
    void text(std::size_t offset, std::string_view value) { std::memcpy(data.data() + offset, value.data(), value.size()); }
    std::string_view view() const { return {reinterpret_cast<const char*>(data.data()), data.size()}; }
};
constexpr std::uint32_t secondVertex = 65544;
void putModel(GameTree& tree, std::string name = "fixture") {
    Bytes mdl(2048), vtx(512), vvd(64 + (secondVertex + 4) * 64);
    mdl.text(0, "IDST"); mdl.put(4, 48); mdl.put(76, 2048);
    mdl.put(204, 2); mdl.put(208, 512);
    mdl.put(212, 1); mdl.put(216, 640); mdl.put(640, 700); mdl.text(700, "test/");
    mdl.put(220, 1); mdl.put(224, 2); mdl.put(228, 648);
    mdl.put<std::int16_t>(648, 0); mdl.put<std::int16_t>(650, 1);
    mdl.put(512, 208); mdl.text(720, "red");
    mdl.put(576, 160); mdl.text(736, "green");
    mdl.put(232, 1); mdl.put(236, 768);
    mdl.put(768, 16); mdl.put(772, 2); mdl.put(776, 1); mdl.put(780, 32); mdl.text(784, "body");
    for (int choice = 0; choice < 2; ++choice) {
        const int model = 800 + choice * 148, mesh = 1120 + choice * 116;
        mdl.text(model, choice ? "centre" : "rails");
        mdl.put(model + 72, 1); mdl.put(model + 76, mesh - model);
        mdl.put(model + 80, choice ? 4 : static_cast<int>(secondVertex));
        mdl.put(model + 84, choice ? static_cast<int>(secondVertex * 48) : 0);
        mdl.put(mesh + 8, choice ? 4 : 8);
    }
    vtx.put(0, 7); vtx.put(20, 1); vtx.put(28, 1); vtx.put(32, 36);
    vtx.put(36, 2); vtx.put(40, 8);
    for (int choice = 0; choice < 2; ++choice) {
        const int model = 44 + choice * 8, lod = 60 + choice * 12;
        const int mesh = 84 + choice * 9, group = 104 + choice * 160;
        const int vertices = choice ? 4 : 8, indices = choice ? 6 : 12;
        vtx.put(model, 1); vtx.put(model + 4, lod - model);
        vtx.put(lod, 1); vtx.put(lod + 4, mesh - lod);
        vtx.put(mesh, 1); vtx.put(mesh + 4, group - mesh);
        vtx.put(group, vertices); vtx.put(group + 4, 25);
        vtx.put(group + 8, indices); vtx.put(group + 12, 97);
        vtx.put(group + 16, 1); vtx.put(group + 20, 121);
        for (int i = 0; i < vertices; ++i) vtx.put<std::uint16_t>(group + 25 + i * 9 + 4, static_cast<std::uint16_t>(i));
        const std::uint16_t order[] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
        for (int i = 0; i < indices; ++i) vtx.put(group + 97 + i * 2, order[i]);
        vtx.put(group + 121, indices); vtx.put(group + 129, vertices);
        vtx.put<std::uint8_t>(group + 139, 1); // trilist
    }
    vvd.text(0, "IDSV"); vvd.put(4, 4); vvd.put(12, 1);
    vvd.put(16, secondVertex + 4); vvd.put(56, 64); vvd.put(60, 64 + (secondVertex + 4) * 48);
    const auto quad = [&](std::uint32_t first, float centre) {
        const Vec3 points[] = {{centre - 4, -10, 0}, {centre + 4, -10, 0},
                               {centre + 4, 10, 0}, {centre - 4, 10, 0}};
        for (int i = 0; i < 4; ++i) {
            const auto base = 64 + (first + static_cast<std::uint32_t>(i)) * 48;
            vvd.put(base + 16, points[i]); vvd.put(base + 28, Vec3(0, 0, 1));
        }
    };
    quad(0, -12); quad(4, 12); quad(secondVertex, 0);
    tree.put("models/" + name + ".mdl", mdl.view());
    tree.put("models/" + name + ".dx90.vtx", vtx.view());
    tree.put("models/" + name + ".vvd", vvd.view());
}

struct Fixture {
    GameTree tree;
    JobPool jobs{2};
    std::unique_ptr<assets::Vfs> vfs;
    std::unique_ptr<assets::ModelCache> models;
    std::unique_ptr<assets::MaterialCache> materials;
    Fixture() {
        tree.put("gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
        putModel(tree);
        for (const auto& [name, colour] : {std::pair{"red", std::array<std::byte, 4>{std::byte{255}, {}, {}, std::byte{255}}},
                                         std::pair{"green", std::array<std::byte, 4>{std::byte{0}, std::byte{255}, std::byte{0}, std::byte{255}}}}) {
            tree.put(std::string("materials/test/") + name + ".vmt", std::string("VertexLitGeneric { $basetexture test/") + name + " }");
            vtfpp::VTF::CreationOptions options;
            options.version = 6;
            options.outputFormat = vtfpp::ImageFormat::RGBA8888;
            options.computeThumbnail = false;
            options.computeReflectivity = false;
            std::vector<std::byte> pixels;
            for (int i = 0; i < 16 * 16; ++i) pixels.insert(pixels.end(), colour.begin(), colour.end());
            auto texture = vtfpp::VTF::create(pixels, vtfpp::ImageFormat::RGBA8888, 16, 16, options);
            REQUIRE(texture.bake(tree.root / (std::string("materials/test/") + name + ".vtf")));
        }
        assets::GameProfile profile; profile.gameDirectory = tree.root;
        auto loaded = assets::Vfs::load(profile, jobs); REQUIRE(loaded);
        vfs = std::make_unique<assets::Vfs>(std::move(*loaded));
        models = std::make_unique<assets::ModelCache>(*vfs, jobs);
        materials = std::make_unique<assets::MaterialCache>(*vfs, jobs);
    }
};
std::unique_ptr<document::Document> map(std::string keys) {
    auto parsed = vmf::parse("entity { id 1 classname prop_static " + keys + " }"); REQUIRE(parsed);
    return std::make_unique<document::Document>(*parsed);
}
MirrorViewOptions view() {
    MirrorViewOptions options;
    options.view.cameraPosition = {0, 0, 100};
    options.view.viewProjection = reversedZOrtho(32, 32, 1, 200) * rotationOnlyView({0, 0, -1}, {1, 0, 0}, {0, 1, 0});
    return options;
}
Rgba8Image draw(Context& context, TextureHeap& heap, const DrawList& list) {
    auto renderer = Renderer::create(context, heap, RenderTarget::kColorFormat, RenderTarget::kDepthFormat); REQUIRE(renderer);
    auto target = RenderTarget::create(context, 128, 128); REQUIRE(target);
    const float clear[] = {0, 0, 0, 1};
    REQUIRE(context.oneShot([&](VkCommandBuffer commands) {
        target->begin(commands, clear); (*renderer)->record(commands, list); target->end(commands);
    }));
    auto image = target->readback(); REQUIRE(image);
    return std::move(*image);
}
int channel(const Rgba8Image& image, int x, int y, int c) { return image.pixels[static_cast<std::size_t>((y * 128 + x) * 4 + c)]; }
}

TEST_CASE("model mirror skins bodygroups loading uint32 and goldens") {
    auto* context = testing::sharedContext();
    if (!context) { MESSAGE("Skipped: no Vulkan device"); return; }
    std::cout << "[model render tests] GPU " << context->info().name << '\n';
    Fixture fixture;
    auto heap = TextureHeap::create(*context); REQUIRE(heap);
    for (int body = 0; body < 2; ++body) for (int skin = 0; skin < 2; ++skin) {
        auto doc = map("model models/fixture.mdl body " + std::to_string(body) + " skin " + std::to_string(skin));
        RenderMirror mirror(*context, **heap, fixture.jobs, *fixture.materials, nullptr, fixture.models.get(),
                            [&](const std::string& path) { return fixture.vfs->exists(path); });
        mirror.rebuild(*doc);
        if (body == 0 && skin == 0) {
            CHECK(mirror.pending());
            CHECK(mirror.buildDrawList(view()).boxes.size() == 1);
        }
        REQUIRE(mirror.settle());
        const auto loadedModel = fixture.models->request("models/fixture.mdl"); REQUIRE(loadedModel);
        REQUIRE(loadedModel->materialNames == std::vector<std::string>{"red", "green"});
        REQUIRE(loadedModel->materialDirectories == std::vector<std::string>{"test/"});
        REQUIRE(loadedModel->skins == std::vector<std::vector<std::int16_t>>{{0}, {1}});
        auto list = mirror.buildDrawList(view());
        CHECK(list.boxes.empty()); REQUIRE(list.meshes.size() == 1);
        CHECK(list.meshes[0].instancedTransforms);
        CHECK(assets::modelDrawRanges(*loadedModel, static_cast<std::uint32_t>(body), static_cast<std::uint32_t>(skin)).front().material == skin);
        CHECK(list.meshes[0].texture != kCheckerboardSlot);
        CHECK(list.meshes[0].colour == Vec4(1));
        CHECK(fixture.vfs->exists("materials/test/red.vmt"));
        CHECK(fixture.materials->state("test/red") == assets::MaterialState::Ready);
        auto image = draw(*context, **heap, list);
        const int x = body ? 64 : 40;
        CHECK(channel(image, x, 64, skin) > 150);
        CHECK(channel(image, x, 64, 1 - skin) == 0);
        CHECK(channel(image, body ? 40 : 64, 64, skin) == 0);
        golden::check("model_skin" + std::to_string(skin) + "_body" + std::to_string(body), image, *context);
        if (body == 1) {
            auto model = fixture.models->request("models/fixture.mdl"); REQUIRE(model);
            CHECK(model->indices.back() > 65535);
        }
        Highlight highlight; highlight.selected = {1}; mirror.setHighlight(highlight);
        auto selected = mirror.buildDrawList(view());
        CHECK(selected.meshes.front().highlight == highlight.selectTint);
        if (body == 1 && skin == 0) {
            const auto tinted = draw(*context, **heap, selected);
            CHECK(channel(tinted, 64, 64, 1) > channel(image, 64, 64, 1) + 20);
            highlight.selected.clear(); highlight.hover = 1; mirror.setHighlight(highlight);
            auto hovered = draw(*context, **heap, mirror.buildDrawList(view()));
            CHECK(channel(hovered, 64, 64, 2) > channel(image, 64, 64, 2) + 20);
        }
    }
    CHECK(context->validation().errorCount() == 0);
}

TEST_CASE("model adapter gap bodygroup and missing model picks") {
    Fixture fixture;
    auto doc = map("model models/fixture.mdl body 0");
    ui::ModelPickCache picks(*fixture.models);
    editor::PickIndex index(*doc, [&](const kv1::Node& block) { return picks.shape(block); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (fixture.models->state("models/fixture.mdl") == assets::ModelState::Loading &&
           std::chrono::steady_clock::now() < deadline) fixture.jobs.drainCompletions();
    REQUIRE(fixture.models->state("models/fixture.mdl") == assets::ModelState::Ready);
    REQUIRE(picks.pump()); index.rebuild();
    CHECK_FALSE(index.first({{0, 0, 30}, {0, 0, -1}}));
    CHECK(index.first({{-12, 0, 30}, {0, 0, -1}})->modelTriangle);
    auto centre = map("model models/fixture.mdl body 1");
    editor::PickIndex centreIndex(*centre, [&](const kv1::Node& block) { return picks.shape(block); });
    CHECK(centreIndex.first({{0, 0, 30}, {0, 0, -1}})->modelTriangle);
    CHECK_FALSE(centreIndex.first({{-12, 0, 30}, {0, 0, -1}}));
    auto missing = map("model models/missing.mdl");
    editor::PickIndex missingIndex(*missing, [&](const kv1::Node& block) { return picks.shape(block); });
    CHECK(missingIndex.first({{0, 0, 30}, {0, 0, -1}})->modelTriangle);
}

TEST_CASE("model mirror mixed rotation scale preview and shared instances") {
    auto* context = testing::sharedContext();
    if (!context) { MESSAGE("Skipped: no Vulkan device"); return; }
    Fixture fixture;
    auto heap = TextureHeap::create(*context); REQUIRE(heap);
    auto doc = map("model models/fixture.mdl body 1 skin 0 origin \"1000000 2000000 0\" angles \"23 37 19\" uniformscale 1.7");
    RenderMirror mirror(*context, **heap, fixture.jobs, *fixture.materials, nullptr, fixture.models.get());
    mirror.rebuild(*doc); REQUIRE(mirror.settle());
    auto options = view(); options.view.cameraPosition += DVec3(1000000, 2000000, 0);
    auto list = mirror.buildDrawList(options);
    REQUIRE(list.meshes.size() == 1); REQUIRE(list.previews.size() == 1);
    const auto handle = doc->handle(1); REQUIRE(handle);
    const auto* entity = doc->get(*handle); REQUIRE(entity);
    const auto transform = document::entityTransform(document::entityPose(*entity->block));
    CHECK(list.previews.front().matrix == transform);
    auto image = draw(*context, **heap, list);
    // Compare every covered pixel to the projected convex quad, allowing one edge pixel.
    const auto model = fixture.models->request("models/fixture.mdl"); REQUIRE(model);
    std::vector<Vec2> projected;
    for (std::uint32_t i = secondVertex; i < secondVertex + 4; ++i) {
        const auto world = DVec3(transform * DVec4(model->vertices[i].position, 1));
        const Vec4 clip = options.view.viewProjection * Vec4(Vec3(world - options.view.cameraPosition), 1);
        projected.push_back((Vec2(clip) / clip.w + Vec2(1)) * 64.0f);
    }
    int covered = 0;
    for (int y = 0; y < 128; ++y) for (int x = 0; x < 128; ++x) {
        if (channel(image, x, y, 0) == 0) continue;
        ++covered;
        for (std::size_t i = 0; i < 4; ++i) {
            const auto a = projected[i], b = projected[(i + 1) % 4];
            const auto edge = b - a, point = Vec2(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f) - a;
            CHECK((edge.x * point.y - edge.y * point.x) / glm::length(edge) < 1.1f);
        }
    }
    CHECK(covered > 500);
    mirror.setPreview({{1, glm::translate(DMat4(1), DVec3(4, 0, 0))}});
    auto preview = mirror.buildDrawList(options);
    CHECK(preview.previews.front().matrix == glm::translate(DMat4(1), DVec3(4, 0, 0)) * transform);
    CHECK(context->validation().errorCount() == 0);
}

TEST_CASE("model mirror missing tint and clear replaces missing model") {
    auto* context = testing::sharedContext();
    if (!context) { MESSAGE("Skipped: no Vulkan device"); return; }
    Fixture fixture;
    auto heap = TextureHeap::create(*context, {4, 4u << 20}); REQUIRE(heap);
    auto doc = map("model models/added.mdl body 1");
    assets::EntityClassTable classes;
    classes.classes["prop_static"].color = {30, 220, 70};
    RenderMirror mirror(*context, **heap, fixture.jobs, *fixture.materials, &classes, fixture.models.get());
    mirror.rebuild(*doc); REQUIRE(mirror.settle());
    auto missing = mirror.buildDrawList(view()); REQUIRE(missing.meshes.size() == 1);
    CHECK(missing.boxes.empty()); CHECK(missing.meshes.front().colour == Vec4(30.0f / 255, 220.0f / 255, 70.0f / 255, 1));
    auto errorImage = draw(*context, **heap, missing);
    CHECK(channel(errorImage, 64, 64, 1) > 70);
    CHECK(channel(errorImage, 64, 64, 1) > channel(errorImage, 64, 64, 0) * 3);
    GameTree addon;
    putModel(addon, "added");
    fixture.tree.put("gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game \"" + addon.root.string() + "\" game |gameinfo_path|. } } }");
    assets::GameProfile profile; profile.gameDirectory = fixture.tree.root;
    auto mounted = assets::Vfs::load(profile, fixture.jobs); REQUIRE(mounted);
    *fixture.vfs = std::move(*mounted);
    fixture.models->clear(); mirror.refreshModels(); mirror.pump();
    REQUIRE(mirror.settle());
    auto real = mirror.buildDrawList(view()); REQUIRE(real.meshes.size() == 1);
    CHECK(fixture.models->state("models/added.mdl") == assets::ModelState::Ready);
    CHECK(real.meshes.front().indexCount == 6);
    CHECK(draw(*context, **heap, real).pixels != errorImage.pixels);
    for (int i = 0; i < 5; ++i) {
        fixture.models->clear(); mirror.refreshModels(); REQUIRE(mirror.settle());
        auto refreshed = mirror.buildDrawList(view());
        REQUIRE(refreshed.meshes.size() == 1);
        CHECK(refreshed.meshes.front().texture != kCheckerboardSlot);
        CHECK((*heap)->liveTextures() == 3); // Checker plus the two skin materials.
    }
    CHECK(context->validation().errorCount() == 0);
}

TEST_CASE("model mirror instances share geometry through edits and display modes") {
    auto* context = testing::sharedContext();
    if (!context) { MESSAGE("Skipped: no Vulkan device"); return; }
    Fixture fixture;
    auto heap = TextureHeap::create(*context); REQUIRE(heap);
    auto parsed = vmf::parse(R"(entity { id 1 classname prop_static model models/fixture.mdl body 1 origin "-12 0 0" }
                               entity { id 2 classname prop_static model models/fixture.mdl body 1 origin "12 0 0" }
                               entity { id 3 classname light origin "24 24 0" })");
    REQUIRE(parsed); document::Document doc(*parsed);
    RenderMirror mirror(*context, **heap, fixture.jobs, *fixture.materials, nullptr, fixture.models.get());
    mirror.rebuild(doc); REQUIRE(mirror.settle());
    auto list = mirror.buildDrawList(view());
    REQUIRE(list.meshes.size() == 1); CHECK(list.meshes.front().instanceCount == 2);
    CHECK(list.boxes.size() == 1);
    const auto vertices = list.meshes.front().vertices->handle();
    auto image = draw(*context, **heap, list);
    CHECK(channel(image, 40, 64, 0) > 150); CHECK(channel(image, 88, 64, 0) > 150);
    auto tx = doc.begin("move light"); tx.setKeyValue(*doc.handle(3), "origin", "24 20 0");
    mirror.apply(doc, tx.commit()); REQUIRE(mirror.settle());
    CHECK(mirror.buildDrawList(view()).meshes.front().vertices->handle() == vertices);
    for (const auto mode : {DisplayMode::Flat, DisplayMode::Wireframe}) {
        auto options = view(); options.mode = mode;
        auto frame = draw(*context, **heap, mirror.buildDrawList(options));
        std::size_t visible = 0;
        for (std::size_t i = 0; i < frame.pixels.size(); i += 4) visible += frame.pixels[i] > 0;
        CHECK(visible > 50);
    }
    auto edit = doc.begin("skin and body"); edit.setKeyValue(*doc.handle(1), "skin", "1");
    edit.setKeyValue(*doc.handle(1), "body", "0"); mirror.apply(doc, edit.commit()); REQUIRE(mirror.settle());
    auto changed = mirror.buildDrawList(view());
    REQUIRE(changed.meshes.size() == 2);
    CHECK(changed.meshes[0].vertices->handle() == vertices);
    CHECK(changed.meshes[1].vertices->handle() == vertices);
    CHECK(context->validation().errorCount() == 0);
}

TEST_CASE("model adapter reorders interleaved choices and session refresh keeps fallback") {
    Fixture fixture;
    auto doc = map("model models/fixture.mdl body 0");
    ui::ModelPickCache picks(*fixture.models);
    editor::EditorSession session;
    assets::EntityClassTable classes;
    auto& cls = classes.classes["prop_static"]; cls.mins = {-20, -20, -20}; cls.maxs = {20, 20, 20};
    session.setEntityClasses(std::move(classes));
    session.setEntityBoundsProvider([&](const kv1::Node& block) { return picks.shape(block); });
    session.attach(*doc);
    REQUIRE(session.picks().first({{0, 0, 30}, {0, 0, -1}}));
    CHECK_FALSE(session.picks().first({{0, 0, 30}, {0, 0, -1}})->modelTriangle);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (fixture.models->state("models/fixture.mdl") == assets::ModelState::Loading &&
           std::chrono::steady_clock::now() < deadline) fixture.jobs.drainCompletions();
    REQUIRE(picks.pump()); session.refreshEntityBounds();
    CHECK_FALSE(session.picks().first({{0, 0, 30}, {0, 0, -1}}));
    auto data = *fixture.models->request("models/fixture.mdl");
    data.submeshes = {{0, 6, 0, 0, 0, 0}, {12, 6, 0, 1, 0, 0}, {6, 6, 0, 0, 1, 0}};
    const auto pick = ui::makePickModel(data);
    for (std::uint32_t body = 0; body < 2; ++body) {
        editor::PickIndex index(*doc, [&](const kv1::Node&) {
            return editor::EntityPickShape{pick, body, {}};
        });
        CHECK(index.first({{-12, 0, 30}, {0, 0, -1}}).has_value() == (body == 0));
        CHECK(index.first({{12, 0, 30}, {0, 0, -1}}).has_value() == (body == 0));
        CHECK(index.first({{0, 0, 30}, {0, 0, -1}}).has_value() == (body == 1));
    }
    session.setEntityBoundsProvider({});
    REQUIRE(session.picks().first({{0, 0, 30}, {0, 0, -1}}));
    CHECK_FALSE(session.picks().first({{0, 0, 30}, {0, 0, -1}})->modelTriangle);
}
