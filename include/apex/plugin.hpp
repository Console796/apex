#pragma once
#include "apex_plugin.h"
#include <string_view>
#include <string>
#include <vector>
#include <exception>

namespace apex::plugin {
inline apex_str string(std::string_view text) { return {text.data(), text.size()}; }
inline std::string_view view(apex_str text) { return text.data ? std::string_view(text.data, text.size) : std::string_view{}; }

// The wrapper owns no foreign memory. Derived objects must outlive their registration.
class Tool {
public:
    virtual ~Tool() = default;
    virtual Tool* createInstance() { return nullptr; }
    virtual void activate() {}
    virtual void deactivate() {}
    virtual bool input(const apex_tool_event&, const apex_tool_view&) { return false; }
    virtual apex_status visual(uint64_t, apex_tool_visual&) { return APEX_OK; }
    virtual apex_status readout(apex_tool_readout&) { return APEX_OK; }
    virtual bool setParameter(std::string_view, double) { return false; }
    virtual bool perform(std::string_view) { return false; }
    virtual void cancel() {}
    virtual void documentChanged() {}
    virtual void forget(uint64_t) {}
    static const apex_tool_callbacks* callbacks() {
        static const apex_tool_callbacks table{
            sizeof(table),
            [](void* p) { try { static_cast<Tool*>(p)->activate(); } catch (...) {} },
            [](void* p) { try { static_cast<Tool*>(p)->deactivate(); } catch (...) {} },
            [](void* p, const apex_tool_event* e, const apex_tool_view* v) -> int {
                try { return e && v && static_cast<Tool*>(p)->input(*e, *v); } catch (...) { return 0; }
            },
            [](void* p, uint64_t s, apex_tool_visual* v) -> apex_status {
                try { return v ? static_cast<Tool*>(p)->visual(s, *v) : APEX_ERR_INVALID; } catch (...) { return APEX_ERR_INTERNAL; }
            },
            [](void* p, apex_tool_readout* r) -> apex_status {
                try { return r ? static_cast<Tool*>(p)->readout(*r) : APEX_ERR_INVALID; } catch (...) { return APEX_ERR_INTERNAL; }
            },
            [](void* p, apex_str id, double n) -> int { try { return static_cast<Tool*>(p)->setParameter(view(id), n); } catch (...) { return 0; } },
            [](void* p, apex_str id) -> int { try { return static_cast<Tool*>(p)->perform(view(id)); } catch (...) { return 0; } },
            [](void* p) { try { static_cast<Tool*>(p)->cancel(); } catch (...) {} },
            [](void* p) { try { static_cast<Tool*>(p)->documentChanged(); } catch (...) {} },
            [](void* p, uint64_t s) { try { static_cast<Tool*>(p)->forget(s); } catch (...) {} },
            [](void* p) -> void* { try { return static_cast<Tool*>(p)->createInstance(); } catch (...) { return nullptr; } },
            [](void* p) { try { delete static_cast<Tool*>(p); } catch (...) {} }
        };
        return &table;
    }
};
// A declarative form owner: a panel, a section or a dialog. Descriptions stay valid until the next describe().
class Form {
public:
    virtual ~Form() = default;
    // Builds the description. context is null for panels and dialogs.
    virtual void describe(const apex_ui_context* context, std::vector<apex_ui_control>& controls) = 0;
    virtual void changed(std::string_view, const apex_ui_value&) {}
    virtual void clicked(std::string_view) {}
    static apex_ui_control control(uint32_t kind, std::string_view key, std::string_view label) {
        apex_ui_control c{}; c.struct_size = sizeof(c); c.kind = kind; c.key = string(key); c.label = string(label);
        c.value.struct_size = sizeof(c.value);
        return c;
    }
    static const apex_ui_callbacks* callbacks() {
        static const apex_ui_callbacks table{
            sizeof(table),
            [](void* p, const apex_ui_context* context, const apex_ui_form** out) -> apex_status {
                try {
                    if (!out) return APEX_ERR_INVALID;
                    auto& self = *static_cast<Form*>(p);
                    self.controls_.clear();
                    self.describe(context, self.controls_);
                    self.form_ = {sizeof(self.form_), self.controls_.data(), self.controls_.size()};
                    *out = &self.form_;
                    return APEX_OK;
                } catch (...) { return APEX_ERR_INTERNAL; }
            },
            [](void* p, apex_str key, const apex_ui_value* v) { try { if (v) static_cast<Form*>(p)->changed(view(key), *v); } catch (...) {} },
            [](void* p, apex_str key) { try { static_cast<Form*>(p)->clicked(view(key)); } catch (...) {} }
        };
        return &table;
    }
private:
    std::vector<apex_ui_control> controls_;
    apex_ui_form form_{};
};
inline const apex_tools_v1* tools(const apex_host& host) {
    return static_cast<const apex_tools_v1*>(host.get_interface(host.context, string("apex.tools/1")));
}
inline const apex_actions_v1* actions(const apex_host& host) {
    return static_cast<const apex_actions_v1*>(host.get_interface(host.context, string("apex.actions/1")));
}
inline const apex_ui_v1* ui(const apex_host& host) {
    return static_cast<const apex_ui_v1*>(host.get_interface(host.context, string("apex.ui/1")));
}
inline const apex_edit_v1* edit(const apex_host& host) {
    return static_cast<const apex_edit_v1*>(host.get_interface(host.context, string("apex.edit/1")));
}
inline const apex_selection_v1* selection(const apex_host& host) {
    return static_cast<const apex_selection_v1*>(host.get_interface(host.context, string("apex.selection/1")));
}
inline apex_status registerPanel(const apex_host& host, std::string_view id, std::string_view title, Form& form) {
    const auto* api = ui(host);
    return api ? api->register_panel(host.context, string(id), string(title), &form, Form::callbacks()) : APEX_ERR_UNSUPPORTED;
}
inline apex_status registerSection(const apex_host& host, std::string_view id, std::string_view title, std::string_view target, Form& form) {
    const auto* api = ui(host);
    return api ? api->register_section(host.context, string(id), string(title), string(target), &form, Form::callbacks()) : APEX_ERR_UNSUPPORTED;
}
inline apex_status refresh(const apex_host& host, std::string_view id) {
    const auto* api = ui(host);
    return api ? api->refresh(host.context, string(id)) : APEX_ERR_UNSUPPORTED;
}
// Returns true when the user accepted the dialog.
inline bool showDialog(const apex_host& host, std::string_view title, Form& form) {
    const auto* api = ui(host);
    uint32_t accepted = 0;
    return api && api->show_dialog(host.context, string(title), &form, Form::callbacks(), &accepted) == APEX_OK && accepted;
}
inline const apex_overlay_v1* overlays(const apex_host& host) {
    return static_cast<const apex_overlay_v1*>(host.get_interface(host.context, string("apex.overlay/1")));
}
inline const apex_helpers_v1* helpers(const apex_host& host) {
    return static_cast<const apex_helpers_v1*>(host.get_interface(host.context, string("apex.helpers/1")));
}
inline const apex_problems_v1* problems(const apex_host& host) {
    return static_cast<const apex_problems_v1*>(host.get_interface(host.context, string("apex.problems/1")));
}

// apex.overlay/1. Fill the vectors, then set(); the host copies them before set() returns.
struct Overlay {
    std::vector<apex_tool_segment> segments;
    std::vector<apex_overlay_point> points;
    std::vector<apex_overlay_label> labels;
    std::vector<apex_overlay_ghost> ghosts;
    std::vector<apex_overlay_tint> tints;
    void clearLists() { segments.clear(); points.clear(); labels.clear(); ghosts.clear(); tints.clear(); }
};
// Activate only. `id` is plugin.<plugin>.<name>; `name` is the toggle's title.
inline apex_status declareOverlay(const apex_host& host, std::string_view id, std::string_view name) {
    const auto* api = overlays(host);
    return api ? api->declare(host.context, string(id), string(name)) : APEX_ERR_UNSUPPORTED;
}
inline apex_status setOverlay(const apex_host& host, std::string_view id, const Overlay& overlay) {
    const auto* api = overlays(host);
    if (!api) return APEX_ERR_UNSUPPORTED;
    apex_overlay_list list{};
    list.struct_size = sizeof(list);
    list.segments = overlay.segments.data(); list.segment_count = overlay.segments.size();
    list.points = overlay.points.data(); list.point_count = overlay.points.size();
    list.labels = overlay.labels.data(); list.label_count = overlay.labels.size();
    list.ghosts = overlay.ghosts.data(); list.ghost_count = overlay.ghosts.size();
    list.tints = overlay.tints.data(); list.tint_count = overlay.tints.size();
    return api->set(host.context, string(id), &list);
}
inline apex_status clearOverlay(const apex_host& host, std::string_view id) {
    const auto* api = overlays(host);
    return api ? api->clear(host.context, string(id)) : APEX_ERR_UNSUPPORTED;
}

// apex.helpers/1. Derive and implement draw(); exceptions never cross the ABI.
class Helper {
public:
    virtual ~Helper() = default;
    virtual void draw(const apex_helper_call& call, const apex_helper_sink& sink) = 0;
    static void segment(const apex_helper_sink& sink, const apex_tool_segment& value) {
        if (sink.segment) sink.segment(sink.sink, &value);
    }
    static void label(const apex_helper_sink& sink, const apex_overlay_label& value) {
        if (sink.label) sink.label(sink.sink, &value);
    }
    static apex_status trampoline(void* user, const apex_helper_call* call, const apex_helper_sink* sink) {
        try { static_cast<Helper*>(user)->draw(*call, *sink); return APEX_OK; }
        catch (...) { return APEX_ERR_INTERNAL; }
    }
};
// Activate only. `name` is the FGD helper name; `helper` must outlive the plugin.
inline apex_status registerHelper(const apex_host& host, std::string_view name, Helper& helper) {
    const auto* api = helpers(host);
    return api ? api->register_helper(host.context, string(name), &helper, &Helper::trampoline) : APEX_ERR_UNSUPPORTED;
}

// apex.problems/1. Derive and implement check(); report through the sink.
class ProblemCheck {
public:
    virtual ~ProblemCheck() = default;
    virtual void check(const apex_problem_sink& sink) = 0;
    static apex_status report(const apex_problem_sink& sink, uint32_t severity, std::string_view text,
                              const std::vector<apex_node_id>& objects = {}) {
        apex_problem problem{};
        problem.struct_size = sizeof(problem); problem.severity = severity; problem.text = string(text);
        problem.objects = objects.data(); problem.object_count = objects.size();
        return sink.report(sink.sink, &problem);
    }
    static apex_status trampoline(void* user, const apex_problem_sink* sink) {
        try { static_cast<ProblemCheck*>(user)->check(*sink); return APEX_OK; }
        catch (...) { return APEX_ERR_INTERNAL; }
    }
};
// Activate only. `id` is plugin.<plugin>.<name>; `name` is the check's title.
inline apex_status registerProblemCheck(const apex_host& host, std::string_view id, std::string_view name, ProblemCheck& check) {
    const auto* api = problems(host);
    return api ? api->register_check(host.context, string(id), string(name), &check, &ProblemCheck::trampoline) : APEX_ERR_UNSUPPORTED;
}
}
