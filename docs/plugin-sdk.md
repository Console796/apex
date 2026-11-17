# Apex plugin SDK (DRAFT)

> **Status: draft for 0.9.9.** Written against the ABI through 0.9.7 (script plugins). 0.9.8 adds more interfaces. Sections
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
| `script`, `runtime` | A script plugin (section 4.14) names a script file inside its folder and the runtime that runs it, instead of `entry`. A plugin has `entry` or `script`, never both. |

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

### 4.9 `apex.io/1`: importers

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

### 4.10 `apex.jobs/1`: background work

`submit(host, user, work, done, &job_id)` runs `work(user, job_context)` on a worker thread and then calls
`done(user, status)` **on the editor thread**, never on the worker. `work` returns an `apex_status`, and must not
call host interfaces other than checking `job_context->cancelled(job_context->job)` (non-zero once the job was
cancelled). Do not touch the document or any shared state from `work` without your own locking: read what you need
before submitting.

- `submit` and `cancel` are thread-safe. `submit` is `APEX_ERR_STATE` once the host is shutting down.
- `cancel(host, job_id)` returns `APEX_OK` if the completion had not been delivered; `done` is then never called
  for that job. It returns `APEX_ERR_NOT_FOUND` for a finished (delivered) or unknown job.
- If `work` throws, `done` receives `APEX_ERR_INTERNAL`. An exception in `done` is caught and logged.
- At shutdown the host cancels, joins and discards pending jobs before the plugin is unloaded, so `done` is not
  called then. The `user` pointer must stay valid until `done` runs or the job is cancelled.

The host interface is Qt-free: the application calls `Host::drainJobs()` regularly (the Qt shell does so from a
50 ms timer, `PluginPump`). A host embedded elsewhere must call it from its own main thread.

### 4.11 `apex.watch/1`: file watching

`watch(host, path, user, on_changed, &watch_id)` watches an absolute path to a file or directory (a directory is
watched recursively, up to 10,000 entries); `unwatch(host, watch_id)` stops it. Allowed during `activate` or any
callback into the plugin. The path must lie inside the plugin's own folder or a root the host was given (the game
directory); otherwise the call returns `APEX_ERR_INVALID`.

`on_changed(user, path)` is delivered on the editor thread **once per save**: the host polls size, modification
time and, for directories, the entry set, and delivers only after the path has been quiet for the debounce
period (200 ms by default). A program that writes a file in several pieces, or a burst of saves inside the window,
is one delivery. A separate save afterwards is another. Creating or deleting the watched path counts as a change.
Callbacks may call `unwatch`. Polling means a very coarse filesystem mtime (1 to 2 s on some filesystems) can merge
two saves made within that resolution into one delivery. `Host::pollWatches()` must be called regularly (the Qt
shell uses the same `PluginPump` timer).

### 4.12 `apex.vfs/1`: serve files into the game's file system

`register_provider(host, id, name, provider, position)` is available only during `activate`. `id` is
`plugin.<plugin>.<name>`; at most 16 providers exist in a session; a duplicate id returns `APEX_ERR_INVALID`; a
call after `activate` returns `APEX_ERR_STATE`. The provider becomes a mount of the editor's virtual file system
(Vfs), next to the game, its addons and its archives, so what it serves reaches the material browser, thumbnails,
model and texture loading and the 3D view like any other file.

- **Order.** Earlier mounts win. `position` is `first`, `last` (the default), `before` or `after` an anchor, a
  case-insensitive substring of another mount's folder or archive path. An anchor that matches nothing places the
  provider last and is logged. Providers at the same position keep registration order.
- **Calls.** `list(user, file_sink)` adds every file the provider has (relative paths, `/` separators, any case; at
  most 200,000 paths of 1,024 bytes each). `refresh(user)` is optional and runs before each listing. `open(user,
  path, data_sink)` writes the file's bytes with `data_sink->write`; stop at `data_sink->max_bytes`. Apex lists
  once per load or refresh and then answers `exists`, `list` and directory walks from that index, so only `open`
  reaches your code afterwards.
- **Threads.** The host calls providers from worker threads, concurrently (`open` may run on several threads at
  once; `list` and `refresh` never overlap an `open` of the same provider). Make `open` thread-safe, do not call
  host interfaces from a provider call, and do not rely on the crash breadcrumb: it is not set for provider calls,
  so a plugin that crashes inside one is not named at the next start.
- **Failures.** Return a non-`APEX_OK` status or throw to say "no such file" or "no files"; neither crashes or
  hangs Apex. Each call is bounded (5 s by default). A call that exceeds it fails, and the provider is taken out of
  service until it has returned and the content is next reloaded. The stuck call keeps running on its own thread;
  Apex does not kill it. If it is still running at shutdown, the plugin library is left loaded instead of unloaded.
- **Changes.** `request_refresh(host, id)` (editor thread, your own provider only) reloads the game content so the
  provider is listed again. It is `APEX_ERR_STATE` when the host has no content to reload.
- **Compiling.** The compile tools (vbsp, vvis, vrad) read the real file system and cannot see a provider. Provider
  mounts are left out of the compile gameinfo; the compile panel and the log name each omitted provider.

### 4.13 `apex.assets/1`: materials

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

### 4.14 Script plugins

A script plugin has no native library. Its manifest names a file and a runtime:

```
{ id = "stair" name = "Stair" version = "1.0.0" apex_abi = { major = 1 minor = 0 }
  script = "stair.as" runtime = "angelscript" }
```

`script` is a path inside the plugin folder (absolute paths and `..` are refused) and the file must exist.
The host asks the build's script engine factory for `runtime`. The build option `APEX_WITH_ANGELSCRIPT`
(default ON) compiles the AngelScript engine (v2.38.0, `plugin_host/angelscript_engine.hpp`); if it is OFF, or
the runtime is unknown, the plugin stays listed with an error that names the runtime and the other plugins
load normally.

A script registers through the same `apex.*` interfaces a native plugin uses, as the script's own plugin id,
so the namespace and lifetime rules above apply unchanged. The ABI is unchanged: no new interface and no
struct changes.

**Reload on save.** The host watches the script file (the `apex.watch/1` mechanism, delivered by
`pollWatches`, 200 ms debounce). A save runs: retire the old version's live tool instances (their
`destroy_instance` runs), neutralise every callback it registered, unload the old engine, then load the new
file into a new engine. Registry entries cannot be removed, so an id that exists before and after keeps its
entry and is rebound: open sessions keep their tool objects, which get a fresh instance and, if they were the
active tool, `activate` again. Limits: an id the new version drops stays as an inert entry until restart; an
id it adds works while the registries are open and is otherwise reported in the log as available after a
restart; a tool that changes kind is also restart-only.

**Errors.** Every call into a script runs with exceptions caught. A compile error, a failing or throwing load
sets the plugin's error (shown in the plugin manager and the log, with the engine's message), leaves the
plugin unloaded and its tools inert, and never takes down the editor. A failed reload keeps watching, so the
next save that fixes it loads normally.

**The `apex::` API (AngelScript).** A script is one file (no `#include`) with a `void main()` that runs at
load; registration is only allowed there. Tool and action ids are `plugin.<plugin id>.<name>`.

- Registration: `apex::registerTool(name, title, description, "ClassName")` (the class is instantiated per
  tool instance) and `apex::registerAction(name, title, description, ActionFn@)`.
- Types: `apex::Vec3`, `apex::Plane`, `apex::Event`, `apex::View` (input and camera ray), `apex::Visual`
  (ghost solids from planes, outlines), `apex::Readout` (status text and declared parameters), `apex::Edit`
  (a batch: `createSolid(...)` and friends, then `commit("name")`, one undo step).
- Document and selection reads: `documentOpen`, `revision`, `rootCount`, `root`, `kind`, `parent`,
  `childCount`, `child`, `keyCount`, `keys`, `value`, `selectionMode`, `selection`, `setSelection`.
- Logging: `apex::log`, global `print`; the AngelScript math add-on (`abs`, `floor`, `sqrt`, ...).

A tool class implements any of these optional methods (found by declaration): `bool input(apex::Event@,
apex::View@)`, `void visual(uint64, apex::Visual@)`, `void readout(apex::Readout@)`, `bool setParameter(const
string &in, double)`, `bool perform(const string &in)` (receives the full action id), `void cancel()`,
`void documentChanged()`, `void forget(uint64)`, `void activate()`, `void deactivate()`.
To the session a script tool is a normal tool (same adapter as a native one); the polygon scenarios of the
native tool run unchanged against a script version.

**Limits and errors.** Loading has a 5 s budget and each call into the script 250 ms; a watchdog aborts a
script that exceeds it (the message names the section, line, and the budget) and the tool keeps working on the
next call. A loop with an empty body has no point where the watchdog can interrupt it and cannot be stopped.
Compile errors are reported as `file:line:col` and runtime exceptions (such as an out-of-range index) are
contained: both go to the log and the plugin's `last_error`, never up into the editor.

**Example: `examples/stair/`.** `plugin.kv3` is generated and `stair.as` is installed by CMake next to the other
examples (`<build>/plugins/stair`). The `plugin.stair.tool` tool aims at the ground plane, snaps to an 8-unit
grid, shows one ghost per step plus a footprint outline, exposes a `steps` parameter (1 to 64, default 8, set
from the readout), and a left click commits all steps in one `apex::Edit` as a single undo step.
Measured on linux-gcc-release at 64 steps: about 0.77 ms per input + refresh (plugins.md section 10, P5).

### Pies, suggested keys and themes (0.9.8)

There is no new C interface and no ABI change: all three use data a plugin already ships.

- **Pies.** A pie layout lives in the user's preset (`pies` in the preset file), keyed by pie id (`pie.tools`,
  `pie.objects`), and is edited on the Pies page of Settings. A preset with no entry for a pie uses the built-in
  layout. An item may name any action id, including `plugin.<id>.<name>`. While the plugin is not loaded that
  item is hidden when the pie opens and shown greyed on the Pies page, but it stays in the preset and reappears
  when the plugin is back. Constraints: slot 0-7 (N, NE, E, SE, S, SW, W, NW), no two items in one slot, at most
  8 items. The preset store refuses to save a layout that breaks them. Plugins cannot add pie items themselves.
- **Suggested keys.** `suggestedBindings` and `suggestedContexts` on an action are keyed by preset name. The
  Keybindings page lists the action under "Plugin: <name>" with the key in italics as "<key> (suggested)".
  A suggestion is not written into the preset; a binding in the preset, even "none", replaces it. If the key
  is already used in the same context the action shows no key and the tooltip names the owner. Suggestions
  exist only for the presets they name, so a user preset copied from one gets none.
- **Themes.** A plugin ships themes as `themes/*.json` in its folder, in the same format as the user's custom
  themes (`name`, `base`, `colors`). Enabled, loaded plugins' themes are registered as `plugin:<plugin id>:<file
  stem>` and listed in the theme choice as "<name> (plugin <plugin name>)". They are read-only (Edit makes a
  copy as a custom theme). A file that does not parse is logged and skipped. If the plugin is absent or
  disabled its themes are not registered and a saved choice falls back to the default theme.

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

- [`examples/obj-import/obj_import.cpp`](../examples/obj-import/obj_import.cpp): C++ using `plugin.hpp`'s
  `Importer`. Imports a Wavefront OBJ as one solid per `o`/`g` group (each group must be a closed convex part;
  face planes are oriented outward from the group's centre, so winding does not matter). A bad group fails and
  undoes the whole import. File > Import, extension `.obj`.

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
