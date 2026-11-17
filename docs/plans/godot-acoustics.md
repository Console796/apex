# Apex: Godot and brush-acoustics integration: requirements for 0.9

**Status: draft, 2026-11-16. Nothing here is accepted.** These are requirements from one consumer, not a
milestone plan: the 0.9 plan decides the plugin API's shape and confirms or rejects each item. A–B are small
core changes that could land in any milestone; P1–P5 are what the 0.9 plugin API would need for the plugin
described in §3.

## 1. Context

brush-acoustics (`/mnt/hdd/projects/brush-acoustics`) is a Godot extension for physically based
audio in games whose levels are brush maps saved as `.vmf` and imported with GodotVMF. Its design docs are in
`rtaudioresearch` (`BUILD_PLAN.md`; §2.2 is the material table, §7 is Apex). Its acoustic code is a
Godot-free C++20 library (`brac_core`, MIT) built so Apex can link it.

Each brush side's acoustic surface comes from the material's **`$surfaceprop`**: GodotVMF imports a VMT as a
Godot material and keeps `$surfaceprop` as metadata, and the importer looks the name up in the game's
`acoustics/materials.kv` (KV1: `density`, `absorption "low mid high"`, `scattering`, optional
`transmission`, `base` inheritance). Path patterns in the same file cover tool textures and untagged dev
textures.

### 1.1 The workflow today (any VMF editor)

The Godot project doubles as a mod folder (GodotVMF's `godot_vmf/import/gameinfo_path` defaults to `res://`),
with a minimal `gameinfo.txt`, and the Apex profile's game directory points at it.

1. New kind of surface: add an entry to `acoustics/materials.kv`.
2. Drop `materials/custom/foo.png` into the project; GodotVMF's *Create VMT materials* writes `foo.vmt` and
   `foo.vtf` beside it.
3. Add `$surfaceprop "foo_surface"` to the VMT by hand.
4. In Apex, refresh the material browser and paint.
5. Godot reimports the VMF; the acoustic import reads the surfaceprop.

Friction: the VTF copy of every texture, hand-editing VMTs, a typo in step 3 shows up only as an import warning,
and nothing in the editor shows which surface a face is.

### 1.2 The workflow this document aims at

Drop a PNG into the Godot project → it shows in Apex's browser → pick its surface from a dropdown in the
material panel → paint, with a viewport mode that colours faces by surface → reimport in Godot. No VTF, no
hand-written VMT. Strata Hammer and other editors keep using §1.1.

## 2. Core changes (useful to any Source game; not plugin work)

### A: Keep `surfaceProp` in the material index

`assets::Material::surfaceProp` is parsed (`vmt.cpp`) but `MaterialIndexEntry` drops it. Keep it, show it in
the materials panel's info, and accept a `surface:<name>` token in `MaterialFilter::name` alongside the
existing `keyword:` tokens. Resolve it after `include`/patch materials, like the other fields.

### B: A "colour by surfaceprop" viewport mode

Tint each face by a stable hash of its material's surfaceprop (untagged: a fixed warning colour), with a legend
of the names in the visible map. Shows untagged or wrongly tagged walls at a glance; Hammer has nothing like
it. It is the default overlay that P4 below can replace with acoustic data.

## 3. Plugin API requirements (0.9)

The plugin would link `brac_core` and read the game's `acoustics/materials.kv`.

### P1: Material and texture provider (the one that removes the most friction)

A plugin can add a **VFS provider**: a mount that answers `exists`/`open`/`list`/`walk` for virtual paths, with
a place in mount order. A Godot provider would serve `materials/**/*.vmt` synthesized from the project's
`.png`/`.tres` (with `$surfaceprop` from the `.tres` metadata, so no VMT file exists on disk) and serve
the textures themselves.

Serving at the VFS layer rather than into the material index means the browser, renderer, thumbnails, hotspot
lookup and reflectivity all work unchanged. It needs one of:
- the texture loader accepting a non-VTF image format (PNG at least) behind `texturePath`, or
- the provider encoding VTF on demand (simplest for Apex, slower first load).

Provider materials can't be compiled with vbsp unless real VTFs exist; that is fine for a Godot target, which
never runs vbsp, but `compileGameInfo` should leave plugin mounts out or say so.

### P2: Material panel extension

A plugin can add a section to the materials panel for the selected material(s) and write the result back:
- a surface dropdown filled from `materials.kv`, writing `$surfaceprop` into the VMT (through Apex's VMT
  writer so formatting is kept) or into the provider's own store for P1 materials;
- the selected surface's acoustic values, editable, writing `materials.kv` (a new surface type is added here).

Needs: the selection's material names, a write path for VMTs that refreshes the index entry, and a
panel-section hook.

### P3: Read-only document access with change notifications

Solids, sides (planes, material names), brush entities with classnames and keyvalues, and instance transforms,
plus a revision counter or change callback, so the plugin can keep a `brac_core` blob current while you edit.
Read-only is enough: the plugin never edits geometry.

### P4: Viewport draw and per-face tint

Lines, boxes and points in world space (occlusion rays, the propagation grid, a leak path, sound routes), and a
per-face colour override (absorption or density as a heat map, replacing B's hash colours). Per-viewport toggle.

### P5: File watch on plugin-owned paths

Notify the plugin when `acoustics/materials.kv` changes on disk, so overlays and the P2 dropdown update without
a restart. Could be the same mechanism the VFS `refresh` already uses for loose files.

## 4. Stays in the VMF

Hammer has no per-face surfaceprop, so an override for one wall without a new texture stays a brush entity
(`func_acoustic`, from brush-acoustics' FGD). It works in every editor and needs nothing from Apex beyond FGD
loading, which already works.

## 5. Open facts to probe

1. Does Apex accept a Godot project root with a minimal `gameinfo.txt` as a profile's game directory? (§1.1)
   Yes (probed 2026-11-17 against `libapex_assets`: `SearchPaths { game "|gameinfo_path|." }` mounts the root).
2. Does Apex's VTF reader open the VTFs GodotVMF writes (`generate_vtf_file`)? (§1.1)
   Not as GodotVMF wrote them: a 7.4 header with no resource dictionary has no image for vtfpp. Fixed on the
   GodotVMF side (brush-acoustics' fork writes 7.2); nothing to change in Apex.
3. Can a VFS provider be added without breaking the "earlier mounts win" order and the worker-side `walkTask`
   contract? (P1)

## 6. Not in scope

- Running acoustics in Apex's viewport as audio (playback). Visual readouts only.
- Any brush-acoustics data in the VMF beyond entities.
- Editing Valve's `surfaceproperties` files; the acoustic table is separate and keyed by the same names.

## 7. Findings from the brush-acoustics plugin (ABI 1.0, 2026-11-17)

brush-acoustics' plugin (its `apex/` folder; panel, materials section, problems check) runs on ABI 1.0. Three
things stood in the way:

1. **Sides can't be read (P3).** A `Solid` keeps its `side` blocks by value inside its own block
   (`src/document/document.hpp`), so `child_at` never reaches them and `key_at` lists only value keys
   (`src/plugin_host/plugin_host.cpp`, `keyAt`). A probe plugin walking a two-solid map sees `solid children=0
   keys=id`. No plugin can map a side ID (from selection, a face-properties context or `ray_pick`) to its
   material or plane, so the surface overlay, per-face readouts and per-map checks are off, and P4's per-face
   heat map and the grid/ray drawing (which need planes) can't be built. Proposed, additive (rule 7, minor 1.1):
   append to `apex_document_v1`
   `side_count(host, solid, size_t*)` and `side_at(host, solid, index, apex_side_info*)`, where
   `apex_side_info { uint32_t struct_size; int32_t id; apex_vec3 plane[3]; apex_str material; apex_vec3
   uaxis, vaxis; double shift_u, shift_v, scale_u, scale_v; }` (an out parameter, rule 6), plus
   `side_lookup(host, int32 side, apex_node_id* solid, size_t* index)` for selection contexts. A generic
   nested-block reader would also do.
2. **No game directory.** Nothing reports the profile's game folder, so the plugin keeps the Godot project path
   as its own setting, and `watch()` refuses `materials.kv` unless the game folder is that project. Proposed: a
   `game_directory(host, apex_str*)` appended to `apex_assets_v1`.
3. **Bug: a section's material names dangle.** `Host::describePanel` (`plugin_host.cpp`, the `names` loop)
   iterates `context ? context->names : std::vector<std::string>{}`. The conditional is a prvalue, so the loop
   runs over a temporary copy and `names` keeps pointers into strings destroyed when the loop ends: every
   `"materials"` section gets dangling `names` (the UI's `PluginUi` calls this path). Seen in a harness as
   `concrete/wall01` arriving as `ui.describe l01`. Fix: `if (context) for (const auto& n : context->names)
   names.push_back(...)`; a test with a `names` context (face-stats only passes `ids`) would have caught it.

Status (2026-11-17): findings 1 and 3 are done. ABI 1.1 appends `side_count`, `side_at` and `side_lookup` to
`apex_document_v1` with `apex_side_info` as proposed (176 bytes; a value that doesn't parse reads as zero so one
bad side hides nothing else; `docs/plugins/document.md`, `versioning.md`), tested by `side_plugin.c`. The
`describePanel` loop now borrows `context->names`, and the ui test uses names past the small-string buffer
(it fails on the old line). `plugin::available()` takes any host table. Finding 2 is still open.
