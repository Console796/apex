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
