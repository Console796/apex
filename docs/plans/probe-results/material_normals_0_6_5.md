# Material normal decoding (0.6.5 a/b, 2026-10-29)

## Green-channel measurement

Read-only input: `$APEX_P2CE_DIR/portal2/portal2/portal2_dir.vpk`, entry
`materials/tile/white_wall_tile003a_normal.vtf` (512 × 512).
SHA-256 of the VTF bytes:
`64db85afa8a2a65bcbae014838f17d2b717fad32b1e3105fd8e4d01e8594eaa4`.

The companion `materials/tile/white_wall_tile003a.vtf` shows two raised rectangular tiles
with recessed perimeter seams. The top edge slopes out toward decreasing image row / V;
the bottom edge slopes out toward increasing V. The normal map's broad interior is flat,
with opposed narrow bevels at those edges. Sampling column 128 avoids the vertical seams.
Decoded mip 0 RGB bytes (origin at the top left, no colour conversion):

| x | y | R | G | B | Geometric interpretation |
|---|---|---|---|---|---|
| 128 | 2 | 123 | 24 | 197 | upper bevel, normal toward −V |
| 128 | 4 | 123 | 49 | 230 | upper bevel transitioning to the face |
| 128 | 128 | 129 | 125 | 255 | nearly flat face |
| 128 | 507 | 123 | 206 | 230 | lower bevel transitioning from the face |
| 128 | 509 | 123 | 235 | 197 | lower bevel, normal toward +V |

Thus `2*G/255 − 1` is the component along **increasing texture V**. Do not flip green
when the cotangent frame uses the gradient of V. This statement specifies the frame as
well as the channel; an “OpenGL/DirectX” label alone does not specify that relationship.
Flipping green makes the raised tile's upper and lower bevels point into the tile.
The inference assumes the visibly raised panels rather than the seams are the high region;
the companion ssbump below independently agrees with the edge directions.

Reproduction: extract the named entries through the VPK reader, hash the normal VTF, and
use the repository's pinned SourcePP `vtfpp::VTF::getImageDataAs(RGBA8888, 0, 0, 0, 0)`.
The stock VTF is 7.6 with compressed image resources, so treating its payload as plain DXT
bytes gives an invalid result. For this probe a temporary C++ decoder linked the existing
build's SourcePP libraries, wrote RGB PPMs under `/tmp`, and Pillow read the pixel rows
above. The base and normal PPMs were visually inspected. All extraction, compilation and
decoding output stayed under `/tmp`; nothing was written inside the install. No SDK or
leaked source was consulted.

## Ssbump

The public [VDC `$ssbump` documentation](https://developer.valvesoftware.com/wiki/$ssbump)
describes RGB as weights on three fixed lighting directions, rather than signed XYZ normal
components. The wiki returned HTTP 403 during this run; the RNM basis geometry is also
published on slide 10 (PDF page 10) of Valve's public
[2004 shading presentation](https://cdn.akamai.steamstatic.com/apps/valve/2004/GDC2004_Half-Life2_Shading.pdf).
No implementation code from that presentation is used.

Using coordinates along +U, +V and the geometric normal, Apex's basis is:

- R: `(sqrt(2/3), 0, sqrt(1/3))`
- G: `(-sqrt(1/6), sqrt(1/2), sqrt(1/3))`
- B: `(-sqrt(1/6), -sqrt(1/2), sqrt(1/3))`

The stock companion `materials/tile/white_wall_tile003_height-ssbump.vtf` (selected by
`white_wall_tile003a.vmt`, with `$ssbump 1`) confirms the G/B orientation at column 128:
row 2 is `(58, 0, 173)`, row 128 is `(145, 144, 145)`, and row 509 is `(58, 190, 0)`.
Blue weights the upper / −V bevel, green the lower / +V bevel, and equal weights are flat.

For this preview, decode as `normalize(R*basisR + G*basisG + B*basisB)` with raw linear
weights. This deliberately discards directional occlusion magnitude: it approximates a
normal, rather than reproducing baked ssbump self-shadowing. All-zero weights fall back
to `(0,0,1)`; a zero-length signed RGB normal does too. BC5 normals reconstruct positive Z
from the two signed RG components before normalization.

## Frame and verification

The fragment shader differentiates camera-relative, preview-transformed position and
final UV, projects texture gradients onto the geometric normal's plane, and normalizes
each gradient independently. This keeps texture density from scaling bump strength,
preserves mirrored and rotated mappings, and follows brush texture axes without adding
vertex attributes. Collapsed UVs fall back to the geometric normal. Model frames are
reconstructed from UVs as well; exact authored tangent discontinuities cannot be recovered
when they are independent of the UV/normal seams (D9 amendment).

The CPU model and compute probe compare decoded normals and frames, including reflected,
skewed and collapsed UVs, zero texels and random orientations. Analytical cases establish
flat normals, the +V convention, equal ssbump weights and mirrored U independently of
image goldens. Synthetic mapped quads check a grazing sun, sky ambient, both slot-selection
paths, Textured/Flat/Wireframe equality and fullbright equality. An actual decoded model
checks the VertexLitGeneric cache-to-draw path under a grazing sun. A float-material quad checks HDR energy
through RGBA16F upload without a colour decode.
