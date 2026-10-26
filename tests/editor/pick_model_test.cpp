#include "fixtures.hpp"
#include "editor/editor_session.hpp"
#include "editor/placement.hpp"

using namespace editor_test;

namespace {

struct Mesh {
    std::vector<DVec3> positions;
    std::vector<std::uint32_t> indices;

    void box(DVec3 min, DVec3 max) {
        const auto poly = geometry_test::build(geometry_test::boxSides(min, max));
        const auto offset = static_cast<std::uint32_t>(positions.size());
        positions.insert(positions.end(), poly.vertices.begin(), poly.vertices.end());
        for (const auto& face : poly.faces) {
            for (std::size_t i = 1; i + 1 < face.vertices.size(); ++i) {
                for (const auto vertex : {face.vertices[0], face.vertices[i], face.vertices[i + 1]})
                    indices.push_back(offset + static_cast<std::uint32_t>(vertex));
            }
        }
    }
    std::shared_ptr<const PickModel> model(std::vector<PickModelBodyPart> parts = {}) {
        return std::make_shared<const PickModel>(std::move(positions), std::move(indices), std::move(parts));
    }
};

EntityBoundsProvider provider(std::shared_ptr<const PickModel> model) {
    return [model](const kv1::Node& block) -> std::optional<EntityPickShape> {
        const auto* body = block.find("body");
        return EntityPickShape{model, body ? static_cast<std::uint32_t>(body->asInt().value_or(0)) : 0, {}};
    };
}

std::optional<Hit> bruteTriangles(const Mesh& mesh, DMat4 transform, Ray ray) {
    std::optional<Hit> best;
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
        const auto a = DVec3{transform * DVec4{mesh.positions[mesh.indices[i]], 1}};
        const auto b = DVec3{transform * DVec4{mesh.positions[mesh.indices[i + 1]], 1}};
        const auto c = DVec3{transform * DVec4{mesh.positions[mesh.indices[i + 2]], 1}};
        const auto normal = glm::normalize(glm::cross(b - a, c - a));
        const double denominator = glm::dot(normal, ray.dir);
        if (std::abs(denominator) < 1e-12) continue;
        const double t = glm::dot(a - ray.origin, normal) / denominator;
        if (t < 0 || (best && t >= best->t)) continue;
        const auto point = ray.at(t);
        if (glm::dot(glm::cross(b - a, point - a), normal) < -1e-8 ||
            glm::dot(glm::cross(c - b, point - b), normal) < -1e-8 ||
            glm::dot(glm::cross(a - c, point - c), normal) < -1e-8) continue;
        best = Hit{0, {}, point, normal, t};
    }
    return best;
}

} // namespace

TEST_CASE("Pick model railing gaps and two sided bar hits preserve point normal and t") {
    Mesh mesh;
    mesh.box({-8, -1, 0}, {-6, 1, 12});
    mesh.box({6, -1, 0}, {8, 1, 12});
    mesh.box({-8, -1, 10}, {8, 1, 12});
    Document doc(parsed());
    const auto prop = addBlock(doc, "entity { classname prop_static origin \"100 20 30\" }");
    PickIndex index(doc, provider(mesh.model()));
    CHECK_FALSE(index.first({{100, 0, 35}, {0, 2, 0}}));
    CHECK(index.pick({{100, 0, 35}, {0, 2, 0}}).empty());
    for (const auto sign : {-1.0, 1.0}) {
        const Ray ray{{107, 20 - 20 * sign, 35}, {0, 2 * sign, 0}};
        const auto hit = index.first(ray);
        REQUIRE(hit);
        CHECK(hit->node == id(doc, prop));
        CHECK_FALSE(hit->side);
        CHECK(hit->t == doctest::Approx(9.5));
        checkPoint(hit->point, {107, 20 - sign, 35});
        checkPoint(hit->normal, {0, -sign, 0});
        CHECK(index.pick(ray, 9.49).empty());
        REQUIRE(index.pick(ray, 9.5).size() == 1);
    }
}

TEST_CASE("Pick model mixed pitch yaw roll and scale match world triangle brute force") {
    Mesh mesh;
    mesh.box({-8, -2, -3}, {12, 4, 7});
    const auto model = std::make_shared<const PickModel>(mesh.positions, mesh.indices);
    for (const auto angles : {"30 45 60", "-23 128 -41", "89 37 14"}) {
        Document doc(parsed());
        const auto prop = addBlock(doc, std::string("entity { classname prop_static origin \"65536 -200 400\" uniformscale 2.5 angles \"") + angles + "\" }");
        const auto transform = document::entityTransform(document::entityPose(*doc.get(prop)->block));
        PickIndex index(doc, provider(model));
        AABB expectedBounds;
        for (const auto p : mesh.positions) expectedBounds.expand(DVec3{transform * DVec4{p, 1}});
        REQUIRE(index.bounds(id(doc, prop)));
        checkPoint(index.bounds(id(doc, prop))->min, expectedBounds.min);
        checkPoint(index.bounds(id(doc, prop))->max, expectedBounds.max);
        for (int axis = 0; axis < 3; ++axis) {
            for (const auto sign : {-1.0, 1.0}) {
                DVec3 direction{0};
                direction[axis] = sign;
                const Ray ray{DVec3{transform * DVec4{DVec3{2, 1, 2} + direction * 40.0, 1}},
                              DVec3{transform * DVec4{-direction * 2.0, 0}}};
                const auto expected = bruteTriangles(mesh, transform, ray);
                const auto hit = index.first(ray);
                REQUIRE(expected);
                REQUIRE(hit);
                CHECK(hit->t == doctest::Approx(expected->t));
                checkPoint(hit->point, expected->point);
                checkPoint(hit->normal, expected->normal);
            }
        }
    }
}

TEST_CASE("Pick model body choices share one BVH and entity edits only refresh their own leaf") {
    Mesh mesh;
    mesh.box({-8, -2, -2}, {-4, 2, 2});
    const auto second = static_cast<std::uint32_t>(mesh.indices.size() / 3);
    mesh.box({4, -2, -2}, {8, 2, 2});
    const auto count = static_cast<std::uint32_t>(mesh.indices.size() / 3) - second;
    // The second part has an empty choice, and a zero base follows the asset decoder's base=1 rule.
    const auto model = mesh.model({{2, {{{{0, second}}}, {{{second, count}}}}}, {0, {{}}}});
    const auto* tree = &model->triangleBvh();
    Document doc(parsed());
    const auto first = addBlock(doc, "entity { classname prop_static body 0 }");
    const auto other = addBlock(doc, "entity { classname prop_static body 2 origin \"0 100 0\" }");
    for (int i = 0; i < 12; ++i) addBlock(doc, "entity { classname prop_static origin \"100 100 100\" }");
    std::size_t calls = 0;
    auto supply = provider(model);
    PickIndex index(doc, [&](const kv1::Node& block) {
        ++calls;
        const auto shape = supply(block);
        CHECK(&shape->model->triangleBvh() == tree);
        return shape;
    });
    calls = 0;
    const Ray left{{-6, -20, 0}, {0, 2, 0}}, right{{6, -20, 0}, {0, 2, 0}};
    REQUIRE(index.first(left));
    CHECK(index.first(left)->node == id(doc, first));
    REQUIRE(index.first(right));
    CHECK_FALSE(index.first(right)->node == id(doc, first));
    CHECK(index.bounds(id(doc, first))->max.x == doctest::Approx(-4));
    const Ray throughBoth{{-20, 0, 0}, {2, 0, 0}};
    REQUIRE(index.first(throughBoth));
    CHECK(index.first(throughBoth)->t == doctest::Approx(6));
    const auto otherBounds = index.bounds(id(doc, other));
    auto tx = doc.begin("body choice");
    tx.setKeyValue(first, "body", "2");
    index.apply(tx.commit());
    CHECK(calls == 1);
    CHECK_FALSE(index.first(left));
    REQUIRE(index.first(right));
    CHECK(index.first(right)->node == id(doc, first));
    CHECK(index.bounds(id(doc, first))->min.x == doctest::Approx(4));
    REQUIRE(index.first(throughBoth));
    CHECK(index.first(throughBoth)->t == doctest::Approx(12));
    checkPoint(index.first(throughBoth)->point, {4, 0, 0});
    CHECK(&model->triangleBvh() == tree);
    calls = 0;
    auto move = doc.begin("move and rotate model");
    move.setKeyValue(first, "origin", "20 0 0");
    move.setKeyValue(first, "angles", "30 45 60");
    index.apply(move.commit());
    CHECK(calls == 1);
    checkPoint(index.bounds(id(doc, other))->min, otherBounds->min);
    checkPoint(index.bounds(id(doc, other))->max, otherBounds->max);
    CHECK(&model->triangleBvh() == tree);
    const auto transform = document::entityTransform(document::entityPose(*doc.get(first)->block));
    const Ray moved{DVec3{transform * DVec4{6, -20, 0, 1}}, DVec3{transform * DVec4{0, 2, 0, 0}}};
    REQUIRE(index.first(moved));
    CHECK(index.first(moved)->node == id(doc, first));
    CHECK(index.first(moved)->t == doctest::Approx(9));
}

TEST_CASE("Pick model uint32 indices beyond 65535 and planar triangles") {
    Mesh mesh;
    mesh.positions.resize(65536, DVec3{1000});
    mesh.positions.insert(mesh.positions.end(), {{-3, -2, 5}, {3, -2, 5}, {0, 2, 5}});
    mesh.indices = {65536, 65537, 65538};
    Document doc(parsed());
    addBlock(doc, "entity { classname prop_static }");
    PickIndex index(doc, provider(mesh.model()));
    const auto hit = index.first({{0, 0, 20}, {0, 0, -3}});
    REQUIRE(hit);
    CHECK(hit->t == doctest::Approx(5));
    checkPoint(hit->point, {0, 0, 5});
    checkPoint(hit->normal, {0, 0, 1});
    const auto back = index.first({{0, 0, -10}, {0, 0, 3}});
    REQUIRE(back);
    checkPoint(back->normal, {0, 0, 1});
}

TEST_CASE("Pick model oriented marquee bounds and unloaded fallback class box") {
    Mesh mesh;
    mesh.box({-10, -1, -1}, {10, 1, 1});
    const auto model = mesh.model();
    Document doc(parsed());
    const auto prop = addBlock(doc, "entity { classname prop_static model loaded angles \"0 45 0\" }");
    const auto fallback = addBlock(doc, "entity { classname prop_static model missing origin \"30 0 0\" angles \"0 90 0\" }");
    PickIndex index(doc, [model](const kv1::Node& block) -> std::optional<EntityPickShape> {
        return EntityPickShape{block.find("model")->decodedValue().value() == "loaded" ? model : nullptr,
                               0, {{-8, -2, -2}, {8, 2, 2}}};
    });
    CHECK(index.frustum(planes({6, -7, -0.5}, {7, -6, 0.5}), Mode::Touching).empty());
    CHECK(index.frustum(planes({5, 5, -0.5}, {6, 6, 0.5}), Mode::Touching) == std::vector<NodeId>{id(doc, prop)});
    CHECK(index.frustum(planes({-8, -8, -2}, {8, 8, 2}), Mode::Enclosed) == std::vector<NodeId>{id(doc, prop)});
    CHECK(index.frustum(planes({5, 5, -0.5}, {6, 6, 0.5}), Mode::Enclosed).empty());
    const auto hit = index.first({{10, 6, 0}, {2, 0, 0}});
    REQUIRE(hit);
    CHECK(hit->node == id(doc, fallback));
    CHECK(hit->t == doctest::Approx(9));
    checkPoint(hit->normal, {-1, 0, 0});
}

TEST_CASE("placement onto picked prop triangles uses top face point and normal") {
    Mesh mesh;
    mesh.box({-10, -10, 10}, {10, 10, 20});
    const auto model = mesh.model();
    Document doc(parsed());
    const auto prop = addBlock(doc, "entity { classname prop_static angles \"20 30 10\" uniformscale 2 }");
    const auto transform = document::entityTransform(document::entityPose(*doc.get(prop)->block));
    PickIndex index(doc, provider(model));
    const auto top = DVec3{transform * DVec4{0, 0, 20, 1}};
    const auto normal = glm::normalize(DVec3{transform * DVec4{0, 0, 1, 0}});
    const auto hit = index.first({top + normal * 100.0, -normal * 2.0});
    REQUIRE(hit);
    CHECK_FALSE(hit->side);
    CHECK(hit->modelTriangle);
    checkPoint(hit->point, top);
    checkPoint(hit->normal, normal);
    EditorSession session;
    session.attach(doc);
    assets::EntityClassTable table;
    assets::EntityClass cls;
    cls.name = "test_point";
    cls.type = "PointClass";
    cls.mins = {-2, -2, -3};
    cls.maxs = {2, 2, 5};
    table.classes[cls.name] = cls;
    session.setEntityClasses(std::move(table));
    REQUIRE(session.placeEntity("test_point", hit->point, hit->normal, true));
    const auto placed = std::get<NodeId>(*session.selection().active());
    const auto pose = document::entityPose(*doc.get(*doc.handle(placed))->block);
    checkPoint(pose.origin, top + normal * 3.0);
    checkPoint(pose.basis[2], normal);
}

TEST_CASE("Pick model rejects invalid indices and body ranges") {
    CHECK_THROWS_AS(PickModel({{0, 0, 0}}, {0, 0}), std::invalid_argument);
    CHECK_THROWS_AS(PickModel({{0, 0, 0}}, {0, 1, 0}), std::invalid_argument);
    CHECK_THROWS_AS(PickModel({{0, 0, 0}}, {0, 0, 0}, {{1, {{{{0, 2}}}}}}), std::invalid_argument);
}

TEST_CASE("Pick model empty body choice removes its leaf until geometry is selected again") {
    Mesh mesh;
    mesh.box(DVec3{-2}, DVec3{2});
    const auto count = static_cast<std::uint32_t>(mesh.indices.size() / 3);
    const auto model = mesh.model({{1, {{{{0, count}}}, {}}}});
    Document doc(parsed());
    const auto prop = addBlock(doc, "entity { classname prop_static body 0 }");
    PickIndex index(doc, provider(model));
    REQUIRE(index.first({{-10, 0, 0}, {1, 0, 0}}));
    auto hide = doc.begin("empty body choice");
    hide.setKeyValue(prop, "body", "1");
    index.apply(hide.commit());
    CHECK(index.size() == 0);
    CHECK_FALSE(index.first({{-10, 0, 0}, {1, 0, 0}}));
    auto show = doc.begin("visible body choice");
    show.setKeyValue(prop, "body", "2");
    index.apply(show.commit());
    REQUIRE(index.first({{-10, 0, 0}, {1, 0, 0}}));
    CHECK(index.first({{-10, 0, 0}, {1, 0, 0}})->node == id(doc, prop));
}
