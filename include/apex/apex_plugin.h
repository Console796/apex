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

typedef struct apex_host {
    uint32_t struct_size, abi_major, abi_minor;
    void* context;
    const void* (*get_interface)(void* context, apex_str name);
    void (*log)(void* context, int level, apex_str message);
    apex_str (*last_error)(void* context);
} apex_host;
typedef struct apex_plugin {
    uint32_t struct_size, abi_major, abi_minor;
    void* user;
    apex_status (*activate)(void* user);
    void (*deactivate)(void* user);
    void (*on_document_opened)(void* user);
    void (*on_document_closing)(void* user);
} apex_plugin;
typedef const apex_plugin* (*apex_plugin_entry_fn)(const apex_host* host);
/* Plugin implementations export this symbol with APEX_PLUGIN_EXPORT. */
const apex_plugin* apex_plugin_entry(const apex_host* host);

#ifdef __cplusplus
}
#endif
#endif
