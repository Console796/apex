# Contributing to Apex

## Licence and sign-off

Apex is MIT licensed. The only planned relicensing target is **GPLv3-or-later**, so dependencies
must be compatible with both (see `THIRD_PARTY.md`; no GPL of any version, which rules out
KDDockWidgets).

Contributions come in under the [Developer Certificate of Origin](https://developercertificate.org/).
Sign every commit with `git commit -s`. There is no CLA.

## Building

Requirements: CMake >= 3.25, Ninja, GCC 14+ or Clang 18+, Qt 6.10 (`qt6-base-dev`), Vulkan
headers/loader, `glslc`, and `vulkan-validationlayers`.

```
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug
ctest --preset linux-gcc-debug
```

Presets: `linux-gcc-{debug,release}`, `linux-clang-{debug,release}`, `linux-asan`,
`windows-msvc-{debug,release}`.

## The CI gate

CI runs locally (plan 0.1, D22): `tools/ci.py`. It needs `APEX_P2CE_DIR` pointing at a P2CE install
(the test corpus is never committed). `tools/ci.py --fast` runs a quick subset, and
`tools/install-hooks.sh` installs it as an opt-in `pre-push` hook.

## Layers

`core -> geometry -> document -> {assets} -> render -> viewport -> ui -> app`, see
`docs/plans/0.1.md` section 2. `tools/check_layers.py` rejects upward includes and Qt includes
below `ui/`.

## Test fixtures

Files in `tests/fixtures/` are Apex-authored and MIT. They must carry an `Apex-authored` marker in
their first lines. Never commit Valve or Strata content.
