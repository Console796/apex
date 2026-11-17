#include <apex/apex_plugin.h>
#include <string.h>
static const apex_host* host;
static apex_str str(const char* text) { apex_str s = { text, strlen(text) }; return s; }
static void read_document(void* user) {
    const apex_document_v1* doc = (const apex_document_v1*)host->get_interface(host->context, str("apex.document/1"));
    apex_document_info info = { sizeof(info), 0, 0, 0 };
    apex_node_id id;
    apex_node_info node = { sizeof(node), 0, 0, {0, 0}, 0, 0 };
    size_t i;
    (void)user;
    if (!doc || doc->info(host->context, &info) != APEX_OK || !info.is_open || !info.root_count ||
        doc->root_at(host->context, 0, &id) != APEX_OK || doc->node(host->context, id, &node) != APEX_OK) return;
    for (i = 0; i < node.key_count; ++i) {
        apex_str key, value;
        if (doc->key_at(host->context, id, i, &key, &value) == APEX_OK && key.size == 9 && memcmp(key.data, "classname", 9) == 0) {
            host->log(host->context, 1, value);
            return;
        }
    }
}
static apex_status activate(void* user) {
    host->log(host->context, 1, str("Hello from hello-tool"));
    read_document(user);
    return APEX_OK;
}
static const apex_plugin plugin = { sizeof(apex_plugin), APEX_PLUGIN_ABI_MAJOR, APEX_PLUGIN_ABI_MINOR, 0, activate, 0, read_document, 0 };
APEX_PLUGIN_EXPORT const apex_plugin* apex_plugin_entry(const apex_host* h) { host = h; return &plugin; }
