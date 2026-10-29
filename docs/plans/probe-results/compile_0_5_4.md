# 0.5.4 cordons: probe notes

## What was checked

- Hammer is Windows-only, so it could not be run on this host. Cordon VMF shape and semantics follow the D7 description:
  `cordons { "active" "1" cordon { "name" ... "active" ... box { "mins" "(x y z)" "maxs" "(x y z)" } } }`.
- Block-level `active` enables cordoning. Each cordon's own `active` is a per-box flag. Only boxes with their own flag on count, and only when the block is enabled.
- vbsp strings were checked for cordon-related handling; compile output relies on a filtered map, not on vbsp cordon support.

## Assumptions (unverified against Hammer)

- Wall material defaults to `tools/toolsskybox` (the gameconfig `CordonTexture` value). It is a profile setting (`cordonMaterial`).
- Wall thickness is 16 units. This has not been confirmed against Hammer's own cordon compile.
- Disjoint active cordons get walls around the union box of all of them.

## Deviations from the plan

- D7 handle-drag editing in the viewport was replaced by a panel editor (`CordonEditor`): list, active toggle, add / from-selection / remove, min/max spin boxes that coalesce into one undo step.

## Still to do

- Completed 2026-10-28: the `compile_chamber` gate cuts the chamber with an active cordon, compiles the filtered/walled snapshot with native VBSP/VVIS/VRAD, and checks no `.lin` is produced. The full CI graph now runs this gate. Hammer parity measurements above remain unverified.
