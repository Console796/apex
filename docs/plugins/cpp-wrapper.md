# The C++ wrapper (plugin.hpp)

`include/apex/plugin.hpp` is header-only, namespace `apex::plugin`, and includes nothing but `apex_plugin.h` and the standard library. It provides:

- `apex::plugin::string(std::string_view)` and `view(apex_str)` conversions.
- `apex::plugin::tools(host)`, `actions(host)`, `ui(host)`, `edit(host)`, `selection(host)`: typed
  `get_interface` lookups.
- `apex::plugin::Tool`: derive and override `input`, `visual`, `readout`, `setParameter`, `perform`,
  `cancel`, `documentChanged`, `forget`. Pass `Tool::callbacks()` and a pointer to your object to
  `register_tool`. Exceptions are caught at the C boundary. The object must outlive its registration.
- `apex::plugin::Form`: derive and override `describe(context, controls)` (push controls built with
  `Form::control(kind, key, label)`), and optionally `changed` and `clicked`. Then
  `registerPanel(host, id, title, form)`, `registerSection(host, id, title, target, form)`,
  `refresh(host, id)` and `showDialog(host, title, form)` (true when accepted). Descriptions are valid until the
  next `describe`, so keep any strings you point at in members of the form.

- Edit-op argument helpers (0.9.3), which set `struct_size`: `faceGeometry(...)`, `solidGeometry(...)`,
  `connection(...)`, `faceMatch(...)`, and `available(ops, &apex_edit_ops_v1::member)` to test that a host's table
  has an operation.
- `apex::plugin::overlays(host)`, `helpers(host)`, `problems(host)`: interface lookups.
- `apex::plugin::Overlay`: vectors `segments`, `points`, `labels`, `ghosts`, `tints` with `clearLists()`;
  `declareOverlay(host, id, name)` in `activate`, then `setOverlay(host, id, overlay)` and `clearOverlay(host, id)`.
  Strings and ghost planes you point at must stay alive until `setOverlay` returns.
- `apex::plugin::Helper`: derive and override `draw(call, sink)`, using `Helper::segment(sink, ...)` and
  `Helper::label(sink, ...)`; `registerHelper(host, name, helper)`. Exceptions are caught at the C boundary.
- `apex::plugin::ProblemCheck`: derive and override `check(sink)`, report with
  `ProblemCheck::report(sink, severity, text, objects)`; `registerProblemCheck(host, id, name, check)`.
- `apex::plugin::io(host)`, `jobs(host)`, `watch(host)`: interface lookups.
- `apex::plugin::Importer`: derive and override `import(path, context)`; `Importer::report(context, text)`;
  `registerImporter(host, id, name, extensions, importer)` in `activate`.
- `apex::plugin::Job`: derive and override `run(context)` (worker thread; `Job::cancelled(context)`) and
  `done(status)` (editor thread); `submitJob(host, job, &id)` and `cancelJob(host, id)`.
- `apex::plugin::Watcher`: derive and override `changed(path)`; `watchPath(host, absolutePath, watcher, &id)` and
  `unwatch(host, id)`.
- `apex::plugin::vfs(host)`, `assets(host)`: interface lookups.
- `apex::plugin::FileProvider`: derive and override `list(add)` (call `add(path)` for each file), `open(path,
  bytes)` (fill the vector; return false for "not found"), and optionally `refresh()`. Register with
  `registerVfsProvider(host, id, name, provider, kind, anchor)` in `activate` (`kind` 0 first, 1 last, 2 before,
  3 after an anchor) and `requestVfsRefresh(host, id)`. Exceptions are caught at the C boundary; `open` runs on
  worker threads.
- `apex::plugin::findMaterial(host, name, info)`, `listMaterials(host, visit)` and `writeVmt(host, material,
  edits)` (a `std::vector<VmtEdit>{key, value, remove}`).

Every wrapper that takes an interface checks the interface pointer and returns `APEX_ERR_UNSUPPORTED` when the host
lacks it. Members that a later minor appends to a table (see [versioning.md](versioning.md)) are tested with
`available(ops, &apex_edit_ops_v1::member)` before use; the 1.0 members need no test.

A tool from the C++ wrapper:

<!-- snippet: examples/polygon-tool/polygon.cpp -->
```cpp
class Polygon final : public apex::plugin::Tool {
public:
    Tool* createInstance() override { return new Polygon; }
    bool input(const apex_tool_event& event, const apex_tool_view& view) override {
```

<!-- snippet: examples/polygon-tool/polygon.cpp -->
```cpp
apex_status activate(void*) {
    const auto* api = apex::plugin::tools(*host);
    return api ? api->register_tool(host->context, apex::plugin::string("plugin.polygon.tool"),
        apex::plugin::string("Polygon tool"), apex::plugin::string("Preview a polygon prism"), &polygon, Polygon::callbacks()) : APEX_ERR_UNSUPPORTED;
}
```
