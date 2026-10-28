# 0.5.3 probe: `.lin` and `.prt` as the real tools write them

Captured by `tools/capture_compile_logs.py` from the real P2CE `vbsp` into `tests/compile/logs/`
(`leak.lin`, `baseline.prt`); `compile/map_files.cpp` is tested against them.

- **`.lin`** (leak variant, vbsp exit 0): one `x y z` per line, 6 decimals, from the first
  entity to the outside. A compile that does not leak writes no `.lin`. Apex clears old outputs
  in the snapshot, so "no file" means "no leak this compile".
- **`.prt`** (baseline): `PRT1`, cluster count, portal count, then per portal
  `N clusterA clusterB (x y z ) (x y z ) ...` on one line. The chamber has 29 clusters and 75
  portals; polygons run from 3 to 9 points.
- **A leaking compile writes no `.prt`**; vvis then fails with `couldn't read ….prt`. So a leak
  and portals never appear together, and fixing the leak and recompiling replaces the first by the second.

Overlay state is session state (`EditorSession::compileOverlay`), stamped with the compiled
revision, and drawn grey once `Document::historyState()` differs. Per-viewport toggles are
`Overlays::leak` / `Overlays::portals` (Overlays menu); the Compile panel has Leak / Portals
checkboxes, a cluster filter and *Go to leak start*. Clicking a diagnostic that carries only a
`position` (the vbsp leak line) also focuses that point.
