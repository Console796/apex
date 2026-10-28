#include <doctest/doctest.h>
#include "document/cordon.hpp"
#include "editor/map_problems.hpp"
#include "fixtures.hpp"

using namespace apex;
using namespace editor_test;

namespace {
NodeHandle world(const Document& doc) { return *doc.handle(doc.roots().front()); }

std::vector<Problem> check(const Document& doc, const ProblemContext* custom = nullptr) {
    return ProblemRegistry::builtin().run(custom ? *custom : ProblemContext{doc});
}
int count(const std::vector<Problem>& problems, ProblemKind kind) {
    return static_cast<int>(std::count_if(problems.begin(), problems.end(), [&](const Problem& p) { return p.kind == kind; }));
}
const char* kStart = "entity { id 90 classname info_player_start origin \"0 0 0\" }\n";
} // namespace

TEST_CASE("a clean map has no problems") {
    Document doc(parsed(std::string("world { id 1 classname worldspawn }\n") + kStart));
    addBox(doc, {0, 0, 0}, {64, 64, 64}, world(doc));
    CHECK(check(doc).empty());
}

TEST_CASE("invalid solids are errors that select the solid") {
    Document doc(parsed(std::string("world { id 1 classname worldspawn }\n") + kStart));
    auto tx = doc.begin("bad solid");
    auto solid = kv1::Node::block("solid");
    auto side = kv1::Node::block("side");
    side.append(kv1::Node::value("id", "5")).value();
    side.append(kv1::Node::value("plane", "(0 0 0) (1 0 0) (2 0 0)")).value();
    solid.append(std::move(side)).value();
    tx.create(std::move(solid), world(doc));
    tx.commit();
    const auto problems = check(doc);
    REQUIRE(count(problems, ProblemKind::InvalidSolid) == 1);
    CHECK(problems.front().severity == ProblemSeverity::Error);
    CHECK(problems.front().objects.size() == 1);
}

TEST_CASE("coordinates beyond the BSP range are errors") {
    Document doc(parsed(std::string("world { id 1 classname worldspawn }\n") + kStart +
                        "entity { id 20 classname info_target origin \"70000 0 0\" }\n"
                        "entity { id 21 classname info_target origin \"65536 0 0\" }"));
    addBox(doc, {60000, 0, 0}, {70000, 64, 64}, world(doc));
    const auto problems = check(doc);
    CHECK(count(problems, ProblemKind::OutOfRange) == 2); // the far entity and the far world solid
}

TEST_CASE("brush entities need brushes and point entities need an origin") {
    Document doc(parsed(std::string("world { id 1 classname worldspawn }\n") + kStart +
                        "entity { id 20 classname func_detail }\n"
                        "entity { id 21 classname func_instance file \"a.vmf\" origin \"0 0 0\" }\n"
                        "entity { id 22 classname info_target }\n"
                        "entity { id 23 classname info_target origin \"1 2 3\" }"));
    const auto problems = check(doc);
    CHECK(count(problems, ProblemKind::EmptyBrushEntity) == 1);
    CHECK(count(problems, ProblemKind::MissingOrigin) == 1);
    auto tx = doc.begin("fill");
    tx.create(solidBlock(geometry_test::build(geometry_test::boxSides({0, 0, 0}, {16, 16, 16}))),
              *doc.handle(doc.roots()[2]));
    tx.commit();
    CHECK(count(check(doc), ProblemKind::EmptyBrushEntity) == 0);
}

TEST_CASE("a missing player start is a warning and the profile's class satisfies it") {
    Document doc(parsed("world { id 1 classname worldspawn }\nentity { id 20 classname info_player_teamspawn origin \"0 0 0\" }"));
    CHECK(count(check(doc), ProblemKind::MissingPlayerStart) == 1);
    ProblemContext context{doc};
    context.playerStartClasses = {"info_player_teamspawn"};
    CHECK(count(check(doc, &context), ProblemKind::MissingPlayerStart) == 0);
    context.playerStartClasses = {};
    CHECK(count(check(doc, &context), ProblemKind::MissingPlayerStart) == 0);
}

TEST_CASE("duplicate targetnames matter only where a unique one is required") {
    Document doc(parsed(std::string("world { id 1 classname worldspawn }\n") + kStart +
                        "entity { id 20 classname logic_relay targetname r origin \"0 0 0\" }\n"
                        "entity { id 21 classname logic_relay targetname R origin \"0 0 0\" }\n"
                        "entity { id 22 classname info_target targetname t origin \"0 0 0\" }\n"
                        "entity { id 23 classname info_target targetname t origin \"0 0 0\" }"));
    const auto problems = check(doc);
    REQUIRE(count(problems, ProblemKind::DuplicateName) == 1);
    for (const auto& p : problems) if (p.kind == ProblemKind::DuplicateName) CHECK(p.objects.size() == 2);
}

TEST_CASE("an active cordon that holds nothing is reported, and one holding a solid is not") {
    Document doc(parsed(std::string("world { id 1 classname worldspawn }\n") + kStart));
    addBox(doc, {0, 0, 0}, {64, 64, 64}, world(doc));
    const auto full = document::addCordon(doc, "full", AABB{{-8, -8, -8}, {72, 72, 72}});
    CHECK(count(check(doc), ProblemKind::EmptyCordon) == 0);
    const auto empty = document::addCordon(doc, "empty", AABB{{1000, 1000, 1000}, {1064, 1064, 1064}});
    auto problems = check(doc);
    REQUIRE(count(problems, ProblemKind::EmptyCordon) == 1);
    for (const auto& p : problems) if (p.kind == ProblemKind::EmptyCordon) CHECK(p.objects == std::vector<NodeId>{empty});
    document::setCordonActive(doc, empty, false);
    CHECK(count(check(doc), ProblemKind::EmptyCordon) == 0);
    (void)full;
}

TEST_CASE("asset, connection and class problems through the registry match the 0.4 report") {
    Document doc(parsed(std::string("world { id 1 classname worldspawn }\n") + kStart +
                        "entity { id 19 classname env_sprite texture \"gone/tex\" origin \"0 0 0\" }\n"
                        "entity { id 20 classname prop_dynamic model \"models/gone.mdl\" origin \"0 0 0\" }\n"
                        "entity { id 21 classname func_instance file \"maps/gone.vmf\" origin \"0 0 0\" }\n"
                        "entity { id 22 classname mystery origin \"0 0 0\" }"));
    AssetReport report(doc, [](auto, auto) { return false; });
    Problems assets{report.missing(), {}, {{22, "mystery"}}};
    ProblemContext context{doc};
    context.assets = &assets;
    const auto problems = check(doc, &context);
    CHECK(count(problems, ProblemKind::MissingAsset) == static_cast<int>(report.missing().size()));
    CHECK(count(problems, ProblemKind::UnknownClass) == 1);
    // Missing models and instances are errors; a missing material is a warning.
    for (const auto& p : problems) if (p.kind == ProblemKind::MissingAsset) {
        const bool soft = p.text.starts_with("Missing material");
        CHECK(p.severity == (soft ? ProblemSeverity::Warning : ProblemSeverity::Error));
    }
    CHECK(problems.front().severity == ProblemSeverity::Error);
    CHECK(countProblems(problems, ProblemSeverity::Error) == 2);
}

TEST_CASE("instance cycles are errors and unresolved instances are not reported twice") {
    Document doc(parsed(std::string("world { id 1 classname worldspawn }\n") + kStart));
    std::vector<InstanceProblem> instances{
        {InstanceProblem::Kind::Cycle, 90, {}, "a.vmf", "recursive"},
        {InstanceProblem::Kind::Unresolved, 90, {}, "b.vmf", "missing"}};
    ProblemContext context{doc};
    context.instances = &instances;
    CHECK(count(check(doc, &context), ProblemKind::InstanceProblem) == 1);
}
