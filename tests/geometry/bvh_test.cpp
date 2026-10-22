#include "geometry/bvh.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace apex;
using apex::geometry::Bvh;

namespace {

using Ids = std::vector<std::uint32_t>;

double random(std::mt19937& engine, double low, double high) {
    return std::uniform_real_distribution<double>{low, high}(engine);
}

DVec3 randomPoint(std::mt19937& engine, double extent = 65536.0) {
    return {random(engine, -extent, extent), random(engine, -extent, extent),
            random(engine, -extent, extent)};
}

AABB randomBox(std::mt19937& engine, std::uint32_t id) {
    const auto center = randomPoint(engine);
    DVec3 half{random(engine, 0.0, 2048.0), random(engine, 0.0, 2048.0),
               random(engine, 0.0, 2048.0)};
    if (id % 7 == 0) {
        half[id % 3] = 0.0;
    }
    if (id % 29 == 0) {
        half = DVec3{0.0};
    }
    return {glm::max(center - half, DVec3{-65536.0}),
            glm::min(center + half, DVec3{65536.0})};
}

std::vector<Bvh::Item> randomItems(std::mt19937& engine, std::uint32_t count) {
    std::vector<Bvh::Item> items;
    for (std::uint32_t i = 0; i < count; ++i) {
        items.push_back({i, randomBox(engine, i)});
    }
    return items;
}

// Independent interval intersection in extended precision; no reciprocal directions.
std::optional<double> bruteRay(const AABB& box, const Ray& ray, double maximum) {
    long double near = 0.0L;
    long double far = maximum;
    for (int axis = 0; axis < 3; ++axis) {
        if (ray.dir[axis] == 0.0) {
            if (ray.origin[axis] < box.min[axis] || ray.origin[axis] > box.max[axis]) {
                return std::nullopt;
            }
        } else {
            long double a = (static_cast<long double>(box.min[axis]) - ray.origin[axis]) /
                            ray.dir[axis];
            long double b = (static_cast<long double>(box.max[axis]) - ray.origin[axis]) /
                            ray.dir[axis];
            if (a > b) {
                std::swap(a, b);
            }
            near = std::max(near, a);
            far = std::min(far, b);
            if (near > far) {
                return std::nullopt;
            }
        }
    }
    return static_cast<double>(near);
}

double bruteDistance(const AABB& box, DVec3 point) {
    double result = 0.0;
    for (int axis = 0; axis < 3; ++axis) {
        const double closest = std::clamp(point[axis], box.min[axis], box.max[axis]);
        result += (point[axis] - closest) * (point[axis] - closest);
    }
    return result;
}

int brutePlanes(const AABB& box, std::span<const Plane> planes) {
    bool inside = true;
    for (const auto& plane : planes) {
        double low = std::numeric_limits<double>::infinity();
        double high = -low;
        for (int corner = 0; corner < 8; ++corner) {
            const DVec3 point{corner & 1 ? box.max.x : box.min.x,
                              corner & 2 ? box.max.y : box.min.y,
                              corner & 4 ? box.max.z : box.min.z};
            const double distance = plane.signedDistance(point);
            low = std::min(low, distance);
            high = std::max(high, distance);
        }
        if (low > 0.0) {
            return -1;
        }
        inside &= high <= 0.0;
    }
    return inside ? 1 : 0;
}

std::array<Plane, 6> volume(DVec3 center, DVec3 half, double angle, double tilt = 0.0) {
    const double c = std::cos(angle), s = std::sin(angle);
    const double ct = std::cos(tilt), st = std::sin(tilt);
    const std::array<DVec3, 3> axes{{{ct * c, s, -st * c},
                                     {-ct * s, c, st * s},
                                     {st, 0.0, ct}}};
    std::array<Plane, 6> result;
    for (int i = 0; i < 3; ++i) {
        result[2 * i] = {axes[i], glm::dot(axes[i], center) + half[i]};
        result[2 * i + 1] = {-axes[i], glm::dot(-axes[i], center) + half[i]};
    }
    return result;
}

void checkIds(Ids actual, Ids expected) {
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    CHECK(actual == expected);
}

void checkRay(const Bvh& bvh, const std::vector<Bvh::Item>& items, const Ray& ray, double limit) {
    std::vector<std::pair<double, std::uint32_t>> expected;
    for (const auto& item : items) {
        if (const auto entry = bruteRay(item.bounds, ray, limit)) {
            expected.emplace_back(*entry, item.id);
        }
    }
    std::sort(expected.begin(), expected.end());
    Ids actual;
    bvh.raycast(ray, limit, [&](std::uint32_t id, double current) -> std::optional<double> {
        CHECK(current == limit);
        actual.push_back(id);
        return std::nullopt;
    });
    Ids ordered;
    for (const auto& [entry, id] : expected) {
        ordered.push_back(id);
    }
    CHECK(actual == ordered);

    // Treat the box as the exact primitive to exercise shrinking the traversal limit.
    std::optional<std::uint32_t> nearest;
    double closest = limit;
    bvh.raycast(ray, limit, [&](std::uint32_t id, double current) -> std::optional<double> {
        const auto item = std::find_if(items.begin(), items.end(),
                                      [=](const auto& candidate) { return candidate.id == id; });
        REQUIRE(item != items.end());
        const auto hit = bruteRay(item->bounds, ray, current);
        if (hit && *hit < current) {
            nearest = id;
            closest = *hit;
            return hit;
        }
        return std::nullopt;
    });
    if (!expected.empty() && expected.front().first < limit) {
        CHECK(nearest == expected.front().second);
        CHECK(closest == doctest::Approx(expected.front().first));
    } else {
        CHECK_FALSE(nearest.has_value());
    }
}

void checkQueries(const Bvh& bvh, const std::vector<Bvh::Item>& items,
                  std::mt19937& engine, int count) {
    CHECK(bvh.size() == items.size());
    AABB all;
    for (const auto& item : items) {
        all.expand(item.bounds);
    }
    CHECK(bvh.bounds().min == all.min);
    CHECK(bvh.bounds().max == all.max);
    Ids allIds, expectedIds;
    bvh.queryBox(all, [&](auto id) { allIds.push_back(id); });
    for (const auto& item : items) {
        expectedIds.push_back(item.id);
    }
    checkIds(allIds, expectedIds);
    for (int i = 0; i < count; ++i) {
        CAPTURE(i);
        auto direction = randomPoint(engine, 1.0);
        if (i % 3 == 0) {
            direction = DVec3{0.0};
            direction[(i / 3) % 3] = i % 2 == 0 ? 1.0 : -1.0;
        }
        checkRay(bvh, items, {randomPoint(engine), direction}, 250000.0);

        const auto center = randomPoint(engine);
        const double radius = random(engine, 0.0, 20000.0);
        const AABB box{center - DVec3{radius}, center + DVec3{radius}};
        const auto planes = volume(center, DVec3{radius}, random(engine, -3.0, 3.0),
                                   random(engine, -3.0, 3.0));
        Ids expectedBox, expectedRadius;
        std::vector<std::pair<std::uint32_t, bool>> expectedPlanes;
        for (const auto& item : items) {
            if (box.intersects(item.bounds)) {
                expectedBox.push_back(item.id);
            }
            if (bruteDistance(item.bounds, center) <= radius * radius) {
                expectedRadius.push_back(item.id);
            }
            const int classification = brutePlanes(item.bounds, planes);
            if (classification >= 0) {
                expectedPlanes.emplace_back(item.id, classification > 0);
            }
        }
        Ids actualBox, actualRadius;
        std::vector<std::pair<std::uint32_t, bool>> actualPlanes;
        bvh.queryBox(box, [&](std::uint32_t id) { actualBox.push_back(id); });
        bvh.queryPlanes(planes, [&](std::uint32_t id, bool inside) {
            actualPlanes.emplace_back(id, inside);
        });
        bvh.queryRadius(center, radius, [&](std::uint32_t id, double squared) {
            actualRadius.push_back(id);
            const auto item =
                std::find_if(items.begin(), items.end(),
                             [=](const auto& candidate) { return candidate.id == id; });
            REQUIRE(item != items.end());
            CHECK(squared == doctest::Approx(bruteDistance(item->bounds, center)));
        });
        checkIds(actualBox, expectedBox);
        checkIds(actualRadius, expectedRadius);
        std::sort(actualPlanes.begin(), actualPlanes.end());
        std::sort(expectedPlanes.begin(), expectedPlanes.end());
        CHECK(actualPlanes == expectedPlanes);
    }
}

} // namespace

TEST_CASE("BVH seeded 5000 box build agrees with brute force for thousands of queries") {
    std::mt19937 engine{0xB7A2022};
    const auto items = randomItems(engine, 5000);
    Bvh bvh;
    bvh.build(items);
    CHECK(bvh.quality() == doctest::Approx(1.0));
    checkQueries(bvh, items, engine, 2000);
}

TEST_CASE("BVH handles parallel grazing inside flat and clip box rays inclusively") {
    const std::vector<Bvh::Item> items{
        {99, {{-65536.0, -65536.0, -65536.0}, {-65535.0, -65535.0, -65535.0}}},
        {2, {{65535.0, 65535.0, 65535.0}, {65536.0, 65536.0, 65536.0}}},
        {7, {{0.0, 0.0, 0.0}, {2.0, 2.0, 2.0}}},
        {0, {{0.0, 0.0, 0.0}, {2.0, 2.0, 2.0}}},
        {55, {{1.0, -2.0, -2.0}, {1.0, 2.0, 2.0}}},
        {15, {{3.0, 3.0, 3.0}, {3.0, 3.0, 3.0}}},
        {81, {{-524288.0, -524288.0, -524288.0}, {524288.0, 524288.0, 524288.0}}}};
    Bvh bvh;
    bvh.build(items);
    const std::array<Ray, 14> rays{{
        {{-4.0, 2.0, 2.0}, {1.0, 0.0, -0.0}},
        {{-4.0, 0.0, 0.0}, {1.0, 0.0, 0.0}},
        {{-4.0, 2.000001, 2.0}, {1.0, 0.0, 0.0}},
        {{-4.0, -4.0, -4.0}, {1.0, 1.0, 1.0}},
        {{-2.0, 4.0, 0.0}, {1.0, -1.0, 0.0}},
        {{1.0, 1.0, 1.0}, {1.0, 0.0, 0.0}},
        {{2.0, 2.0, 2.0}, {-1.0, 0.0, 0.0}},
        {{65536.0, 65536.0, 65536.0}, {0.0, 0.0, -1.0}},
        {{-65536.0, -65536.0, -65536.0}, {0.0, 1.0, 0.0}},
        {{-524289.0, 524288.0, 524288.0}, {1.0, 0.0, 0.0}},
        {{1.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},
        {{3.0, 3.0, 3.0}, {-0.0, 0.0, 0.0}},
        {{0.0, 0.0, 0.0}, {std::numeric_limits<double>::denorm_min(), 1.0, 0.0}},
        {{1.0, 0.0, 0.0}, {-std::numeric_limits<double>::denorm_min(), 1.0, 0.0}}}};
    for (const auto& ray : rays) {
        for (const double limit : {0.0, 1.0, 4.0, 2000000.0,
                                   std::numeric_limits<double>::infinity()}) {
            checkRay(bvh, items, ray, limit);
        }
    }
}

TEST_CASE("BVH planes certify enclosed subtrees and include touching boxes and radius boundaries") {
    const std::vector<Bvh::Item> items{{1, {{-1.0, -1.0, -1.0}, {1.0, 1.0, 1.0}}},
                                       {2, {{1.0, -1.0, -1.0}, {3.0, 1.0, 1.0}}},
                                       {3, {{3.0, 0.0, 0.0}, {3.0, 0.0, 0.0}}}};
    Bvh bvh;
    bvh.build(items);
    std::vector<std::pair<std::uint32_t, bool>> result;
    const auto planes = volume(DVec3{0.0}, DVec3{1.0}, 0.0);
    bvh.queryPlanes(planes, [&](auto id, bool inside) { result.emplace_back(id, inside); });
    CHECK(result == std::vector<std::pair<std::uint32_t, bool>>{{1, true}, {2, false}});
    result.clear();
    bvh.queryPlanes({}, [&](auto id, bool inside) { result.emplace_back(id, inside); });
    CHECK(result == std::vector<std::pair<std::uint32_t, bool>>{{1, true}, {2, true}, {3, true}});
    result.clear();
    const auto enclosed = volume(DVec3{0.0}, DVec3{4.0}, 0.3);
    bvh.queryPlanes(enclosed, [&](auto id, bool inside) { result.emplace_back(id, inside); });
    CHECK(result == std::vector<std::pair<std::uint32_t, bool>>{{1, true}, {2, true}, {3, true}});
    Ids ids;
    bvh.queryRadius({0.0, 0.0, 0.0}, 3.0, [&](auto id, double) { ids.push_back(id); });
    CHECK(ids == Ids{1, 2, 3});
    ids.clear();
    bvh.queryRadius({3.0, 0.0, 0.0}, 0.0, [&](auto id, double squared) {
        CHECK(squared == 0.0);
        ids.push_back(id);
    });
    CHECK(ids == Ids{2, 3});
    ids.clear();
    bvh.queryBox({{3.0, 0.0, 0.0}, {3.0, 0.0, 0.0}}, [&](auto id) { ids.push_back(id); });
    CHECK(ids == Ids{2, 3});
}

TEST_CASE("BVH repeated edits agree with brute force and caller rebuild keeps quality bounded") {
    std::mt19937 engine{0xED172022};
    auto items = randomItems(engine, 5000);
    Bvh bvh;
    bvh.build(items);
    std::uint32_t nextId = 5000;
    for (int batch = 0; batch < 50; ++batch) {
        CAPTURE(batch);
        for (int edit = 0; edit < 100; ++edit) {
            const auto index =
                std::uniform_int_distribution<std::size_t>{0, items.size() - 1}(engine);
            auto& item = items[index];
            if (edit % 5 == 0) {
                const auto id = item.id;
                bvh.remove(id);
                CHECK_FALSE(bvh.contains(id));
                bvh.remove(id);
                item = {nextId, randomBox(engine, nextId)};
                ++nextId;
                bvh.insert(item.id, item.bounds);
            } else {
                item.bounds = edit % 5 == 1 ? AABB{item.bounds.center(), item.bounds.center()}
                                           : randomBox(engine, item.id);
                bvh.update(item.id, item.bounds);
            }
            CHECK(bvh.contains(item.id));
        }
        CHECK(std::isfinite(bvh.quality()));
        CHECK(bvh.quality() < 8.0);
        checkQueries(bvh, items, engine, 10);
        if (bvh.quality() > 2.0) {
            bvh.rebuild();
            CHECK(bvh.quality() == doctest::Approx(1.0));
            checkQueries(bvh, items, engine, 2);
        }
        CHECK(bvh.quality() <= 2.0);
    }
    for (const auto& item : items) {
        bvh.remove(item.id);
    }
    CHECK(bvh.size() == 0);
    CHECK(bvh.bounds().empty());
    CHECK(bvh.quality() == 1.0);
    checkQueries(bvh, {}, engine, 1);
    bvh.insert(17, {{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}});
    CHECK(bvh.contains(17));
    bvh.build({});
    CHECK(bvh.size() == 0);
    bvh.rebuild();
    CHECK(bvh.bounds().empty());
}

TEST_CASE("BVH coincident and ordered inserts stay stable and ray ties use caller IDs") {
    const AABB box{{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}};
    Bvh inserted;
    std::vector<Bvh::Item> items;
    for (std::uint32_t i = 4096; i > 0; --i) {
        inserted.insert(i, box);
        items.push_back({i, box});
    }
    inserted.rebuild();
    Bvh built;
    built.build(items);
    std::reverse(items.begin(), items.end());
    Bvh reversed;
    reversed.build(items);
    const auto order = [&](const Bvh& bvh) {
        Ids ids;
        bvh.raycast({{-1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}}, 2.0,
                    [&](auto id, double) -> std::optional<double> {
                        ids.push_back(id);
                        return std::nullopt;
                    });
        return ids;
    };
    Ids expected(4096);
    std::iota(expected.begin(), expected.end(), 1);
    CHECK(order(inserted) == expected);
    CHECK(order(built) == expected);
    CHECK(order(reversed) == expected);
    const auto boxOrder = [&](const Bvh& bvh) {
        Ids ids;
        bvh.queryBox(box, [&](auto id) { ids.push_back(id); });
        return ids;
    };
    CHECK(boxOrder(built) == boxOrder(reversed));
    CHECK(built.quality() == reversed.quality());
    Ids enclosedIds;
    const auto planes = volume(DVec3{0.0}, DVec3{2.0}, 0.0);
    built.queryPlanes(planes, [&](auto id, bool inside) {
        CHECK(inside);
        enclosedIds.push_back(id);
    });
    checkIds(enclosedIds, expected);

    Bvh points;
    for (std::uint32_t i = 0; i < 4096; ++i) {
        const DVec3 point{static_cast<double>(i), 0.0, 0.0};
        points.insert(i, {point, point});
    }
    CHECK(points.size() == 4096);
    CHECK(points.quality() == 1.0);
    points.rebuild();
    for (std::uint32_t i = 0; i < 4096; ++i) {
        points.update(i, {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}});
    }
    Ids ids;
    points.queryRadius(DVec3{0.0}, 0.0, [&](auto id, double) { ids.push_back(id); });
    CHECK(ids.size() == 4096);
}

TEST_CASE("BVH rejects invalid boxes and duplicate IDs without changing existing items") {
    Bvh bvh;
    const AABB valid{{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}};
    bvh.insert(42, valid);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<AABB> invalid{{}, {{1.0, 0.0, 0.0}, {0.0, 1.0, 1.0}}};
    for (int axis = 0; axis < 3; ++axis) {
        for (const double bad : {nan, inf, -inf}) {
            auto box = valid;
            box.min[axis] = bad;
            invalid.push_back(box);
            box = valid;
            box.max[axis] = bad;
            invalid.push_back(box);
        }
    }
    for (const auto& box : invalid) {
        CHECK_THROWS_AS(bvh.insert(2, box), std::invalid_argument);
        CHECK_THROWS_AS(bvh.update(42, box), std::invalid_argument);
        const std::array<Bvh::Item, 2> items{{{3, valid}, {4, box}}};
        CHECK_THROWS_AS(bvh.build(items), std::invalid_argument);
        CHECK(bvh.size() == 1);
        CHECK(bvh.bounds().min == valid.min);
        CHECK(bvh.bounds().max == valid.max);
    }
    CHECK_THROWS_AS(bvh.insert(42, valid), std::invalid_argument);
    CHECK_THROWS_AS(bvh.update(123, valid), std::out_of_range);
    const std::array<Bvh::Item, 2> duplicate{{{3, valid}, {3, valid}}};
    CHECK_THROWS_AS(bvh.build(duplicate), std::invalid_argument);
    CHECK(bvh.contains(42));
    bvh.remove(123);
    CHECK(bvh.size() == 1);
}

TEST_CASE("BVH reports 100000 box build and single update timings") {
    std::mt19937 engine{0x71AE2022};
    auto items = randomItems(engine, 100000);
    Bvh bvh;
    const auto start = std::chrono::steady_clock::now();
    bvh.build(items);
    const auto built = std::chrono::steady_clock::now();
    // A contained change exercises the ancestor-refit path.
    bvh.update(items[1].id, {items[1].bounds.center(), items[1].bounds.center()});
    const auto updated = std::chrono::steady_clock::now();
    MESSAGE("BVH 100000 box build: ",
            std::chrono::duration<double, std::milli>(built - start).count(), " ms");
    MESSAGE("BVH single contained update: ",
            std::chrono::duration<double, std::micro>(updated - built).count(), " us");
    CHECK(bvh.size() == items.size());
    CHECK(std::isfinite(bvh.quality()));
}
