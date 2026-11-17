/* Reads brush sides through apex.document/1 (ABI 1.1: side_count, side_at, side_lookup) and logs what it
   finds, one line per check, for plugin_host_test.cpp. */
#include <apex/apex_plugin.h>
#include <stdio.h>
#include <string.h>
static const apex_host* host;
static apex_str name(const char* text) { apex_str out = { text, strlen(text) }; return out; }
static void say(const char* text) { host->log(host->context, 1, name(text)); }
static apex_status activate(void* user) {
    const apex_document_v1* doc = (const apex_document_v1*)host->get_interface(host->context, name("apex.document/1"));
    apex_node_id world = 0, solid = 0, found = 0;
    size_t count = 0, i, index = 99;
    apex_side_info side;
    char line[256];
    (void)user;
    if (!doc) return APEX_ERR_INTERNAL;
    if (doc->struct_size < offsetof(apex_document_v1, side_lookup) + sizeof(doc->side_lookup)) { say("no sides"); return APEX_OK; }
    if (doc->root_at(host->context, 0, &world) != APEX_OK || doc->child_at(host->context, world, 0, &solid) != APEX_OK)
        return APEX_ERR_INTERNAL;
    if (doc->side_count(host->context, world, &count) == APEX_OK) { snprintf(line, sizeof line, "world sides %zu", count); say(line); }
    if (doc->side_count(host->context, solid, &count) != APEX_OK) return APEX_ERR_INTERNAL;
    snprintf(line, sizeof line, "solid sides %zu", count); say(line);
    for (i = 0; i < count; ++i) {
        memset(&side, 0, sizeof side); side.struct_size = sizeof side;
        if (doc->side_at(host->context, solid, i, &side) != APEX_OK) return APEX_ERR_INTERNAL;
        snprintf(line, sizeof line, "side %d %.*s p1=%g,%g,%g u=%g,%g,%g/%g/%g v=%g,%g,%g/%g/%g", side.id,
                 (int)side.material.size, side.material.data, side.plane[1].x, side.plane[1].y, side.plane[1].z,
                 side.uaxis.x, side.uaxis.y, side.uaxis.z, side.shift_u, side.scale_u,
                 side.vaxis.x, side.vaxis.y, side.vaxis.z, side.shift_v, side.scale_v);
        say(line);
    }
    memset(&side, 0, sizeof side); side.struct_size = sizeof side;
    if (doc->side_at(host->context, solid, count, &side) == APEX_ERR_NOT_FOUND) say("past the end: not found");
    if (doc->side_at(host->context, world, 0, &side) == APEX_ERR_NOT_FOUND) say("world side: not found");
    side.struct_size = (uint32_t)offsetof(apex_side_info, material);
    if (doc->side_at(host->context, solid, 0, &side) == APEX_ERR_INVALID) say("short struct: invalid");
    if (doc->side_lookup(host->context, 11, &found, &index) == APEX_OK) {
        memset(&side, 0, sizeof side); side.struct_size = sizeof side;
        if (doc->side_at(host->context, found, index, &side) == APEX_OK) {
            snprintf(line, sizeof line, "lookup 11: index %zu, same solid %d, id %d", index, found == solid, side.id);
            say(line);
        }
    }
    if (doc->side_lookup(host->context, 999, &found, &index) == APEX_ERR_NOT_FOUND) say("lookup 999: not found");
    return APEX_OK;
}
static const apex_plugin plugin = { sizeof(apex_plugin), 1, 1, 0, 0, activate, 0, 0, 0 };
APEX_PLUGIN_EXPORT const apex_plugin* apex_plugin_entry(const apex_host* h) { host = h; return &plugin; }
