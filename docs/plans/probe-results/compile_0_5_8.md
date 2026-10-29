# 0.5.8: Create Instance from Selection — 2026-10-28

The command writes the selected VMF fragment as a standalone, non-prefab VMF, translated by the
negative selection pivot with texture lock and entity-class metadata. It replaces the selection
with an unrotated `func_instance` at that pivot, with `fixup_style 2` (names unchanged). The
external file is created exclusively. A failed document edit removes only that new file; undo
restores the parent document/selection and keeps the file so redo remains usable.

Tests cover a solid and point entity, one history entry, byte-identical undo, redo, collapse back
to the original bounds and entity origin/name, incoming and outgoing crossing connections,
refusal before confirmation, and refusal to overwrite an existing file. The created-instance
case also passed native VBSP/VVIS/VRAD in the compile-chamber gate.

Instance snapshots now parse the referring entities and copy source-relative, absolute and
parent-relative templates into the work directory, rewriting only snapshot references. The
external/nested-path test checks containment, addon BSP destination and unchanged source VMF.

Focused verification before the full gate: 25 tests / 926 assertions passed, and all 160 offscreen
UI tests / 4,325 assertions passed. The later campaign-runner and release-measurement tests are
included in the full gate. Separate Hammer re-save and srctools checks are tracked with the
release verification; the native compiler result does not establish Hammer UI acceptance.
