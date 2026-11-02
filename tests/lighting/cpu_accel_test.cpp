#include "lighting/cpu_accel.hpp"

#include <doctest/doctest.h>

using namespace apex;
using namespace apex::lighting;

TEST_CASE("lighting: CPU acceleration queries and instance updates without Vulkan") {
    const std::array<std::array<float, 3>, 4> positions{{{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}}};
    const std::array<std::uint32_t, 6> indices{0, 1, 2, 0, 2, 3};
    CHECK_FALSE(CpuBlas::build({}, indices));
    const std::array<std::uint32_t, 3> bad{0, 1, 4};
    CHECK_FALSE(CpuBlas::build(positions, bad));
    auto mesh = CpuBlas::build(positions, indices); REQUIRE(mesh);
    std::array<CpuInstance, 2> instances{{{*mesh, {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}}, 17},
                                        {*mesh, {{{1, 0, 0, 4}, {0, 1, 0, 0}, {0, 0, 1, 2}}}, 29}}};
    auto scene = CpuAccel::build(instances); REQUIRE(scene);
    const auto hit = (*scene)->closest({0.25f, 0, 10}, {0, 0, -1}, 100);
    REQUIRE(hit);
    CHECK(hit->distance == doctest::Approx(10));
    CHECK(hit->customIndex == 17);
    CHECK(hit->primitive == 0);
    CHECK((*scene)->occluded({0.25f, 0, 10}, {0, 0, -1}, 11));
    CHECK_FALSE((*scene)->occluded({0.25f, 0, 10}, {0, 0, -1}, 9));
    CHECK_FALSE((*scene)->closest({2, 0, 10}, {0, 0, -1}, 100));
    CHECK_FALSE((*scene)->updateInstances(std::span(instances).first(1)));
    instances[1].transform[0][3] = 8;
    instances[1].transform[2][3] = 4;
    REQUIRE((*scene)->updateInstances(instances));
    CHECK_FALSE((*scene)->closest({4.25f, 0, 10}, {0, 0, -1}, 100));
    const auto moved = (*scene)->closest({8.25f, 0, 10}, {0, 0, -1}, 100);
    REQUIRE(moved);
    CHECK(moved->distance == doctest::Approx(6));
    CHECK(moved->customIndex == 29);
    auto empty = CpuAccel::build({}); REQUIRE(empty);
    CHECK_FALSE((*empty)->occluded({0, 0, 10}, {0, 0, -1}, 100));
    CHECK_FALSE((*empty)->closest({0, 0, 10}, {0, 0, -1}, 100));
}
