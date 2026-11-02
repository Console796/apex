# Offscreen poses with CPU baked lightmaps

`apex_render_pose --mode baked` expands instances into the document before both baking and
rendering, retaining identical document NodeId/SideId keys. It shares instance resolution,
material compile flags, VTF linear reflectivity and caster setup with `apex_bake_probe` via
`tools/common/bake_setup.*`. Neither tool uses an FGD table. Bake scope is visible world and
func_detail brush faces; sky/nodraw occlude, other tools faces do not. Props, displacements and
other brush entities do not contribute baked transport.

Run from the repository root (output directory must be new):

```sh
QT_QPA_PLATFORM=offscreen build/linux-clang-release/apex_render_pose \
  --vmf "$HOME/maps/apex/test1.vmf" --game "$APEX_P2CE_DIR" \
  --mode baked --bake-passes 8 --bake-bounces 2 --bake-bounce-rays 32 \
  --bake-threads 3 --bake-seed 0 --bake-scale 2.02 \
  --pose 11 -1135.97 -1135.97 64.03 -9.13 34.18 0 \
  --pose 21 -645.80 -1078.80 64.03 -7.55 177.66 0 \
  --pose 31 -1046.74 -693.74 64.03 -7.03 -83.60 0 \
  --pose 41 -1135.97 -973.72 64.03 -10.20 -9.25 0 \
  --out /tmp/apex-baked-set2
```

Defaults: 16 passes per direct/bounce level, 2 bounces, 32 bounce rays, 3 threads, seed 0,
display scale **1.0**, exposure 1. `--bake-scale 2.02` is the optional fitted set-2
baker-to-display multiplier from `docs/plans/probe-results/bake_parity_set2_0_7_4.md`;
no scale is applied to the baker's accumulation or uploaded radiance. Width/height/FOV defaults
match the 1920x1080 game captures. Baked filenames are `11-baked.png` through `41-baked.png`.
Lit/textured/fullbright behavior and filenames are unchanged.

For a signed luminance diff and region measurements, this optional Python recipe uses Pillow
and NumPy (analysis only; neither is a build dependency). Red is brighter in Apex, blue darker;
intensity is four times absolute linear Rec.709 luminance error. It writes `N1-diff.png` and
prints editor/game ratios. Rectangles are exclusive at the right/bottom and avoid HUD/crosshair:

```sh
python3 - <<'PY'
from pathlib import Path
from PIL import Image
import numpy as np
out = Path('/tmp/apex-baked-set2')
game = Path.home() / 'maps/apex/captures/set 2'
weights = np.array([.2126, .7152, .0722])
def linear(path):
    v = np.asarray(Image.open(path).convert('RGB'), dtype=float) / 255
    return np.where(v <= .04045, v / 12.92, ((v + .055) / 1.055)**2.4)
regions = {'floor': (400,900,1000,1040), 'ceiling': (400,20,800,140),
           'shaded wall': (650,500,850,730)}
for n in range(1, 5):
    a, b = linear(out / f'{n}1-baked.png'), linear(game / f'{n}1.png')
    delta = 4 * ((a-b) @ weights)
    heat = np.stack([np.maximum(delta,0), np.zeros_like(delta), np.maximum(-delta,0)], axis=2)
    Image.fromarray(np.uint8(np.clip(heat,0,1)*255)).save(out / f'{n}1-diff.png')
    if n == 1:
        for name, (x0,y0,x1,y1) in regions.items():
            editor = (a[y0:y1,x0:x1] @ weights).mean()
            reference = (b[y0:y1,x0:x1] @ weights).mean()
            print(name, 'editor', editor, 'game', reference, 'ratio', editor/reference)
PY
```

These compare complete rendered surfaces, including texture detail, reflections, self-illumination
and material differences. They are not the probe's isolated lit/fullbright irradiance statistics.

## Renderer API and next editor slice

`LightmapAtlas::pack` preserves `BakeScene::faces` indices (the exact row order of `Baker::resolve`).
Each fragment gets a separate interior rectangle surrounded by a duplicated one-texel guard.
Packing is deterministic, uses descending-height shelves, grows a square to at most 4096, and
returns an error if it cannot fit. Callers can supply a different maximum up to 16384.
`uv(face, world)` uses the chart's worldToLuxel/offset/mins and the texel-centre +0.5 convention.

Create `BakedLightmaps` with the render Context/TextureHeap, then call `upload(baker.resolve())`.
`RenderMirror::setLightmaps(maps)` schedules remeshing with a 44-byte BrushVertex (one additional
UV pair). The mesher uses the exact bake fragment polygons and original surface texture mapping.
Faces retain their source SideId/selection ranges. Unmatched/edited charts use current geometry
and negative UVs. Fragment matching uses the source NodeId, SideId and the ordered chart loops;
bake and mirror must use the **same expanded document snapshot**, not independently assigned IDs.
This slice's tool collapses instances for that reason. Live instance mirrors still need an explicit
expanded-instance identity mapping in the next slice.

`maps->upload(resolve, selectedFaceIndices)` uploads only selected rows and their guard texels,
without repacking, reallocating or changing UVs/texture slot. Unselected rectangles retain their
previous values. This implementation is synchronous: call on the main thread between frames;
it waits for in-flight draws and completes staging copies before returning. Context/heap must
outlive maps; CPU meshing jobs retain only the immutable layout. Replace the layout and call
`setLightmaps` again when charts change; call with nullptr to clear it. A radiance-only update
requires a redraw, but no geometry upload. Dirty-face scheduling, revision tracking, redraw
notification, invalidation of changed lights/materials, and asynchronous transfer fences remain
for the live editor slice. Do not reuse the bake snapshot across document reloads.

Baked brushes sample RGBA16F with bilinear clamp-to-edge (no anisotropy or mipmaps), multiply
diffuse by `DrawList::bakedScale` (default 1), and share Lit's HDR target, tonemap, exposure,
material normals/parallax, envmaps, self-illumination, alpha and fog. Direct-light specular is off
for baked brushes. Unmapped brush diffuse is black. Models/props, displacements and tool previews
use the existing Lit path, including its lights/shadows. Other brush entities are unmapped brushes.
The editor's Baked menu action stays disabled; this slice only provides renderer plumbing.
