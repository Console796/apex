#include "document/sidelist_remap.hpp"

#include <doctest/doctest.h>

using namespace apex;
using namespace apex::document;

namespace {
kv1::Node overlay(std::string sides) {
    auto entity = kv1::Node::block("entity");
    (void)entity.append(kv1::Node::value("classname", "info_overlay"));
    (void)entity.append(kv1::Node::value("sides", std::move(sides)));
    (void)entity.append(kv1::Node::value("targetname", "12"));
    return entity;
}
InstanceKeyType types(std::string_view classname, std::string_view key) {
    return classname == "info_overlay" && key == "sides" ? InstanceKeyType::SideList : InstanceKeyType::String;
}
} // namespace

TEST_CASE("[document][sidelist] remap keeps or drops unmapped sides and touches only sidelist keys") {
    const std::map<int, int> sides{{3, 30}, {1, 10}, {12, 120}};
    auto kept = overlay("3 7  1");
    const auto changed = remapSideLists(kept, types, sides, UnmappedSide::Keep);
    REQUIRE(changed.size() == 1);
    CHECK(changed[0] == std::pair<std::string, std::string>{"sides", "30 7 10"});
    CHECK(kept.find("sides")->decodedValue().value_or("") == "30 7 10");
    CHECK(kept.find("targetname")->decodedValue().value_or("") == "12");

    auto dropped = overlay("3 7 1");
    CHECK(remapSideLists(dropped, types, sides, UnmappedSide::Drop).size() == 1);
    CHECK(dropped.find("sides")->decodedValue().value_or("") == "30 10");

    auto unchanged = overlay("7 8");
    CHECK(remapSideLists(unchanged, types, sides, UnmappedSide::Keep).empty());
    CHECK(remapSideLists(unchanged, {}, sides, UnmappedSide::Drop).empty());
}
