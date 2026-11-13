// 0.8.2 (plan 0.8 D8, D1): picking, marquee, bounds and snapping use a displaced side's mesh, and
// the Problems list and the Face Properties model report displacements.
#include "fixtures.hpp"
#include "editor/face_edit.hpp"
#include "editor/group_index.hpp"
#include "editor/map_problems.hpp"
#include "editor/selection.hpp"
#include "editor/selection_ops.hpp"
#include "editor/snapper.hpp"
#include "editor/tool.hpp"

#include <fstream>
#include <sstream>

using namespace editor_test;

namespace {

constexpr SideId kSide = 355; // The displaced top of solid 637 (flat base at z = 64).

std::string fixture(const std::string& name = "test1_disp") {
    std::ifstream in(std::string(APEX_FIXTURE_DIR) + "/vmf/disp/" + name + ".vmf", std::ios::binary);
    REQUIRE(in);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

NodeId solidId(const Document& doc) {
    const auto location = doc.side(kSide);
    REQUIRE(location);
    return doc.get(location->solid)->id;
}

// Grid vertex 12 (row 1, column 3) of the fixture's VBSP oracle (test1_disp.vbsp.txt).
const DVec3 kVertex12{-24, -1608, 90.7945995};

int count(const std::vector<Problem>& problems, ProblemKind kind) {
    return static_cast<int>(std::ranges::count(problems, kind, &Problem::kind));
}

ToolView overhead(DVec3 centre) {
    ToolView view;
    view.size = {1000, 1000};
    view.cameraPosition = {centre.x, centre.y, 400.0};
    view.cameraForward = {0, 0, -1};
    view.cameraUp = {0, 1, 0};
    view.orthographic = true;
    view.orthoHeight = 200.0; // 0.2 units per pixel.
    view.fovYDegrees = 90;
    return view;
}

} // namespace

TEST_CASE("a ray hits the sculpted surface of a displacement, not its flat base face") {
    Document doc(parsed(fixture()));
    PickIndex index(doc);
    const auto solid = solidId(doc);
    const auto hit = index.first({{kVertex12.x, kVertex12.y, 500}, {0, 0, -1}});
    REQUIRE(hit);
    CHECK(hit->node == solid);
    CHECK(hit->side == kSide);
    CHECK(hit->displacement);
    CHECK(hit->point.z == doctest::Approx(kVertex12.z).epsilon(1e-6));
    CHECK(hit->normal.z > 0.0);
    // Between grid vertices the hit lies above the flat base, on the mesh.
    const auto between = index.first({{-12, -1596, 500}, {0, 0, -1}});
    REQUIRE(between);
    CHECK(between->side == kSide);
    CHECK(between->point.z > 75.0);
    // From the side, a ray above the base face hits the slope where row 5 rises through z = 70
    // (between columns 4 and 5: x = 0 at z 65.6, x = 24 at z 71.7).
    const auto low = index.first({{-500, -1512, 70}, {1, 0, 0}});
    REQUIRE(low);
    CHECK(low->side == kSide);
    CHECK(low->point.x > 0.0);
    CHECK(low->point.x < 24.0);

    // Selection by click in object, solid and face modes.
    GroupIndex groups(doc);
    const Ray down{{kVertex12.x, kVertex12.y, 500}, {0, 0, -1}};
    for (const auto mode : {SelectionMode::Objects, SelectionMode::Solids}) {
        Selection selection(mode);
        click(selection, index, groups, down, {});
        CHECK(selection.ordered() == std::vector<Target>{solid});
    }
    Selection faces(SelectionMode::Faces);
    click(faces, index, groups, down, {});
    CHECK(faces.ordered() == std::vector<Target>{kSide});
    // A ray that passes over the hill misses it entirely.
    CHECK_FALSE(index.first({{-500, -1620, 110}, {1, 0, 0}}));
}

TEST_CASE("marquee selection and selection bounds include displaced vertices") {
    Document doc(parsed(fixture()));
    PickIndex index(doc);
    const auto solid = solidId(doc);
    const auto bounds = index.bounds(solid);
    REQUIRE(bounds);
    CHECK(bounds->max.z == doctest::Approx(100.225399).epsilon(1e-6)); // Oracle vertex 6.
    CHECK(bounds->min.z == doctest::Approx(0));
    const auto sideBounds = index.bounds(solid, kSide);
    REQUIRE(sideBounds);
    CHECK(sideBounds->max.z == doctest::Approx(100.225399).epsilon(1e-6));
    CHECK(sideBounds->min.z == doctest::Approx(64));
    REQUIRE(index.displacement(solid, kSide));
    CHECK(index.displacement(solid, kSide)->mesh.positions.size() == 81);

    // Only the hill rises above z = 80: touching that slab selects the solid by its mesh.
    const auto above = planes({-200, -1700, 80}, {200, -1400, 200});
    CHECK(index.frustum(above, Mode::Touching) == std::vector<NodeId>{solid});
    // The brush's flat box is enclosed, its displaced surface is not.
    CHECK(index.frustum(planes({-100, -1640, -1}, {100, -1430, 70}), Mode::Enclosed).empty());
    CHECK(index.frustum(planes({-100, -1640, -1}, {100, -1430, 110}), Mode::Enclosed) == std::vector<NodeId>{solid});

    GroupIndex groups(doc);
    Selection selection(SelectionMode::Solids);
    marquee(selection, index, groups, above, MarqueeMode::Touching, {});
    CHECK(selection.ordered() == std::vector<Target>{solid});

    // Point placement and nearest-surface queries land on the mesh.
    const auto nearest = index.nearest({kVertex12.x, kVertex12.y, 150}, 100);
    REQUIRE(nearest.size() == 1);
    CHECK(nearest[0].point.z > 80.0); // On the hill, not the flat base at 64.
}

TEST_CASE("snapping offers displacement grid vertices and lands on the sculpted surface") {
    Document doc(parsed(fixture()));
    PickIndex index(doc);
    WorkPlaneState plane;
    SnapSettings settings;
    settings.gridSize = 16;
    const SurfacePlane ground{{0, 0, 0}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {}};
    const Snapper snapper(doc, index, plane, settings, ground);

    const auto near = DVec3{kVertex12.x + 0.6, kVertex12.y - 0.6, 0};
    const auto view = overhead(near);
    const auto vertex = snapper.snap({{near.x, near.y, 400}, {0, 0, -1}}, view);
    CHECK(vertex.kind == SnapKind::Vertex);
    CHECK(vertex.side == kSide);
    CHECK(glm::length(vertex.point - kVertex12) < 1e-6);

    const auto features = index.nearestFeature({{near.x, near.y, 400}, {0, 0, -1}}, 8.0, view);
    REQUIRE(features.vertex);
    CHECK(features.vertex->feature.kind == FeatureKind::DisplacementVertex);
    CHECK(features.vertex->feature.a == 12);

    // Away from any vertex the cursor lands on the mesh, not on the flat base's grid.
    const auto off = DVec3{-12, -1596, 0};
    const auto surface = snapper.snap({{off.x, off.y, 400}, {0, 0, -1}}, overhead(off));
    CHECK(surface.kind == SnapKind::Surface);
    CHECK(surface.side == kSide);
    CHECK(surface.point.z > 75.0);
}

TEST_CASE("the Problems list reports displacement alpha, non-quad bases and unreadable dispinfo") {
    {
        Document doc(parsed(fixture()));
        const auto problems = ProblemRegistry::builtin().run(ProblemContext{doc});
        CHECK(count(problems, ProblemKind::DisplacementAlphaRange) == 1);
        CHECK(count(problems, ProblemKind::DisplacementNonQuad) == 0);
        CHECK(count(problems, ProblemKind::DisplacementUnreadable) == 0);
        const auto alpha = std::ranges::find(problems, ProblemKind::DisplacementAlphaRange, &Problem::kind);
        REQUIRE(alpha != problems.end());
        CHECK(alpha->severity == ProblemSeverity::Warning);
        CHECK(alpha->text.find("side 355 of solid 637") != std::string::npos);
        CHECK(alpha->text.find("-96 to 72") != std::string::npos);
        CHECK(alpha->objects == std::vector<NodeId>{solidId(doc)});
        CHECK(alpha->fixId == "select");
    }
    {
        // Alphas inside 0..255 are not a problem.
        auto source = fixture();
        for (std::size_t at; (at = source.find("\"-96 -72 -48 -24 0 24 48 72 0\"")) != std::string::npos;)
            source.replace(at, 30, "\"0 32 64 96 128 160 192 224 255\"");
        Document doc(parsed(source));
        CHECK(count(ProblemRegistry::builtin().run(ProblemContext{doc}), ProblemKind::DisplacementAlphaRange) == 0);
    }
    {
        Document doc(parsed(fixture("test1_disp_nonquad")));
        const auto problems = ProblemRegistry::builtin().run(ProblemContext{doc});
        REQUIRE(count(problems, ProblemKind::DisplacementNonQuad) == 1);
        const auto found = std::ranges::find(problems, ProblemKind::DisplacementNonQuad, &Problem::kind);
        CHECK(found->severity == ProblemSeverity::Error);
        CHECK(found->text.find("Trying to create a non-quad displacement!") != std::string::npos);
        CHECK(found->objects == std::vector<NodeId>{solidId(doc)});
        CHECK(found->fixId == "select");
        CHECK(count(problems, ProblemKind::InvalidSolid) == 0);
        // The side is picked as its flat face.
        PickIndex index(doc);
        const auto hit = index.first({{-24, -1608, 500}, {0, 0, -1}});
        REQUIRE(hit);
        CHECK(hit->side == kSide);
        CHECK_FALSE(hit->displacement);
        CHECK(hit->point.z == doctest::Approx(64));
    }
    {
        Document doc(parsed(fixture("test1_disp_unreadable")));
        const auto problems = ProblemRegistry::builtin().run(ProblemContext{doc});
        REQUIRE(count(problems, ProblemKind::DisplacementUnreadable) == 1);
        const auto found = std::ranges::find(problems, ProblemKind::DisplacementUnreadable, &Problem::kind);
        CHECK(found->severity == ProblemSeverity::Error);
        CHECK(found->text.find("cannot be read") != std::string::npos);
        CHECK(found->objects == std::vector<NodeId>{solidId(doc)});
        CHECK(found->fixId == "select");
        CHECK(count(problems, ProblemKind::DisplacementAlphaRange) == 0);
    }
}

TEST_CASE("the Face Properties displacement model summarises the selected displaced faces") {
    Document doc(parsed(fixture()));
    const std::vector<SideId> flat{356};
    CHECK(summarizeDisplacements(doc, flat).displaced == 0);
    const std::vector<SideId> both{kSide, 356};
    const auto summary = summarizeDisplacements(doc, both);
    CHECK(summary.displaced == 1);
    CHECK(summary.readable == 1);
    CHECK(summary.power == 3);
    CHECK(summary.gridSize() == 9);
    CHECK(summary.flags == 0);
    CHECK(summary.elevation == 0.0);
    CHECK(summary.alphaMin == -96.0);
    CHECK(summary.alphaMax == 72.0);
    REQUIRE(summary.problems.size() == 1);
    CHECK(summary.problems[0].find("alpha out of range") != std::string::npos);

    CHECK(displacementFlagNames(0).empty());
    CHECK(displacementFlagNames(2 | 4 | 8) == std::vector<std::string>{"NoPhys", "NoHull", "NoRay"});
    CHECK(displacementFlagNames(1 | 8) == std::vector<std::string>{"0x1", "NoRay"});

    Document unreadable(parsed(fixture("test1_disp_unreadable")));
    const std::vector<SideId> one{kSide};
    const auto broken = summarizeDisplacements(unreadable, one);
    CHECK(broken.displaced == 1);
    CHECK(broken.readable == 0);
    CHECK_FALSE(broken.power);
    REQUIRE(broken.problems.size() == 1);
    CHECK(broken.problems[0].find("cannot be read") != std::string::npos);
}

TEST_CASE("the vertex tool's feature query never treats a displacement grid vertex as a brush vertex") {
    Document doc(parsed(fixture()));
    PickIndex index(doc);
    const auto near = DVec3{kVertex12.x + 0.6, kVertex12.y - 0.6, 0};
    const auto features = index.nearestFeature({{near.x, near.y, 400}, {0, 0, -1}}, 8.0, overhead(near),
        [](const FeatureRef& feature) { return feature.kind != FeatureKind::DisplacementVertex; });
    CHECK_FALSE(features.vertex);
}
