// Plan 0.8 D13 (0.8.7): a material's hotspot rects from its VTF's hotspot resource or a Hammer++
// .rect file, in D13's precedence. P2CE ships no hotspot data (P0, 2026-11-14: none of its 5,665
// VTFs has the resource, and no .rect or %rectanglemap exists), so the VTFs here are hand-made from
// Strata's "Hotspot Embedded Format" page and the .rect from its "Hotspot Text Format" page.
#include "assets/hotspot.hpp"
#include "assets/vmt.hpp"
#include "fixture.hpp"

#include <doctest/doctest.h>

using namespace apex;
using namespace apex::assets;

namespace {

void put32(std::string& s, std::uint32_t v) { for (int i = 0; i < 4; ++i) s += char((v >> (8 * i)) & 0xff); }
void put16(std::string& s, std::uint16_t v) { s += char(v & 0xff); s += char(v >> 8); }

// A 4x4 RGBA8888 VTF 7.3 with a hotspot resource holding `rects`, laid out as the spec's header
// note asks: the hotspot resource ("+\0\0") before the high-res image (0x30).
std::string hotspotVtf(const std::vector<HotspotRegion>& rects, std::uint8_t version = 1) {
    std::string body;
    body += char(version);
    body += char(0);
    put16(body, std::uint16_t(rects.size()));
    for (const auto& r : rects) {
        body += char(r.flags);
        put16(body, std::uint16_t(r.x1)); put16(body, std::uint16_t(r.y1));
        put16(body, std::uint16_t(r.x2)); put16(body, std::uint16_t(r.y2));
    }
    constexpr std::uint32_t header = 80 + 2 * 8;
    std::string s("VTF\0", 4);
    put32(s, 7); put32(s, 3); put32(s, header);
    put16(s, 4); put16(s, 4);       // width, height
    put32(s, 0);                    // flags
    put16(s, 1); put16(s, 0);       // frames, first frame
    put32(s, 0);                    // padding
    for (int i = 0; i < 3; ++i) put32(s, 0); // reflectivity
    put32(s, 0);                    // padding
    put32(s, 0x3f800000);           // bumpmap scale 1.0
    put32(s, 0);                    // high-res format RGBA8888
    s += char(1);                   // mip count
    put32(s, 0xffffffff);           // low-res format: none
    s += char(0); s += char(0);     // low-res size
    put16(s, 1);                    // depth
    s += std::string(3, '\0');      // padding
    put32(s, 2);                    // resource count
    s += std::string(8, '\0');      // padding
    REQUIRE(s.size() == 80);
    s += std::string("+\0\0\0", 4); put32(s, header);
    s += std::string("\x30\0\0\0", 4); put32(s, header + 4 + std::uint32_t(body.size()));
    put32(s, std::uint32_t(body.size()));
    s += body;
    s += std::string(4 * 4 * 4, '\x7f');
    return s;
}
std::span<const std::byte> bytes(const std::string& s) { return {reinterpret_cast<const std::byte*>(s.data()), s.size()}; }

constexpr std::string_view kRectFile = R"(Rectangles
{
    rectangle
    {
        min "0 0"
        max "512 32"
    }
    rectangle
    {
        min "512 0"
        max "768 32"
        rotate  1   // Should this region be randomly rotated?
        reflect 1
        alt 1
    }
}
)";

Result<Vfs> load(GameTree& tree, JobPool& jobs) {
    tree.put("gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
    GameProfile profile;
    profile.gameDirectory = tree.root;
    return Vfs::load(profile, jobs);
}

} // namespace

TEST_CASE("the VTF hotspot resource reads as Strata specifies it") {
    const std::vector<HotspotRegion> rects{{0, 0, 256, 64, 0}, {256, 0, 512, 64, HotspotRegion::kRotate | HotspotRegion::kReflect},
                                           {0, 64, 1024, 128, HotspotRegion::kAlt}};
    const auto vtf = hotspotVtf(rects);
    const auto read = readVtfHotspots(bytes(vtf));
    REQUIRE(read);
    REQUIRE(*read);
    CHECK(**read == rects);
    // Cut short inside the resource: an error, so the caller reads the whole file.
    CHECK_FALSE(readVtfHotspots(bytes(vtf).first(100)));
    CHECK_FALSE(readVtfHotspots(bytes(hotspotVtf(rects, 2))));
    CHECK_FALSE(readVtfHotspots(bytes(std::string("VTX\0", 4) + vtf.substr(4))));
    // No hotspot resource: none, not an error.
    auto plain = vtf;
    plain[80] = '\x01'; // The first resource's tag becomes another resource's.
    const auto none = readVtfHotspots(bytes(plain));
    REQUIRE(none);
    CHECK_FALSE(*none);
}

TEST_CASE("a Hammer++ .rect file reads with its flags") {
    const auto rects = parseRectFile(kRectFile);
    REQUIRE(rects);
    REQUIRE(rects->size() == 2);
    CHECK((*rects)[0] == HotspotRegion{0, 0, 512, 32, 0});
    CHECK((*rects)[1] == HotspotRegion{512, 0, 768, 32, HotspotRegion::kRotate | HotspotRegion::kReflect | HotspotRegion::kAlt});
    CHECK_FALSE(parseRectFile("Rectangles { rectangle { min \"0 0\" } }"));
    CHECK_FALSE(parseRectFile("Something { }"));
    const auto empty = parseRectFile("Rectangles { }");
    REQUIRE(empty);
    CHECK(empty->empty());
}

TEST_CASE("a material's hotspots come from its VTF first, then %rectanglemap, then a .rect beside it") {
    GameTree tree;
    tree.put("materials/trim/both.vmt", "LightmappedGeneric { $basetexture trim/both %rectanglemap trim/shared }");
    tree.put("materials/trim/both.vtf", hotspotVtf({{0, 0, 4, 2, 0}}));
    tree.put("materials/trim/mapped.vmt", "LightmappedGeneric { $basetexture trim/plain %rectanglemap trim/shared }");
    tree.put("materials/trim/plain.vtf", hotspotVtf({}));
    tree.put("materials/trim/shared.rect", std::string(kRectFile));
    tree.put("materials/trim/beside.vmt", "LightmappedGeneric { $basetexture trim/plain }");
    tree.put("materials/trim/beside.rect", "Rectangles { rectangle { min \"0 0\" max \"64 64\" } }");
    tree.put("materials/trim/none.vmt", "LightmappedGeneric { $basetexture trim/plain }");
    tree.put("materials/trim/broken.vmt", "LightmappedGeneric { $basetexture trim/plain %rectanglemap trim/broken }");
    tree.put("materials/trim/broken.rect", "Rectangles { rectangle { max \"1 1\" } }");
    JobPool jobs(1);
    auto vfs = load(tree, jobs);
    REQUIRE(vfs);

    auto set = loadHotspots(*vfs, "trim/both");
    REQUIRE(set);
    REQUIRE(*set);
    CHECK((*set)->source == HotspotSource::VtfResource);
    CHECK((*set)->path == "materials/trim/both.vtf");
    CHECK((*set)->rects == std::vector<HotspotRegion>{{0, 0, 4, 2, 0}});

    set = loadHotspots(*vfs, "trim/mapped");
    REQUIRE(set);
    REQUIRE(*set);
    CHECK((*set)->source == HotspotSource::RectFile);
    CHECK((*set)->path == "materials/trim/shared.rect");
    CHECK((*set)->rects.size() == 2);

    set = loadHotspots(*vfs, "trim/beside");
    REQUIRE(set);
    REQUIRE(*set);
    CHECK((*set)->path == "materials/trim/beside.rect");

    set = loadHotspots(*vfs, "trim/none");
    REQUIRE(set);
    CHECK_FALSE(*set);
    CHECK_FALSE(loadHotspots(*vfs, "trim/broken"));
    CHECK_FALSE(loadHotspots(*vfs, "trim/missing"));
}
