// Plan 0.8 D6 (0.8.4) timing: sculpting a 16x16 grid of power-4 displacements (256 patches,
// 74,000 vertices). Opt-in (APEX_MEASURE=1), in apex_measure_tests; it reports, it does not gate.
#include "fixtures.hpp"
#include "editor/displacement_edit.hpp"
#include "editor/displacement_tool.hpp"
#include "editor/editor_session.hpp"
#include "geometry/displacement_sculpt.hpp"
#include "geometry/solid_geometry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>

using namespace editor_test;

namespace {

using Clock = std::chrono::steady_clock;

bool measureEnabled() {
    const auto* value = std::getenv("APEX_MEASURE");
    return value && std::string_view(value) == "1";
}

template <typename F> double milliseconds(F&& operation) {
    const auto start = Clock::now();
    operation();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct Summary {
    double mean = 0.0, median = 0.0, worst = 0.0;
};

Summary summarise(std::vector<double> samples) {
    Summary s;
    if (samples.empty()) return s;
    for (const auto v : samples) s.mean += v;
    s.mean /= double(samples.size());
    std::ranges::sort(samples);
    s.median = samples[samples.size() / 2];
    s.worst = samples.back();
    return s;
}

const char* kEmpty = "versioninfo\n{\n\"editorversion\" \"400\"\n}\nworld\n{\n\"id\" \"1\"\n\"classname\" \"worldspawn\"\n}\n";
constexpr int kGrid = 16;
constexpr double kCell = 128.0;

ToolView rayView(DVec3 at) {
    ToolView view;
    view.cursorRay = {at + DVec3{0, 0, 1024}, {0, 0, -1}};
    view.size = {1600, 900};
    return view;
}

} // namespace

TEST_CASE("[measure] 0.8.4 sculpt a 16x16 grid of power-4 displacements") {
    if (!measureEnabled()) {
        MESSAGE("APEX_MEASURE=1 is required; skipping sculpt measurements");
        return;
    }
    Document doc(parsed(kEmpty));
    EditorSession session;
    session.attach(doc);
    const auto world = *doc.handle(doc.roots().front());
    std::vector<SideId> tops;
    for (int y = 0; y < kGrid; ++y)
        for (int x = 0; x < kGrid; ++x) {
            const DVec3 min{x * kCell, y * kCell, 0.0};
            const auto box = addBox(doc, min, min + DVec3{kCell, kCell, 64.0}, world);
            const auto geometry = geometry::solidGeometry(*doc.get(box)->block).value();
            for (const auto& face : geometry.poly.faces)
                if (face.plane.normal.z > 0.999) tops.push_back(static_cast<SideId>(face.sideId));
        }
    REQUIRE(tops.size() == std::size_t(kGrid * kGrid));
    const double create = milliseconds([&] { REQUIRE(createDisplacements(session, tops, 4, 0.0)); });
    REQUIRE(session.perform("tool.displacement"));
    auto& tool = dynamic_cast<DisplacementTool&>(session.activeToolObject());
    tool.setMode(DisplacementTool::Mode::Sculpt);
    tool.setParameter("radius", 96);
    tool.setParameter("amount", 4);

    // A diagonal stroke across the grid, the cursor reported every 6 units: one input sample per
    // event, a dab every quarter radius (24 units).
    const double span = kGrid * kCell;
    const auto at = [&](double t) { return DVec3{40.0 + t * (span - 80.0), 40.0 + t * (span - 80.0) * 0.7, 64.0}; };
    const int samples = static_cast<int>(glm::length(at(1.0) - at(0.0)) / 6.0);
    InputEvent press;
    press.kind = InputKind::Press;
    press.button = PointerButton::Left;
    press.left = true;
    const double first = milliseconds([&] { REQUIRE(session.input(press, rayView(at(0.0)))); }); // Reads the working set.
    REQUIRE(tool.stroking());
    std::vector<double> moves;
    InputEvent move;
    move.kind = InputKind::Move;
    move.left = true;
    for (int i = 1; i <= samples; ++i)
        moves.push_back(milliseconds([&] { session.input(move, rayView(at(double(i) / samples))); }));
    InputEvent release;
    release.kind = InputKind::Release;
    release.button = PointerButton::Left;
    const double commit = milliseconds([&] { session.input(release, rayView(at(1.0))); });
    REQUIRE(doc.undoLabels().back() == "Sculpt displacement");

    // The second stroke reuses the working set, so its press is a raycast and one dab.
    const double again = milliseconds([&] { session.input(press, rayView(at(0.5))); });
    session.input(release, rayView(at(0.5)));

    // The stroke alone, without the tool's preview: dabs straight into a DisplacementStroke.
    std::vector<geometry::DisplacementPatch> patches;
    for (const auto side : tops) {
        auto base = sideDisplacement(doc, side).value();
        patches.push_back({base.corners, *base.disp});
    }
    std::optional<geometry::DisplacementStroke> stroke;
    const double build = milliseconds([&] { stroke = geometry::DisplacementStroke::create(std::move(patches)).value(); });
    stroke->begin();
    std::vector<double> dabs;
    geometry::SculptSettings settings;
    settings.radius = 96;
    settings.amount = 4;
    const int dabCount = static_cast<int>(glm::length(at(1.0) - at(0.0)) / 24.0);
    for (int i = 0; i <= dabCount; ++i)
        dabs.push_back(milliseconds([&] { stroke->sculpt(settings, at(double(i) / dabCount)); }));
    std::vector<double> casts;
    for (int i = 0; i <= 64; ++i) {
        const auto view = rayView(at(double(i) / 64));
        casts.push_back(milliseconds([&] { REQUIRE(stroke->raycast(view.cursorRay)); }));
    }
    const double finish = milliseconds([&] { REQUIRE(stroke->finish()); });

    const auto m = summarise(moves), d = summarise(dabs), c = summarise(casts);
    std::cout << "[measure] sculpt 16x16 power-4 (" << tops.size() << " patches, "
              << tops.size() * 17 * 17 << " vertices)\n"
              << "  create displacements: " << create << " ms\n"
              << "  tool: first press (working set + dab) " << first << " ms; next stroke's press " << again
              << " ms\n"
              << "  tool: per input sample (" << moves.size() << " samples, raycast + dabs + preview) mean "
              << m.mean << " ms, median " << m.median << " ms, worst " << m.worst << " ms\n"
              << "  tool: commit (retag + setDisplacement + re-read) " << commit << " ms\n"
              << "  stroke: build " << build << " ms; per dab (" << dabs.size() << ", radius 96) mean " << d.mean
              << " ms, median " << d.median << " ms, worst " << d.worst << " ms\n"
              << "  stroke: raycast mean " << c.mean << " ms, worst " << c.worst << " ms; finish " << finish
              << " ms\n";
}
