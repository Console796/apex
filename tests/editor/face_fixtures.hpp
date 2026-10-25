#pragma once
#include "fixtures.hpp"
#include "editor/editor_session.hpp"
#include "geometry/solid_block.hpp"

namespace face_test {
using namespace editor_test;
struct Scene {
    Document doc{parsed()};
    EditorSession session;
    Scene() {
        auto tx = doc.begin("Authored UV box");
        tx.create(geometry::solidBlock(geometry_test::build(geometry_test::boxSides()), {"apex/base"}));
        tx.commit();
        session.attach(doc);
        session.perform("tool.apply");
        session.faceTextureSize = [](std::string_view) { return std::pair{128, 64}; };
    }
    static Ray ray(DVec3 normal) { return {normal * 256.0, -normal}; }
    SideId side(DVec3 normal) const { return * session.picks().first(ray(normal)) -> side; }
    FaceSnapshot face(DVec3 normal) const { return * faceSnapshot(doc, side(normal)); }
};
inline ToolView view(Ray ray) {
    ToolView v;
    v.cursorRay = ray; v.cameraPosition = ray.origin; v.cameraForward = ray.dir;
    v.cameraUp = std::abs(ray.dir.z) > .9 ? DVec3{0, 1, 0} : DVec3{0, 0, 1};
    v.size = {800, 600};
    return v;
}
inline InputEvent click(PointerButton button = PointerButton::Left, bool ctrl = false, bool shift = false, bool alt = false) {
    InputEvent e;
    e.kind = InputKind::Press; e.button = button; e.source = 1;
    e.ctrl = ctrl; e.shift = shift; e.alt = alt;
    e.left = button == PointerButton::Left; e.right = button == PointerButton::Right;
    return e;
}
inline std::map<std::string, std::string> keys(const FaceSnapshot& f) {
    std::map<std::string, std::string> r;
    for (const auto& k : f.keys.children()) if (k.kind() == kv1::NodeKind::Value) r[std::string(k.key())] = k.decodedValue().value();
    return r;
}
inline std::set<std::string> changed(const FaceSnapshot& a, const FaceSnapshot& b) {
    auto x = keys(a), y = keys(b);
    std::set<std::string> result;
    for (auto [k, v] : x) if (!y.contains(k) || y[k] != v) result.insert(k);
    for (auto [k, v] : y) if (!x.contains(k)) result.insert(k);
    return result;
}
inline void edgeEqual(const FaceSnapshot& a, const FaceSnapshot& b) {
    std::size_t count = 0;
    for (auto p : a.points) for (auto q : b.points) if (glm::length(p - q)<1e-6) {
        // The texture repeats: whole textures apart is the same texel.
        const auto d = geometry::uvAt(a.face, p, 128, 64) - geometry::uvAt(b.face, p, 128, 64);
        CHECK(glm::length(d - glm::round(d)) < 1e-8);
++ count;
    }
    CHECK(count >= 2);
}
} // namespace face_test
