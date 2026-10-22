#include "geometry/bvh.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace apex::geometry {
namespace {

constexpr int binCount = 16;

bool finite(DVec3 point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

AABB combined(AABB a, const AABB& b) {
    a.expand(b);
    return a;
}

} // namespace

void Bvh::validate(const AABB& box) {
    if (box.empty() || !finite(box.min) || !finite(box.max)) {
        throw std::invalid_argument("BVH requires a finite, nonempty box");
    }
}

double Bvh::area(const AABB& box) {
    if (box.empty()) {
        return 0.0;
    }
    const auto size = box.max - box.min;
    return 2.0 * (size.x * size.y + size.y * size.z + size.z * size.x);
}

std::optional<double> Bvh::rayEntry(const AABB& box, const Ray& ray, double tMax) {
    if (!finite(ray.origin) || !finite(ray.dir)) {
        return std::nullopt;
    }
    double near = 0.0;
    double far = tMax;
    for (int axis = 0; axis < 3; ++axis) {
        if (ray.dir[axis] == 0.0) {
            if (ray.origin[axis] < box.min[axis] || ray.origin[axis] > box.max[axis]) {
                return std::nullopt;
            }
        } else {
            // Divide directly: reciprocal multiplication can turn 0 into NaN for
            // tiny directions, including a ray exactly on a slab boundary.
            double a = (box.min[axis] - ray.origin[axis]) / ray.dir[axis];
            double b = (box.max[axis] - ray.origin[axis]) / ray.dir[axis];
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
    return near;
}

double Bvh::distanceSquared(const AABB& box, DVec3 point) {
    const auto delta = glm::max(glm::max(box.min - point, point - box.max), DVec3{0.0});
    return glm::dot(delta, delta);
}

int Bvh::classify(const AABB& box, std::span<const Plane> planes) {
    bool inside = true;
    for (const auto& plane : planes) {
        DVec3 near, far;
        for (int axis = 0; axis < 3; ++axis) {
            near[axis] = plane.normal[axis] >= 0.0 ? box.min[axis] : box.max[axis];
            far[axis] = plane.normal[axis] >= 0.0 ? box.max[axis] : box.min[axis];
        }
        if (plane.signedDistance(near) > 0.0) {
            return -1;
        }
        inside &= plane.signedDistance(far) <= 0.0;
    }
    return inside ? 1 : 0;
}

int Bvh::allocate() {
    if (!free_.empty()) {
        const int index = free_.back();
        free_.pop_back();
        return index;
    }
    nodes_.emplace_back();
    return static_cast<int>(nodes_.size() - 1);
}

void Bvh::release(int index) {
    nodes_[index] = Node{};
    free_.push_back(index);
}

void Bvh::refit(int index) {
    auto& node = nodes_[index];
    node.bounds = {};
    if (node.leaf()) {
        std::sort(node.items.begin(), node.items.begin() + node.count,
                  [](const Item& a, const Item& b) { return a.id < b.id; });
        for (int i = 0; i < node.count; ++i) {
            node.bounds.expand(node.items[i].bounds);
        }
        node.minId = node.items[0].id;
        node.height = 0;
        node.cost = area(node.bounds) * node.count;
    } else {
        const auto& left = nodes_[node.left];
        const auto& right = nodes_[node.right];
        node.bounds = combined(left.bounds, right.bounds);
        node.minId = std::min(left.minId, right.minId);
        node.height = 1 + std::max(left.height, right.height);
        node.cost = area(node.bounds) + left.cost + right.cost;
    }
}

int Bvh::buildRange(std::span<Item> items, int parent) {
    const int index = allocate();
    nodes_[index].parent = parent;
    if (items.size() <= 4) {
        nodes_[index].count = static_cast<int>(items.size());
        std::copy(items.begin(), items.end(), nodes_[index].items.begin());
        for (const auto& item : items) {
            locations_.emplace(item.id, index);
        }
        refit(index);
        return index;
    }

    AABB centroids;
    for (const auto& item : items) {
        centroids.expand(item.bounds.center());
    }
    const auto extent = centroids.max - centroids.min;
    double bestCost = std::numeric_limits<double>::infinity();
    int bestAxis = -1;
    int bestBin = -1;
    const auto binFor = [&](const Item& item, int axis) {
        const double fraction = (item.bounds.center()[axis] - centroids.min[axis]) / extent[axis];
        return std::clamp(static_cast<int>(fraction * binCount), 0, binCount - 1);
    };
    for (int axis = 0; axis < 3; ++axis) {
        if (!(extent[axis] > 0.0)) {
            continue;
        }
        struct Bin {
            AABB bounds;
            std::size_t count = 0;
        };
        std::array<Bin, binCount> bins{};
        for (const auto& item : items) {
            auto& bin = bins[binFor(item, axis)];
            bin.bounds.expand(item.bounds);
            ++bin.count;
        }
        std::array<Bin, binCount> suffix{};
        Bin right;
        for (int i = binCount - 1; i >= 0; --i) {
            right.bounds.expand(bins[i].bounds);
            right.count += bins[i].count;
            suffix[i] = right;
        }
        Bin left;
        for (int i = 0; i < binCount - 1; ++i) {
            left.bounds.expand(bins[i].bounds);
            left.count += bins[i].count;
            const auto& other = suffix[i + 1];
            if (left.count == 0 || other.count == 0) {
                continue;
            }
            const double cost = area(left.bounds) * static_cast<double>(left.count) +
                                area(other.bounds) * static_cast<double>(other.count);
            if (cost < bestCost) {
                bestCost = cost;
                bestAxis = axis;
                bestBin = i;
            }
        }
    }
    std::size_t middle = items.size() / 2;
    if (bestAxis != -1) {
        const auto split = std::stable_partition(items.begin(), items.end(), [&](const Item& item) {
            return binFor(item, bestAxis) <= bestBin;
        });
        middle = static_cast<std::size_t>(split - items.begin());
    } else {
        // Coincident centroids have no spatial split; ID order gives bounded depth.
        std::sort(items.begin(), items.end(),
                  [](const Item& a, const Item& b) { return a.id < b.id; });
    }
    const int left = buildRange(items.first(middle), index);
    const int right = buildRange(items.subspan(middle), index);
    nodes_[index].left = left;
    nodes_[index].right = right;
    refit(index);
    return index;
}

void Bvh::build(std::span<const Item> items) {
    std::vector<Item> sorted(items.begin(), items.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const Item& a, const Item& b) { return a.id < b.id; });
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        validate(sorted[i].bounds);
        if (i != 0 && sorted[i - 1].id == sorted[i].id) {
            throw std::invalid_argument("Duplicate BVH ID");
        }
    }
    Bvh result;
    result.nodes_.reserve(sorted.size() * 2);
    result.locations_.reserve(sorted.size());
    if (!sorted.empty()) {
        result.root_ = result.buildRange(sorted, -1);
        result.baseline_ = result.normalizedCost();
    }
    *this = std::move(result);
}

void Bvh::replace(int index, int replacement) {
    const int parent = nodes_[index].parent;
    if (parent == -1) {
        root_ = replacement;
    } else if (nodes_[parent].left == index) {
        nodes_[parent].left = replacement;
    } else {
        nodes_[parent].right = replacement;
    }
    if (replacement != -1) {
        nodes_[replacement].parent = parent;
    }
}

int Bvh::rotateLeft(int index) {
    const int right = nodes_[index].right;
    const int middle = nodes_[right].left;
    replace(index, right);
    nodes_[right].left = index;
    nodes_[index].parent = right;
    nodes_[index].right = middle;
    nodes_[middle].parent = index;
    refit(index);
    refit(right);
    return right;
}

int Bvh::rotateRight(int index) {
    const int left = nodes_[index].left;
    const int middle = nodes_[left].right;
    replace(index, left);
    nodes_[left].right = index;
    nodes_[index].parent = left;
    nodes_[index].left = middle;
    nodes_[middle].parent = index;
    refit(index);
    refit(left);
    return left;
}

void Bvh::repair(int index) {
    while (index != -1) {
        refit(index);
        if (!nodes_[index].leaf()) {
            const int left = nodes_[index].left;
            const int right = nodes_[index].right;
            const int balance = nodes_[left].height - nodes_[right].height;
            if (balance > 1) {
                if (nodes_[nodes_[left].left].height < nodes_[nodes_[left].right].height) {
                    rotateLeft(left);
                }
                index = rotateRight(index);
            } else if (balance < -1) {
                if (nodes_[nodes_[right].right].height < nodes_[nodes_[right].left].height) {
                    rotateRight(right);
                }
                index = rotateLeft(index);
            }
        }
        index = nodes_[index].parent;
    }
}

void Bvh::insert(std::uint32_t id, const AABB& box) {
    validate(box);
    if (contains(id)) {
        throw std::invalid_argument("Duplicate BVH ID");
    }
    int leaf = root_;
    while (leaf != -1 && !nodes_[leaf].leaf()) {
        const int left = nodes_[leaf].left;
        const int right = nodes_[leaf].right;
        const double leftCost =
            area(combined(nodes_[left].bounds, box)) - area(nodes_[left].bounds);
        const double rightCost =
            area(combined(nodes_[right].bounds, box)) - area(nodes_[right].bounds);
        leaf = leftCost < rightCost ||
                       (leftCost == rightCost && nodes_[left].minId < nodes_[right].minId)
                   ? left
                   : right;
    }
    if (leaf != -1 && nodes_[leaf].count < 4) {
        nodes_[leaf].items[nodes_[leaf].count++] = {id, box};
        locations_.emplace(id, leaf);
        repair(leaf);
    } else {
        const int added = allocate();
        nodes_[added].count = 1;
        nodes_[added].items[0] = {id, box};
        locations_.emplace(id, added);
        refit(added);
        if (leaf == -1) {
            root_ = added;
        } else {
            const int parent = allocate();
            replace(leaf, parent);
            nodes_[parent].left = leaf;
            nodes_[parent].right = added;
            nodes_[leaf].parent = parent;
            nodes_[added].parent = parent;
            repair(parent);
        }
    }
    if (baseline_ == 0.0) {
        baseline_ = normalizedCost();
    }
}

void Bvh::remove(std::uint32_t id) {
    const auto found = locations_.find(id);
    if (found == locations_.end()) {
        return;
    }
    const int index = found->second;
    locations_.erase(found);
    auto& node = nodes_[index];
    for (int i = 0; i < node.count; ++i) {
        if (node.items[i].id == id) {
            node.items[i] = node.items[--node.count];
            break;
        }
    }
    if (node.count != 0) {
        repair(index);
    } else {
        const int parent = node.parent;
        if (parent == -1) {
            root_ = -1;
            baseline_ = 0.0;
        } else {
            const int sibling =
                nodes_[parent].left == index ? nodes_[parent].right : nodes_[parent].left;
            replace(parent, sibling);
            const int ancestor = nodes_[sibling].parent;
            release(parent);
            repair(ancestor);
        }
        release(index);
    }
}

void Bvh::update(std::uint32_t id, const AABB& box) {
    validate(box);
    const auto found = locations_.find(id);
    if (found == locations_.end()) {
        throw std::out_of_range("Missing BVH ID");
    }
    const int index = found->second;
    if (!nodes_[index].bounds.contains(box)) {
        remove(id);
        insert(id, box);
        return;
    }
    auto& node = nodes_[index];
    for (int i = 0; i < node.count; ++i) {
        if (node.items[i].id == id) {
            node.items[i].bounds = box;
            break;
        }
    }
    repair(index);
}

double Bvh::normalizedCost() const {
    if (root_ == -1 || area(nodes_[root_].bounds) == 0.0) {
        return 0.0;
    }
    return nodes_[root_].cost / area(nodes_[root_].bounds);
}

double Bvh::quality() const {
    return baseline_ == 0.0 || normalizedCost() == 0.0 ? 1.0 : normalizedCost() / baseline_;
}

void Bvh::rebuild() {
    std::vector<Item> items;
    items.reserve(size());
    for (const auto& node : nodes_) {
        if (node.leaf()) {
            items.insert(items.end(), node.items.begin(), node.items.begin() + node.count);
        }
    }
    build(items);
}

} // namespace apex::geometry
