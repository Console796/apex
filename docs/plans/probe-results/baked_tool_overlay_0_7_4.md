# Tool-texture fixed-brightness overlay (0.7.4, slice 6a part 1)

Status: implemented and verified.

## Investigation
Tool materials (`tools/*`) are `noLight` in `render::setupBake` (bake_setup.cpp), so they receive no
transport and Baked shows them black; in Lit they are lit like any brush. The editor classifier is
`isToolFace` (`tools/` prefix except `tools/toolsskybox*`) and `isNodraw` (`tools/toolsnodraw` or
`%compilenodraw`) in render_mirror.cpp; the overlay reuses exactly those two.

## What the overlay does
`Overlays::toolFixedBrightness` (viewport JSON key `toolFixedBrightness`, default false, menu entry
"Tool textures: fixed brightness") -> `MirrorViewOptions::toolFixedBrightness` -> `DrawList::toolBrightness`
(= `render::kToolFixedBrightness`, 1.0, only in Lit/Baked) and `MeshDraw::toolFixed` per tool batch ->
push `p1.w` bit 64 + `FrameBlock.fill.w` (brightness / exposure, so on-screen level is exposure-independent).
brush_textured.frag.glsl sets diffuse irradiance to that constant, skipping lightmap/lights/fill.
Envmap, self-illumination and fog still apply. Brightness 1 shows linear albedo, identical to the unlit
Textured view's sRGB output. No struct size changed (fill.w was reserved).
`apex_render_pose --tool-fixed-brightness [VALUE]` also un-hides tools faces (sky/nodraw stay per flags).

## Verification
Pose 11 (set 2), `--mode baked --bake-passes 4`, region x100-300 y600-700 (translucent `tools/toolstrigger`,
RGB mean): overlay off 36/35/42 (dark, lightmapped black); `--tool-fixed-brightness` 165/102/31;
0.5 -> 122/76/31; 0.1 -> 62/42/31 (blue is the translucent blend with the background). Lit mode: off
35/34/42, on 165/102/30, matching Baked. Textured view of the same region 155/123/72 (different
background through the translucency). Pose 21 sees no tool faces and is pixel-identical off/on.
Images: scratchpad `tool/{off,v1,v0.5,v0.1,lit-off,lit-on,tex}/`.
Tests: viewport JSON round trip and back-compat default; mirror draw-list test (trigger and nodraw
flagged, red box and sky not, Lit/Baked only); shader compiled and ran on RADV (RX 6600).
Full ctest: all pass.

## Unmapped
`%compilenolight` non-tools materials are not tracked by the mirror's MaterialState, so they are not
flagged; toolsnodraw is only visible in the editor (render_pose keeps nodraw hidden like the game).
