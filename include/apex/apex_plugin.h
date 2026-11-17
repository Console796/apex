#ifndef APEX_PLUGIN_H
#define APEX_PLUGIN_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APEX_PLUGIN_ABI_MAJOR 1u
#define APEX_PLUGIN_ABI_MINOR 0u
#if defined(_WIN32)
#define APEX_PLUGIN_EXPORT __declspec(dllexport)
#else
#define APEX_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

typedef struct apex_str { const char* data; size_t size; } apex_str;
typedef enum apex_status { APEX_OK = 0, APEX_ERR_INVALID, APEX_ERR_NOT_FOUND,
                           APEX_ERR_STATE, APEX_ERR_UNSUPPORTED, APEX_ERR_INTERNAL } apex_status;
typedef uint64_t apex_node_id;

/* Declarative UI values and controls are borrowed for the duration of a call. */
typedef enum apex_ui_kind {
    APEX_UI_LABEL, APEX_UI_TEXT, APEX_UI_NUMBER, APEX_UI_CHECKBOX,
    APEX_UI_CHOICE, APEX_UI_COLOUR, APEX_UI_BUTTON, APEX_UI_TABLE,
    APEX_UI_MATERIAL, APEX_UI_MODEL, APEX_UI_SOUND, APEX_UI_SECTION
} apex_ui_kind;
typedef struct apex_ui_value {
    uint32_t struct_size;
    apex_str text;
    double number;
    uint32_t checked;
} apex_ui_value;
typedef struct apex_ui_control {
    uint32_t struct_size;
    uint32_t kind;
    apex_str key, label;
    apex_ui_value value;
    double minimum, maximum;
    const apex_str* options;
    size_t option_count;
    const struct apex_ui_control* children;
    size_t child_count;
} apex_ui_control;
typedef struct apex_ui_form {
    uint32_t struct_size;
    const apex_ui_control* controls;
    size_t control_count;
} apex_ui_form;
/* What a host panel passes to a plugin section: kind is "materials" (names), "entities" or "sides" (ids). */
typedef struct apex_ui_context {
    uint32_t struct_size;
    apex_str kind;
    const apex_str* names;
    size_t name_count;
    const apex_node_id* ids;
    size_t id_count;
} apex_ui_context;
/* Descriptions and callback values are borrowed until the callback returns.
   context is null for panels and dialogs. */
typedef struct apex_ui_callbacks {
    uint32_t struct_size;
    apex_status (*describe)(void* user, const apex_ui_context* context, const apex_ui_form** form);
    void (*on_change)(void* user, apex_str key, const apex_ui_value* value);
    void (*on_click)(void* user, apex_str key);
} apex_ui_callbacks;
typedef struct apex_ui_v1 {
    uint32_t struct_size;
    apex_status (*register_panel)(void* host, apex_str id, apex_str title, void* user, const apex_ui_callbacks*);
    apex_status (*refresh)(void* host, apex_str id);
    /* target: "materials", "inspector" or "face_properties". */
    apex_status (*register_section)(void* host, apex_str id, apex_str title, apex_str target, void* user, const apex_ui_callbacks*);
    /* Modal: returns once the user closes it; *accepted is 1 for OK. Callbacks run while it is open. */
    apex_status (*show_dialog)(void* host, apex_str title, void* user, const apex_ui_callbacks*, uint32_t* accepted);
} apex_ui_v1;

/* Returned strings are borrowed until the next call to the same interface or document mutation. */
typedef struct apex_document_info {
    uint32_t struct_size;
    uint32_t is_open;
    uint64_t revision;
    size_t root_count;
} apex_document_info;
typedef struct apex_node_info {
    uint32_t struct_size;
    apex_node_id id, parent;
    apex_str kind;
    size_t child_count, key_count;
} apex_node_info;
typedef struct apex_document_v1 {
    uint32_t struct_size;
    apex_status (*info)(void* host, apex_document_info* out);
    apex_status (*root_at)(void* host, size_t index, apex_node_id* out);
    apex_status (*node)(void* host, apex_node_id id, apex_node_info* out);
    apex_status (*child_at)(void* host, apex_node_id id, size_t index, apex_node_id* out);
    apex_status (*key_at)(void* host, apex_node_id id, size_t index, apex_str* key, apex_str* value);
} apex_document_v1;

/* Registration is available only during activate; IDs must belong to the plugin. */
typedef void (*apex_callback)(void* user);
typedef struct apex_registration_v1 {
    uint32_t struct_size;
    apex_status (*action)(void* host, apex_str id, apex_str name, apex_str description,
                          void* user, apex_callback invoke);
    apex_status (*tool)(void* host, apex_str id, apex_str name, apex_str description,
                        void* user, apex_callback activate, apex_callback deactivate);
    apex_status (*panel)(void* host, apex_str id, apex_str title);
} apex_registration_v1;

/* All pointers in tool results are borrowed until the next callback into that tool.
   View callbacks are valid only for the duration of the input callback. */
typedef struct apex_vec3 { double x, y, z; } apex_vec3;
typedef struct apex_plane { apex_vec3 normal; double distance; } apex_plane;
/* Operations run synchronously within one session transaction. Return a non-OK
   status to roll the entire batch back. Operation context is callback-scoped. */
typedef struct apex_edit_ops_v1 {
    uint32_t struct_size;
    apex_status (*set_key)(void*, apex_node_id, apex_str key, apex_str value);
    apex_status (*destroy)(void*, apex_node_id);
    apex_status (*create_solid)(void*, apex_node_id parent, const apex_plane*, size_t, apex_str material);
    apex_status (*reparent)(void*, apex_node_id, apex_node_id parent, size_t position);
    apex_status (*set_side_key)(void*, int32_t side, apex_str key, apex_str value);
    apex_status (*remove_displacement)(void*, int32_t side);
    apex_status (*rebuild_solid)(void*, apex_node_id, const apex_plane*, size_t);
} apex_edit_ops_v1;
typedef apex_status (*apex_edit_callback)(void*, const apex_edit_ops_v1*, void* operation_context);
typedef struct apex_edit_v1 {
    uint32_t struct_size;
    apex_status (*apply)(void* host, apex_str label, apex_str coalesce_key, void* user, apex_edit_callback);
    apex_status (*defer)(void* host, void* user, void (*callback)(void*));
} apex_edit_v1;
/* Selection IDs are node IDs except in face mode, where they are side IDs. */
typedef struct apex_selection_v1 {
    uint32_t struct_size;
    apex_status (*info)(void* host, uint32_t* mode, size_t* count);
    apex_status (*at)(void* host, size_t index, uint64_t* id);
    apex_status (*set)(void* host, uint32_t mode, const uint64_t* ids, size_t count);
} apex_selection_v1;
typedef struct apex_ray { apex_vec3 origin, direction; } apex_ray;
enum apex_input_kind { APEX_INPUT_PRESS, APEX_INPUT_RELEASE, APEX_INPUT_MOVE, APEX_INPUT_KEY_DOWN,
                       APEX_INPUT_KEY_UP, APEX_INPUT_LEAVE, APEX_INPUT_CANCEL, APEX_INPUT_CANCEL_POINTER };
enum apex_pointer_button { APEX_BUTTON_NONE, APEX_BUTTON_LEFT, APEX_BUTTON_RIGHT, APEX_BUTTON_MIDDLE };
typedef struct apex_tool_event {
    uint32_t struct_size, kind, button, modifiers;
    uint64_t source;
    double x, y;
    apex_str key;
    uint32_t flags;
} apex_tool_event;
typedef struct apex_pick { apex_node_id node; int32_t side; apex_vec3 point, normal; } apex_pick;
typedef struct apex_tool_view {
    uint32_t struct_size;
    apex_ray ray;
    double width, height;
    apex_vec3 camera_position, camera_angles, camera_forward, camera_up;
    double fov_y_degrees, ortho_height;
    uint32_t orthographic;
    void* context;
    apex_status (*ray_pick)(void*, apex_ray, apex_pick*);
    apex_status (*rect_frustum)(void*, double, double, double, double, apex_plane out[4]);
    apex_status (*world_to_pixel)(void*, apex_vec3, double* x, double* y);
    apex_status (*snap)(void*, apex_ray, apex_vec3*);
} apex_tool_view;
typedef struct apex_tool_segment { apex_vec3 a, b; float rgba[4], width; } apex_tool_segment;
typedef struct apex_tool_ghost { const apex_plane* planes; size_t plane_count; apex_str material; float opacity; } apex_tool_ghost;
typedef struct apex_tool_visual {
    uint32_t struct_size;
    const apex_tool_segment* segments; size_t segment_count;
    const apex_tool_ghost* ghosts; size_t ghost_count;
} apex_tool_visual;
typedef struct apex_tool_parameter {
    apex_str id, label; uint32_t kind;
    double value, minimum, maximum;
    const apex_str* choices; size_t choice_count;
} apex_tool_parameter;
typedef struct apex_tool_readout {
    uint32_t struct_size;
    apex_str detail, status;
    uint32_t warn;
    const apex_tool_parameter* parameters; size_t parameter_count;
} apex_tool_readout;
typedef struct apex_tool_callbacks {
    uint32_t struct_size;
    apex_callback activate, deactivate;
    int (*input)(void*, const apex_tool_event*, const apex_tool_view*);
    apex_status (*visual)(void*, uint64_t source, apex_tool_visual*);
    apex_status (*readout)(void*, apex_tool_readout*);
    int (*set_parameter)(void*, apex_str id, double value);
    int (*perform)(void*, apex_str action);
    apex_callback cancel, document_changed;
    void (*forget)(void*, uint64_t source);
    /* Optional factory and destructor for state owned by each editor session. */
    void* (*create_instance)(void* registration_user);
    void (*destroy_instance)(void* instance);
} apex_tool_callbacks;
typedef struct apex_tools_v1 {
    uint32_t struct_size;
    apex_status (*register_tool)(void*, apex_str id, apex_str name, apex_str description,
                                 void* user, const apex_tool_callbacks* callbacks);
} apex_tools_v1;
/* Empty tool_id means the global context. */
typedef struct apex_action_suggestion { apex_str preset, chord, tool_id; } apex_action_suggestion;
typedef struct apex_actions_v1 {
    uint32_t struct_size;
    apex_status (*register_action)(void*, apex_str id, apex_str name, apex_str description,
                                   void* user, apex_callback invoke,
                                   const apex_action_suggestion* suggestions, size_t suggestion_count);
} apex_actions_v1;

/* apex.overlay/1 (0.9.5). Overlay lists are world primitives drawn in every display mode. declare() is
   available only during activate and creates the per-viewport toggle named `name`; set() and clear()
   need the declaring plugin and are available during activate and any callback into that plugin.
   The host copies everything in a list before set() returns. Segment width is in pixels. Ghost
   planes use the apex.tools ghost convention (a convex solid). Tints colour whole faces by side ID. */
typedef struct apex_overlay_point { apex_vec3 position; float rgba[4]; float size; } apex_overlay_point;
typedef struct apex_overlay_label { apex_vec3 position; apex_str text; float rgba[4]; } apex_overlay_label;
typedef struct apex_overlay_ghost { const apex_plane* planes; size_t plane_count; float rgba[4]; } apex_overlay_ghost;
typedef struct apex_overlay_tint { int32_t side; float rgba[4]; } apex_overlay_tint;
typedef struct apex_overlay_list {
    uint32_t struct_size;
    const apex_tool_segment* segments; size_t segment_count;
    const apex_overlay_point* points; size_t point_count;
    const apex_overlay_label* labels; size_t label_count;
    const apex_overlay_ghost* ghosts; size_t ghost_count;
    const apex_overlay_tint* tints; size_t tint_count;
} apex_overlay_list;
typedef struct apex_overlay_v1 {
    uint32_t struct_size;
    apex_status (*declare)(void* host, apex_str id, apex_str name);
    apex_status (*set)(void* host, apex_str id, const apex_overlay_list* list);
    apex_status (*clear)(void* host, apex_str id);
} apex_overlay_v1;

/* apex.helpers/1 (0.9.5). register_helper() is available only during activate: `name` is an FGD helper
   name (for example "mywidget(5)" is registered as "mywidget") that the host does not draw itself;
   built-in helpers cannot be replaced and each name belongs to one plugin. draw() runs for each entity of
   a class that lists the helper, on the editor thread, with apex.document readable. The call and sink
   are borrowed until draw returns. Helpers draw from the saved document, so they do not follow a live
   drag, and they are shown only while the viewport's entity helpers are on. */
typedef struct apex_helper_call {
    uint32_t struct_size;
    apex_node_id entity;
    apex_str class_name, helper, arguments;
    apex_vec3 origin, angles;
} apex_helper_call;
typedef struct apex_helper_sink {
    uint32_t struct_size;
    void* sink;
    apex_status (*segment)(void* sink, const apex_tool_segment* segment);
    apex_status (*label)(void* sink, const apex_overlay_label* label);
} apex_helper_sink;
typedef apex_status (*apex_helper_draw)(void* user, const apex_helper_call* call, const apex_helper_sink* sink);
typedef struct apex_helpers_v1 {
    uint32_t struct_size;
    apex_status (*register_helper)(void* host, apex_str name, void* user, apex_helper_draw draw);
} apex_helpers_v1;

/* apex.problems/1 (0.9.5). register_check() is available only during activate; `id` is namespaced
   plugin.<plugin>.<name> and `name` is the check's title. check() runs on the editor thread whenever the
   Problems list refreshes, with apex.document reading the document being checked, and reports through
   the sink. Problems are listed grouped under the plugin, and "select" selects `objects`. */
typedef struct apex_problem {
    uint32_t struct_size;
    uint32_t severity; /* 0 error, 1 warning, 2 note */
    apex_str text;
    const apex_node_id* objects; size_t object_count;
} apex_problem;
typedef struct apex_problem_sink {
    uint32_t struct_size;
    void* sink;
    apex_status (*report)(void* sink, const apex_problem* problem);
} apex_problem_sink;
typedef apex_status (*apex_problem_check)(void* user, const apex_problem_sink* sink);
typedef struct apex_problems_v1 {
    uint32_t struct_size;
    apex_status (*register_check)(void* host, apex_str id, apex_str name, void* user, apex_problem_check check);
} apex_problems_v1;

/* apex.io/1 (0.9.5b): importers. register_importer() is available only during activate. `id` is
   plugin.<plugin>.<name>; `extensions` is a comma-separated list without dots ("obj,mtl"), matched
   case-insensitively. import() runs on the editor thread, inside ONE edit transaction on the open
   document: whatever it creates through ctx->ops is a single undo step, and a non-OK return (or a
   failed operation) rolls all of it back. `path` is a host file path in UTF-8 (the VFS arrives in
   0.9.6; until then the importer reads the file itself). ctx->world is the NodeId of the world block.
   ctx->report(sink, text) records a message the host shows to the user (the failure reason, or a
   summary). Everything is borrowed until import() returns. */
typedef struct apex_import_context {
    uint32_t struct_size;
    apex_node_id world;
    const apex_edit_ops_v1* ops;
    void* operation;
    void* sink;
    apex_status (*report)(void* sink, apex_str message);
} apex_import_context;
typedef apex_status (*apex_import_callback)(void* user, apex_str path, const apex_import_context* context);
typedef struct apex_io_v1 {
    uint32_t struct_size;
    apex_status (*register_importer)(void* host, apex_str id, apex_str name, apex_str extensions,
                                     void* user, apex_import_callback import);
} apex_io_v1;

/* apex.jobs/1 (0.9.5b): background work with the completion delivered on the editor thread.
   submit() and cancel() are thread-safe (a job may submit jobs). work() runs on a host worker
   thread: it may not call any host interface except submit/cancel and the host's log(), and must not
   touch the document. It should poll ctx->cancelled(ctx->job) during long loops. done() runs on the
   editor thread, exactly once, with work's status (APEX_ERR_INTERNAL if work threw, APEX_ERR_STATE if
   the job was cancelled before it started because the host is shutting down), unless the job was
   cancelled with cancel(): once cancel() returns APEX_OK on the editor thread, done() does not run.
   `user` must stay valid until done() runs or cancel() succeeds. Pending jobs are cancelled and
   running ones joined when the host is destroyed, so work() must honour `cancelled`. */
typedef struct apex_job_context {
    uint32_t struct_size;
    void* job;
    int (*cancelled)(void* job);
} apex_job_context;
typedef apex_status (*apex_job_work)(void* user, const apex_job_context* context);
typedef void (*apex_job_done)(void* user, apex_status status);
typedef struct apex_jobs_v1 {
    uint32_t struct_size;
    apex_status (*submit)(void* host, void* user, apex_job_work work, apex_job_done done, uint64_t* job_id);
    apex_status (*cancel)(void* host, uint64_t job_id); /* APEX_ERR_NOT_FOUND once done() has been delivered */
} apex_jobs_v1;

/* apex.watch/1 (0.9.5b): file watching. watch() is available during activate and in any callback
   into the plugin. `path` is an absolute path to a file or a directory (a directory is watched
   recursively, up to 10000 entries) inside the plugin's own folder or a root the host allows (the game
   directory). It need not exist yet. on_changed(user, path) runs on the editor thread once per
   change burst: a save that touches the file several times, or replaces it, is reported once, after
   the file has been quiet for the host's debounce interval. The host polls, so there is no delivery
   from a thread other than the editor thread. */
typedef void (*apex_watch_callback)(void* user, apex_str path);
typedef struct apex_watch_v1 {
    uint32_t struct_size;
    apex_status (*watch)(void* host, apex_str path, void* user, apex_watch_callback on_changed, uint64_t* watch_id);
    apex_status (*unwatch)(void* host, uint64_t watch_id);
} apex_watch_v1;

typedef struct apex_host {
    uint32_t struct_size, abi_major, abi_minor;
    void* context;
    const void* (*get_interface)(void* context, apex_str name);
    void (*log)(void* context, int level, apex_str message);
    apex_str (*last_error)(void* context);
} apex_host;
typedef struct apex_modified_node { apex_node_id id; uint32_t flags; } apex_modified_node;
typedef struct apex_change_notice {
    uint32_t struct_size;
    uint64_t revision;
    const apex_node_id* created; size_t created_count;
    const apex_modified_node* modified; size_t modified_count;
    const apex_node_id* destroyed; size_t destroyed_count;
    const int32_t* vanished_sides; size_t vanished_side_count;
    uint32_t undo_or_redo;
} apex_change_notice;
typedef struct apex_plugin {
    uint32_t struct_size, abi_major, abi_minor;
    void* user;
    apex_status (*activate)(void* user);
    void (*deactivate)(void* user);
    void (*on_document_opened)(void* user);
    void (*on_document_closing)(void* user);
    void (*on_changed)(void* user, const apex_change_notice* notice);
} apex_plugin;
typedef const apex_plugin* (*apex_plugin_entry_fn)(const apex_host* host);
/* Plugin implementations export this symbol with APEX_PLUGIN_EXPORT. */
const apex_plugin* apex_plugin_entry(const apex_host* host);

#ifdef __cplusplus
}
#endif
#endif
