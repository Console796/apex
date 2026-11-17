# Examples

The example plugins are in `share/apex-sdk/examples/` of the SDK and in `examples/` of the Apex repository, and are built by `examples/CMakeLists.txt` through `apex_add_plugin`. The Apex tests load them in a headless host; the `plugin_sdk_out_of_tree` test builds three of them from the unpacked SDK with only `find_package(ApexPlugin)`.

| Example | Language | Shows | Id |
|---|---|---|---|
| `hello-tool/hello.c` | C | The C header alone: log, read the document, a tool with a parameter and a preview marker, an action with a suggested key | `plugin.hello.tool`, `plugin.hello.read_document` |
| `polygon-tool/polygon.cpp` | C++ | `Tool` wrapper, per-viewport state, overlay segments and a ghost, a committed edit as one undo step | `plugin.polygon.tool` |
| `face-stats/face_stats.cpp` | C++ | `Form` wrapper, a section under Face Properties, document walk, refresh from `on_changed` | `plugin.facestats.stats` |
| `obj-import/obj_import.cpp` | C++ | `Importer` wrapper: one solid per OBJ group, one undo step, failures roll back | `plugin.objimport.obj` |
| `stair/stair.as` | AngelScript | A script plugin: parametric stair tool, one undo step | `plugin.stair.tool` |

## hello-tool

plain C. Logs on activate, reads the
  document through `apex.document/1`, registers tool `plugin.hello.tool` and action
  `plugin.hello.read_document` with a suggested Hammer-preset binding on F24.

<!-- snippet: examples/hello-tool/hello.c -->
```c
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
```

## polygon-tool

Click-to-place polygon tool. Enter or the `plugin.polygon.commit` action commits.

<!-- snippet: examples/polygon-tool/polygon.cpp -->
```cpp
    bool input(const apex_tool_event& event, const apex_tool_view& view) override {
        if (event.kind == APEX_INPUT_KEY_DOWN && apex::plugin::view(event.key) == "Escape") { cancel(); return true; }
        if (event.kind == APEX_INPUT_KEY_DOWN && apex::plugin::view(event.key) == "Enter")
            return perform("plugin.polygon.commit");
        if (event.kind != APEX_INPUT_PRESS || event.button != APEX_BUTTON_LEFT || std::abs(view.ray.direction.z) < 1e-9) return false;
        source_ = event.source;
        const auto t = -view.ray.origin.z / view.ray.direction.z;
        if (t < 0) return false;
        auto& points = points_[event.source];
        points.push_back({view.ray.origin.x + t * view.ray.direction.x,
                          view.ray.origin.y + t * view.ray.direction.y, 0});
        return true;
    }
```

## face-stats

<!-- snippet: examples/face-stats/face_stats.cpp -->
```cpp
// Face stats: an apex.ui/1 section under Face Properties that summarises the selected faces.
// It shows how a plugin
//   - registers a section for a host panel (registerSection with the "face_properties" target),
//   - reads the context the host passes to describe() (the selected side IDs),
//   - reads the open map through apex.document/1, and
//   - asks for a re-describe when the map changes (refresh from on_changed).
// Nothing here throws across the ABI: Form::callbacks() already catches, and on_changed catches itself.
```

<!-- snippet: examples/face-stats/face_stats.cpp -->
```cpp
void changed(void*, const apex_change_notice*) {
    try { apex::plugin::refresh(*host, sectionId); } catch (...) {}
}
```

## obj-import

C++ using `plugin.hpp`'s
  `Importer`. Imports a Wavefront OBJ as one solid per `o`/`g` group (each group must be a closed convex part;
  face planes are oriented outward from the group's centre, so winding does not matter). A bad group fails and
  undoes the whole import. File > Import, extension `.obj`.

<!-- snippet: examples/obj-import/obj_import.cpp -->
```cpp
ObjImporter importer;
apex_status activate(void*) {
    return apex::plugin::registerImporter(*host, "plugin.objimport.obj", "Wavefront OBJ (convex parts)", "obj", importer);
}
```

## stair

The AngelScript stair registers one tool and commits all steps in one `apex::Edit`.

<!-- snippet: examples/stair/stair.as -->
```cpp
// Stair: an Apex script plugin (docs/plugins/script-host.md).
//
// Move the cursor over the ground (z = 0) to place the stair, change "Steps" in the tool strip, click to
// create it. All steps are created in ONE apex.edit batch, so a single undo removes the whole stair.
// Escape forgets the placement. Everything the viewport shows is recomputed from the parameters on each
// visual() call: the script keeps no cached geometry.
```

<!-- snippet: examples/stair/stair.as -->
```cpp
void main() {
    apex::registerTool("tool", "Stair", "Create a straight stair with a chosen number of steps", "StairTool");
}
```
