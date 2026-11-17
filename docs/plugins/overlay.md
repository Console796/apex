# apex.overlay/1

Fetch `"apex.overlay/1"`. In `activate`, `declare(host, id, name)` creates a named overlay (`id` must be
`plugin.<yourid>.<name>`; `name` is the checkbox label). Each viewport gets a "Plugins" group in its Overlays
panel with one box per declared overlay, on by default; the choice is saved with the viewport layout.

`set(host, id, list)` replaces what the overlay draws; `clear(host, id)` empties it. Both need the declaring
plugin and work in `activate` and in any callback into your plugin (an action, a tool, a UI handler). The
host copies the list before `set` returns, and rejects the whole list on a NaN/inf, a bad ghost or a bad
`struct_size`, leaving the previous contents alone. A list holds:

- `segments` (the `apex.tools` segment, width in pixels), `points` (size in pixels), `labels` (world position and
  text), all in world space.
- `ghosts`: a convex solid given as planes (the `apex.tools` ghost convention), drawn as translucent faces
  and edges.
- `tints`: `{side ID, rgba}` colours a whole face. Later declarations win a shared side. A tint never selects.

Lists draw in every display mode. Tints apply to filled faces, so how a tint reads in wireframe is up to the
display mode. A viewport with the overlay's box off draws none of its lists. Overlays are dropped when the
document goes away; disabling a plugin takes effect on restart like everything else. Tinting every face of a very large map is measured in
`docs/plans/plugins.md` (probe P4, in the Apex repository); keep updates to the rate a human can read.

<!-- snippet: include/apex/apex_plugin.h -->
```c
typedef struct apex_overlay_list {
    uint32_t struct_size;
    uint32_t reserved;
    const apex_tool_segment* segments; size_t segment_count;
    const apex_overlay_point* points; size_t point_count;
    const apex_overlay_label* labels; size_t label_count;
    const apex_overlay_ghost* ghosts; size_t ghost_count;
    const apex_overlay_tint* tints; size_t tint_count;
} apex_overlay_list;
typedef struct apex_overlay_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*declare)(void* host, apex_str id, apex_str name);
    apex_status (*set)(void* host, apex_str id, const apex_overlay_list* list);
    apex_status (*clear)(void* host, apex_str id);
} apex_overlay_v1;
```

Segments use the same `apex_tool_segment` as tools; in C++ build elements with `apex::plugin::segment(...)`:

<!-- snippet: examples/polygon-tool/polygon.cpp -->
```cpp
            segments_.push_back(apex::plugin::segment(a, b, 2, {1, 0.65f, 0.15f, 1}));
```
