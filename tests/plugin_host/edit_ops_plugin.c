/* Fixture for the 0.9.3 edit operations. The document it expects: root 0 is the world holding one box solid
   (child 0), root 1 an entity whose "sides" key lists the box's six side IDs in +X -X +Y -Y +Z -Z order.
   Every action runs one apply() batch and logs "<action>=<status>"; "bad" actions first change a key and then
   issue an invalid operation, so the test can prove the whole batch rolled back. changed() logs every notice. */
#include <apex/apex_plugin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const apex_host* host;
static const apex_edit_v1* edit;
static const apex_document_v1* doc;
static apex_node_id world, solid, entity;
static int sides[6];

static apex_str str(const char* s) { apex_str v = {s, strlen(s)}; return v; }
static void say(const char* text) { host->log(host->context, 1, str(text)); }

static int locate(void) {
    apex_str key, value;
    size_t i;
    if (doc->root_at(host->context, 0, &world) != APEX_OK || doc->root_at(host->context, 1, &entity) != APEX_OK ||
        doc->child_at(host->context, world, 0, &solid) != APEX_OK) return 0;
    for (i = 0; doc->key_at(host->context, entity, i, &key, &value) == APEX_OK; ++i) {
        if (key.size == 5 && memcmp(key.data, "sides", 5) == 0) {
            char text[128];
            char* cursor = text;
            int n = 0;
            if (value.size >= sizeof(text)) return 0;
            memcpy(text, value.data, value.size); text[value.size] = 0;
            while (n < 6) { char* end; long id = strtol(cursor, &end, 10); if (end == cursor) break; sides[n++] = (int)id; cursor = end; }
            return n == 6;
        }
    }
    return 0;
}

/* The unit box scaled to 128 x 64 x 64 (x runs 0..128). Vertex index = x + 2 y + 4 z. */
static const apex_vec3 corners[8] = {{0,0,0},{128,0,0},{0,64,0},{128,64,0},{0,0,64},{128,0,64},{0,64,64},{128,64,64}};
static const uint32_t loops[6][4] = {{1,3,7,5},{0,4,6,2},{2,6,7,3},{0,1,5,4},{4,5,7,6},{0,2,3,1}};

static apex_status set_geometry(void* user, const apex_edit_ops_v1* ops, void* ctx) {
    apex_face_geometry faces[6];
    apex_solid_geometry geometry;
    int i;
    (void)user;
    for (i = 0; i < 6; ++i) {
        memset(&faces[i], 0, sizeof(faces[i]));
        faces[i].struct_size = sizeof(faces[i]);
        faces[i].side = sides[i];
        faces[i].uaxis.x = 0; faces[i].uaxis.y = 1; faces[i].uaxis.z = 0;
        faces[i].vaxis.x = 0; faces[i].vaxis.y = 0; faces[i].vaxis.z = -1;
        faces[i].shift_u = 3.5; faces[i].shift_v = (double)i; faces[i].scale_u = 0.5; faces[i].scale_v = 0.25;
        faces[i].indices = loops[i]; faces[i].index_count = 4;
    }
    memset(&geometry, 0, sizeof(geometry));
    geometry.struct_size = sizeof(geometry);
    geometry.vertices = corners; geometry.vertex_count = 8;
    geometry.faces = faces; geometry.face_count = 6;
    return ops->set_solid_geometry(ctx, solid, &geometry);
}
/* Sets a marker key and then submits geometry that is missing a face. */
static apex_status set_geometry_bad(void* user, const apex_edit_ops_v1* ops, void* ctx) {
    apex_face_geometry faces[6];
    apex_solid_geometry geometry;
    int i;
    (void)user;
    ops->set_key(ctx, entity, str("marker"), str("set"));
    for (i = 0; i < 6; ++i) {
        memset(&faces[i], 0, sizeof(faces[i]));
        faces[i].struct_size = sizeof(faces[i]);
        faces[i].side = i == 5 ? 99999 : sides[i];  /* a side that is not the solid's */
        faces[i].scale_u = 1; faces[i].scale_v = 1;
        faces[i].indices = loops[i]; faces[i].index_count = 4;
    }
    memset(&geometry, 0, sizeof(geometry));
    geometry.struct_size = sizeof(geometry);
    geometry.vertices = corners; geometry.vertex_count = 8;
    geometry.faces = faces; geometry.face_count = 6;
    return ops->set_solid_geometry(ctx, solid, &geometry);
}

static apex_status fill_displacement(const apex_edit_ops_v1* ops, void* ctx, int side, int power) {
    apex_vec3 normals[25], offsets[25];
    double distances[25], alphas[25];
    int32_t tags[32];
    apex_displacement d;
    int i;
    for (i = 0; i < 25; ++i) {
        normals[i].x = 0; normals[i].y = 0; normals[i].z = 1;
        offsets[i].x = 0; offsets[i].y = 0; offsets[i].z = 0;
        distances[i] = i == 12 ? 16.0 : 0.0; alphas[i] = 0.0;
    }
    for (i = 0; i < 32; ++i) tags[i] = 9;
    memset(&d, 0, sizeof(d));
    d.struct_size = sizeof(d); d.power = power;
    d.start_position.x = 0; d.start_position.y = 0; d.start_position.z = 64;
    d.elevation = 2.0; d.flags = 0; d.subdiv = 0;
    d.normals = normals; d.distances = distances; d.offsets = offsets; d.offset_normals = 0;
    d.alphas = alphas; d.triangle_tags = tags;
    return ops->set_displacement(ctx, side, &d);
}
static apex_status set_displacement(void* user, const apex_edit_ops_v1* ops, void* ctx) {
    (void)user;
    return fill_displacement(ops, ctx, sides[4], 2);
}
static apex_status set_displacement_bad(void* user, const apex_edit_ops_v1* ops, void* ctx) {
    (void)user;
    ops->set_key(ctx, entity, str("marker"), str("set"));
    return fill_displacement(ops, ctx, sides[4], 7);  /* power out of range */
}

static apex_status set_connections(void* user, const apex_edit_ops_v1* ops, void* ctx) {
    apex_connection list[2];
    int i;
    (void)user;
    memset(list, 0, sizeof(list));
    for (i = 0; i < 2; ++i) list[i].struct_size = sizeof(list[i]);
    list[0].output = str("OnTrigger"); list[0].target = str("door"); list[0].input = str("Open");
    list[0].parameter = str(""); list[0].delay = str("0.5"); list[0].times = str("-1");
    list[1].output = str("OnTrigger"); list[1].target = str("light"); list[1].input = str("TurnOn");
    list[1].parameter = str("p"); list[1].delay = str("0"); list[1].times = str("1");
    return ops->set_connections(ctx, entity, list, 2);
}
static apex_status clear_connections(void* user, const apex_edit_ops_v1* ops, void* ctx) {
    (void)user;
    return ops->set_connections(ctx, entity, 0, 0);
}
static apex_status set_connections_bad(void* user, const apex_edit_ops_v1* ops, void* ctx) {
    apex_connection list[1];
    (void)user;
    memset(list, 0, sizeof(list));
    list[0].struct_size = sizeof(list[0]);
    list[0].target = str("door");  /* no output name */
    ops->set_key(ctx, entity, str("marker"), str("set"));
    return ops->set_connections(ctx, entity, list, 1);
}

/* The box's planes in reverse order; rebuild_solid alone cannot say which side is which after that. */
static const apex_plane box_planes[6] = {{{1,0,0},128},{{-1,0,0},0},{{0,1,0},64},{{0,-1,0},0},{{0,0,1},64},{{0,0,-1},0}};
static apex_status rebuild_matched(void* user, const apex_edit_ops_v1* ops, void* ctx) {
    apex_plane planes[6];
    apex_face_match match[6];
    int i;
    (void)user;
    for (i = 0; i < 6; ++i) {
        planes[i] = box_planes[5 - i];
        memset(&match[i], 0, sizeof(match[i]));
        match[i].struct_size = sizeof(match[i]);
        match[i].plane_index = (uint32_t)i;
        match[i].old_side = sides[5 - i];
    }
    for (i = 0; i < 6; ++i) if (planes[i].normal.x > 0.5) planes[i].distance = 256;  /* widen +X */
    return ops->rebuild_solid_matched(ctx, solid, planes, 6, match, 6);
}
static apex_status rebuild_matched_bad(void* user, const apex_edit_ops_v1* ops, void* ctx) {
    apex_plane planes[6];
    apex_face_match match[1];
    int i;
    (void)user;
    ops->set_key(ctx, entity, str("marker"), str("set"));
    for (i = 0; i < 6; ++i) planes[i] = box_planes[i];
    memset(match, 0, sizeof(match));
    match[0].struct_size = sizeof(match[0]); match[0].plane_index = 0; match[0].old_side = 99999;
    return ops->rebuild_solid_matched(ctx, solid, planes, 6, match, 1);
}

static apex_status destroy_solid(void* user, const apex_edit_ops_v1* ops, void* ctx) {
    (void)user;
    return ops->destroy(ctx, solid);
}
static apex_status create_block(void* user, const apex_edit_ops_v1* ops, void* ctx) {
    apex_node_id made = 0;
    char text[64];
    apex_status status = ops->create_block(ctx, 0, str("entity { id 0 classname info_target targetname \"made\" origin \"1 2 3\" }"), &made);
    (void)user;
    snprintf(text, sizeof(text), "created_id=%llu", (unsigned long long)made);
    say(text);
    return status;
}
static apex_status create_group(void* user, const apex_edit_ops_v1* ops, void* ctx) {
    apex_node_id made = 0;
    (void)user;
    return ops->create_block(ctx, 0, str("group { id 0 editor { color \"1 2 3\" } }"), &made);
}
static apex_status create_block_bad(void* user, const apex_edit_ops_v1* ops, void* ctx) {
    apex_node_id made = 0;
    (void)user;
    ops->set_key(ctx, entity, str("marker"), str("set"));
    return ops->create_block(ctx, 0, str("solid { id 0 }"), &made);
}
static apex_status editor_color(void* user, const apex_edit_ops_v1* ops, void* ctx) {
    (void)user;
    return ops->set_editor(ctx, entity, APEX_EDITOR_COLOR, str("10 20 30"));
}
static apex_status editor_visgroup(void* user, const apex_edit_ops_v1* ops, void* ctx) {
    (void)user;
    return ops->set_editor(ctx, entity, APEX_EDITOR_ADD_VISGROUP, str("7"));
}
static apex_status editor_bad(void* user, const apex_edit_ops_v1* ops, void* ctx) {
    (void)user;
    ops->set_key(ctx, entity, str("marker"), str("set"));
    return ops->set_editor(ctx, entity, APEX_EDITOR_ADD_VISGROUP, str("seven"));
}
static apex_status set_key(void* user, const apex_edit_ops_v1* ops, void* ctx) {
    (void)user;
    return ops->set_key(ctx, entity, str("targetname"), str("renamed"));
}
static apex_status set_material(void* user, const apex_edit_ops_v1* ops, void* ctx) {
    (void)user;
    return ops->set_side_key(ctx, sides[4], str("material"), str("DEV/TEST"));
}
static apex_status reparent_entity(void* user, const apex_edit_ops_v1* ops, void* ctx) {
    (void)user;
    return ops->reparent(ctx, entity, world, 0);
}
/* Everything in one batch: one undo step. */
static apex_status everything(void* user, const apex_edit_ops_v1* ops, void* ctx) {
    apex_status status = set_key(user, ops, ctx);
    if (status == APEX_OK) status = set_connections(user, ops, ctx);
    if (status == APEX_OK) status = set_geometry(user, ops, ctx);
    if (status == APEX_OK) status = editor_color(user, ops, ctx);
    return status;
}

struct action { const char* name; apex_edit_callback callback; };
static const struct action actions[] = {
    {"set_geometry", set_geometry}, {"set_geometry_bad", set_geometry_bad},
    {"set_displacement", set_displacement}, {"set_displacement_bad", set_displacement_bad},
    {"set_connections", set_connections}, {"clear_connections", clear_connections}, {"set_connections_bad", set_connections_bad},
    {"rebuild_matched", rebuild_matched}, {"rebuild_matched_bad", rebuild_matched_bad},
    {"destroy_solid", destroy_solid}, {"create_block", create_block}, {"create_group", create_group},
    {"create_block_bad", create_block_bad}, {"editor_color", editor_color}, {"editor_visgroup", editor_visgroup},
    {"editor_bad", editor_bad}, {"set_key", set_key}, {"set_material", set_material},
    {"reparent_entity", reparent_entity}, {"everything", everything},
};
#define ACTION_COUNT (sizeof(actions) / sizeof(actions[0]))

static void invoke(void* user) {
    const struct action* action = (const struct action*)user;
    char text[96];
    apex_status status;
    if (!locate()) { say("locate failed"); return; }
    status = edit->apply(host->context, str(action->name), str(""), (void*)0, action->callback);
    snprintf(text, sizeof(text), "%s=%d", action->name, (int)status);
    say(text);
}

static void put(char* out, size_t size, const char* fmt, unsigned long long value) {
    size_t used = strlen(out);
    snprintf(out + used, size - used, fmt, value);
}
static void changed(void* user, const apex_change_notice* n) {
    char text[512];
    size_t i;
    (void)user;
    if (n->struct_size < sizeof(*n)) say("short notice");
    snprintf(text, sizeof(text), "notice rev=%llu u=%u c=", (unsigned long long)n->revision, n->undo_or_redo);
    for (i = 0; i < n->created_count; ++i) put(text, sizeof(text), "%llu,", n->created[i]);
    strncat(text, " m=", sizeof(text) - strlen(text) - 1);
    for (i = 0; i < n->modified_count; ++i) {
        char item[48];
        if (n->modified[i].struct_size != sizeof(apex_modified_node)) say("bad modified struct_size");
        snprintf(item, sizeof(item), "%llu:%u,", (unsigned long long)n->modified[i].id, n->modified[i].flags);
        strncat(text, item, sizeof(text) - strlen(text) - 1);
    }
    strncat(text, " d=", sizeof(text) - strlen(text) - 1);
    for (i = 0; i < n->destroyed_count; ++i) put(text, sizeof(text), "%llu,", n->destroyed[i]);
    strncat(text, " v=", sizeof(text) - strlen(text) - 1);
    for (i = 0; i < n->vanished_side_count; ++i) put(text, sizeof(text), "%llu,", (unsigned long long)n->vanished_sides[i]);
    say(text);
}

static apex_status activate(void* user) {
    const apex_registration_v1* reg;
    size_t i;
    (void)user;
    edit = (const apex_edit_v1*)host->get_interface(host->context, str("apex.edit/1"));
    doc = (const apex_document_v1*)host->get_interface(host->context, str("apex.document/1"));
    reg = (const apex_registration_v1*)host->get_interface(host->context, str("apex.registration/1"));
    if (!edit || !doc || !reg) return APEX_ERR_INTERNAL;
    for (i = 0; i < ACTION_COUNT; ++i) {
        char id[96];
        snprintf(id, sizeof(id), "plugin.editops.%s", actions[i].name);
        if (reg->action(host->context, str(id), str(actions[i].name), str(actions[i].name), (void*)&actions[i], invoke) != APEX_OK)
            return APEX_ERR_INTERNAL;
    }
    return APEX_OK;
}
static const apex_plugin plugin = {sizeof(plugin), 1, 0, 0, 0, activate, 0, 0, 0, changed};
APEX_PLUGIN_EXPORT const apex_plugin* apex_plugin_entry(const apex_host* h) { host = h; return &plugin; }
