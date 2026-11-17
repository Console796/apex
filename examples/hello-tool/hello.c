#include <apex/apex_plugin.h>
#include <string.h>
static const apex_host* host;
static apex_str str(const char* text) { apex_str s = { text, strlen(text) }; return s; }
static void read_document(void* user) {
    const apex_document_v1* doc = (const apex_document_v1*)host->get_interface(host->context, str("apex.document/1"));
    apex_document_info info = { sizeof(info), 0, 0, 0 };
    apex_node_id id;
    apex_node_info node = { sizeof(node), 0, 0, 0, {0, 0}, 0, 0 };
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
/* Preview-only marker: writing entities requires apex.edit/1 in 0.9.3. */
static apex_vec3 marker;
static uint64_t marker_source;
static int has_marker;
static double marker_size = 16;
static apex_tool_segment segments[2];
static apex_tool_parameter parameter;
static int tool_input(void* user, const apex_tool_event* event, const apex_tool_view* view) {
    double t;
    (void)user;
    if (event->kind == APEX_INPUT_KEY_DOWN && event->key.size == 6 && memcmp(event->key.data, "Escape", 6) == 0) {
        has_marker = 0; return 1;
    }
    if (event->kind != APEX_INPUT_PRESS || event->button != APEX_BUTTON_LEFT || view->ray.direction.z == 0) return 0;
    t = -view->ray.origin.z / view->ray.direction.z;
    if (t < 0) return 0;
    marker.x = view->ray.origin.x + t * view->ray.direction.x;
    marker.y = view->ray.origin.y + t * view->ray.direction.y;
    marker.z = 0; marker_source = event->source; has_marker = 1;
    return 1;
}
static apex_status tool_visual(void* user, uint64_t source, apex_tool_visual* out) {
    (void)user;
    out->segments = 0; out->segment_count = 0; out->ghosts = 0; out->ghost_count = 0;
    if (has_marker && source == marker_source) {
        const float color[4] = {1, 0.7f, 0.2f, 1};
        segments[0].a = (apex_vec3){marker.x - marker_size, marker.y, marker.z};
        segments[0].b = (apex_vec3){marker.x + marker_size, marker.y, marker.z};
        segments[1].a = (apex_vec3){marker.x, marker.y - marker_size, marker.z};
        segments[1].b = (apex_vec3){marker.x, marker.y + marker_size, marker.z};
        memcpy(segments[0].rgba, color, sizeof(color)); memcpy(segments[1].rgba, color, sizeof(color));
        segments[0].struct_size = segments[1].struct_size = sizeof(apex_tool_segment);  /* every array element has its own */
        segments[0].width = segments[1].width = 2;
        out->segments = segments; out->segment_count = 2;
    }
    return APEX_OK;
}
static apex_status tool_readout(void* user, apex_tool_readout* out) {
    (void)user;
    parameter = (apex_tool_parameter){sizeof(apex_tool_parameter), 2, str("size"), str("Marker size"), marker_size, 1, 128, 0, 0};
    out->status = str("Click to preview a point; writing requires apex.edit/1");
    out->parameters = &parameter; out->parameter_count = 1;
    return APEX_OK;
}
static int tool_parameter(void* user, apex_str id, double value) {
    (void)user;
    if (id.size != 4 || memcmp(id.data, "size", 4) != 0 || value < 1 || value > 128) return 0;
    marker_size = value; return 1;
}
static void tool_cancel(void* user) { (void)user; has_marker = 0; }
static void tool_forget(void* user, uint64_t source) { (void)user; if (source == marker_source) has_marker = 0; }
static const apex_tool_callbacks tool = {sizeof(tool), 0, 0, 0, tool_input, tool_visual, tool_readout,
                                          tool_parameter, 0, tool_cancel, tool_cancel, tool_forget};
static apex_status activate(void* user) {
    host->log(host->context, 1, str("Hello from hello-tool"));
    const apex_tools_v1* tools;
    const apex_actions_v1* actions;
    apex_action_suggestion suggestion;
    apex_status status;
    read_document(user);
    tools = (const apex_tools_v1*)host->get_interface(host->context, str("apex.tools/1"));
    actions = (const apex_actions_v1*)host->get_interface(host->context, str("apex.actions/1"));
    if (!tools || !actions) return APEX_ERR_UNSUPPORTED;
    status = tools->register_tool(host->context, str("plugin.hello.tool"), str("Hello tool"),
                                  str("Preview a point marker"), 0, &tool);
    if (status != APEX_OK) return status;
    suggestion = (apex_action_suggestion){sizeof(apex_action_suggestion), 0, str("Hammer"), str("F24"), str("plugin.hello.tool")};
    return actions->register_action(host->context, str("plugin.hello.read_document"), str("Read document"),
                                    str("Log the root class name"), 0, read_document, &suggestion, 1);
}
static const apex_plugin plugin = { sizeof(apex_plugin), APEX_PLUGIN_ABI_MAJOR, APEX_PLUGIN_ABI_MINOR, 0, 0, activate, 0, read_document, 0 };
APEX_PLUGIN_EXPORT const apex_plugin* apex_plugin_entry(const apex_host* h) { host = h; return &plugin; }
