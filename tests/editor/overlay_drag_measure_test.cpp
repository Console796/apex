// Plan 0.8 D12 (0.8.6) timing: dragging each Overlay tool handle on a 48x48 floor of boxes (2,304
// solids, 13,824 faces) carrying 256 overlays and 32 decals. Each move is timed through
// EditorSession::input; a document observer stands in for the render mirror (an OverlayMirror with
// a no-op upload) so the per-move document traffic shows too, and the tool's preview goes through
// OverlayMirror::setPreview as the viewport sends it. Every ChangeSet also reaches the editor's
// document-wide observers (the outliner rebuilds its tree and a GroupIndex, the materials panel
// walks every solid, the bake restarts); a GroupIndex build stands in for their cost. Opt-in
// (APEX_MEASURE=1), in apex_measure_tests; it reports, it does not gate.
#include "face_fixtures.hpp"
#include "editor/group_index.hpp"
#include "editor/overlay_placement.hpp"
#include "editor/overlay_tool.hpp"
#include "render/overlay_mirror.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>

using namespace face_test;

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
constexpr int kGrid = 48;
constexpr double kCell = 64.0;
constexpr int kOverlayStride = 3; // One overlay per 3x3 cells: 16x16 = 256.
constexpr int kDecalStride = 8;   // One decal per 8x8 cells beside them: 6x6 = 36, 32 kept.
constexpr int kMoves = 60;

ToolView above(DVec3 point) { return view({point + DVec3{0, 0, 256}, {0, 0, -1}}); }
InputEvent at(InputKind kind, const ToolView& v, DVec3 point) {
    auto event = click(PointerButton::Left);
    event.kind = kind;
    event.left = kind != InputKind::Release;
    event.cursor = *worldToPixel(v, point);
    return event;
}

render::OverlayMirror::Upload noUpload() {
    return [](std::span<const std::byte>, VkBufferUsageFlags) -> Result<render::Buffer> { return render::Buffer{}; };
}

} // namespace

TEST_CASE("[measure] 0.8.6 drag each Overlay tool handle on a floor of 2,304 boxes with 256 overlays") {
    if (!measureEnabled()) {
        MESSAGE("APEX_MEASURE=1 is required; skipping overlay drag measurements");
        return;
    }
    Document doc(parsed(kEmpty));
    const auto world = *doc.handle(doc.roots().front());
    {
        auto tx = doc.begin("floor");
        for (int y = 0; y < kGrid; ++y)
            for (int x = 0; x < kGrid; ++x) {
                const DVec3 min{x * kCell, y * kCell, -32.0};
                tx.create(solidBlock(geometry_test::build(geometry_test::boxSides(min, min + DVec3{kCell, kCell, 32.0}))), world);
            }
        tx.commit();
    }
    std::vector<OverlaySurface> tops;
    {
        SurfaceLookup lookup(doc);
        for (auto& surface : lookup.all())
            if (surface.normal.z > 0.999) tops.push_back(std::move(surface));
    }
    std::vector<NodeId> overlays;
    {
        auto tx = doc.begin("overlays");
        int decals = 0;
        for (int y = 1; y < kGrid; y += kOverlayStride)
            for (int x = 1; x < kGrid; x += kOverlayStride) {
                const DVec3 centre{(x + 0.5) * kCell, (y + 0.5) * kCell, 0.0};
                const auto quad = geometry::faceAlignedQuad(centre, {0, 0, 1}, {1, 0, 0}, {128.0, 64.0});
                std::vector<OverlaySurface> near;
                for (const auto& surface : tops)
                    if (glm::length(surface.polygon.front() - centre) < 4.0 * kCell) near.push_back(surface);
                const auto handle = tx.create(overlayBlock(quad, "dev/dev_measuregeneric01", footprintSides(near, quad)), world);
                overlays.push_back(doc.get(handle)->id);
            }
        for (int y = 2; y < kGrid && decals < 32; y += kDecalStride)
            for (int x = 2; x < kGrid && decals < 32; x += kDecalStride, ++decals)
                tx.create(decalBlock({(x + 0.25) * kCell, (y + 0.25) * kCell, 0.0}, "decals/dirt"), world);
        tx.commit();
    }
    REQUIRE(overlays.size() == 256);
    const double groupIndex = milliseconds([&] { editor::GroupIndex groups(doc); });

    // The render mirror's overlays, fed every ChangeSet the drag commits.
    render::OverlayMirror mirror;
    std::vector<render::Buffer> garbage;
    const auto decalSize = [](const std::string&) -> std::optional<DVec2> { return DVec2{64.0}; };
    const double mirrorLoad = milliseconds([&] {
        document::ChangeSet all;
        for (const auto handle : nodes(doc)) all.created.push_back(handle);
        mirror.apply(doc, all, noUpload(), decalSize, garbage);
    });
    REQUIRE(mirror.overlayCount() == 256 + 32);
    std::size_t changeSets = 0;
    double mirrorTime = 0.0;
    const auto token = doc.observe([&](const document::ChangeSet& changes) {
        ++changeSets;
        mirrorTime += milliseconds([&] { mirror.apply(doc, changes, noUpload(), decalSize, garbage); });
        garbage.clear();
    });

    EditorSession session;
    session.attach(doc);
    session.faceTextureSize = [](std::string_view) { return std::pair{128, 64}; };
    REQUIRE(session.perform("tool.overlay"));
    const NodeId target = overlays[overlays.size() / 2 + 8]; // Near the middle of the floor.
    session.selection().select(target);
    const auto quadOf = [&] { return *geometry::parseOverlay(*doc.get(*doc.handle(target))->block); };
    const auto start = quadOf();

    struct Kind {
        const char* name;
        OverlayHandle handle;
        DVec3 step; // Per move, in world units, on the floor plane.
    };
    const std::vector<Kind> kinds{
        {"move", {OverlayHandleKind::Move}, {1.5, 0.75, 0.0}},
        {"rotate", {OverlayHandleKind::Rotate}, {0.0, 1.0, 0.0}},
        {"scale U", {OverlayHandleKind::ScaleU}, {1.0, 0.0, 0.0}},
        {"scale V", {OverlayHandleKind::ScaleV}, {0.0, 0.75, 0.0}},
        {"corner", {OverlayHandleKind::Corner, 2}, {1.0, 0.5, 0.0}},
    };
    std::cout << "[measure] overlay drag: " << kGrid * kGrid << " solids, " << kGrid * kGrid * 6 << " faces, "
              << overlays.size() << " overlays + 32 decals; mirror initial load " << mirrorLoad
              << " ms; GroupIndex build (the outliner's per-ChangeSet floor) " << groupIndex << " ms\n";
    for (const auto& kind : kinds) {
        const auto quad = quadOf();
        DVec3 from{};
        for (const auto& spot : overlayHandles(quad))
            if (spot.handle == kind.handle) from = spot.position;
        const auto undo = doc.undoCount();
        changeSets = 0;
        mirrorTime = 0.0;
        const auto pressView = above(from);
        const double press = milliseconds([&] { REQUIRE(session.input(at(InputKind::Press, pressView, from), pressView)); });
        std::vector<double> moves, previews;
        DVec3 to = from;
        for (int i = 0; i < kMoves; ++i) {
            to += kind.step;
            auto v = pressView; // The camera stays; the cursor ray moves.
            v.cursorRay = {to + DVec3{0, 0, 256}, {0, 0, -1}};
            const auto event = at(InputKind::Move, v, to);
            moves.push_back(milliseconds([&] { session.input(event, v); }));
            previews.push_back(milliseconds([&] { mirror.setPreview(session.preview().overlays(), noUpload(), garbage); }));
            garbage.clear();
        }
        const auto changesDuringMoves = changeSets;
        const double mirrorDuringMoves = mirrorTime;
        auto v = pressView;
        v.cursorRay = {to + DVec3{0, 0, 256}, {0, 0, -1}};
        const double release = milliseconds([&] { session.input(at(InputKind::Release, v, to), v); });
        CHECK(session.preview().empty());
        mirror.setPreview({}, noUpload(), garbage);
        garbage.clear();
        CHECK(doc.undoCount() == undo + 1);
        CHECK(doc.undoLabels().back() == "Edit overlay");
        CHECK(quadOf() != quad);
        const auto m = summarise(moves), p = summarise(previews);
        std::cout << "  " << kind.name << ": press " << press << " ms; per move (" << moves.size() << ") mean " << m.mean
                  << " ms, median " << m.median << " ms, worst " << m.worst << " ms; mirror preview mean " << p.mean
                  << " ms, worst " << p.worst << " ms; release " << release
                  << " ms; ChangeSets during moves " << changesDuringMoves << " (mirror " << mirrorDuringMoves
                  << " ms), after release " << changeSets << " (mirror " << mirrorTime << " ms)\n";
    }
    doc.unobserve(token);
    CHECK(quadOf() != start);
}
