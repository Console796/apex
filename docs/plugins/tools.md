# apex.tools/1: viewport tools

`register_tool(id, name, description, user, callbacks)` adds a tool to the tool list. `apex_tool_callbacks`:

| Callback | Role |
|---|---|
| `activate` / `deactivate` | Tool selected / deselected |
| `input(event, view)` | Press, move, release, key, leave, cancel. Return non-zero if consumed. `view` carries the pick ray, camera, and `ray_pick`, `rect_frustum`, `world_to_pixel`, `snap` helpers, valid only during this call. |
| `visual(source, out)` | Provide overlay line segments and translucent ghost brushes (as plane sets). |
| `readout(out)` | Status text, a warning flag, and tool parameters shown in the tool options bar. |
| `set_parameter(id, value)` / `perform(action)` | Edits from the options bar. |
| `cancel` | Esc or tool switch mid-gesture. |
| `document_changed` | The document changed under the tool. |
| `forget(source)` | A viewport (input source) went away; drop per-viewport state. |

`source` is a viewport identity. Keep per-viewport state keyed by it. The host keeps each viewport's ghosts apart
too: the preview shows the ghosts every viewport's `visual` last returned, and `forget(source)` removes that
viewport's. A pointer press that `input` handled (returned non-zero) owns its viewport until the buttons are up or
the gesture is cancelled, so the editor does not open a pie or start a navigation over your drag; there is no
other plugin-declared ownership.

`apex_tool_event.key` is the canonical key name the keybinding presets use (`"A"`, `"5"`, `"Space"`, `"Escape"`,
`"Enter"`, `"BracketLeft"`, `"F5"`, ...) without modifiers (read those from `modifiers`). It is empty for pointer
events and for a bare modifier key, which sets `APEX_EVENT_MODIFIER_KEY` in `flags`; `flags` also carries the held
buttons (`APEX_EVENT_LEFT_HELD` ...), `APEX_EVENT_AUTO_REPEAT` and `APEX_EVENT_NAVIGATING`.

The callbacks and the registration call:

<!-- snippet: include/apex/apex_plugin.h -->
```c
typedef struct apex_tool_callbacks {
    uint32_t struct_size;
    uint32_t reserved;
    apex_callback activate, deactivate;
    int (*input)(void*, const apex_tool_event*, const apex_tool_view*);
    apex_status (*visual)(void*, uint64_t source, apex_tool_visual*);
    apex_status (*readout)(void*, apex_tool_readout*);
    int (*set_parameter)(void*, apex_str id, double value);
    int (*perform)(void*, apex_str action);
    apex_callback cancel, document_changed;
    void (*forget)(void*, uint64_t source);
    /* Optional factory and destructor for state owned by each editor session; they are given together (a
       registration with only one is refused with APEX_ERR_INVALID). The host calls create_instance once per
       editor session and passes its result as the first argument of every other callback of that session's tool;
       destroy_instance runs when the session's tool is destroyed. A tool without them, or whose create_instance
       returns null, runs on the registration's `user` pointer, which every session shares; the host logs one
       warning when a second session gets such a tool. */
    void* (*create_instance)(void* registration_user);
    void (*destroy_instance)(void* instance);
} apex_tool_callbacks;
```

<!-- snippet: include/apex/apex_plugin.h -->
```c
typedef struct apex_tools_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*register_tool)(void*, apex_str id, apex_str name, apex_str description,
                                 void* user, const apex_tool_callbacks* callbacks);
} apex_tools_v1;
```

## Tool state is per editor session

Every editor session builds its own tool object for each registered tool,
so two windows or documents open at once do not see each other's tool state, provided the plugin uses
`create_instance`. If `apex_tool_callbacks.create_instance` is set, the host calls it with the registration's
`user` when a session creates its tool, passes the returned pointer instead of `user` to every other callback of
that session's tool, and calls `destroy_instance` when the session's tool goes. `create_instance` and
`destroy_instance` must be given together: registering with only one fails with `APEX_ERR_INVALID`.
A *legacy* tool (no `create_instance`, or one that returns null) gets the registration's `user` in every
session: all sessions share that one state. This is allowed and not refused, but the host logs one warning when
a second session gets such a tool, because the sessions then overwrite each other's state. The C++ `Tool`
wrapper supplies both callbacks: override `Tool::createInstance()` to return a new `Tool` (the host deletes it
when the session's tool goes). The default returns null, which makes the tool a legacy tool.

## Example: a tool in C

The hello-tool example keeps its state in globals (one session). It registers the callbacks table and the tool:

<!-- snippet: examples/hello-tool/hello.c -->
```c
static const apex_tool_callbacks tool = {sizeof(tool), 0, 0, 0, tool_input, tool_visual, tool_readout,
                                          tool_parameter, 0, tool_cancel, tool_cancel, tool_forget};
```

The input callback:

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

## Example: a tool in C++

The polygon tool derives from `apex::plugin::Tool`, keeps points per viewport and implements `createInstance`:

<!-- snippet: examples/polygon-tool/polygon.cpp -->
```cpp
class Polygon final : public apex::plugin::Tool {
public:
    Tool* createInstance() override { return new Polygon; }
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

<!-- snippet: examples/polygon-tool/polygon.cpp -->
```cpp
    void cancel() override { points_.clear(); }
    void forget(uint64_t source) override { points_.erase(source); }
```
