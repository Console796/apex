#include <apex/plugin.hpp>
#include <cmath>
#include <map>
#include <vector>

namespace {
const apex_host* host;
class Polygon final : public apex::plugin::Tool {
public:
    Tool* createInstance() override { return new Polygon; }
    bool input(const apex_tool_event& event, const apex_tool_view& view) override {
        if (event.kind == APEX_INPUT_KEY_DOWN && apex::plugin::view(event.key) == "Escape") { cancel(); return true; }
        if (event.kind == APEX_INPUT_KEY_DOWN && apex::plugin::view(event.key) == "Enter")
            return perform("plugin.polygon.commit");
        if (event.kind != APEX_INPUT_PRESS || event.button != APEX_BUTTON_LEFT || std::abs(view.ray.direction.z) < 1e-9) return false;
        source_ = event.source;
        const auto t = -view.ray.origin.z / view.ray.direction.z;
        if (t < 0) return false;
        auto& points = points_[event.source];
        points.push_back({view.ray.origin.x + t * view.ray.direction.x,
                          view.ray.origin.y + t * view.ray.direction.y, 0});
        return true;
    }
    apex_status visual(uint64_t source, apex_tool_visual& out) override {
        segments_.clear(); planes_.clear(); ghosts_.clear();
        const auto it = points_.find(source);
        if (it == points_.end()) { out.segments = nullptr; out.segment_count = 0; out.ghosts = nullptr; out.ghost_count = 0; return APEX_OK; }
        const auto& points = it->second;
        for (std::size_t i = 0; i < points.size(); ++i) {
            if (points.size() < 2) break;
            const auto& a = points[i]; const auto& b = points[(i + 1) % points.size()];
            segments_.push_back(apex::plugin::segment(a, b, 2, {1, 0.65f, 0.15f, 1}));
        }
        if (points.size() >= 3) {
            double area = 0;
            for (std::size_t i = 0; i < points.size(); ++i) {
                const auto& a = points[i]; const auto& b = points[(i + 1) % points.size()];
                area += a.x * b.y - b.x * a.y;
            }
            if (std::abs(area) > 1e-9) {
                const double sign = area > 0 ? 1 : -1;
                for (std::size_t i = 0; i < points.size(); ++i) {
                    const auto& a = points[i]; const auto& b = points[(i + 1) % points.size()];
                    const auto length = std::hypot(b.x - a.x, b.y - a.y);
                    if (length < 1e-9) { planes_.clear(); break; }
                    apex_vec3 n{sign * (b.y - a.y) / length, sign * (a.x - b.x) / length, 0};
                    planes_.push_back({n, n.x * a.x + n.y * a.y});
                }
                if (!planes_.empty()) {
                    planes_.push_back({{0, 0, 1}, 16}); planes_.push_back({{0, 0, -1}, 0});
                    ghosts_.push_back(apex::plugin::ghost(planes_.data(), planes_.size(), 0.35f, "DEV/DEV_MEASUREGENERIC01"));
                }
            }
        }
        out.segments = segments_.data(); out.segment_count = segments_.size();
        out.ghosts = ghosts_.data(); out.ghost_count = ghosts_.size();
        return APEX_OK;
    }
    bool perform(std::string_view action) override {
        if (action != "plugin.polygon.commit") return false;
        apex_tool_visual result{};
        result.struct_size = sizeof(result);
        if (visual(source_, result) != APEX_OK || !result.ghost_count) return false;
        const auto* doc = static_cast<const apex_document_v1*>(host->get_interface(host->context, apex::plugin::string("apex.document/1")));
        const auto* edit = static_cast<const apex_edit_v1*>(host->get_interface(host->context, apex::plugin::string("apex.edit/1")));
        apex_node_id root = 0;
        if (!doc || !edit || doc->root_at(host->context, 0, &root) != APEX_OK) return false;
        struct Commit { Polygon* polygon; apex_node_id root; } commit{this, root};
        const auto status = edit->apply(host->context, apex::plugin::string("Create polygon"), {}, &commit,
            [](void* user, const apex_edit_ops_v1* ops, void* context) -> apex_status {
                auto& c = *static_cast<Commit*>(user);
                return ops->create_solid(context, c.root, c.polygon->planes_.data(), c.polygon->planes_.size(),
                                         apex::plugin::string("DEV/DEV_MEASUREGENERIC01"));
            });
        if (status != APEX_OK) return false;
        cancel();
        return true;
    }
    void cancel() override { points_.clear(); }
    void forget(uint64_t source) override { points_.erase(source); }
private:
    std::map<uint64_t, std::vector<apex_vec3>> points_;
    uint64_t source_ = 0;
    std::vector<apex_tool_segment> segments_;
    std::vector<apex_plane> planes_;
    std::vector<apex_tool_ghost> ghosts_;
};
Polygon polygon;
apex_status activate(void*) {
    const auto* api = apex::plugin::tools(*host);
    return api ? api->register_tool(host->context, apex::plugin::string("plugin.polygon.tool"),
        apex::plugin::string("Polygon tool"), apex::plugin::string("Preview a polygon prism"), &polygon, Polygon::callbacks()) : APEX_ERR_UNSUPPORTED;
}
const apex_plugin plugin{sizeof(apex_plugin), APEX_PLUGIN_ABI_MAJOR, APEX_PLUGIN_ABI_MINOR, 0, nullptr, activate, nullptr, nullptr, nullptr};
}
extern "C" APEX_PLUGIN_EXPORT const apex_plugin* apex_plugin_entry(const apex_host* h) { host = h; return &plugin; }
