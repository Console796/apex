#include "fixtures.hpp"
#include "geometry/face_matching.hpp"
#include "../../tools/replane/vmf_geometry.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace geometry_test;
namespace fs = std::filesystem;

namespace {

std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE_MESSAGE(input.good(), path.string());
    return {std::istreambuf_iterator<char>(input), {}};
}

std::string reasons(const std::vector<Problem>& problems) {
    const std::map<ProblemCode, std::string> names{
        {ProblemCode::Empty, "Empty"}, {ProblemCode::InvalidPlane, "InvalidPlane"},
        {ProblemCode::InvalidVertex, "InvalidVertex"}, {ProblemCode::InvalidIndex, "InvalidIndex"},
        {ProblemCode::DegenerateFace, "DegenerateFace"}, {ProblemCode::DegenerateVolume, "DegenerateVolume"},
        {ProblemCode::ShortEdge, "ShortEdge"}, {ProblemCode::NonPlanarFace, "NonPlanarFace"},
        {ProblemCode::InvertedWinding, "InvertedWinding"}, {ProblemCode::NonConvex, "NonConvex"},
        {ProblemCode::OpenEdge, "OpenEdge"}, {ProblemCode::EulerCharacteristic, "EulerCharacteristic"},
        {ProblemCode::Unbounded, "Unbounded"}};
    std::set<std::string> unique;
    for (const auto& problem : problems) unique.insert(names.at(problem.code));
    std::string result;
    for (const auto& reason : unique) {
        if (!result.empty()) result += ',';
        result += reason;
    }
    return result;
}

} // namespace

TEST_CASE("geometry P2CE corpus construction point data replanes and face identity") {
    const char* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) {
        MESSAGE("APEX_P2CE_DIR is unset; skipping geometry corpus cases 1, 2, 3 and 6");
        return;
    }
    const fs::path root = fs::path(install) / "sdk_content/maps";
    REQUIRE(fs::is_directory(root));
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".vmf") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    REQUIRE_FALSE(files.empty());
    std::set<std::string> expected, actual;
    std::istringstream list(read(fs::path(APEX_FIXTURE_DIR) / "geometry/expected_invalid.txt"));
    for (std::string line; std::getline(list, line);) {
        if (line.empty() || line.front() == '#') continue;
        REQUIRE(expected.insert(line).second);
    }
    std::size_t solids = 0, built = 0, planeBuiltCount = 0, pointFaces = 0;
    std::map<std::string, std::size_t> invalidReasons;
    for (const auto& path : files) {
        CAPTURE(path.string());
        const auto bytes = read(path);
        auto file = apex::vmf::parse(bytes);
        if (!file) file = apex::vmf::parse(bytes, {apex::kv1::EscapeMode::Legacy});
        REQUIRE_MESSAGE(file.has_value(), (file ? "" : file.error().what()));
        apex::vmf_geometry::visitFile(*file, [&](const apex::vmf::Solid& solid) {
            ++solids;
            REQUIRE(solid.id());
            const auto key = path.lexically_relative(root).generic_string() + "\t" +
                             std::string(solid.id()->raw());
            CAPTURE(key);
            const auto sides = apex::vmf_geometry::sides(solid);
            const auto planeBuilt = ConvexPolyhedron::fromPlanes(apex::vmf_geometry::planes(sides));
            const auto polyhedron = ConvexPolyhedron::fromPointData(sides);
            std::string reason;
            if (!polyhedron) reason = "reconciled:" + reasons(polyhedron.error());
            if (!planeBuilt) {
                if (!reason.empty()) reason += ';';
                reason += "planes:" + reasons(planeBuilt.error());
            }
            if (polyhedron) ++built;
            if (planeBuilt) {
                ++planeBuiltCount;
                bool mismatch = false;
                for (const auto& side : sides) {
                    if (side.points.empty()) continue;
                    ++pointFaces;
                    const auto face = std::find_if(planeBuilt->faces.begin(), planeBuilt->faces.end(),
                        [&](const auto& f) { return f.sideId == side.sideId; });
                    if (face == planeBuilt->faces.end()) {
                        mismatch = true;
                        continue;
                    }
                    mismatch |= side.points.size() != face->vertices.size();
                    // Check both directions, including solids rejected by reconciliation.
                    for (const auto point : side.points) {
                        mismatch |= !std::any_of(face->vertices.begin(), face->vertices.end(),
                            [&](auto index) {
                                return glm::length(point - planeBuilt->vertices[index]) <=
                                       kPointDataTrustEps;
                            });
                    }
                    for (const auto index : face->vertices) {
                        mismatch |= !std::any_of(side.points.begin(), side.points.end(),
                            [&](auto point) {
                                return glm::length(point - planeBuilt->vertices[index]) <=
                                       kPointDataTrustEps;
                            });
                    }
                }
                if (mismatch) {
                    if (!reason.empty()) reason += ';';
                    reason += "PointDataMismatch";
                }
                if (planeBuilt->faces.size() != sides.size()) {
                    if (!reason.empty()) reason += ';';
                    reason += "RedundantSides";
                }
            }
            if (!reason.empty()) {
                actual.insert(key + "\t" + reason);
                ++invalidReasons[reason];
            }
            if (!planeBuilt) return;
            // Exercise cases 3/6 even when rounded point_data fails strict validation.
            const auto& original = polyhedron ? *polyhedron : *planeBuilt;
            auto rebuilt = build(rederivedSides(original));
            sameVertices(rebuilt, original);
            const auto matches = matchFaces(original, rebuilt);
            CHECK(matches.unmatchedNew.empty());
            CHECK(matches.vanishedOld.empty());
            REQUIRE(matches.matched.size() == original.faces.size());
            for (const auto& match : matches.matched) {
                sameAttributes(rebuilt.faces[match.newIndex], original.faces[match.oldIndex]);
            }
        });
    }
    MESSAGE("Geometry corpus: ", files.size(), " VMFs, ", solids, " solids, ", planeBuiltCount,
            " plane-built, ", built, " reconciled, ", solids - built, " rejected; ", actual.size(),
            " inventoried exceptions; ", pointFaces, " point_data faces compared");
    for (const auto& [reason, count] : invalidReasons) MESSAGE(reason, ": ", count);
    for (const auto& entry : actual) CHECK_MESSAGE(expected.contains(entry), "new invalid: ", entry);
    for (const auto& entry : expected) CHECK_MESSAGE(actual.contains(entry), "vanished invalid: ", entry);
}
