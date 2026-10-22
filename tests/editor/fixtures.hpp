#pragma once

#include "editor/pick_index.hpp"
#include "../geometry/fixtures.hpp"

#include <iomanip>
#include <sstream>

namespace editor_test {

using namespace apex;
using namespace apex::document;
using namespace apex::editor;

inline vmf::VmfFile parsed(std::string_view text = "") {
    auto result = vmf::parse(text);
    REQUIRE(result);
    return std::move(*result);
}

inline kv1::Node block(std::string text) {
    auto result = kv1::parse(std::move(text));
    REQUIRE(result);
    REQUIRE(result->root().children().size() == 1);
    return result->root().children()[0];
}

inline kv1::Node solidBlock(const geometry::ConvexPolyhedron& poly) {
    auto solid = kv1::Node::block("solid");
    for (const auto& face : poly.faces) {
        auto side = kv1::Node::block("side");
        side.append(kv1::Node::value("id", std::to_string(face.sideId))).value();
        const auto points = poly.sourcePlanePoints(face);
        REQUIRE(points);
        std::ostringstream text;
        text << std::setprecision(17);
        for (const auto point : *points) text << '(' << point.x << ' ' << point.y << ' ' << point.z << ") ";
        side.append(kv1::Node::value("plane", text.str())).value();
        solid.append(std::move(side)).value();
    }
    return solid;
}

inline NodeHandle addBox(Document& doc, DVec3 min, DVec3 max, std::optional<NodeHandle> parent = {}) {
    auto tx = doc.begin("box");
    auto handle = tx.create(solidBlock(geometry_test::build(geometry_test::boxSides(min, max))), parent);
    tx.commit();
    return handle;
}

inline NodeHandle addBlock(Document& doc, std::string text, std::optional<NodeHandle> parent = {}) {
    auto tx = doc.begin("block");
    auto handle = tx.create(block(std::move(text)), parent);
    tx.commit();
    return handle;
}

inline NodeId id(const Document& doc, NodeHandle handle) { return doc.get(handle)->id; }

inline std::vector<NodeHandle> nodes(const Document& doc) {
    std::vector<NodeHandle> result;
    const auto walk = [&](const auto& self, NodeId node) -> void {
        const auto h = *doc.handle(node);
        result.push_back(h);
        for (const auto child : doc.get(h)->children) self(self, child);
    };
    for (const auto root : doc.roots()) walk(walk, root);
    return result;
}

inline std::vector<Plane> planes(DVec3 min, DVec3 max) {
    std::vector<Plane> result;
    for (const auto& side : geometry_test::boxSides(min, max)) result.push_back(side.plane);
    return result;
}

inline void checkPoint(DVec3 actual, DVec3 expected) {
    CHECK(glm::length(actual - expected) < 1e-6);
}

inline void translate(geometry::SolidGeometry& geometry, DVec3 delta) {
    for (auto& vertex : geometry.poly.vertices) vertex += delta;
    for (auto& face : geometry.poly.faces) {
        const auto plane = geometry.poly.derivePlane(face);
        REQUIRE(plane);
        face.plane = *plane;
    }
}

} // namespace editor_test
