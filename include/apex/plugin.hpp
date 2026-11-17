#pragma once
#include "apex_plugin.h"
#include <string_view>
#include <string>
#include <functional>
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
            sizeof(table), 0,
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
            sizeof(table), 0,
            [](void* p, const apex_ui_context* context, const apex_ui_form** out) -> apex_status {
                try {
                    if (!out) return APEX_ERR_INVALID;
                    auto& self = *static_cast<Form*>(p);
                    self.controls_.clear();
                    self.describe(context, self.controls_);
                    self.form_ = {sizeof(self.form_), 0, self.controls_.data(), self.controls_.size()};
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

// Element constructors: every element of an ABI array carries its own struct_size (the array's stride), so build
// elements with these (or set struct_size yourself) rather than with a zero-initialised aggregate.
inline apex_tool_segment segment(apex_vec3 a, apex_vec3 b, float width, const float (&rgba)[4]) {
    apex_tool_segment s{}; s.struct_size = sizeof(s); s.width = width; s.a = a; s.b = b;
    for (int i = 0; i < 4; ++i) s.rgba[i] = rgba[i];
    return s;
}
inline apex_tool_ghost ghost(const apex_plane* planes, size_t count, float opacity, std::string_view material = {}) {
    apex_tool_ghost g{}; g.struct_size = sizeof(g); g.opacity = opacity; g.planes = planes; g.plane_count = count;
    g.material = string(material);
    return g;
}
inline apex_overlay_point point(apex_vec3 position, float size, const float (&rgba)[4]) {
    apex_overlay_point p{}; p.struct_size = sizeof(p); p.size = size; p.position = position;
    for (int i = 0; i < 4; ++i) p.rgba[i] = rgba[i];
    return p;
}
inline apex_overlay_label label(apex_vec3 position, std::string_view text, const float (&rgba)[4]) {
    apex_overlay_label l{}; l.struct_size = sizeof(l); l.position = position; l.text = string(text);
    for (int i = 0; i < 4; ++i) l.rgba[i] = rgba[i];
    return l;
}
inline apex_overlay_ghost overlayGhost(const apex_plane* planes, size_t count, const float (&rgba)[4]) {
    apex_overlay_ghost g{}; g.struct_size = sizeof(g); g.planes = planes; g.plane_count = count;
    for (int i = 0; i < 4; ++i) g.rgba[i] = rgba[i];
    return g;
}
inline apex_overlay_tint tint(int32_t side, const float (&rgba)[4]) {
    apex_overlay_tint t{}; t.struct_size = sizeof(t); t.side = side;
    for (int i = 0; i < 4; ++i) t.rgba[i] = rgba[i];
    return t;
}

// Edit operation arguments (apex_edit_ops_v1, 0.9.3). Same rule: every element carries its struct_size.
inline apex_face_geometry faceGeometry(int32_t side, const std::vector<uint32_t>& indices, apex_vec3 uaxis, apex_vec3 vaxis,
                                       double shiftU = 0, double shiftV = 0, double scaleU = 0.25, double scaleV = 0.25) {
    apex_face_geometry f{}; f.struct_size = sizeof(f); f.side = side; f.uaxis = uaxis; f.vaxis = vaxis;
    f.shift_u = shiftU; f.shift_v = shiftV; f.scale_u = scaleU; f.scale_v = scaleV;
    f.indices = indices.data(); f.index_count = indices.size();   // `indices` must outlive the call
    return f;
}
inline apex_solid_geometry solidGeometry(const std::vector<apex_vec3>& vertices, const std::vector<apex_face_geometry>& faces) {
    apex_solid_geometry g{}; g.struct_size = sizeof(g);
    g.vertices = vertices.data(); g.vertex_count = vertices.size(); g.faces = faces.data(); g.face_count = faces.size();
    return g;
}
inline apex_connection connection(std::string_view output, std::string_view target, std::string_view input,
                                  std::string_view parameter = {}, std::string_view delay = "0", std::string_view times = "-1") {
    apex_connection c{}; c.struct_size = sizeof(c); c.output = string(output); c.target = string(target);
    c.input = string(input); c.parameter = string(parameter); c.delay = string(delay); c.times = string(times);
    return c;
}
inline apex_face_match faceMatch(uint32_t planeIndex, int32_t oldSide) {
    apex_face_match m{}; m.struct_size = sizeof(m); m.plane_index = planeIndex; m.old_side = oldSide;
    return m;
}
// The operation table of an edit callback, checked: null when the host's table is too old to have `member`.
// Use as `if (auto fn = plugin::available(ops, &apex_edit_ops_v1::set_connections)) fn(ctx, ...)`.
template <class Fn> inline Fn available(const apex_edit_ops_v1* ops, Fn apex_edit_ops_v1::* member) {
    if (!ops) return nullptr;
    const auto end = reinterpret_cast<const char*>(&(ops->*member)) + sizeof(Fn) - reinterpret_cast<const char*>(ops);
    return static_cast<size_t>(end) <= ops->struct_size ? ops->*member : nullptr;
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

inline const apex_io_v1* io(const apex_host& host) {
    return static_cast<const apex_io_v1*>(host.get_interface(host.context, string("apex.io/1")));
}
inline const apex_jobs_v1* jobs(const apex_host& host) {
    return static_cast<const apex_jobs_v1*>(host.get_interface(host.context, string("apex.jobs/1")));
}
inline const apex_watch_v1* watch(const apex_host& host) {
    return static_cast<const apex_watch_v1*>(host.get_interface(host.context, string("apex.watch/1")));
}

// apex.io/1. Derive and implement import(); edit through context.ops/context.operation. A non-OK
// return (or an exception) rolls the whole import back.
class Importer {
public:
    virtual ~Importer() = default;
    virtual apex_status import(std::string_view path, const apex_import_context& context) = 0;
    static apex_status report(const apex_import_context& context, std::string_view message) {
        return context.report ? context.report(context.sink, string(message)) : APEX_ERR_UNSUPPORTED;
    }
    static apex_status trampoline(void* user, apex_str path, const apex_import_context* context) {
        try { return static_cast<Importer*>(user)->import(view(path), *context); }
        catch (...) { return APEX_ERR_INTERNAL; }
    }
};
// Activate only. `id` is plugin.<plugin>.<name>; `extensions` is comma separated without dots ("obj,mesh").
inline apex_status registerImporter(const apex_host& host, std::string_view id, std::string_view name,
                                    std::string_view extensions, Importer& importer) {
    const auto* api = io(host);
    return api ? api->register_importer(host.context, string(id), string(name), string(extensions), &importer, &Importer::trampoline)
               : APEX_ERR_UNSUPPORTED;
}

// apex.jobs/1. run() executes on a worker thread (no host calls except from the cancellation query);
// done() executes on the editor thread.
class Job {
public:
    virtual ~Job() = default;
    virtual apex_status run(const apex_job_context& context) = 0;
    virtual void done(apex_status status) = 0;
    static bool cancelled(const apex_job_context& context) { return context.cancelled && context.cancelled(context.job) != 0; }
    static apex_status workTrampoline(void* user, const apex_job_context* context) {
        try { return static_cast<Job*>(user)->run(*context); }
        catch (...) { return APEX_ERR_INTERNAL; }
    }
    static void doneTrampoline(void* user, apex_status status) {
        try { static_cast<Job*>(user)->done(status); }
        catch (...) {}
    }
};
// The Job must outlive its completion (or be cancelled first).
inline apex_status submitJob(const apex_host& host, Job& job, uint64_t* id = nullptr) {
    const auto* api = jobs(host);
    if (!api) return APEX_ERR_UNSUPPORTED;
    uint64_t local = 0;
    const auto status = api->submit(host.context, &job, &Job::workTrampoline, &Job::doneTrampoline, id ? id : &local);
    return status;
}
inline apex_status cancelJob(const apex_host& host, uint64_t id) {
    const auto* api = jobs(host);
    return api ? api->cancel(host.context, id) : APEX_ERR_UNSUPPORTED;
}

// apex.watch/1. changed() runs on the editor thread, once per burst of writes.
class Watcher {
public:
    virtual ~Watcher() = default;
    virtual void changed(std::string_view path) = 0;
    static void trampoline(void* user, apex_str path) {
        try { static_cast<Watcher*>(user)->changed(view(path)); }
        catch (...) {}
    }
};
inline apex_status watchPath(const apex_host& host, std::string_view absolutePath, Watcher& watcher, uint64_t* id) {
    const auto* api = watch(host);
    return api ? api->watch(host.context, string(absolutePath), &watcher, &Watcher::trampoline, id) : APEX_ERR_UNSUPPORTED;
}
inline apex_status unwatch(const apex_host& host, uint64_t id) {
    const auto* api = watch(host);
    return api ? api->unwatch(host.context, id) : APEX_ERR_UNSUPPORTED;
}

// apex.vfs/1. Derive and implement list() and open(); both may run on host worker threads (open() on several
// at once), so they must be thread-safe and quick, and must not call the host. Exceptions never cross the ABI
// (a throw fails that call: the file is reported missing).
class FileProvider {
public:
    virtual ~FileProvider() = default;
    // Prepare for a new listing (optional).
    virtual apex_status refresh() { return APEX_OK; }
    // Call add(path) for each file: relative paths with '/' separators.
    virtual apex_status list(const std::function<void(std::string_view)>& add) = 0;
    // Return the file's bytes (at most maxBytes are kept) through `bytes`, or APEX_ERR_NOT_FOUND.
    virtual apex_status open(std::string_view path, std::vector<char>& bytes) = 0;
    static apex_vfs_provider table(FileProvider& provider) {
        return {sizeof(apex_vfs_provider), 0, &provider,
            [](void* p) -> apex_status { try { return static_cast<FileProvider*>(p)->refresh(); } catch (...) { return APEX_ERR_INTERNAL; } },
            [](void* p, const apex_vfs_file_sink* sink) -> apex_status {
                try {
                    if (!sink || !sink->add) return APEX_ERR_INVALID;
                    return static_cast<FileProvider*>(p)->list([&](std::string_view path) { sink->add(sink->sink, string(path)); });
                } catch (...) { return APEX_ERR_INTERNAL; }
            },
            [](void* p, apex_str path, const apex_vfs_data_sink* sink) -> apex_status {
                try {
                    if (!sink || !sink->write) return APEX_ERR_INVALID;
                    std::vector<char> bytes;
                    const auto status = static_cast<FileProvider*>(p)->open(view(path), bytes);
                    if (status != APEX_OK) return status;
                    return bytes.empty() ? APEX_OK : sink->write(sink->sink, bytes.data(), bytes.size());
                } catch (...) { return APEX_ERR_INTERNAL; }
            }};
    }
};
inline const apex_vfs_v1* vfs(const apex_host& host) {
    return static_cast<const apex_vfs_v1*>(host.get_interface(host.context, string("apex.vfs/1")));
}
inline const apex_assets_v1* assets(const apex_host& host) {
    return static_cast<const apex_assets_v1*>(host.get_interface(host.context, string("apex.assets/1")));
}
// Activate only. `id` is plugin.<plugin>.<name>; `name` is the mount's display name. `kind`: 0 first, 1 last,
// 2 before, 3 after the profile mount whose path contains `anchor`. `provider` must outlive the plugin.
inline apex_status registerVfsProvider(const apex_host& host, std::string_view id, std::string_view name,
                                       FileProvider& provider, uint32_t kind = 1, std::string_view anchor = {}) {
    const auto* api = vfs(host);
    if (!api) return APEX_ERR_UNSUPPORTED;
    const auto table = FileProvider::table(provider);  // the host copies it during the call
    const apex_vfs_position position{sizeof(position), kind, string(anchor)};
    return api->register_provider(host.context, string(id), string(name), &table, &position);
}
// Editor thread: ask the host to reload the game's files because the provider's content changed.
inline apex_status requestVfsRefresh(const apex_host& host, std::string_view id) {
    const auto* api = vfs(host);
    return api ? api->request_refresh(host.context, string(id)) : APEX_ERR_UNSUPPORTED;
}
// apex.assets/1 helpers (editor thread).
inline apex_status findMaterial(const apex_host& host, std::string_view name, apex_material_info& out) {
    const auto* api = assets(host);
    out = {}; out.struct_size = sizeof(out);
    return api ? api->find_material(host.context, string(name), &out) : APEX_ERR_UNSUPPORTED;
}
inline apex_status listMaterials(const apex_host& host, const std::function<bool(const apex_material_info&)>& visit) {
    const auto* api = assets(host);
    if (!api) return APEX_ERR_UNSUPPORTED;
    struct Box { const std::function<bool(const apex_material_info&)>& visit; };
    Box box{visit};
    return api->list_materials(host.context, &box, [](void* user, const apex_material_info* info) -> int {
        try { return static_cast<Box*>(user)->visit(*info) ? 0 : 1; } catch (...) { return 1; }
    });
}
struct VmtEdit { std::string key, value; bool remove = false; };
inline apex_status writeVmt(const apex_host& host, std::string_view material, const std::vector<VmtEdit>& edits) {
    const auto* api = assets(host);
    if (!api) return APEX_ERR_UNSUPPORTED;
    std::vector<apex_vmt_edit> native;
    for (const auto& e : edits) native.push_back({sizeof(apex_vmt_edit), e.remove ? 1u : 0u, string(e.key), string(e.value)});
    return api->write_vmt(host.context, string(material), native.data(), native.size());
}
}
