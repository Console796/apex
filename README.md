# Apex

Apex is a native, cross-platform level editor for Source-engine maps, aimed at the Strata Source
(P2CE) toolchain. It is a C++23 / Qt 6 / Vulkan program that reads and writes `.vmf` files and is
meant to interoperate with Strata Hammer in both directions without loss.

**Status: pre-release (0.2.12 shipped, 0.3 in progress).** Apex can open, view, select, rearrange
and save maps. It cannot yet author brushes. Do not use it on maps you have no backup of.

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

**Brush authoring groundwork (0.3.1, in progress)**
- A common tool interface; Select and Entity tools now sit behind it.
- Geometry kernel additions: convex hull, split, subtract and solid-block construction, with a
  fixed-seed hull fuzzer in CI.
- Topology-changing solid rebuilds in the document that preserve side IDs where the correspondence
  is known.
- A snapper (vertex, edge, grid priority), preview geometry and ghosts in the renderer, and world
  overlays and labels.

## What is not here yet

The block, clip, vertex, primitive, carve, hollow and merge tools, the material browser, texture
face editing and Strata addon mounting are planned for 0.3.2 to 0.3.9. After that: entities and
assets (0.4), compile and play (0.5), realtime and baked lighting preview (0.6, 0.7), displacements
and overlays (0.8), plugins (0.9) and hardening (0.10). See [docs/roadmap.md](docs/roadmap.md).

## Design rules

- Every task is doable in a 3D viewport; tools see a ray, a projection and a work plane, not "2D"
  or "3D".
- VMF round-trips with Strata Hammer are a hard requirement.
- The core (math, KV1, geometry, document) builds and tests with no GPU and no UI.
- Every document change is a transaction; renderer, picking and outliner update from its change set.
- Game specifics live in a profile. Strata/P2CE is the only one planned through 0.10.

The layer order is `core -> geometry -> document -> assets -> render -> viewport -> ui -> app`,
enforced by `tools/check_layers.py`.

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
`windows-msvc-{debug,release}`.

Run it with an optional map path:

```bash
build/linux-gcc-release/apex path/to/map.vmf
```

(The binary location depends on your preset and generator.)

## Testing and CI

CI runs locally with `tools/ci.py` (`--fast` for a quick subset). It needs `APEX_P2CE_DIR` pointing
at a P2CE install, because the VMF test corpus is never committed. `tools/install-hooks.sh`
installs the fast run as an opt-in `pre-push` hook. Tests run under ASan/UBSan on the `linux-asan`
preset, and libFuzzer targets (KV1, VMF, hull) build with `APEX_BUILD_FUZZ`.

## Packaging

`tools/package.py` builds a Linux `.tar.gz` or a Windows `.zip` from an existing release build.
There is no AppImage or installer yet.

## Repository layout

| Path | Contents |
|---|---|
| `src/core`, `src/geometry`, `src/document` | Pure library layers: math, KV1, brush kernel, VMF model and transactions |
| `src/render`, `src/viewport` | Vulkan renderer and viewport management |
| `src/editor` | Tools, selection, gizmos, snapping, clipboard, autosave |
| `src/ui`, `src/app` | Qt shell and the `apex` executable |
| `tests/` | doctest suites, fixtures (Apex-authored only), fuzz targets |
| `tools/` | CI runner, layer and licence checks, packaging, VMF dump and Hammer-check helpers |
| `docs/` | [roadmap](docs/roadmap.md), [spec](docs/research.md), per-milestone [plans](docs/plans) |

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
