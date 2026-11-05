#include "lighting/bake_session.hpp"
#include "document/document.hpp"

#include <doctest/doctest.h>
#include <chrono>
#include <set>
#include <thread>

using namespace apex;
using namespace apex::lighting;

namespace {
BakeScene scene() {
    BakeScene result;
    for (const double x : {100.0, 10.0, -10.0}) {
        BakeFace face;
        face.vertices = {{x, 0, 0}};
        face.chart.dimensions = {2, 1};
        face.chart.luxelToWorld = {DVec3{1, 0, 0}, DVec3{0, 1, 0}};
        result.faces.push_back(face);
    }
    Light light;
    light.kind = LightKind::Ambient;
    light.intensity = {1, 1, 1};
    result.lights.push_back(light);
    return result;
}
BakeSessionOptions options() {
    BakeSessionOptions result;
    result.baker.threads = 1;
    result.baker.bounces = 2; // The schedule assertions below exercise exactly two bounce levels.
    result.baker.skyRays = result.baker.bounceRays = 1;
    result.initialPasses = 1;
    result.maxPasses = 2;
    return result;
}
}

TEST_CASE("bake session defaults use the VRAD-parity bounce budget") {
    const BakeSessionOptions defaults;
    CHECK(defaults.baker.bounces == 8);
    CHECK(defaults.baker.bounceRays == 32);
    CHECK(defaults.initialPasses == 4);
    CHECK(defaults.maxPasses == 4);
}

TEST_CASE("bake session camera ordering is nearest first with stable ties") {
    const auto input = scene();
    CHECK(BakeSession::cameraOrder(input.faces, {0, 0, 0}) == std::vector<std::size_t>{1, 2, 0});
    CHECK(BakeSession::cameraOrder(input.faces, {101, 0, 0}) == std::vector<std::size_t>{0, 1, 2});
    BakeSession session(input, options());
    session.advance(1);
    // A camera move during a traversal must not restart it or starve the distant face.
    session.setCamera({101, 0, 0});
    for (int i = 0; i < 5; ++i) session.advance(1);
    CHECK(session.progress().passes[0] == 1);
    CHECK(session.progress().level == 1);
}

TEST_CASE("bake session settles direct then bounce levels then refines and idles") {
    BakeSession session(scene(), options());
    const std::vector<std::vector<unsigned>> expected{{1, 0, 0}, {1, 1, 0}, {1, 1, 1},
                                                     {2, 1, 1}, {2, 2, 1}, {2, 2, 2}};
    for (const auto& counts : expected) {
        for (int face = 0; face < 3; ++face) session.advance(100);
        CHECK(session.progress().passes == counts);
    }
    CHECK(session.progress().idle);
    session.advance(100);
    CHECK(session.progress().passes == expected.back());
}

TEST_CASE("bake session publishes only changed rows and tracks partial coverage") {
    BakeSession session(scene(), options());
    session.advance(1);
    auto publication = session.tick(CancelToken{}, std::chrono::milliseconds(0));
    REQUIRE(publication.faces.size() == 1);
    CHECK(publication.faces[0].face == 1);
    CHECK(publication.faces[0].sampled == std::vector<bool>{true, false});
    CHECK(publication.faces[0].radiance.size() == 2);
    CHECK(session.tick(CancelToken{}, std::chrono::milliseconds(0)).faces.empty());
    session.advance(100);
    publication = session.tick(CancelToken{}, std::chrono::milliseconds(0));
    REQUIRE(publication.faces.size() == 1);
    CHECK(publication.faces[0].face == 1);
    CHECK(publication.faces[0].sampled == std::vector<bool>{true, true});
}

TEST_CASE("document edit restarts a bake from an independent identity preserving snapshot") {
    auto file = vmf::parse("world { \"id\" \"1\" \"classname\" \"worldspawn\" }");
    REQUIRE(file);
    document::Document document(*file);
    const auto root = document.roots().front();
    const auto handle = *document.handle(root);
    BakeSession session(scene(), options(), document.historyState());
    session.advance(100);
    auto before = document.snapshot();
    const auto token = document.observe([&](const document::ChangeSet&) {
        session.restart(scene(), options(), document.historyState());
    });
    auto tx = document.begin("lighting edit");
    tx.setKeyValue(handle, "skyname", "newsky");
    tx.commit();
    CHECK(session.progress().revision == document.historyState());
    CHECK(session.progress().passes == std::vector<unsigned>{0, 0, 0});
    CHECK(session.tick(CancelToken{}, std::chrono::milliseconds(0)).faces.empty());
    REQUIRE(before->handle(root));
    CHECK(before->get(*before->handle(root))->block->find("skyname") == nullptr);
    auto after = document.snapshot();
    CHECK(after->get(*after->handle(root))->block->find("skyname") != nullptr);
    CHECK(after->undoCount() == 0);
    document.unobserve(token);
}

TEST_CASE("bake worker cancellation finishes promptly and suppresses publication") {
    auto settings = options();
    settings.maxPasses = 1000000;
    BakeSession session(scene(), settings);
    BakePublication result;
    const auto start = std::chrono::steady_clock::now();
    std::jthread worker([&](std::stop_token stop) {
        result = session.tick(CancelToken(stop), std::chrono::seconds(10));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    worker.request_stop();
    worker.join();
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500));
    CHECK(result.faces.empty());
}

TEST_CASE("bake session handles empty scenes and maximum below bootstrap count") {
    BakeSession empty({}, options());
    CHECK(empty.progress().idle);
    CHECK(empty.tick().faces.empty());
    auto settings = options();
    settings.baker.bounces = 0;
    settings.initialPasses = 4;
    settings.maxPasses = 1;
    BakeSession session(scene(), settings);
    for (int face = 0; face < 3; ++face) session.advance(100);
    CHECK(session.progress().idle);
    CHECK(session.progress().passes == std::vector<unsigned>{1});
}

TEST_CASE("document worker snapshots preserve identities after deletion and creation") {
    auto file = vmf::parse("world { \"id\" \"1\" } entity { \"id\" \"2\" }");
    REQUIRE(file);
    document::Document document(*file);
    const auto removed = document.roots().back();
    auto tx = document.begin("replace entity");
    tx.destroy(*document.handle(removed));
    const auto created = tx.create(kv1::Node::block("entity"));
    tx.commit();
    auto copy = document.snapshot();
    CHECK_FALSE(copy->handle(removed));
    REQUIRE(document.get(created));
    const auto id = document.get(created)->id;
    REQUIRE(copy->handle(id));
    CHECK(copy->get(*copy->handle(id))->block == document.get(created)->block);
    auto edit = copy->begin("worker only");
    edit.setKeyValue(*copy->handle(id), "classname", "light");
    edit.commit();
    CHECK(document.get(created)->block->find("classname") == nullptr);
}

namespace {
// Five faces 300 apart with one point light and a constant ambient light, so sky seeds are exercised.
BakeScene row(double lightX) {
    BakeScene result;
    for (std::uint32_t i = 0; i < 5; ++i) {
        BakeFace face;
        face.solidId = 7;
        face.sideId = i + 1;
        const DVec3 c{300.0 * i, 0, 0};
        face.vertices = {c + DVec3{-32, -32, 0}, c + DVec3{32, -32, 0}, c + DVec3{32, 32, 0}, c + DVec3{-32, 32, 0}};
        face.chart.dimensions = {5, 5};
        face.chart.origin = face.vertices[0];
        face.chart.luxelToWorld = {DVec3{16, 0, 0}, DVec3{0, 16, 0}};
        result.faces.push_back(face);
    }
    Light point;
    point.id = 8;
    point.origin = {lightX, 0, 50};
    point.intensity = {10000, 10000, 10000};
    point.quadratic = 1;
    point.radius = 500;
    Light ambient;
    ambient.kind = LightKind::Ambient;
    ambient.id = 16;
    ambient.intensity = {1, 1, 1};
    result.lights = {point, ambient};
    return result;
}
BakeSessionOptions directOnly() {
    BakeSessionOptions result;
    result.baker.threads = 1;
    result.baker.skyRays = 4;
    result.baker.bounces = 0;
    result.initialPasses = 2;
    result.maxPasses = 4;
    return result;
}
void settle(BakeSession& session) {
    for (int i = 0; i < 100000 && !session.progress().idle; ++i) session.advance(1000);
    REQUIRE(session.progress().idle);
}
bool sameBuffers(const BakeSession& a, const BakeSession& b, std::size_t face, unsigned level = 0) {
    const auto& x = a.baker().buffers(level)[face];
    const auto& y = b.baker().buffers(level)[face];
    return x.samples == y.samples && x.radiance == y.radiance;
}
}

TEST_CASE("incremental bake: dirty faces equal a full bake of the new scene, clean faces are untouched") {
    BakeSession first(row(0), directOnly());
    settle(first);
    BakeSession reference(row(1200), directOnly());
    settle(reference);
    const auto before = first.baker().buffers(0);
    auto state = first.takeState();
    CHECK(first.progress().idle);

    BakeSession next(row(0), directOnly());
    const auto diff = next.restart(std::move(state), row(1200), directOnly(), 5);
    REQUIRE_FALSE(diff.full);
    CHECK(next.progress().revision == 5);
    CHECK(diff.dirty == std::vector<char>{1, 1, 0, 1, 1});
    // Clean face: adopted, counts intact.
    CHECK(next.facePasses(0) == std::vector<unsigned>{0, 0, 4, 0, 0});
    CHECK(next.baker().buffers(0)[2].radiance == before[2].radiance);
    CHECK(next.progress().passes[0] == 0);
    // Only dirty faces are ever scheduled; the clean one never changes.
    std::set<std::size_t> touched;
    while (!next.progress().idle) {
        next.advance(1000);
        for (std::size_t face = 0; face < 5; ++face)
            if (next.baker().buffers(0)[face].samples != before[face].samples) touched.insert(face);
    }
    CHECK(touched == std::set<std::size_t>{0, 1, 3, 4});
    CHECK(next.facePasses(0) == std::vector<unsigned>{4, 4, 4, 4, 4});
    CHECK(next.baker().buffers(0)[2].samples == before[2].samples);
    // Bit-identical to a full bake, dirty and clean alike (no bounce coupling at bounces = 0).
    for (std::size_t face = 0; face < 5; ++face) CHECK(sameBuffers(next, reference, face));
}

TEST_CASE("incremental bake: a restart that changed nothing finishes immediately") {
    BakeSession first(row(0), directOnly());
    settle(first);
    first.tick(CancelToken{}, std::chrono::milliseconds(0)); // Publish, so nothing is left unpublished.
    BakeSession next(row(0), directOnly());
    next.restart(first.takeState(), row(0), directOnly(), 1);
    next.advance(1);
    CHECK(next.progress().idle);
    CHECK(next.tick().faces.empty());
}

TEST_CASE("incremental bake: an interrupted session resumes where it stopped") {
    BakeSession first(row(0), directOnly());
    first.advance(1000);
    first.advance(1000);
    const auto done = first.facePasses(0);
    BakeSession next(row(0), directOnly());
    next.restart(first.takeState(), row(1200), directOnly(), 2);
    const auto& counts = next.facePasses(0);
    CHECK(counts[2] == done[2]); // Clean face keeps a partial count.
    settle(next);
    BakeSession reference(row(1200), directOnly());
    settle(reference);
    CHECK(sameBuffers(next, reference, 0));
    CHECK(next.facePasses(0) == std::vector<unsigned>{4, 4, 4, 4, 4});
}

TEST_CASE("incremental bake: camera order still holds among dirty faces") {
    BakeSession first(row(0), directOnly());
    settle(first);
    BakeSession next(row(0), directOnly());
    next.restart(first.takeState(), row(1200), directOnly(), 2);
    next.setCamera({1190, 0, 0}); // Nearest dirty face is 4, then 3.
    next.advance(1000);
    const auto& buffers = next.baker().buffers(0);
    CHECK(buffers[4].samples[0] == 4);
    CHECK(buffers[3].samples[0] == 0);
    CHECK(buffers[0].samples[0] == 0);
    next.advance(1000);
    CHECK(buffers[3].samples[0] == 4);
}

TEST_CASE("incremental bake: unbounded changes and changed settings restart everything") {
    BakeSession first(row(0), directOnly());
    settle(first);
    SUBCASE("ambient light") {
        auto scene = row(0);
        scene.lights[1].intensity = {2, 2, 2};
        BakeSession next(row(0), directOnly());
        const auto diff = next.restart(first.takeState(), std::move(scene), directOnly(), 2);
        CHECK(diff.full);
        CHECK(next.facePasses(0) == std::vector<unsigned>(5, 0));
        CHECK(next.baker().buffers(0)[2].radiance[0] == Vec3{0});
    }
    SUBCASE("settings") {
        auto settings = directOnly();
        settings.baker.skyRays = 8;
        BakeSession next(row(0), directOnly());
        const auto diff = next.restart(first.takeState(), row(0), settings, 2);
        CHECK(diff.full);
        CHECK(diff.reason == "baker settings changed");
        CHECK(next.facePasses(0) == std::vector<unsigned>(5, 0));
    }
    SUBCASE("retired state") {
        auto state = first.takeState();
        CHECK(first.takeState().empty());
        BakeSession next(row(0), directOnly());
        CHECK(next.restart(std::move(state), row(0), directOnly(), 2).full == false);
    }
}

TEST_CASE("incremental bake: cancelled tick leaves its faces to be republished after takeState") {
    BakeSession first(row(0), directOnly());
    first.advance(1000);
    BakeSession next(row(0), directOnly());
    next.restart(first.takeState(), row(0), directOnly(), 2);
    const auto publication = next.tick(CancelToken{}, std::chrono::milliseconds(0));
    CHECK(publication.faces.size() == 1); // The face advanced before the retire is republished.
}

namespace {
// Floor lit by a spot, a wall beside it (inside the bounce radius) and a wall far beyond it.
BakeScene bounceRoom(double lightX) {
    BakeScene scene;
    struct Quad { DVec3 origin, u, v, normal; };
    const std::array<Quad, 3> quads{{{{-64, -64, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
                                     {{260, -64, 0}, {0, 1, 0}, {0, 0, 1}, {-1, 0, 0}},
                                     {{-1100, -64, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 0}}}};
    std::vector<std::array<float, 3>> positions;
    std::vector<std::uint32_t> indices;
    for (std::uint32_t i = 0; i < quads.size(); ++i) {
        const auto& q = quads[i];
        BakeFace face;
        face.solidId = 3;
        face.sideId = i + 1;
        face.normal = q.normal;
        face.albedo = Vec3{0.5f};
        face.vertices = {q.origin, q.origin + 128.0 * q.u, q.origin + 128.0 * (q.u + q.v), q.origin + 128.0 * q.v};
        face.chart.dimensions = {9, 9};
        face.chart.origin = q.origin;
        face.chart.luxelToWorld = {16.0 * q.u, 16.0 * q.v};
        face.chart.worldToLuxel = {q.u / 16.0, q.v / 16.0};
        scene.faces.push_back(face);
        const auto base = std::uint32_t(positions.size());
        for (const auto& p : face.vertices) positions.push_back({float(p.x), float(p.y), float(p.z)});
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    Light light;
    light.kind = LightKind::Spot;
    light.id = 8;
    light.origin = {lightX, 0, 96};
    light.direction = {0, 0, -1};
    light.intensity = DVec3{30000};
    light.quadratic = 1;
    light.stopdot = 0.98;
    light.stopdot2 = 0.95;
    scene.lights = {light};
    auto blas = CpuBlas::build(positions, indices);
    REQUIRE(blas);
    const std::array<CpuInstance, 1> instances{{{*blas, {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}}, 1}}};
    auto accel = CpuAccel::build(instances);
    REQUIRE(accel);
    scene.occlusion.accel = std::shared_ptr<const CpuAccel>(std::move(*accel));
    scene.occlusion.isSky = [](const RayHit&) { return false; };
    scene.occlusion.faceOf = [](const RayHit& hit) -> std::optional<std::size_t> { return hit.primitive / 2; };
    return scene;
}
BakeSessionOptions withBounce() {
    BakeSessionOptions result;
    result.baker.threads = 1;
    result.baker.skyRays = 1;
    result.baker.bounces = 1;
    result.baker.bounceRays = 32;
    result.initialPasses = 4;
    result.maxPasses = 8;
    return result;
}
double meanOf(const BakeSession& session, std::size_t face) {
    const auto radiance = session.baker().resolve()[face];
    double sum = 0;
    for (const auto v : radiance) sum += double(v.x + v.y + v.z) / 3;
    return sum / double(radiance.size());
}
}

TEST_CASE("incremental bake: with bounce, faces in reach match a full bake and far faces stay within tolerance") {
    BakeSession first(bounceRoom(-32), withBounce());
    settle(first);
    BakeSession reference(bounceRoom(0), withBounce());
    settle(reference);
    BakeSession next(bounceRoom(-32), withBounce());
    const auto diff = next.restart(first.takeState(), bounceRoom(0), withBounce(), 2);
    REQUIRE_FALSE(diff.full);
    CHECK(diff.dirty == std::vector<char>{1, 0, 0});
    CHECK(diff.bounceDirty == std::vector<char>{1, 1, 0}); // The far wall is beyond kBounceRadius.
    CHECK(next.facePasses(1) == std::vector<unsigned>{0, 0, 8});
    CHECK(next.facePasses(0) == std::vector<unsigned>{0, 8, 8});
    settle(next);
    // The dirty face reproduces the full bake exactly. Its bounce neighbour keeps the same direct, but its
    // first bounce samples are drawn after the dirty face's direct has refined (dirty faces lead), so it
    // agrees only statistically.
    CHECK(sameBuffers(next, reference, 0, 0));
    CHECK(sameBuffers(next, reference, 0, 1));
    CHECK(sameBuffers(next, reference, 1, 0));
    // The far wall keeps its old bounce: the documented approximation. Tolerance: 2% of the brightest face.
    const double peak = std::max({meanOf(reference, 0), meanOf(reference, 1), meanOf(reference, 2)});
    MESSAGE("far wall: incremental " << meanOf(next, 2) << " full " << meanOf(reference, 2) << " peak " << peak);
    CHECK(std::abs(meanOf(next, 2) - meanOf(reference, 2)) < 0.02 * peak);
    CHECK(std::abs(meanOf(next, 1) - meanOf(reference, 1)) < 0.02 * peak);
}

TEST_CASE("incremental bake: dirty faces finish their bounce levels before other faces get new bounce passes") {
    BakeSession first(bounceRoom(-32), withBounce());
    settle(first);
    BakeSession next(bounceRoom(-32), withBounce());
    const auto diff = next.restart(first.takeState(), bounceRoom(0), withBounce(), 2);
    REQUIRE_FALSE(diff.full);
    REQUIRE(diff.dirty == std::vector<char>{1, 0, 0});
    REQUIRE(diff.bounceDirty == std::vector<char>{1, 1, 0});
    bool dirtyDone = false;
    for (int i = 0; i < 100000 && !next.progress().idle; ++i) {
        next.advance(1000);
        const bool done = next.facePasses(0)[0] == 8 && next.facePasses(1)[0] == 8;
        dirtyDone = dirtyDone || done;
        // The neighbour's bounce stays untouched until the dirty face has fully converged both levels.
        if (!dirtyDone) CHECK(next.facePasses(1)[1] == 0);
        // The clean face's direct is never touched.
        CHECK(next.facePasses(0)[1] == 8);
    }
    CHECK(dirtyDone);
    CHECK(next.progress().idle);
    CHECK(next.facePasses(1) == std::vector<unsigned>{8, 8, 8});
}

namespace {
std::vector<std::vector<Vec3>> seedValues(const BakeScene& scene) {
    std::vector<std::vector<Vec3>> values;
    for (std::size_t i = 0; i < scene.faces.size(); ++i) {
        const auto dims = scene.faces[i].chart.dimensions;
        values.emplace_back(std::size_t(dims[0]) * std::size_t(dims[1]), Vec3(float(i + 1)));
    }
    return values;
}
}

TEST_CASE("bake seed: unchanged faces retain total light and publish") {
    auto input = bounceRoom(-32);
    auto settings = withBounce();
    settings.freezeBounceDirty = true;
    const auto values = seedValues(input);
    const std::vector<char> known(input.faces.size(), 1);
    auto state = seedState(input, {}, values, known, settings);
    BakeSession session(input, settings);
    const auto diff = session.restart(std::move(state), input, settings, 1);
    CHECK_FALSE(diff.full);
    CHECK(diff.dirtyCount == 0);
    CHECK(session.baker().resolve() == values);
    const auto publication = session.tick();
    CHECK(publication.progress.idle);
    CHECK(publication.faces.size() == input.faces.size());
}

TEST_CASE("bake seed: moved light leaves bounce dirty neighbours frozen") {
    auto input = bounceRoom(-32);
    auto settings = withBounce();
    settings.freezeBounceDirty = true;
    const auto values = seedValues(input);
    const std::vector<char> known(input.faces.size(), 1);
    auto state = seedState(input, {}, values, known, settings);
    BakeSession session(input, settings);
    const auto diff = session.restart(std::move(state), bounceRoom(0), settings, 1);
    REQUIRE_FALSE(diff.full);
    REQUIRE(diff.dirty == std::vector<char>{1, 0, 0});
    REQUIRE(diff.bounceDirty == std::vector<char>{1, 1, 0});
    settle(session);
    const auto result = session.baker().resolve();
    CHECK(result[0] != values[0]);
    CHECK(result[1] == values[1]);
    CHECK(result[2] == values[2]);
}

TEST_CASE("bake seed: the bounce ring option rebakes neighbours from direct") {
    auto input = bounceRoom(-32);
    auto settings = withBounce();
    settings.freezeBounceDirty = true;
    settings.rebakeBounceRing = true;
    const auto values = seedValues(input);
    const std::vector<char> known(input.faces.size(), 1);
    auto state = seedState(input, {}, values, known, settings);
    BakeSession session(input, settings);
    const auto diff = session.restart(std::move(state), bounceRoom(0), settings, 1);
    REQUIRE_FALSE(diff.full);
    CHECK(diff.dirty == std::vector<char>{1, 1, 0});
    CHECK(diff.dirtyCount == 2);
    CHECK(session.facePasses(0)[1] == 0); // Nothing kept: no frozen total to count twice.
    settle(session);
    const auto result = session.baker().resolve();
    CHECK(result[1] != values[1]);
    CHECK(result[2] == values[2]);
}

TEST_CASE("bake seed: dirty face gathers frozen total as first bounce") {
    auto input = bounceRoom(-32);
    input.lights.clear();
    auto settings = withBounce();
    settings.freezeBounceDirty = true;
    auto values = seedValues(input);
    for (auto& face : values) std::fill(face.begin(), face.end(), Vec3{10});
    const std::vector<char> known(input.faces.size(), 1);
    auto state = seedState(input, {}, values, known, settings);
    auto edited = input;
    edited.faces[1].albedo = Vec3{0.6f};
    BakeSession session(input, settings);
    const auto diff = session.restart(std::move(state), edited, settings, 1);
    REQUIRE_FALSE(diff.full);
    REQUIRE(diff.dirty[1]);
    REQUIRE_FALSE(diff.dirty[0]);
    settle(session);
    const auto direct = session.baker().resolveLevel(0)[1];
    const auto bounce = session.baker().resolveLevel(1)[1];
    for (const auto value : direct) CHECK(glm::length(value) == doctest::Approx(0.0));
    float total = 0;
    for (const auto value : bounce) total += value.x;
    CHECK(total > 0);
}

TEST_CASE("bake seed: uncovered faces reach their pass target") {
    auto input = bounceRoom(-32);
    auto settings = withBounce();
    settings.freezeBounceDirty = true;
    const auto values = seedValues(input);
    const std::vector<char> known{1, 0, 1};
    auto state = seedState(input, {}, values, known, settings);
    BakeSession session(input, settings);
    const auto diff = session.restart(std::move(state), input, settings, 1);
    CHECK_FALSE(diff.full);
    CHECK(session.facePasses(0)[1] == 0);
    settle(session);
    CHECK(session.facePasses(0)[1] == settings.maxPasses);
    CHECK(session.facePasses(1)[1] == settings.maxPasses);
}
