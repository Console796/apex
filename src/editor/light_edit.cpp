#include "editor/light_edit.hpp"

#include "editor/editor_session.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace apex::editor {
namespace {
std::string numeric(double value) {
    std::ostringstream text;
    text << std::setprecision(10) << value;
    return text.str();
}
struct Light {
    NodeId id;
    const assets::EntityClass* cls;
    kv1::Node values;
    document::EntityPose pose;
};
std::optional<Light> light(const EditorSession& session) {
    if (!session.document()) return {};
    const auto targets = session.selection().ordered();
    if (targets.size() != 1 || !std::holds_alternative<NodeId>(targets.front())) return {};
    const auto id = std::get<NodeId>(targets.front());
    const auto handle = session.document()->handle(id);
    if (!handle || !std::holds_alternative<Handle<document::Entity>>(*handle)) return {};
    const auto& block = *session.document()->get(*handle)->block;
    const auto* name = block.find("classname");
    const auto* cls = name ? session.entityClasses().find(name->decodedValue().value_or("")) : nullptr;
    if (!cls || !std::ranges::any_of(cls->properties, [](const auto& p) { return ciEquals(p.name, "_light"); })) return {};
    auto values = document::entityHelperValues(block, *cls);
    auto pose = document::entityPose(values);
    if (document::hasEntityHelper(*cls, "lightcone")) {
        pose = document::spotlightPose(values, pose, document::spotlightTarget(*session.document(), values));
    }
    return Light{id, cls, std::move(values), pose};
}
double number(const kv1::Node& block, std::string_view key) {
    const auto* node = block.find(key);
    const auto value = node ? node->asDouble() : Result<double>{};
    return value && std::isfinite(*value) ? *value : 0;
}
std::map<std::string, std::string> keysFor(const kv1::Node& block, std::string_view key, double value) {
    std::map<std::string, std::string> keys{{std::string(key), numeric(value)}};
    if (key == "_fifty_percent_distance" && value > 0 && number(block, "_zero_percent_distance") <= value)
        keys["_zero_percent_distance"] = numeric(value * 2);
    if (key == "_zero_percent_distance" && value > 0) {
        const double half = number(block, "_fifty_percent_distance");
        if (half <= 0 || half >= value) keys["_fifty_percent_distance"] = numeric(value * 0.5);
    }
    if (key == "_cone" && number(block, "_inner_cone") > value) keys["_inner_cone"] = numeric(value);
    if (key == "_inner_cone" && number(block, "_cone") < value) keys["_cone"] = numeric(value);
    return keys;
}
std::optional<DVec3> planePoint(Ray ray, DVec3 origin, DVec3 normal) {
    const double denominator = glm::dot(ray.dir, normal);
    if (std::abs(denominator) < 1e-8) return {};
    const double t = glm::dot(origin - ray.origin, normal) / denominator;
    if (!std::isfinite(t) || t < 0) return {};
    return ray.origin + t * ray.dir;
}
}
std::optional<LightControl> lightControl(std::string_view key) {
    const auto name = toLowerAscii(key);
    if (name == "_fifty_percent_distance") return LightControl{name, "50% falloff"};
    if (name == "_zero_percent_distance") return LightControl{name, "Negligible falloff"};
    if (name == "_distance" || name == "distance") return LightControl{name, "Max distance"};
    if (name == "_hard_radius_override") return LightControl{name, "Hard radius"};
    if (name == "_inner_cone") return LightControl{name, "Inner angle", 0, 89.9, 0.1};
    if (name == "_cone") return LightControl{name, "Outer angle", 0.1, 89.9, 0.1};
    if (name == "_constant_attn") return LightControl{name, "Constant", 0, 1048576, 0.01};
    if (name == "_linear_attn") return LightControl{name, "Linear", 0, 1048576, 0.01};
    if (name == "_quadratic_attn") return LightControl{name, "Quadratic", 0, 1048576, 0.01};
    if (name == "_exponent") return LightControl{name, "Focus", 0, 1048576, 0.1};
    return {};
}
std::vector<LightEdit::Handle> LightEdit::handles(const EditorSession& session) const {
    auto selected = light(session);
    if (!selected) return {};
    if (drag_ && drag_->node == selected->id)
        for (const auto& [key, value] : drag_->keys) {
            if (auto* node = selected->values.find(key)) *node = kv1::Node::value(key, value);
        }
    std::vector<Handle> result;
    const auto& block = selected->values;
    for (const auto key : {"_fifty_percent_distance", "_zero_percent_distance", "_distance", "_hard_radius_override"}) {
        if (!block.find(key)) continue;
        const double value = number(block, key);
        if (!(value > 0)) continue;
        const auto control = *lightControl(key);
        const Vec4 color = key == std::string_view{"_fifty_percent_distance"} ? Vec4{0.3f, 0.85f, 1, 1}
            : key == std::string_view{"_zero_percent_distance"} ? Vec4{1, 0.65f, 0.2f, 1} : Vec4{0.7f, 1, 0.4f, 1};
        for (const auto axis : {DVec3{1, 0, 0}, DVec3{0, 1, 0}, DVec3{0, 0, 1}, DVec3{-1, 0, 0}, DVec3{0, -1, 0}, DVec3{0, 0, -1}})
            result.push_back({control.key, control.label, selected->pose.origin + axis * value, axis, value, 0, color});
    }
    if (document::hasEntityHelper(*selected->cls, "lightcone")) {
        double distance = 256;
        for (const auto key : {"distance", "_distance", "_zero_percent_distance", "_hard_radius_override"})
            if (const auto value = number(block, key); value > 0) { distance = value; break; }
        for (const auto key : {"_inner_cone", "_cone"}) {
            const double angle = number(block, key);
            if (!(angle > 0 && angle < 90)) continue;
            const auto control = *lightControl(key);
            for (const auto radial : {selected->pose.basis[1], -selected->pose.basis[1], selected->pose.basis[2], -selected->pose.basis[2]}) {
                const auto point = selected->pose.origin + selected->pose.basis[0] * distance + radial * (distance * std::tan(glm::radians(angle)));
                result.push_back({control.key, control.label, point, radial, angle, distance,
                    key == std::string_view{"_cone"} ? Vec4{1, 0.65f, 0.2f, 1} : Vec4{0.3f, 0.85f, 1, 1}});
            }
        }
    }
    return result;
}
std::vector<ToolParameter> LightEdit::parameters(const EditorSession& session) const {
    const auto selected = light(session);
    if (!selected) return {};
    std::vector<ToolParameter> result;
    for (const auto key : {"_fifty_percent_distance", "_zero_percent_distance", "_distance", "_inner_cone", "_cone"}) {
        if (!selected->values.find(key)) continue;
        const auto control = *lightControl(key);
        double value = number(selected->values, key);
        if (drag_) if (const auto it = drag_->keys.find(key); it != drag_->keys.end()) value = std::stod(it->second);
        result.push_back({control.key, control.label, ToolParameter::Kind::Number, value, control.minimum, control.maximum, {}});
    }
    return result;
}
bool LightEdit::setParameter(EditorSession& session, std::string_view key, double value) {
    const auto selected = light(session);
    const auto control = lightControl(key);
    if (!selected || !control || !selected->values.find(key) || !std::isfinite(value) || active()) return false;
    const auto keys = keysFor(selected->values, key, std::clamp(value, control->minimum, control->maximum));
    // One transaction includes dependent cone/falloff bounds.
    auto tx = session.document()->begin("Tune light");
    const auto handle = *session.document()->handle(selected->id);
    for (const auto& [name, text] : keys) tx.setKeyValue(handle, name, text);
    tx.commit();
    return true;
}
std::string LightEdit::status() const {
    return drag_ ? drag_->handle.label + ": " + numeric(drag_->value) + " — Shift: fine; Escape: cancel" : std::string{};
}
bool LightEdit::input(EditorSession& session, Preview& preview, const InputEvent& e, const ToolView& view) {
    if (drag_) {
        const auto selected = light(session);
        if (!selected || selected->id != drag_->node) { cancel(preview); session.publishToolStatus(); return false; }
        if (e.source != drag_->source) return false;
        if (e.kind == InputKind::Cancel || (e.kind == InputKind::KeyDown && e.key == "Escape") ||
            (e.kind == InputKind::Press && e.button == PointerButton::Right)) {
            cancel(preview); session.publishToolStatus(); return true;
        }
        if (e.kind == InputKind::Move || (e.kind == InputKind::Release && e.button == PointerButton::Left)) {
            if (const auto startPixel = worldToPixel(view, drag_->start); startPixel && drag_->keys.empty() &&
                glm::length(e.cursor - *startPixel) < 3) {
                if (e.kind == InputKind::Release) { cancel(preview); session.publishToolStatus(); }
                return true;
            }
            if (const auto p = planePoint(view.cursorRay, drag_->start, drag_->planeNormal)) {
                const auto axis = drag_->handle.axis - drag_->planeNormal * glm::dot(drag_->handle.axis, drag_->planeNormal);
                const double denominator = glm::dot(axis, axis);
                if (denominator > 1e-6) {
                    double delta = glm::dot(*p - drag_->start, axis) / denominator;
                    if (e.shift) delta *= 0.1;
                    const auto& h = drag_->handle;
                    double value = h.distance > 0 ? glm::degrees(std::atan(std::max(0.0, h.distance * std::tan(glm::radians(h.value)) + delta) / h.distance)) : h.value + delta;
                    const auto control = *lightControl(h.key);
                    const auto mods = transformModifiers(session.preset(), e);
                    if (!mods.suspend) {
                        const double step = h.distance > 0 ? (e.shift ? 0.1 : 1.0) : (e.shift ? 0.1 : session.gridSize());
                        if (step > 0) value = std::round(value / step) * step;
                    }
                    drag_->value = std::clamp(value, control.minimum, control.maximum);
                    drag_->keys = keysFor(selected->values, h.key, drag_->value);
                    preview.setEntityKeys({{drag_->node, drag_->keys}});
                    session.publishToolStatus();
                }
            }
            if (e.kind == InputKind::Release) {
                const auto node = drag_->node;
                const auto keys = drag_->keys;
                cancel(preview);
                // Reset ownership before document observers see the committed edit.
                if (!keys.empty() && session.document() && session.document()->handle(node)) {
                    auto tx = session.document()->begin("Tune light");
                    const auto handle = *session.document()->handle(node);
                    for (const auto& [key, value] : keys) tx.setKeyValue(handle, key, value);
                    tx.commit();
                }
                session.publishToolStatus();
            }
        }
        return true;
    }
    if (e.navigating || (e.held() && e.kind != InputKind::Press) || e.ctrl || e.alt) return false;
    if (e.kind == InputKind::Leave && hovered_) { hovered_.reset(); session.publishToolStatus(); }
    if (e.kind != InputKind::Press && e.kind != InputKind::Move) return false;
    double best = 81;
    std::optional<Handle> hit;
    for (const auto& handle : handles(session)) {
        const auto axis = handle.axis - view.cameraForward * glm::dot(handle.axis, view.cameraForward);
        if (glm::length(axis) < 0.1) continue;
        if (const auto pixel = worldToPixel(view, handle.point)) {
            const double distance = glm::dot(*pixel - e.cursor, *pixel - e.cursor);
            if (distance < best) { best = distance; hit = handle; }
        }
    }
    if (e.kind == InputKind::Move && !e.held()) {
        const bool changed = bool(hovered_) != bool(hit) || (hit && hovered_ &&
            (hit->key != hovered_->key || hit->point != hovered_->point));
        hovered_ = hit;
        if (changed) session.publishToolStatus();
    }
    if (!hit) return false;
    if (e.kind == InputKind::Press && e.button == PointerButton::Left) {
        const auto selected = light(session);
        const auto start = planePoint(view.cursorRay, hit->point, view.cameraForward);
        if (!selected || !start) return false;
        drag_ = Drag{e.source, selected->id, *hit, *start, view.cameraForward, hit->value, {}};
        session.clearHover();
        session.publishToolStatus();
        return true;
    }
    return e.kind == InputKind::Move;
}
void LightEdit::overlay(const EditorSession& session, ToolOverlay& overlay, const ToolView& view) const {
    bool showHover = false;
    for (const auto& h : handles(session)) {
        if (!worldToPixel(view, h.point)) continue;
        const bool active = (drag_ && drag_->handle.key == h.key) ||
            (!drag_ && hovered_ && hovered_->key == h.key && hovered_->point == h.point);
        showHover |= active && !drag_;
        const auto color = active ? Vec4{1, 1, 1, 1} : h.color;
        overlay.worldPoints.push_back({h.point, color, active ? 11.0f : 8.0f});
        const double half = (active ? 5.0 : 4.0) / gizmoViewFor(view, h.point).pixelsPerWorldUnit;
        const auto u = glm::normalize(glm::cross(view.cameraForward, view.cameraUp)) * half;
        const auto v = glm::normalize(view.cameraUp) * half;
        const std::array<DVec3, 4> corners{h.point - u - v, h.point + u - v, h.point + u + v, h.point - u + v};
        for (std::size_t i = 0; i < corners.size(); ++i)
            overlay.segments.push_back({corners[i], corners[(i + 1) % corners.size()], color, active ? 3.0f : 2.0f});
    }
    if (showHover && hovered_)
        overlay.labels.push_back({hovered_->point, hovered_->label + ": " + numeric(hovered_->value), {12, -12}, hovered_->color});
    if (drag_) overlay.labels.push_back({drag_->handle.point, status(), {12, -12}, {1, 1, 1, 1}});
}
} // namespace apex::editor
