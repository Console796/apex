# Light helpers — 2026-10-28

Select one light with the Select tool to expose viewport controls for its declared
falloff distances and spotlight cone angles. Enabled distance bounds have coloured
sphere handles; inner/outer cone rims have matching handles. Hover identifies the
bound and current value. Dragging previews helper keys without editing the document;
release commits one undoable transaction. Shift enables fine adjustment; Escape,
right-click, or changing selection cancels. Clicking without dragging is a no-op.

The inspector provides numeric step controls for distance, attenuation coefficients,
cone angles, and focus. The viewport parameter strip exposes the main distance/cone
controls and scrolls horizontally in narrow panes. Strip and drag edits keep the two
falloff distances and inner/outer angles ordered. Raw inspector values remain editable.
A zero maximum distance retains the engine's unlimited/automatic behavior; the cone
uses a finite display length when no positive bound supplies one.

FGD `lightprop`, `studio`, and `studioprop` declarations supply an editor model when
there is no explicit entity model. This does not serialize a model key into the VMF.
Spotlight display, helper geometry, and model picking share the effective FGD values,
light pitch convention, and target direction. Rotation updates the separate light
pitch key so the compiled direction follows the editor rotation.

Native P2CE VBSP/VVIS/VRAD checks on temporary maps confirmed normals of
`(0.8660254, 0, -0.5)` for pitch `-30`, and `(0.8660254, 0, 0.5)` for angles
`30 0 0` with pitch `0`. The installed `light_rt_spot` FGD loads and renders
`models/editor/spot.mdl` without an explicit model key and supplies matching picking
geometry. Tests cover default and zero distances, both cones, target direction,
live preview/clear, fine dragging, bounds, undo, cancellation, and narrow UI controls.

Validation: 139 core/render tests (1,083,067 assertions), 160 UI tests (4,326
assertions), full Clang release build, and layer checks.
