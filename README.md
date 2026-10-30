# Apex

Apex is a native, cross-platform level editor for Source-engine maps, aimed at the Strata Source
(P2CE) toolchain. It is a C++23 / Qt 6 / Vulkan program that reads and writes `.vmf` files and is
meant to interoperate with Strata Hammer in both directions without loss.

**Status: pre-release.** `version.txt` reads `0.5.8`. Milestones 0.1 to 0.3 are released (latest
`v0.3.10-release`). 0.4 and 0.5 have landed, but 0.5 has no release tag yet because its
verification is still open. 0.6 (realtime lighting) is in progress on `master`. Apex can open,
author, entity-edit, compile and launch maps, and it can preview direct lighting. Do not use it on
maps you have no backup of.

## What works today

**Open, view and save (0.1)**
- Opens VMF files from the P2CE corpus; saving an unmodified file is byte-identical.
- Vulkan renderer with any number of viewports in a split tree. Each viewport switches between
  perspective and orthographic, and between wireframe, flat and textured display. 2D views are just
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

**Brush authoring (0.3, released)**
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

**Lighting preview (0.6, in progress)**
- A light model in the `lighting/` layer, built from `light`, `light_spot`, `light_environment`
  and `light_rt` entity keys, with falloff and spotlight helpers in the editor.
- A `Lit` display mode with HDR rendering and fixed-exposure tone mapping, and direct light plus
  sky ambient where the sky is visible.
- Material fidelity from VMT parameters: normal maps, specular, envmaps and Portal 2 refractive
  light covers.

## Game support

Apex is tested only with Portal 2: Community Edition. Every other Source game is untested, including
Portal 2 itself. See [docs/source-game-support.md](docs/source-game-support.md) for the list.

## What is not here yet

Clustered lights and `light_rt` dynamic lights, shadows, and sky, fog and cubemap previews
(0.6.6 to 0.6.8), followed by the 0.6 release. After that: baked lighting preview (0.7),
displacements and overlays (0.8), plugins (0.9) and hardening (0.10). See
[docs/roadmap.md](docs/roadmap.md).

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
exists and is checked from a VM at sub-milestone tags. The renderer targets Vulkan 1.3 and the
design assumes ray-query capable devices later on.

```bash
cmake --preset linux-gcc-release
```

```bash
cmake --build --preset linux-gcc-release
```

```bash
ctest --preset linux-gcc-release
```

Presets: `linux-gcc-{debug,release}`, `linux-clang-{debug,release}`, `linux-asan`,
`linux-fuzz` (Clang libFuzzer), and `windows-msvc-{debug,release}`.

Run it with an optional map path:

```bash
build/linux-gcc-release/apex path/to/map.vmf
```

(The binary location depends on your preset and generator.)

## Testing and CI

CI runs locally with `tools/ci.py` (`--fast` for a quick subset). It needs `APEX_P2CE_DIR` pointing
at a P2CE install, because the VMF test corpus is never committed. `tools/install-hooks.sh`
installs the fast run as an opt-in `pre-push` hook. Tests run under ASan/UBSan on the `linux-asan`
preset and in parallel with `apex_tests --jobs=N`. The libFuzzer targets (KV1, VMF, hull) build on
the `linux-fuzz` preset. The compile-chamber stage runs the real P2CE compile tools, and the lighting parity scripts in
`tools/` do too. Both need `APEX_P2CE_DIR`.

## Packaging

`tools/package.py` builds a Linux `.tar.gz` or a Windows `.zip` from an existing release build.
There is no AppImage or installer yet.

## Repository layout

| Path | Contents |
|---|---|
| `src/core`, `src/geometry`, `src/document` | Pure library layers: math, KV1, brush kernel, VMF model and transactions |
| `src/compile`, `src/lighting` | Compile profiles, sequences and log parsers; the light model and sky visibility |
| `src/editor` | Tools, selection, gizmos, snapping, clipboard, autosave, cordons, problems |
| `src/assets` | Virtual filesystem, game profiles, addons, VMT/VTF/FGD, asset indexes, model baking |
| `src/render`, `src/viewport` | Vulkan renderer (including the `Lit` passes) and viewport management |
| `src/ui`, `src/app` | Qt shell and the `apex` executable |
| `tests/` | doctest suites that mirror `src/`, fixtures (Apex-authored only), fuzz targets, golden images |
| `tools/` | CI runner, layer and licence checks, packaging, VMF dump, probes, and Hammer and compile cross-checks |
| `cmake/`, `third_party/`, `spikes/` | Build modules, vendored Wayland protocol XML, and throwaway experiments outside the build |
| `docs/` | [roadmap](docs/roadmap.md), [spec](docs/research.md), per-milestone [plans](docs/plans), [repository index](docs/index/README.md) |

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
