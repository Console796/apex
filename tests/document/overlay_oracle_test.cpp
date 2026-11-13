// Plan 0.8.6 D10 oracle: Apex's overlay parse and clip against what the native P2CE vbsp compiles
// from the same corpus VMFs (tests/fixtures/vmf/overlay/corpus_overlays.vbsp.txt, written by
// tools/overlay_oracle.py from the OVERLAYS lump).
//
// VBSP does not clip; it stores the quad and the compiled faces made from the listed sides, and the
// engine clips at load. So the quad, basis and texture range must match exactly (to float
// precision), and the area Apex clips onto the brush faces must be at least the area the same quad
// covers on the compiled faces: CSG and the BSP split remove pieces of brush faces, never add.
#include "../editor/corpus.hpp"
#include "document/overlay_fragments.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

using namespace editor_test;
namespace fs = std::filesystem;

namespace {

struct BspFace {
    DVec3 normal;
    std::vector<DVec3> points;
};
struct BspOverlay {
    geometry::OverlayQuad quad;
    std::vector<BspFace> faces;
};
struct BspMap {
    std::string path;
    std::vector<BspOverlay> overlays;
};

std::vector<BspMap> oracle() {
    std::ifstream input(fs::path(APEX_FIXTURE_DIR) / "vmf/overlay/corpus_overlays.vbsp.txt");
    REQUIRE(input.good());
    std::vector<BspMap> maps;
    std::string line;
    const auto vec = [](std::istringstream& in) { DVec3 v; in >> v.x >> v.y >> v.z; return v; };
    while (std::getline(input, line)) {
        std::istringstream in(line);
        std::string word;
        in >> word;
        if (word == "map") {
            maps.emplace_back();
            in >> maps.back().path;
        } else if (word == "overlay") {
            REQUIRE(!maps.empty());
            BspOverlay overlay;
            auto& q = overlay.quad;
            std::string label;
            int id = 0, texinfo = 0, unknown0 = 0, unknown1 = 0;
            in >> id >> texinfo >> unknown0 >> unknown1 >> label >> q.startU >> q.endU >> q.startV >> q.endV >> label;
            CHECK(unknown0 == 0);
            CHECK(unknown1 == -1);
            for (auto& p : q.points) in >> p.x >> p.y;
            in >> label;
            q.basisU = vec(in);
            in >> label;
            q.origin = vec(in);
            in >> label;
            q.normal = vec(in);
            q.basisV = glm::cross(q.normal, q.basisU); // The engine rebuilds it (P0 notes).
            maps.back().overlays.push_back(std::move(overlay));
        } else if (word == "face") {
            REQUIRE(!maps.empty());
            REQUIRE(!maps.back().overlays.empty());
            BspFace face;
            int index = 0;
            std::string label;
            in >> index >> label;
            face.normal = vec(in);
            in >> label;
            double x, y, z;
            while (in >> x >> y >> z) face.points.push_back({x, y, z});
            maps.back().overlays.back().faces.push_back(std::move(face));
        }
    }
    return maps;
}

bool hiddenNode(const Document& doc, NodeId id) {
    for (NodeId parent = doc.get(*doc.handle(id))->parent; parent;) {
        const auto* node = doc.get(*doc.handle(parent));
        if (node->block && node->block->key() == "hidden") return true;
        parent = node->parent;
    }
    return false;
}

bool close(DVec3 a, DVec3 b, double eps) { return glm::length(a - b) <= eps; }

bool sameQuad(const geometry::OverlayQuad& apex, const geometry::OverlayQuad& bsp) {
    constexpr double kPosition = 0.01, kDirection = 1e-4;
    if (!close(apex.origin, bsp.origin, kPosition) || !close(apex.normal, bsp.normal, kDirection) ||
        !close(apex.basisU, bsp.basisU, kDirection))
        return false;
    for (std::size_t i = 0; i < 4; ++i)
        if (glm::length(apex.points[i] - bsp.points[i]) > kPosition) return false;
    return true;
}

void checkMaps(std::size_t part, std::size_t parts) {
    const char* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) {
        MESSAGE("APEX_P2CE_DIR unset; skipping the overlay oracle");
        return;
    }
    const auto root = fs::path(install) / "sdk_content/maps";
    const auto maps = oracle();
    REQUIRE(maps.size() >= 18);
    std::size_t matched = 0, nested = 0, hidden = 0, faceless = 0;
    double worstShortfall = 0.0;
    for (std::size_t m = part; m < maps.size(); m += parts) {
        const auto& map = maps[m];
        CAPTURE(map.path);
        Document doc(readCorpus(root / map.path));
        std::vector<bool> used(map.overlays.size(), false);
        document::SurfaceLookup surfaces(doc);
        for (const auto id : document::entityNodes(doc)) {
            const auto& entity = *doc.get(*doc.handle(id))->block;
            if (document::overlayKind(entity) != document::OverlayKind::Overlay) continue;
            const auto quad = geometry::parseOverlay(entity);
            REQUIRE(quad);
            const auto geometry = document::overlayGeometry(surfaces, entity);
            if (hiddenNode(doc, id)) {
                // VBSP leaves entities in hidden{} wrappers out of the compile.
                ++hidden;
                CHECK(std::ranges::none_of(map.overlays, [&](const auto& o) { return sameQuad(*quad, o.quad); }));
                continue;
            }
            if (geometry.listed.empty()) continue; // VBSP drops an overlay with no faces (D11 rule 4).
            CAPTURE(id);
            std::size_t found = map.overlays.size();
            for (std::size_t i = 0; i < map.overlays.size(); ++i)
                if (!used[i] && sameQuad(*quad, map.overlays[i].quad)) { found = i; break; }
            REQUIRE_MESSAGE(found < map.overlays.size(), "no compiled overlay matches the VMF's quad");
            used[found] = true;
            ++matched;
            const auto& bsp = map.overlays[found];
            CHECK(quad->startU == doctest::Approx(bsp.quad.startU));
            CHECK(quad->endU == doctest::Approx(bsp.quad.endU));
            CHECK(quad->startV == doctest::Approx(bsp.quad.startV));
            CHECK(quad->endV == doctest::Approx(bsp.quad.endV));
            // The VMF's BasisV is the one the engine rebuilds (no corpus overlay is mirrored).
            CHECK(close(quad->basisV, bsp.quad.basisV, 1e-4));
            CHECK(geometry.missing.empty());
            // A listed side may compile to no face (nodraw, or removed as hidden), so the face
            // count is not compared; only the area the quad lands on.
            std::ostringstream detail;
            double compiled = 0.0;
            for (const auto& face : bsp.faces)
                if (const auto fragment = geometry::clipOverlay(bsp.quad, face.points, face.normal)) {
                    compiled += geometry::fragmentArea(*fragment);
                    detail << " bsp(" << face.points.front().z << ")=" << geometry::fragmentArea(*fragment);
                }
            double apex = 0.0;
            for (const auto& piece : geometry.pieces) {
                apex += geometry::fragmentArea(piece.fragment);
                detail << " side" << piece.fragment.side << "=" << geometry::fragmentArea(piece.fragment);
            }
            CAPTURE(detail.str());
            if (bsp.faces.empty()) {
                // Its sides compiled to nothing (outside the sealed probe map, or nodraw): VBSP
                // keeps the overlay with no faces, so there is nothing to compare.
                ++faceless;
                continue;
            }
            CHECK(compiled > 0.0);
            // BSP vertices are floats: allow 5e-4 of the area for their rounding.
            CHECK(apex >= compiled * (1.0 - 5e-4) - 1e-3);
            worstShortfall = std::max(worstShortfall, (compiled - apex) / std::max(compiled, 1.0));
        }
        // Compiled overlays Apex did not match come from nested func_instances, which are not in
        // this VMF's own entities.
        for (std::size_t i = 0; i < used.size(); ++i) nested += used[i] ? 0 : 1;
    }
    MESSAGE("overlay oracle part ", part, ": ", matched, " matched, ", hidden, " hidden (not compiled), ", nested,
            " from nested instances, ", faceless,
            " compiled with no faces; worst area shortfall ", worstShortfall);
    CHECK(matched > 0);
}

} // namespace

TEST_CASE("[overlay][oracle] corpus overlays match the native vbsp's OVERLAYS lump (part 1 of 2)") { checkMaps(0, 2); }
TEST_CASE("[overlay][oracle] corpus overlays match the native vbsp's OVERLAYS lump (part 2 of 2)") { checkMaps(1, 2); }
