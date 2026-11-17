#include <apex/apex_plugin.h>
#include <cstring>
#include <limits>
static const apex_host* host;
static apex_str str(const char* s) { return {s, std::strlen(s)}; }
static void action(void*) {}
static apex_status emptyEdit(void*, const apex_edit_ops_v1*, void*) { return APEX_OK; }
static apex_status failingEdit(void*, const apex_edit_ops_v1* ops, void* context) {
    const apex_str huge{"x", std::numeric_limits<size_t>::max()};
    if (ops->set_key(context, 1, str("before_failure"), str("temporary")) != APEX_OK) return APEX_ERR_INVALID;
    if (ops->set_key(context, 1, str("oversize"), huge) != APEX_ERR_INTERNAL) return APEX_ERR_INVALID;
    // Ignoring a failed operation must still roll back the edit.
    return APEX_OK;
}
static apex_status activate(void*) {
    auto* reg = static_cast<const apex_registration_v1*>(host->get_interface(host->context, str("apex.registration/1")));
    auto* actions = static_cast<const apex_actions_v1*>(host->get_interface(host->context, str("apex.actions/1")));
    const apex_str huge{"x", std::numeric_limits<size_t>::max()};
    // Each failure must be contained here, while the plugin frame is still running.
    if (reg->action(host->context, str("plugin.exceptions.basic"), huge, {}, nullptr, action) != APEX_ERR_INTERNAL) return APEX_ERR_INVALID;
    if (!host->last_error(host->context).size) return APEX_ERR_INVALID;
    apex_action_suggestion suggestion{sizeof(apex_action_suggestion), 0, huge, str("F24"), {}};
    if (actions->register_action(host->context, str("plugin.exceptions.suggest"), str("Suggest"), {}, nullptr, action, &suggestion, 1) != APEX_ERR_INTERNAL) return APEX_ERR_INVALID;
    if (!host->last_error(host->context).size) return APEX_ERR_INVALID;
    auto* edit = static_cast<const apex_edit_v1*>(host->get_interface(host->context, str("apex.edit/1")));
    if (edit->apply(host->context, huge, {}, nullptr, emptyEdit) != APEX_ERR_INTERNAL) return APEX_ERR_INVALID;
    if (edit->apply(host->context, str("Fail"), {}, nullptr, failingEdit) != APEX_ERR_INTERNAL) return APEX_ERR_INVALID;
    host->log(host->context, 1, str("throwing logger"));
    if (!host->last_error(host->context).size) return APEX_ERR_INVALID;
    // Retry the exact IDs: exceptions must not leave partial pending entries.
    if (reg->action(host->context, str("plugin.exceptions.basic"), str("Basic"), {}, nullptr, action) != APEX_OK) return APEX_ERR_INVALID;
    return reg->action(host->context, str("plugin.exceptions.suggest"), str("Suggest"), {}, nullptr, action);
}
static const apex_plugin plugin{sizeof(apex_plugin), 1, 0, 0, nullptr, activate};
APEX_PLUGIN_EXPORT const apex_plugin* apex_plugin_entry(const apex_host* h) { host = h; return &plugin; }
