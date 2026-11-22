# LOD Shadows port (branch `foxnne/lod-shadows`)

Port of the open-shaders "LOD Shadows" feature (`openshaders/dev`: d66ccad74 #949, a973fb2e4 #1029,
bb327f2d4 #1087, 503b5a161 #1125) onto `foxnne/nr-pr6`. Distant terrain, object LOD (which is where
DynDOLOD "Ultra" 3D tree LOD lives) and billboard tree LOD cast and receive sun/moon shadows past the
shadow cascades.

Not deployed and not run in game. The PR-Tracy build passes with zero errors
(`build/PR-Tracy/Release/CommunityShaders.dll`); the shaders were only compiled offline with fxc
(Lighting deferred / forward / LOD land / LOD objects / landscape, RunGrass, DistantTree, the caster).

**Deploying:** PR-Tracy builds the DLL only, there is no AIO folder. Along with the DLL, copy
`features/LOD Shadows/Shaders/` (`Features/LODShadows.ini`, `LODShadows/*.hlsl*`) and the changed
`package/Shaders/Common/{SharedData,ShadowSampling}.hlsli`, `package/Shaders/Lighting.hlsl` and
`en.json` into the mod's `Shaders` / `SKSE` folders, and clear the shader cache: `FeatureData`
grew, so every shader must be rebuilt against the new `SharedData.hlsli`.

## What was ported

- `src/Features/LODShadows.{h,cpp}`, `LODShadowSettings.h`, `LODShadowUpdateBudget.h`: the depth cache
  (3 maps, half extents 18432 / 55296 / >=165888 units, 2048 x 2048, D16), three slices per map
  (current, previous, in progress) so a new map can fade in over the old one, plus a second copy of
  each slice without the loaded objects the native cascades already shadow. Casters: terrain LOD,
  loaded land, object LOD, tree LOD (instanced billboards, alpha tested with the tree atlas), loaded
  statics and trees. The per-frame triangle budget adapts to a GPU time target from timestamp queries.
- `src/Utils/GpuTimestamps.{h,cpp}`: copied unchanged from open-shaders (the budget needs it).
- `features/LOD Shadows/`: `CORE` marker, `LODShadows.ini` (1-0-0), `Caster.hlsl`, `LODShadows.hlsli`.

## What differs from open-shaders, and why

- **No free texture slots.** This fork uses t104-t127 for Landscape Seams, Fur Shells, Night Eye and
  the atmosphere volume, so upstream's t117 / t118 / t127 are taken. The depth array is on **PS t49**
  (free in pixel shaders; Physical Sky only uses it in compute), and the map data goes through the
  shared feature buffer (`SharedData::lodShadowsSettings`, last member of `FeatureData`,
  `LODShadows::GetCommonBufferData()` in `FeatureBuffer.cpp`).
- **Left out:** the half-resolution filtered receiver mask (needs another slot and a compute prepass),
  the effect ray bounds and the effect constant buffer, hardware comparison sampling (needs the per-draw
  hook this fork does not have), VR, the IBL reflection-radiance capture, the devbench actions and
  runtime flags (a Debug section replaces them).
- **Receivers are Lighting, grass and tree LOD only** (`HasShaderDefine`). Effects, particles, water
  and the volumetric / fog compute shaders do not sample the maps yet.
- **CommonLib:** upstream is on CommonLibSSE-NG 15.2; ours is older and lacks the terrain manager
  statics, `GetLODMode`, `BSMeshLODTriShape` and the tree atlas accessor. They are small local shims at
  the top of `LODShadows.cpp` (address ids and offsets taken from 15.2), the submodule is not bumped.
- No `FullscreenPassScope` here: `CasterPassScope` in `LODShadows.cpp` saves and restores the state the
  caster pass touches.

## Integration

- **Terrain Shadows: coexist.** `TerrainShadows.cpp` is untouched, so the world map shadows and the
  height map normals keep working. In `ShadowSampling::GetWorldShadow` the LOD maps are sampled first;
  the height map terrain shadow is then weighted by `1 - coverage` once all three maps are complete
  and hold the terrain (`LODShadows::CoversTerrain`), and applied in full otherwise (maps still
  building, terrain caster unsupported, outside the maps, world map). Upstream instead switches the
  height map off and frees its texture.
- **Sky Sync / Physical Sky:** the light direction is the engine sun light
  (`shadowSceneNode[0]->sunLight`), the same source Terrain Shadows uses, so it follows whatever body
  Sky Sync puts there. A completed celestial transition forces an immediate full refresh
  (`Util::GetCompletedCelestialTransitionGeneration`).
- **Tree Wind:** casters are drawn at rest, without sway. Tree LOD billboards already look up the world
  shadow at `RestWorldPosition`, so caster and receiver agree. Object LOD in `Lighting.hlsl` is looked
  up at the bent position; the sway there is small next to a map texel (18 / 54 / 160+ units) and the
  depth bias (8 units).
- **Lighting.hlsl:** one call site changed, it passes `ddx` / `ddy` of the world position so the maps
  can correct for the receiver plane. Grass and tree LOD use the existing 3-argument call. The world
  shadow multiplies `dirLightColor` before pre-exposure, as before; nothing in `PreExposure.hlsli`
  is involved.
- **Light Limit Fix, Screen Space Shadows, Cloud Shadows:** not touched. Cloud shadows still multiply
  on top. Upstream's fade of screen-space shadows past the cascades was not ported, so they still
  apply at distance together with the LOD shadows.
- Inside the native cascades a pixel reads the slice without loaded objects (they are shadowed by the
  cascades), blending to the full slice over the last fifth of the cascade range.

## Settings (Shadows category, "LOD Shadows")

| Setting | Default | Range |
| --- | --- | --- |
| Enable LOD shadows (`EnableLODShadows`) | **on** (upstream: off) | |
| GPU update budget (`UpdateBudgetMs`) | 0.1 ms | 0.01 - 0.4 |
| Depth bias (`DepthBias`) | 8 units | 0 - 64 |
| Texel bias (`TexelBias`) | 1 texel | 0 - 4 |
| Tree crown bias (`CrownBias`) | 256 units | 0 - 1024 |
| Light direction step (`DirectionStepDegrees`) | 0.5 degrees | 0 - 2 |
| Far map limit (`FarMapLimit`) | 0 (none) | 0 - 1048576 |

## Stability changes (branch `foxnne/lod-shadows-stability`)

Not run in game either. What changed against the port above:

- **Held light direction.** Each map is drawn for a direction that stays fixed until the light has moved
  a step (far map: the setting, middle: half, near: a quarter), then moves half a step ahead of the
  light and the map fades to it. Between steps a redraw (camera moved, scene changed, cache expired)
  lands on the same texel and depth grid, so shadow edges do not crawl. The step grows with the measured
  angular speed of the light so that no map retargets more than once per round of redraws plus fade.
- **Jumps of the light** (wait, sleep, set gamehour): maps more than 4 degrees and 3 steps off fade out
  over 0.5 s, work in progress for the old direction is dropped, and the new maps are drawn within the
  budget and fade in. Nothing redraws all maps in one frame any more except the debug switch, and the
  budget no longer scales with the timescale.
- **Bias.** `DepthBias` plus `TexelBias` texels of the map plus 1.5 steps of D16. Crowns of object LOD
  trees (Tree Wind `LODTreeCrown`) are looked up `CrownBias` units toward the light and without the
  receiver plane, so they are not shaded by themselves.
- **Far map** keeps its size while the casters need up to 4 cells less, and can be capped.
- **CPU.** The LOD roots are scanned with the loaded references every 4th frame (every 16th while a map
  is being drawn) instead of every frame. The caster list is built once per scene hash and each map
  only filters it; before, every map start walked every geometry.

Debug section: valid maps, caster counts, GPU timings, "redraw all maps every frame", history blend
and scan throttle toggles, "Rebuild maps".

## Expected cost

- VRAM: 18 slices of 2048 x 2048 R16 = about 151 MB.
- GPU update: capped by the budget (0.1 ms per frame by default). See "Stability changes": no full
  refresh in one frame.
- CPU: the loaded-reference walk runs every 4th frame and reuses cached geometry per reference, with a
  full walk every 8th scan or on a cell change.
- Sampling: per lit exterior pixel one 4-tap gather per map it falls in (usually one, two in the 5%
  border), doubled while a new map fades in and doubled again in the cascade fade band.

## Needs in-game testing

1. It loads at all: feature listed, `LODShadows.ini` found, no shader compile failure for Lighting,
   RunGrass and DistantTree; Debug section shows 3 valid maps a few seconds after loading an exterior.
2. The address shims on the installed runtime (SE and AE ids are both given, neither was exercised).
   A wrong id shows as a crash on entering an exterior or as zero geometry counts.
3. Distant terrain, object LOD and 3D tree LOD receive and cast shadows; they follow the sun and the
   moon, and refresh after waiting / fast travel.
4. Shadow acne or peter-panning on LOD terrain and on tree LOD crowns (Depth bias), and hard texel
   edges in the far map, which has no filtering beyond bilinear now that the mask is left out.
5. The seam at the end of the native cascades: double shadow or a lit gap in the fade band.
6. Terrain Shadows hand-over: no doubled or missing mountain shadow, world map unchanged.
7. Sky, Light Limit Fix spots, screen-space shadows and cloud shadows look as before.
8. Frame time with the feature on and off, and the "GPU update" numbers in the Debug section.
9. Interiors and Interior Sun cells: nothing should change (no worldspace, maps stay invalid).
