/* Fixture for I-6 (per-session tool state). Three tools:
   plugin.toolstate.shared  a legacy tool (no create_instance): its counter is the registration's user pointer.
   plugin.toolstate.own     keeps its counter in a per-session instance (create_instance / destroy_instance).
   plugin.toolstate.half    gives only create_instance, which the host must refuse; activate logs the status.
   perform() increments the counter and readout() reports it as "count=N" (own: "own=N"). */
#include <apex/apex_plugin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const apex_host* host;
static int shared_counter;
static int live_instances;
static int created_instances;

static apex_str str(const char* s) { apex_str v = {s, strlen(s)}; return v; }
static char status_text[32];

static int shared_perform(void* user, apex_str action) { (void)action; ++*(int*)user; return 1; }
static apex_status shared_readout(void* user, apex_tool_readout* out) {
    snprintf(status_text, sizeof(status_text), "count=%d", *(int*)user);
    out->status.data = status_text; out->status.size = strlen(status_text);
    return APEX_OK;
}
static void* own_create(void* registration_user) { (void)registration_user; ++live_instances; ++created_instances; return calloc(1, sizeof(int)); }
static void own_destroy(void* instance) { --live_instances; free(instance); }
static int own_perform(void* instance, apex_str action) { (void)action; ++*(int*)instance; return 1; }
static apex_status own_readout(void* instance, apex_tool_readout* out) {
    snprintf(status_text, sizeof(status_text), "own=%d live=%d created=%d", *(int*)instance, live_instances, created_instances);
    out->status.data = status_text; out->status.size = strlen(status_text);
    return APEX_OK;
}
static void* half_create(void* registration_user) { return registration_user; }

static apex_status activate(void* user) {
    const apex_tools_v1* tools;
    apex_tool_callbacks callbacks;
    apex_status status;
    (void)user;
    tools = (const apex_tools_v1*)host->get_interface(host->context, str("apex.tools/1"));
    if (!tools) return APEX_ERR_INTERNAL;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.struct_size = sizeof(callbacks);
    callbacks.perform = shared_perform; callbacks.readout = shared_readout;
    if (tools->register_tool(host->context, str("plugin.toolstate.shared"), str("Shared"), str("legacy tool"), &shared_counter, &callbacks) != APEX_OK)
        return APEX_ERR_INTERNAL;

    callbacks.perform = own_perform; callbacks.readout = own_readout;
    callbacks.create_instance = own_create; callbacks.destroy_instance = own_destroy;
    if (tools->register_tool(host->context, str("plugin.toolstate.own"), str("Own"), str("per-session tool"), &shared_counter, &callbacks) != APEX_OK)
        return APEX_ERR_INTERNAL;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.struct_size = sizeof(callbacks);
    callbacks.create_instance = half_create;
    status = tools->register_tool(host->context, str("plugin.toolstate.half"), str("Half"), str("unpaired"), &shared_counter, &callbacks);
    host->log(host->context, 1, str(status == APEX_ERR_INVALID ? "half refused" : "half accepted"));
    return APEX_OK;
}
static const apex_plugin plugin = {sizeof(plugin), 1, 0, 0, 0, activate, 0, 0, 0, 0};
APEX_PLUGIN_EXPORT const apex_plugin* apex_plugin_entry(const apex_host* h) { host = h; return &plugin; }
