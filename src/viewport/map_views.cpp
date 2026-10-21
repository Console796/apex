#include "viewport/map_views.hpp"

#include "core/strings.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string>

namespace apex::viewport {

namespace {

constexpr double kEps = 1e-6;

bool close(double a, double b) {
    return std::abs(a - b) <= kEps * std::max({1.0, std::abs(a), std::abs(b)});
}
bool close(DVec3 a, DVec3 b) { return close(a.x, b.x) && close(a.y, b.y) && close(a.z, b.z); }

std::string real(double v) {
    char buffer[64]{};
    const auto [end, ec] = std::to_chars(buffer, buffer + sizeof buffer, v, std::chars_format::general, 6);
    return ec == std::errc{} ? std::string(buffer, end) : "0";
}

std::string vecText(DVec3 v, char open, char closeCh) {
    return std::string(1, open) + real(v.x) + " " + real(v.y) + " " + real(v.z) +
           std::string(1, closeCh);
}

// "v0".."v3" -> 0..3.
std::optional<int> viewIndex(std::string_view name) {
    if (name.size() != 2 || (name[0] != 'v' && name[0] != 'V') || name[1] < '0' || name[1] > '3') {
        return std::nullopt;
    }
    return name[1] - '0';
}

const vmf::Value* findKey(const vmf::Block& b, std::string_view key) {
    for (const auto& v : b.keyValues()) {
        if (apex::ciEquals(v.key(), key)) return &v;
    }
    return nullptr;
}

std::optional<DVec3> readVec(const vmf::Value* v) {
    if (!v) return std::nullopt;
    const auto parsed = v->asVec3();
    if (!parsed) return std::nullopt;
    return DVec3{(*parsed)[0], (*parsed)[1], (*parsed)[2]};
}

std::optional<MapView> readView(const vmf::View& view) {
    MapView out;
    if (const auto* d3 = findKey(view, "3d")) {
        const auto i = d3->asInt();
        if (!i) return std::nullopt;
        out.is3d = *i != 0;
    }
    const auto pos = readVec(findKey(view, "position"));
    if (!pos) return std::nullopt;
    out.position = *pos;
    if (out.is3d) {
        const auto* ang = findKey(view, "angle");
        if (!ang) ang = findKey(view, "angles");
        const auto a = readVec(ang);
        if (!a) return std::nullopt;
        out.angles = *a;
    } else if (const auto* z = findKey(view, "zoom")) {
        const auto zoom = z->asDouble();
        if (!zoom || !(*zoom > 0.0)) return std::nullopt;
        out.zoom = *zoom;
    }
    return out;
}

// Rebuilds the typed key/value list of a block after its node's children changed.
void rebindValues(vmf::Block& block) {
    block.values.clear();
    for (auto& child : block.node->children()) {
        if (child.kind() != kv1::NodeKind::Block) block.values.push_back(vmf::Value(&child));
    }
}

// Rebuilds the typed views list after structural edits to `viewsettings` / `views`.
void rebindViewSettings(vmf::ViewSettings& vs) {
    rebindValues(vs);
    vs.views.reset();
    vs.unknownBlocks.clear();
    for (auto& child : vs.node->children()) {
        if (child.kind() != kv1::NodeKind::Block) continue;
        if (apex::ciEquals(child.key(), "views") && !vs.views) {
            vmf::Views views;
            views.node = &child;
            rebindValues(views);
            for (auto& v : child.children()) {
                if (v.kind() != kv1::NodeKind::Block) continue;
                vmf::View view;
                view.node = &v;
                rebindValues(view);
                if (v.key().size() >= 2 && (v.key().front() == 'v' || v.key().front() == 'V')) {
                    views.children.push_back(std::move(view));
                } else {
                    vmf::OpaqueBlock o;
                    o.node = &v;
                    rebindValues(o);
                    views.unknownBlocks.push_back(std::move(o));
                }
            }
            vs.views = std::move(views);
        } else {
            vmf::OpaqueBlock o;
            o.node = &child;
            rebindValues(o);
            vs.unknownBlocks.push_back(std::move(o));
        }
    }
}

Result<void> setKey(kv1::Node& node, std::string_view key, std::string text) {
    if (auto* existing = node.find(key); existing && existing->kind() == kv1::NodeKind::Value) {
        return existing->setValue(std::move(text));
    }
    return node.append(kv1::Node::value(std::string(key), std::move(text)));
}

} // namespace

MapViews readMapViews(const vmf::VmfFile& file) {
    MapViews out;
    if (!file.viewSettings || !file.viewSettings->views) return out;
    for (const auto& view : file.viewSettings->views->children) {
        const auto idx = viewIndex(view.name());
        if (idx) out[static_cast<std::size_t>(*idx)] = readView(view);
    }
    return out;
}

Result<void> writeMapViews(vmf::VmfFile& file, const MapViews& views) {
    const bool any = std::any_of(views.begin(), views.end(), [](const auto& v) { return v.has_value(); });
    if (!any) return {};
    if (!file.viewSettings) {
        return std::unexpected(Error{ErrorCode::Unsupported, "VMF has no viewsettings block"});
    }
    auto& vs = *file.viewSettings;
    const MapViews existing = readMapViews(file);

    // Pass 1: structural edits (append `views` and missing vN blocks), then rebind pointers.
    kv1::Node* viewsNode = vs.views ? vs.views->node : nullptr;
    if (!viewsNode) {
        if (auto r = vs.node->append(kv1::Node::block("views")); !r) return r;
        rebindViewSettings(vs);
        viewsNode = vs.views->node;
    }
    bool structural = false;
    for (std::size_t i = 0; i < views.size(); ++i) {
        if (!views[i]) continue;
        const std::string name = "v" + std::to_string(i);
        bool found = false;
        for (const auto& child : viewsNode->children()) {
            if (child.kind() == kv1::NodeKind::Block && apex::ciEquals(child.key(), name)) found = true;
        }
        if (!found) {
            if (auto r = viewsNode->append(kv1::Node::block(name)); !r) return r;
            structural = true;
        }
    }
    if (structural) rebindViewSettings(vs);

    // Pass 2: value edits in place; only changed values are touched.
    for (std::size_t i = 0; i < views.size(); ++i) {
        if (!views[i]) continue;
        const MapView& want = *views[i];
        const auto& have = existing[i];
        kv1::Node* node = nullptr;
        for (auto& child : viewsNode->children()) {
            if (child.kind() == kv1::NodeKind::Block &&
                apex::ciEquals(child.key(), "v" + std::to_string(i))) {
                node = &child;
                break;
            }
        }
        if (!node) return std::unexpected(Error{ErrorCode::Internal, "view block vanished"});
        if (!have || have->is3d != want.is3d) {
            if (auto r = setKey(*node, "3d", want.is3d ? "1" : "0"); !r) return r;
        }
        if (!have || !close(have->position, want.position)) {
            if (auto r = setKey(*node, "position", vecText(want.position, '(', ')')); !r) return r;
        }
        if (want.is3d) {
            if (!have || !have->is3d || !close(have->angles, want.angles)) {
                const char* key = node->find("angles") && !node->find("angle") ? "angles" : "angle";
                if (auto r = setKey(*node, key, vecText(want.angles, '[', ']')); !r) return r;
            }
        } else if (!have || have->is3d || !close(have->zoom, want.zoom)) {
            if (auto r = setKey(*node, "zoom", real(want.zoom)); !r) return r;
        }
    }
    return {};
}

double orthoHeightFromZoom(double zoom, double px) { return px / std::max(zoom, 1e-9); }
double zoomFromOrthoHeight(double height, double px) { return px / std::max(height, 1e-9); }

void applyMapViews(ViewportLayout& layout, const MapViews& views, double px) {
    const auto ids = layout.paneIds();
    for (std::size_t i = 0; i < views.size() && i < ids.size(); ++i) {
        if (!views[i]) continue;
        auto* vp = layout.viewport(ids[i]);
        const MapView& mv = *views[i];
        if (mv.is3d) {
            if (vp->camera.isOrtho()) applyPreset(*vp, Preset::Camera);
            vp->camera.position = mv.position;
            vp->camera.angles = mv.angles;
        } else {
            if (!vp->camera.isOrtho() || !vp->camera.axisLocked()) {
                applyPreset(*vp, i == 1 ? Preset::Top : i == 2 ? Preset::Front : Preset::Side);
            }
            auto& cam = vp->camera;
            const auto basis = cam.basis();
            const DVec3 depth = basis.forward * glm::dot(basis.forward, cam.position);
            cam.position = mv.position - basis.forward * glm::dot(basis.forward, mv.position) + depth;
            cam.orthoHeight = orthoHeightFromZoom(mv.zoom, px);
        }
    }
}

MapViews captureMapViews(const ViewportLayout& layout, double px) {
    MapViews out;
    const auto ids = layout.paneIds();
    for (std::size_t i = 0; i < out.size() && i < ids.size(); ++i) {
        const auto& cam = layout.viewport(ids[i])->camera;
        MapView mv;
        mv.is3d = !cam.isOrtho();
        mv.position = cam.position;
        if (mv.is3d) {
            mv.angles = cam.angles;
        } else {
            mv.zoom = zoomFromOrthoHeight(cam.orthoHeight, px);
        }
        out[i] = mv;
    }
    return out;
}

} // namespace apex::viewport
