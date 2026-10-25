#include "fixtures.hpp"
#include "editor/editor_session.hpp"
#include "editor/face_edit.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

using namespace editor_test;

namespace {
// D17's chamber list is data. Coordinates describe the clear 512 x 512 x 256 room;
// an interior partition makes an exit vestibule without opening the outside shell.
constexpr const char* white = "tile/white_wall_tile003a";
constexpr const char* black = "tile/black_wall_metal_002a";
constexpr const char* reveal = "tile/white_wall_tile003b";
struct BlockSpec { const char* name; DVec3 low, high; const char* material; };
const std::array shell{
    BlockSpec{"floor", {-16,-16,-16}, {528,528,0}, white},
    BlockSpec{"west", {-16,-16,0}, {0,528,256}, white},
    BlockSpec{"east", {512,-16,0}, {528,528,256}, white},
    BlockSpec{"south", {0,-16,0}, {512,0,256}, white},
    BlockSpec{"north", {0,512,0}, {512,528,256}, white},
    BlockSpec{"ceiling", {-16,-16,256}, {528,528,272}, black},
};
const BlockSpec partition{"exit partition", {0,384,0}, {512,400,256}, white};
const BlockSpec doorway{"doorway carver", {208,376,0}, {304,408,128}, black};
const BlockSpec panel{"clipped panel", {32,256,0}, {128,320,96}, white};
const BlockSpec slope{"vertex slope", {352,256,0}, {448,320,64}, white};
struct PrimitiveSpec { const char* name; int type; SurfacePlane plane; DVec2 end; int height; int count; };
const std::array primitives{
    PrimitiveSpec{"ramp", 0, {{32,64,0},{0,0,1},{1,0,0},{0,1,0},{}}, {128,128}, 64, 1},
    PrimitiveSpec{"pillar", 1, {{400,64,0},{0,0,1},{1,0,0},{0,1,0},{}}, {48,48}, 192, 1},
    // Center the upright semicircle on the floor; its inner span frames the doorway.
    PrimitiveSpec{"exit arch", 3, {{192,384,-128},{0,-1,0},{1,0,0},{0,0,1},{}}, {128,256}, 16, 8},
};
const std::array entityNames{"info_player_start", "light"};
constexpr int carveFragments = 3;
constexpr int expectedSolids = 21; // 6 shell + 3 partition + panel + slope + 1 + 1 + 8

ToolView at(DVec3 point, DVec3 direction = {0,0,-1}, double distance = 128) {
    ToolView view;
    view.cursorRay = {point - direction * distance, direction};
    view.cameraPosition = view.cursorRay.origin;
    view.cameraForward = direction;
    view.cameraUp = std::abs(direction.z) > 0.9 ? DVec3{0,1,0} : DVec3{0,0,1};
    view.size = {800,600};
    return view; // Perspective throughout, including typed-height and modal inputs.
}
InputEvent event(InputKind kind) {
    InputEvent e;
    e.cursor = {400,300};
    e.kind = kind; e.source = 1; e.button = PointerButton::Left;
    e.left = kind == InputKind::Press;
    return e;
}
void key(EditorSession& session, std::string name, const ToolView& view = at({256,256,128})) {
    auto e = event(InputKind::KeyDown); e.key = std::move(name);
    REQUIRE(session.input(e, view));
}
void click(EditorSession& session, ToolView view, bool right = false) {
    auto e = event(InputKind::Press);
    if (right) { e.button = PointerButton::Right; e.left = false; e.right = true; }
    session.input(e, view);
    e.kind = InputKind::Release; e.left = false;
    session.input(e, view);
}
void draw(EditorSession& session, SurfacePlane plane, DVec2 end, int height) {
    // Pin a real authored face with the tool's plane-pick gesture. The first
    // floor starts on the empty document's world grid and extrudes downward.
    bool pinned = false;
    for (const auto h : nodes(*session.document())) {
        if (!std::holds_alternative<Handle<Solid>>(h)) continue;
        const auto solid = geometry::solidGeometry(*session.document()->get(h)->block);
        REQUIRE(solid);
        for (const auto& face : solid->poly.faces) {
            if (glm::length(face.plane.normal-plane.normal) > 1e-6 ||
                std::abs(face.plane.dist-glm::dot(plane.origin, plane.normal)) > 1e-6) continue;
            DVec3 center{0};
            for (const auto i : face.vertices) center += solid->poly.vertices[i];
            center /= static_cast<double>(face.vertices.size());
            auto pick = event(InputKind::Press); pick.ctrl = true;
            REQUIRE(session.input(pick, at(center, -plane.normal, 2)));
            pinned = true;
            break;
        }
        if (pinned) break;
    }
    const auto all = nodes(*session.document());
    REQUIRE((pinned || std::ranges::none_of(all, [](const auto h) {
        return std::holds_alternative<Handle<Solid>>(h);
    })));
    const auto start = at(plane.origin, -plane.normal);
    const auto finish = at(plane.origin + plane.uAxis * end.x + plane.vAxis * end.y, -plane.normal);
    REQUIRE(session.input(event(InputKind::Press), start));
    REQUIRE(session.input(event(InputKind::Release), finish));
    for (const char c : std::to_string(height)) key(session, std::string(1,c));
    key(session, "Enter");
    auto unpin = event(InputKind::Press); unpin.ctrl = true;
    REQUIRE(session.input(unpin, at({2048,2048,2048}, {0,0,1})));
    CHECK_FALSE(session.workPlane().pinned());
}
NodeId block(EditorSession& session, const BlockSpec& spec) {
    INFO(std::string(spec.name));
    REQUIRE(session.perform("tool.block"));
    session.setNewSolidMaterial(spec.material); // Material browser's active-material callback.
    const bool floor = spec.high.z == 0;
    auto base = spec.low;
    if (floor) base.z = 0;
    draw(session, {base,{0,0,1},{1,0,0},{0,1,0},{}},
         {spec.high.x-spec.low.x, spec.high.y-spec.low.y},
         static_cast<int>(floor ? spec.low.z : spec.high.z-spec.low.z));
    const auto selected = selectedNodes(session.selection());
    REQUIRE(selected.size() == 1);
    const auto solid = geometry::solidGeometry(*session.document()->get(*session.document()->handle(selected[0]))->block);
    REQUIRE(solid);
    checkPoint(solid->poly.bounds().min, spec.low);
    checkPoint(solid->poly.bounds().max, spec.high);
    for (const auto& face : solid->poly.faces) CHECK(face.material == spec.material);
    return selected[0];
}
std::vector<geometry::ConvexPolyhedron> polys(const Document& doc) {
    std::vector<geometry::ConvexPolyhedron> result;
    for (const auto h : nodes(doc)) if (std::holds_alternative<Handle<Solid>>(h)) {
        auto solid = geometry::solidGeometry(*doc.get(h)->block);
        REQUIRE(solid);
        result.push_back(std::move(solid->poly));
    }
    return result;
}
using Identity = std::map<std::string, std::vector<std::pair<std::string,std::string>>>;
Identity identity(const Document& doc) {
    Identity result;
    for (const auto h : nodes(doc)) {
        const auto& b = *doc.get(h)->block;
        const auto* id = b.find("id");
        if (!id) continue;
        auto& sides = result[std::string(b.key()) + ":" + std::string(id->decodedValue().value())];
        for (const auto* side : b.findAll("side"))
            sides.emplace_back(side->find("id")->decodedValue().value(), side->find("plane")->decodedValue().value());
    }
    return result;
}
}

TEST_CASE("[chamber] D17 scripted chamber build") {
    // File > New's empty map. No fixture geometry or document transactions
    // participate in the replay.
    Document doc{vmf::emptyMap()};
    EditorSession session;
    session.attach(doc);
    while (session.gridSize() > 8) REQUIRE(session.perform("grid.smaller"));
    CHECK(session.gridSize() == 8);
    for (const auto& spec : shell) block(session, spec);
    block(session, partition);
    block(session, doorway); // The block tool selects its new solid: it is the carver.
    REQUIRE(session.perform("edit.carve"));
    CHECK(polys(doc).size() == shell.size() + carveFragments + 1);
    REQUIRE(session.perform("edit.delete")); // Remove the still-selected cutting block.
    CHECK(polys(doc).size() == shell.size() + carveFragments);
    int fragmentCount = 0;
    double partitionVolume = 0;
    for (const auto& solid : polys(doc)) {
        const auto bounds = solid.bounds();
        if (std::abs(bounds.min.y-partition.low.y) < 1e-6 &&
            std::abs(bounds.max.y-partition.high.y) < 1e-6) {
            ++fragmentCount;
            partitionVolume += solid.volume();
        }
    }
    CHECK(fragmentCount == carveFragments);
    CHECK(partitionVolume == doctest::Approx(512.0*16*256 - 96.0*16*128));

    REQUIRE(session.perform("tool.apply"));
    session.setNewSolidMaterial(reveal);
    // Cast from inside the doorway onto its two jambs and lintel.
    for (const auto& [point, direction] : std::array{
             std::pair{DVec3{208,392,64}, DVec3{-1,0,0}},
             std::pair{DVec3{304,392,64}, DVec3{1,0,0}},
             std::pair{DVec3{256,392,128}, DVec3{0,0,1}}}) {
        const auto view = at(point, direction, 32);
        const auto hit = session.picks().first(view.cursorRay);
        REQUIRE(hit); REQUIRE(hit->side);
        click(session, view, true);
        const auto face = doc.side(*hit->side);
        REQUIRE(face);
        CHECK(faceSnapshot(doc, *hit->side)->face.material == reveal);
    }

    const auto panelId = block(session, panel);
    REQUIRE(session.perform("tool.clip"));
    // Three actual surface clicks define x + z = 128 on the panel.
    click(session, at({32,272,96}));
    click(session, at({32,304,96}));
    click(session, at({128,288,0}, {-1,0,0}, 32));
    key(session, "Enter");
    REQUIRE(doc.handle(panelId));
    const auto clipped = geometry::solidGeometry(*doc.get(*doc.handle(panelId))->block);
    REQUIRE(clipped);
    CHECK(clipped->poly.faces.size() == 5);
    CHECK(clipped->poly.volume() == doctest::Approx(96.0*64*96/2));
    CHECK(std::ranges::any_of(clipped->poly.faces, [](const auto& face) {
        return std::abs(face.plane.normal.x) > 0.5 && std::abs(face.plane.normal.z) > 0.5;
    }));

    const auto slopeId = block(session, slope);
    session.setPreset(blenderPreset());
    REQUIRE(session.perform("tool.vertex"));
    // Move one top corner upward with the normal modal grab/axis/typed-distance keys.
    const auto vertexView = at({448,320,64}, glm::normalize(DVec3{-1,-1,-1}), 96);
    click(session, vertexView);
    auto grab = event(InputKind::KeyDown); grab.key = "G"; grab.chord = KeyChord{"G"};
    REQUIRE(session.input(grab, vertexView));
    key(session, "Z"); key(session, "3"); key(session, "2"); key(session, "Enter");
    const auto sloped = geometry::solidGeometry(*doc.get(*doc.handle(slopeId))->block);
    REQUIRE(sloped);
    CHECK(sloped->poly.bounds().max.z == doctest::Approx(96));
    // Match alignment through Apply's normal modifier click, using the floor's
    // default axes as the source. This realigns changed normals after topology edits.
    session.setPreset(hammerPreset());
    REQUIRE(session.perform("tool.apply"));
    REQUIRE(session.perform("face.extrapolation.cycle"));
    REQUIRE(session.perform("face.extrapolation.cycle"));
    session.setNewSolidMaterial(white);
    click(session, at({256,256,0}));
    for (const auto node : {panelId, slopeId}) {
        const auto solid = geometry::solidGeometry(*doc.get(*doc.handle(node))->block);
        REQUIRE(solid);
        for (const auto& face : solid->poly.faces) {
            DVec3 center{0};
            for (auto i : face.vertices) center += solid->poly.vertices[i];
            center /= static_cast<double>(face.vertices.size());
            auto apply = event(InputKind::Press);
            apply.button = PointerButton::Right; apply.left = false; apply.right = true; apply.alt = true;
            REQUIRE(session.input(apply, at(center, -face.plane.normal, 2)));
        }
    }

    for (const auto& spec : primitives) {
        INFO(std::string(spec.name));
        REQUIRE(session.perform("tool.block")); // End adjust-last before choosing the next type.
        REQUIRE(session.perform("tool.primitive"));
        session.setNewSolidMaterial(white);
        REQUIRE(session.activeToolObject().setParameter("type", spec.type));
        const auto before = polys(doc).size();
        draw(session, spec.plane, spec.end, spec.height);
        CHECK(polys(doc).size() == before + static_cast<std::size_t>(spec.count));
    }

    // Minimal FGD metadata, as supplied by the content loader, with no game dependency.
    assets::EntityClassTable classes;
    for (const auto* name : entityNames) {
        assets::EntityClass cls; cls.name = name; cls.type = "PointClass";
        cls.mins = {-8,-8,-8}; cls.maxs = {8,8,8};
        classes.classes[name] = std::move(cls);
    }
    session.setEntityClasses(std::move(classes));
    REQUIRE(session.perform("tool.entity"));
    for (std::size_t i = 0; i < entityNames.size(); ++i) {
        session.setCurrentEntityClass(entityNames[i]);
        click(session, at({256,128 + static_cast<double>(i)*64.0,0}, {0,0,-1}, 96));
    }
    std::map<std::string,int> entities;
    for (const auto h : nodes(doc)) if (std::holds_alternative<Handle<Entity>>(h))
        ++entities[std::string(doc.get(h)->block->find("classname")->decodedValue().value())];
    entities.erase("worldspawn");
    CHECK(entities.size() == entityNames.size());
    for (const auto* name : entityNames) CHECK(entities[name] == 1);

    const auto solids = polys(doc);
    REQUIRE(solids.size() == expectedSolids);
    for (const auto& solid : solids) {
        CHECK(solid.validate().empty());
        for (const auto& face : solid.faces) {
            CAPTURE(face.material);
            CHECK(face.uv.scaleU == doctest::Approx(0.25).epsilon(1e-5));
            CHECK(face.uv.scaleV == doctest::Approx(0.25).epsilon(1e-5));
            CHECK(glm::length(face.uv.uaxis) == doctest::Approx(1).epsilon(1e-5));
            CHECK(glm::length(face.uv.vaxis) == doctest::Approx(1).epsilon(1e-5));
            CHECK(std::abs(glm::dot(face.uv.uaxis, face.uv.vaxis)) < 1e-5);
            CHECK(std::abs(glm::dot(face.uv.uaxis, face.plane.normal)) < 1e-5);
            CHECK(std::abs(glm::dot(face.uv.vaxis, face.plane.normal)) < 1e-5);
        }
    }
    // Exact volumetric boundary coverage, rather than a finite sample of rays: every
    // continuous path from the interior box to outside crosses this closed shell.
    // Subtract the authored solids from each of its six slabs using the hull kernel.
    for (const auto& slab : shell) {
        std::vector remaining{geometry_test::build(geometry_test::boxSides(slab.low, slab.high))};
        for (const auto& solid : solids) {
            std::vector<geometry::ConvexPolyhedron> next;
            for (const auto& piece : remaining) {
                auto fragments = geometry::subtract(piece, solid);
                next.insert(next.end(), fragments.begin(), fragments.end());
            }
            remaining = std::move(next);
        }
        CHECK(remaining.empty());
    }
    MESSAGE("D17 chamber: " << solids.size() << " solids, " << entityNames.size()
            << " entities; undo memory = " << doc.historyBytes() << " bytes");

    // The normal Qt-free serialization path used by MainWindow::saveFile.
    const auto bytes = doc.write(); REQUIRE(bytes);
    const auto directory = std::filesystem::temp_directory_path() / ("apex-chamber-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(directory));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{directory};
    const auto path = directory / "chamber.vmf";
    const auto save = [&](const std::filesystem::path& output) {
        std::ofstream stream(output, std::ios::binary); REQUIRE(stream);
        stream << *bytes; stream.close(); REQUIRE(stream.good());
    };
    save(path);
    std::ifstream input(path, std::ios::binary);
    const std::string saved{std::istreambuf_iterator<char>(input), {}};
    Document reopened{parsed(saved)};
    CHECK(identity(reopened) == identity(doc));
    CHECK(polys(reopened).size() == solids.size());
    const auto vmf = reopened.toVmf();
    REQUIRE(vmf); REQUIRE(vmf->world);
    CHECK(vmf->world->solids.size() == solids.size());
    for (const auto& solid : polys(reopened)) CHECK(solid.validate().empty());
    if (const auto* output = std::getenv("APEX_CHAMBER_OUT")) save(output);
}
