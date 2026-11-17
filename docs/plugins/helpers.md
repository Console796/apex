# apex.helpers/1: FGD helper drawing

Fetch `"apex.helpers/1"`. In `activate`, `register_helper(host, name, user, draw)` claims an FGD helper name
(for `mywidget(5)` the name is `mywidget`). Only names the host does not draw itself are accepted:
`APEX_ERR_INVALID` for a built-in helper, a duplicate or a name outside `activate`. For each entity whose class
lists the helper, `draw(user, call, sink)` receives the entity, class, helper name, argument text, origin and
angles, and pushes world segments and labels through the sink. Reading the document through `apex.document` is
valid inside `draw`. The output is cached per document revision: helpers do not follow a live drag, and they
are shown only while the viewport's "Entity helpers" overlay is on. A draw that throws or returns an error
contributes nothing for that entity.

<!-- snippet: include/apex/apex_plugin.h -->
```c
typedef struct apex_helper_call {
    uint32_t struct_size;
    uint32_t reserved;
    apex_node_id entity;
    apex_str class_name, helper, arguments;
    apex_vec3 origin, angles;
} apex_helper_call;
typedef struct apex_helper_sink {
    uint32_t struct_size;
    uint32_t reserved;
    void* sink;
    apex_status (*segment)(void* sink, const apex_tool_segment* segment);
    apex_status (*label)(void* sink, const apex_overlay_label* label);
} apex_helper_sink;
typedef apex_status (*apex_helper_draw)(void* user, const apex_helper_call* call, const apex_helper_sink* sink);
typedef struct apex_helpers_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*register_helper)(void* host, apex_str name, void* user, apex_helper_draw draw);
} apex_helpers_v1;
```

No example plugin draws a helper; the C++ `Helper` base class wraps the table (see [cpp-wrapper.md](cpp-wrapper.md)).
