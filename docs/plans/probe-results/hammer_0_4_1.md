# Strata Hammer 0.4.1 probes: instance search, prefabs, scale keys (2026-10-25)

Settles the Hammer probes of [0.4](../0.4.md) §3 0.4.1 for D4, D7 and D12 (D15's P3 is separate, below). P2CE 0.0.8.2,
Strata Hammer under Wine on Linux, headless on Xvfb with lavapipe (`tools/hammer_wine_check.py`'s setup).

**Method.** A VMF copied from `tests/fixtures/probe/e_control.vmf` plus three `func_instance` entities naming files that
exist nowhere (`apexprobe_plain.vmf`, `apexprobe_dir/apexprobe_nested.vmf`, `../apexprobe_up.vmf`) was opened with
`WINEDEBUG=+file`. Only the `CreateFileW` attempts for those names are kept, in the order Hammer made them. The map sat
outside the install, so nothing was written there. `hammer/addons/addonlist.kv3` and `hammer/cfg/*` were backed up first
and match their SHA-256 sums afterwards.

## Instance `file` search order (D7)

Map at `…/maps/sub/probe_map.vmf` (run 1) and `…/proj/deep/x/probe_map.vmf` (run 2, no `maps` ancestor). For
`file "apexprobe_plain.vmf"`:

| Try | Run 1 | Run 2 |
|---|---|---|
| 1 | `…/maps/sub/apexprobe_plain.vmf` (the map's own folder) | `…/proj/deep/x/apexprobe_plain.vmf` |
| 2 | `…/maps/apexprobe_plain.vmf` (nearest ancestor folder named `maps`) | absent |
| 3 | `<install>/content/p2ce/maps/apexprobe_plain.vmf` | the same |

- A path with subfolders or `..` is appended to each root unchanged: `apexprobe_dir/apexprobe_nested.vmf` goes through
  the same three roots, and `../apexprobe_up.vmf` gets `…/maps/../apexprobe_up.vmf` as written beside the normalised
  map-folder attempt.
- **The first root that holds the file wins.** Relative-to-the-map's-folder is tried first, so it beats the others.
- Hammer never tried `sdk_content/maps/instances/…` or any other `instances` folder. P2CE's `gameconfig.txt` `MapDir` is
  `<install>/sdk_content/maps`, and Hammer didn't use it either. The third root is `<install>/content/p2ce/maps`, which
  doesn't exist in this install. A map outside `sdk_content/maps` therefore finds its instances only beside itself or
  under an ancestor named `maps`.
- The file the plan counts as "relative to `sdk_content/maps`" is found by root 1 or 2 for a map that lives in
  `sdk_content/maps`, which is every corpus map.

Not probed: a hit in root 2 vs root 1 when both exist (the order above says root 1), and the case-insensitivity of the
match (Wine's `z:` is case-insensitive here, so the trace can't show Hammer's own rule).

## `hammer/prefabs` (D12)

At startup Hammer enumerates `hammer/prefabs\*.vmf` and `hammer/prefabs\*.*` and registers a change notification on the
folder (`FindFirstChangeNotificationW`, watching the subtree). The `PrefabDir` setting in `gameconfig.txt` names it.
The folder is empty in the P2CE install, so nothing was opened. The strings in `hammer_dll.dll` show VMF prefab
classes (`CPrefabLibraryVMF`, `CPrefabVMF`) and no RMF one. So prefabs are `.vmf` files, loose or in subfolders
(libraries), and Hammer watches the folder live. Not checked: that a file Apex writes there shows up in Hammer's prefab
list, since the probe was a file trace and the folder is empty.

## Scale keys (D4)

`hammer_dll.dll` holds `modelscale` and `uniformscale` (plus `scalesettings`); `vbsp.exe` holds only `uniformscale`;
`p2ce.fgd` declares no vector scale key. No vector-scale key exists to render, so **D4 renders uniform scale only**
(`uniformscale` on `prop_static`, `modelscale` on dynamic props) and the roadmap's non-uniform line is amended.
