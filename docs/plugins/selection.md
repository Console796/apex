# apex.selection/1

`apex.selection/1` reads the selection mode and the ordered ids, or sets both together.

<!-- snippet: include/apex/apex_plugin.h -->
```c
enum apex_selection_mode { APEX_SELECT_GROUPS = 0, APEX_SELECT_OBJECTS = 1, APEX_SELECT_SOLIDS = 2, APEX_SELECT_FACES = 3 };
/* Selection IDs are node IDs except in face mode, where they are side IDs (hence uint64_t, not apex_node_id). */
typedef struct apex_selection_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*info)(void* host, uint32_t* mode, size_t* count);
    apex_status (*at)(void* host, size_t index, uint64_t* id);
    apex_status (*set)(void* host, uint32_t mode, const uint64_t* ids, size_t count);
} apex_selection_v1;
```

- `info(host, &mode, &count)` gives the mode and how many ids are selected; `at(host, index, &id)` one id.
- `set(host, mode, ids, count)` replaces the selection. A mode above `APEX_SELECT_FACES` is refused with `APEX_ERR_INVALID`.
- In face mode the ids are side ids; otherwise node ids. They are `uint64_t` in this table for that reason.
- Selection changes reach `on_changed` observers like any other change ([edit.md](edit.md)). An observer cannot edit
  directly: use `apex.edit/1`'s `defer`.
