// Plan 0.8 D1 (dispinfo parse/write) and D2 (the mesh against VBSP's DISP_VERTS).
#include "document/displacement.hpp"
#include "document/document.hpp"
#include "document/vmf.hpp"
#include "geometry/displacement.hpp"
#include "geometry/solid_geometry.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <ranges>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

using namespace apex;

namespace {

constexpr int kSide = 355;

std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.good());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::filesystem::path dispDir() { return std::filesystem::path(APEX_FIXTURE_DIR) / "vmf" / "disp"; }

vmf::VmfFile parsed(std::string_view source) {
    auto result = vmf::parse(source);
    REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().what()));
    return std::move(*result);
}

struct Found {
    vmf::Solid* solid = nullptr;
    vmf::Side* side = nullptr;
};
Found find(vmf::VmfFile& file, int sideId) {
    for (auto& solid : file.world->solids)
        for (auto& side : solid.sides)
            if (side.id() && side.id()->asInt().value_or(0) == sideId) return {&solid, &side};
    FAIL("side not found");
    return {};
}

std::vector<std::string> lines(std::string_view text) {
    std::vector<std::string> result;
    std::istringstream in{std::string(text)};
    for (std::string line; std::getline(in, line);) result.push_back(line);
    return result;
}

geometry::DisplacementMesh meshOf(vmf::Solid& solid, int sideId, const geometry::Displacement& disp) {
    const auto geometry = geometry::solidGeometry(*solid.node);
    REQUIRE(geometry);
    const auto face = std::ranges::find(geometry->poly.faces, static_cast<std::uint32_t>(sideId), &geometry::Face::sideId);
    REQUIRE(face != geometry->poly.faces.end());
    const auto corners = geometry::displacementCorners(geometry->poly, *face);
    REQUIRE(corners);
    auto mesh = geometry::displacementMesh(*corners, disp);
    REQUIRE(mesh);
    return std::move(*mesh);
}

struct Variant {
    std::string name;
    std::vector<std::array<std::string, 3>> edits; // block ("-" for a scalar), key, value
    std::map<std::size_t, std::array<double, 4>> verts; // x y z alpha
};

std::vector<Variant> oracle() {
    std::istringstream in(read(dispDir() / "test1_disp.vbsp.txt"));
    std::vector<Variant> result;
    for (std::string line; std::getline(in, line);) {
        std::istringstream words(line);
        std::string kind;
        words >> kind;
        if (kind == "variant") {
            result.emplace_back();
            words >> result.back().name;
        } else if (kind == "edit") {
            std::array<std::string, 3> edit;
            words >> edit[0] >> edit[1];
            std::getline(words >> std::ws, edit[2]);
            result.back().edits.push_back(edit);
        } else if (kind == "vert") {
            std::size_t index = 0;
            std::array<double, 4> v{};
            words >> index >> v[0] >> v[1] >> v[2] >> v[3];
            result.back().verts[index] = v;
        }
    }
    return result;
}

} // namespace

TEST_CASE("dispinfo parses the Strata Hammer fixture and writes back byte-identically") {
    const auto source = read(dispDir() / "test1_disp.vmf");
    auto file = parsed(source);
    auto [solid, side] = find(file, kSide);
    REQUIRE(side->dispInfo);
    const auto disp = document::parseDisplacement(*side->dispInfo->node);
    REQUIRE_MESSAGE(disp, (disp ? "" : disp.error().what()));
    CHECK(disp->power == 3);
    CHECK(disp->startPosition == DVec3{-96, -1632, 64});
    CHECK(disp->normals.size() == 81);
    CHECK(disp->distances[0] == doctest::Approx(19.9938));
    CHECK(disp->normals[4 * 9] == DVec3{0, 0, 0});
    // The fixture's alphas are vertex X coordinates, not 0..255; they are kept as written.
    CHECK(std::vector(disp->alphas.begin(), disp->alphas.begin() + 9) ==
          std::vector<double>{-96, -72, -48, -24, 0, 24, 48, 72, 0});
    CHECK(disp->triangleTags.size() == 128);
    CHECK(disp->triangleTags[14] == 1);
    CHECK(disp->triangleTags[0] == 9);
    // Strata Hammer writes five 64-bit words: 289 bits set, one per power-4 vertex.
    REQUIRE(disp->allowedVerts.size() == 320);
    CHECK(std::count(disp->allowedVerts.begin(), disp->allowedVerts.end(), true) == 289);

    REQUIRE(document::writeDisplacement(*side->dispInfo->node, *disp));
    CHECK(vmf::write(file) == source);
    CHECK(document::parseDisplacement(*side->dispInfo->node).value() == *disp);
}

TEST_CASE("dispinfo writes change only the rows that differ") {
    const auto source = read(dispDir() / "test1_disp.vmf");
    auto file = parsed(source);
    auto* node = find(file, kSide).side->dispInfo->node;
    auto disp = document::parseDisplacement(*node).value();
    disp.distances[5 * 9 + 3] = 42.5;
    REQUIRE(document::writeDisplacement(*node, disp));
    const auto before = lines(source), after = lines(vmf::write(file));
    REQUIRE(before.size() == after.size());
    std::vector<std::string> changed;
    for (std::size_t i = 0; i < before.size(); ++i)
        if (before[i] != after[i]) changed.push_back(after[i]);
    REQUIRE(changed.size() == 1);
    CHECK(changed[0] == "\t\t\t\t\t\"row5\" \"0 0 0 42.5 1.6047 7.70393 17.363 16.5555 6.9591\"\r");
    CHECK(document::parseDisplacement(*node).value() == disp);

    // A start position and an allowed-verts bit go back the way they were written.
    disp.startPosition = {96, -1440, 64};
    disp.allowedVerts[3] = false;
    REQUIRE(document::writeDisplacement(*node, disp));
    const auto text = vmf::write(file);
    CHECK(text.find("\"startposition\" \"[96 -1440 64]\"") != std::string::npos);
    CHECK(text.find("\"5\" \"18446744073709551607 18446744073709551615") != std::string::npos);

    // Invalid values are rejected without writing.
    auto bad = disp;
    bad.alphas.pop_back();
    CHECK_FALSE(document::writeDisplacement(*node, bad));
    CHECK(vmf::write(file) == text);
}

TEST_CASE("dispinfo parse rejects malformed rows and a missing power") {
    const auto parse = [](std::string text) {
        auto doc = kv1::parse(std::move(text));
        REQUIRE(doc);
        return document::parseDisplacement(doc->root().children()[0]);
    };
    CHECK_FALSE(parse("dispinfo { startposition \"[0 0 0]\" }"));
    CHECK_FALSE(parse("dispinfo { power 5 }"));
    CHECK_FALSE(parse("dispinfo { power 2 distances { row0 \"1 2 3\" } }"));
    CHECK_FALSE(parse("dispinfo { power 2 distances { row0 \"1 2 x 4 5\" } }"));
    // Absent rows read as zero; the block stays valid.
    const auto sparse = parse("dispinfo { power 2 distances { row1 \"1 2 3 4 5\" } }");
    REQUIRE(sparse);
    CHECK(sparse->distances[5] == 1);
    CHECK(sparse->normals.size() == 25);
}

// D2: compiled with the native P2CE vbsp by tools/disp_oracle.py (fixture test1_disp.vbsp.txt).
TEST_CASE("the displacement mesh matches VBSP's DISP_VERTS within 0.01 units") {
    const auto source = read(dispDir() / "test1_disp.vmf");
    const auto variants = oracle();
    REQUIRE(variants.size() == 7);
    for (const auto& variant : variants) {
        CAPTURE(variant.name);
        auto file = parsed(source);
        auto [solid, side] = find(file, kSide);
        auto* node = side->dispInfo->node;
        for (const auto& [block, key, value] : variant.edits) {
            auto* target = block == "-" ? node : node->find(block);
            REQUIRE(target);
            REQUIRE(target->find(key));
            REQUIRE(target->find(key)->setValue(value));
        }
        const auto disp = document::parseDisplacement(*node);
        REQUIRE(disp);
        const auto mesh = meshOf(*solid, kSide, *disp);
        REQUIRE(variant.verts.size() == mesh.positions.size());
        for (const auto& [index, expected] : variant.verts) {
            const auto& p = mesh.positions[index];
            INFO("test1_disp.vmf side ", kSide, " variant ", variant.name, " vertex ", index, " (row ", index / 9,
                 " column ", index % 9, ") apex ", p.x, ' ', p.y, ' ', p.z, " vbsp ", expected[0], ' ', expected[1],
                 ' ', expected[2]);
            REQUIRE(glm::distance(p, DVec3{expected[0], expected[1], expected[2]}) < 0.01);
            // VBSP stores alphas unclamped as float: -96 stays -96.
            REQUIRE(mesh.alphas[index] == doctest::Approx(expected[3]).epsilon(1e-6));
        }
    }
}

// Strata Hammer computed the fixture's triangle_tags from its own mesh: the three triangles it
// left without bit 8 are exactly the ones steeper than VBSP's 0.8 walkable limit in Apex's mesh.
// This pins the triangle split per quad (VBSP copies the tags, so the compile cannot check it).
TEST_CASE("Hammer's triangle tags agree with the mesh's triangle slopes") {
    auto file = parsed(read(dispDir() / "test1_disp.vmf"));
    auto [solid, side] = find(file, kSide);
    const auto disp = document::parseDisplacement(*side->dispInfo->node).value();
    const auto mesh = meshOf(*solid, kSide, disp);
    for (std::size_t t = 0; t < mesh.triangles.size(); ++t) {
        const auto& tri = mesh.triangles[t];
        const auto& a = mesh.positions[tri[0]];
        const auto normal = glm::normalize(glm::cross(mesh.positions[tri[1]] - a, mesh.positions[tri[2]] - a));
        CAPTURE(t);
        CHECK(normal.z > 0.0); // Counter-clockwise from outside (the face points up).
        CHECK((normal.z < 0.8) == ((mesh.tags[t] & 8) == 0));
    }
}

TEST_CASE("document displacement lookup and setDisplacement are one undoable write") {
    const auto source = read(dispDir() / "test1_disp.vmf");
    document::Document doc(parsed(source));
    const auto disp = document::displacement(doc, kSide);
    REQUIRE(disp);
    REQUIRE(disp->has_value());
    CHECK((*disp)->power == 3);
    CHECK_FALSE(document::displacement(doc, 356).value().has_value()); // Flat side.
    CHECK_FALSE(document::displacement(doc, 99999).value().has_value()); // Unknown side.

    {
        auto tx = doc.begin("same");
        tx.setDisplacement(kSide, **disp);
        tx.commit();
    }
    CHECK(doc.undoCount() == 0);
    CHECK(doc.write().value() == source);

    auto edited = **disp;
    edited.alphas[0] = 255;
    {
        auto tx = doc.begin("paint");
        tx.setDisplacement(kSide, edited);
        tx.commit();
    }
    CHECK(doc.undoCount() == 1);
    CHECK(document::displacement(doc, kSide).value() == edited);
    const auto before = lines(source), after = lines(doc.write().value());
    REQUIRE(before.size() == after.size());
    CHECK(std::ranges::count_if(std::views::iota(std::size_t{0}, before.size()),
                                [&](std::size_t i) { return before[i] != after[i]; }) == 1);
    REQUIRE(doc.undo());
    CHECK(doc.write().value() == source);

    auto bad = edited;
    bad.power = 9;
    auto tx = doc.begin("bad");
    CHECK_THROWS_AS(tx.setDisplacement(kSide, bad), std::invalid_argument);
    CHECK_THROWS_AS(tx.setDisplacement(99999, edited), std::invalid_argument);
}
