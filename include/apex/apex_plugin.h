#ifndef APEX_PLUGIN_H
#define APEX_PLUGIN_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APEX_PLUGIN_ABI_MAJOR 1u
#define APEX_PLUGIN_ABI_MINOR 1u
#if defined(_WIN32)
#define APEX_PLUGIN_EXPORT __declspec(dllexport)
#else
#define APEX_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

/* ABI evolution rules (docs/plugins/versioning.md). The ABI is 64-bit only (LP64 on
   Linux and macOS, LLP64 on Windows: every field below is a fixed-width integer, double, size_t or a pointer,
   so the layouts are identical).
   1. Every struct that crosses the boundary except the frozen value types (apex_str, apex_vec3, apex_plane,
      apex_ray, and the scalar arrays of node IDs, side IDs and apex_str) begins with `uint32_t struct_size`,
      set by whoever fills it to sizeof() of the struct in the header it was compiled against.
   2. Structs only grow, by appending fields. A reader uses a field only when struct_size covers it
      (offsetof(T, field) + sizeof(field) <= struct_size); a missing field means "the old behaviour".
   3. No implicit padding anywhere: members are ordered and `reserved` fields written so that every field
      sits at its natural offset with no gaps, and sizeof(T) is the end of the last field. Fields are
      appended in whole units that keep that true (8 bytes for a 64-bit field, pairs of 32-bit fields).
      `reserved` fields are zero; a later minor version may give them a meaning whose zero is the old behaviour.
      tests/plugin_host/abi_layout.inc checks all of this at compile time, in C and in C++.
   4. An array of structs is a contiguous array whose stride is the element's struct_size (every element of an
      array has the same one). Read a host-provided array with apex_array_element(), never with T[i], so a
      plugin built against an older header keeps working when the host's element grows. The host reads a
      plugin-provided array the same way.
   5. A struct embedded by value inside another struct (apex_ui_value in apex_ui_control) is frozen at its
      1.0 layout: new data goes into new fields of the parent.
   6. Out parameters (apex_pick, apex_document_info, apex_node_info, apex_side_info, apex_material_info and the
      apex_tool_visual and apex_tool_readout the host passes to the plugin) carry the caller's struct_size;
      the callee writes only the fields it covers.
   7. A function table grows by appending function pointers; a null or uncovered pointer is "not provided". */
typedef struct apex_str { const char* data; size_t size; } apex_str;
typedef char apex_abi_requires_64_bit_pointers[sizeof(void*) == 8 ? 1 : -1];
/* Element `index` of an array of ABI structs (rule 4): the stride is the first element's struct_size.
   `array` must not be null and `index` must be below the array's count. */
static inline const void* apex_array_element(const void* array, size_t index) {
    return (const char*)array + index * (size_t)*(const uint32_t*)array;
}
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
    uint32_t checked;
    apex_str text;
    double number;
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
    uint32_t reserved;
    const apex_ui_control* controls;
    size_t control_count;
} apex_ui_form;
/* What a host panel passes to a plugin section: kind is "materials" (names), "entities" or "sides" (ids). */
typedef struct apex_ui_context {
    uint32_t struct_size;
    uint32_t reserved;
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
    uint32_t reserved;
    apex_status (*describe)(void* user, const apex_ui_context* context, const apex_ui_form** form);
    void (*on_change)(void* user, apex_str key, const apex_ui_value* value);
    void (*on_click)(void* user, apex_str key);
} apex_ui_callbacks;
typedef struct apex_ui_v1 {
    uint32_t struct_size;
    uint32_t reserved;
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
    uint32_t reserved;
    apex_node_id id, parent;
    apex_str kind;
    size_t child_count, key_count;
} apex_node_info;
/* One side (face) of a solid, as saved: defined after apex_vec3 below. */
typedef struct apex_side_info apex_side_info;
/* Added in ABI 1.1, at the end of the table (check struct_size before use; plugin.hpp's available()):
     side_count   the number of side blocks of a node (0 for anything but a solid).
     side_at      the index-th side of a node, in source order (an out parameter, rule 6).
     side_lookup  the solid and index of a side ID; with duplicate IDs, the first occurrence in the hierarchy. */
typedef struct apex_document_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*info)(void* host, apex_document_info* out);
    apex_status (*root_at)(void* host, size_t index, apex_node_id* out);
    apex_status (*node)(void* host, apex_node_id id, apex_node_info* out);
    apex_status (*child_at)(void* host, apex_node_id id, size_t index, apex_node_id* out);
    apex_status (*key_at)(void* host, apex_node_id id, size_t index, apex_str* key, apex_str* value);
    apex_status (*side_count)(void* host, apex_node_id solid, size_t* out);
    apex_status (*side_at)(void* host, apex_node_id solid, size_t index, apex_side_info* out);
    apex_status (*side_lookup)(void* host, int32_t side, apex_node_id* solid, size_t* index);
} apex_document_v1;

/* Registration is available only during activate; IDs must belong to the plugin. This is the 0.9.1 skeleton:
   apex.actions/1, apex.tools/1 and apex.ui/1 carry the full registrations and supersede it. */
typedef void (*apex_callback)(void* user);
typedef struct apex_registration_v1 {
    uint32_t struct_size;
    uint32_t reserved;
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
/* apex_document_v1.side_at (ABI 1.1). plane is the side's three saved plane points; uaxis/vaxis, shifts and
   scales its texture axes. A value that is missing or does not parse reads as zero; the call still succeeds, so
   one bad side never hides another's material. material is borrowed like key_at's strings. */
struct apex_side_info {
    uint32_t struct_size;
    int32_t id;
    apex_vec3 plane[3];
    apex_str material;
    apex_vec3 uaxis, vaxis;
    double shift_u, shift_v, scale_u, scale_v;
};
/* Arguments of the edit operations added in 0.9.3. Every struct starts with struct_size, and an array of
   them has a stride of its elements' struct_size (rule 4). Pointers must stay valid for the call only. */
/* One face of apex_solid_geometry. `side` is an existing side ID of the solid; `indices` are vertex indices of
   the face loop, counter-clockwise as seen from outside the solid (at least 3). */
typedef struct apex_face_geometry {
    uint32_t struct_size;
    int32_t side;
    apex_vec3 uaxis, vaxis;
    double shift_u, shift_v, scale_u, scale_v;
    const uint32_t* indices;
    size_t index_count;
} apex_face_geometry;
/* A solid's full geometry: a vertex pool and one face per existing side of the solid (each side exactly once).
   The faces' UV axes, shifts and scales are written too. */
typedef struct apex_solid_geometry {
    uint32_t struct_size;
    uint32_t reserved;
    const apex_vec3* vertices; size_t vertex_count;
    const apex_face_geometry* faces; size_t face_count;
} apex_solid_geometry;
/* A displacement (dispinfo) of one side. power is 2..4; with n = (1 << power) + 1, normals, offsets,
   offset_normals and alphas have n*n entries (row-major, as in a VMF) and triangle_tags has 2*(n-1)*(n-1).
   offset_normals may be null (it then equals normals); allowed_verts may be null (count 0). */
typedef struct apex_displacement {
    uint32_t struct_size;
    int32_t power;
    apex_vec3 start_position;
    double elevation;
    int32_t flags;
    int32_t subdiv;
    const apex_vec3* normals;
    const double* distances;
    const apex_vec3* offsets;
    const apex_vec3* offset_normals;
    const double* alphas;
    const int32_t* triangle_tags;
    const uint8_t* allowed_verts;
    size_t allowed_vert_count;
} apex_displacement;
/* One entity output: `output` is the connection's name, the other fields are the VMF's five. */
typedef struct apex_connection {
    uint32_t struct_size;
    uint32_t reserved;
    apex_str output, target, input, parameter, delay, times;
} apex_connection;
/* Face correspondence of rebuild_solid_matched: the face the plane at `plane_index` produces keeps the identity
   (side ID, material, UVs) of the solid's existing side `old_side`. */
typedef struct apex_face_match {
    uint32_t struct_size;
    uint32_t plane_index;
    int32_t old_side;
    uint32_t reserved;
} apex_face_match;
/* What set_editor changes (apex_edit_ops_v1.set_editor). The value is text: a decimal ID for the group and
   visgroup kinds (an empty group value removes the node from its group), "R G B" for the color, "0" or "1"
   for the shown flags, any text for comments. Works on a solid, entity or group. */
enum apex_editor_kind { APEX_EDITOR_GROUP = 0, APEX_EDITOR_ADD_VISGROUP = 1, APEX_EDITOR_REMOVE_VISGROUP = 2,
                        APEX_EDITOR_COLOR = 3, APEX_EDITOR_VISGROUP_SHOWN = 4, APEX_EDITOR_VISGROUP_AUTO_SHOWN = 5,
                        APEX_EDITOR_COMMENTS = 6 };
/* Operations run synchronously within one session transaction. Return a non-OK
   status to roll the entire batch back. Operation context is callback-scoped.
   Every operation that fails (unknown ID, invalid geometry, ...) returns APEX_ERR_INVALID and poisons the
   batch: the host rolls everything back when the callback returns, even if it returns APEX_OK.
   Added in 0.9.3, at the end of the table (check struct_size before use):
     set_solid_geometry  replaces a solid's geometry (and UVs); the side IDs must be the solid's own.
     set_displacement    sets a side's displacement; an invalid one (or a side that is not a quad) is refused.
     set_connections     replaces an entity's connections block (count 0 removes it).
     rebuild_solid_matched  rebuild_solid with a face correspondence, so sides keep identity, material and
                         UVs (and an overlay keeps its face). Matches whose plane was discarded as redundant
                         are ignored; an unknown old_side is refused.
     create_block        creates one block parsed from KV1 text under `parent` (0: at the root). Entities,
                         groups and other containers; "world" and solids are refused (use create_solid).
                         The IDs in the text are replaced with fresh ones. *out receives the new node's ID.
     set_editor          a Hammer editor-block change, see apex_editor_kind. */
typedef struct apex_edit_ops_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*set_key)(void*, apex_node_id, apex_str key, apex_str value);
    apex_status (*destroy)(void*, apex_node_id);
    apex_status (*create_solid)(void*, apex_node_id parent, const apex_plane*, size_t, apex_str material);
    apex_status (*reparent)(void*, apex_node_id, apex_node_id parent, size_t position);
    apex_status (*set_side_key)(void*, int32_t side, apex_str key, apex_str value);
    apex_status (*remove_displacement)(void*, int32_t side);
    apex_status (*rebuild_solid)(void*, apex_node_id, const apex_plane*, size_t);
    apex_status (*set_solid_geometry)(void*, apex_node_id solid, const apex_solid_geometry*);
    apex_status (*set_displacement)(void*, int32_t side, const apex_displacement*);
    apex_status (*set_connections)(void*, apex_node_id entity, const apex_connection*, size_t count);
    apex_status (*rebuild_solid_matched)(void*, apex_node_id, const apex_plane*, size_t,
                                         const apex_face_match*, size_t match_count);
    apex_status (*create_block)(void*, apex_node_id parent, apex_str kv1, apex_node_id* out);
    apex_status (*set_editor)(void*, apex_node_id, uint32_t kind, apex_str value);
} apex_edit_ops_v1;
typedef apex_status (*apex_edit_callback)(void*, const apex_edit_ops_v1*, void* operation_context);
typedef struct apex_edit_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*apply)(void* host, apex_str label, apex_str coalesce_key, void* user, apex_edit_callback);
    /* Queues callback(user) on the open document's deferred list. APEX_ERR_STATE without an editor session and
       document; APEX_ERR_INVALID for a null callback. */
    apex_status (*defer)(void* host, void* user, void (*callback)(void*));
} apex_edit_v1;
/* Selection modes for apex_selection_v1 (the editor's selection granularity); set() rejects others. */
enum apex_selection_mode { APEX_SELECT_GROUPS = 0, APEX_SELECT_OBJECTS = 1, APEX_SELECT_SOLIDS = 2, APEX_SELECT_FACES = 3 };
/* Selection IDs are node IDs except in face mode, where they are side IDs (hence uint64_t, not apex_node_id). */
typedef struct apex_selection_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*info)(void* host, uint32_t* mode, size_t* count);
    apex_status (*at)(void* host, size_t index, uint64_t* id);
    apex_status (*set)(void* host, uint32_t mode, const uint64_t* ids, size_t count);
} apex_selection_v1;
typedef struct apex_ray { apex_vec3 origin, direction; } apex_ray;
enum apex_input_kind { APEX_INPUT_PRESS, APEX_INPUT_RELEASE, APEX_INPUT_MOVE, APEX_INPUT_KEY_DOWN,
                       APEX_INPUT_KEY_UP, APEX_INPUT_LEAVE, APEX_INPUT_CANCEL, APEX_INPUT_CANCEL_POINTER };
enum apex_pointer_button { APEX_BUTTON_NONE, APEX_BUTTON_LEFT, APEX_BUTTON_RIGHT, APEX_BUTTON_MIDDLE };
/* modifiers: bit 0 ctrl, 1 shift, 2 alt. flags: APEX_EVENT_* below. `key` is the canonical key name of a key
   event ("A", "5", "Space", "Escape", "Enter", "Backspace", "BracketLeft", "F5", ...; the names the keybinding
   presets use), without modifiers, and is empty for pointer events and for a bare modifier key. reserved0 is
   zero and keeps the layout free of implicit padding. */
enum apex_event_flags { APEX_EVENT_LEFT_HELD = 1, APEX_EVENT_RIGHT_HELD = 2, APEX_EVENT_MIDDLE_HELD = 4,
                        APEX_EVENT_AUTO_REPEAT = 8, APEX_EVENT_NAVIGATING = 16, APEX_EVENT_MODIFIER_KEY = 32 };
typedef struct apex_tool_event {
    uint32_t struct_size, kind, button, modifiers, flags, reserved0;
    uint64_t source;
    double x, y;
    apex_str key;
} apex_tool_event;
/* Out parameter of ray_pick: the caller sets struct_size to sizeof(apex_pick) before the call. */
typedef struct apex_pick { uint32_t struct_size; int32_t side; apex_node_id node; apex_vec3 point, normal; } apex_pick;
typedef struct apex_tool_view {
    uint32_t struct_size;
    uint32_t orthographic;
    apex_ray ray;
    double width, height;
    apex_vec3 camera_position, camera_angles, camera_forward, camera_up;
    double fov_y_degrees, ortho_height;
    void* context;
    apex_status (*ray_pick)(void*, apex_ray, apex_pick*);
    apex_status (*rect_frustum)(void*, double, double, double, double, apex_plane out[4]);
    apex_status (*world_to_pixel)(void*, apex_vec3, double* x, double* y);
    apex_status (*snap)(void*, apex_ray, apex_vec3*);
} apex_tool_view;
typedef struct apex_tool_segment { uint32_t struct_size; float width; apex_vec3 a, b; float rgba[4]; } apex_tool_segment;
typedef struct apex_tool_ghost { uint32_t struct_size; float opacity; const apex_plane* planes; size_t plane_count; apex_str material; } apex_tool_ghost;
typedef struct apex_tool_visual {
    uint32_t struct_size;
    uint32_t reserved;
    const apex_tool_segment* segments; size_t segment_count;
    const apex_tool_ghost* ghosts; size_t ghost_count;
} apex_tool_visual;
typedef struct apex_tool_parameter {
    uint32_t struct_size, kind;
    apex_str id, label;
    double value, minimum, maximum;
    const apex_str* choices; size_t choice_count;
} apex_tool_parameter;
typedef struct apex_tool_readout {
    uint32_t struct_size;
    uint32_t warn;
    apex_str detail, status;
    const apex_tool_parameter* parameters; size_t parameter_count;
} apex_tool_readout;
typedef struct apex_tool_callbacks {
    uint32_t struct_size;
    uint32_t reserved;
    apex_callback activate, deactivate;
    int (*input)(void*, const apex_tool_event*, const apex_tool_view*);
    apex_status (*visual)(void*, uint64_t source, apex_tool_visual*);
    apex_status (*readout)(void*, apex_tool_readout*);
    int (*set_parameter)(void*, apex_str id, double value);
    int (*perform)(void*, apex_str action);
    apex_callback cancel, document_changed;
    void (*forget)(void*, uint64_t source);
    /* Optional factory and destructor for state owned by each editor session; they are given together (a
       registration with only one is refused with APEX_ERR_INVALID). The host calls create_instance once per
       editor session and passes its result as the first argument of every other callback of that session's tool;
       destroy_instance runs when the session's tool is destroyed. A tool without them, or whose create_instance
       returns null, runs on the registration's `user` pointer, which every session shares; the host logs one
       warning when a second session gets such a tool. */
    void* (*create_instance)(void* registration_user);
    void (*destroy_instance)(void* instance);
} apex_tool_callbacks;
/* Registration ids are "plugin.<plugin id>.<name>": the plugin id is dot-separated parts of letters, digits,
   '_' and '-' (the manifest's id), and <name> is one such part with no dot. The host matches the prefix up to
   the dot after the plugin id, so com.example cannot register under com.example.paint. A malformed id (empty
   name, trailing dot, other characters) or one outside the plugin's namespace fails with APEX_ERR_INVALID.
   Gesture ownership: the host treats a pointer press that input() returned non-zero for as owned by the tool
   until the buttons are up or the gesture is cancelled; there is no plugin-declared ownership beyond that. */
typedef struct apex_tools_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*register_tool)(void*, apex_str id, apex_str name, apex_str description,
                                 void* user, const apex_tool_callbacks* callbacks);
} apex_tools_v1;
/* Empty tool_id means the global context. */
typedef struct apex_action_suggestion { uint32_t struct_size, reserved; apex_str preset, chord, tool_id; } apex_action_suggestion;
typedef struct apex_actions_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*register_action)(void*, apex_str id, apex_str name, apex_str description,
                                   void* user, apex_callback invoke,
                                   const apex_action_suggestion* suggestions, size_t suggestion_count);
} apex_actions_v1;

/* apex.overlay/1 (0.9.5). Overlay lists are world primitives drawn in every display mode. declare() is
   available only during activate and creates the per-viewport toggle named `name`; set() and clear()
   need the declaring plugin and are available during activate and any callback into that plugin.
   The host copies everything in a list before set() returns. Segment width is in pixels. Ghost
   planes use the apex.tools ghost convention (a convex solid). Tints colour whole faces by side ID. */
typedef struct apex_overlay_point { uint32_t struct_size; float size; apex_vec3 position; float rgba[4]; } apex_overlay_point;
typedef struct apex_overlay_label { uint32_t struct_size, reserved; apex_vec3 position; apex_str text; float rgba[4]; } apex_overlay_label;
typedef struct apex_overlay_ghost { uint32_t struct_size, reserved; const apex_plane* planes; size_t plane_count; float rgba[4]; } apex_overlay_ghost;
typedef struct apex_overlay_tint { uint32_t struct_size; int32_t side; float rgba[4]; } apex_overlay_tint;
typedef struct apex_overlay_list {
    uint32_t struct_size;
    uint32_t reserved;
    const apex_tool_segment* segments; size_t segment_count;
    const apex_overlay_point* points; size_t point_count;
    const apex_overlay_label* labels; size_t label_count;
    const apex_overlay_ghost* ghosts; size_t ghost_count;
    const apex_overlay_tint* tints; size_t tint_count;
} apex_overlay_list;
typedef struct apex_overlay_v1 {
    uint32_t struct_size;
    uint32_t reserved;
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
    uint32_t reserved;
    apex_node_id entity;
    apex_str class_name, helper, arguments;
    apex_vec3 origin, angles;
} apex_helper_call;
typedef struct apex_helper_sink {
    uint32_t struct_size;
    uint32_t reserved;
    void* sink;
    apex_status (*segment)(void* sink, const apex_tool_segment* segment);
    apex_status (*label)(void* sink, const apex_overlay_label* label);
} apex_helper_sink;
typedef apex_status (*apex_helper_draw)(void* user, const apex_helper_call* call, const apex_helper_sink* sink);
typedef struct apex_helpers_v1 {
    uint32_t struct_size;
    uint32_t reserved;
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
    uint32_t reserved;
    void* sink;
    apex_status (*report)(void* sink, const apex_problem* problem);
} apex_problem_sink;
typedef apex_status (*apex_problem_check)(void* user, const apex_problem_sink* sink);
typedef struct apex_problems_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*register_check)(void* host, apex_str id, apex_str name, void* user, apex_problem_check check);
} apex_problems_v1;

/* apex.io/1 (0.9.5b): importers. register_importer() is available only during activate. `id` is
   plugin.<plugin>.<name>; `extensions` is a comma-separated list without dots ("obj,mtl"), matched
   case-insensitively. import() runs on the editor thread, inside ONE edit transaction on the open
   document: whatever it creates through ctx->ops is a single undo step, and a non-OK return (or a
   failed operation) rolls all of it back. `path` is a host file path in UTF-8 (an arbitrary file the
   user picked, not a VFS path: the importer reads it itself). ctx->world is the NodeId of the world block.
   ctx->report(sink, text) records a message the host shows to the user (the failure reason, or a
   summary). Everything is borrowed until import() returns. */
typedef struct apex_import_context {
    uint32_t struct_size;
    uint32_t reserved;
    apex_node_id world;
    const apex_edit_ops_v1* ops;
    void* operation;
    void* sink;
    apex_status (*report)(void* sink, apex_str message);
} apex_import_context;
typedef apex_status (*apex_import_callback)(void* user, apex_str path, const apex_import_context* context);
typedef struct apex_io_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*register_importer)(void* host, apex_str id, apex_str name, apex_str extensions,
                                     void* user, apex_import_callback import);
} apex_io_v1;

/* apex.jobs/1 (0.9.5b): background work with the completion delivered on the editor thread.
   submit() and cancel() are thread-safe (a job may submit jobs). work() runs on a host worker
   thread: it may not call any host interface except submit/cancel and the host's log(), and must not
   touch the document. It should poll ctx->cancelled(ctx->job) during long loops. done() runs on the
   editor thread, exactly once, with work's status (APEX_ERR_INTERNAL if work threw), unless the job was
   cancelled with cancel(): once cancel() returns APEX_OK on the editor thread, done() does not run.
   Jobs still pending when the host shuts down are discarded without calling done().
   `user` must stay valid until done() runs or cancel() succeeds. Pending jobs are cancelled and
   running ones joined when the host is destroyed, so work() must honour `cancelled`. */
typedef struct apex_job_context {
    uint32_t struct_size;
    uint32_t reserved;
    void* job;
    int (*cancelled)(void* job);
} apex_job_context;
typedef apex_status (*apex_job_work)(void* user, const apex_job_context* context);
typedef void (*apex_job_done)(void* user, apex_status status);
typedef struct apex_jobs_v1 {
    uint32_t struct_size;
    uint32_t reserved;
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
    uint32_t reserved;
    apex_status (*watch)(void* host, apex_str path, void* user, apex_watch_callback on_changed, uint64_t* watch_id);
    apex_status (*unwatch)(void* host, uint64_t watch_id);
} apex_watch_v1;

/* apex.vfs/1 (0.9.6): a plugin serves files as a virtual mount. register_provider() is available only
   during activate. `id` is plugin.<plugin>.<name>; `name` is the mount's display name (the log and the
   compile warning use it). The mount sits where `position` says (NULL means last): kind 0 first, 1 last,
   2 before and 3 after the first profile mount whose directory or archive path contains `anchor`
   (case-insensitive; no match mounts it last and the host logs that). Earlier mounts win, so a first
   provider overrides the game and a last one only fills gaps.
   The host asks list() once when a game loads and again on every refresh, keeps the answer as the
   mount's index, and answers exists/list/walk from it: a directory walk never calls the plugin. Only
   open() is called per file. list() adds relative paths with '/' separators through sink->add (up to
   200000 paths of at most 1024 bytes each; the rest are dropped). open() writes the file's bytes through
   sink->write, in one or more calls, and writes at most sink->max_bytes (more is ignored); a non-OK
   return means the file is missing or unreadable (APEX_ERR_NOT_FOUND for a missing one).
   Threads: refresh() and list() run on whichever thread loads or refreshes the game (a host worker, not
   necessarily the editor thread); open() runs on host workers, several at once. The host never runs
   refresh() or list() at the same time as an open() of the same provider, and it serialises refresh(),
   but several open() calls overlap, so open() must be thread-safe. Providers may not call any host
   interface from these callbacks except the host's log(). They must return within the host's timeout
   (5 s by default): a call that does not return, or an exception, fails that call and the file is
   reported missing; a call that times out takes the provider out of service until it returns and the
   game reloads. `user` and everything the provider points at must stay valid until the host is destroyed.
   request_refresh() asks the host to reload the game's files because the provider's content changed; it
   is available on the editor thread (for example from a watch or job callback), and `id` names the
   provider. The reload is asynchronous. */
typedef struct apex_vfs_file_sink {
    uint32_t struct_size;
    uint32_t reserved;
    void* sink;
    apex_status (*add)(void* sink, apex_str path);
} apex_vfs_file_sink;
typedef struct apex_vfs_data_sink {
    uint32_t struct_size;
    uint32_t reserved;
    void* sink;
    size_t max_bytes;
    apex_status (*write)(void* sink, const void* data, size_t size);
} apex_vfs_data_sink;
typedef struct apex_vfs_provider {
    uint32_t struct_size;
    uint32_t reserved;
    void* user;
    apex_status (*refresh)(void* user); /* optional: prepare for a new listing */
    apex_status (*list)(void* user, const apex_vfs_file_sink* sink);
    apex_status (*open)(void* user, apex_str path, const apex_vfs_data_sink* sink);
} apex_vfs_provider;
typedef struct apex_vfs_position {
    uint32_t struct_size;
    uint32_t kind; /* 0 first, 1 last, 2 before anchor, 3 after anchor */
    apex_str anchor;
} apex_vfs_position;
typedef struct apex_vfs_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*register_provider)(void* host, apex_str id, apex_str name, const apex_vfs_provider* provider,
                                     const apex_vfs_position* position);
    apex_status (*request_refresh)(void* host, apex_str id);
} apex_vfs_v1;

/* apex.assets/1 (0.9.6): the game's material index and VMT write-back. Both are editor-thread only.
   list_materials() calls `visit` once per indexed material (the winning file of each name, as the
   material browser sees it) until it returns non-zero; the strings are borrowed for the callback. The
   index may still be loading: it reports what is indexed so far, and the plugin should list again after
   a change it asked for. find_material() fills `out` for one name (path under materials/, without
   extension, any case); its strings stay valid until the next call to the host.
   write_vmt() edits the material's VMT: each edit sets `key` to `value`, or removes the key when
   `remove` is non-zero (a new key goes at the end of the shader block; matching is case-insensitive).
   Everything else in the file (comments, spacing, order, line endings) is kept. Only a loose file can
   be written: a material that is served from an archive or from a plugin returns APEX_ERR_UNSUPPORTED.
   The write is atomic (a temporary file replaces the VMT), then the host re-indexes that material, so
   find_material() and the browser list show the new values right after the call returns (thumbnails and
   the 3D view keep their old image of the material until the content next reloads). No document is
   needed. All calls are on the editor thread. last_error() says why a call failed. */
typedef struct apex_material_info {
    uint32_t struct_size;
    uint32_t reserved;
    apex_str name, shader, base_texture, surface_prop, source; /* source: "game", the addon's name, or "provider:<name>" */
} apex_material_info;
typedef struct apex_vmt_edit {
    uint32_t struct_size;
    uint32_t remove;
    apex_str key, value;
} apex_vmt_edit;
typedef int (*apex_material_visit)(void* user, const apex_material_info* info);
typedef struct apex_assets_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*list_materials)(void* host, void* user, apex_material_visit visit);
    apex_status (*find_material)(void* host, apex_str name, apex_material_info* out);
    apex_status (*write_vmt)(void* host, apex_str material, const apex_vmt_edit* edits, size_t edit_count);
} apex_assets_v1;

/* Log levels for apex_host.log. The 0.9 host writes every message to its log and ignores the level; a later
   minor may route by it, so pass the true level. */
enum apex_log_level { APEX_LOG_DEBUG = 0, APEX_LOG_INFO = 1, APEX_LOG_WARNING = 2, APEX_LOG_ERROR = 3 };
/* Problem severities (apex_problem.severity) and apex_vfs_position kinds, named for the numbers used above. */
enum apex_problem_severity { APEX_PROBLEM_ERROR = 0, APEX_PROBLEM_WARNING = 1, APEX_PROBLEM_NOTE = 2 };
enum apex_vfs_position_kind { APEX_VFS_FIRST = 0, APEX_VFS_LAST = 1, APEX_VFS_BEFORE = 2, APEX_VFS_AFTER = 3 };
/* Tool parameter kinds (apex_tool_parameter.kind) and apex_modified_node.flags bits (the document's dirty bits). */
enum apex_parameter_kind { APEX_PARAM_CHOICE = 0, APEX_PARAM_INTEGER = 1, APEX_PARAM_NUMBER = 2 };
enum apex_modified_flags { APEX_MODIFIED_GEOMETRY = 1, APEX_MODIFIED_MATERIAL = 2, APEX_MODIFIED_KEY_VALUES = 4,
    APEX_MODIFIED_VISIBILITY = 8, APEX_MODIFIED_HIERARCHY = 16 };

/* The host passed to apex_plugin_entry and the interface tables it returns outlive the plugin (until unload);
   host->context is valid for every callback until deactivate returns. */
typedef struct apex_host {
    uint32_t struct_size, abi_major, abi_minor, reserved;
    void* context;
    const void* (*get_interface)(void* context, apex_str name);
    void (*log)(void* context, int level, apex_str message);
    apex_str (*last_error)(void* context);
} apex_host;
typedef struct apex_modified_node { uint32_t struct_size; uint32_t flags; apex_node_id id; } apex_modified_node;
typedef struct apex_change_notice {
    uint32_t struct_size;
    uint32_t undo_or_redo;
    uint64_t revision;
    const apex_node_id* created; size_t created_count;
    const apex_modified_node* modified; size_t modified_count;
    const apex_node_id* destroyed; size_t destroyed_count;
    const int32_t* vanished_sides; size_t vanished_side_count;
} apex_change_notice;
typedef struct apex_plugin {
    uint32_t struct_size, abi_major, abi_minor, reserved;
    void* user;
    apex_status (*activate)(void* user);
    void (*deactivate)(void* user);
    void (*on_document_opened)(void* user);
    void (*on_document_closing)(void* user);
    void (*on_changed)(void* user, const apex_change_notice* notice);
} apex_plugin;
typedef const apex_plugin* (*apex_plugin_entry_fn)(const apex_host* host);
/* Plugin implementations export this symbol with APEX_PLUGIN_EXPORT. The returned table is owned by the plugin
   and must stay valid until the host unloads the library, after deactivate. Return NULL to refuse to load. */
const apex_plugin* apex_plugin_entry(const apex_host* host);

#ifdef __cplusplus
}
#endif
#endif
