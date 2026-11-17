# Apex plugin SDK (DRAFT)

> **Status: draft for 0.9.9.** Written against the ABI through 0.9.5a. 0.9.5b–0.9.8 add more interfaces. Sections
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

### 4.4 Writing to the document

`apex.edit/1` applies a callback's operations as one session edit and one undo step. The operation context
and table are valid only during the callback. A failed operation or a non-OK callback return rolls the batch
back; `last_error` contains the failure detail. The current table exposes key writes, node destruction and
reparenting, solid creation and rebuilding, side-key writes, and displacement removal. Other `Transaction`
operations are not yet exposed.

`apex.selection/1` reads the selection mode and ordered IDs, or sets both together. Face mode uses side IDs;
other modes use node IDs. The plugin's optional `on_changed` callback receives borrowed ID lists, dirty flags,
vanished side IDs, the revision, and whether the notice is for undo or redo. Observers cannot edit directly:
use `apex.edit/1`'s `defer` to run a callback after the notification pass.

### 4.5 `apex.ui/1`: panels, sections and dialogs

A plugin describes a form; the host draws it with native widgets. The plugin never sees a widget, and all
callbacks run on the UI thread.

```c
typedef struct apex_ui_v1 {
    uint32_t struct_size;
    apex_status (*register_panel)(void* host, apex_str id, apex_str title, void* user, const apex_ui_callbacks*);
    apex_status (*refresh)(void* host, apex_str id);
    apex_status (*register_section)(void* host, apex_str id, apex_str title, apex_str target, void* user, const apex_ui_callbacks*);
    apex_status (*show_dialog)(void* host, apex_str title, void* user, const apex_ui_callbacks*, uint32_t* accepted);
} apex_ui_v1;
```

**Callbacks.** One `apex_ui_callbacks` table serves all three uses. `describe(user, context, &form)` returns the
current `apex_ui_form` (an array of `apex_ui_control`). `on_change(user, key, value)` reports an edited
control and `on_click(user, key)` a pressed button. The form, its strings and the value are borrowed: the
host copies what it needs before `describe` returns, and the plugin must keep the description alive only
until the next call to `describe`. `context` is `NULL` for panels and dialogs and set for sections.

**Controls.** Every control has a stable `key` (unique within its form; it identifies the control across
re-describes), a `label` and a `value` (`text`, `number` or `checked`, depending on the kind).

| Kind | Widget | Value | Notes |
|------|--------|-------|-------|
| `APEX_UI_LABEL` | read-only text | `text` | |
| `APEX_UI_TEXT` | line edit | `text` | `on_change` fires as the user types |
| `APEX_UI_NUMBER` | spin box | `number` | `minimum` / `maximum` bound it |
| `APEX_UI_CHECKBOX` | check box | `checked` | `label` is the box text |
| `APEX_UI_CHOICE` | drop-down | `text` | `options` lists the entries; `text` is the selected one |
| `APEX_UI_COLOUR` | colour button | `text`, `#aarrggbb` | opens the colour dialog; the button shows the value |
| `APEX_UI_BUTTON` | push button | none | reports through `on_click` |
| `APEX_UI_TABLE` | list of rows | `text` | `options` are the rows; `text` the selected row |
| `APEX_UI_MATERIAL` | editable picker | `text` | material name; `options` are suggestions |
| `APEX_UI_MODEL` | editable picker | `text` | model path |
| `APEX_UI_SOUND` | editable picker | `text` | sound path |
| `APEX_UI_SECTION` | collapsible group | none | `children` / `child_count` hold nested controls |

Only `APEX_UI_SECTION` may have children. Set `struct_size` on every control and on every value, and always
fill `kind`; a description the host cannot read is rejected as a whole and the previous widgets stay.

**Re-describing.** Call `refresh(host, id)` whenever your state changes (also from inside `on_change` or
`on_click`). The host queues it to the event loop and coalesces repeated requests, then calls `describe`
again. The host matches controls by `key` and updates them in place, so widgets, the cursor and keyboard
focus survive a re-describe. A control whose `kind` changes is rebuilt, and one whose key disappears is
removed. Describing a value the user has just typed back unchanged is a no-op; describing a different one
replaces what the user typed, so keep the state your form reports.

**Panels.** `register_panel(host, id, title, user, callbacks)` is available only during `activate`. `id` must be
`plugin.<plugin id>.<name>`. The panel appears in every area's role menu under `title`, lives in the same
dockable area system as the built-in panels and is saved in the layout by `id`. If a saved layout names a
panel whose plugin is missing, disabled or failed to load, the area shows a placeholder naming the panel
and the layout keeps the entry; when the plugin is loaded again the placeholder becomes the real panel.
Plugin panels dock in the main window; they cannot yet be popped out as separate floating windows.

**Sections.** `register_section(host, id, title, target, user, callbacks)` (also `activate` only) adds a titled
group to a built-in panel. `target` is `"materials"`, `"inspector"` or `"face_properties"`; sections are not
area roles and are not saved in layouts. The host calls `describe` with an `apex_ui_context` that names the
panel's current selection and again whenever it changes:

| Target | `context->kind` | Selection |
|--------|-----------------|-----------|
| `materials` | `"materials"` | `names`: the selected material names |
| `inspector` | `"entities"` | `ids`: the selected entity node IDs |
| `face_properties` | `"sides"` | `ids`: the selected side IDs |

Context arrays are borrowed until `describe` returns. An empty selection gives zero counts; the section stays
visible, so describe what it should show then.

**Dialogs.** `show_dialog(host, title, user, callbacks, &accepted)` opens a modal OK / Cancel dialog built from
`describe`, valid only from inside one of your callbacks or `activate`. `on_change` and `on_click` run while it
is open; the call returns when the user closes it and `*accepted` is 1 for OK. Read your own state to find out
what to apply, since the host only reports the decision.

**Errors and failure.** Calls return `APEX_ERR_INVALID` for a bad id, target or table, `APEX_ERR_STATE` outside the
allowed window, `APEX_ERR_NOT_FOUND` for an unknown id and `APEX_ERR_UNSUPPORTED` when the host has no UI
(for example a headless run). An exception, or a non-OK status from `describe`, leaves the existing widgets
as they were; `last_error` holds the detail.

### 4.6 `apex.overlay/1`: lines, points, labels, ghosts and face tints

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
`docs/plans/plugins.md` (probe P4); keep updates to the rate a human can read.

### 4.7 `apex.helpers/1`: FGD helper drawing

Fetch `"apex.helpers/1"`. In `activate`, `register_helper(host, name, user, draw)` claims an FGD helper name
(for `mywidget(5)` the name is `mywidget`). Only names the host does not draw itself are accepted:
`APEX_ERR_INVALID` for a built-in helper, a duplicate or a name outside `activate`. For each entity whose class
lists the helper, `draw(user, call, sink)` receives the entity, class, helper name, argument text, origin and
angles, and pushes world segments and labels through the sink. Reading the document through `apex.document` is
valid inside `draw`. The output is cached per document revision: helpers do not follow a live drag, and they
are shown only while the viewport's "Entity helpers" overlay is on. A draw that throws or returns an error
contributes nothing for that entity.

### 4.8 `apex.problems/1`: Problems checks

Fetch `"apex.problems/1"`. In `activate`, `register_check(host, id, name, user, check)` adds a check
(`id` is `plugin.<yourid>.<name>`). Whenever Problems refreshes, `check(user, sink)` runs on the editor thread and
reports `{severity (0 error, 1 warning, 2 note), text, objects}` through the sink; a bad severity, an object
that is not in the document or a bad `struct_size` rejects that one report. The Problems list groups plugin
reports under a header per plugin (Apex's own come first), and clicking one selects its `objects`. A check that
throws or fails reports one warning ("failed") and none of its partial results. Registration closes once the
first Problems run starts, so register in `activate`.

### 4.9 Later interfaces *(pending)*

Importers, jobs and file watching (0.9.5b), VFS and assets (0.9.6), scripts (0.9.7), pies and themes
(0.9.8).

## 5. The C++ wrapper

`plugin.hpp` provides:

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

- `apex::plugin::overlays(host)`, `helpers(host)`, `problems(host)`: interface lookups.
- `apex::plugin::Overlay`: vectors `segments`, `points`, `labels`, `ghosts`, `tints` with `clearLists()`;
  `declareOverlay(host, id, name)` in `activate`, then `setOverlay(host, id, overlay)` and `clearOverlay(host, id)`.
  Strings and ghost planes you point at must stay alive until `setOverlay` returns.
- `apex::plugin::Helper`: derive and override `draw(call, sink)`, using `Helper::segment(sink, ...)` and
  `Helper::label(sink, ...)`; `registerHelper(host, name, helper)`. Exceptions are caught at the C boundary.
- `apex::plugin::ProblemCheck`: derive and override `check(sink)`, report with
  `ProblemCheck::report(sink, severity, text, objects)`; `registerProblemCheck(host, id, name, check)`.

## 6. Examples

All are built by the main CMake project and placed under `<build>/plugins/<id>/` with a generated manifest.

- [`examples/hello-tool/hello.c`](../examples/hello-tool/hello.c): plain C. Logs on activate, reads the
  document through `apex.document/1`, registers tool `plugin.hello.tool` and action
  `plugin.hello.read_document` with a suggested Hammer-preset binding on F24.
- [`examples/polygon-tool/polygon.cpp`](../examples/polygon-tool/polygon.cpp): C++ using `plugin.hpp`. A
  click-to-place polygon tool that draws overlay segments and a ghost and commits through `apex.edit/1`.
- [`examples/face-stats/face_stats.cpp`](../examples/face-stats/face_stats.cpp): C++ using `plugin.hpp`'s `Form`.
  Registers a section under Face Properties that shows how many faces are selected, their side ID range and
  the number of solids in the map, and refreshes itself from `on_changed`.

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
new fields appended to existing structs bump the minor once the ABI is frozen; during 0.9 the minor stays 0
(0.9.4 and 0.9.5a added `apex.ui`, `apex.overlay`, `apex.helpers` and `apex.problems` without bumping it). Always set and honour `struct_size`, and treat a
`NULL` from `get_interface` as "not available on this host".
