# Repository map

What each top-level path is for, and what you can ignore. Paths are relative to the repository
root. For the source tree in detail, see [source-map.md](source-map.md).

## Top level

| Path | What it is | Read it when |
|---|---|---|
| [`src/`](../../src/) | The program: nine layers, see [architecture.md](architecture.md). | You change behaviour. |
| [`tests/`](../../tests/) | doctest suites that mirror `src/`, plus fixtures, fuzz targets and golden images. | You add or change behaviour, or need a test to copy from. |
| [`tools/`](../../tools/) | Helper scripts and small C++ programs: the CI gate, layer and licence checks, packaging, a VMF dump tool, probes, and Windows VM control. | You run CI, package a release, or debug a file. |
| [`cmake/`](../../cmake/) | CMake modules: pinned dependencies, the layer macro, shader compilation, packaging rules. | You add a dependency, a source file to a layer, or a shader. |
| [`docs/`](../) | Roadmap, research notes, the development procedure, per-milestone plans, and probe results. | You need the reason behind a design. |
| [`third_party/`](../../third_party/) | Vendored Wayland protocol XML. | You touch the Wayland pointer-lock code. |
| [`spikes/`](../../spikes/) | Throwaway experiments with their own CMake projects (Qt multimedia, Wayland and Vulkan). Not part of the root build. | You want to see an earlier experiment. Do not extend them. |
| [`CMakeLists.txt`](../../CMakeLists.txt), [`CMakePresets.json`](../../CMakePresets.json) | The build entry point and its presets (`linux-gcc-release`, `linux-asan`, and so on). | You build the project. |
| [`version.txt`](../../version.txt) | The version string, read by CMake. | You cut a version. |
| [`CONTRIBUTING.md`](../../CONTRIBUTING.md), [`LICENSE`](../../LICENSE), [`THIRD_PARTY.md`](../../THIRD_PARTY.md) | Contribution rules (DCO sign-off, layers, fixtures), the MIT licence, and the list of third-party components. | You contribute, or add a dependency. |
| [`.clang-format`](../../.clang-format), [`.clang-tidy`](../../.clang-tidy), [`.editorconfig`](../../.editorconfig) | Formatting and lint configuration. | You write code. Match the surrounding files. |

## Not in the repository

- **Game content.** Valve and Strata assets are never committed. The VMF corpus used by the corpus
  tests is read from a P2CE install named by the `APEX_P2CE_DIR` environment variable. Without it,
  those tests skip. See [../development.md](../development.md) section 4.
- **Build output.** `build/<preset>/` holds the build trees. It is git-ignored, and so is
  `compile_commands.json`.
- **Local tooling state.** `.claude/` (including `.claude/worktrees`) and the `.venv-tools/` Python
  environment are git-ignored. `.codex/`, `.agents/` and `.aws/` are empty local folders that git
  does not track. None of them is source.
- **Caches.** `/.cache/`, `__pycache__/` and `/corpus/` are ignored.

## Where generated code comes from

- **Shader headers.** GLSL files in `src/render/shaders/` are compiled to SPIR-V by `glslc` at build
  time. `cmake/Shaders.cmake` writes one header per layer under `build/<preset>/gen/`.
  `tools/embed_spirv.py` turns the binaries into `constexpr` arrays. Do not edit generated headers.
- **Version header.** `cmake/version.hpp.in` becomes `gen/apex/version.hpp`. It carries the version,
  the build number and the git hash. The build number is set by `tools/ci.py`.
- **Dependencies.** `cmake/deps.cmake` fetches pinned third-party libraries through CPM on the first
  configure. They are cached after that.

## Entry points

| What you want to run | Where it is |
|---|---|
| The editor | [`src/app/main.cpp`](../../src/app/main.cpp): sets up Qt, logging and themes, then opens `MainWindow` and an optional map from `argv[1]`. |
| The unit and integration tests | [`tests/main.cpp`](../../tests/main.cpp), plus the `apex_tests` and `apex_ui_tests` targets in [`tests/CMakeLists.txt`](../../tests/CMakeLists.txt). |
| The full CI gate | [`tools/ci.py`](../../tools/ci.py). |
| The layer check | [`tools/check_layers.py`](../../tools/check_layers.py). |
| The packager | [`tools/package.py`](../../tools/package.py). |
