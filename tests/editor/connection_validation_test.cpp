#include "fixtures.hpp"
#include "editor/connection_completion.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>

using namespace editor_test;
namespace {
assets::EntityClassTable table() {
    assets::EntityClassTable result;
    auto& a = result.classes["alpha"];
    a.outputs = {{"OnGo", "void", ""}};
    a.inputs = {{"Int", "integer", ""}, {"Float", "float", ""}, {"Bool", "bool", ""}, {"Text", "string", ""}, {"Void", "void", ""}};
    result.classes["beta"].inputs = {{"Other", "void", ""}, {"INT", "integer", ""}};
    result.classes["func_instance"] = {};
    result.classes["func_instance_io_proxy"] = {};
    result.classes["func_instance_io_proxy"].inputs = {{"ProxyRelay", "string", ""}};
    result.classes["logic_relay"].inputs = {{"Trigger", "integer", ""}};
    result.classes["logic_relay"].outputs = {{"OnTrigger", "void", ""}};
    result.classes["logic_float"].inputs = {{"Trigger", "float", ""}};
    return result;
}
bool has(const std::vector<ConnectionIssue>& issues, ConnectionIssueKind kind) {
    return std::ranges::any_of(issues, [&](const auto& issue) { return issue.kind == kind; });
}
}
TEST_CASE("Connection validation marks outputs targets inputs parameters and numeric fields") {
    Document doc(parsed("entity { classname alpha targetname first } entity { classname beta targetname finish } entity { classname unknown targetname mystery }"));
    NameIndex names(doc);
    const auto classes = table();
    ConnectionValidator validator(doc, names, classes);
    const auto source = doc.roots()[0];
    auto c = document::parseConnection("first,Int,12,-0.5,-1");
    CHECK(validator.validate(source, c, "oNgO").empty());
    CHECK(has(validator.validate(source, c, "Bad"), ConnectionIssueKind::UnknownOutput));
    CHECK(has(validator.validate(source, c, ""), ConnectionIssueKind::UnknownOutput));
    CHECK_FALSE(has(validator.validate(doc.roots()[2], c, "Bad"), ConnectionIssueKind::UnknownOutput));
    for (const auto& target : {"f*", "*irst", "*irs*", "ALPHA", "alp*"}) {
        c.target = target;
        CHECK(validator.validate(source, c).empty());
    }
    c.target = "f*"; c.input = "Other";
    CHECK(validator.validate(source, c).empty());
    c.target = "first";
    CHECK(has(validator.validate(source, c), ConnectionIssueKind::UnknownInput));
    c.input = "Int";
    c.target = "fi*st";
    CHECK(has(validator.validate(source, c), ConnectionIssueKind::UnmatchedTarget));
    c.target = "missing";
    CHECK(has(validator.validate(source, c), ConnectionIssueKind::UnmatchedTarget));
    for (const auto& target : {"@global", "@transition_from_map", "$fixup", "prefix$fixup"}) {
        c.target = target;
        CHECK_FALSE(has(validator.validate(source, c), ConnectionIssueKind::UnmatchedTarget));
    }
    c.target = "missing";
    for (const auto& target : specialTargets()) {
        c.target = target; c.input = "Unknown";
        CHECK(validator.validate(source, c).empty());
    }
    c.target = "mystery";
    CHECK(validator.validate(source, c).empty());
    c.target = "first";
    CHECK(has(validator.validate(source, c), ConnectionIssueKind::UnknownInput));
    for (const auto& [input, good, bad] : std::vector<std::tuple<std::string, std::string, std::string>>{
        {"Int", "+12", "1.5"}, {"Float", "-1.5e2", "nan"}, {"Bool", "1", "true"}}) {
        c.input = input; c.parameter = good;
        CHECK(validator.validate(source, c).empty());
        c.parameter = bad;
        CHECK(has(validator.validate(source, c), ConnectionIssueKind::BadParameter));
        c.parameter = "";
        CHECK(validator.validate(source, c).empty());
    }
    for (auto input : {"Text", "Void"}) {
        c.input = input; c.parameter = "anything";
        CHECK(validator.validate(source, c).empty());
    }
    for (auto value : {"-1", "+1"}) {
        c.input = "Int"; c.parameter = value;
        CHECK_FALSE(has(validator.validate(source, c), ConnectionIssueKind::BadParameter));
    }
    for (auto value : {"+-1", "++1"}) {
        c.input = "Int"; c.parameter = value;
        CHECK(has(validator.validate(source, c), ConnectionIssueKind::BadParameter));
    }
    for (auto delay : {"", "0", "-2", "+1.5"}) {
        c.delay = delay; CHECK_FALSE(has(validator.validate(source, c), ConnectionIssueKind::BadDelay));
    }
    for (auto delay : {"bad", "inf", "1tail"}) {
        c.delay = delay; CHECK(has(validator.validate(source, c), ConnectionIssueKind::BadDelay));
    }
    c.input = "Bool"; c.parameter = "0"; c.delay = "0";
    CHECK(validator.validate(source, c).empty());
    for (auto times : {"-1", "0", "2"}) {
        c.times = times; CHECK_FALSE(has(validator.validate(source, c), ConnectionIssueKind::BadTimes));
    }
    for (auto times : {"", "-2", "1.2", "bad"}) {
        c.times = times; CHECK(has(validator.validate(source, c), ConnectionIssueKind::BadTimes));
    }
    c.times = "+-1"; CHECK(has(validator.validate(source, c), ConnectionIssueKind::BadTimes));
    c.times = "++1"; CHECK(has(validator.validate(source, c), ConnectionIssueKind::BadTimes));
    c.times = "+1"; CHECK_FALSE(has(validator.validate(source, c), ConnectionIssueKind::BadTimes));
    const auto malformed = validator.validate(source, document::parseConnection("broken"));
    REQUIRE(malformed.size() == 1);
    CHECK(malformed[0].kind == ConnectionIssueKind::Malformed);
}
TEST_CASE("Connection completion unions classes and preserves wildcard text") {
    Document doc(parsed("entity { classname alpha targetname first } entity { classname beta targetname finish }"));
    NameIndex names(doc);
    auto classes = table();
    ConnectionValidator validator(doc, names, classes);
    CHECK(inputsFor(validator, "f*") == std::vector<std::string>{"Bool", "Float", "INT", "Other", "Text", "Void"});
    CHECK(inputsFor(validator, "beta") == std::vector<std::string>{"INT", "Other"});
    CHECK(outputsFor(classes, "ALPHA") == std::vector<std::string>{"OnGo"});
    CHECK(outputsFor(classes, "unknown").empty());
    auto targets = targetsFor(names);
    for (const auto& special : specialTargets()) CHECK(std::ranges::find(targets, special) != targets.end());
    CHECK(targetsFor(names, "fi") == std::vector<std::string>{"finish", "first"});
    targets = targetsFor(names, "f*");
    CHECK(std::ranges::find(targets, "f*") != targets.end());
    CHECK(parameterChoices(classes, "bool", names) == std::vector<std::string>{"0", "1"});
    CHECK(parameterChoices(classes, "target_destination", names) == targetsFor(names));
    CHECK(parameterChoices(classes, "string", names).empty());
}
TEST_CASE("Connection instance proxy validation completion and instance file leniency") {
    const auto dir = std::filesystem::temp_directory_path() / ("apex-connection-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    struct Cleanup { std::filesystem::path dir; ~Cleanup() { std::filesystem::remove_all(dir); } } cleanup{dir};
    std::ofstream(dir / "child.vmf") << "entity { classname func_instance_io_proxy targetname public_proxy "
        "connections { OnProxyRelay \"internal,Trigger,,0,-1\" } } "
        "entity { classname logic_relay targetname internal } entity { classname logic_float targetname internal }";
    std::ofstream(dir / "broken.vmf") << "entity {";
    Document doc(parsed("entity { classname alpha } entity { classname func_instance targetname inst file child.vmf } "
        "entity { classname func_instance targetname missing file missing.vmf } entity { classname func_instance_io_proxy } "
        "entity { classname func_instance targetname broken file broken.vmf } "
        "entity { classname func_instance_io_proxy targetname proxy }"));
    NameIndex names(doc);
    auto classes = table();
    InstanceTemplates templates;
    ConnectionValidator validator(doc, names, classes, &templates, dir / "map.vmf");
    auto c = document::parseConnection("inst,instance:relay;ProxyRelay,,0,-1");
    c.input = "instance:internal;Trigger";
    CHECK(validator.validate(doc.roots()[0], c).empty());
    c.parameter = "+2.5";
    CHECK_FALSE(has(validator.validate(doc.roots()[0], c), ConnectionIssueKind::BadParameter));
    c.parameter = "not-a-number";
    CHECK(has(validator.validate(doc.roots()[0], c), ConnectionIssueKind::BadParameter));
    c.parameter = "";
    const auto completions = inputsFor(validator, "inst");
    CHECK(completions == std::vector<std::string>{"instance:internal;Trigger", "instance:public_proxy;ProxyRelay"});
    c.input = "instance:absent;Trigger";
    CHECK(has(validator.validate(doc.roots()[0], c), ConnectionIssueKind::UnknownInput));
    c.input = "instance:internal;Missing";
    CHECK(has(validator.validate(doc.roots()[0], c), ConnectionIssueKind::UnknownInput));
    c.input = "instance:internal;";
    CHECK(has(validator.validate(doc.roots()[0], c), ConnectionIssueKind::UnknownInput));
    c.input = "instance:internal;Trigger";
    CHECK(validator.validate(doc.roots()[1], c, "instance:internal;OnTrigger").empty());
    CHECK(has(validator.validate(doc.roots()[1], c, "instance:absent;OnTrigger"), ConnectionIssueKind::UnknownOutput));
    c.target = "missing";
    CHECK(validator.validate(doc.roots()[0], c).empty());
    c.target = "broken";
    CHECK(validator.validate(doc.roots()[0], c).empty());
    c.target = "alpha";
    CHECK(has(validator.validate(doc.roots()[0], c), ConnectionIssueKind::UnknownInput));
    c.target = "parent_name";
    CHECK(validator.validate(doc.roots()[0], c).empty());
    CHECK_FALSE(has(validator.validate(doc.roots()[5], c, "OnProxyRelay"), ConnectionIssueKind::UnknownOutput));
    CHECK_FALSE(has(validator.validate(doc.roots()[5], c, "OnProxyRelay12"), ConnectionIssueKind::UnknownOutput));
    CHECK(has(validator.validate(doc.roots()[5], c, "OnProxyRelayA"), ConnectionIssueKind::UnknownOutput));
    ConnectionValidator strict(doc, names, classes, nullptr, {}, {}, false);
    CHECK(has(strict.validate(doc.roots()[0], c), ConnectionIssueKind::UnmatchedTarget));
}
