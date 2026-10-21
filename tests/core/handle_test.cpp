#include "core/handle.hpp"

#include <doctest/doctest.h>

#include <concepts>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

TEST_CASE("handles are invalid by default and reject stale generations") {
    static_assert(sizeof(apex::Handle<int>) == 8);
    static_assert(std::forward_iterator<apex::Pool<int>::iterator>);
    static_assert(std::forward_iterator<apex::Pool<int>::const_iterator>);
    apex::Pool<int> pool;
    const apex::Handle<int> invalid;
    CHECK(pool.size() == 0);
    CHECK(pool.begin() == pool.end());
    CHECK_FALSE(pool.contains(invalid));
    CHECK(pool.get(invalid) == nullptr);
    CHECK_FALSE(pool.destroy(invalid));
    const auto first = pool.create(10);
    CHECK(pool.contains(first));
    REQUIRE(pool.get(first) != nullptr);
    CHECK(*pool.get(first) == 10);
    CHECK(pool.get({first.index, first.generation + 1}) == nullptr);
    CHECK(pool.get({1000, 1}) == nullptr);
    CHECK(pool.destroy(first));
    CHECK_FALSE(pool.contains(first));
    CHECK(pool.get(first) == nullptr);
    CHECK_FALSE(pool.destroy(first));
    CHECK(pool.size() == 0);
    const auto reused = pool.create(20);
    CHECK(reused.index == first.index);
    CHECK(reused.generation == first.generation + 1);
    CHECK(reused != first);
    CHECK(pool.get(first) == nullptr);
    CHECK_FALSE(pool.destroy(first));
    REQUIRE(pool.get(reused) != nullptr);
    CHECK(*pool.get(reused) == 20);
    CHECK(pool.size() == 1);
    std::unordered_set<apex::Handle<int>> handles{invalid, first, reused, reused};
    CHECK(handles.size() == 3);
    CHECK(handles.contains(first));
    CHECK(first < reused);
}

TEST_CASE("pool iteration skips holes and permits value edits") {
    apex::Pool<std::string> pool;
    const auto first = pool.create("one");
    const auto middle = pool.create("two");
    const auto last = pool.create("three");
    CHECK(pool.destroy(first));
    CHECK(pool.destroy(middle));
    for (auto& value : pool) {
        value += "!";
    }
    const auto replacement = pool.create("replacement");
    CHECK(replacement.index == middle.index);
    const auto& constPool = pool;
    CHECK(constPool.size() == 2);
    REQUIRE(constPool.get(last) != nullptr);
    CHECK(*constPool.get(last) == "three!");
    CHECK(constPool.get(first) == nullptr);
    std::vector<std::string> values(constPool.cbegin(), constPool.cend());
    REQUIRE(values.size() == 2);
    CHECK(values[0] == "replacement");
    CHECK(values[1] == "three!");
    auto it = pool.begin();
    CHECK(it->size() == 11);
    CHECK(*it++ == "replacement");
    CHECK(*it == "three!");
    CHECK(pool.destroy(last));
    CHECK(pool.destroy(replacement));
    CHECK(pool.begin() == pool.end());
    const auto next = pool.create("next");
    CHECK(next.index == replacement.index);
    CHECK(next.generation == replacement.generation + 1);
    CHECK_FALSE(pool.contains(replacement));
}

TEST_CASE("pool supports move-only values and remains reusable after moving") {
    apex::Pool<std::unique_ptr<int>> pool;
    const auto handle = pool.create(std::make_unique<int>(7));
    // Force growth while preserving handle resolution.
    for (int i = 0; i < 100; ++i) {
        pool.create(std::make_unique<int>(i));
    }
    REQUIRE(pool.get(handle) != nullptr);
    CHECK(**pool.get(handle) == 7);
    auto moved = std::move(pool);
    CHECK(moved.size() == 101);
    REQUIRE(moved.get(handle) != nullptr);
    CHECK(**moved.get(handle) == 7);
    CHECK(pool.size() == 0);
    CHECK(pool.begin() == pool.end());
    CHECK(pool.contains(pool.create(std::make_unique<int>(3))));
    apex::Pool<std::unique_ptr<int>> assigned;
    assigned = std::move(moved);
    CHECK(assigned.size() == 101);
    CHECK(moved.size() == 0);
    CHECK(assigned.destroy(handle));
    CHECK(assigned.get(handle) == nullptr);
}
