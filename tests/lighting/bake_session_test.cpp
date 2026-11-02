#include "lighting/bake_session.hpp"
#include "document/document.hpp"

#include <doctest/doctest.h>
#include <chrono>
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
    result.baker.skyRays = result.baker.bounceRays = 1;
    result.initialPasses = 1;
    result.maxPasses = 2;
    return result;
}
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
