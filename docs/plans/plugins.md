# Apex: plugin interface design for 0.9

**Status: draft, 2026-11-16. The owner accepted every recommendation on 2026-11-16, including the four
owner decisions (D1, D8, D10, D12); godot-acoustics is one input among others (§9).** This is the cross-milestone design for the plugin interface, written
ahead of the 0.9 milestone plan, which adds the sub-milestones' detail and owns the release gates. D11 stays
gated on probe P2, and the probes in §4 can still reverse a decision.

"Plugin" here means Apex's own extensions: native code or scripts that add tools, panels, importers and
so on. It is not a Strata **content addon** (a folder of game content; [addons.md](addons.md)), which
Apex has mounted since 0.3. addons.md D10 renamed the roadmap's 0.9 "addon manager" to "plugin manager"
for that reason, and this document keeps the word "plugin" throughout, including in the API's names.

Working design for the milestone [roadmap.md](../roadmap.md) describes as:

> **0.9: Extensibility.** Plugin API (stable C ABI with a header-only C++ wrapper), a scripting host
> (language decided in the 0.9 plan), custom tools/panels/importers, keymap editor for the Hammer and
> Blender presets (0.2 D15), theme editor, plugin manager, scriptable and persistent parametric primitives
> (including Strata Hammer's AngelScript ones; 0.3 D6), user-editable pie menus (0.3 D20).
> **Exit:** a third-party tool can be built, loaded and shipped without rebuilding Apex.

research.md §2 item 9 ("Plugin API for tools, panels, importers and entity helpers, plus a scripting
layer") and §6 (the scripting options) are the design this refines. Where they disagree with this
document, they win until the 0.9 release applies amendments.

## 1. What 0.9 owes earlier plans

- **Tools through the existing interface** (research.md §6; 0.3 D1: "the shape the 0.9 plugin API exposes
  for custom tools"). Plugin tools are `editor::Tool`s; D6.
- **A Qt-free tool surface** (0.2 D1: the `editor` layer is Qt-free partly so "the 0.9 plugin API's tool
  surface" is too). D6 keeps it that way.
- **The keymap editor**, with 0.2 D15's `Preset`/`Keymap` registry as its data model (0.2 §4). D7 covers
  how plugin actions appear in it; the editor itself is not plugin-interface work (§7).
- **User-editable pies and pie layouts in presets** (0.3 D20, 0.4 D13's "More" level). D7.
- **Re-editable parametric primitives and Strata's scriptable primitives** (0.3 D6, 0.3 appendix). D11,
  gated on probe P2.
- **Two-way game link: console control, reload on save, debugger attach** (0.5 D9 and §4: "plugin
  territory"). A test case for D5 and D9, not core work.
- **Polygon tool, sprinkle, physics drop** (0.8 §4: "candidates for 0.9 as plugins; they are good tests of
  the plugin API"). Candidate example plugins (§6).
- **The Godot/brush-acoustics requirements P1–P5** ([godot-acoustics.md](godot-acoustics.md)). §5 maps
  each one to an extension point; it is the only external consumer with written requirements.

## 2. What 0.9 inherits

Symbols, not line numbers, because the 0.8 branch is still moving.

- **`editor::Tool`** (`editor/tool.hpp`): `input(InputEvent, ToolView)`, `preview()`, `overlay()`,
  `readout()` with `ToolParameter`s, `cancel()`, `perform(ActionId)`/`actions()`, `documentChanged()`.
  Its types carry `std::string`, `std::optional`, `std::variant` and `std::function`
  (`ToolView::rectFrustum`), so none of it can cross a C ABI as is.
- **`EditorSession::edit(label, fn(Transaction&), coalesceKey)`**: the one write seam. It captures
  before/after selection; writes that skip it (straight `Document::begin()`) lose selection undo. The
  session keeps tools in `std::map<ToolId, unique_ptr<Tool>> tools_`, filled at construction.
- **`document::Transaction`**: a finite op list: `create(kv1 block, parent)`, `destroy`, `reparent`,
  `setKeyValue`, `setKeyValueOccurrence`, `setEntityKeyValueEntry`, `setConnections`, `setEditor`,
  `setSideKeyValue`, `setDisplacement`, `removeDisplacement`, `setSolidGeometry`, `rebuildSolid`,
  `createFragment`, `commit`.
- **Identity.** `NodeId` (uint64) and `SideId` (int) are stable across edits and undo. `NodeHandle`s and
  `Node*` are invalidated by arena mutations.
- **Change notification.** `Document::observe(Observer)` where `Observer` is `std::function<void()>`, no
  payload; `Transaction::commit()` returns a `ChangeSet` of **handles**. Observers run synchronously on the
  single writer thread and must not mutate the document. `historyState()` is a stable revision identity.
- **Actions and keymap** (`editor/actions.hpp`): `actionRegistry()` is a static, declaration-ordered list;
  `Keymap` throws `invalid_argument` on any validation problem; `Binding` already has `Unbound{reason}` and
  `provisional`.
- **Panels.** `createPanel(AreaRole, …)` in `ui/area_frame.cpp` is an enum switch over built-in Qt
  widgets. The command palette has providers (`ui/command_providers.hpp`).
- **Overlays.** `ToolOverlay` (screen segments/triangles, world segments/points/triangles, labels, a
  rectangle) and `Preview` ghosts; 0.3 D16 world labels.
- **VFS** (`assets/vfs.hpp`): main-thread owner, "earlier mounts win"; `walkTask()` returns a closure that
  **runs on workers** while the VFS is immutable.
- **Layers** (`tools/check_layers.py`): `core` → `document`/`geometry` → `assets` → `render` →
  `viewport` → `editor` → `ui` → `app`; Qt only in `ui`/`app`.
- **Already built, so not 0.9 scope:** a theme editor (`ui/theme_editor.hpp`, `ThemeEditorDialog`, since
  0.3.4). The roadmap's "theme editor" item reduces to letting plugins ship themes (D8).

## 3. Decisions

### D1: In-process native plugins behind a C ABI; the scripting host is itself a plugin (owner: accepted)

A plugin is a shared library (`.so`/`.dll`) loaded into the Apex process. It talks to Apex only through
`apex_plugin.h`, a C header with no includes beyond `<stdint.h>`/`<stddef.h>`. The scripting host (D10)
is a plugin shipped with Apex and built against the same header, so scripts get nothing native plugins
can't, and the ABI is exercised by Apex's own code from day one.

Why in-process: tools receive every pointer move and return an overlay every frame; viewport overlays and
per-face tints are read by the renderer each frame; P1's VFS provider answers file opens during loads.
An IPC hop per call costs more than those budgets allow, and every extension point would need a
serialisation format.

Cost: a crashing plugin takes Apex down with it, and a plugin has the user's full privileges. Mitigation:
autosave already exists (0.2); the plugin manager records which plugin was in a callback when the process
died (a breadcrumb file written around each callback) and offers to disable it on the next start; plugins
are only loaded from folders the user installed into (D9), and are off until enabled.

*Alternative:* out-of-process plugins over a pipe (the LSP / VS Code extension-host model). Crash
isolation and a natural sandbox, but per-frame tool input and overlays become round trips, and P1 would
block file loads on IPC. Could be added later for *non-interactive* kinds (importers, problem checks)
behind the same manifest without changing the in-process ABI.

### D2: ABI shape: one entry point, versioned interface tables, append-only structs

```c
/* apex_plugin.h (sketch) */
#define APEX_PLUGIN_ABI_MAJOR 1

typedef struct apex_str { const char* data; size_t size; } apex_str;   /* UTF-8, not NUL-terminated */
typedef enum apex_status { APEX_OK = 0, APEX_ERR_INVALID, APEX_ERR_NOT_FOUND,
                           APEX_ERR_STATE, APEX_ERR_UNSUPPORTED, APEX_ERR_INTERNAL } apex_status;

typedef struct apex_host {
    uint32_t struct_size;            /* sizeof as compiled by Apex */
    uint32_t abi_major, abi_minor;
    /* Interface lookup: "apex.document/1", "apex.tools/1", "apex.vfs/1", ... NULL if absent. */
    const void* (*get_interface)(apex_str name);
    void (*log)(void* plugin, int level, apex_str message);
    apex_str (*last_error)(void* plugin);
} apex_host;

typedef struct apex_plugin {
    uint32_t struct_size;
    uint32_t abi_major;              /* must equal the host's */
    void* user;                      /* passed back on every callback */
    apex_status (*activate)(void* user);   /* register extension points here */
    void (*deactivate)(void* user);
} apex_plugin;

/* The only exported symbol. */
APEX_PLUGIN_EXPORT const apex_plugin* apex_plugin_entry(const apex_host* host);
```

Rules:
- **Versioning.** `abi_major` must match exactly; a mismatch is refused with a message naming both
  versions. Within a major, structs only grow at the end, and both sides read `struct_size` before touching
  a later field (the Vulkan `sType`/`pNext` lesson without the chain). Each interface table is named with its
  own version (`apex.document/1`), so one area can get a `/2` without bumping the whole ABI.
- **Strings** cross as `apex_str` (pointer + length, UTF-8). **Memory:** whoever allocates frees. Data the
  host hands to a callback is valid until the callback returns; data a plugin returns is valid until the
  plugin's next call into the same interface or an explicit `release`. No C++ types, no exceptions, no
  `std::function` cross.
- **Errors** are `apex_status`; detail via `last_error`. The C++ wrapper turns them into exceptions or
  `std::expected` (D3), and catches everything at the boundary; an exception reaching the host is a bug the
  host reports and treats as `APEX_ERR_INTERNAL`.
- **Callbacks** take the plugin's `void* user` first, then a pointer to an args struct (with `struct_size`),
  so arguments can be added without new function signatures.

*Alternatives:* a C++ ABI (stable only with one compiler and standard library; rules out MSVC/GCC mixes and
the roadmap's "stable C ABI"); a COM-style `IUnknown` vtable (equivalent power, more ceremony in C).

### D3: The C++ wrapper is header-only, ships with the SDK, and includes nothing from `src/`

`apex/plugin.hpp` wraps the C header: RAII handles, `std::string_view` for `apex_str`, `std::expected`
returns, and base classes (`apex::plugin::Tool`, `Panel`, `VfsProvider`, …) whose virtuals the wrapper's
generated trampolines call. It deliberately mirrors the names of the internal types (`InputEvent`,
`ToolOverlay`, `ToolParameter`) but is a separate definition: internal types stay free to change, and the
host-side adapters (D6) translate.

The SDK is `apex_plugin.h`, `apex/plugin.hpp`, a CMake package (`find_package(ApexPlugin)` giving an
`apex::plugin_sdk` interface target and an `apex_add_plugin()` helper that sets visibility, the export
macro and the output layout of D9), and the example plugins (§6). It installs with Apex and is also a
standalone archive, so a plugin builds without an Apex source tree, which is the exit criterion.

Layering: a new `plugin_host` layer under `src/plugin_host/` depends on `editor` and `assets` (and owns the
adapters); `ui` depends on it for panels and the plugin manager. `check_layers.py` gains the layer and a
rule that `include/apex/` includes nothing from `src/`. The public headers live in `include/apex/`, outside
`src/`.

### D4: Threading: plugins are called on the main thread, except where an interface says otherwise

Every callback runs on the main (document writer) thread unless its interface documents otherwise, and a
plugin may call host functions only from the thread it was called on, plus the explicitly thread-safe
ones (logging, `apex.jobs/1` submission, reading a document snapshot).

The exception is **VFS providers** (D5, P1). `Vfs::walkTask()` returns a closure that runs on workers
while the VFS is immutable, and material/thumbnail indexing depends on it. So `apex.vfs/1` providers must
be thread-safe for `exists`/`open`/`list`/`walk`, and they get a `refresh` call on the main thread when the
host refreshes mounts; the host promises no `refresh` while a walk is outstanding, which is the contract
`walkTask` already has. This is godot-acoustics §5 probe 3, and P3 below checks it before 0.9.x commits
to it.

Long work goes through `apex.jobs/1` (the existing `core::JobPool`), with completion delivered back on the
main thread.

### D5: Document access: read by ID, write only through the session's `edit()`, notify with ID change sets

**Read** (`apex.document/1`): enumerate roots and children by `NodeId`; a node's kind, classname, keyvalues
(including duplicate keys, by occurrence) and its KV1 block; a solid's sides by `SideId` with plane,
material, UV axes and the derived polygon; `exportSubtree(NodeId)` as VMF text. Never a pointer or handle:
both are invalidated by arena mutations. `historyState()` is exposed as the revision, which answers
godot-acoustics P3's "revision counter". A worker can read a snapshot (`Document::snapshot()`) handed to it
through `apex.jobs/1`.

**Write** (`apex.edit/1`): `begin_edit(label, coalesce_key)` → a set of ops that map 1:1 onto
`Transaction`'s list (create a KV1 block under a parent, destroy, reparent, set keyvalue, set side keyvalue,
set connections, set/remove displacement, set solid geometry from planes, rebuild with correspondence) →
`commit`. The host runs the whole batch inside **one `EditorSession::edit()` call**, so a plugin edit is
one undo step, captures selection like built-in tools, gets sidelist upkeep (0.8 D11) and coalescing for
free. A plugin can't open a raw `Document::begin()`. An op that throws inside the transaction aborts the
batch, and the plugin gets `APEX_ERR_INVALID` with the message.

**Notify** (`apex.document/1` `on_changed`): `{revision, created[], modified[] (id + dirty flags),
destroyed[], vanished_sides[], undo_or_redo}` as **IDs**. The current `Observer` has no payload and the
`ChangeSet` holds handles, so the host builds the ID set when the transaction commits (and for undo/redo
from the history delta) and keeps it for the notification. Observers must not mutate the document, so a
plugin that wants to react with an edit calls `defer(fn)`, which runs after the current notification pass,
still on the main thread, as its own undo step.

Plugin-specific data **per document** (P2 below) is not part of 0.9's write API until probe P2 answers where
it can live losslessly.

### D6: Plugin tools are host-side `editor::Tool` adapters

`apex.tools/1` registers a tool with an ID (namespaced, D7), a name, an icon (SVG bytes or a path inside the
plugin folder) and a callback table: `activate`, `deactivate`, `input`, `overlay`, `readout`,
`set_parameter`, `perform`, `cancel`, `document_changed`.

Host side, `plugin_host::PluginTool : editor::Tool` holds that table and translates:
- `InputEvent` → a flat C struct (chord as key name + modifier bits);
- `ToolView` → a C struct with the camera, plus host callbacks for picking (`ray_pick` against the scene
  BVH, `rect_frustum` replacing `std::function`, `world_to_pixel`, grid snap) so plugins pick exactly as
  built-in tools do;
- the returned overlay arrays → `ToolOverlay`; `ToolParameter` lists → the existing parameter strip;
- `Preview` ghosts → a list of convex solids (planes) and segments.

The session gains `addTool(unique_ptr<Tool>)` / `removeTool(ToolId)`; nothing else in the session or panes
learns that plugins exist, because panes already draw what the active tool describes (0.3 D1). Plugin tools
appear in the tool drop-down, the command palette and (D7) pies.

### D7: Plugin actions are namespaced, live in a mutable registry, and a missing plugin leaves bindings dormant

IDs are `plugin.<plugin-id>.<name>` for actions and tools; the host refuses registrations outside the
plugin's namespace. `actionRegistry()` becomes a registry with a static built-in part and a dynamic part
(declaration order kept: built-ins first, then plugins in load order).

A plugin may ship **suggested bindings** per preset. They are applied only where the chord is free; a
conflict leaves the action unbound with `Unbound{"conflicts with <action>"}` instead of throwing, which
`Keymap`'s constructor does today for any problem. A user's saved binding to an action whose plugin is not
loaded stays in the preset file, is skipped when building the `Keymap` (marked `provisional`-like, so it
is not a validation error), and comes back when the plugin does. The keymap editor shows such bindings
greyed with the plugin's name.

Pies (0.3 D20): user-editable pie layouts are preset data (`Preset` gains `pies`), and a pie item is an
action ID, so a plugin tool or action can be placed in any pie with no pie-specific API. A plugin can
suggest a pie of its own; the 0.4 D13 "More" level is where tools beyond the eighth go.

### D8: Panels are declarative in 0.9; a native Qt escape hatch is explicitly unstable (owner: accepted, Qt tier not shipped in 0.9)

Qt types can't cross a stable C ABI, and a plugin compiled against a different Qt minor would crash. So
`apex.ui/1` offers a **declarative form**: the plugin returns a tree of controls (label, text field,
number with range, checkbox, choice, colour, button, list/table of rows, material/model/sound picker,
collapsible section) with IDs and values; the host builds Qt widgets from it with the theme's tokens and
calls `on_changed(control id, value)` / `on_clicked`. The plugin re-describes the form when its state
changes (keyed diffing on the host keeps focus and scroll). It grows from `ToolParameter`, which is already
this pattern for one row.

Where it appears:
- a **panel** (a new `AreaRole::Plugin` carrying the panel ID, so it docks, floats and is saved in layouts
  like built-ins);
- a **section** in an existing panel: the materials panel (godot-acoustics P2), the inspector for a
  classname, face properties. Each host panel names the context it passes (the selected material names;
  the selected entity IDs; the selected side IDs);
- the tool's parameter strip (already the case via `ToolParameter`);
- a modal **dialog** (an importer's options).

Themes: a plugin folder may contain theme files in the theme editor's format; the plugin manager lists them
and the theme picker shows them.

*Alternative, offered as an unstable tier:* `apex.qt/0` hands the plugin a `QWidget*` parent. Allowed only
when the plugin's manifest names the exact Qt version Apex was built with, and the plugin manager says it
may break on any update. Recommended: ship it disabled in 0.9 and decide from demand.

### D9: Packaging, discovery and the plugin manager

A plugin is a folder:

```
<plugin-id>/
  plugin.kv3            # manifest
  bin/linux-x86_64/libfoo.so
  bin/windows-x86_64/foo.dll
  icons/, themes/, scripts/ (script plugins), LICENSE
```

`plugin.kv3` (KV3, the format Strata uses for `addon.kv3`; `core/kv3` already parses it): `id`
(reverse-DNS, lowercase), `name`, `version` (semver), `authors`, `description`, `apex_abi` (major, minimum
minor), `entry` per platform or `script` + `runtime` for script plugins, `provides` (a summary of the
extension kinds, so the manager can show it before loading), `dependencies` (other plugin IDs; no version
solving in 0.9).

Search paths, earlier wins on duplicate IDs: the user plugin folder (`~/.local/share/apex/plugins`,
`%APPDATA%\Apex\plugins`), then the bundled folder next to the binary (the scripting host, examples). A
project/profile-level folder is deferred: it would load native code because a map's folder says so.

The plugin manager (a panel) lists installed plugins with their manifest, enabled state, load errors and
the crash breadcrumb (D1); enables/disables; installs from a `.zip`/folder by copying into the user folder.
**Plugins load at startup only** in 0.9: enabling or disabling takes effect after a restart. Unloading a
library while its tools, panels, actions and callbacks may be referenced is the hard part of hot reload,
and it isn't needed for the exit criterion. Script plugins (D10) can reload live, because the script host
owns their registrations.

No Workshop or online catalogue in 0.9.

### D10: The scripting host: AngelScript in 0.9 (owner: accepted)

research.md §6 lists three. Recommendation: **AngelScript**, because:
- it is the only option with a compatibility payoff on the roadmap: Strata Hammer's scriptable primitives
  are `hammer/scripts/*.as` (D11), and mirroring that API means maps' primitives can be re-edited in both
  editors;
- it embeds as a small static library on every platform and brings no runtime to package (relevant for
  0.10's AppImage/Flatpak/Windows installers);
- its typed, C++-like syntax maps closely onto the C++ wrapper, so docs and examples can be shared.

The script host is a bundled plugin (D1) that exposes the same interfaces to scripts, with script tools,
actions, panels and importers registered under the script plugin's ID. Scripts reload on save.

*Alternatives:* **Python** (the srctools ecosystem, which the community uses for map automation; the best
batch-processing story; but an embedded CPython adds about 30–60 MB to installers, the GIL meets D4's
threading rules awkwardly, and nothing loads Strata's `.as` files) and **Lua** (smallest and fastest to
embed; no ecosystem tie to Source mapping). A Python host could be a third-party plugin later on the same
ABI, which is an argument for keeping the ABI complete rather than for picking Python first.

### D11: Persistent parametric primitives depend on probe P2

0.3 D6 committed primitives as plain solids because parameters would need Apex-only keys (0.1 D19 forbids
them). The way out it named is Strata's own format. If P2 shows Strata Hammer stores scriptable-primitive
parameters in the VMF in a form Apex can read and write losslessly, 0.9 adopts that format: a primitive is
a script (D10) with typed parameters, generates solids through `apex.edit/1`, and is re-editable from the
inspector in both editors. If P2 shows no such encoding, primitives stay "adjust last operation" (0.3 D6),
and plugin primitives get the same treatment; nothing Apex-only is written to the VMF.

### D12: ABI stability promise before 1.0 (owner: accepted, frozen at the 0.9 release)

Recommendation: `abi_major` 1 is frozen at the 0.9 release for additions-only within 0.x; a break before
1.0 needs a new major and is listed in the release notes, and Apex keeps loading the previous major for one
milestone if the adapters make that cheap. At 1.0 the promise becomes "major 1 for the life of 1.x".
*Alternative:* no promise until 1.0 (cheaper, but "shipped without rebuilding Apex" would then only hold
within one release).

### D13: The extension points in 0.9

| Kind | Interface | Notes |
|---|---|---|
| Tools | `apex.tools/1` | D6. |
| Actions and commands | `apex.actions/1` | D7. Appear in menus (a Plugins menu, or a named existing menu), palette, keymap, pies. |
| Panels, sections, dialogs | `apex.ui/1` | D8. |
| Viewport overlays | `apex.overlay/1` | World lines, boxes, points, labels and per-face tint, outside any tool, with a per-viewport toggle in the pane header. Builds on `ToolOverlay`; per-face tint is new render work (godot-acoustics P4 and core item B share it). |
| Document read/write and change notices | `apex.document/1`, `apex.edit/1` | D5. |
| Selection | `apex.selection/1` | Read and set selection by `NodeId`/`SideId`; change notice. |
| Importers and exporters | `apex.io/1` | File-extension handlers: import produces KV1 blocks inserted through one edit (or a new document); export receives a read-only snapshot. |
| VFS providers | `apex.vfs/1` | D4 threading rules; a provider declares its mount position relative to the profile's mounts. Compile gameinfo (`compileGameInfo`) leaves provider mounts out and says so in the compile log. |
| Problem checks | `apex.problems/1` | Add entries to the Problems list (0.5) with an object ID and an optional fix action. |
| Entity helpers | `apex.helpers/1` | Draw for an FGD helper name the host doesn't know (world primitives, like overlays). |
| File watch | `apex.watch/1` | Notify on changes to paths inside the game directory or the plugin's folder (godot-acoustics P5). |
| Jobs | `apex.jobs/1` | D4. |
| Asset reads | `apex.assets/1` | Read VFS files, the material index (including `surfaceProp` once core item A lands), FGD classes; write a VMT through Apex's VMT writer and refresh its index entry (P2). |

Not in 0.9: custom renderers or shaders, editing the compile pipeline beyond adding a compile step command,
custom VMF block kinds, network access APIs.

## 4. Open probes

- **P1: ABI round-trip on both compilers.** Build the example plugin with GCC and Clang on Linux and with
  MSVC and clang-cl on Windows against an Apex built with the other compiler; load, run its tool, unload at
  exit. Answers whether D2's rules are enough in practice.
- **P2: Strata scriptable-primitive storage.** Make a scriptable primitive in Strata Hammer, save, and read
  the VMF: are the script name and parameters stored, where, and does Strata Hammer regenerate or keep the
  solids on load? Does an Apex save of that VMF (unchanged) round-trip it? Decides D11.
- **P3: VFS provider in mount order.** A test provider placed between profile mounts: `walkTask` on
  workers, `refresh` on the main thread, the material index and thumbnails see its files, and "earlier
  mounts win" holds. Decides D4's VFS rule (godot-acoustics §5.3).
- **P4: Per-face tint cost.** A per-face colour override for every face in the largest test map, updated
  at 10 Hz; frame time against the 0.10 budgets. Sizes `apex.overlay/1`'s tint.
- **P5: Script host size and speed.** AngelScript embedded with the D13 interfaces bound: binary size, and
  a primitive script regenerating 64 solids within 0.3 D12's 2 ms preview budget.

## 5. Acceptance: godot-acoustics against D13

godot-acoustics is one consumer among others (owner, 2026-11-16): a plugin the owner intends to build, so this
table checks the API covers it, but it doesn't define 0.9's acceptance. The example plugins (§6) do.

| Requirement | Covered by | Gap |
|---|---|---|
| A: keep `surfaceProp` in the material index | Core change, any milestone | None for the API; `apex.assets/1` exposes it. |
| B: colour-by-surfaceprop view mode | Core change; shares per-face tint with `apex.overlay/1` | P4. |
| P1: material and texture provider | `apex.vfs/1` | P3; and either a PNG path in the texture loader or VTF encoding in the provider (godot-acoustics P1, unchanged). |
| P2: material panel section | `apex.ui/1` section in the materials panel + `apex.assets/1` VMT write | The section's context is the selected material names. |
| P3: read-only document access with changes | `apex.document/1` | None. `historyState` is the revision. |
| P4: viewport draw and per-face tint | `apex.overlay/1` | P4. |
| P5: file watch | `apex.watch/1` | None. |

## 6. Example plugins (shipped with the SDK, built out of tree in CI)

1. **hello-tool** (C, not C++): a tool that places a point entity where you click, with one parameter and an
   overlay. Proves the C header alone is enough.
2. **polygon-tool** (C++ wrapper): 0.8 §4's polygon tool; exercises preview ghosts, picking and `apex.edit/1`.
3. **face-stats panel** (C++): a dockable panel and a materials-panel section reading the selection.
4. **script primitive** (AngelScript): a parametric stair, re-editable per D11's outcome.
5. **obj-import** (C++): an importer producing solids from convex OBJ parts.

CI builds them from the installed SDK only (no `src/` on the include path) and loads them in a headless
session test. That is how the exit criterion is tested.

## 7. Not plugin-interface work, but in the 0.9 milestone

- **Keymap editor** over `Preset` (0.2 D15), with D7's plugin and dormant bindings.
- **Pie editor** over the preset's pie layouts (D7).
- **Theme editor**: already built (§2); 0.9 adds plugin-shipped themes (D8).
- **Plugin manager** UI (D9).

## 8. Sub-milestones (proposed)

1. **0.9.1** ABI skeleton: `apex_plugin.h`, loader, manifest, plugin manager (list/enable), logging,
   `apex.document/1` read, hello-tool's C build loading. P1.
2. **0.9.2** Tools and actions: `apex.tools/1`, `apex.actions/1`, the mutable action registry, D7 bindings;
   hello-tool and polygon-tool.
3. **0.9.3** Writes and notifications: `apex.edit/1`, change sets as IDs, `defer`, `apex.selection/1`.
4. **0.9.4** UI: declarative forms, plugin panels and sections, dialogs; face-stats.
5. **0.9.5** Overlays, helpers, problems, importers, jobs, file watch. P4.
6. **0.9.6** VFS providers and asset writes. P3.
7. **0.9.7** Script host (D10), script primitives (D11 after P2), P5.
8. **0.9.8** Keymap editor, pie editor, plugin themes.
9. **0.9.9** SDK packaging, CMake package, out-of-tree CI, docs, the D12 freeze.

## 9. Owner decisions

Answered 2026-11-16: the owner accepted each recommendation.

1. D1: in-process native plugins.
2. D8: declarative panels only; the Qt tier is not shipped in 0.9.
3. D10: AngelScript for the bundled script host.
4. D12: ABI major 1 is frozen (additions only) at the 0.9 release.

5. godot-acoustics is one input among others, not the 0.9 acceptance consumer (§5). It is a plugin the
   owner intends to build, but the API is not shaped around it; the exit criterion is tested by the
   example plugins (§6).

## 10. Probe results

### P4: per-face tint cost (0.9.5a, measured 2026-11-16)

Method: `[measure] 0.9.5 P4 per-face tint ...` in `tests/render/preview_overlay_test.cpp` (opt-in,
`APEX_MEASURE=1 scripts/test.sh linux-gcc-debug -R "P4 per-face" -V`). Every face of the map gets an RGBA
tint through `render::MirrorViewOptions::sideTints`, rebuilt on each of 20 repetitions (a 10 Hz repaint);
the mean per repetition covers building the tint map, `buildDrawList`, and one recorded and waited frame into a
128x128 target on an RX 6600 (RADV). **Debug build** (`linux-gcc-debug`): absolute times are pessimistic,
the ratios are what matters. The tint rides the existing selected-face path, which splits a solid's batch
into one draw per face.

| Map | Faces | Draws (untinted -> all tinted) | buildDrawList | Record + submit + wait |
|---|---|---|---|---|
| `~/maps/apex/test1.vmf` (largest real map here) | 776 | 260 -> 775 | 0.8 -> 1.05 ms | 1.55 -> 3.86 ms |
| 48x48 floor of boxes (synthetic stress) | 13,824 | 2,304 -> 13,824 | 1.8 -> 7.2 ms | 11.6 -> 62.5 ms |

Building the tint map is 0.22 ms (776 faces) to 5 ms (13,824). It is per call, not mirror-wide, because one mirror serves every pane. Reading: on the real map the cost is
fine (a few ms in debug, at 10 Hz). On the synthetic 13.8k-face floor it is bad: the frame is about 5x
slower (63 ms against a 16.7 ms 60 Hz frame), all of it draw-call count, because every tinted face is its
own draw. Consequences taken for `apex.overlay/1`: tint is a map keyed by side ID, applied only to faces
that carry one (untinted faces in untouched solids stay batched); plugins should tint regions, not unique
colours per face. If a later pass needs more headroom the fix is coalescing adjacent faces with equal
tint into one index range (not done: Lit reflection points are per face), or a per-vertex tint
attribute. Release-build numbers were not taken in this pass.
