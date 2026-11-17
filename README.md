# Apex

Apex is a native, cross-platform level editor for Source-engine maps, aimed at the Strata Source
(P2CE) toolchain. It is a C++23 / Qt 6 / Vulkan program that reads and writes `.vmf` files and is
meant to interoperate with Strata Hammer in both directions without loss.

**Status: 0.8 released, 0.9 feature complete with release checks open.** The latest release is `0.8.9-release` (tag
`v0.8.9-release`), which completes milestone 0.8, advanced geometry and surfaces. Earlier releases
are `0.7.7-release` (lighting II, baked preview), `0.6.8-release` (lighting I, realtime direct
lighting), and the 0.1 to 0.3 releases. 0.4 and 0.5 have landed without a release tag; 0.5's
verification is still open. Milestone 0.9 (extensibility: plugins and scripting) is feature complete on
`master` but not released: its release checks (Windows, Strata Hammer interop, the plugin round trip) are open. Apex can open, author, entity-edit, compile and launch maps, preview direct and baked
lighting, and edit displacements, overlays and ropes. It is pre-release software. Back up every map
before you open it.

Release notes for each release are in [docs/releases](docs/releases/).

## What works today

**Open, view and save (0.1)**
- Opens VMF files from the P2CE corpus; saving an unmodified file is byte-identical.
- Vulkan renderer with any number of viewports in a split tree. Each viewport switches between
  perspective and orthographic, and between wireframe, flat, textured and lit display. 2D views are
  axis-locked orthographic wireframe viewports.
- Game profile and virtual filesystem (gameinfo, mounts, VPK), plus VMT, VTF and FGD loading.

**Selection and manipulation (0.2)**
- Object, face and solid selection modes; picking, marquee and frustum select.
- Translate, rotate and scale gizmos (local/global, pivot) with snapping, surface-anchored grids
  and work planes, and texture lock under transforms.
- Undo history panel, clipboard, duplicate and paste-special, groups, visgroups, hide and isolate.
- Editable outliner and an FGD-driven entity property inspector; point-entity placement on
  surfaces.
- Autosave and crash recovery.
- Hammer and Blender keybinding presets with modal transforms, a Preferences dialog, a keybinding
  editor, and savable areas and workspaces.

**Brush authoring (0.3)**
- Block, primitive, clip, vertex/edge/face and measure tools, with carve, hollow and merge.
- Pie menus and a tool strip with selection modes on number keys.
- A material browser, texture application and face editing (shift, scale, rotate, justify, fit,
  align), and texture lock.
- Read-only mounting of Strata content addons, so the material browser sees what Hammer shows.
- A snapper (vertex, edge, grid priority), preview geometry and ghosts, and world overlays and labels.

**Entities and assets (0.4)**
- MDL model rendering with skins, bodygroups and non-uniform scale, and model-triangle picking.
- FGD helper shapes, an I/O connection editor with validation and autocompletion.
- `func_instance` rendering, in-context instance preview, and instance collapse.
- Model, sound and particle browsers, prefabs, an entity report, and find and replace.
- An Addons panel that writes mount toggles back to Strata Hammer's addon list.

**Compile and play (0.5)**
- Compile profiles, including import of Hammer `.wc` files. vbsp, vvis, vrad and the postcompiler
  run as cancellable background jobs using the native tools on Linux.
- Compile logs are parsed into diagnostics that link back to objects. `.lin` leak and `.prt`
  portal overlays are read from the compile output.
- Cordons, a Problems list with map validation, and a pre-compile gate.
- Compile & Run and Run Game launch the game with `-map`, with a game log and reload.
- Addon targets, addon scaffolding, and Create Instance from Selection.

**Lighting preview (0.6 and 0.7)**
- Realtime preview: a light model shared with the baker, built from `light`, `light_spot`,
  `light_environment` and `light_rt` entity keys, with Source falloff. The `Lit` display mode uses
  HDR rendering, tone mapping, and direct light plus sky ambient. Material fidelity comes from VMT
  parameters: normal maps, specular, envmaps and Portal 2 refractive light covers.
- Baked preview: luxel charting that matches VBSP, a progressive GPU baker (Vulkan ray query) with
  an Embree CPU fallback, bounce from VTF reflectivity, and sky. Baked and Hybrid views compare
  against VRAD, and an A/B compare view and a luxel density view help with checking.
- Hybrid view keeps the last VRAD result and re-bakes only the faces that edits made stale. Realtime
  probes give an approximate bounce without baking.
- The baked preview is a preview, not VRAD. Compile with VRAD before you judge a map's final lighting.

**Advanced geometry and surfaces (0.8)**
- **Displacements:** a Displacement tool with Create, Sew, Sculpt and Paint modes; every stroke is
  one undo step and previews live. Destroy displacement turns a displaced face back into a flat one.
  Face Properties has a Displacement tab (power, NoPhys, NoHull, NoRay). Displacements follow
  transforms and paste, survive save and reopen, and get lightmaps in the baked and hybrid views.
  Clip, vertex, carve, hollow and merge refuse displaced solids and say so. Problems reports unsewn
  gaps and unreadable displacements.
- **Overlays and decals:** the Overlay tool places overlays and decals and edits their quads. They
  are clipped to their faces, follow their faces through splits, moves and pastes, and are checked
  against the compiled BSP's overlay data.
- **Hotspot texturing:** a Hotspot button in Face Properties and a Hotspot mode in the Apply tool fit
  a face to a rectangle from the material's VTF hotspot resource or a `.rect` file. Hold Alt for the
  alternate group. The material browser tags such materials with "H". The fit rule is Apex's own
  and may differ from Hammer++.
- **Ropes and cables:** `move_rope` and `keyframe_rope` entities can be placed. Ropes draw with sag,
  node count, smoothing and width taken from the entity keys, as the SDK's rule describes. The Rope
  tool creates a rope from clicked points in one undo step.
- **Detail and structural tooling:** To Detail and To World convert brushes to and from
  `func_detail`. A Structural overlay greys out non-structural geometry, the outliner has a
  Structural filter, and Problems flags small world brushes.

## Game support

Apex is tested only with Portal 2: Community Edition. Every other Source game is untested, including
Portal 2 itself. See [docs/source-game-support.md](docs/source-game-support.md) for the list.

## What is not here yet

- **Plugins and scripting** (milestone 0.9) are built on `master` and not released. The plugin C ABI is frozen at
  major 1, minor 0; there is a plugin manager, AngelScript script plugins and an SDK package
  (`python3 tools/package.py --sdk`). Windows and MSVC are untested for plugins, and scriptable primitives, floating plugin
  panels and native hot reload are not built. See [docs/plugins](docs/plugins/README.md) and the
  [draft release notes](docs/releases/0.9.9-release.md).
- **Detail prop preview** was dropped from 0.8 and returns in a later milestone. The code stays in
  the tree with no menu entry.
- **Displacement limits:** no clipping, carving or merging through a displaced face. Destroy the
  displacement first.
- **Baking limits:** no bumped lightmaps; no GPU bake on GPUs without ray queries (NVIDIA GTX 16-series
  and older bake on the CPU); concave creases bake darker than VRAD.
- **Ropes:** wind and `Collide` are not previewed.
- **Structural tint** is lost on the contents of `func_instance`.
- **Production hardening** (performance budgets, interop test suite, user manual, installers
  beyond the current packages, accessibility) is milestone 0.10, followed by 1.0 RC.
- **0.8 release checks still open:** the written outdoor scenario that defines parity with
  Hammer++, the interop check of every fixture edited in Apex and reopened in Strata Hammer, and the
  Windows run of that scenario. Windows baker validation for 0.7 is also still owed.

See [docs/roadmap.md](docs/roadmap.md) for the full plan.

## Design rules

- Every task is doable in a 3D viewport; tools see a ray, a projection and a work plane, not "2D"
  or "3D".
- VMF round-trips with Strata Hammer are a hard requirement.
- The core (math, KV1, geometry, document) builds and tests with no GPU and no UI.
- Every document change is a transaction; renderer, picking and outliner update from its change set.
- Game specifics live in a profile. Strata/P2CE is the only one planned through 0.10.

The layers are `core`, `geometry`, `document`, `compile`, `lighting`, `editor`, `assets`, `render`,
`viewport`, `ui` and `app`. Each may include only itself and the layers it depends on, and
`tools/check_layers.py` enforces that. The exact edges are in
[docs/index/architecture.md](docs/index/architecture.md#layers).

## Building

Requirements: CMake 3.25+, Ninja, GCC 14+ or Clang 18+, Qt 6.10, Vulkan headers and loader,
`glslc`, and `vulkan-validationlayers`. Linux is the primary platform; a Windows MSVC preset
exists and is checked from a VM at sub-milestone tags. The renderer targets Vulkan 1.3, and the
GPU baker and Realtime indirect need a device with ray-query support.

```bash
cmake --preset linux-gcc-release
```

```bash
cmake --build --preset linux-gcc-release
```

```bash
ctest --preset linux-gcc-release
```

On a machine with limited memory, `scripts/build.sh` and `scripts/test.sh` run the same steps under
a memory cap. Pass a preset name as the first argument, such as `scripts/build.sh linux-gcc-release`.

Presets: `linux-gcc-{debug,release}`, `linux-clang-{debug,release}`, `linux-asan`,
`linux-fuzz` (Clang libFuzzer), and `windows-msvc-{debug,release}`.

Run it with an optional map path:

```bash
build/linux-gcc-release/apex path/to/map.vmf
```

(The binary location depends on your preset and generator.)

On Windows, get Qt with [aqtinstall](https://github.com/miurahr/aqtinstall), which needs no Qt
account (the official online installer does):

```bat
py -m pip install aqtinstall
```

```bat
py -m aqt install-qt windows desktop 6.10.2 win64_msvc2022_64 -m qtmultimedia -O C:\Qt
```

Then configure with `-DCMAKE_PREFIX_PATH=C:/Qt/6.10.2/msvc2022_64`. Each link of `apex.exe` runs
`windeployqt` on it, so the build folder carries its own Qt DLLs and runs without Qt on `PATH`.

## Testing and CI

CI runs locally with `tools/ci.py` (`--fast` for a quick subset). It needs `APEX_P2CE_DIR` pointing
at a P2CE install, because the VMF test corpus is never committed. `tools/install-hooks.sh`
installs the fast run as an opt-in `pre-push` hook. Tests run under ASan/UBSan on the `linux-asan`
preset and in parallel with `apex_tests --jobs=N`. The libFuzzer targets (KV1, VMF, hull) build on
the `linux-fuzz` preset. The compile-chamber stage runs the real P2CE compile tools, and the lighting
parity scripts in `tools/` do too. Both need `APEX_P2CE_DIR`.

## Packaging

`tools/package.py` builds the release packages from an existing release build: on Linux a
`.tar.xz` that bundles Qt, plus a `.deb` and an `.rpm` that use the system Qt; on Windows a `.zip`.
`--installer` turns that zip into an NSIS installer. The Windows `.zip` carries the Qt DLLs and the
C++ runtime next to `apex.exe`, so running it needs no Qt install, only a GPU driver with Vulkan
support. There is no AppImage yet.

The 0.8.9 Linux packages were built on Ubuntu 26.04 and need glibc 2.43 or newer.

## Repository layout

| Path | Contents |
|---|---|
| `src/core`, `src/geometry`, `src/document` | Pure library layers: math, KV1, brush kernel, displacement and overlay geometry, VMF model and transactions |
| `src/compile`, `src/lighting` | Compile profiles, sequences and log parsers; the light model, luxel charting, the baker and sky visibility |
| `src/editor` | Tools (including the Displacement, Overlay and Rope tools), selection, gizmos, snapping, clipboard, autosave, cordons, problems |
| `src/assets` | Virtual filesystem, game profiles, addons, VMT/VTF/FGD, hotspot rects, asset indexes, model baking |
| `src/render`, `src/viewport` | Vulkan renderer (including the `Lit` and bake passes) and viewport management |
| `src/plugin_host` | Host side of the plugin ABI: loader, interface tables, tool adapter, the AngelScript script host (Qt-free) |
| `src/ui`, `src/app` | Qt shell (including the plugin manager and plugin panels) and the `apex` executable |
| `include/apex`, `examples`, `cmake/sdk` | The public plugin headers (C ABI and C++ wrapper), example plugins, and the SDK's CMake package |
| `tests/` | doctest suites that mirror `src/`, fixtures (Apex-authored only), fuzz targets, golden images |
| `tools/` | CI runner, layer and licence checks, packaging, VMF dump, probes, and Hammer and compile cross-checks |
| `cmake/`, `third_party/`, `spikes/` | Build modules, vendored Wayland protocol XML, and throwaway experiments outside the build |
| `scripts/` | Memory-capped build, test and memory-check wrappers |
| `docs/` | [plugin SDK docs](docs/plugins/README.md), [roadmap](docs/roadmap.md), [spec](docs/research.md), [releases](docs/releases), per-milestone [plans](docs/plans), [repository index](docs/index/README.md) |

## New to the repository?

Start with the [repository index](docs/index/README.md). It explains where things are, how the
layers fit together, and which document answers which question.

## Contributing and licence

See [CONTRIBUTING.md](CONTRIBUTING.md). Contributions are signed off under the Developer
Certificate of Origin (`git commit -s`); there is no CLA. Never commit Valve or Strata content.

Apex is MIT licensed ([LICENSE](LICENSE)). The only planned relicensing target is GPLv3-or-later,
so dependencies must be compatible with both. Third-party components are listed in
[THIRD_PARTY.md](THIRD_PARTY.md).

## LLM disclosure

Apex is developed with substantial help from large language models. Anthropic's Claude (through
Claude Code) and OpenAI's Codex CLI were used to write code, tests, tooling and documentation, and
to review changes. The project owner directs the work, reviews and accepts the plans and the
results, and runs the CI gate; LLM output is not trusted unverified. Anyone contributing should
assume that any file here may contain LLM-generated text or code.
