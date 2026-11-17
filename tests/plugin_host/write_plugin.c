#include <apex/apex_plugin.h>
#include <string.h>
static const apex_host* host;
static const apex_edit_v1* edit;
static const apex_document_v1* doc;
static const apex_selection_v1* selection;
static int pending;
static apex_str str(const char* s) { apex_str v = {s, strlen(s)}; return v; }
static apex_status write_key(void* user, const apex_edit_ops_v1* ops, void* ctx) {
    apex_node_id root = 0;
    if (doc->root_at(host->context, 0, &root) != APEX_OK) return APEX_ERR_STATE;
    if (ops->set_key(ctx, root, str((const char*)user), str("yes")) != APEX_OK) return APEX_ERR_INVALID;
    return APEX_OK;
}
static apex_status fail_batch(void* user, const apex_edit_ops_v1* ops, void* ctx) {
    apex_node_id root = 0;
    (void)user;
    if (doc->root_at(host->context, 0, &root) != APEX_OK) return APEX_ERR_STATE;
    ops->set_key(ctx, root, str("before_failure"), str("yes"));
    ops->destroy(ctx, UINT64_MAX);
    return APEX_OK;
}
static apex_status rebuild(void* user, const apex_edit_ops_v1* ops, void* ctx) {
    const apex_plane planes[] = {{{1,0,0},80}, {{-1,0,0},0}, {{0,1,0},64},
                                  {{0,-1,0},0}, {{0,0,1},64}, {{0,0,-1},0}};
    apex_node_id root = 0, solid = 0;
    (void)user;
    if (doc->root_at(host->context, 0, &root) != APEX_OK ||
        doc->child_at(host->context, root, 0, &solid) != APEX_OK) return APEX_ERR_INVALID;
    return ops->rebuild_solid(ctx, solid, planes, sizeof(planes)/sizeof(planes[0]));
}
static void deferred(void* user) {
    (void)user;
    host->log(host->context, 1, str("deferred"));
    edit->apply(host->context, str("Deferred"), str(""), "deferred", write_key);
}
static void invoke(void* user) {
    int mode = *(int*)user;
    if (mode == 0) { pending = 1; edit->apply(host->context, str("First"), str(""), "first", write_key); }
    if (mode == 1) {
        apex_status status = edit->apply(host->context, str("Fail"), str(""), 0, fail_batch);
        host->log(host->context, 1, str(status == APEX_ERR_INVALID ? "failed as expected" : "failure missed"));
    }
    if (mode == 2) {
        apex_node_id root = 0; uint64_t selected = 0; uint32_t selected_mode = 0; size_t count = 0;
        doc->root_at(host->context, 0, &root);
        if (selection->set(host->context, 1, &root, 1) == APEX_OK &&
            selection->info(host->context, &selected_mode, &count) == APEX_OK && count == 1 && selected_mode == 1 &&
            selection->at(host->context, 0, &selected) == APEX_OK && selected == root)
            host->log(host->context, 1, str("selection ok"));
    }
    if (mode == 3 && edit->apply(host->context, str("Rebuild"), str(""), 0, rebuild) != APEX_OK)
        host->log(host->context, 1, str("rebuild failed"));
}
static void changed(void* user, const apex_change_notice* notice) {
    apex_node_id root = 0;
    (void)user;
    if (notice->modified_count) {
        doc->root_at(host->context, 0, &root);
        if (notice->modified[0].id != root || notice->modified[0].flags == 0)
            host->log(host->context, 1, str("bad notice"));
        else
            host->log(host->context, 1, str(notice->undo_or_redo ? "undo notice" : "notice"));
    }
    if (pending) { pending = 0; edit->defer(host->context, 0, deferred); }
}
static int modes[] = {0, 1, 2, 3};
static apex_status activate(void* user) {
    const apex_registration_v1* reg;
    (void)user;
    edit = (const apex_edit_v1*)host->get_interface(host->context, str("apex.edit/1"));
    doc = (const apex_document_v1*)host->get_interface(host->context, str("apex.document/1"));
    selection = (const apex_selection_v1*)host->get_interface(host->context, str("apex.selection/1"));
    reg = (const apex_registration_v1*)host->get_interface(host->context, str("apex.registration/1"));
    if (!edit || !doc || !selection || !reg) return APEX_ERR_INTERNAL;
    if (reg->action(host->context, str("plugin.writes.edit"), str("Edit"), str("Edit"), &modes[0], invoke) != APEX_OK) return APEX_ERR_INTERNAL;
    if (reg->action(host->context, str("plugin.writes.fail"), str("Fail"), str("Fail"), &modes[1], invoke) != APEX_OK) return APEX_ERR_INTERNAL;
    if (reg->action(host->context, str("plugin.writes.selection"), str("Selection"), str("Selection"), &modes[2], invoke) != APEX_OK) return APEX_ERR_INTERNAL;
    return reg->action(host->context, str("plugin.writes.rebuild"), str("Rebuild"), str("Rebuild"), &modes[3], invoke);
}
static const apex_plugin plugin = {sizeof(plugin), 1, 0, 0, activate, 0, 0, 0, changed};
APEX_PLUGIN_EXPORT const apex_plugin* apex_plugin_entry(const apex_host* h) { host = h; return &plugin; }
