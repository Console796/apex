#include <apex/apex_plugin.h>
#include <string.h>
static const apex_host* host;
static const apex_registration_v1* registry;
static apex_str str(const char* text) { apex_str s = {text, strlen(text)}; return s; }
static void invoke(void* user) {
    (void)user;
    if (registry->action(host->context, str("plugin.example.late"), str("Late"), str("Late"), 0, invoke) != APEX_ERR_STATE) {
        host->log(host->context, 1, str("late registration accepted"));
        return;
    }
    host->log(host->context, 1, str("action called"));
}
static void tool_on(void* user) { (void)user; host->log(host->context, 1, str("tool activated")); }
static apex_status activate(void* user) {
    const apex_registration_v1* reg = (const apex_registration_v1*)host->get_interface(host->context, str("apex.registration/1"));
    (void)user;
    registry = reg;
    if (!reg || reg->action(host->context, str("plugin.other.action"), str("Bad"), str("Bad"), 0, invoke) != APEX_ERR_INVALID)
        return APEX_ERR_INTERNAL;
    if (reg->action(host->context, str("plugin.example.action"), str("Example action"), str("Example"), 0, invoke) != APEX_OK)
        return APEX_ERR_INTERNAL;
    if (reg->tool(host->context, str("plugin.other.tool"), str("Bad"), str("Bad"), 0, tool_on, 0) != APEX_ERR_INVALID)
        return APEX_ERR_INTERNAL;
    if (reg->tool(host->context, str("plugin.example.tool"), str("Example tool"), str("Example"), 0, tool_on, 0) != APEX_OK)
        return APEX_ERR_INTERNAL;
    if (reg->panel(host->context, str("plugin.example.panel"), str("Example panel")) != APEX_OK)
        return APEX_ERR_INTERNAL;
    return APEX_OK;
}
static const apex_plugin plugin = { sizeof(apex_plugin), 1, 0, 0, activate, 0, 0, 0 };
APEX_PLUGIN_EXPORT const apex_plugin* apex_plugin_entry(const apex_host* h) { host = h; return &plugin; }
