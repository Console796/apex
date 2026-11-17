# apex.assets/1: materials

`list_materials(host, user, visit)` calls `visit` with an `apex_material_info` (`name`, `shader`, `base_texture`,
`surface_prop`, `source`) for each indexed material, until it returns non-zero. The strings are borrowed for the
call. The index may still be loading and then reports what is indexed so far. `find_material(host, name, &out)`
fills one record (`APEX_ERR_NOT_FOUND` for an unknown name; strings valid until the next host call). `source` is
`game`, the addon's name, or `provider:<name>` for a plugin provider.

`write_vmt(host, material, edits, count)` sets or removes top-level parameters of the material's VMT (1 to 256
edits; keys without whitespace or quotes; values up to 4,096 bytes without control characters; a key is matched
case-insensitively and a new one goes at the end of the shader block). Everything else in the file keeps its
bytes: comments, spacing, order, line endings. The write is atomic and the host then re-indexes that material, so
the next `find_material` and the browser list show the new values. Thumbnails and the 3D view keep their old
image of it until the content next reloads. Only a loose file in a folder mount can be written: a material served
from an archive or from a plugin provider returns `APEX_ERR_UNSUPPORTED`; an unknown one `APEX_ERR_NOT_FOUND`;
malformed edits `APEX_ERR_INVALID`. `last_error()` carries the reason. Material calls run on the editor thread.

<!-- snippet: include/apex/apex_plugin.h -->
```c
typedef struct apex_material_info {
    uint32_t struct_size;
    uint32_t reserved;
    apex_str name, shader, base_texture, surface_prop, source; /* source: "game", the addon's name, or "provider:<name>" */
} apex_material_info;
typedef struct apex_vmt_edit {
    uint32_t struct_size;
    uint32_t remove;
    apex_str key, value;
} apex_vmt_edit;
typedef int (*apex_material_visit)(void* user, const apex_material_info* info);
typedef struct apex_assets_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*list_materials)(void* host, void* user, apex_material_visit visit);
    apex_status (*find_material)(void* host, apex_str name, apex_material_info* out);
    apex_status (*write_vmt)(void* host, apex_str material, const apex_vmt_edit* edits, size_t edit_count);
} apex_assets_v1;
```

No example plugin reads the material index; `findMaterial`, `listMaterials` and `writeVmt` in `plugin.hpp` wrap the table.
