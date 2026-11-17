# Versioning and ABI evolution

**State: ABI major 1, minor 1.** 1.0 is frozen; 1.1 only appends. This describes the contract; it does not change with the Apex version.

ABI major changes only for breaking changes and is 1 for the whole 1.x series. New interfaces and new fields
appended to existing structs bump the minor. Treat a `NULL` from `get_interface` as "not available on this host".

## Rules

The rules, also at the top of `include/apex/apex_plugin.h`, let an older plugin run on a newer host and
the reverse:

1. Every struct that crosses the boundary begins with `uint32_t struct_size`, set by whoever fills it to
   `sizeof` of the struct in the header it was compiled against. The exceptions are the frozen value types
   `apex_str`, `apex_vec3`, `apex_plane`, `apex_ray` and the scalar arrays (node IDs, side IDs, `apex_str`).
2. Structs only grow, by appending. A reader uses a field only when `struct_size` covers it
   (`offsetof(T, field) + sizeof(field) <= struct_size`); a missing field means the old behaviour.
3. No implicit padding: fields are ordered and `reserved` fields added so every field sits at its natural
   offset with no gaps (so LP64 and LLP64 agree). `reserved` fields are zero. New fields are appended in whole
   units that keep that true. `tests/plugin_host/abi_layout.inc` checks this for every struct at compile time,
   in C and in C++, and pins each struct's size.
4. **Arrays of structs** have a stride equal to the element's `struct_size`, and every element carries it. The
   host copies each plugin-provided element into its own current layout (fields the plugin's struct does not
   cover read as zero, fields the host does not know are ignored) and rejects an array whose first
   `struct_size` is below the 1.0 size, not a multiple of the alignment, or over 4096, or whose elements
   differ. Read a host-provided array with `apex_array_element(array, i)`, not `array[i]`, so a plugin built
   against an older header survives the host's element growing. In C++ build elements with the wrapper's
   `segment()`, `ghost()`, `point()`, `label()`, `overlayGhost()` and `tint()` helpers, which set `struct_size`.
   An element with `struct_size` zero (the old, stride-less arrays) is a bad array.
5. A struct embedded by value in another (`apex_ui_value` in `apex_ui_control`) is frozen at its 1.0 layout; new
   data goes in new fields of the parent.
6. Out parameters (`apex_pick`, `apex_document_info`, `apex_node_info`, `apex_side_info`, `apex_material_info`, and the
   `apex_tool_visual` / `apex_tool_readout` the host hands to a tool) carry the caller's `struct_size`; the
   callee writes only the part it covers.
7. A function table grows by appending function pointers; a null or uncovered pointer is "not provided".

0.9.3 added the structs `apex_face_geometry` (104 bytes), `apex_solid_geometry` (40), `apex_displacement` (112),
`apex_connection` (104) and `apex_face_match` (16), and appended six functions to `apex_edit_ops_v1` (now 112
bytes). Their sizes are pinned in `abi_layout.inc`; array elements follow rule 4.

1.1 appended `side_count`, `side_at` and `side_lookup` to `apex_document_v1` (now 72 bytes) and added the out
parameter `apex_side_info` (176 bytes), so a plugin can read a solid's sides (see `document.md`). A plugin that
uses them but should still load on a 1.0 host keeps `abi_minor` 0 in its `apex_plugin` and manifest and checks
the table with `available(...)`; one that declares minor 1 is refused by a 1.0 host.

## Evolution

- **New interface**: a new name (`apex.foo/1`). Plugins fetch it with `get_interface` and treat `NULL` as "not on
  this host". Minor goes up.
- **New field or function**: appended to its struct (rules 2 and 7), minor goes up. A plugin built against the
  newer header and run on an older host sees the shorter `struct_size` of host-provided tables and must test it
  (`available(...)` in `plugin.hpp`); a plugin built against an older header and run on a newer host never fills the
  new fields, which read as zero, the old behaviour.
- **New version of an interface**: `apex.foo/2` beside `/1`; the host serves both for the whole major.
- **Break**: a new major. The host refuses a plugin of another major with a message naming both versions.
- **Enums**: values are only added. A host rejects a value it does not know (`APEX_ERR_INVALID`); tool parameter
  kinds and similar switches skip what they do not know.
- Changing a layout means editing the header, the list in `tests/plugin_host/abi_layout.inc` with the new golden
  size, and appending only. The layout test checks every struct, in C and C++.

Deferred and regretted items, and how each can be added without a break, are in the review log of
`docs/plans/plugins.md` (Apex repository).
