#ifndef GROUND_FOG_HLSLI
#define GROUND_FOG_HLSLI

// Ground Fog: a thin sheet of fog that follows the terrain.
//
// This file holds what both ways of drawing it share: the constants, the ground source, the shaping noise and the
// height profile with its closed-form integrals.
// - GroundFogPixel.cs.hlsl integrates the layer along each pixel's view ray (the default).
// - EvaluateCell below injects it into the shared atmosphere volume (setting VolumeInjection). A froxel cell is
//   often taller than the layer, so the density is never point sampled there: each cell gets the exact mean of the
//   height profile over the heights the cell covers.
//
// The idea of a noise-shaped column top with an exponential profile integrated in closed form comes from
// Iwillbewarlock's GroundFog (GPL-3.0). No code is shared with it.
//
// Include after Common/Random.hlsli and Common/ViewAtmosphere.hlsli, with a clamped linear `LinearSampler` in scope.

namespace GroundFog
{
	cbuffer GroundFogCB : register(b2)
	{
		float4 MapScaleOffset;   // height map uv = absolute xy * xy + zw
		float2 MapZ;             // world z at texel value 0 and 1
		float2 MapEdgeFade;      // uv distance from the map edge to full strength, as a reciprocal
		float Thickness;         // top of the tallest mounds above the ground, game units
		float Steepness;         // density falls to exp(-Steepness) at the top
		float GroundExtinction;  // extinction at the ground, per game unit, amount included
		float GroundFill;        // depth below the ground that still holds fog, game units
		float FadeStart;         // game units
		float FadeEnd;
		float2 WindTravel;  // how far the wind has carried the pattern, game units
		float InvNoiseScale;
		float NoiseStrength;
		float InvBankScale;
		float BankStrength;
		float BasinStrength;
		float BasinRadius;  // game units
		float InvBasinDepth;
		float WaterPreference;
		float Albedo;
		float SunScale;
		float SkyAmbientScale;
		float SelfShadow;
		uint Flags;      // bit 0: raise the ground to the water surface, bit 1: the pixel pass runs at half resolution
		uint DebugMode;  // 0 off, 1 slab, 2 ground height, 3 layer mask, 4 tap count
		// Pixel pass only.
		float SurfaceBlend;  // length of the end of the view ray over which the ground becomes the visible surface
		float SurfaceLift;   // a surface up to this far above the mapped ground is ground, game units
		float StepMin;       // smallest reach of a ground read, game units
		float StepGrowth;    // reach of a ground read as a share of the distance from the camera
		uint MaxSteps;
		float WaterTop;       // highest water surface near the camera, absolute z
		float2 PyramidSize;   // texels of the first level of HeightPyramid
		float PyramidLevels;  // number of levels of HeightPyramid
		float MapTexel;       // game units per height map texel
		float2 MapSize;       // texels of HeightMap
		// The valley layer: deeper fog in hollows and over water, from where the sheet fades out to FarEnd.
		float FarExtinction;  // extinction at the ground, per game unit, amount included; 0 = none
		float FarThickness;   // game units
		float FarEnd;         // game units
		float FarBasinRadius;
		float InvFarBasinDepth;
		float CloudDarkening;  // how much cloud overhead dims the sky light the fog takes
		// The near volume: a taller, softer body of fog around the camera, on top of the sheet.
		float NearExtinction;  // extinction at the ground, per game unit, amount included; 0 = none
		float NearThickness;   // height at which it has thinned to nothing, game units
		float NearFalloff;     // density falls by exp(-NearFalloff) over NearThickness, then is cut to reach zero there
		float NearEnd;         // game units from the camera; fades out over the second half
		float InvDriftScale;   // patches of the density modifier, per game unit
		float DriftStrength;
		float BreathPhase;   // radians, advances with time
		float NearBreath;    // share by which the near volume's top rises and falls
		float2 FloorAnchor;  // camera-relative xy of the player
		float FloorOffset;   // how far the floor the player stands on lies above the fog's ground there, game units
		float FloorMax;      // visible level floors up to this far above the mapped ground carry the near volume
		float SoftLength;    // fog fades over this last stretch of a ray that ends on something that is not ground
		// Burn-off: amounts by time of day for ground the sun has reached, and what ground in terrain shadow keeps.
		float BurnSunlitNear;
		float BurnSunlitFar;
		float BurnShaded;
		float BurnRetention;  // share of the way from the sunlit to the shaded amount that shadow gives; 0 at night
		// Water. With a baked water map (WaterTexel > 0) the surface is known wherever the height map reaches;
		// without one it comes from the engine's data for the cells around the camera (Flags bit 0).
		float InvShoreDepth;    // 1 / the water depth over which fog over water comes in at a shore, per game unit
		float WaterPatchiness;  // how uneven the fog over water is
		float WaterTexel;       // game units per texel of WaterMap; 0 = no map
		float4 FlowWindow;      // fog simulation window: uv = camera-relative xy * z + xy; w > 0 when there is one
		// The pool: the knee-high fog around the camera, drawn on what the screen shows (GroundFog/Pool.hlsli).
		float PoolExtinction;  // extinction at the ground, per game unit, amount included; 0 = none
		float PoolThickness;   // height at which it has thinned to nothing, game units
		float PoolEnd;         // game units from the camera; fades out over the second half
		float SunWrap;         // share of an even (isotropic) phase added to the sun and moon light the fog scatters
	};

	Texture2D<float> HeightMap : register(t6);
	// Coarser copies of HeightMap for the pixel pass. Level 0 has half the size of the map. x: mean height, for
	// taps that stand for a long stretch of the ray. y: the highest height in and around the texel, which bounds the
	// ground under a whole stretch from above (see GroundMaxAlong).
	Texture2D<float2> HeightPyramid : register(t7);

	// The baked water map of the worldspace (terrain-heightmap/build_watermap.py), over the same rectangle as the
	// height map, with mips. x: the water surface, encoded like HeightMap. y: 1 where the cell has water; 0 where it
	// has none, fading inside the wet side where water runs into a cell without any. z: the highest water surface
	// in and around the texel (one texel on every side), 0 = none.
	Texture2D<float4> WaterMap : register(t9);

	static const float TopLimit = 0.3;  // banks raise the layer by at most this share per unit of BankStrength

	struct Cell
	{
		float extinction;  // mean extinction of the cell, per game unit
		float3 lightPos;   // camera-relative point inside the layer, for lighting
		float tauAbove;    // vertical optical depth of the fog above lightPos
		float3 albedo;
	};

	struct Profile
	{
		float fill;   // constant density from -fill to 0
		float top;    // exponential from 0 to top, nothing above
		float scale;  // e-folding height
		float total;  // integral over the whole profile
	};

	float2 MapUv(float2 xy)
	{
		return (xy + FrameBuffer::CameraPosAdjust.xy) * MapScaleOffset.xy + MapScaleOffset.zw;
	}

	// 0 outside the height map, rising to 1 one cell inside it.
	float MapCoverage(float2 uv)
	{
		const float2 inside = min(uv, 1.0 - uv) * MapEdgeFade;
		return saturate(min(inside.x, inside.y));
	}

	float TerrainZ(float2 uv)
	{
		return lerp(MapZ.x, MapZ.y, HeightMap.SampleLevel(LinearSampler, uv, 0));
	}

	// The terrain at a level of detail: 0 is the height map, each level above it has texels twice as long. Continuous
	// in lod, so the ground a ray is integrated over changes smoothly with distance.
	float TerrainZ(float2 uv, float lod)
	{
		float height;
		if (lod >= 1.0) {
			height = HeightPyramid.SampleLevel(LinearSampler, uv, lod - 1.0).x;
		} else {
			height = HeightMap.SampleLevel(LinearSampler, uv, 0);
			if (lod > 0.0)
				height = lerp(height, HeightPyramid.SampleLevel(LinearSampler, uv, 0).x, lod);
		}
		return lerp(MapZ.x, MapZ.y, height);
	}

	// ---------------------------------------------------------------------------------------------------------
	// Water. The fog lies on max(terrain, water). How much of a place is water is not a per-cell yes or no: it is
	// the water's depth over the terrain there, so shorelines follow the terrain, and the fog over water comes in
	// over the first 1 / InvShoreDepth of depth.

	bool HasWaterMap()
	{
		return WaterTexel > 0.0;
	}

	// The water map covers the height map's rectangle, but its texels are areas where the height map's are terrain
	// vertices (MapUv is shifted by half a height map texel for that).
	float2 WaterUv(float2 uv)
	{
		return uv - float2(0.5, -0.5) / MapSize;
	}

	// No water surface within `hold` of a point, nor in anything a filtered read there touches, is higher than
	// this (absolute z); far below everything where there is no water. One point read of a coarse level.
	float WaterCeiling(float2 uv, float hold)
	{
		uint width, height, levels;
		WaterMap.GetDimensions(0u, width, height, levels);
		const int mip = int(clamp(ceil(log2(8.0 * max(hold, 1.0) / WaterTexel)), 0.0, float(levels) - 1.0));
		const int2 size = max(int2(width, height) >> mip, 1);
		const int2 texel = clamp(int2(WaterUv(uv) * float2(size)), 0, size - 1);
		const float highest = WaterMap.Load(int3(texel, mip)).z;
		return highest > 0.0 ? lerp(MapZ.x, MapZ.y, highest) : -1.0e30;
	}

	// The engine's water for the cells around the camera, for worldspaces without a water map. x: how far the
	// surface lies above the terrain (0 or less: none), y: 1 inside the known cells, fading to 0 toward every edge
	// that borders a cell without data, so the fog does not end in the outline of a cell.
	float2 LiveWater(float2 xy, float terrain)
	{
		const float2 cellPosition = (xy + FrameBuffer::CameraPosAdjust.xy) / 4096.0;
		const float2 cellFloor = floor(cellPosition);
		const float2 f = cellPosition - cellFloor;
		const int2 cell = int2(cellFloor - floor(FrameBuffer::CameraPosAdjust.xy / 4096.0)) + 2;
		const float own = SharedData::GetWaterTile(cell).w;
		if (own < -1.0e6)
			return 0.0;
		float reach = 1.0;
		[unroll] for (int dy = -1; dy <= 1; ++dy)
		{
			[unroll] for (int dx = -1; dx <= 1; ++dx)
			{
				if ((dx != 0 || dy != 0) && SharedData::GetWaterTile(cell + int2(dx, dy)).w < -1.0e6) {
					const float2 away = float2(dx < 0 ? f.x : (dx > 0 ? 1.0 - f.x : 0.0), dy < 0 ? f.y : (dy > 0 ? 1.0 - f.y : 0.0));
					reach = min(reach, length(away));
				}
			}
		}
		return float2(own + FrameBuffer::CameraPosAdjust.z - terrain, smoothstep(0.0, 0.35, reach));
	}

	// Raises a ground sample to the water over it. ceiling: from WaterCeiling, or a large number to always look.
	// water: how much of the surface is water. amount: what is left of the fog there; below 1 only where known
	// water ends against a cell without any (the surface stays at the water's level and the fog on it thins out:
	// lowering the surface instead would sink the fog under the water at once and leave a straight edge).
	// Returns the texture reads it made.
	uint ApplyWater(float2 xy, float2 uv, float lod, float ceiling, float terrain, inout float z, out float water, out float amount)
	{
		water = 0.0;
		amount = 1.0;
		float2 surface = 0.0;  // x: depth of the water over the terrain, y: validity
		uint reads = 0u;
		if (HasWaterMap()) {
			if (terrain < ceiling) {
				const float4 texel = WaterMap.SampleLevel(LinearSampler, WaterUv(uv), max(lod - log2(WaterTexel / MapTexel), 0.0));
				surface = float2(lerp(MapZ.x, MapZ.y, texel.x) - terrain, texel.y);
				reads = 1u;
			}
		} else if (Flags & 1u) {
			surface = LiveWater(xy, terrain);
		}
		if (surface.x > 0.0 && surface.y > 0.0) {
			z = terrain + surface.x;
			water = smoothstep(0.0, 1.0, surface.x * InvShoreDepth);
			amount = lerp(1.0, surface.y, water);
		}
		return reads;
	}

	// Absolute z of the surface the fog lies on at a camera-relative xy; water: how much of that surface is water.
	float Ground(float2 xy, out float water)
	{
		const float2 uv = MapUv(xy);
		const float terrain = TerrainZ(uv);
		float z = terrain;
		float amount;
		ApplyWater(xy, uv, 0.0, 1.0e30, terrain, z, water, amount);
		// The volume path has one factor for water; the thinning toward unknown water goes through it.
		water *= amount;
		return z;
	}

	// ---------------------------------------------------------------------------------------------------------
	// Ground source for the pixel pass. The march learns about the ground only through GroundAt, GroundMaxAlong and
	// GroundAround (and MapCoverage for where the source has data), all taking camera-relative xy. Today they read
	// the terrain height map and the water map. Another source (a floor found from the depth buffer for interiors,
	// a simulation texture) only has to supply these.

	struct GroundSample
	{
		float z;        // absolute z of the surface the fog lies on
		float terrain;  // absolute z of the land under it
		float water;    // how much of that surface is water: 0 on land, rising to 1 over the shore depth
		float amount;   // share of the fog left there: below 1 only where known water ends against unknown
		uint reads;     // texture reads this sample took
	};

	float GroundLod(float reach)
	{
		return max(log2(reach / MapTexel), 0.0);
	}

	// reach: the size of terrain feature this read should still see, game units; it grows with distance from the
	// camera. The march puts its joints on the texel grid of the level this selects (GroundCell), so between two
	// joints the ground read here is straight. waterCeiling: see ApplyWater.
	GroundSample GroundAt(float2 xy, float reach, float waterCeiling)
	{
		GroundSample ground;
		const float2 uv = MapUv(xy);
		const float lod = GroundLod(reach);
		ground.terrain = TerrainZ(uv, lod);
		ground.z = ground.terrain;
		ground.reads = 1u + ApplyWater(xy, uv, lod, waterCeiling, ground.terrain, ground.z, ground.water, ground.amount);
		return ground;
	}

	GroundSample GroundAt(float2 xy, float reach)
	{
		return GroundAt(xy, reach, 1.0e30);
	}

	// No ground between a and b is higher than this. One point read of the level whose texels are at least as
	// long as the stretch: such a texel holds the highest terrain within one texel of its centre in every direction,
	// and the whole stretch lies inside that. With a water map the water surfaces are in that level already
	// (HeightPyramid.cs.hlsl); without one the highest water near the camera stands in for them.
	float GroundMaxAlong(float2 a, float2 b)
	{
		const float2 span = abs(b - a);
		const float level = clamp(ceil(log2(max(max(span.x, span.y) / MapTexel, 2.0))), 1.0, PyramidLevels);
		const int mip = int(level) - 1;
		const int2 size = max(int2(PyramidSize) >> mip, 1);
		const int2 texel = clamp(int2(MapUv(0.5 * (a + b)) * size), 0, size - 1);
		float z = lerp(MapZ.x, MapZ.y, HeightPyramid.Load(int3(texel, mip)).y);
		if (!HasWaterMap() && (Flags & 1u))
			z = max(z, WaterTop);
		return z;
	}

	// Texels across the ground source at the level whose grid the march walks for this reach: the coarsest level
	// that GroundAt(xy, reach) blends in is twice as coarse, so its kinks all lie on this grid.
	float2 GroundCells(float reach)
	{
		const int level = int(min(floor(GroundLod(reach)), PyramidLevels));
		return level == 0 ? MapSize : float2(max(int2(PyramidSize) >> (level - 1), 1));
	}

	// The mean terrain height within about radius of a point.
	float GroundAround(float2 xy, float radius)
	{
		return TerrainZ(MapUv(xy), log2(radius / MapTexel) + 0.5);
	}

	// ---------------------------------------------------------------------------------------------------------

	float Hash(int2 cell)
	{
		return Random::pcg2d(uint2(cell)).x * (1.0 / 4294967296.0);
	}

	float ValueNoise(float2 p)
	{
		const float2 base = floor(p);
		const float2 f = p - base;
		const float2 u = f * f * (3.0 - 2.0 * f);
		const int2 cell = int2(base);
		return lerp(lerp(Hash(cell), Hash(cell + int2(1, 0)), u.x), lerp(Hash(cell + int2(0, 1)), Hash(cell + int2(1, 1)), u.x), u.y);
	}

	// Top of the layer above the ground at an absolute xy. Mounds average about 0.42 of Thickness; a cell longer
	// than a mound gets that average, so distant cells do not flicker as the camera moves.
	float LayerTop(float2 xyAbs, float cellLength)
	{
		const float2 q = xyAbs - WindTravel;
		float top = Thickness;
		if (NoiseStrength > 0.0) {
			const float2 p = q * InvNoiseScale;
			const float n = smoothstep(0.2, 0.8, 0.65 * ValueNoise(p) + 0.35 * ValueNoise(p * 2.3 + 17.3));
			const float mound = lerp(0.15 + 0.85 * n * n, 0.42, saturate(cellLength * InvNoiseScale - 0.5));
			top *= lerp(1.0, mound, NoiseStrength);
		}
		if (BankStrength > 0.0) {
			const float bank = lerp(0.45, 1.0 + TopLimit, smoothstep(0.15, 0.85, ValueNoise(q * InvBankScale + 31.7)));
			top *= max(1.0 + BankStrength * (bank - 1.0), 0.0);
		}
		return max(top, 1.0);
	}

	// ---------------------------------------------------------------------------------------------------------
	// The fog simulation near the camera (GroundFog/FlowSim.cs.hlsl): a square window of the world's xy plane,
	// fixed to a world grid, in which the fog is carried about by wind, slopes and whatever moves through it, and
	// settles back to its resting amount. Everything in it is a function of world position and time only.
	//   FogSim    xy: velocity of the fog, game units per second. z: amount of fog (the density multiplier the pool
	//             is drawn with). w: the resting amount there, which z settles to.
	//   FogFloor  the level of the floor at each texel, absolute z, gathered from the up-facing surfaces the screen
	//             has shown there; FloorUnknown or more where nothing has been seen yet.
	//   FogSlope  the slope of that floor over about a meter. z: 1 where it is known, xy: rise over run times z (so a
	//             filtered read divides xy by z).
	// The pool reads z directly. The sheet and the tall near volume, which are placed by the same resting amount,
	// take z / w: how far the fog there is from rest.
	Texture2D<float4> FogSim : register(t8);
	Texture2D<float> FogFloor : register(t10);
	Texture2D<float4> FogSlope : register(t11);

	static const float FloorUnknown = 1.0e20;
	static const float FloorEmpty = 1.0e30;

	float2 SimUv(float2 xy)
	{
		return xy * FlowWindow.z + FlowWindow.xy;
	}

	// 1 inside the window, falling to 0 over its outer eighth, where the simulation hands over to the resting amount.
	float SimInside(float2 uv)
	{
		if (FlowWindow.w <= 0.0)
			return 0.0;
		const float2 edge = saturate(min(uv, 1.0 - uv) * 8.0);
		return edge.x * edge.y;
	}

	bool InSimWindow(float2 xy)
	{
		const float2 uv = SimUv(xy);
		return FlowWindow.w > 0.0 && all(uv > 0.0) && all(uv < 1.0);
	}

	// How much fog the simulation holds at a camera-relative xy, relative to what rests there: 1 where nothing has
	// disturbed it and outside the window, less in a wake, more where it has been pushed together. One read.
	float SimDisturbance(float2 xy)
	{
		const float2 uv = SimUv(xy);
		const float inside = SimInside(uv);
		if (inside <= 0.0)
			return 1.0;
		const float4 state = FogSim.SampleLevel(LinearSampler, uv, 0);
		return lerp(1.0, clamp(state.z / max(state.w, 0.05), 0.0, 1.5), inside);
	}

	// The density of the fog at rest relative to its mean, at an absolute xy: two octaves of value noise travelling
	// with the wind at different speeds, so patches drift past and change shape. It is part of what the simulation
	// settles to, so inside the window the patches are carried and stirred like the rest of the fog. reach: the
	// size of detail the caller can resolve; the patches fade to their mean (1) as it grows past them, so distant
	// fog does not flicker.
	float DensityModifier(float2 xyAbs, float reach)
	{
		if (DriftStrength <= 0.0)
			return 1.0;
		const float fade = saturate(reach * InvDriftScale - 0.5);
		if (fade >= 1.0)
			return 1.0;
		const float2 p = (xyAbs - WindTravel) * InvDriftScale;
		const float2 p2 = (xyAbs - 1.7 * WindTravel) * InvDriftScale * 2.3 + 11.9;
		const float n = smoothstep(0.25, 0.75, 0.62 * ValueNoise(p) + 0.38 * ValueNoise(p2));
		return 1.0 + DriftStrength * 0.8 * (2.0 * n - 1.0) * (1.0 - fade);
	}

	// Value noise from two lattices turned against each other (by 31 and 68 degrees, the second 1.37 times finer),
	// so that shapes cut out of it by a threshold do not show the square grid a single lattice has.
	float TurnedNoise(float2 p)
	{
		const float2 a = float2(0.8572 * p.x - 0.5150 * p.y, 0.5150 * p.x + 0.8572 * p.y);
		const float2 b = float2(0.3746 * p.x - 0.9272 * p.y, 0.9272 * p.x + 0.3746 * p.y) * 1.37 + 19.19;
		return 0.5 * (ValueNoise(a) + ValueNoise(b));
	}

	// Fog over water, relative to its mean (about 1): thick and thin areas, patches and wisps, each carried by the
	// wind at its own speed, so a lake does not hold an even sheet. Three sizes; each fades to its mean as the
	// detail the caller can resolve (reach) grows past it, so the broad areas still show on a lake far away.
	float WaterPatches(float2 xyAbs, float reach)
	{
		if (WaterPatchiness <= 0.0)
			return 1.0;
		static const float3 InvSize = 1.0 / float3(28000.0, 4900.0, 1260.0);  // 400 m, 70 m, 18 m
		const float3 keep = (1.0 - saturate(reach * InvSize - 0.5)) * WaterPatchiness;
		float amount = 1.0;
		float2 warp = 0.0;
		if (keep.x > 0.0) {
			const float n = TurnedNoise((xyAbs - 0.6 * WindTravel) * InvSize.x + 41.3);
			amount *= 1.0 + keep.x * 0.6 * (2.0 * smoothstep(0.3, 0.7, n) - 1.0);
		}
		if (keep.y > 0.0) {
			const float n = TurnedNoise((xyAbs - WindTravel) * InvSize.y + 5.7);
			amount *= 1.0 + keep.y * 0.75 * (2.0 * smoothstep(0.36, 0.64, n) - 1.0);
			warp = float2(n, 1.0 - n) * 2.4;
		}
		if (keep.z > 0.0) {
			// Bent by the patch it lies in, so the wisps curl.
			const float n = TurnedNoise((xyAbs - 1.5 * WindTravel) * InvSize.z + warp + 23.1);
			amount *= 1.0 + keep.z * 0.55 * (2.0 * smoothstep(0.33, 0.67, n) - 1.0);
		}
		return amount;
	}

	// Slow rise and fall of the near volume's top, out of step from place to place.
	float Breathing(float2 xyAbs)
	{
		if (NearBreath <= 0.0)
			return 1.0;
		return 1.0 + NearBreath * sin(BreathPhase + 6.2831853 * ValueNoise((xyAbs - WindTravel) * InvDriftScale * 0.25 + 3.1));
	}

	// More fog in hollows, less on rises: the terrain around the point against the terrain at it.
	float BasinFactor(float2 xy, float terrain)
	{
		const float2 uv = MapUv(xy);
		const float2 du = float2(BasinRadius * MapScaleOffset.x, 0.0);
		const float2 dv = float2(0.0, BasinRadius * MapScaleOffset.y);
		const float around = 0.25 * (TerrainZ(uv + du) + TerrainZ(uv - du) + TerrainZ(uv + dv) + TerrainZ(uv - dv));
		const float hollow = clamp((around - terrain) * InvBasinDepth, -1.0, 1.0);
		return clamp(1.0 + BasinStrength * hollow, 0.0, 1.0 + BasinStrength);
	}

	// ---------------------------------------------------------------------------------------------------------
	// The resting amount: how much fog a place holds when nothing disturbs it, relative to Amount. One rule for the
	// whole body of fog: the simulation settles to it (and the pool is drawn from the simulation), and the sheet
	// and the tall near volume are placed by it directly. Its factors: whether the ground source has data there,
	// water, the time of day and whether the sun has reached the place, and how the land lies (hollows hold more).
	// The pieces are separate so the pixel march can hold its slow reads between joints; RestingAmountAt does all
	// the reads itself.

	// Hollows hold more, rises less: the terrain averaged around the point against the terrain at it.
	float Basin(float around, float terrain)
	{
		const float hollow = clamp((around - terrain) * InvBasinDepth, -1.0, 1.0);
		return clamp(1.0 + BasinStrength * hollow, 0.0, 1.0 + BasinStrength);
	}

	// Ground this far below the terrain shadow's surface has been out of the sun for long: it keeps all its fog.
	static const float ShadeDepth = 1050.0;

	// 1 where the terrain has kept the sun off a point for long, 0 where the sun reaches it. From the Terrain
	// Shadows height field for the present sun: a point is in shadow when it lies below that field, and the deeper
	// below, the longer ago the shadow's edge passed over it (in the morning the field sinks as the sun climbs).
	// So depth below the field stands for time out of the sun, with no history stored, and the edge of the mask
	// moves smoothly as the sun does. One read. Only in shaders that define GROUND_FOG_SHADE (the pixel pass and
	// the simulation, which have the terrain shadow bound).
	float TerrainShade(float2 xyAbs, float z, inout uint taps)
	{
#if defined(TERRAIN_SHADOWS) && defined(GROUND_FOG_SHADE)
		if (BurnRetention <= 0.0 || !(ViewAtmosphere::Data[0].flags.z & 256u) || !SharedData::terraOccSettings.EnableTerrainShadow)
			return 0.0;
		const float2 uv = TerrainShadows::GetTerrainShadowUV(xyAbs);
		if (any(uv < 0.0) || any(uv > 1.0))
			return 0.0;
		++taps;
		const float2 heights = TerrainShadows::GetTerrainZ(TerrainShadows::ShadowHeightTexture.SampleLevel(LinearSampler, uv, 0));
		// The field is stored lowered by SelfShadowBias; one meter of slack keeps lit slopes clear of the mask.
		const float field = 0.5 * (heights.x + heights.y) + TerrainShadows::SelfShadowBias;
		return smoothstep(70.0, 70.0 + ShadeDepth, field - z);
#else
		return 0.0;
#endif
	}

	// Amount by time of day and sun: what sunlit ground holds now, raised toward the shaded amount in shadow.
	float BurnOff(float sunlit, float shade)
	{
		return lerp(sunlit, max(sunlit, BurnShaded), BurnRetention * shade);
	}

	// The resting amount without the lie of the land (multiply by Basin). sunlit: BurnSunlitNear or BurnSunlitFar.
	// Over water: WaterPreference times as much, through the water's own drifting patches. ground.water rises from
	// 0 at the waterline over the shore depth, so the change has the shape of the shore.
	float RestingAmount(float coverage, GroundSample ground, float shade, float sunlit, float2 xyAbs, float reach)
	{
		float rest = coverage * ground.amount * BurnOff(sunlit, shade);
		if (ground.water > 0.0)
			rest *= lerp(1.0, WaterPreference * WaterPatches(xyAbs, reach), ground.water);
		return rest;
	}

	// The whole rule at a camera-relative xy, with its own reads (up to five). water: how much of the place is water.
	float RestingAmountAt(float2 xy, float reach, float sunlit, out float water)
	{
		water = 0.0;
		const float coverage = MapCoverage(MapUv(xy));
		if (coverage <= 0.0)
			return 0.0;
		const float2 xyAbs = xy + FrameBuffer::CameraPosAdjust.xy;
		const GroundSample ground = GroundAt(xy, reach);
		water = ground.water;
		uint taps = 0u;
		float rest = RestingAmount(coverage, ground, TerrainShade(xyAbs, ground.z + 70.0, taps), sunlit, xyAbs, reach);
		if (rest > 0.0 && BasinStrength > 0.0)
			rest *= Basin(GroundAround(xy, BasinRadius), ground.terrain);
		return rest;
	}

	// 1 - exp(-t), exact for small t.
	float OneMinusExp(float t)
	{
		return t < 0.25 ? t * (1.0 - t * (0.5 - t * (1.0 / 6.0 - t * (1.0 / 24.0 - t * (1.0 / 120.0))))) : 1.0 - exp(-t);
	}

	// t - 1 + exp(-t), exact for small t.
	float ExpRemainder(float t)
	{
		return t < 0.25 ? t * t * (0.5 - t * (1.0 / 6.0 - t * (1.0 / 24.0 - t * (1.0 / 120.0 - t * (1.0 / 720.0))))) : t - 1.0 + exp(-t);
	}

	Profile MakeProfile(float top, float fill)
	{
		Profile profile;
		profile.fill = fill;
		profile.top = top;
		profile.scale = top / max(Steepness, 0.02);
		profile.total = profile.fill + profile.scale * OneMinusExp(profile.top / profile.scale);
		return profile;
	}

	Profile MakeProfile(float top)
	{
		return MakeProfile(top, GroundFill);
	}

	// The near volume's profile: 1 from -fill to 0, then an exponential lowered and rescaled so that it reaches
	// exactly zero at top. No step at the top, so the layer has no visible lid from inside or from the side.
	struct SoftProfile
	{
		float fill;
		float top;
		float scale;  // e-folding height
		float cut;    // exp(-top / scale)
		float norm;   // 1 / (1 - cut)
	};

	SoftProfile MakeSoftProfile(float top, float fill, float falloff)
	{
		SoftProfile profile;
		profile.fill = fill;
		profile.top = top;
		const float f = max(falloff, 0.25);
		profile.scale = top / f;
		profile.cut = exp(-f);
		profile.norm = 1.0 / (1.0 - profile.cut);
		return profile;
	}

	float SoftDensity(SoftProfile profile, float h)
	{
		if (h < -profile.fill || h > profile.top)
			return 0.0;
		return h < 0.0 ? 1.0 : profile.norm * (exp(-h / profile.scale) - profile.cut);
	}

	// Antiderivative of the density from -fill to x.
	float SoftFirst(SoftProfile profile, float x)
	{
		const float below = clamp(x + profile.fill, 0.0, profile.fill);
		const float inside = clamp(x, 0.0, profile.top);
		return below + profile.norm * (profile.scale * OneMinusExp(inside / profile.scale) - profile.cut * inside);
	}

	// Mean relative density along a ray whose height above the ground runs straight from ha to hb.
	float SoftLineMean(SoftProfile profile, float ha, float hb)
	{
		const float low = min(ha, hb);
		const float high = max(ha, hb);
		if (high <= -profile.fill || low >= profile.top)
			return 0.0;
		if (high - low < 1.0)
			return SoftDensity(profile, 0.5 * (low + high));
		return (SoftFirst(profile, high) - SoftFirst(profile, low)) / (high - low);
	}

	// Density integrated from h up to the top.
	float SoftAbove(SoftProfile profile, float h)
	{
		const float x = clamp(h, 0.0, profile.top);
		return profile.norm * (profile.scale * (exp(-x / profile.scale) - profile.cut) - profile.cut * (profile.top - x));
	}

	// Relative density at height h above the ground: 1 from -fill to 0, exp(-h / scale) up to top, 0 above. Space
	// deeper than fill is not part of the cell at all: it is hidden by the ground, so it must not dilute the mean.
	// Returns the first and second antiderivatives from -fill to x of the density (xy) and of the counted space (zw).
	float4 Antiderivatives(Profile profile, float x)
	{
		const float counted = max(x + profile.fill, 0.0);
		const float below = min(counted, profile.fill);
		const float inside = clamp(x, 0.0, profile.top);
		const float above = max(x - profile.top, 0.0);
		const float t = inside / profile.scale;
		return float4(below + profile.scale * OneMinusExp(t),
			0.5 * below * below + profile.fill * inside + profile.scale * profile.scale * ExpRemainder(t) + above * profile.total,
			counted, 0.5 * counted * counted);
	}

	// Mean relative density along a thin ray whose height above the ground runs straight from ha to hb.
	float LineMean(Profile profile, float ha, float hb)
	{
		const float low = min(ha, hb);
		const float high = max(ha, hb);
		if (high <= -profile.fill || low >= profile.top)
			return 0.0;
		if (high - low < 1.0) {
			const float mid = 0.5 * (low + high);
			return mid < -profile.fill || mid > profile.top ? 0.0 : (mid < 0.0 ? 1.0 : exp(-mid / profile.scale));
		}
		return (Antiderivatives(profile, high).x - Antiderivatives(profile, low).x) / (high - low);
	}

	// For a ray segment whose height above the ground runs from ha to hb, widened to the cell's vertical half size r:
	// x = density integrated over the heights it covers, y = how much of them counts, both as shares of the whole.
	// x / y is the mean density. Exact for ground that is straight along the segment.
	float2 SegmentIntegral(Profile profile, float ha, float hb, float r)
	{
		if (max(ha, hb) + r <= -profile.fill)
			return 0.0;
		if (min(ha, hb) - r >= profile.top)
			return float2(0.0, 1.0);
		const float rise = hb - ha;
		// The general form divides by r * rise; below these sizes the missing dimension changes nothing.
		if (abs(rise) < 2.0) {
			const float mid = 0.5 * (ha + hb);
			if (r < 0.5)
				return mid < -profile.fill ? 0.0 : float2(mid > profile.top ? 0.0 : (mid < 0.0 ? 1.0 : exp(-mid / profile.scale)), 1.0);
			return (Antiderivatives(profile, mid + r).xz - Antiderivatives(profile, mid - r).xz) / (2.0 * r);
		}
		if (r < 2.0)
			return (Antiderivatives(profile, hb).xz - Antiderivatives(profile, ha).xz) / rise;
		return (Antiderivatives(profile, hb + r).yw - Antiderivatives(profile, hb - r).yw - Antiderivatives(profile, ha + r).yw + Antiderivatives(profile, ha - r).yw) / (2.0 * r * rise);
	}

	float3 Hue(float h)
	{
		return saturate(abs(frac(h + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
	}

	// Sun transmittance through the fog above a point.
	float SelfShadowing(float tauAbove, float lightZ)
	{
		return exp(-SelfShadow * tauAbove / max(lightZ, 0.15));
	}

	// Mean incident sky radiance, clouds included.
	float3 SkyMean()
	{
		return max(float3(PhysSky::TexCloudDiffuseTransport.Load(int3(0, 0, 0)).x, PhysSky::TexCloudDiffuseTransport.Load(int3(1, 0, 0)).x,
					   PhysSky::TexCloudDiffuseTransport.Load(int3(2, 0, 0)).x) *
					   0.28209479177387814,
			0.0);
	}

	// jittered is the pass's own sample of this cell at depth fraction depthOffset; lighting stays near it.
	bool EvaluateCell(ViewAtmosphere::Grid grid, uint3 coord, uint level, float3 jittered, float depthOffset, out Cell cell)
	{
		cell.extinction = 0.0;
		cell.lightPos = jittered;
		cell.tauAbove = 0.0;
		cell.albedo = Albedo;

		const float nearDistance = ViewAtmosphere::Distance(grid, coord.z);
		const float farDistance = ViewAtmosphere::Distance(grid, coord.z + 1u);
		if (nearDistance >= FadeEnd)
			return false;
		const float distanceFade = 1.0 - smoothstep(FadeStart, FadeEnd, 0.5 * (nearDistance + farDistance));
		if (distanceFade <= 0.0)
			return false;

		// The far face of the cell; the near face is the same four rays scaled by nearScale.
		const float3 farPoint = ViewAtmosphere::Position(grid, coord, float3(0.5, 0.5, 1.0));
		const float4 cornerZ = float4(ViewAtmosphere::Position(grid, coord, float3(0.0, 0.0, 1.0)).z, ViewAtmosphere::Position(grid, coord, float3(1.0, 0.0, 1.0)).z,
			ViewAtmosphere::Position(grid, coord, float3(0.0, 1.0, 1.0)).z, ViewAtmosphere::Position(grid, coord, float3(1.0, 1.0, 1.0)).z);
		const float exitLow = min(min(cornerZ.x, cornerZ.y), min(cornerZ.z, cornerZ.w));
		const float exitHigh = max(max(cornerZ.x, cornerZ.y), max(cornerZ.z, cornerZ.w));
		const float nearScale = nearDistance / farDistance;
		const float exitMid = 0.5 * (exitLow + exitHigh);
		const float exitHalf = 0.5 * (exitHigh - exitLow);

		const float midScale = 0.5 * (1.0 + nearScale);
		const float2 midXY = farPoint.xy * midScale;
		const float coverage = MapCoverage(MapUv(midXY));
		if (coverage <= 0.0)
			return false;
		const float camZ = FrameBuffer::CameraPosAdjust.z;
		float midWater;
		const float midGround = Ground(midXY, midWater);

		// Reject cells clear of the layer. The ground under the rest of the cell is not sampled yet, so allow for a
		// 45 degree slope over half its length.
		const float topMax = Thickness * (1.0 + TopLimit * BankStrength);
		const float slack = 0.5 * (1.0 - nearScale) * length(farPoint.xy);
		const float cellLow = min(exitLow, exitLow * nearScale) + camZ - midGround;
		const float cellHigh = max(exitHigh, exitHigh * nearScale) + camZ - midGround;
		if (cellLow - slack >= topMax || cellHigh + slack <= -GroundFill)
			return false;

		const float cellLength = farDistance - nearDistance;
		const float top = LayerTop(midXY + FrameBuffer::CameraPosAdjust.xy, cellLength);
		const Profile profile = MakeProfile(top);

		// The ray is cut where the ground is sampled, so long cells follow terrain that is not straight.
		const uint halfSteps = level == 0u ? 1u : (cellLength > 1024.0 ? 2u : 1u);
		float water;
		const float nearGround = Ground(farPoint.xy * nearScale, water);
		const float farGround = Ground(farPoint.xy, water);
		float2 sum = 0.0;
		float scaleA = nearScale;
		float groundA = nearGround;
		[loop] for (uint i = 1u; i <= 2u * halfSteps; ++i)
		{
			const float scaleB = lerp(nearScale, 1.0, float(i) / float(2u * halfSteps));
			float groundB = farGround;
			if (i == halfSteps)
				groundB = midGround;
			else if (i < 2u * halfSteps)
				groundB = Ground(farPoint.xy * scaleB, water);
			sum += SegmentIntegral(profile, scaleA * exitMid + camZ - groundA, scaleB * exitMid + camZ - groundB, 0.5 * (scaleA + scaleB) * exitHalf);
			scaleA = scaleB;
			groundA = groundB;
		}
		if (sum.x <= 0.0 || sum.y <= 1e-4)
			return false;
		const float mean = min(sum.x / sum.y, 1.0);

		float placement = coverage * distanceFade * lerp(1.0, WaterPreference * WaterPatches(midXY + FrameBuffer::CameraPosAdjust.xy, cellLength), midWater);
		float basin = 1.0;
		if (BasinStrength > 0.0) {
			basin = BasinFactor(midXY, TerrainZ(MapUv(midXY)));
			placement *= basin;
		}
		const float groundExtinction = GroundExtinction * placement;
		cell.extinction = groundExtinction * mean;
		if (cell.extinction <= 0.0)
			return false;

		// Light the fog where it is: at the sample's place along the cell, but inside the layer even when the
		// sample itself is under the ground or above the top.
		const float lightGround = depthOffset < 0.5 ? lerp(nearGround, midGround, depthOffset * 2.0) : lerp(midGround, farGround, depthOffset * 2.0 - 1.0);
		const float lightHeight = clamp(jittered.z + camZ - lightGround, 0.15 * top, top);
		cell.lightPos = float3(jittered.xy, lightGround + lightHeight - camZ);
		cell.tauAbove = groundExtinction * profile.scale * (exp(-lightHeight / profile.scale) - exp(-top / profile.scale));

		if (DebugMode == 2u)
			cell.albedo = Hue(midGround / 1024.0) * (frac(midGround / 128.0) < 0.5 ? 1.0 : 0.3);
		else if (DebugMode == 3u)
			cell.albedo = 0.08 + 0.92 * float3(saturate(basin - 1.0), saturate(top / max(topMax, 1.0)), midWater);
		return true;
	}
}

#endif
