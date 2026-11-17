# apex.io/1: importers

`register_importer(host, id, name, extensions, user, import)` is available only during `activate`. `id` is
`plugin.<plugin>.<name>`; `extensions` is a comma separated list without dots (`"obj,mesh"`, case-insensitive).
The editor offers registered importers under File > Import.

`import(user, path, context)` receives the file path as UTF-8 and an `apex_import_context`: `world` (the world
node), `ops` and `operation` (the same `apex_edit_ops_v1` table `apex.edit/1` uses) and `report(sink, message)`
for messages shown to the user. **One import is one undo step.** The host wraps the call in a single edit
transaction; returning anything but `APEX_OK`, throwing, or a failing operation rolls every change back and
leaves no undo entry. Importers run on the editor thread. Exporters, and importing into a new document, are not
part of this interface.

> **Files.** The importer receives a plain host file path (anything the user picked, not necessarily game
> content) and reads it itself (`std::ifstream`). Importers do not go through `apex.vfs/1`; that interface is
> for serving files *into* the game's virtual file system, not for reading arbitrary paths.

<!-- snippet: include/apex/apex_plugin.h -->
```c
typedef struct apex_import_context {
    uint32_t struct_size;
    uint32_t reserved;
    apex_node_id world;
    const apex_edit_ops_v1* ops;
    void* operation;
    void* sink;
    apex_status (*report)(void* sink, apex_str message);
} apex_import_context;
typedef apex_status (*apex_import_callback)(void* user, apex_str path, const apex_import_context* context);
typedef struct apex_io_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*register_importer)(void* host, apex_str id, apex_str name, apex_str extensions,
                                     void* user, apex_import_callback import);
} apex_io_v1;
```

The obj-import example:

<!-- snippet: examples/obj-import/obj_import.cpp -->
```cpp
class ObjImporter final : public apex::plugin::Importer {
public:
    apex_status import(std::string_view path, const apex_import_context& context) override {
        const std::string filePath(path);
```

<!-- snippet: examples/obj-import/obj_import.cpp -->
```cpp
        const auto status = context.ops->create_solid(context.operation, context.world, planes.data(), planes.size(),
                                                      apex::plugin::string(material));
        if (status != APEX_OK) return fail(context, "group '" + label + "' is not closed and convex");
```

<!-- snippet: examples/obj-import/obj_import.cpp -->
```cpp
ObjImporter importer;
apex_status activate(void*) {
    return apex::plugin::registerImporter(*host, "plugin.objimport.obj", "Wavefront OBJ (convex parts)", "obj", importer);
}
```
