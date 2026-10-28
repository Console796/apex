#include "document/cordon.hpp"

#include "core/strings.hpp"
#include "geometry/solid_block.hpp"
#include "geometry/solid_geometry.hpp"

#include <charconv>
#include <cstdio>

namespace apex::document {
namespace {

const kv1::Node* childBlock(const Document& document, const Node& parent, std::string_view key) {
    for (const auto child : parent.children) {
        const auto handle = document.handle(child);
        if (!handle) continue;
        const auto* node = document.get(*handle);
        if (node && node->block && ciEquals(node->block->key(), key)) return node->block.get();
    }
    return nullptr;
}

std::optional<NodeHandle> childHandle(const Document& document, const Node& parent, std::string_view key) {
    for (const auto child : parent.children) {
        const auto handle = document.handle(child);
        if (!handle) continue;
        const auto* node = document.get(*handle);
        if (node && node->block && ciEquals(node->block->key(), key)) return handle;
    }
    return std::nullopt;
}

std::optional<DVec3> parsePoint(const kv1::Node* value) {
    if (!value) return std::nullopt;
    const auto text = value->decodedValue();
    if (!text) return std::nullopt;
    std::string body = *text;
    for (auto& c : body) if (c == '(' || c == ')') c = ' ';
    DVec3 point{};
    const char* cursor = body.c_str();
    for (int i = 0; i < 3; ++i) {
        char* end = nullptr;
        point[i] = std::strtod(cursor, &end);
        if (end == cursor) return std::nullopt;
        cursor = end;
    }
    return point;
}

std::string formatNumber(double value) {
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
    return std::string(buffer, result.ptr);
}

std::string formatPoint(DVec3 point) {
    return "(" + formatNumber(point.x) + " " + formatNumber(point.y) + " " + formatNumber(point.z) + ")";
}

bool flag(const kv1::Node* value, bool fallback) {
    if (!value) return fallback;
    const auto number = value->asInt();
    return number ? *number != 0 : fallback;
}

std::optional<NodeHandle> containerHandle(const Document& document) {
    for (const auto root : document.roots()) {
        const auto handle = document.handle(root);
        const auto* node = handle ? document.get(*handle) : nullptr;
        if (node && node->block && ciEquals(node->block->key(), "cordons")) return handle;
    }
    return std::nullopt;
}

kv1::Node boxBlock(const AABB& box) {
    auto block = kv1::Node::block("box");
    block.append(kv1::Node::value("mins", formatPoint(box.min))).value();
    block.append(kv1::Node::value("maxs", formatPoint(box.max))).value();
    return block;
}

NodeId idOf(const Document& document, NodeHandle handle) { return document.get(handle)->id; }

} // namespace

std::vector<AABB> CordonSet::compileBoxes() const {
    std::vector<AABB> result;
    if (!enabled) return result;
    for (const auto& cordon : boxes) {
        if (cordon.active && !cordon.box.empty()) result.push_back(cordon.box);
    }
    return result;
}

CordonSet readCordons(const Document& document) {
    CordonSet set;
    const auto container = containerHandle(document);
    if (!container) return set;
    const auto* parent = document.get(*container);
    set.container = parent->id;
    set.enabled = flag(parent->block->find("active"), false);
    for (const auto child : parent->children) {
        const auto handle = document.handle(child);
        const auto* node = handle ? document.get(*handle) : nullptr;
        if (!node || !node->block || !ciEquals(node->block->key(), "cordon")) continue;
        CordonBox cordon;
        cordon.id = node->id;
        if (const auto* name = node->block->find("name")) cordon.name = name->decodedValue().value_or("");
        cordon.active = flag(node->block->find("active"), true);
        if (const auto* box = childBlock(document, *node, "box")) {
            const auto mins = parsePoint(box->find("mins"));
            const auto maxs = parsePoint(box->find("maxs"));
            if (mins && maxs) cordon.box = AABB{glm::min(*mins, *maxs), glm::max(*mins, *maxs)};
        }
        set.boxes.push_back(std::move(cordon));
    }
    return set;
}

NodeId addCordon(Document& document, std::string name, const AABB& box) {
    auto tx = document.begin("Add cordon");
    auto container = containerHandle(document);
    if (!container) {
        auto block = kv1::Node::block("cordons");
        block.append(kv1::Node::value("active", "1")).value();
        container = tx.create(std::move(block));
    } else {
        tx.setKeyValue(*container, "active", "1");
    }
    auto cordon = kv1::Node::block("cordon");
    cordon.append(kv1::Node::value("name", std::move(name))).value();
    cordon.append(kv1::Node::value("active", "1")).value();
    cordon.append(boxBlock(box)).value();
    const auto created = tx.create(std::move(cordon), *container);
    tx.commit();
    return idOf(document, created);
}

void setCordonBox(Document& document, NodeId cordon, const AABB& box, std::string coalesceKey) {
    const auto handle = document.handle(cordon);
    if (!handle) return;
    const auto boxHandle = childHandle(document, *document.get(*handle), "box");
    auto tx = document.begin("Edit cordon");
    if (boxHandle) {
        tx.setKeyValue(*boxHandle, "mins", formatPoint(box.min));
        tx.setKeyValue(*boxHandle, "maxs", formatPoint(box.max));
    } else {
        tx.create(boxBlock(box), *handle);
    }
    const bool coalesce = !coalesceKey.empty();
    tx.commit(coalesce, std::move(coalesceKey));
}

void setCordonActive(Document& document, NodeId cordon, bool active) {
    const auto handle = document.handle(cordon);
    if (!handle) return;
    auto tx = document.begin(active ? "Activate cordon" : "Deactivate cordon");
    tx.setKeyValue(*handle, "active", active ? "1" : "0");
    if (active) {
        if (const auto container = containerHandle(document)) tx.setKeyValue(*container, "active", "1");
    }
    tx.commit();
}

void removeCordon(Document& document, NodeId cordon) {
    const auto handle = document.handle(cordon);
    if (!handle) return;
    auto tx = document.begin("Remove cordon");
    tx.destroy(*handle);
    tx.commit();
}

namespace {

std::optional<AABB> solidBounds(const kv1::Node& solid) {
    const auto geometry = geometry::solidGeometry(solid);
    if (!geometry) return std::nullopt;
    const auto bounds = geometry->poly.bounds();
    if (bounds.empty()) return std::nullopt;
    return bounds;
}

bool touches(const std::vector<AABB>& boxes, const AABB& bounds) {
    for (const auto& box : boxes) if (box.intersects(bounds)) return true;
    return false;
}

// Unparseable solids and entities with neither a position nor brushes stay in, so the compiler reports them.
bool entityTouches(const kv1::Node& entity, const std::vector<AABB>& boxes) {
    bool known = false;
    if (const auto origin = parsePoint(entity.find("origin"))) {
        known = true;
        if (touches(boxes, AABB{*origin, *origin})) return true;
    }
    for (const auto& child : entity.children()) {
        if (child.kind() != kv1::NodeKind::Block || !ciEquals(child.key(), "solid")) continue;
        const auto bounds = solidBounds(child);
        if (!bounds) return true;
        known = true;
        if (touches(boxes, *bounds)) return true;
    }
    return !known;
}

void collectMaxId(const kv1::Node& block, long long& maximum) {
    for (const auto& child : block.children()) {
        if (child.kind() == kv1::NodeKind::Block) {
            collectMaxId(child, maximum);
        } else if (ciEquals(child.key(), "id")) {
            if (const auto number = child.asInt()) maximum = std::max<long long>(maximum, *number);
        }
    }
}

void assignIds(kv1::Node& block, long long& next) {
    for (auto& child : block.children()) {
        if (child.kind() == kv1::NodeKind::Block) assignIds(child, next);
        else if (ciEquals(child.key(), "id")) child.setValue(std::to_string(next++)).value();
    }
}

} // namespace

Result<std::string> cordonFilteredText(const std::string& text, const std::vector<AABB>& boxes,
                                       const CordonWalls& walls) {
    if (boxes.empty()) return text;
    auto parsed = kv1::parse(text);
    if (!parsed) return std::unexpected(parsed.error());
    auto& root = parsed->root();
    AABB all;
    for (const auto& box : boxes) all.expand(box);
    long long maximumId = 0;
    collectMaxId(root, maximumId);
    for (std::size_t i = root.children().size(); i-- > 0;) {
        const auto& block = root.children()[i];
        if (block.kind() == kv1::NodeKind::Block && ciEquals(block.key(), "entity") &&
            !entityTouches(block, boxes))
            root.remove(i).value();
    }
    kv1::Node* world = nullptr;
    for (auto& block : root.children()) {
        if (block.kind() == kv1::NodeKind::Block && ciEquals(block.key(), "world")) { world = &block; break; }
    }
    if (!world) return std::unexpected(Error{ErrorCode::ParseError, "map has no world block"});
    for (std::size_t i = world->children().size(); i-- > 0;) {
        const auto& child = world->children()[i];
        if (child.kind() != kv1::NodeKind::Block || !ciEquals(child.key(), "solid")) continue;
        const auto bounds = solidBounds(child);
        if (bounds && !touches(boxes, *bounds)) world->remove(i).value();
    }
    const double t = walls.thickness;
    const DVec3 lo = all.min, hi = all.max;
    const AABB slabs[6] = {
        {{lo.x - t, lo.y - t, lo.z - t}, {lo.x, hi.y + t, hi.z + t}},
        {{hi.x, lo.y - t, lo.z - t}, {hi.x + t, hi.y + t, hi.z + t}},
        {{lo.x, lo.y - t, lo.z - t}, {hi.x, lo.y, hi.z + t}},
        {{lo.x, hi.y, lo.z - t}, {hi.x, hi.y + t, hi.z + t}},
        {{lo.x, lo.y, lo.z - t}, {hi.x, hi.y, lo.z}},
        {{lo.x, lo.y, hi.z}, {hi.x, hi.y, hi.z + t}},
    };
    long long nextId = maximumId + 1;
    for (const auto& slab : slabs) {
        std::vector<geometry::PlaneSide> sides;
        for (int axis = 0; axis < 3; ++axis) {
            DVec3 normal{0.0};
            normal[axis] = 1.0;
            geometry::PlaneSide side;
            side.plane = {normal, slab.max[axis]};
            sides.push_back(side);
            side.plane = {-normal, -slab.min[axis]};
            sides.push_back(side);
        }
        auto poly = geometry::ConvexPolyhedron::fromPlanes(sides);
        if (!poly) return std::unexpected(Error{ErrorCode::InvalidArgument, "cordon is too thin to wall in"});
        auto brush = geometry::solidBlock(*poly, geometry::NewSolidStyle{.material = walls.material});
        assignIds(brush, nextId);
        world->append(std::move(brush)).value();
    }
    return kv1::write(*parsed);
}

} // namespace apex::document
