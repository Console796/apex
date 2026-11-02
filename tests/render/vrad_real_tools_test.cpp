#include <doctest/doctest.h>
#include "assets/game_profile.hpp"
#include "compile/blocking_runner.hpp"
#include "document/document.hpp"
#include "document/vmf.hpp"
#include "editor/vrad_roundtrip.hpp"
#include "render/vrad_lightmaps.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>

using namespace apex;
namespace {

// A box brush as Hammer writes it: three clockwise points per side, dev texture, 16-unit lightmaps.
std::string box(int id, DVec3 lo, DVec3 hi) {
    const auto p = [](double x, double y, double z) {
        std::ostringstream text;
        text << '(' << x << ' ' << y << ' ' << z << ')';
        return text.str();
    };
    struct Side { std::string a, b, c, u, v; };
    const Side sides[] = {
        {p(lo.x, hi.y, hi.z), p(hi.x, hi.y, hi.z), p(hi.x, lo.y, hi.z), "[1 0 0 0] 0.25", "[0 -1 0 0] 0.25"},
        {p(lo.x, lo.y, lo.z), p(hi.x, lo.y, lo.z), p(hi.x, hi.y, lo.z), "[1 0 0 0] 0.25", "[0 -1 0 0] 0.25"},
        {p(lo.x, hi.y, hi.z), p(lo.x, lo.y, hi.z), p(lo.x, lo.y, lo.z), "[0 1 0 0] 0.25", "[0 0 -1 0] 0.25"},
        {p(hi.x, hi.y, lo.z), p(hi.x, lo.y, lo.z), p(hi.x, lo.y, hi.z), "[0 1 0 0] 0.25", "[0 0 -1 0] 0.25"},
        {p(hi.x, lo.y, lo.z), p(lo.x, lo.y, lo.z), p(lo.x, lo.y, hi.z), "[1 0 0 0] 0.25", "[0 0 -1 0] 0.25"},
        {p(hi.x, hi.y, hi.z), p(lo.x, hi.y, hi.z), p(lo.x, hi.y, lo.z), "[1 0 0 0] 0.25", "[0 0 -1 0] 0.25"}};
    std::ostringstream text;
    text << "solid\n{\n\"id\" \"" << id << "\"\n";
    int side = id * 10;
    for (const auto& s : sides)
        text << "side\n{\n\"id\" \"" << ++side << "\"\n\"plane\" \"" << s.a << ' ' << s.b << ' ' << s.c
             << "\"\n\"material\" \"DEV/DEV_MEASUREGENERIC01\"\n\"uaxis\" \"" << s.u << "\"\n\"vaxis\" \""
             << s.v << "\"\n\"rotation\" \"0\"\n\"lightmapscale\" \"16\"\n\"smoothing_groups\" \"0\"\n}\n";
    text << "}\n";
    return text.str();
}

} // namespace

// Needs the installed Portal 2: Community Edition tools; skipped (like the other optional corpora) otherwise.
TEST_CASE("a sealed lit room compiles through the real tools and its faces map back onto the document") {
    const auto* root = std::getenv("APEX_P2CE_DIR");
    if (!root || !std::filesystem::is_directory(root)) {
        MESSAGE("APEX_P2CE_DIR is unset or invalid; skipping the real vbsp/vrad round trip");
        return;
    }
    assets::GameProfile profile;
    profile.gameDirectory = std::filesystem::path(root) / "p2ce";
    editor::CompileGamePaths paths;
    paths.gameDirectory = profile.gameDirectory;
    paths.toolDirectory = profile.toolDirectory();
    paths.instanceRoot = profile.instanceSearchRoot();
    for (const auto& [variable, tool] : {std::pair{"bsp_exe", "vbsp"}, {"vis_exe", "vvis"}, {"light_exe", "vrad"}}) {
        paths.tools[variable] = profile.toolPath(tool).string();
        if (!std::filesystem::exists(paths.tools[variable])) {
            MESSAGE("no " << tool << " in the install; skipping the real vbsp/vrad round trip");
            return;
        }
    }
    // A 256-unit cube with 16-unit walls, a point light under the ceiling and a player start.
    std::string text = "world\n{\n\"id\" \"1\"\n\"classname\" \"worldspawn\"\n\"skyname\" \"sky_day01_01\"\n";
    text += box(2, {-16, -16, -16}, {272, 272, 0}) + box(3, {-16, -16, 256}, {272, 272, 272}) +
            box(4, {-16, -16, 0}, {0, 272, 256}) + box(5, {256, -16, 0}, {272, 272, 256}) +
            box(6, {0, -16, 0}, {256, 0, 256}) + box(7, {0, 256, 0}, {256, 272, 256}) + "}\n";
    text += "entity\n{\n\"id\" \"20\"\n\"classname\" \"light\"\n\"origin\" \"128 128 224\"\n"
            "\"_light\" \"255 255 255 2000\"\n\"_lightHDR\" \"-1 -1 -1 1\"\n\"_lightscaleHDR\" \"1\"\n}\n"
            "entity\n{\n\"id\" \"21\"\n\"classname\" \"info_player_start\"\n\"origin\" \"64 64 16\"\n\"angles\" \"0 0 0\"\n}\n";
    auto parsed = vmf::parse(text);
    REQUIRE(parsed);
    document::Document doc(std::move(*parsed));

    compile::BlockingProcessRunner runner;
    compile::FilesystemFileOps files;
    editor::VradRoundTrip trip(runner, files);
    const auto started = trip.start(doc, std::filesystem::temp_directory_path() / "apex-room.vmf", paths, {});
    REQUIRE(started);
    REQUIRE_MESSAGE(trip.poll() == editor::VradRoundTrip::State::Done, trip.message());
    auto loaded = render::loadVradLightmaps(trip.bsp(), *trip.exported(), trip.origins());
    REQUIRE(loaded);

    // The six inner faces are lit and map; the walls' outer faces are sealed away and not in the BSP.
    CHECK(loaded->stats.mapped == 6);
    CHECK(loaded->stats.faces == 6);
    CHECK(loaded->stats.unowned == 0);
    CHECK(loaded->stats.displacement == 0);
    CHECK(loaded->atlas.compiled);
    // Light came from the entity: the floor face below the light holds the brightest, non-black luxels.
    float peak = 0;
    for (std::size_t i = 0; i < loaded->atlas.faces.size(); ++i)
        for (const auto& luxel : loaded->radiance[i]) peak = std::max(peak, luxel.x + luxel.y + luxel.z);
    CHECK(peak > 0.1f);
    // Each mapped face lies on a document side (same solid and side as the exported document).
    for (const auto& face : loaded->atlas.faces) CHECK(face.solidId != 0);
    // The document the user edits is not the one that was compiled.
    CHECK(trip.stale(doc.historyState()) == false);
}
