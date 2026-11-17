#include <apex/apex_plugin.h>
#include <string.h>
static const apex_host* host;
static apex_str name(const char* text) { apex_str out = { text, strlen(text) }; return out; }
static apex_status activate(void* user) {
    const apex_document_v1* doc = (const apex_document_v1*)host->get_interface(host->context, name("apex.document/1"));
    apex_document_info info = { sizeof(info), 0, 0, 0 };
    apex_node_info root = { sizeof(root), 0, 0, 0, {0, 0}, 0, 0 };
    apex_node_info child = { sizeof(child), 0, 0, 0, {0, 0}, 0, 0 };
    apex_node_id root_id = 0, child_id = 0;
    size_t i, messages = 0;
    (void)user;
    if (!doc || doc->info(host->context, &info) != APEX_OK || !info.is_open || info.root_count != 1 ||
        doc->root_at(host->context, 0, &root_id) != APEX_OK ||
        doc->node(host->context, root_id, &root) != APEX_OK || root.child_count != 1 ||
        doc->child_at(host->context, root_id, 0, &child_id) != APEX_OK ||
        doc->node(host->context, child_id, &child) != APEX_OK || child.parent != root_id ||
        doc->root_at(host->context, 1, &child_id) != APEX_ERR_NOT_FOUND) return APEX_ERR_INTERNAL;
    for (i = 0; i < root.key_count; ++i) {
        apex_str key, value;
        if (doc->key_at(host->context, root_id, i, &key, &value) != APEX_OK) return APEX_ERR_INTERNAL;
        if (key.size == 7 && memcmp(key.data, "message", 7) == 0) {
            const char* expected = messages++ ? "two" : "one";
            if (value.size != 3 || memcmp(value.data, expected, 3) != 0) return APEX_ERR_INTERNAL;
        }
    }
    return messages == 2 ? APEX_OK : APEX_ERR_INTERNAL;
}
static const apex_plugin plugin = { sizeof(apex_plugin), 1, 0, 0, 0, activate, 0, 0, 0 };
APEX_PLUGIN_EXPORT const apex_plugin* apex_plugin_entry(const apex_host* h) { host = h; return &plugin; }
