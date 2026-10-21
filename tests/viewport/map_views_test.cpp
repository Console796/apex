#include "viewport/map_views.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>

using namespace apex;
using namespace apex::viewport;

namespace {

std::string read(const std::filesystem::path& p) {
    std::ifstream s(p, std::ios::binary);
    REQUIRE(s.good());
    return {std::istreambuf_iterator<char>(s), std::istreambuf_iterator<char>()};
}

vmf::VmfFile parsed(const std::string& src) {
    auto r = vmf::parse(src);
    REQUIRE(r.has_value());
    return std::move(*r);
}

const std::string kBase = "versioninfo\n{\n}\nviewsettings\n{\n\t\"bSnapToGrid\" \"1\"\n";

} // namespace

TEST_CASE("map views are read from the probe fixture") {
    const auto path = std::filesystem::path(APEX_FIXTURE_DIR) / "probe" / "e_control.vmf";
    auto file = parsed(read(path));
    const auto views = readMapViews(file);
    REQUIRE(views[0]);
    CHECK(views[0]->is3d);
    CHECK(views[0]->position == DVec3{0, -300, 100});
    CHECK(views[0]->angles == DVec3{15, 90, 0});
    REQUIRE(views[1]);
    CHECK_FALSE(views[1]->is3d);
    CHECK(views[1]->zoom == 0.5);
    CHECK(views[3]);
}

TEST_CASE("unchanged views write byte-identically") {
    const auto path = std::filesystem::path(APEX_FIXTURE_DIR) / "probe" / "e_control.vmf";
    const auto src = read(path);
    auto file = parsed(src);
    REQUIRE(writeMapViews(file, readMapViews(file)));
    CHECK(vmf::write(file) == src);

    // Through a layout with a different pixel height the zoom still round trips.
    auto layout = ViewportLayout::quad();
    applyMapViews(layout, readMapViews(file), 900.0);
    REQUIRE(writeMapViews(file, captureMapViews(layout, 900.0)));
    CHECK(vmf::write(file) == src);
}

TEST_CASE("changed views edit only their values") {
    const auto path = std::filesystem::path(APEX_FIXTURE_DIR) / "probe" / "e_control.vmf";
    const auto src = read(path);
    auto file = parsed(src);
    auto views = readMapViews(file);
    views[0]->position = {1, 2, 3.5};
    views[1]->zoom = 0.25;
    REQUIRE(writeMapViews(file, views));
    const auto out = vmf::write(file);
    CHECK(out != src);
    CHECK(out.find("\"position\" \"(1 2 3.5)\"") != std::string::npos);
    CHECK(out.find("\"zoom\" \"0.25\"") != std::string::npos);
    CHECK(out.find("\"angle\" \"[15 90 0]\"") != std::string::npos);

    auto reparsed = parsed(out);
    const auto back = readMapViews(reparsed);
    CHECK(back[0]->position == DVec3{1, 2, 3.5});
    CHECK(back[1]->zoom == 0.25);
    CHECK(back[2] == views[2]);
    // Everything outside the edits is untouched.
    std::string expected = src;
    auto replace = [&](std::string_view a, std::string_view b) {
        const auto at = expected.find(a);
        REQUIRE(at != std::string::npos);
        expected.replace(at, a.size(), b);
    };
    replace("\"(0 -300 100)\"", "\"(1 2 3.5)\"");
    replace("\"0.5\"", "\"0.25\"");
    CHECK(out == expected);
}

TEST_CASE("views are created inside an existing viewsettings block") {
    auto file = parsed(kBase + "}\nworld\n{\n\t\"id\" \"1\"\n}\n");
    CHECK(readMapViews(file)[0] == std::nullopt);
    MapViews views;
    views[0] = MapView{true, {10, 20, 30}, {5, 45, 0}, 1};
    views[2] = MapView{false, {-64, 128, 0}, {}, 2.0};
    REQUIRE(writeMapViews(file, views));
    const auto out = vmf::write(file);
    auto again = parsed(out);
    const auto back = readMapViews(again);
    CHECK(back[0] == views[0]);
    CHECK(back[1] == std::nullopt);
    CHECK(back[2] == views[2]);
    REQUIRE(again.world);
    CHECK(out.find("\"bSnapToGrid\" \"1\"") != std::string::npos);

    // Typed document stays usable after structural edits and a second write.
    views[2]->zoom = 4.0;
    REQUIRE(writeMapViews(file, views));
    CHECK(readMapViews(parsed(vmf::write(file)))[2]->zoom == 4.0);
    CHECK(file.viewSettings->views->children.size() == 2);
}

TEST_CASE("missing viewsettings is reported, empty write is a no-op") {
    auto file = parsed("world\n{\n}\n");
    CHECK(writeMapViews(file, MapViews{}));
    MapViews views;
    views[0] = MapView{};
    const auto r = writeMapViews(file, views);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::Unsupported);
}

TEST_CASE("malformed views are skipped") {
    auto file = parsed(kBase + "\tviews\n\t{\n\t\tv0\n\t\t{\n\t\t\t\"3d\" \"1\"\n\t\t\t\"position\" \"oops\"\n\t\t}\n"
                              "\t\tv1\n\t\t{\n\t\t\t\"3d\" \"0\"\n\t\t\t\"position\" \"(1 2 3)\"\n\t\t\t\"zoom\" \"-1\"\n\t\t}\n"
                              "\t\tv9\n\t\t{\n\t\t}\n\t}\n}\n");
    const auto views = readMapViews(file);
    for (const auto& v : views) CHECK_FALSE(v);
}

TEST_CASE("views map to and from layout panes") {
    auto layout = ViewportLayout::quad();
    MapViews views;
    views[0] = MapView{true, {100, 200, 300}, {10, 20, 0}, 1};
    views[1] = MapView{false, {512, -256, 0}, {}, 0.5};
    applyMapViews(layout, views, 800.0);
    const auto ids = layout.paneIds();
    const auto& cam = layout.viewport(ids[0])->camera;
    CHECK(cam.position == DVec3{100, 200, 300});
    CHECK(cam.angles == DVec3{10, 20, 0});
    const auto& top = layout.viewport(ids[1])->camera;
    CHECK(top.orthoHeight == doctest::Approx(1600.0));
    CHECK(top.position.x == doctest::Approx(512));
    CHECK(top.position.y == doctest::Approx(-256));
    const auto cap = captureMapViews(layout, 800.0);
    CHECK(cap[0]->is3d);
    CHECK_FALSE(cap[1]->is3d);
    CHECK(cap[1]->zoom == doctest::Approx(0.5));
    CHECK(cap[3]);

    // A 2D view onto a perspective pane converts it.
    ViewportLayout single;
    MapViews one;
    one[0] = MapView{false, {1, 2, 3}, {}, 1.0};
    applyMapViews(single, one);
    CHECK(single.viewport(single.paneIds()[0])->camera.isOrtho());
}
