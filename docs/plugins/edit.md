# apex.edit/1: writing to the document

`apex.edit/1` applies a callback's operations as one session edit and one undo step. The operation context
and table are valid only during the callback. A failed operation or a non-OK callback return rolls the batch
back; `last_error` contains the failure detail. A failing operation returns `APEX_ERR_INVALID` and poisons the
batch: it rolls back when the callback returns, even if the callback returns `APEX_OK`, and the selection is
restored as for any undo step. Every ops-table member beyond the first seven is at the end of the table: check
`struct_size` (the wrapper's `available(ops, &apex_edit_ops_v1::member)` does) before calling.

| Operation | Effect |
|---|---|
| `set_key(node, key, value)` | Sets an entity or world key-value |
| `destroy(node)` | Removes a node and its subtree; sides that vanish are reported in the change notice and the sidelists of overlays and similar entities are updated |
| `create_solid(parent, planes, count, material)` | New convex solid from planes |
| `reparent(node, parent, position)` | Moves a node |
| `set_side_key(side, key, value)` | A side's material, UVs, plane and so on |
| `remove_displacement(side)` | Removes a side's displacement |
| `rebuild_solid(solid, planes, count)` | Replaces the shape from planes; the host infers which face is which (see `rebuild_solid_matched` to say so explicitly) |
| `set_solid_geometry(solid, geometry)` | (0.9.3) Full geometry: a vertex pool and one `apex_face_geometry` per existing side (loops of vertex indices, UV axes, shifts, scales). Side IDs must be exactly the solid's; each side's other attributes are kept |
| `set_displacement(side, disp)` | (0.9.3) Sets a side's dispinfo (power 2 to 4, `(2^power + 1)^2` normals, offsets, alphas and distances, `2 (2^power)^2` triangle tags). Invalid data, or a side that is not a quad, is refused |
| `set_connections(entity, list, count)` | (0.9.3) Replaces the entity's outputs with `apex_connection` elements; count 0 removes them |
| `rebuild_solid_matched(solid, planes, n, matches, m)` | (0.9.3) `rebuild_solid` plus `apex_face_match` elements saying which existing side each plane's face continues, so side IDs, materials, UVs and an overlay's face survive a rebuild that reorders or moves planes |
| `create_block(parent, kv1, &out)` | (0.9.3) Creates one block from KV1 text (an entity, a group, ...) under `parent` (0 is the root) and returns its node ID. Worlds and solids are refused: use `create_solid`. IDs in the text are replaced |
| `set_editor(node, kind, value)` | (0.9.3) Hammer editor-block changes (`apex_editor_kind`): group, visgroup add/remove, color, visgroup shown flags, comments |

**Not exposed** (still in `Transaction`, nothing plugin-facing needs them yet): `setKeyValueOccurrence` and
`setEntityKeyValueEntry` (editing one of several duplicate keys), and `createFragment` (an internal importer and
paste mechanism). There is no read interface for displacements or connections yet, so a plugin can write them
but not inspect them through `apex.document/1`. The AngelScript API has no bindings for the 0.9.3 operations
(scripts keep the earlier set); script plugins that need them can be given bindings later, since the
bindings would be additive.

**Change notices.** `on_changed` receives, per committed transaction or undo/redo step, `created` and
`destroyed` node IDs (a destroyed ID is no longer readable), `modified` (`apex_modified_node`: id plus
`flags`, a bit set of geometry 1, material 2, key-values 4, visibility 8, hierarchy 16), `vanished_sides`
(side IDs that no longer exist, so a plugin can drop references), `undo_or_redo` (1 when the step came from
undo or redo, so the plugin should resynchronise instead of echoing its own edit), and `revision` (a value that
changes with every undoable step and every undo or redo; compare it for equality, do not assume it is dense).

The tables:

<!-- snippet: include/apex/apex_plugin.h -->
```c
typedef struct apex_edit_ops_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*set_key)(void*, apex_node_id, apex_str key, apex_str value);
    apex_status (*destroy)(void*, apex_node_id);
    apex_status (*create_solid)(void*, apex_node_id parent, const apex_plane*, size_t, apex_str material);
    apex_status (*reparent)(void*, apex_node_id, apex_node_id parent, size_t position);
    apex_status (*set_side_key)(void*, int32_t side, apex_str key, apex_str value);
    apex_status (*remove_displacement)(void*, int32_t side);
    apex_status (*rebuild_solid)(void*, apex_node_id, const apex_plane*, size_t);
    apex_status (*set_solid_geometry)(void*, apex_node_id solid, const apex_solid_geometry*);
    apex_status (*set_displacement)(void*, int32_t side, const apex_displacement*);
    apex_status (*set_connections)(void*, apex_node_id entity, const apex_connection*, size_t count);
    apex_status (*rebuild_solid_matched)(void*, apex_node_id, const apex_plane*, size_t,
                                         const apex_face_match*, size_t match_count);
    apex_status (*create_block)(void*, apex_node_id parent, apex_str kv1, apex_node_id* out);
    apex_status (*set_editor)(void*, apex_node_id, uint32_t kind, apex_str value);
} apex_edit_ops_v1;
typedef apex_status (*apex_edit_callback)(void*, const apex_edit_ops_v1*, void* operation_context);
typedef struct apex_edit_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*apply)(void* host, apex_str label, apex_str coalesce_key, void* user, apex_edit_callback);
    /* Queues callback(user) on the open document's deferred list. APEX_ERR_STATE without an editor session and
       document; APEX_ERR_INVALID for a null callback. */
    apex_status (*defer)(void* host, void* user, void (*callback)(void*));
} apex_edit_v1;
```

## Example

The polygon tool commits through `apply` as one undo step:

<!-- snippet: examples/polygon-tool/polygon.cpp -->
```cpp
    bool perform(std::string_view action) override {
        if (action != "plugin.polygon.commit") return false;
        apex_tool_visual result{};
        result.struct_size = sizeof(result);
        if (visual(source_, result) != APEX_OK || !result.ghost_count) return false;
        const auto* doc = static_cast<const apex_document_v1*>(host->get_interface(host->context, apex::plugin::string("apex.document/1")));
        const auto* edit = static_cast<const apex_edit_v1*>(host->get_interface(host->context, apex::plugin::string("apex.edit/1")));
        apex_node_id root = 0;
        if (!doc || !edit || doc->root_at(host->context, 0, &root) != APEX_OK) return false;
        struct Commit { Polygon* polygon; apex_node_id root; } commit{this, root};
        const auto status = edit->apply(host->context, apex::plugin::string("Create polygon"), {}, &commit,
            [](void* user, const apex_edit_ops_v1* ops, void* context) -> apex_status {
                auto& c = *static_cast<Commit*>(user);
                return ops->create_solid(context, c.root, c.polygon->planes_.data(), c.polygon->planes_.size(),
                                         apex::plugin::string("DEV/DEV_MEASUREGENERIC01"));
            });
        if (status != APEX_OK) return false;
        cancel();
        return true;
    }
```

`defer(host, user, callback)`
queues `callback(user)` on the open document's deferred list, to run after the current notification pass; it returns
`APEX_ERR_STATE` without an editor session and document, and `APEX_ERR_INVALID` for a null callback.
