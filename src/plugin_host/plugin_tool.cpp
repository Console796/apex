#include "plugin_host/plugin_tool.hpp"
#include "editor/editor_session.hpp"
#include "editor/pick_index.hpp"
#include <algorithm>
#include <cmath>

namespace apex::plugin_host {
namespace {
apex_vec3 cv(DVec3 v) { return {v.x, v.y, v.z}; }
DVec3 cv(apex_vec3 v) { return {v.x, v.y, v.z}; }
apex_str str(std::string_view s) { return {s.data(), s.size()}; }
std::string text(apex_str s) { return s.data ? std::string(s.data, s.size) : std::string{}; }
struct ViewContext { editor::EditorSession& session; const editor::ToolView& view; const std::function<void(const char*)>& error; };
apex_status viewError(void* ptr) noexcept {
    const auto& sink = static_cast<ViewContext*>(ptr)->error;
    try { throw; }
    catch (const std::exception& e) { try { if (sink) sink(e.what()); } catch (...) {} }
    catch (...) { try { if (sink) sink("exception in view API"); } catch (...) {} }
    return APEX_ERR_INTERNAL;
}
apex_status pick(void* ptr, apex_ray ray, apex_pick* out) try {
    auto& c = *static_cast<ViewContext*>(ptr);
    if (!out) return APEX_ERR_INVALID;
    if (!c.session.document()) return APEX_ERR_STATE;
    auto hit = c.session.picks().first({cv(ray.origin), cv(ray.direction)});
    if (!hit) return APEX_ERR_NOT_FOUND;
    *out = {hit->node, hit->side.value_or(0), cv(hit->point), cv(hit->normal)};
    return APEX_OK;
} catch (...) { return viewError(ptr); }
apex_status frustum(void* ptr, double x1, double y1, double x2, double y2, apex_plane out[4]) try {
    auto& c = *static_cast<ViewContext*>(ptr);
    if (!out || !c.view.rectFrustum) return APEX_ERR_INVALID;
    const auto planes = c.view.rectFrustum({x1, y1}, {x2, y2});
    for (std::size_t i = 0; i < 4; ++i) out[i] = {cv(planes[i].normal), planes[i].dist};
    return APEX_OK;
} catch (...) { return viewError(ptr); }
apex_status pixel(void* ptr, apex_vec3 point, double* x, double* y) try {
    if (!x || !y) return APEX_ERR_INVALID;
    const auto& view = static_cast<ViewContext*>(ptr)->view;
    const auto result = editor::worldToPixel(view, cv(point));
    if (!result) return APEX_ERR_NOT_FOUND;
    *x = result->x; *y = result->y; return APEX_OK;
} catch (...) { return viewError(ptr); }
apex_status snap(void* ptr, apex_ray ray, apex_vec3* out) try {
    auto& c = *static_cast<ViewContext*>(ptr);
    if (!out) return APEX_ERR_INVALID;
    if (!c.session.document()) return APEX_ERR_STATE;
    *out = cv(c.session.snap({cv(ray.origin), cv(ray.direction)}, c.view).point); return APEX_OK;
} catch (...) { return viewError(ptr); }
apex_tool_view makeView(ViewContext& c) {
    const auto& v = c.view;
    return {sizeof(apex_tool_view), {cv(v.cursorRay.origin), cv(v.cursorRay.dir)}, v.size.x, v.size.y,
            cv(v.cameraPosition), cv(v.cameraAngles), cv(v.cameraForward), cv(v.cameraUp),
            v.fovYDegrees, v.orthoHeight, v.orthographic ? 1u : 0u, &c, pick, frustum, pixel, snap};
}
}
PluginTool::PluginTool(editor::EditorSession& session, std::string id, void* user,
                       apex_tool_callbacks callbacks, Guard guard, std::function<void(const char*)> error)
    : session_(session), id_(std::move(id)), user_(user), callbacks_(callbacks), guard_(std::move(guard)), error_(std::move(error)),
      retirement_(std::make_shared<std::function<void()>>([this] { retire(); })),
      reviver_(std::make_shared<Revive>([this](void* registrationUser, const apex_tool_callbacks& next) {
          retire();
          user_ = registrationUser; callbacks_ = next;
          createInstance(registrationUser);
          if (active_) activate();
      })) {
    createInstance(user);
}
void PluginTool::createInstance(void* user) {
    if (callbacks_.create_instance && callbacks_.destroy_instance) {
        void* instance = nullptr;
        guard_("tool.create_instance", [&] { instance = callbacks_.create_instance(user); });
        if (instance) { user_ = instance; ownsUser_ = true; }
    }
}
PluginTool::~PluginTool() { retire(); }
void PluginTool::retire() {
    if (ownsUser_) guard_("tool.destroy_instance", [&] { callbacks_.destroy_instance(user_); });
    ownsUser_ = false;
    callbacks_ = {};
    preview_.clear();
}
void PluginTool::activate() { active_ = true; if (callbacks_.activate) guard_("tool.activate", [&] { callbacks_.activate(user_); }); refresh(lastSource_); session_.publishToolStatus(); }
void PluginTool::deactivate() { active_ = false; if (callbacks_.deactivate) guard_("tool.deactivate", [&] { callbacks_.deactivate(user_); }); cancel(); }
editor::Handled PluginTool::input(const editor::InputEvent& e, const editor::ToolView& v) {
    if (!callbacks_.input) return false;
    ViewContext context{session_, v, error_}; auto view = makeView(context);
    apex_tool_event event{sizeof(event), static_cast<uint32_t>(e.kind), static_cast<uint32_t>(e.button),
        (e.ctrl ? 1u : 0u) | (e.shift ? 2u : 0u) | (e.alt ? 4u : 0u), e.source, e.cursor.x, e.cursor.y,
        str(e.key), (e.left ? 1u : 0u) | (e.right ? 2u : 0u) | (e.middle ? 4u : 0u) |
                    (e.autoRepeat ? 8u : 0u) | (e.navigating ? 16u : 0u)};
    int handled = 0; lastSource_ = e.source;
    guard_("tool.input", [&] { handled = callbacks_.input(user_, &event, &view); });
    refresh(e.source); session_.publishToolStatus(); return handled != 0;
}
void PluginTool::refresh(editor::InputSource source) {
    if (!callbacks_.visual) { preview_.clear(); return; }
    apex_tool_visual visual{}; visual.struct_size = sizeof(visual);
    std::vector<editor::GhostSolid> ghosts;
    guard_("tool.visual", [&] {
        if (callbacks_.visual(user_, source, &visual) != APEX_OK || (!visual.ghosts && visual.ghost_count)) return;
        for (std::size_t i = 0; i < std::min<std::size_t>(visual.ghost_count, 1024); ++i) {
            const auto& ghost = visual.ghosts[i];
            if (!ghost.planes || ghost.plane_count < 4 || ghost.plane_count > 256) continue;
            std::vector<geometry::PlaneSide> planes;
            for (std::size_t j = 0; j < ghost.plane_count; ++j) {
                geometry::PlaneSide side; side.plane = {cv(ghost.planes[j].normal), ghost.planes[j].distance};
                planes.push_back(std::move(side));
            }
            auto poly = geometry::ConvexPolyhedron::fromPlanes(planes);
            if (poly) ghosts.push_back({std::move(*poly), text(ghost.material), ghost.opacity});
        }
    });
    preview_.setGhosts(ghosts);
}
editor::ToolOverlay PluginTool::overlay(const editor::ToolView&, editor::InputSource source) const {
    editor::ToolOverlay result;
    if (!callbacks_.visual) return result;
    apex_tool_visual visual{}; visual.struct_size = sizeof(visual);
    guard_("tool.overlay", [&] {
        if (callbacks_.visual(user_, source, &visual) != APEX_OK || (!visual.segments && visual.segment_count)) return;
        for (std::size_t i = 0; i < std::min<std::size_t>(visual.segment_count, 65536); ++i) {
            const auto& s = visual.segments[i];
            result.worldSegments.push_back({cv(s.a), cv(s.b), {s.rgba[0], s.rgba[1], s.rgba[2], s.rgba[3]}, s.width});
        }
    });
    return result;
}
editor::ToolReadout PluginTool::readout() const {
    editor::ToolReadout result;
    if (!callbacks_.readout) return result;
    apex_tool_readout readout{}; readout.struct_size = sizeof(readout);
    guard_("tool.readout", [&] {
        if (callbacks_.readout(user_, &readout) != APEX_OK) return;
        result.detail = text(readout.detail); result.status = text(readout.status); result.warn = readout.warn != 0;
        if (!readout.parameters) return;
        for (std::size_t i = 0; i < std::min<std::size_t>(readout.parameter_count, 1024); ++i) {
            const auto& p = readout.parameters[i];
            if (p.kind > 2) continue;
            editor::ToolParameter param{text(p.id), text(p.label), static_cast<editor::ToolParameter::Kind>(p.kind), p.value, p.minimum, p.maximum, {}};
            if (p.choices) for (std::size_t j = 0; j < std::min<std::size_t>(p.choice_count, 1024); ++j) param.choices.push_back(text(p.choices[j]));
            result.parameters.push_back(std::move(param));
        }
    });
    return result;
}
void PluginTool::cancel() { if (callbacks_.cancel) guard_("tool.cancel", [&] { callbacks_.cancel(user_); }); preview_.clear(); session_.publishToolStatus(); }
void PluginTool::forget(editor::InputSource source) { if (callbacks_.forget) guard_("tool.forget", [&] { callbacks_.forget(user_, source); }); if (source == lastSource_) refresh(source); }
void PluginTool::documentChanged() { if (callbacks_.document_changed) guard_("tool.document_changed", [&] { callbacks_.document_changed(user_); }); refresh(lastSource_); session_.publishToolStatus(); }
bool PluginTool::setParameter(std::string_view id, double value) { int result = 0; if (callbacks_.set_parameter) guard_("tool.set_parameter", [&] { result = callbacks_.set_parameter(user_, str(id), value); }); refresh(lastSource_); session_.publishToolStatus(); return result != 0; }
bool PluginTool::perform(const editor::ActionId& action) { int result = 0; if (callbacks_.perform) guard_("tool.perform", [&] { result = callbacks_.perform(user_, str(action)); }); refresh(lastSource_); session_.publishToolStatus(); return result != 0; }
}
