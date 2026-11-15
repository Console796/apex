// Plan 0.8.5: displacement lightmaps against the native P2CE vbsp/vrad (fixture test1_disp.lightmap.txt,
// written by tools/disp_lightmap_oracle.py; P0 note docs/plans/probe-results/displacement_lighting_0_8_5_p0.md).
#include "lighting/bake_diff.hpp"
#include "lighting/baker.hpp"
#include "lighting/bsp_lightmaps.hpp"
#include "lighting/light.hpp"

#include "document/displacement.hpp"
#include "render/lightmap_atlas.hpp"
#include "render/solid_mesh.hpp"
#include "document/vmf.hpp"
#include "geometry/solid_geometry.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

using namespace apex;
using namespace apex::lighting;

namespace {

constexpr int kSide = 355;

std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.good());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::filesystem::path dispDir() { return std::filesystem::path(APEX_FIXTURE_DIR) / "vmf" / "disp"; }

struct Variant {
    std::string name;
    std::vector<std::array<std::string, 3>> edits; // side id ("-" for dispinfo), key, value
    std::array<int, 2> mins{}, extents{};
    std::optional<std::array<double, 7>> light; // x y z r g b brightness
    std::vector<double> luxels;
};

std::vector<Variant> oracle() {
    std::istringstream in(read(dispDir() / "test1_disp.lightmap.txt"));
    std::vector<Variant> result;
    for (std::string line; std::getline(in, line);) {
        std::istringstream words(line);
        std::string kind;
        words >> kind;
        if (kind == "variant") {
            result.emplace_back();
            words >> result.back().name;
        } else if (kind == "edit" || kind == "side") {
            std::array<std::string, 3> edit;
            words >> edit[0] >> edit[1];
            std::getline(words >> std::ws, edit[2]);
            result.back().edits.push_back(edit);
        } else if (kind == "chart") {
            auto& v = result.back();
            words >> v.mins[0] >> v.mins[1] >> v.extents[0] >> v.extents[1];
        } else if (kind == "light") {
            std::array<double, 7> light{};
            for (auto& value : light) words >> value;
            result.back().light = light;
        } else if (kind == "luxels") {
            for (double value; words >> value;) result.back().luxels.push_back(value);
        } // "point" lines are VBSP's point_data; Apex builds the base face from the planes.
    }
    return result;
}

struct Built {
    vmf::VmfFile file;
    BakeFace face;
    geometry::DisplacementMesh mesh;
    std::array<DVec3, 4> gridCorners{};
};

// The fixture with a variant's edits applied, and the displaced side's BakeFace.
Built build(const Variant& variant) {
    auto parsed = vmf::parse(read(dispDir() / "test1_disp.vmf"));
    REQUIRE(parsed);
    Built result{std::move(*parsed), {}, {}, {}};
    vmf::Solid* solid = nullptr;
    for (auto& candidate : result.file.world->solids)
        for (auto& side : candidate.sides)
            if (side.id() && side.id()->asInt().value_or(0) == kSide) solid = &candidate;
    REQUIRE(solid);
    const auto sideOf = [&](int id) -> vmf::Side& {
        const auto it = std::ranges::find_if(solid->sides, [&](vmf::Side& s) { return s.id()->asInt().value_or(0) == id; });
        REQUIRE(it != solid->sides.end());
        return *it;
    };
    for (const auto& [target, key, value] : variant.edits) {
        auto* node = target == "-" ? sideOf(kSide).dispInfo->node : sideOf(std::stoi(target)).node;
        REQUIRE(node->find(key));
        REQUIRE(node->find(key)->setValue(value));
    }
    const auto disp = document::parseDisplacement(*sideOf(kSide).dispInfo->node);
    REQUIRE(disp);
    const auto geometry = geometry::solidGeometry(*solid->node);
    REQUIRE(geometry);
    const auto face = std::ranges::find(geometry->poly.faces, std::uint32_t(kSide), &geometry::Face::sideId);
    REQUIRE(face != geometry->poly.faces.end());
    result.face = bakeDisplacementFace(*face, geometry->poly.vertices, *disp);
    const auto corners = geometry::displacementCorners(geometry->poly, *face);
    REQUIRE(corners);
    result.mesh = geometry::displacementMesh(*corners, *disp).value();
    result.gridCorners = displacementGridCorners(*corners, disp->startPosition);
    return result;
}

double correlation(std::span<const double> a, std::span<const double> b) {
    const double ma = std::accumulate(a.begin(), a.end(), 0.0) / double(a.size());
    const double mb = std::accumulate(b.begin(), b.end(), 0.0) / double(b.size());
    double ab = 0, aa = 0, bb = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        ab += (a[i] - ma) * (b[i] - mb);
        aa += (a[i] - ma) * (a[i] - ma);
        bb += (b[i] - mb) * (b[i] - mb);
    }
    return ab / std::sqrt(aa * bb);
}

} // namespace

TEST_CASE("displacement charts match VBSP's mins and extents for every compiled variant") {
    const auto variants = oracle();
    REQUIRE(variants.size() == 16);
    for (const auto& variant : variants) {
        CAPTURE(variant.name);
        const auto built = build(variant);
        const auto& chart = built.face.chart;
        CHECK(chart.mins == variant.mins);
        CHECK(chart.extents == variant.extents);
        CHECK(chart.dimensions == std::array<int, 2>{variant.extents[0] + 1, variant.extents[1] + 1});
        CHECK(built.face.lattice.size() == std::size_t(chart.dimensions[0] + 1) * std::size_t(chart.dimensions[1] + 1));
        CHECK(built.face.surface.size() == 81);
        CHECK(built.face.vertices.size() == 4);
    }
}

TEST_CASE("the displacement luxel scale truncates like VBSP's float reciprocal") {
    geometry::Face face;
    face.uv.uaxis = {1, 0, 0};
    face.uv.vaxis = {0, -1, 0};
    for (const auto& [scale, expected] : {std::pair{16, 16}, {7, 6}, {13, 12}, {14, 13}, {15, 14}, {1, 1}, {0, 16}}) {
        CAPTURE(scale);
        face.lightmapScale = scale;
        CHECK(displacementLuxelScale(face) == expected);
    }
    face.uv.uaxis = {1, 0, 0.5}; // Not normalised: only |s| counts (16 / 1.118 = 14.3).
    face.lightmapScale = 16;
    CHECK(displacementLuxelScale(face) == 14);
}

TEST_CASE("the displacement lattice starts at the start corner and has unit normals") {
    const auto variants = oracle();
    for (const auto& variant : variants) {
        if (variant.name != "base" && variant.name != "start_1" && variant.name != "narrow_start_2") continue;
        CAPTURE(variant.name);
        const auto built = build(variant);
        const auto& face = built.face;
        // Corner (0, 0) surrounds luxel (0, 0) on the start corner's side, clamped 1 unit inside the edges;
        // the last corner sits likewise by the grid's last vertex.
        CHECK(glm::distance(face.lattice.front().position, face.surface.front()) < 2.0);
        CHECK(glm::distance(face.lattice.back().position, face.surface.back()) < 2.0);
        for (const auto& point : face.lattice) CHECK(glm::length(point.normal) == doctest::Approx(1.0));
    }
}

// VRAD's own luxels (point light, -bounce 0) against Apex's CPU baker on the same lattice. The baker has no
// casters here (displacements do not cast in 0.8.5), so absolute levels differ where VRAD self-shadows;
// the orientation must be the identity and the pattern must correlate.
TEST_CASE("baked displacement luxels follow VRAD's orientation on the lit variants") {
    const auto variants = oracle();
    std::size_t lit = 0;
    for (const auto& variant : variants) {
        if (!variant.light) continue;
        ++lit;
        CAPTURE(variant.name);
        const auto built = build(variant);
        auto entity = kv1::Node::block("entity");
        const auto& l = *variant.light;
        std::ostringstream origin, colour;
        origin << l[0] << ' ' << l[1] << ' ' << l[2];
        colour << l[3] << ' ' << l[4] << ' ' << l[5] << ' ' << l[6];
        for (auto [key, value] : std::vector<std::pair<std::string, std::string>>{
                 {"classname", "light"}, {"origin", origin.str()}, {"_light", colour.str()}})
            entity.append(kv1::Node::value(key, value)).value();
        BakeScene scene;
        scene.faces = {built.face};
        scene.lights = lightsFromEntity(entity);
        REQUIRE(scene.lights.size() == 1);
        Baker baker(std::move(scene), BakerOptions{.threads = 2, .skyRays = 1});
        baker.step();
        const auto radiance = baker.resolveFace(0);
        const auto [w, h] = built.face.chart.dimensions;
        REQUIRE(radiance.size() == variant.luxels.size());
        std::vector<double> apex;
        for (const auto& c : radiance) apex.push_back((c.x + c.y + c.z) / 3.0);
        // The eight lattice symmetries that keep the shape; transposes only for square charts.
        double identity = 0, best = -2;
        std::string bestName;
        for (int transpose = 0; transpose < (w == h ? 2 : 1); ++transpose)
            for (int flip = 0; flip < 4; ++flip) {
                std::vector<double> moved(apex.size());
                for (int t = 0; t < h; ++t) for (int s = 0; s < w; ++s) {
                    int x = flip & 1 ? w - 1 - s : s, y = flip & 2 ? h - 1 - t : t;
                    if (transpose) std::swap(x, y);
                    moved[std::size_t(t * w + s)] = apex[std::size_t(y * w + x)];
                }
                const double r = correlation(moved, variant.luxels);
                if (!transpose && !flip) identity = r;
                if (r > best) { best = r; bestName = std::to_string(transpose) + "/" + std::to_string(flip); }
            }
        const double level = std::accumulate(apex.begin(), apex.end(), 0.0) /
                             std::accumulate(variant.luxels.begin(), variant.luxels.end(), 0.0);
        // 0.6.2/0.7-style relative error: |apex - vrad| / max(vrad, floor), floor 1% of the 99th percentile.
        auto sorted = variant.luxels;
        std::ranges::sort(sorted);
        const double floor = 0.01 * sorted[std::size_t(0.99 * double(sorted.size() - 1))];
        std::vector<double> errors;
        for (std::size_t i = 0; i < apex.size(); ++i)
            if (apex[i] >= floor || variant.luxels[i] >= floor)
                errors.push_back(std::abs(apex[i] - variant.luxels[i]) / std::max(variant.luxels[i], floor));
        std::ranges::sort(errors);
        const double mean = std::accumulate(errors.begin(), errors.end(), 0.0) / double(errors.size());
        MESSAGE(variant.name, ": r = ", identity, " (best symmetry ", bestName, " r = ", best, "), Apex/VRAD level ",
                level, ", error mean ", mean, " p50 ", errors[errors.size() / 2], " p99 ",
                errors[std::size_t(0.99 * double(errors.size() - 1))], " over ", errors.size(), " luxels");
        CHECK(identity == best);
        CHECK(identity > 0.8);
    }
    CHECK(lit == 6);
}

TEST_CASE("VRAD displacement fragments map to their displaced side by base quad") {
    Variant base;
    const auto built = build(base);
    const auto& face = built.face;
    // A synthetic fragment as VRAD stores it: the base quad (clockwise), the chart and its luxels.
    BspFragment fragment;
    fragment.displacement = true;
    fragment.vertices = face.vertices;
    std::ranges::reverse(fragment.vertices);
    fragment.plane = {face.normal, glm::dot(face.normal, face.vertices[0])};
    for (int axis = 0; axis < 2; ++axis)
        fragment.lightmapVectors[axis] = DVec4(face.chart.worldToLuxel[axis], face.chart.offset[axis]);
    fragment.mins = face.chart.mins;
    fragment.extents = face.chart.extents;
    fragment.luxels.assign(std::size_t(face.chart.dimensions[0] * face.chart.dimensions[1]), Vec3{1.0f});

    SideOwner owner;
    owner.solidId = 9;
    owner.sideId = kSide;
    owner.plane = fragment.plane;
    owner.loop = face.vertices;
    owner.displacement = built.mesh;
    owner.gridCorners = built.gridCorners;
    SideOwner flat = owner;
    flat.displacement.reset();
    flat.sideId = 1;

    const SideOwners owners{flat, owner};
    const auto mapped = mapBspFragments(std::span(&fragment, 1), owners);
    CHECK(mapped.stats.displacement == 1);
    CHECK(mapped.stats.mapped == 1);
    REQUIRE(mapped.faces.size() == 1);
    CHECK(mapped.faces[0].sideId == std::uint32_t(kSide));
    CHECK(mapped.faces[0].displaced());
    CHECK(mapped.faces[0].chart.extents == face.chart.extents);
    CHECK(mapped.faces[0].surface.size() == 81);
    REQUIRE(mapped.faces[0].lattice.size() == face.lattice.size());
    for (std::size_t i = 0; i < face.lattice.size(); ++i)
        CHECK(glm::distance(mapped.faces[0].lattice[i].position, face.lattice[i].position) < 1e-6);

    // A flat fragment on the same plane does not map to the displaced side, only to the flat one.
    BspFragment plain = fragment;
    plain.displacement = false;
    const auto flatMapped = mapBspFragments(std::span(&plain, 1), owners);
    REQUIRE(flatMapped.faces.size() == 1);
    CHECK(flatMapped.faces[0].sideId == 1);
    CHECK_FALSE(flatMapped.faces[0].displaced());

    // Moved corners: unowned.
    fragment.vertices[0] += DVec3{8, 0, 0};
    CHECK(mapBspFragments(std::span(&fragment, 1), owners).stats.unowned == 1);
}

// Hybrid (plan 0.8.5): a sculpt changes the displaced face's geometry hash, so it alone rebakes.
TEST_CASE("an edited displacement is dirty in the bake diff, an unedited one is not") {
    Variant base;
    const auto before = build(base);
    auto sculpted = build(base);
    // Move one displaced vertex of the lattice's surface as a sculpt would (the chart is unchanged).
    BakeFace edited = sculpted.face;
    edited.surface[40].z += 16;
    edited.lattice[edited.lattice.size() / 2].position.z += 16;

    const std::vector<Light> lights; // No lights: only geometry decides.
    const std::vector<std::string> materials{"dev/dev_measuregeneric01b"};
    const std::array<BakeFace, 1> was{before.face}, same{sculpted.face}, now{edited};
    const auto keys = faceKeys(was);
    const auto unchanged = diffScenes({was, lights, materials, keys}, {same, lights, materials, keys});
    CHECK_FALSE(unchanged.full);
    CHECK(unchanged.dirtyCount == 0);
    const auto changed = diffScenes({was, lights, materials, keys}, {now, lights, materials, keys});
    CHECK_FALSE(changed.full);
    CHECK(changed.dirtyCount == 1);
}

TEST_CASE("a displaced face's atlas UVs run edge to edge over its luxels") {
    Variant base;
    const auto built = build(base);
    auto atlas = render::LightmapAtlas::pack(std::span(&built.face, 1));
    REQUIRE(atlas);
    const auto& rect = atlas->rects[0];
    const auto& chart = built.face.chart;
    const auto texel = [&](DVec2 uv) { return uv * double(atlas->size) - DVec2(rect.x, rect.y) - 0.5; };
    CHECK(texel(atlas->displacementUv(0, 0, 0, 9)) == DVec2{0, 0});
    CHECK(texel(atlas->displacementUv(0, 8, 8, 9)) == DVec2(chart.extents[0], chart.extents[1]));
    const auto middle = texel(atlas->displacementUv(0, 2, 4, 9));
    CHECK(middle.x == doctest::Approx(chart.extents[0] / 2.0));
    CHECK(middle.y == doctest::Approx(chart.extents[1] / 4.0));
    CHECK(rect.width == chart.dimensions[0]);
    CHECK(rect.height == chart.dimensions[1]);

    // Retained radiance on a restart: the same displaced face keeps its luxels by grid fraction.
    std::vector<std::vector<Vec3>> radiance{std::vector<Vec3>(std::size_t(rect.width * rect.height))};
    for (std::size_t i = 0; i < radiance[0].size(); ++i) radiance[0][i] = Vec3(float(i));
    std::vector<char> covered;
    const auto retained = atlas->retainRadiance(&*atlas, radiance, &covered);
    REQUIRE(retained.size() == 1);
    CHECK(retained[0] == radiance[0]);
}

// Baked mode draws a displaced face with its lightmap once the mesher gives its vertices the fragment's UVs.
TEST_CASE("the mesher gives displaced vertices their baked fragment's lightmap UVs") {
    Variant base;
    auto built = build(base);
    built.face.solidId = 7;
    built.face.sideId = kSide;
    auto atlas = render::LightmapAtlas::pack(std::span(&built.face, 1));
    REQUIRE(atlas);
    const auto meshOf = [&](const vmf::VmfFile& file) {
        for (const auto& solid : file.world->solids)
            for (const auto& side : solid.sides)
                if (side.id()->asInt().value_or(0) == kSide)
                    return render::meshSolid(*solid.node, [](std::string_view) { return render::TextureSize{}; },
                                             &*atlas, 7);
        FAIL("solid not found");
        return render::meshSolid(*file.world->solids.front().node, {}, nullptr, 0);
    };
    const auto check = [&](const render::SolidMesh& mesh, bool expected) {
        bool found = false;
        for (const auto& batch : mesh.batches)
            for (const auto& range : batch.faces) {
                if (range.side != kSide) continue;
                found = true;
                CHECK(range.displacement);
                CHECK(range.displacementLightmap == expected);
                if (!expected) continue;
                // The displaced segment's vertices are the grid, row-major, first in the batch.
                const auto first = batch.indices[range.firstIndex];
                const auto uv = atlas->displacementUv(0, 8, 8, 9);
                const auto& v = batch.vertices[first + 80];
                CHECK(v.lightmapUv[0] == doctest::Approx(uv.x));
                CHECK(v.lightmapUv[1] == doctest::Approx(uv.y));
                const auto start = atlas->displacementUv(0, 0, 0, 9);
                CHECK(batch.vertices[first].lightmapUv[0] == doctest::Approx(start.x));
                CHECK(batch.vertices[first].lightmapUv[1] == doctest::Approx(start.y));
            }
        CHECK(found);
    };
    const auto same = meshOf(built.file);
    REQUIRE(same);
    check(*same, true);
    // A sculpt since the bake: the old lightmap no longer fits, so the face keeps the Lit path.
    Variant moved{"moved", {{"-", "startposition", "[96 -1440 64]"}}, {}, {}, {}, {}};
    const auto edited = build(moved);
    const auto stale = meshOf(edited.file);
    REQUIRE(stale);
    check(*stale, false);
}
