#include "viewport/layout.hpp"

#include <algorithm>
#include <set>

namespace apex::viewport {

struct ViewportLayout::Node {
    NodeId id = 0;
    bool leaf = true;
    Viewport viewport;                       // Leaf.
    Orientation orientation = Orientation::Horizontal; // Split.
    double ratio = 0.5;
    std::unique_ptr<Node> first, second;

    std::unique_ptr<Node> clone() const {
        auto copy = std::make_unique<Node>();
        copy->id = id;
        copy->leaf = leaf;
        copy->viewport = viewport;
        copy->orientation = orientation;
        copy->ratio = ratio;
        if (first) copy->first = first->clone();
        if (second) copy->second = second->clone();
        return copy;
    }
};

namespace {

constexpr double kMinRatio = 0.05, kMaxRatio = 0.95;
constexpr int kMaxDepth = 64;

Error bad(std::string message) {
    return Error{ErrorCode::InvalidArgument, std::move(message)};
}

template <typename N, typename F> void walk(N* node, F&& visit) {
    if (!node) return;
    visit(*node);
    walk(node->first.get(), visit);
    walk(node->second.get(), visit);
}

template <typename N> N* findNode(N* node, NodeId id) {
    if (!node) return nullptr;
    if (node->id == id) return node;
    if (auto* hit = findNode(node->first.get(), id)) return hit;
    return findNode(node->second.get(), id);
}

Rect firstRect(const Rect& r, Orientation o, double ratio) {
    return o == Orientation::Horizontal ? Rect{r.x, r.y, r.w * ratio, r.h}
                                        : Rect{r.x, r.y, r.w, r.h * ratio};
}
Rect secondRect(const Rect& r, Orientation o, double ratio) {
    return o == Orientation::Horizontal ? Rect{r.x + r.w * ratio, r.y, r.w * (1 - ratio), r.h}
                                        : Rect{r.x, r.y + r.h * ratio, r.w, r.h * (1 - ratio)};
}

json::Value num(double v) { json::Value r; r.data = v; return r; }
json::Value str(std::string v) { json::Value r; r.data = std::move(v); return r; }

} // namespace

ViewportLayout::ViewportLayout() : ViewportLayout(makePreset(Preset::Camera)) {}

ViewportLayout::ViewportLayout(Viewport single) {
    root_ = std::make_unique<Node>();
    root_->id = nextId_++;
    root_->viewport = std::move(single);
    active_ = root_->id;
}

ViewportLayout::ViewportLayout(const ViewportLayout& o)
    : root_(o.root_->clone()), nextId_(o.nextId_), active_(o.active_), maximized_(o.maximized_) {}

ViewportLayout& ViewportLayout::operator=(const ViewportLayout& o) {
    if (this != &o) {
        root_ = o.root_->clone();
        nextId_ = o.nextId_;
        active_ = o.active_;
        maximized_ = o.maximized_;
    }
    return *this;
}

ViewportLayout::ViewportLayout(ViewportLayout&&) noexcept = default;
ViewportLayout& ViewportLayout::operator=(ViewportLayout&&) noexcept = default;
ViewportLayout::~ViewportLayout() = default;

ViewportLayout ViewportLayout::quad() {
    ViewportLayout layout(makePreset(Preset::Camera));
    const NodeId camera = layout.paneIds().front();
    const NodeId bottomLeft = *layout.split(camera, Orientation::Vertical);
    const NodeId topRight = *layout.split(camera, Orientation::Horizontal);
    const NodeId bottomRight = *layout.split(bottomLeft, Orientation::Horizontal);
    applyPreset(*layout.viewport(topRight), Preset::Top);
    applyPreset(*layout.viewport(bottomLeft), Preset::Front);
    applyPreset(*layout.viewport(bottomRight), Preset::Side);
    layout.active_ = camera;
    return layout;
}

LayoutNode ViewportLayout::tree() const {
    const auto build = [](const auto& self, const Node& n) -> LayoutNode {
        LayoutNode out;
        out.id = n.id;
        out.leaf = n.leaf;
        out.orientation = n.orientation;
        out.ratio = n.ratio;
        if (!n.leaf) {
            out.children.push_back(self(self, *n.first));
            out.children.push_back(self(self, *n.second));
        }
        return out;
    };
    return build(build, *root_);
}

std::vector<NodeId> ViewportLayout::paneIds() const {
    std::vector<NodeId> ids;
    walk(root_.get(), [&](const auto& n) { if (n.leaf) ids.push_back(n.id); });
    return ids;
}

std::size_t ViewportLayout::paneCount() const {
    return paneIds().size();
}

bool ViewportLayout::hasPane(NodeId id) const {
    const auto* n = findNode(root_.get(), id);
    return n && n->leaf;
}

Viewport* ViewportLayout::viewport(NodeId pane) {
    auto* n = findNode(root_.get(), pane);
    return n && n->leaf ? &n->viewport : nullptr;
}

const Viewport* ViewportLayout::viewport(NodeId pane) const {
    const auto* n = findNode(root_.get(), pane);
    return n && n->leaf ? &n->viewport : nullptr;
}

Result<NodeId> ViewportLayout::split(NodeId pane, Orientation orientation, double ratio) {
    auto* leaf = findNode(root_.get(), pane);
    if (!leaf || !leaf->leaf) return std::unexpected(bad("split: no such pane"));
    auto first = std::make_unique<Node>();
    first->id = pane; // The original pane keeps its id.
    first->viewport = leaf->viewport;
    auto second = std::make_unique<Node>();
    second->id = nextId_++;
    second->viewport = leaf->viewport;
    const NodeId newPane = second->id;
    leaf->id = nextId_++; // The split node takes a fresh id.
    leaf->leaf = false;
    leaf->viewport = {};
    leaf->orientation = orientation;
    leaf->ratio = std::clamp(ratio, kMinRatio, kMaxRatio);
    leaf->first = std::move(first);
    leaf->second = std::move(second);
    maximized_.reset();
    return newPane;
}

Result<void> ViewportLayout::close(NodeId pane) {
    if (!hasPane(pane)) return std::unexpected(bad("close: no such pane"));
    if (root_->leaf) return std::unexpected(bad("close: cannot close the last pane"));
    // Find the parent of `pane`.
    Node* parent = nullptr;
    walk(root_.get(), [&](auto& n) {
        if (!n.leaf && (n.first->id == pane || n.second->id == pane)) parent = &n;
    });
    auto& removed = parent->first->id == pane ? parent->first : parent->second;
    auto& sibling = parent->first->id == pane ? parent->second : parent->first;
    auto survivor = std::move(sibling);
    (void)removed;
    *parent = std::move(*survivor); // The sibling subtree takes the parent's place.
    if (maximized_ == pane) maximized_.reset();
    if (active_ == pane) active_ = paneIds().front();
    return {};
}

Result<void> ViewportLayout::setRatio(NodeId id, double ratio) {
    auto* n = findNode(root_.get(), id);
    if (!n || n->leaf) return std::unexpected(bad("setRatio: no such split"));
    n->ratio = std::clamp(ratio, kMinRatio, kMaxRatio);
    return {};
}

Result<void> ViewportLayout::maximize(NodeId pane) {
    if (!hasPane(pane)) return std::unexpected(bad("maximize: no such pane"));
    maximized_ = pane;
    active_ = pane;
    return {};
}

void ViewportLayout::toggleMaximize(NodeId pane) {
    if (maximized_ == pane) restore();
    else (void)maximize(pane);
}

Result<void> ViewportLayout::setActive(NodeId pane) {
    if (!hasPane(pane)) return std::unexpected(bad("setActive: no such pane"));
    active_ = pane;
    return {};
}

std::vector<PaneRect> ViewportLayout::layout(Rect area) const {
    std::vector<PaneRect> out;
    if (maximized_) {
        out.push_back({*maximized_, area});
        return out;
    }
    const auto recurse = [&](const auto& self, const Node& n, Rect r) -> void {
        if (n.leaf) {
            out.push_back({n.id, r});
            return;
        }
        self(self, *n.first, firstRect(r, n.orientation, n.ratio));
        self(self, *n.second, secondRect(r, n.orientation, n.ratio));
    };
    recurse(recurse, *root_, area);
    return out;
}

std::vector<SplitInfo> ViewportLayout::splits(Rect area) const {
    std::vector<SplitInfo> out;
    if (maximized_) return out;
    const auto recurse = [&](const auto& self, const Node& n, Rect r) -> void {
        if (n.leaf) return;
        out.push_back({n.id, n.orientation, n.ratio, r});
        self(self, *n.first, firstRect(r, n.orientation, n.ratio));
        self(self, *n.second, secondRect(r, n.orientation, n.ratio));
    };
    recurse(recurse, *root_, area);
    return out;
}

json::Value ViewportLayout::toJson() const {
    const auto node = [&](const auto& self, const Node& n) -> json::Value {
        json::Value::Object o;
        o["id"] = num(n.id);
        if (n.leaf) {
            o["type"] = str("pane");
            o["viewport"] = viewport::toJson(n.viewport);
        } else {
            o["type"] = str("split");
            o["orientation"] = str(n.orientation == Orientation::Horizontal ? "horizontal" : "vertical");
            o["ratio"] = num(n.ratio);
            o["first"] = self(self, *n.first);
            o["second"] = self(self, *n.second);
        }
        json::Value v;
        v.data = std::move(o);
        return v;
    };
    json::Value::Object o;
    o["version"] = num(1);
    o["active"] = num(active_);
    if (maximized_) o["maximized"] = num(*maximized_);
    o["root"] = node(node, *root_);
    json::Value v;
    v.data = std::move(o);
    return v;
}

Result<ViewportLayout> ViewportLayout::fromJson(const json::Value& value) {
    const auto fail = [](std::string m) {
        return std::unexpected(Error{ErrorCode::ParseError, "layout: " + std::move(m)});
    };
    const auto* top = value.get<json::Value::Object>();
    if (!top) return fail("expected an object");
    const auto rootIt = top->find("root");
    if (rootIt == top->end()) return fail("missing 'root'");

    std::set<NodeId> seen;
    NodeId maxId = 0;
    const auto integer = [](const json::Value::Object& o, const char* key) -> std::optional<NodeId> {
        const auto it = o.find(key);
        if (it == o.end()) return std::nullopt;
        const auto* d = it->second.get<double>();
        if (!d || *d < 1 || *d > 1e9 || *d != std::floor(*d)) return std::nullopt;
        return static_cast<NodeId>(*d);
    };
    std::optional<std::string> error;
    const auto build = [&](const auto& self, const json::Value& v, int depth) -> std::unique_ptr<Node> {
        const auto* o = v.get<json::Value::Object>();
        if (!o || depth > kMaxDepth) { error = "bad node or nesting too deep"; return nullptr; }
        auto n = std::make_unique<Node>();
        const auto id = integer(*o, "id");
        if (!id || !seen.insert(*id).second) { error = "missing or duplicate id"; return nullptr; }
        n->id = *id;
        maxId = std::max(maxId, *id);
        const auto typeIt = o->find("type");
        const auto* type = typeIt == o->end() ? nullptr : typeIt->second.get<std::string>();
        if (type && *type == "pane") {
            const auto vp = o->find("viewport");
            if (vp == o->end()) { error = "pane without viewport"; return nullptr; }
            auto parsed = viewportFromJson(vp->second);
            if (!parsed) { error = parsed.error().what(); return nullptr; }
            n->viewport = std::move(*parsed);
            return n;
        }
        if (type && *type == "split") {
            n->leaf = false;
            const auto ori = o->find("orientation");
            const auto* os = ori == o->end() ? nullptr : ori->second.get<std::string>();
            if (!os || (*os != "horizontal" && *os != "vertical")) { error = "bad orientation"; return nullptr; }
            n->orientation = *os == "horizontal" ? Orientation::Horizontal : Orientation::Vertical;
            const auto rat = o->find("ratio");
            const auto* rd = rat == o->end() ? nullptr : rat->second.get<double>();
            if (!rd || !(*rd >= kMinRatio && *rd <= kMaxRatio)) { error = "bad ratio"; return nullptr; }
            n->ratio = *rd;
            const auto a = o->find("first"), b = o->find("second");
            if (a == o->end() || b == o->end()) { error = "split missing children"; return nullptr; }
            n->first = self(self, a->second, depth + 1);
            if (!n->first) return nullptr;
            n->second = self(self, b->second, depth + 1);
            if (!n->second) return nullptr;
            return n;
        }
        error = "unknown node type";
        return nullptr;
    };

    ViewportLayout layout;
    layout.root_ = build(build, rootIt->second, 0);
    if (!layout.root_) return fail(error.value_or("invalid tree"));
    layout.nextId_ = maxId + 1;
    const auto ids = layout.paneIds();
    layout.active_ = ids.front();
    if (const auto a = integer(*top, "active"); a && layout.hasPane(*a)) layout.active_ = *a;
    if (const auto m = integer(*top, "maximized"); m && layout.hasPane(*m)) layout.maximized_ = *m;
    return layout;
}

} // namespace apex::viewport
