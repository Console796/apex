#include "document/instance_collapse.hpp"
#include "document/connection_codec.hpp"

#include <doctest/doctest.h>
#include <fstream>
#include <sstream>
#include <set>

using namespace apex;
using namespace apex::document;
namespace {
vmf::VmfFile parsed(std::string_view text) {
    auto file = vmf::parse(text);
    REQUIRE(file);
    return std::move(*file);
}
kv1::Node entity(std::string_view text) {
    auto file = kv1::parse(std::string(text));
    REQUIRE(file);
    return file->root().children()[0];
}
std::string get(const kv1::Node& node, std::string_view key) {
    const auto* value = node.find(key);
    return value ? value->decodedValue().value_or("") : "";
}
NodeId instanceId(const Document& doc) {
    for (auto id : doc.roots()) {
        const auto* node = doc.get(*doc.handle(id));
        if (get(*node->block, "classname") == "func_instance") return id;
    }
    FAIL("no instance");
    return 0;
}
std::string fixture() {
    std::ifstream in(std::string(APEX_FIXTURE_DIR) + "/vmf/instances/instance_rules.vmf");
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}
}
TEST_CASE("Instance fixup variables names and copied drawing keys") {
    auto inst = entity(R"(entity { targetname room replace01 "$var metal" replace02 "$variable long" replace03 "$skin 3" })");
    auto fixup = InstanceFixup::parse(inst);
    CHECK(fixup.substitute("$VARIABLE/$var/$missing/$$var/$/!$skin") == "long/metal//$metal/$/!3");
    CHECK(fixup.fixName("door") == "room-door");
    CHECK(fixup.fixName("@global") == "@global");
    CHECK(fixup.fixName("!activator") == "!activator");
    CHECK(fixup.fixName("") == "");
    fixup.style = FixupStyle::Postfix;
    CHECK(fixup.fixName("door") == "door-room");
    fixup.style = FixupStyle::None;
    CHECK(fixup.fixName("door") == "door");
    auto original = entity(R"(entity { classname prop_dynamic model "models/$var.mdl" skin "$skin" targetname door solid { side { material "$var/panel" } } })");
    kv1::Document snapshot;
    (void)snapshot.root().append(original);
    const auto originalBytes = kv1::write(snapshot).value();
    auto copy = fixedInstanceKeys(original, fixup);
    kv1::Document after;
    (void)after.root().append(original);
    CHECK(kv1::write(after).value() == originalBytes);
    CHECK(get(copy, "model") == "models/metal.mdl");
    CHECK(get(copy, "skin") == "3");
    CHECK(get(copy.find("solid")->children()[0], "material") == "metal/panel");
    CHECK(get(original, "model") == "models/$var.mdl");
    CHECK(get(original, "skin") == "$skin");
    for (const auto& [style, expected] : std::vector<std::pair<int, std::string>>{{0,"room-door"}, {1,"door-room"}, {2,"door"}}) {
        (void)inst.append(kv1::Node::value("fixup_style", std::to_string(style)));
        auto value = InstanceFixup::parse(inst);
        CHECK(value.fixName("door") == expected);
        (void)inst.remove(inst.children().size() - 1);
    }
}
TEST_CASE("Instance collapse transforms materials proxies and preserves inputs") {
    Document parent(parsed(R"(world { id 1 classname worldspawn }
        entity { id 2 classname func_instance targetname room file test origin "10 20 30" angles "0 90 0"
          replace01 "$var metal" replace02 "$skin 3"
          editor { groupid 80 visgroupid 90 }
          connections { "instance:relay;OnTrigger" "outside,Fire,outside parameter,1,1" } }
        entity { id 3 classname logic_relay connections { OnTrigger "room,instance:model;Enable,,1,-1" } })"));
    Document templ(parsed(fixture()));
    const auto beforeParent = parent.write().value();
    const auto beforeTemplate = templ.write().value();
    auto result = collapseInstance(parent, instanceId(parent), templ, {});
    REQUIRE(result);
    CHECK(parent.write().value() == beforeParent);
    CHECK(templ.write().value() == beforeTemplate);
    CHECK(result->fragment.find("func_instance_parms") == std::string::npos);
    CHECK(result->fragment.find("func_instance_io_proxy") == std::string::npos);
    CHECK(get(result->parentMembership, "groupid") == "80");
    CHECK(get(result->parentMembership, "visgroupid") == "90");
    auto fragment = kv1::parse(result->fragment);
    REQUIRE(fragment);
    CHECK(fragment->root().find("world") == nullptr);
    CHECK(get(fragment->root().find("solid")->children()[1], "material") == "metal/panel");
    const auto* model = fragment->root().find("entity");
    REQUIRE(model);
    CHECK(get(*model, "targetname") == "room-model");
    CHECK(get(*model, "origin") == "10 21 30");
    CHECK(get(*model, "skin") == "3");
    REQUIRE(result->connections.size() == 1);
    auto direct = parseConnection(result->connections[0].connections.children()[0].decodedValue().value());
    CHECK(direct.target == "room-model");
    CHECK(direct.input == "Enable");
    CHECK(direct.parameter == "proxy parameter");
    CHECK(direct.delay == "1.5");
    CHECK(direct.times == "2");
    const auto& relay = fragment->root().children().back();
    REQUIRE(relay.find("connections"));
    CHECK(relay.find("connections")->children()[0].key() == "OnTrigger");
    auto outgoing = parseConnection(relay.find("connections")->children()[0].decodedValue().value());
    CHECK(outgoing.target == "outside");
    CHECK(outgoing.parameter == "outside parameter");
    CHECK(outgoing.delay == "1.25");
    CHECK(outgoing.times == "1");
}
TEST_CASE("Instance nested collapse detects cycles and depth without mutations") {
    Document parent(parsed(R"(entity { classname func_instance file A targetname outer origin "10 0 0" angles "0 90 0" })"));
    Document a(parsed(R"(entity { classname func_instance file B targetname inner origin "2 0 0" angles "0 90 0" })"));
    Document b(parsed(R"(entity { classname info_target targetname point origin "1 0 0" angles "0 0 0" })"));
    auto lookup = [&](const Document&, std::string_view file) -> Result<const Document*> { return file == "A" ? &a : &b; };
    const auto before = a.write().value();
    auto result = collapseInstance(parent, instanceId(parent), a, lookup);
    REQUIRE(result);
    auto tree = kv1::parse(result->fragment);
    REQUIRE(tree);
    const auto& point = tree->root().children()[0];
    CHECK(get(point, "targetname") == "outer-inner-point");
    CHECK(get(point, "origin") == "9 2 0");
    CHECK(a.write().value() == before);
    Document cycle(parsed(R"(entity { classname func_instance file A })"));
    const auto cycleBefore = cycle.write().value(), parentBefore = parent.write().value();
    auto cyclic = [&](const Document& referring, std::string_view) -> Result<const Document*> { return &referring == &a ? &cycle : &a; };
    auto failed = collapseInstance(parent, instanceId(parent), a, cyclic);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().message.find("cycle") != std::string::npos);
    CHECK(a.write().value() == before);
    CHECK(parent.write().value() == parentBefore);
    CHECK(cycle.write().value() == cycleBefore);
    std::vector<std::unique_ptr<Document>> deep;
    for (int i = 0; i < 9; ++i) deep.push_back(std::make_unique<Document>(parsed("entity { classname func_instance file next }")));
    std::size_t index = 1;
    auto deeper = [&](const Document&, std::string_view) -> Result<const Document*> { return deep[index++].get(); };
    auto depth = collapseInstance(parent, instanceId(parent), *deep[0], deeper);
    REQUIRE_FALSE(depth);
    CHECK(depth.error().message.find("depth") != std::string::npos);
}
TEST_CASE("Instance empty replacements preserve srctools dollar behavior") {
    const auto empty = InstanceFixup::parse(entity("entity { classname func_instance }"));
    CHECK(empty.substitute("$var/$$/$9/!$foo") == "var//9/!foo");
    auto populated = InstanceFixup::parse(entity(R"(entity { replace01 "$v yes" replace02 "$var $v" })"));
    CHECK(populated.substitute("$VAR/$v/$9/$/$$v") == "$v/yes/$9/$/$yes");
}
TEST_CASE("Instance proxy fanout composes delays fire limits and separators") {
    Document parent(parsed(R"(entity { classname func_instance file part targetname room }
        entity { classname logic_relay connections { OnTrigger "room,instance:door;Open,original,1,3" } })"));
    auto file = parsed(R"(entity { classname func_instance_io_proxy targetname proxy connections {
        OnProxyRelay "door,Open,,0.25,2"
        OnProxyRelay "door,Open,second,0.5,-1"
    } } entity { classname func_door targetname door })");
    Document templ(file);
    const auto before = parent.write().value();
    auto result = collapseInstance(parent, instanceId(parent), templ, {});
    REQUIRE(result);
    REQUIRE(result->connections.size() == 1);
    const auto& rows = result->connections[0].connections.children();
    REQUIRE(rows.size() == 2);
    std::set<std::string> parameters, times, delays;
    for (const auto& row : rows) {
        auto c = parseConnection(row.decodedValue().value());
        CHECK(c.target == "room-door");
        CHECK(c.separator == ',');
        parameters.insert(c.parameter); times.insert(c.times); delays.insert(c.delay);
    }
    CHECK(parameters == std::set<std::string>{"original", "second"});
    CHECK(times == std::set<std::string>{"2", "3"});
    CHECK(delays == std::set<std::string>{"1.25", "1.5"});
    CHECK(parent.write().value() == before);
}
TEST_CASE("Instance nested fragments have unique IDs and inherited memberships") {
    Document parent(parsed("entity { classname func_instance file A targetname top }"));
    Document a(parsed(R"(world { id 1 classname worldspawn group { id 5 } }
        entity { id 2 classname func_instance targetname first file B editor { groupid 5 visgroupid 6 } }
        entity { id 3 classname func_instance targetname second file B })"));
    Document b(parsed(fixture()));
    const auto aBefore = a.write().value(), bBefore = b.write().value(), parentBefore = parent.write().value();
    auto lookup = [&](const Document&, std::string_view) -> Result<const Document*> { return &b; };
    auto result = collapseInstance(parent, instanceId(parent), a, lookup);
    REQUIRE(result);
    auto tree = kv1::parse(result->fragment);
    REQUIRE(tree);
    std::set<int> objects, sides;
    const auto walk = [&](auto&& self, const kv1::Node& node) -> void {
        if (node.key() == "entity" || node.key() == "solid" || node.key() == "side") {
            const auto* value = node.find("id");
            REQUIRE(value);
            CHECK((node.key() == "side" ? sides : objects).insert(value->asInt().value()).second);
        }
        for (const auto& child : node.children()) self(self, child);
    };
    walk(walk, tree->root());
    CHECK(sides.size() == 2);
    CHECK(objects.size() == 6);
    const auto* first = tree->root().find("solid");
    REQUIRE(first);
    REQUIRE(first->find("editor"));
    CHECK_FALSE(get(*first->find("editor"), "groupid").empty());
    CHECK_FALSE(get(*first->find("editor"), "visgroupid").empty());
    CHECK(a.write().value() == aBefore);
    CHECK(b.write().value() == bBefore);
    CHECK(parent.write().value() == parentBefore);
}
TEST_CASE("Instance nested replacement declarations retain their variable names") {
    Document parent(parsed(R"(entity { classname func_instance file A targetname outer replace01 "$skin 4" })"));
    Document a(parsed(R"(entity { classname func_instance file B targetname inner replace01 "$skin $skin" })"));
    Document b(parsed(R"(entity { classname prop_dynamic targetname prop skin "$skin" model "models/test.mdl" })"));
    auto lookup = [&](const Document&, std::string_view) -> Result<const Document*> { return &b; };
    auto result = collapseInstance(parent, instanceId(parent), a, lookup);
    REQUIRE(result);
    auto tree = kv1::parse(result->fragment);
    REQUIRE(tree);
    CHECK(get(tree->root().children()[0], "skin") == "4");
}
TEST_CASE("Instance fixup distinguishes names from class targets after substitution") {
    auto source = entity(R"(entity { classname logic_relay target "$kind" targetname relay })");
    const auto fixup = InstanceFixup::parse(entity(R"(entity { targetname room replace01 "$kind prop_dynamic" })"));
    const auto types = [](auto cls, auto key) {
        return key == "target" ? InstanceKeyType::NameOrClass : defaultInstanceKeyType(cls, key);
    };
    auto fixed = fixedInstanceKeys(source, fixup, types, [](auto name) { return name == "prop_dynamic"; });
    CHECK(get(fixed, "target") == "prop_dynamic");
    CHECK(get(fixed, "targetname") == "room-relay");
}
TEST_CASE("Instance collapse drops hidden template content and reports only changed connections") {
    Document parent(parsed(R"(entity { classname func_instance file A targetname room }
        entity { classname logic_relay connections { OnTrigger "elsewhere,Trigger,,0.000,-1" } })"));
    Document templ(parsed(R"(hidden { entity { classname info_target targetname hidden } }
        entity { classname info_target targetname vis_hidden editor { visgroupshown 0 } }
        entity { classname info_target targetname visible })"));
    const auto before = templ.write().value();
    auto result = collapseInstance(parent, instanceId(parent), templ, {});
    REQUIRE(result);
    CHECK(result->connections.empty());
    auto tree = kv1::parse(result->fragment);
    REQUIRE(tree);
    REQUIRE(tree->root().children().size() == 1);
    CHECK(get(tree->root().children()[0], "targetname") == "room-visible");
    CHECK(templ.write().value() == before);
}
TEST_CASE("Instance nested proxy inputs rewrite connections in the parent map") {
    Document parent(parsed(R"(entity { classname func_instance file A targetname outer }
        entity { classname logic_relay connections { OnTrigger "outer,instance:inner;instance:relay;Trigger,,1,-1" } })"));
    Document a(parsed(R"(entity { classname func_instance_io_proxy targetname proxy connections {
        OnProxyRelay "inner,instance:relay;Trigger,,0.5,-1" } }
        entity { classname func_instance file B targetname inner })"));
    Document b(parsed(R"(entity { classname func_instance_io_proxy targetname proxy connections {
        OnProxyRelay "relay,Trigger,,0.25,1" } }
        entity { classname logic_relay targetname relay })"));
    const auto parentBefore = parent.write().value(), aBefore = a.write().value(), bBefore = b.write().value();
    auto lookup = [&](const Document&, std::string_view) -> Result<const Document*> { return &b; };
    auto result = collapseInstance(parent, instanceId(parent), a, lookup);
    REQUIRE(result);
    REQUIRE(result->connections.size() == 1);
    const auto direct = parseConnection(result->connections[0].connections.children()[0].decodedValue().value());
    CHECK(direct.target == "outer-inner-relay");
    CHECK(direct.input == "Trigger");
    CHECK(direct.delay == "1.75");
    CHECK(direct.times == "1");
    CHECK(parent.write().value() == parentBefore);
    CHECK(a.write().value() == aBefore);
    CHECK(b.write().value() == bBefore);
}
