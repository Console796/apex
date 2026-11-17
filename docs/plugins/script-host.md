# The script host: script plugins

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

## Example: `examples/stair/`

 `plugin.kv3` is generated and `stair.as` is installed by CMake next to the other
examples (`<build>/plugins/stair`). The `plugin.stair.tool` tool aims at the ground plane, snaps to an 8-unit
grid, shows one ghost per step plus a footprint outline, exposes a `steps` parameter (1 to 64, default 8, set
from the readout), and a left click commits all steps in one `apex::Edit` as a single undo step.
Measured on linux-gcc-release at 64 steps: about 0.77 ms per input + refresh (plugins.md section 10, P5).

The tool class and its registration, verbatim from `stair.as`:

<!-- snippet: examples/stair/stair.as -->
```cpp
class StairTool {
    int steps = 8;
    bool placed = false;
    uint64 source = 0;
    apex::Vec3 at;

    double snap(double v) { return floor(v / GRID + 0.5) * GRID; }
```

<!-- snippet: examples/stair/stair.as -->
```cpp
    bool commit() {
        if (!placed || !apex::documentOpen()) return false;
        uint64 world = apex::root(0);
        if (world == 0) return false;
        apex::Edit@ edit = apex::Edit();
        for (int i = 0; i < steps; i++) edit.createSolid(world, stepPlanes(at, i), MATERIAL);
        return edit.commit("Create stair");
    }
```

<!-- snippet: examples/stair/stair.as -->
```cpp
void main() {
    apex::registerTool("tool", "Stair", "Create a straight stair with a chosen number of steps", "StairTool");
}
```
