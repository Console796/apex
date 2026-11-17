# Apex plugin SDK (DRAFT)

> **Status: draft for 0.9.9.** Written against the ABI as committed at 0.9.2. 0.9.3 adds `apex.edit/1`,
> `apex.selection/1`, change notifications and `defer`, and 0.9.4–0.9.8 add more interfaces. Sections
> marked *(pending)* will be filled in as those land. The ABI is frozen at the 0.9 release; until then
> anything here may change. See `docs/plans/plugins.md` for the design rationale.

## 1. What a plugin is

A plugin is a native shared library (`.so` on Linux, `.dll` on Windows) loaded in-process, plus a
`plugin.kv3` manifest. It talks to Apex only through a versioned C ABI in
[`include/apex/apex_plugin.h`](../include/apex/apex_plugin.h). A header-only C++ wrapper,
[`include/apex/plugin.hpp`](../include/apex/plugin.hpp), adds exception-safe base classes on top.

Plugins run inside the editor process with no sandbox. A crash in a plugin crashes Apex; see
[Safe mode](#7-crashes-and-safe-mode).

## 2. Layout and manifest

Each plugin lives in its own directory under `<data>/plugins/` (or the bundled plugins directory):

```
plugins/hello/
  plugin.kv3
  hello_tool.so        # or .dll
```

`plugin.kv3`:

```
<!-- kv3 encoding:text:version{apex} format:generic:version{fixture} -->
{
    id = "hello"
    name = "Hello tool"
    version = "1.0.0"
    apex_abi = { major = 1 minor = 0 }
    entry = {
        "linux-x86_64"   = "hello_tool.so"
        "windows-x86_64" = "hello_tool.dll"
    }
}
```

| Field | Meaning |
|---|---|
| `id` | Plugin namespace. Everything you register must be prefixed `plugin.<id>.`. |
| `name`, `version` | Shown in the plugin manager. |
| `apex_abi` | The ABI you were built against. Major must equal the host's; minor must be ≤ the host's. |
| `entry` | Library file per platform key. A plugin with no entry for the current platform is skipped. |

## 3. Entry point and lifecycle

Export one symbol:

```c
APEX_PLUGIN_EXPORT const apex_plugin* apex_plugin_entry(const apex_host* host);
```

Apex calls it once. Check `host->abi_major`/`abi_minor`, stash whatever you need from `host`, and return a
pointer to a static `apex_plugin` table. Always set `struct_size = sizeof(...)` on every struct you fill in;
the host uses it to tell older and newer layouts apart.

| Callback | When |
|---|---|
| `activate(user)` | After the handshake, before the main window. **Registration is only valid here.** Return `APEX_OK`. |
| `on_document_opened(user)` / `on_document_closing(user)` | A document became / is about to stop being current. |
| `deactivate(user)` | Shutdown or when the plugin is disabled. |

Rules that apply to every callback:

- Never let a C++ exception cross the boundary. The `plugin.hpp` wrappers catch for you; raw C++ code must too.
- Returned `apex_str` values are borrowed. Copy them if you need them past the next call into the same
  interface or any document mutation.
- Failures return an `apex_status`; `host->last_error(context)` gives a human-readable reason for the
  most recent failure.
- `host->log(context, level, message)` writes to the Apex log, tagged with your plugin id.

## 4. Host interfaces

Fetch an interface by name with `host->get_interface(host->context, name)`. It returns `NULL` if the host
does not provide it (for instance an interface added in a newer minor version). Interface names carry their
version: `apex.document/1`.

### 4.1 `apex.document/1`: read the document

A read-only view of the open document as a tree of nodes (`apex_document_v1`):

| Function | Purpose |
|---|---|
| `info` | `is_open`, `revision`, `root_count` |
| `root_at(i)` | NodeId of the *i*-th root |
| `node(id)` | `kind`, `parent`, `child_count`, `key_count` |
| `child_at(id, i)` | NodeId of a child |
| `key_at(id, i, &key, &value)` | The *i*-th key/value pair of a node |

`apex_node_id` is a stable arena NodeId, not a VMF id. Check `is_open` first; calls with no open document
return `APEX_ERR_STATE`.

### 4.2 `apex.actions/1`: commands and keybindings

`register_action(id, name, description, user, invoke, suggestions, n)` adds a command. It appears in the
command palette and the keybinding page. Actions are *optional* in presets: Apex never binds a plugin
action on its own. `apex_action_suggestion{preset, chord, tool_id}` proposes a default binding (empty
`tool_id` = global context). Suggestions apply only when they do not conflict; a user binding always wins.

### 4.3 `apex.tools/1`: viewport tools

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

`source` is a viewport identity. Keep per-viewport state keyed by it. *(Known issue I-6: until 0.9.3, tool
callback state is shared between editor sessions.)*

### 4.4 Writing to the document *(pending, 0.9.3)*

`apex.edit/1` will apply a batch of changes as a single undoable edit. `apex.selection/1` will read and set
the selection. Change notifications will report created, modified and destroyed NodeIds. A `defer` helper
will run work after the document notification pass.

### 4.5 Later interfaces *(pending)*

Panels and UI (0.9.4), overlays, helpers, problems, importers, jobs and file watching (0.9.5), VFS and
assets (0.9.6), scripts (0.9.7), pies and themes (0.9.8).

## 5. The C++ wrapper

`plugin.hpp` provides:

- `apex::plugin::string(std::string_view)` and `view(apex_str)` conversions.
- `apex::plugin::tools(host)` and `actions(host)`: typed `get_interface` lookups.
- `apex::plugin::Tool`: derive and override `input`, `visual`, `readout`, `setParameter`, `perform`,
  `cancel`, `documentChanged`, `forget`. Pass `Tool::callbacks()` and a pointer to your object to
  `register_tool`. Exceptions are caught at the C boundary. The object must outlive its registration.

## 6. Examples

Both are built by the main CMake project and placed under `<build>/plugins/<id>/` with a generated manifest.

- [`examples/hello-tool/hello.c`](../examples/hello-tool/hello.c): plain C. Logs on activate, reads the
  document through `apex.document/1`, registers tool `plugin.hello.tool` and action
  `plugin.hello.read_document` with a suggested Hammer-preset binding on F24.
- [`examples/polygon-tool/polygon.cpp`](../examples/polygon-tool/polygon.cpp): C++ using `plugin.hpp`. A
  click-to-place polygon tool that draws overlay segments and a ghost; committing to the document arrives
  with `apex.edit/1` *(pending, 0.9.3)*.

To build your own: compile a `MODULE` library with `include/` on the include path, no prefix, export
`apex_plugin_entry`, and write a `plugin.kv3` beside it.

```cmake
add_library(my_plugin MODULE my_plugin.cpp)
target_include_directories(my_plugin PRIVATE path/to/apex/include)
set_target_properties(my_plugin PROPERTIES PREFIX "")
```

Build with a compiler and standard library compatible with the Apex release you target; only the C ABI is
guaranteed across compilers, so keep C++ types out of the boundary.

## 7. Crashes and safe mode

Apex keeps a memory-mapped breadcrumb recording which plugin is running and in which callback. If the
previous run died inside a plugin, the next start names it and offers to disable it. `apex --no-plugins`
starts with every plugin disabled. The plugin manager (Settings → Plugins…) lists discovered plugins, their ABI
status, and any load errors, and lets you enable or disable them (takes effect on restart).

## 8. Versioning

ABI major changes only for breaking changes and is frozen at 1 for the whole 1.x series. New interfaces and
new fields appended to existing structs bump the minor. Always set and honour `struct_size`, and treat a
`NULL` from `get_interface` as "not available on this host".
