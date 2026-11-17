// Plan 0.8.9 D16 oracle: Apex's detail-prop placement against the dprp lump P2CE's Windows vbsp.exe
// compiles from tests/fixtures/detail/detail_probe.vmf (written by tools/detail_probe.py --fixture).
//
// The map has one displaced face, so every field VBSP writes is reproducible: the count and the
// model and its settings exactly, origins and orientations to float precision, and the Gaussian
// sprite scale too (it is only off for faces after the first). The leaf and the lump order (VBSP sorts
// by leaf with an unstable qsort) are not compared.
#include "core/kv1.hpp"
#include "core/math.hpp"
#include "document/displacement.hpp"
#include "geometry/detail_props.hpp"
#include "geometry/displacement.hpp"
#include "geometry/solid_geometry.hpp"
#include "render/detail_mirror.hpp"

#include <doctest/doctest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace apex;
namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

struct OracleObject {
    Vec3 origin, angles;
    int model, sway, shapeAngle, shapeSize, orientation, type;
    float scale;
};

struct Oracle {
    std::vector<std::array<float, 8>> sprites;
    std::vector<OracleObject> objects;
};

Oracle readOracle() {
    std::ifstream input(fs::path(APEX_FIXTURE_DIR) / "detail/dprp.txt");
    REQUIRE(input.good());
    Oracle oracle;
    std::string line, word;
    while (std::getline(input, line)) {
        std::istringstream in(line);
        in >> word;
        if (word == "sprite") {
            auto& sprite = oracle.sprites.emplace_back();
            for (auto& v : sprite) in >> v;
        } else if (word == "object") {
            OracleObject o{};
            int leaf = 0;
            in >> o.origin.x >> o.origin.y >> o.origin.z >> word >> o.angles.x >> o.angles.y >> o.angles.z;
            in >> word >> o.model >> word >> leaf >> word >> o.sway >> word >> o.shapeAngle >> o.shapeSize;
            in >> word >> o.orientation >> word >> o.type >> word >> o.scale;
            REQUIRE(!in.fail());
            oracle.objects.push_back(o);
        }
    }
    return oracle;
}

struct Probe {
    geometry::Displacement disp;
    std::array<DVec3, 4> corners;
    int sideId = 0;
};

// The displaced side of the probe VMF: the top of the block (-384,-384,-32)-(128,128,0).
Probe readProbe() {
    auto vmf = kv1::parse(slurp(fs::path(APEX_FIXTURE_DIR) / "detail/detail_probe.vmf"), kv1::EscapeMode::Legacy);
    REQUIRE(vmf);
    const auto* world = vmf->root().find("world");
    REQUIRE(world);
    for (const auto* solid : world->findAll("solid"))
        for (const auto* side : solid->findAll("side"))
            if (const auto* dispinfo = side->find("dispinfo")) {
                auto disp = document::parseDisplacement(*dispinfo);
                REQUIRE(disp);
                const int sideId = side->find("id")->asInt().value();
                // The corners as the editor gets them, so the order and the start corner are tested too.
                const auto geometry = geometry::solidGeometry(*solid);
                REQUIRE(geometry);
                const auto& poly = geometry->poly;
                const auto face = std::ranges::find(poly.faces, static_cast<std::uint32_t>(sideId), &geometry::Face::sideId);
                REQUIRE(face != poly.faces.end());
                const auto corners = geometry::displacementCorners(poly, *face);
                REQUIRE(corners);
                return {std::move(*disp), *corners, sideId};
            }
    FAIL("no displaced side in the probe VMF");
    return {};
}

const geometry::DetailType& grass(const geometry::DetailVbsp& vbsp) {
    const auto* type = vbsp.find("apexprobe_grass");
    REQUIRE(type);
    return *type;
}

} // namespace

TEST_CASE("detail.vbsp parses as VBSP's ParseDetailObjectFile stores it") {
    const auto vbsp = geometry::parseDetailVbsp(slurp(fs::path(APEX_FIXTURE_DIR) / "detail/detail.vbsp"));
    REQUIRE(vbsp);
    REQUIRE(vbsp->types.size() == 2);
    const auto& type = grass(*vbsp);
    CHECK(type.density == 4000.0f);
    REQUIRE(type.groups.size() == 2);
    CHECK(type.groups[0].alpha == 0.0f);
    CHECK(type.groups[1].alpha == 1.0f);
    const auto& tall = type.groups[0].models.at(0);
    const auto& small = type.groups[0].models.at(1);
    CHECK(tall.type == geometry::DetailPropType::Sprite);
    CHECK(tall.amount == doctest::Approx(0.6f));
    CHECK(small.amount == doctest::Approx(0.9f)); // Cumulative, and under 1 so not renormalised.
    CHECK(tall.sway == 255);
    CHECK(small.upright);
    CHECK(small.maxCosAngle == -1.0f); // minAngle and maxAngle default to 180.
    CHECK(tall.pos[0] == Vec2{-12.0f, 32.0f});
    CHECK(tall.pos[1] == Vec2{12.0f, 0.0f});
    const auto& cross = type.groups[1].models.at(0);
    CHECK(cross.type == geometry::DetailPropType::ShapeCross);
    CHECK(cross.shapeAngle == 30);
    CHECK(cross.shapeSize == 127);

    CHECK_FALSE(geometry::parseDetailVbsp("d { t { g { m { \"sprite\" \"0 0 64\" } } } }"));
}

TEST_CASE("detail props on a displacement match the Windows vbsp's dprp lump") {
    const auto vbsp = geometry::parseDetailVbsp(slurp(fs::path(APEX_FIXTURE_DIR) / "detail/detail.vbsp"));
    REQUIRE(vbsp);
    const auto& type = grass(*vbsp);
    const auto probe = readProbe();
    const auto oracle = readOracle();
    const auto props = geometry::placeDetailProps(probe.corners, probe.disp, type, probe.sideId);
    REQUIRE(props);
    CHECK(props->size() == oracle.objects.size());

    // The lump's sprite dictionary entry names the model.
    const auto modelOf = [&](int sprite) -> const geometry::DetailModel* {
        const auto& s = oracle.sprites.at(static_cast<std::size_t>(sprite));
        for (const auto& group : type.groups)
            for (const auto& model : group.models)
                if (model.pos[0] == Vec2{s[0], s[1]} && model.pos[1] == Vec2{s[2], s[3]} &&
                    model.tex[0] == Vec2{s[4], s[5]} && model.tex[1] == Vec2{s[6], s[7]})
                    return &model;
        return nullptr;
    };
    std::vector<bool> used(props->size());
    int unmatched = 0, misoriented = 0, misscaled = 0;
    for (const auto& o : oracle.objects) {
        const auto* model = modelOf(o.model);
        REQUIRE(model);
        CHECK(o.type == static_cast<int>(model->type));
        CHECK(o.orientation == model->orientation);
        CHECK(o.sway == model->sway);
        CHECK(o.shapeAngle == model->shapeAngle);
        CHECK(o.shapeSize == model->shapeSize);
        std::size_t found = props->size();
        for (std::size_t i = 0; i < props->size(); ++i) {
            const auto& p = (*props)[i];
            const auto& m = type.groups[p.group].models[p.model];
            if (!used[i] && &m == model && glm::distance(p.origin, o.origin) < 1e-3f) {
                found = i;
                break;
            }
        }
        if (found == props->size()) {
            ++unmatched;
            continue;
        }
        used[found] = true;
        const auto& p = (*props)[found];
        // Angles wrap and alias, so compare the orientations they give.
        const auto want = angleVectors(DVec3(o.angles)), got = angleVectors(DVec3(p.angles));
        if (glm::distance(want.forward, got.forward) > 1e-3 || glm::distance(want.up, got.up) > 1e-3) ++misoriented;
        if (std::fabs(p.scale - o.scale) > 1e-5f * o.scale) ++misscaled;
    }
    CHECK(unmatched == 0);
    CHECK(misoriented == 0);
    CHECK(misscaled == 0);
}

TEST_CASE("the detail-prop preview reads the displaced side and draws one quad set per placed prop") {
    const auto vbsp = geometry::parseDetailVbsp(slurp(fs::path(APEX_FIXTURE_DIR) / "detail/detail.vbsp"));
    REQUIRE(vbsp);
    const auto& type = grass(*vbsp);
    const auto probe = readProbe();
    const auto oracle = readOracle();

    auto vmf = kv1::parse(slurp(fs::path(APEX_FIXTURE_DIR) / "detail/detail_probe.vmf"), kv1::EscapeMode::Legacy);
    REQUIRE(vmf);
    std::vector<render::DetailFace> faces;
    for (const auto* solid : vmf->root().find("world")->findAll("solid"))
        for (auto& face : render::detailFaces(*solid)) faces.push_back(std::move(face));
    REQUIRE(faces.size() == 1);
    CHECK(faces[0].seed == probe.sideId);
    CHECK(faces[0].corners == probe.corners);
    CHECK(faces[0].material == "apexprobe/grass");

    render::DetailPlacement out;
    out.origin = faces[0].corners[0];
    REQUIRE(render::appendDetailProps(faces[0], type, out));
    CHECK(out.props == oracle.objects.size());
    const auto props = geometry::placeDetailProps(probe.corners, probe.disp, type, probe.sideId);
    REQUIRE(props);
    std::size_t quads = 0;
    for (const auto& p : *props) {
        const auto kind = type.groups[p.group].models[p.model].type;
        quads += kind == geometry::DetailPropType::Sprite ? 1 : kind == geometry::DetailPropType::ShapeCross ? 2
               : kind == geometry::DetailPropType::ShapeTri ? 3 : 0;
    }
    CHECK(out.vertices.size() == 4 * quads);
    CHECK(out.indices.size() == 12 * quads); // Two-sided.
    CHECK(out.models.empty());
    // Each sprite stands on its placed origin: the bottom edge (pos y = 0) passes through it.
    const auto& first = props->front();
    const auto& model = type.groups[first.group].models[first.model];
    if (model.type == geometry::DetailPropType::Sprite) {
        const DVec3 lower = DVec3(out.vertices[2].position[0], out.vertices[2].position[1], out.vertices[2].position[2]) +
                            out.origin;
        const DVec3 left = DVec3(out.vertices[3].position[0], out.vertices[3].position[1], out.vertices[3].position[2]) +
                           out.origin;
        CHECK(glm::distance(0.5 * (lower + left), DVec3(first.origin)) < 1e-3);
    }
}

TEST_CASE("detail model props become model transforms on the placed origin and angles") {
    const auto vbsp = geometry::parseDetailVbsp(R"(detail.vbsp {
        apexprobe_grass {
            "density" "4000.0"
            "Group1" { "alpha" "0" "rock" { "amount" "1" "model" "models/props/rock.mdl" } }
            "Group2" { "alpha" "1" "stone" { "amount" "1" "model" "models/props/stone.mdl" "upright" "1" } }
        }
    })");
    REQUIRE(vbsp);
    const auto& type = grass(*vbsp);
    const auto probe = readProbe();
    render::DetailFace face{probe.corners, probe.disp, probe.sideId, "apexprobe/grass"};
    render::DetailPlacement out;
    out.origin = face.corners[0];
    REQUIRE(render::appendDetailProps(face, type, out));
    const auto props = geometry::placeDetailProps(probe.corners, probe.disp, type, probe.sideId);
    REQUIRE(props);
    REQUIRE(!props->empty());
    CHECK(out.props == props->size());
    CHECK(out.vertices.empty());
    REQUIRE(out.models.size() == props->size());
    for (std::size_t i = 0; i < props->size(); ++i) {
        const auto& p = (*props)[i];
        const auto& m = out.models[i];
        CHECK(m.model == type.groups[p.group].models[p.model].model);
        CHECK(glm::distance(DVec3(m.transform[3]), DVec3(p.origin)) < 1e-6);
        // Columns forward, left, up, as document::entityPose builds a prop_static's.
        const auto axes = angleVectors(DVec3(p.angles));
        CHECK(glm::distance(DVec3(m.transform[0]), axes.forward) < 1e-9);
        CHECK(glm::distance(DVec3(m.transform[1]), -axes.right) < 1e-9);
        CHECK(glm::distance(DVec3(m.transform[2]), axes.up) < 1e-9);
    }
}
