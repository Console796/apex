#pragma once

#include "core/math.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <queue>
#include <span>
#include <unordered_map>
#include <vector>

namespace apex::geometry {

class Bvh {
public:
    struct Item {
        std::uint32_t id;
        AABB bounds;
    };

    // IDs are unique. Invalid boxes and duplicate IDs throw invalid_argument; invalid
    // builds preserve the old tree. Flat boxes (including points) are valid.
    void build(std::span<const Item> items);
    void insert(std::uint32_t id, const AABB& bounds);
    void remove(std::uint32_t id); // Missing IDs are a no-op.
    void update(std::uint32_t id, const AABB& bounds); // Missing IDs throw out_of_range.

    bool contains(std::uint32_t id) const { return locations_.contains(id); }
    std::size_t size() const { return locations_.size(); }
    AABB bounds() const { return root_ == -1 ? AABB{} : nodes_[root_].bounds; }

    // Root-area-normalized SAH cost / cost at the last build (1 for empty or
    // zero-area trees). Callers may rebuild above 2. Inserts/removes also affect cost.
    double quality() const;
    void rebuild();

    // Visits intersected item boxes in increasing entry t, then ID. Boundary contact
    // counts, including at tMax; a returned finite t in [0, current tMax) prunes later
    // visits. The callback performs the caller's exact primitive intersection.
    template <class F> void raycast(const Ray& ray, double tMax, F&& visit) const;
    template <class F> void queryBox(const AABB& box, F&& visit) const;

    // Outward normals: inside is signedDistance <= 0. fullyInside certifies the
    // item box; enclosed subtrees skip further plane tests. Empty planes report
    // every item as fully inside. Boundary contact counts as intersection.
    template <class F> void queryPlanes(std::span<const Plane> planes, F&& visit) const;
    template <class F> void queryRadius(DVec3 point, double radius, F&& visit) const;

    // Writes require exclusive access; const queries use only local scratch storage.
private:
    struct Node {
        AABB bounds;
        std::array<Item, 4> items{};
        int parent = -1;
        int left = -1;
        int right = -1;
        int height = 0;
        int count = 0;
        std::uint32_t minId = 0;
        double cost = 0.0;

        bool leaf() const { return left == -1; }
    };

    std::vector<Node> nodes_;
    std::vector<int> free_;
    std::unordered_map<std::uint32_t, int> locations_; // Lookup only; never traversal order.
    int root_ = -1;
    double baseline_ = 0.0;

    static void validate(const AABB& box);
    static double area(const AABB& box);
    static std::optional<double> rayEntry(const AABB& box, const Ray& ray, double tMax);
    static double distanceSquared(const AABB& box, DVec3 point);
    static int classify(const AABB& box, std::span<const Plane> planes);
    int allocate();
    void release(int index);
    int buildRange(std::span<Item> items, int parent);
    void refit(int index);
    void repair(int index);
    int rotateLeft(int index);
    int rotateRight(int index);
    void replace(int index, int replacement);
    double normalizedCost() const;

    template <class Test, class Visit> void query(Test&& test, Visit&& visit) const;
};

template <class F> void Bvh::raycast(const Ray& ray, double tMax, F&& visit) const {
    if (root_ == -1 || !(tMax >= 0.0)) {
        return;
    }
    struct Candidate {
        double entry;
        std::uint32_t id;
        int node;
        int item; // -1 expands a node; otherwise visits an item in a leaf.
    };
    const auto later = [](const Candidate& a, const Candidate& b) {
        if (a.entry != b.entry) {
            return a.entry > b.entry;
        }
        if (a.id != b.id) {
            return a.id > b.id;
        }
        return a.item > b.item; // Expand before visiting an equal-key candidate.
    };
    std::priority_queue<Candidate, std::vector<Candidate>, decltype(later)> pending(later);
    const auto pushNode = [&](int index) {
        if (const auto entry = rayEntry(nodes_[index].bounds, ray, tMax)) {
            pending.push({*entry, nodes_[index].minId, index, -1});
        }
    };
    pushNode(root_);
    while (!pending.empty()) {
        const auto candidate = pending.top();
        pending.pop();
        if (candidate.entry > tMax) {
            break;
        }
        const auto& node = nodes_[candidate.node];
        if (candidate.item != -1) {
            const auto hit = visit(candidate.id, tMax);
            if (hit && std::isfinite(*hit) && *hit >= 0.0 && *hit < tMax) {
                tMax = *hit;
            }
        } else if (node.leaf()) {
            for (int i = 0; i < node.count; ++i) {
                if (const auto entry = rayEntry(node.items[i].bounds, ray, tMax)) {
                    pending.push({*entry, node.items[i].id, candidate.node, i});
                }
            }
        } else {
            pushNode(node.left);
            pushNode(node.right);
        }
    }
}

template <class Test, class Visit> void Bvh::query(Test&& test, Visit&& visit) const {
    if (root_ == -1) {
        return;
    }
    struct Pending {
        int node;
        bool inside;
    };
    std::vector<Pending> pending{{root_, false}};
    while (!pending.empty()) {
        const auto [index, inherited] = pending.back();
        pending.pop_back();
        const auto& node = nodes_[index];
        const int classification = inherited ? 1 : test(node.bounds);
        if (classification < 0) {
            continue;
        }
        const bool inside = classification > 0;
        if (node.leaf()) {
            for (int i = 0; i < node.count; ++i) {
                const int itemClass = inside ? 1 : test(node.items[i].bounds);
                if (itemClass >= 0) {
                    visit(node.items[i], itemClass > 0);
                }
            }
        } else {
            pending.push_back({node.right, inside});
            pending.push_back({node.left, inside});
        }
    }
}

template <class F> void Bvh::queryBox(const AABB& box, F&& visit) const {
    query(
        [&](const AABB& candidate) {
            return !box.intersects(candidate) ? -1 : (box.contains(candidate) ? 1 : 0);
        },
        [&](const Item& item, bool) { visit(item.id); });
}

template <class F> void Bvh::queryPlanes(std::span<const Plane> planes, F&& visit) const {
    query([&](const AABB& box) { return classify(box, planes); },
          [&](const Item& item, bool inside) { visit(item.id, inside); });
}

template <class F> void Bvh::queryRadius(DVec3 point, double radius, F&& visit) const {
    if (!(radius >= 0.0)) {
        return;
    }
    const double squared = radius * radius;
    query([&](const AABB& box) { return distanceSquared(box, point) <= squared ? 0 : -1; },
          [&](const Item& item, bool) { visit(item.id, distanceSquared(item.bounds, point)); });
}

} // namespace apex::geometry
