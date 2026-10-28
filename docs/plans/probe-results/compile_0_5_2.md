# 0.5.2 probe: compile log shapes and object mapping

Logs were captured from the real P2CE `vbsp`/`vvis`/`vrad` by `tools/capture_compile_logs.py`
(variants of the chamber map) and stored in `tests/compile/logs/`. Rules in
`src/compile/diagnostics.cpp` are golden-tested against them.

## Reported with a stable id (becomes a clickable diagnostic)
- `vbsp`: `Brush N: FloatPlane: bad normal, side S, texture T` — brush id N (VMF id).
- `vbsp`: `Material not found!: path`, missing models and missing instances — text only
  unless the line names an entity id.
- `vvis`: portal problems — text only.

## Reported without an id
- Leaks: classname and position only. The diagnostic carries `position` and no objects;
  navigation comes from the `.lin` file in 0.5.3 (D6).

## Silent in the tools (need Apex validation, 0.5.5)
- Missing `target` / `targetname` references and duplicate targetnames: no tool prints
  anything. The plan's 0.5.2 exit example "missing entity target" is therefore met with
  an invalid brush (selects the brush) plus missing material/model/instance diagnostics;
  target checks move to 0.5.5.

## Click behaviour
Ids are looked up in the document at click time (`editor::findDiagnosticObject`), never
stored as handles. A deleted id shows "No longer in the map" and changes nothing.
