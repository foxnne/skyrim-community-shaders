// Ground Fog, drawn per screen pixel.
//
// For every pixel the fog's optical depth is integrated along the view ray from the camera to the scene depth (to
// the end of the fog's range for sky). Nothing here is laid out in view space:
// - The ground is a surface in the world: the terrain height map, read at a level of detail that coarsens smoothly
//   with distance from the camera.
// - The ray is cut where it crosses the texel grid of that level, which is fixed in the world. Between two such
//   joints the ground is straight, and the height profile is integrated over the segment in closed form. So the
//   result is the exact integral over that surface however long the segments are, and a layer far thinner than a
//   segment keeps its mass and its shape. There are no depth slices to band or ladder as the camera moves.
// - Nothing is accumulated over frames: the result is a function of the pixel's ray, its depth and the world.
//
// The pool, the knee-high fog around the camera, is not part of the march: a layer that thin cannot take its ground
// from a height map. It is integrated in closed form over the end of the ray, on what the pixel shows, from the
// simulation (GroundFog/Pool.hlsli, GroundFog/FlowSim.cs.hlsl), and added behind what the march found.
//
// Three layers share the march:
// - the near volume (only when the near layer is set thicker than a pool): a few meters tall, soft-topped, within
//   NearEnd of the camera. A level ray at eye height runs through it. Near the camera the joints are one height
//   map texel (under 2 m) apart, and each segment is integrated in closed form.
// - the sheet: thin, on the ground, out to FadeEnd.
// - the valley layer: deeper, in hollows and over water, from where the sheet fades out to kilometers away. It lies
//   on level low ground only: far ground is read coarsely, so it fades out on slopes and where the coarse ground
//   is off by more than the layer is thick, and where the ray ends on a surface it is anchored to that surface.
//
// Water: the ground is max(terrain, water surface), the water from the baked water map (or, without one, the
// engine's data for the cells around the camera). Fog over water comes in with the water's depth at the shore and
// is broken up by its own drifting patches.
//
// What the pixel shows is ground only if it faces up (normal rebuilt from the full-resolution depth around the
// pixel). Then the fog's ground meets it over the end of the ray. Walls, trunks, rock sides and undersides never
// move the ground: they stand in the fog, which is a function of height above the ground beside them, and the fog
// fades over the last SoftLength of the ray in front of them.
//
// Output, for ViewAtmosphere/Composite.cs.hlsl to lay over the scene before the atmosphere:
//   FogOut    rgb: light scattered toward the camera per unit of opacity, a: transmittance to the far depth
//   FogAuxOut x, y: the nearest and farthest view depth of the scene pixels this output pixel stands for (/ 64),
//             z: transmittance to the nearest depth
// At half resolution every output pixel carries both ends of the depth range of its 2x2 scene pixels, from one
// march, so the upsample can give each full-resolution pixel the fog for its own depth.

#define COMPUTESHADER
#define PS_LINEAR_SHADOW_SAMPLER
// The fog lies on the ground, where a shadow's edge is seen at full size: the cascades are read filtered, and inside
// the simulation's window from its sun map (GroundFog::FogSlope.w), which is steady in the world and in time.
#define MEDIUM_SOFT_CASCADE
#define MEDIUM_EXTERNAL_CASCADE
#include "Common/Random.hlsli"
#include "Common/ViewAtmosphere.hlsli"
#include "PhysicalSky/Common.hlsli"
#include "ViewAtmosphere/MediumLighting.hlsli"

#ifdef SKYLIGHTING
#	define SKYLIGHTING_PROBE_REGISTER t50
#	include "Skylighting/Skylighting.hlsli"
#endif

#define GROUND_FOG_SHADE
#include "GroundFog/GroundFog.hlsli"
#include "GroundFog/Pool.hlsli"

Texture2D<float> SceneDepth : register(t40);
RWTexture2D<float4> FogOut : register(u0);
RWTexture2D<float4> FogAuxOut : register(u1);
// What this frame shows of the floor, for the simulation's floor map: per texel of its window the lowest absolute z
// of the up-facing surfaces seen there. Cleared to FloorEmpty before the pass.
RWTexture2D<float> FloorSplat : register(u2);

namespace GroundFog
{
	// Distances from the player over which the floor offset under the player's feet stops applying, game units.
	static const float FloorNear = 1400.0;
	static const float FloorFar = 3150.0;
	// A level surface above the mapped ground is a floor if it continues this far around the pixel, game units.
	static const float FloorSupportRadius = 84.0;

	// One layer at a point of the ray.
	struct Layer
	{
		bool inside;  // close enough to the ground to hold fog
		float top;    // layer top above the ground
		float place;  // density multiplier
	};

	// A point of the view ray where the ground is read.
	struct Joint
	{
		float t;       // distance from the camera
		float ground;  // absolute z of the ground under it
		float h;       // height above that ground
		float hn;      // height above the near volume's ground, which also follows floors
		float hv;      // height above the valley layer's ground, which meets a far surface at the end of the ray
		float shade;   // how much the terrain keeps the sun off this place
		Layer sheet;
		Layer valley;
		Layer near;
	};

	// What the pixel sees of the fog, for lighting: sums weighted by each segment's visible share.
	struct Seen
	{
		float tau;
		float share;   // 1 - transmittance
		float t;       // distance along the ray
		float z;       // absolute height of a point inside the fog
		float above;   // optical depth of the fog above that point
		float valley;  // part of share that is the valley layer
		float shade;   // share-weighted terrain shade, for the Layer Mask view
	};

	// Placement terms that change over tens of meters, not from joint to joint: read again only after the ray has
	// gone on by SlowStep or 15 % of the distance, and held in between. The segment integrals average them anyway.
	struct Slow
	{
		float tShade;  // where the terrain shade was last read; negative: not yet
		float shade;
		float tAround;  // the same for the terrain averaged around the point
		float around;
		float tWater;  // the same for the highest water surface around the point
		float waterCeiling;
		// The lie of the land under the valley layer (x: rise over run, y: how far the coarse ground the march
		// reads may be off the land, game units), read at distances LieBase * LieStep^k along the ray and
		// interpolated between the two around a joint: lieNear at k = lieIndex, lieFar at k + 1.
		int lieIndex;
		float2 lieNear;
		float2 lieFar;
	};
	static const float LieBase = 4096.0;
	static const float LieStep = 1.5;
	static const float SlowStep = 420.0;

	bool Stale(float last, float t)
	{
		return last < 0.0 || t - last > max(SlowStep, 0.15 * t);
	}

	// The pixel's ray and what its end says about the ground.
	struct Ray
	{
		float3 dir;
		float camZ;
		float tEnd;
		// Sheet: the ground under the last blendLength of the ray moves by surfaceOffset, fully at the end.
		float surfaceOffset;
		float tBlend;
		float blendLength;
		// Near volume: its ground is the mapped ground plus the player's floor offset near the player, and meets
		// floorEnd over the floorLength of the ray before tFloorEnd.
		float floorEnd;
		float tFloor;
		float tFloorEnd;
		float floorLength;
		// Valley layer: its ground moves by valleyOffset over the valleyLength of the ray before its end, where
		// the pixel shows the real ground and the coarse far ground does not matter any more.
		float valleyOffset;
		float tValley;
		float valleyLength;
	};

	float TopMax()
	{
		return Thickness * (1.0 + TopLimit * BankStrength);
	}

	float ValleyTopMax()
	{
		return 1.25 * FarThickness;
	}

	float ValleyFill()
	{
		// Far ground is read coarsely and lies above the real valley floor, so the layer reaches well below it.
		return FarThickness;
	}

	float NearTopMax()
	{
		return NearThickness * (1.0 + NearBreath);
	}

	float NearFill()
	{
		return GroundFill;
	}

	// How much a visible surface counts as the ground, by its height above the mapped ground. Rocks, roads, steps
	// and ground meshes do; roofs, bridges and tree crowns do not. The caller multiplies by how much it faces up.
	float SurfaceWeight(float above)
	{
		if (SurfaceBlend <= 0.0 || SurfaceLift <= 0.0)
			return 0.0;
		return (1.0 - smoothstep(SurfaceLift, 2.0 * SurfaceLift, above)) * (1.0 - smoothstep(2.0 * SurfaceLift, 4.0 * SurfaceLift, -above));
	}

	// The same for the near volume, which also takes level floors well above the map: up to FloorMax.
	float FloorWeight(float above)
	{
		if (SurfaceBlend <= 0.0)
			return 0.0;
		const float top = max(FloorMax, SurfaceLift);
		return (1.0 - smoothstep(top, 1.5 * top, above)) * (1.0 - smoothstep(2.0 * SurfaceLift, 4.0 * SurfaceLift, -above));
	}

	// Share of the player's floor offset that applies at a camera-relative xy.
	float FloorReach(float2 xy)
	{
		return FloorOffset > 0.0 ? 1.0 - smoothstep(FloorNear, FloorFar, length(xy - FloorAnchor)) : 0.0;
	}

	// How far the near volume's ground lies above the fog's mapped ground at a point of the ray.
	float FloorAt(Ray ray, float t)
	{
		const float base = FloorOffset * FloorReach(ray.dir.xy * t);
		return lerp(base, ray.floorEnd, saturate((t - ray.tFloor) / max(ray.floorLength, 1e-3)));
	}

	float SheetLiftAt(Ray ray, float t)
	{
		return ray.surfaceOffset * saturate((t - ray.tBlend) / max(ray.blendLength, 1e-3));
	}

	// How much of the way to the visible surface the valley layer's ground has gone at a point of the ray.
	float ValleyAnchorAt(Ray ray, float t)
	{
		return ray.valleyLength > 0.0 ? saturate((t - ray.tValley) / ray.valleyLength) : 0.0;
	}

	// The lie of the land at a distance along the ray: the terrain as the march reads it there, and three reads one
	// level coarser a reach apart around the point. x: rise over run of the plane through the three. y: how far
	// the march's ground lies from their mean, which is what the coarse read can be off by where the land bends (a
	// slope foot, a gully, a ridge). Four reads.
	float2 LandLie(Ray ray, float t, inout uint taps)
	{
		const float reach = max(t * StepGrowth, StepMin);
		const float2 uv = MapUv(ray.dir.xy * t);
		const float lod = GroundLod(reach);
		const float2 du = float2(reach * MapScaleOffset.x, 0.0);
		const float2 dv = float2(0.0, reach * MapScaleOffset.y);
		const float a = TerrainZ(uv + du, lod + 1.0);
		const float b = TerrainZ(uv - 0.5 * du + 0.8660254 * dv, lod + 1.0);
		const float c = TerrainZ(uv - 0.5 * du - 0.8660254 * dv, lod + 1.0);
		taps += 4u;
		const float2 gradient = float2((2.0 * a - b - c) / 3.0, (b - c) / 1.7320508) / reach;
		return float2(length(gradient), abs(TerrainZ(uv, lod) - (a + b + c) / 3.0));
	}

	// The same at a joint, from the reads at the two fixed distances around it. The distances are the same for
	// every ray, so the result changes smoothly from pixel to pixel and along the ray; a new read is made only
	// when the ray passes into the next interval (one in every LieStep times the distance).
	float2 LandLieAt(Ray ray, float t, inout Slow slow, inout uint taps)
	{
		const int index = int(floor(log2(max(t, LieBase) / LieBase) / log2(LieStep)));
		const float tNear = LieBase * pow(LieStep, float(index));
		if (index != slow.lieIndex) {
			if (index == slow.lieIndex + 1)
				slow.lieNear = slow.lieFar;
			else
				slow.lieNear = LandLie(ray, tNear, taps);
			slow.lieFar = LandLie(ray, tNear * LieStep, taps);
			slow.lieIndex = index;
		}
		return lerp(slow.lieNear, slow.lieFar, saturate((t - tNear) / (tNear * (LieStep - 1.0))));
	}

	// No fog lies higher than this above the mapped ground anywhere between two distances along the ray.
	float SlabTop(float from, float to, float lift, float floorLift, float valleyLift)
	{
		float top = -1.0e30;
		if (from < FadeEnd)
			top = TopMax() + lift;
		if (NearExtinction > 0.0 && from < NearEnd)
			top = max(top, NearTopMax() + floorLift);
		if (FarExtinction > 0.0 && to > FadeStart)
			top = max(top, ValleyTopMax() + valleyLift);
		return top;
	}

	// Fills in what a joint needs to hold fog. Joints clear of all layers cost nothing more than their ground read.
	void Shade(inout Joint joint, Ray ray, float2 xy, GroundSample ground, float reach, inout Slow slow, inout uint taps)
	{
		const float2 xyAbs = xy + FrameBuffer::CameraPosAdjust.xy;
		// Where the pool lies the sheet does not: it is laid on the height map, which near the camera puts it on
		// walls and banks. It comes in exactly as the pool goes out.
		const float sheetFade = (1.0 - smoothstep(FadeStart, FadeEnd, joint.t)) * (PoolExtinction > 0.0 ? smoothstep(0.5 * PoolEnd, PoolEnd, joint.t) : 1.0);
		// The valley layer comes in exactly as the sheet goes out, so there is no ring where the sheet ends.
		// (By distance alone: where the sheet makes way for the pool the valley layer has no business.)
		const float valleyFade = FarExtinction > 0.0 ? smoothstep(FadeStart, FadeEnd, joint.t) * (1.0 - smoothstep(0.75 * FarEnd, FarEnd, joint.t)) : 0.0;
		const float nearFade = NearExtinction > 0.0 ? 1.0 - smoothstep(0.5 * NearEnd, NearEnd, joint.t) : 0.0;
		joint.shade = 0.0;
		joint.sheet.inside = sheetFade > 0.0 && joint.h < TopMax() && joint.h > -GroundFill - 64.0;
		joint.sheet.top = Thickness;
		joint.sheet.place = 0.0;
		joint.valley.inside = valleyFade > 0.0 && joint.hv < ValleyTopMax() && joint.hv > -ValleyFill() - 64.0;
		joint.valley.top = FarThickness;
		joint.valley.place = 0.0;
		joint.near.inside = nearFade > 0.0 && joint.hn < NearTopMax() && joint.hn > -NearFill() - 64.0;
		joint.near.top = NearThickness;
		joint.near.place = 0.0;
		if (!joint.sheet.inside && !joint.valley.inside && !joint.near.inside)
			return;
		const float coverage = MapCoverage(MapUv(xy));
		if (BurnRetention > 0.0 && Stale(slow.tShade, joint.t)) {
			slow.shade = TerrainShade(xyAbs, ground.z + 70.0, taps);
			slow.tShade = joint.t;
		}
		joint.shade = slow.shade;
		if (joint.sheet.inside || joint.near.inside) {
			// Shared by the sheet and the near volume: they are one body of fog, thin far away and tall close by,
			// placed by the resting amount (the same pieces as RestingAmountAt, with the slow reads held).
			float place = RestingAmount(coverage, ground, joint.shade, BurnSunlitNear, xyAbs, reach);
			if (place > 0.0 && BasinStrength > 0.0) {
				if (Stale(slow.tAround, joint.t)) {
					slow.around = GroundAround(xy, BasinRadius);
					slow.tAround = joint.t;
					++taps;
				}
				place *= Basin(slow.around, ground.terrain);
			}
			place *= DensityModifier(xyAbs, reach);
			// Inside the simulation's window: by how far the fog there is from rest (a wake, a pile-up).
			if (place > 0.0 && InSimWindow(xy)) {
				place *= SimDisturbance(xy);
				++taps;
			}
			joint.sheet.place = place * sheetFade;
			joint.near.place = place * nearFade;
			if (joint.sheet.inside)
				joint.sheet.top = LayerTop(xyAbs, reach);
			if (joint.near.inside)
				joint.near.top = max(NearThickness * Breathing(xyAbs), 1.0);
		}
		if (joint.valley.inside) {
			// Only where the land lies lower than the country around it, or on water: pools in basins and on
			// lakes, nothing on rises and open slopes.
			const float hollow = saturate((GroundAround(xy, FarBasinRadius) - ground.terrain) * InvFarBasinDepth);
			++taps;
			const float pool = smoothstep(0.0, 1.0, max(hollow, ground.water));
			// Broad banks, travelling with the wind like the sheet's.
			const float bank = lerp(0.7, 1.25, ValueNoise((xyAbs - WindTravel) * InvBankScale * 0.25 + 7.3));
			joint.valley.top = max(FarThickness * (0.35 + 0.65 * pool) * bank, 1.0);
			float place = coverage * ground.amount * valleyFade * pool * BurnOff(BurnSunlitFar, joint.shade);
			if (place > 0.0) {
				// It pools on level low ground and does not climb hillsides. And the ground it is laid on is
				// read coarsely this far out: where that ground may be off the land by more than the layer is
				// thick, the layer would hang over the land or sink into it in bands, so it thins out there,
				// except near the end of a ray, where its ground is the surface the pixel shows. Water is
				// level and exactly known.
				if (ground.water < 1.0) {
					const float2 lie = LandLieAt(ray, joint.t, slow, taps);
					const float level = 1.0 - smoothstep(0.12, 0.35, lie.x);
					const float sure = 1.0 - smoothstep(0.5, 1.5, lie.y / joint.valley.top);
					place *= lerp(level * lerp(sure, 1.0, ValleyAnchorAt(ray, joint.t)), 1.0, ground.water);
				}
				if (ground.water > 0.0)
					place *= lerp(1.0, min(WaterPreference, 1.0) * WaterPatches(xyAbs, reach), ground.water);
			}
			joint.valley.place = place;
		}
	}

	// Reads the ground under a point of the ray and fills in the joint.
	Joint MakeJoint(Ray ray, float t, float reach, inout Slow slow, inout uint taps)
	{
		const float2 xy = ray.dir.xy * t;
		// Joints whose land is above every water surface around them skip the water read. The bound is read from a
		// coarse level of the water map and held until the ray has gone on by 30 % of the distance (12 m near the
		// camera); it is asked for half as far again as it is held.
		if (HasWaterMap() && (slow.tWater < 0.0 || t - slow.tWater > max(2.0 * SlowStep, 0.3 * t))) {
			slow.waterCeiling = WaterCeiling(MapUv(xy), max(3.0 * SlowStep, 0.65 * t));
			slow.tWater = t;
			++taps;
		}
		const GroundSample ground = GroundAt(xy, reach, slow.waterCeiling);
		taps += ground.reads;
		Joint joint;
		joint.t = t;
		joint.ground = ground.z + SheetLiftAt(ray, t);
		const float z = ray.camZ + ray.dir.z * t;
		joint.h = z - joint.ground;
		joint.hn = z - ground.z - FloorAt(ray, t);
		joint.hv = z - ground.z - ray.valleyOffset * ValleyAnchorAt(ray, t);
		Shade(joint, ray, xy, ground, reach, slow, taps);
		return joint;
	}

	// Adds a stretch of fog of optical depth tau to what the pixel sees. height: where inside the layer it is lit,
	// above the layer's ground at absolute z ground. above: optical depth of the layer over that height.
	void Accumulate(inout Seen seen, Joint a, Joint b, float tau, float ground, float height, float above, bool valley)
	{
		const float share = exp(-seen.tau) * OneMinusExp(tau);
		seen.share += share;
		seen.t += share * 0.5 * (a.t + b.t);
		seen.z += share * (ground + height);
		seen.above += share * above;
		seen.valley += valley ? share : 0.0;
		seen.shade += share * 0.5 * (a.shade + b.shade);
		seen.tau += tau;
	}

	// Adds the sheet's or the valley layer's share of the segment between two joints.
	void Integrate(inout Seen seen, Joint a, Joint b, Layer la, Layer lb, float ha, float hb, float extinction, float fill, bool valley)
	{
		if (!la.inside && !lb.inside)
			return;
		const bool both = la.inside && lb.inside;
		const float top = both ? 0.5 * (la.top + lb.top) : (la.inside ? la.top : lb.top);
		float place = both ? 0.5 * (la.place + lb.place) : (la.inside ? la.place : lb.place);
		if (valley && both) {
			// The valley layer's placement can differ a lot between two joints (it is anchored at the end of the
			// ray). A joint that has only just come within reach of the layer must not count for half at once, or
			// the fog would step from one pixel to the next: each joint counts by how far inside the reach it is.
			const float2 h = float2(ha, hb);
			const float2 weight = max(saturate((ValleyTopMax() - h) / max(ValleyTopMax() - top, 0.05 * ValleyTopMax())) * saturate((h + fill + 64.0) / 64.0), 1e-4);
			place = (la.place * weight.x + lb.place * weight.y) / (weight.x + weight.y);
		}
		const Profile profile = MakeProfile(top, fill);
		const float tau = extinction * place * LineMean(profile, ha, hb) * (b.t - a.t);
		if (tau <= 0.0)
			return;
		// Lit at the middle of the segment, but inside the layer even where the ray itself is under the ground
		// or above the top.
		const float height = clamp(0.5 * (ha + hb), 0.15 * top, top);
		Accumulate(seen, a, b, tau, 0.5 * (a.ground + a.h - ha + b.ground + b.h - hb), height,
			extinction * place * profile.scale * (exp(-height / profile.scale) - exp(-top / profile.scale)), valley);
	}

	// The near volume's share of the segment. scale: the fade in front of what the ray ends on.
	void IntegrateNear(inout Seen seen, Joint a, Joint b, float scale)
	{
		if (!a.near.inside && !b.near.inside)
			return;
		const bool both = a.near.inside && b.near.inside;
		const float top = both ? 0.5 * (a.near.top + b.near.top) : (a.near.inside ? a.near.top : b.near.top);
		const float place = scale * (both ? 0.5 * (a.near.place + b.near.place) : (a.near.inside ? a.near.place : b.near.place));
		const SoftProfile profile = MakeSoftProfile(top, NearFill(), NearFalloff);
		const float tau = NearExtinction * place * SoftLineMean(profile, a.hn, b.hn) * (b.t - a.t);
		if (tau <= 0.0)
			return;
		const float height = clamp(0.5 * (a.hn + b.hn), 0.15 * top, top);
		const float ground = 0.5 * (a.ground + a.h - a.hn + b.ground + b.h - b.hn);
		Accumulate(seen, a, b, tau, ground, height, NearExtinction * place * SoftAbove(profile, height), false);
	}

	float3 WorldPosition(uint2 scenePixel, float depth, uint2 mainSize)
	{
		// The same reconstruction as the atmosphere composite: the jittered projection the depth was drawn with.
		const float2 uv = FrameBuffer::GetDynamicResolutionUnadjustedScreenPosition((float2(scenePixel) + 0.5) / float2(mainSize));
		const float4 position = mul(FrameBuffer::CameraViewProjInverse, float4(uv * float2(2, -2) + float2(-1, 1), depth, 1));
		return position.xyz / position.w;
	}

	// The surface normal at a scene pixel from the full-resolution depth around it: per axis the neighbour nearer
	// in depth, so the result does not bend around outlines. Four loads. The G-buffer normal is not used: this
	// pass runs after water, whose surface is in the depth but not in the G-buffer.
	float3 SurfaceNormal(uint2 scenePixel, float depth, float3 position, uint2 mainSize, uint2 activeMain)
	{
		const int2 p = int2(scenePixel);
		const int2 last = int2(activeMain) - 1;
		const int2 left = max(p - int2(1, 0), 0);
		const int2 right = min(p + int2(1, 0), last);
		const int2 up = max(p - int2(0, 1), 0);
		const int2 down = min(p + int2(0, 1), last);
		const float4 depths = float4(SceneDepth[left], SceneDepth[right], SceneDepth[up], SceneDepth[down]);
		const bool useLeft = right.x == p.x || (left.x != p.x && abs(depths.x - depth) < abs(depths.y - depth));
		const bool useUp = down.y == p.y || (up.y != p.y && abs(depths.z - depth) < abs(depths.w - depth));
		float3 dx, dy;
		if (useLeft)
			dx = position - WorldPosition(left, depths.x, mainSize);
		else
			dx = WorldPosition(right, depths.y, mainSize) - position;
		if (useUp)
			dy = position - WorldPosition(up, depths.z, mainSize);
		else
			dy = WorldPosition(down, depths.w, mainSize) - position;
		float3 normal = cross(dx, dy);
		const float size = length(normal);
		if (size < 1e-12)
			return float3(0.0, 0.0, 0.0);
		normal /= size;
		return dot(normal, position) > 0.0 ? -normal : normal;
	}

	// Whether the level surface at a pixel goes on around it: four points on its plane, FloorSupportRadius away,
	// looked up in the depth buffer. A point that shows something off the plane and not in front of it means the
	// surface ends there: the top of a rock, a barrel, a fence or a wall, not a floor. Something standing in front
	// of the point says nothing either way. Four loads.
	float FloorSupport(float3 position, float3 normal, uint2 mainSize, uint2 activeMain, inout uint taps)
	{
		const float3 t1 = normalize(cross(normal, abs(normal.x) < 0.9 ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0)));
		const float3 t2 = cross(normal, t1);
		float support = 1.0;
		[unroll] for (uint i = 0u; i < 4u; ++i)
		{
			const float3 expected = position + ((i & 1u) ? t1 : t2) * ((i & 2u) ? FloorSupportRadius : -FloorSupportRadius);
			const float4 clip = mul(FrameBuffer::CameraViewProj, float4(expected, 1.0));
			const float2 uv = clip.xy / max(clip.w, 1e-3) * float2(0.5, -0.5) + 0.5;
			const bool onScreen = clip.w > 1.0 && all(uv >= 0.0) && all(uv < 1.0);
			const uint2 tapPixel = min(uint2(saturate(uv) * FrameBuffer::DynamicResolutionParams1.xy * float2(mainSize)), activeMain - 1u);
			const float tapDepth = SceneDepth[tapPixel];
			++taps;
			const float3 seenAt = WorldPosition(tapPixel, tapDepth, mainSize);
			const bool inFront = length(seenAt) < length(expected) - 40.0;
			if (onScreen && !inFront)
				support = min(support, 1.0 - smoothstep(18.0, 40.0, abs(dot(normal, seenAt - position))));
		}
		return support;
	}

	float3 TapColor(uint taps)
	{
		// Blue: up to 4 reads, green: 34, yellow: 64, red: 94 and more.
		return Hue(0.66 * (1.0 - saturate((float(taps) - 4.0) / 90.0)));
	}
}

[numthreads(8, 8, 1)] void main(uint2 pixel : SV_DispatchThreadID) {
	uint2 mainSize, outSize;
	SceneDepth.GetDimensions(mainSize.x, mainSize.y);
	FogOut.GetDimensions(outSize.x, outSize.y);
	const uint2 activeMain = uint2(clamp(float2(mainSize) * FrameBuffer::DynamicResolutionParams1.xy, 1.0, float2(mainSize)));
	const bool halfResolution = (GroundFog::Flags & 2u) != 0u;
	const uint2 activeOut = halfResolution ? (activeMain + 1u) / 2u : activeMain;
	if (any(pixel >= min(activeOut, outSize)))
		return;

	// At half resolution the output pixel stands for four scene pixels. The ray is marched to the farthest of them
	// and the fog in front of the nearest is noted on the way, so both ends of their depth range come out of one
	// march and the upsample has the fog for any depth in between. No pattern alternates between pixels or frames.
	uint taps = 1u;
	uint2 scenePixel = pixel;
	float depth = SceneDepth[pixel];
	float nearDepth = depth;
	if (halfResolution) {
		scenePixel = min(pixel * 2u, activeMain - 1u);
		depth = SceneDepth[scenePixel];
		nearDepth = depth;
		[unroll] for (uint i = 1u; i < 4u; ++i)
		{
			const uint2 candidate = min(pixel * 2u + uint2(i & 1u, i >> 1u), activeMain - 1u);
			const float candidateDepth = SceneDepth[candidate];
			if (FrameBuffer::ToStandardDepth(candidateDepth) > FrameBuffer::ToStandardDepth(depth)) {
				depth = candidateDepth;
				scenePixel = candidate;
			}
			if (FrameBuffer::ToStandardDepth(candidateDepth) < FrameBuffer::ToStandardDepth(nearDepth))
				nearDepth = candidateDepth;
		}
		taps = 4u;
	}
	const float viewFar = SharedData::GetScreenDepth(depth);
	const float viewNear = min(SharedData::GetScreenDepth(nearDepth), viewFar);
	const float2 auxDepths = min(float2(viewNear, viewFar) / 64.0, 65000.0);

	const float3 position = GroundFog::WorldPosition(scenePixel, depth, mainSize);
	const bool sky = FrameBuffer::ToStandardDepth(depth) >= 1.0 - 1e-6;
	const float sceneDistance = length(position);
	const float3 dir = position / max(sceneDistance, 1e-6);
	const float range = GroundFog::FarExtinction > 0.0 ? max(GroundFog::FarEnd, GroundFog::FadeEnd) : GroundFog::FadeEnd;
	const bool surface = !sky && sceneDistance < range;
	const float tEnd = surface ? sceneDistance : range;
	const float2 endXY = dir.xy * tEnd;
	const float camZ = FrameBuffer::CameraPosAdjust.z;
	// What the ray ends on matters for surfaces within the sheet's range; it lifts the layers by less than this.
	const bool wrap = surface && tEnd < GroundFog::FadeEnd;
	const float lift = wrap ? 2.0 * GroundFog::SurfaceLift : 0.0;
	const float floorLift = max(GroundFog::FloorOffset, wrap ? 1.5 * max(GroundFog::FloorMax, GroundFog::SurfaceLift) : 0.0);
	// A far surface the valley layer is anchored to lies at most this far above the coarse ground under it.
	const float reachEnd = max(tEnd * GroundFog::StepGrowth, GroundFog::StepMin);
	const bool anchor = surface && GroundFog::FarExtinction > 0.0 && tEnd > GroundFog::FadeStart;
	const float anchorReach = GroundFog::FarThickness + 0.1 * reachEnd;
	const float valleyLift = anchor ? 2.0 * anchorReach : 0.0;
	const bool debugTaps = GroundFog::DebugMode == 4u;
	const float4 clear = float4(0.0, 0.0, 0.0, 1.0);
	const float4 clearAux = float4(auxDepths, 1.0, 0.0);

	// Where the nearest of the four scene pixels lies along this ray.
	const float tNear = viewNear < viewFar * 0.98 ? min(sceneDistance * viewNear / viewFar, tEnd) : tEnd;

	// The pool: from the pixel, its surface and the simulation alone. No height map is read for it.
	float3 normal = 0.0;
	bool hasNormal = false;
	GroundFog::Pool pool;
	pool.tau = pool.tauNear = pool.t = pool.z = pool.above = pool.amount = 0.0;
	pool.speed = 0.0;
	pool.floorZ = GroundFog::FloorEmpty;
	pool.range = pool.known = pool.ground = pool.over = pool.within = 0.0;
	pool.crest = pool.sunSide = 0.5;
	// How much of the fog this pixel sees is the pool, for what only the pool's shading takes.
	float poolShare = 0.0;
	// From the pool's volume: the sun its fog sees (and how much that stands for the sun map lookup), its sky light.
	float poolSun = 1.0, poolSunWeight = 0.0, poolSky = 1.0;
	if (GroundFog::PoolExtinction > 0.0 && surface && tEnd < GroundFog::PoolEnd) {
		normal = GroundFog::SurfaceNormal(scenePixel, depth, position, mainSize, activeMain);
		hasNormal = true;
		taps += 4u;
		pool = GroundFog::EvaluatePool(position, dir, tEnd, camZ, normal, tNear, SharedData::DirLightDirection.xyz, taps, poolSun, poolSunWeight, poolSky);
		if (pool.floorZ < GroundFog::FloorUnknown) {
			uint2 splatSize;
			FloorSplat.GetDimensions(splatSize.x, splatSize.y);
			const float2 splatUv = GroundFog::SimUv(position.xy);
			if (all(splatUv > 0.0) && all(splatUv < 1.0)) {
				// Not atomic: of two pixels that write one texel at the same moment either may win. The simulation
				// takes the lowest of each texel's surroundings and lets the floor rise only slowly, so a lost
				// write changes nothing that lasts.
				const uint2 texel = min(uint2(splatUv * float2(splatSize)), splatSize - 1u);
				if (pool.floorZ < FloorSplat[texel])
					FloorSplat[texel] = pool.floorZ;
			}
		}
	}

	// Debug views of the pool alone, laid over the scene at half strength.
	if (GroundFog::DebugMode == 6u || GroundFog::DebugMode == 7u) {
		float3 colour = 0.0;
		float cover = 0.0;
		if (pool.range > 0.0) {
			cover = 0.5;
			if (GroundFog::DebugMode == 6u) {
				// Floor map. Blue: no floor known under the pixel. Green: ground (brighter the more it counts).
				// Red: stands on a known floor, brightest at the floor and dark at and above the layer top.
				// Yellow: ground that is partly counted as standing above the floor.
				const float standing = 1.0 - saturate(pool.over / max(GroundFog::PoolThickness, 1.0));
				colour = pool.known > 0.0 ? float3((1.0 - pool.ground) * (0.15 + 0.85 * standing), pool.ground, 0.0) : float3(0.0, 0.3 * pool.ground, 1.0);
			} else {
				// Pool optical depth along the ray: black none, then blue 0.05, green 0.5, yellow 1, red 2 (its cap).
				colour = pool.tau > 0.0 ? GroundFog::Hue(0.66 * (1.0 - saturate(log2(pool.tau / 0.05) / log2(40.0)))) : 0.0;
			}
		}
		FogOut[pixel] = float4(colour * GroundFog::SkyMean() * 4.0, 1.0 - cover);
		FogAuxOut[pixel] = float4(auxDepths, 1.0 - cover, 0.0);
		return;
	}

#if GROUND_FOG_POOL_DEBUG > 0 && GROUND_FOG_POOL_DEBUG < 5
	{
		// The pool's debug view chosen by #define (see Pool.hlsli), opaque over the scene within the pool's reach.
		// 1 Floor map. Blue: no floor known under the pixel. Green: ground, the fog lies on it. Red: stands in the
		//   fog on a known floor, bright at the floor, dark at and above the top of the fog. Magenta: ground on
		//   the top of a raised thing (a cart bed, a table, a rock), which gets no fog. A blue tint on green or
		//   red: the floor there comes from the terrain height map (the screen has not shown it).
		// 2 Pool optical depth along the ray: black none, then blue 0.05, green 0.5, yellow 1, red 2 (its cap).
		// 3 Simulation at the pixel. Red: amount of fog (full at 2). Green: the resting amount it settles to (full
		//   at 2). Blue: how fast it moves (full at 3 m/s).
		// 4 Placement. Red: what terrain, water, time of day and shade give the place (full at 1.5), before the
		//   floor's relief and the wisps. Green: the resting amount after them (full at 2). Blue: raised thing
		//   (no fog rests there).
		float3 colour = 0.0;
		float cover = 0.5;
		if (pool.range > 0.0) {
			cover = 1.0;
			const float2 simUv = GroundFog::SimUv(position.xy);
			// How much the floor here is a floor the fog lies on, not a raised thing's top (FogSlope.z).
			const float floorness = saturate(GroundFog::FogSlope.SampleLevel(LinearSampler, simUv, 0).z);
	#	if GROUND_FOG_POOL_DEBUG == 1
			const float standing = 1.0 - saturate(pool.over / GroundFog::PoolHardTop);
			if (pool.known <= 0.0)
				colour = float3(0.0, 0.25 * pool.ground, 1.0);
			else
				colour = float3((1.0 - pool.ground) * (0.15 + 0.85 * standing) + pool.ground * (1.0 - floorness), pool.ground * floorness, pool.ground * (1.0 - floorness) + (pool.known < 1.0 ? 0.4 : 0.0));
	#	elif GROUND_FOG_POOL_DEBUG == 2
			colour = pool.tau > 0.0 ? GroundFog::Hue(0.66 * (1.0 - saturate(log2(pool.tau / 0.05) / log2(40.0)))) : 0.0;
	#	elif GROUND_FOG_POOL_DEBUG == 3
			const float4 state = GroundFog::FogSim.SampleLevel(LinearSampler, simUv, 0);
			colour = float3(saturate(0.5 * state.z), saturate(0.5 * state.w), saturate(length(state.xy) / 210.0));
	#	else
			const float4 state = GroundFog::FogSim.SampleLevel(LinearSampler, simUv, 0);
			const float4 slopeSun = GroundFog::FogSlope.SampleLevel(LinearSampler, simUv, 0);
			float water, groundZ;
			const float open = GroundFog::RestingAmountAt(position.xy, GroundFog::StepMin, GroundFog::BurnSunlitNear, 1.0 - saturate(slopeSun.w), water, groundZ);
			const bool floorKnown = GroundFog::FogFloor.SampleLevel(LinearSampler, simUv, 0) < GroundFog::FloorUnknown;
			colour = float3(saturate(open / 1.5), saturate(0.5 * state.w), floorKnown ? 1.0 - floorness : 0.0);
	#	endif
		}
		FogOut[pixel] = float4(colour * GroundFog::SkyMean() * 4.0, cover >= 1.0 ? 1e-4 : 0.5);
		FogAuxOut[pixel] = float4(auxDepths, cover >= 1.0 ? 1e-4 : 0.5, 0.0);
		return;
	}
#endif

	// The whole ray against the highest ground under it: one read settles short rays that end well above the ground.
	++taps;
	const bool march = camZ + min(dir.z * tEnd, 0.0) - GroundFog::GroundMaxAlong(0.0, endXY) <= GroundFog::SlabTop(0.0, tEnd, lift, floorLift, valleyLift);
	if (!march && pool.tau <= 0.0) {
#if GROUND_FOG_POOL_DEBUG >= 5
		FogOut[pixel] = float4(0.0, 0.0, 0.0, 1e-4);
		FogAuxOut[pixel] = float4(auxDepths, 1e-4, 0.0);
		return;
#endif
		FogOut[pixel] = debugTaps ? float4(GroundFog::TapColor(taps) * GroundFog::SkyMean() * 4.0, 0.5) : clear;
		FogAuxOut[pixel] = debugTaps ? float4(auxDepths, 0.5, 0.0) : clearAux;
		return;
	}

	GroundFog::Ray ray;
	ray.dir = dir;
	ray.camZ = camZ;
	ray.tEnd = tEnd;
	ray.surfaceOffset = 0.0;
	ray.blendLength = 0.0;
	ray.floorEnd = GroundFog::FloorOffset * GroundFog::FloorReach(endXY);
	ray.floorLength = 0.0;
	ray.tFloorEnd = tEnd;
	ray.valleyOffset = 0.0;
	ray.valleyLength = 0.0;
	if (anchor && march) {
		// Far ground is read from coarse levels of the height map, and a layer a few meters thick laid on that
		// lifts off rising ground or sinks into it in bands. Where the ray ends on a surface, that surface is the
		// ground there: the layer's ground meets it over the end of the ray, so the fog touches what the pixel
		// shows. Things standing well above the coarse ground (trees, towers, cliffs) are not ground.
		const GroundFog::GroundSample endGround = GroundFog::GroundAt(endXY, reachEnd);
		taps += endGround.reads;
		float above = position.z + camZ - endGround.z;
		above = endGround.water > 0.0 ? max(above, 0.0) : above;
		ray.valleyOffset = above * (1.0 - smoothstep(anchorReach, 2.0 * anchorReach, abs(above)));
		ray.valleyLength = ray.valleyOffset != 0.0 ? min(2.0 * reachEnd, tEnd) : 0.0;
	}
	ray.tValley = tEnd - ray.valleyLength;
	// In front of anything that is not ground the fog fades out over the end of the ray, so it never draws the
	// outline of what stands in it.
	float softScale = 1.0;
	if (wrap && march) {
		// Near its end the ray is over whatever the pixel shows, not over the mapped terrain. If that faces up it
		// is ground: the ground under the end of the ray is moved to it, so the fog lies on rocks, roads and
		// floors and thins out against them like against the terrain. If it does not, nothing moves.
		if (!hasNormal) {
			normal = GroundFog::SurfaceNormal(scenePixel, depth, position, mainSize, activeMain);
			taps += 4u;
		}
		const float facingUp = smoothstep(0.5, 0.85, normal.z);
		softScale = lerp(0.5, 1.0, facingUp);
		const bool nearEnd = GroundFog::NearExtinction > 0.0 && tEnd < GroundFog::NearEnd;
		if ((facingUp > 0.0 || (nearEnd && ray.floorEnd > 0.0)) && GroundFog::SurfaceBlend > 0.0) {
			const GroundFog::GroundSample endGround = GroundFog::GroundAt(endXY, 1.0);
			taps += endGround.reads;
			float above = position.z + camZ - endGround.z;
			// Under water the surface seen may be the bed; the fog stays on the water.
			above = endGround.water > 0.0 ? max(above, 0.0) : above;
			ray.surfaceOffset = above * GroundFog::SurfaceWeight(above) * facingUp;
			ray.blendLength = ray.surfaceOffset != 0.0 ? min(GroundFog::SurfaceBlend, tEnd) : 0.0;
			if (nearEnd) {
				// The near volume follows level floors above the map instead: roads on fill, bridges, town floors.
				// A surface more than a step above the map must go on around the pixel to be one (the top of a
				// rock, a barrel or a wall is something standing in the fog), unless it is the level the player
				// stands on. Ground below the map is always followed.
				const float base = ray.floorEnd;
				float known = facingUp * GroundFog::FloorWeight(above);
				const float needsSupport = smoothstep(0.25 * GroundFog::SurfaceLift, 0.75 * GroundFog::SurfaceLift, above);
				if (known > 0.0 && needsSupport > 0.0 && abs(above - base) > 0.5 * GroundFog::SurfaceLift)
					known *= lerp(1.0, GroundFog::FloorSupport(position, normal, mainSize, activeMain, taps), needsSupport);
				// Whatever is seen below the floor's level shows that the floor does not reach there: the fog's
				// ground is no higher than that point, and it has come down by where the ray sinks through the
				// floor's level.
				ray.floorEnd = min(lerp(base, above, known), max(above, 0.0));
				// A floor far from the level the ray came over is met over a longer stretch.
				ray.floorLength = min(GroundFog::SurfaceBlend + 4.0 * abs(ray.floorEnd - base), tEnd);
				if (ray.floorEnd < base && dir.z < -1e-4)
					ray.tFloorEnd = clamp((endGround.z + base - camZ) / dir.z, 0.0, tEnd);
			}
		}
	}
	ray.tBlend = tEnd - ray.blendLength;
	ray.tFloor = ray.tFloorEnd - ray.floorLength;
	const float tSoft = softScale < 1.0 && GroundFog::SoftLength > 0.0 ? max(tEnd - GroundFog::SoftLength, 0.0) : tEnd;
	const float4 stops = float4(ray.tBlend, ray.tFloor, tSoft, tNear);

	// The ray in height map uv, to find where it crosses the texel grid.
	const float2 uvStart = GroundFog::MapUv(0.0);
	const float2 uvStep = dir.xy * GroundFog::MapScaleOffset.xy;

	GroundFog::Slow slow;
	slow.tShade = slow.tAround = slow.tWater = -1.0;
	slow.shade = slow.around = 0.0;
	slow.lieIndex = -1000;
	slow.lieNear = slow.lieFar = 0.0;
	slow.waterCeiling = 1.0e30;

	GroundFog::Seen seen;
	seen.tau = seen.share = seen.t = seen.z = seen.above = seen.valley = seen.shade = 0.0;
	float tauNear = -1.0;
	// A ray that only the pool has fog for is not marched.
	GroundFog::Joint a = (GroundFog::Joint)0;
	if (march)
		a = GroundFog::MakeJoint(ray, 0.0, GroundFog::StepMin, slow, taps);
	[loop] for (uint i = 0u; i < (march ? GroundFog::MaxSteps : 0u); ++i)
	{
		if (a.t >= tEnd - 0.01)
			break;
		if (tauNear < 0.0 && a.t >= tNear - 0.01)
			tauNear = seen.tau;
		// Above the fog: pass over the stretches of the ray that no ground under them reaches up to, each three
		// times as long as the one before, at one read each. A ray that looks up goes through to its end this way.
		if (!a.sheet.inside && !a.valley.inside && !a.near.inside && a.h > 0.0 && a.hn > 0.0 && a.hv > 0.0) {
			float from = a.t;
			[loop] for (uint k = 0u; k < 10u && from < tEnd - 0.01; ++k)
			{
				const float to = min(max(3.0 * from, from + 4.0 * GroundFog::StepMin), tEnd);
				++taps;
				if (camZ + dir.z * (dir.z < 0.0 ? to : from) - GroundFog::GroundMaxAlong(dir.xy * from, dir.xy * to) <= GroundFog::SlabTop(from, to, lift, floorLift, valleyLift))
					break;
				from = to;
			}
			// Nothing was added on the way, so the fog in front of the nearest scene pixel is what there is now.
			if (tauNear < 0.0 && from >= tNear - 0.01)
				tauNear = seen.tau;
			if (from >= tEnd - 0.01)
				break;
			if (from > a.t)
				a = GroundFog::MakeJoint(ray, from, max(from * GroundFog::StepGrowth, GroundFog::StepMin), slow, taps);
		}

		// The next joint: where the ray next crosses the texel grid of the ground it is over. The grid is fixed in
		// the world and coarsens with distance. Where the ground starts to move toward the visible surface, where
		// the fade in front of an object starts and where the nearest scene pixel lies there is a joint too.
		float tb;
		{
			const float2 cells = GroundFog::GroundCells(max(a.t * GroundFog::StepGrowth, GroundFog::StepMin));
			const float from = a.t + 0.25;
			const float2 cell = (uvStart + uvStep * from) * cells - 0.5;
			const float2 rate = uvStep * cells;
			const float2 next = float2(rate.x > 0.0 ? floor(cell.x) + 1.0 : floor(cell.x), rate.y > 0.0 ? floor(cell.y) + 1.0 : floor(cell.y));
			const float2 crossing = float2(abs(rate.x) > 1e-12 ? (next.x - cell.x) / rate.x : 1.0e30, abs(rate.y) > 1e-12 ? (next.y - cell.y) / rate.y : 1.0e30);
			tb = from + max(min(crossing.x, crossing.y), 0.0) + 0.25;
			const float4 ahead = float4(stops.x > a.t + 0.01 ? stops.x : tEnd, stops.y > a.t + 0.01 ? stops.y : tEnd,
				stops.z > a.t + 0.01 ? stops.z : tEnd, stops.w > a.t + 0.01 ? stops.w : tEnd);
			tb = min(min(tb, tEnd), min(min(ahead.x, ahead.y), min(ahead.z, ahead.w)));
			if (ray.tFloorEnd > a.t + 0.01)
				tb = min(tb, ray.tFloorEnd);
		}
		const GroundFog::Joint b = GroundFog::MakeJoint(ray, tb, max(tb * GroundFog::StepGrowth, GroundFog::StepMin), slow, taps);

		const float scale = a.t >= tSoft - 0.01 ? softScale : 1.0;
		GroundFog::IntegrateNear(seen, a, b, scale);
		GroundFog::Integrate(seen, a, b, a.sheet, b.sheet, a.h, b.h, GroundFog::GroundExtinction * scale, GroundFog::GroundFill, false);
		GroundFog::Integrate(seen, a, b, a.valley, b.valley, a.hv, b.hv, GroundFog::FarExtinction, GroundFog::ValleyFill(), true);
		a = b;
		if (seen.tau > 9.0)
			break;
	}
	if (tauNear < 0.0)
		tauNear = seen.tau;

	// The pool lies at the end of the ray, behind what the march found.
	if (pool.tau > 0.0) {
		const float share = exp(-seen.tau) * GroundFog::OneMinusExp(pool.tau);
		seen.share += share;
		seen.t += share * pool.t;
		seen.z += share * pool.z;
		seen.above += share * pool.above;
		seen.tau += pool.tau;
		tauNear += pool.tauNear;
		poolShare = share / max(seen.share, 1e-6);
	}

	if (debugTaps) {
		FogOut[pixel] = float4(GroundFog::TapColor(taps) * GroundFog::SkyMean() * 4.0, 0.5);
		FogAuxOut[pixel] = float4(auxDepths, 0.5, 0.0);
		return;
	}
	if (seen.share < 1.0 / 2048.0) {
#if GROUND_FOG_POOL_DEBUG >= 5
		FogOut[pixel] = float4(0.0, 0.0, 0.0, 1e-4);
		FogAuxOut[pixel] = float4(auxDepths, 1e-4, 0.0);
		return;
#endif
		FogOut[pixel] = clear;
		FogAuxOut[pixel] = clearAux;
		return;
	}

	// One lighting point, in the middle of the fog this pixel sees, lit by the same functions as the shared
	// atmosphere volume: sun and moons through the cascades, terrain shadow, cloud shadows and the atmosphere,
	// dimmed by the fog above the point, plus the sky.
	const float lightT = seen.t / seen.share;
	const float3 lightPos = float3(dir.xy * lightT, seen.z / seen.share - camZ);
	const float tauAbove = seen.above / seen.share;

	float3 albedo = GroundFog::Albedo;
	if (GroundFog::DebugMode == 2u) {
		const float ground = GroundFog::GroundAt(lightPos.xy, max(lightT * GroundFog::StepGrowth, GroundFog::StepMin)).z;
		albedo = GroundFog::Hue(ground / 1024.0) * (frac(ground / 128.0) < 0.5 ? 1.0 : 0.3);
	} else if (GroundFog::DebugMode == 3u) {
		// Red: valley layer. Green: sheet and near volume. Blue: fog kept by terrain shadow (the burn-off mask).
		const float valley = seen.valley / seen.share;
		albedo = 0.08 + 0.92 * float3(valley, 1.0 - valley, seen.shade / seen.share);
	} else if (GroundFog::DebugMode == 5u) {
		// The simulation under the pool. Red: how fast the fog moves (full at 3 m/s). Green: how much there is
		// (full at twice the mean). Blue: pixels that are not floor (they add nothing to the floor map).
		albedo = pool.tau > 0.0 ? 0.05 + 0.95 * float3(saturate(length(pool.speed) / 210.0), saturate(0.5 * pool.amount), pool.floorZ < GroundFog::FloorUnknown ? 0.0 : 1.0) : 0.02;
	}

	const SharedData::PhysSkyData physSky = SharedData::physSkyData;
	float atmosphereVisibility;
	float3 diffuseVisibility;
	float visibility = MediumVisibility(lightPos, atmosphereVisibility, diffuseVisibility);
	{
		// The sun's shadow in the fog. A medium is lit all along the ray and through its whole depth, so a shadow in
		// it has no edge and no height of its own. Inside the simulation's window it comes from the sun map: the
		// cascades taken over the area of a texel and the height of the pool, smoothed over time, read at two
		// places along the stretch of the ray the fog was seen on. Outside it, from the cascades at the lighting
		// point, filtered.
		const float2 sunUv = GroundFog::SimUv(lightPos.xy);
		const float mapped = GroundFog::SimInside(sunUv);
		float sun = 1.0;
		if (mapped > 0.0) {
			const float2 along = dir.xy * max(0.3 * pool.within, 40.0) * GroundFog::FlowWindow.z;
			sun = 0.5 * (GroundFog::FogSlope.SampleLevel(LinearSampler, sunUv - along, 0).w + GroundFog::FogSlope.SampleLevel(LinearSampler, sunUv + along, 0).w);
		}
		if (mapped < 1.0)
			sun = lerp(MediumCascadeShadow(lightPos), sun, mapped);
		// Near the camera the pool's volume has taken the sun at each place of its own fog (and through the fog
		// above each toward the sun): that stands for it, for the pool's share.
		sun = lerp(sun, poolSun, poolShare * poolSunWeight);
		visibility *= sun;
		atmosphereVisibility *= sun;
		diffuseVisibility *= sun;
	}

	// The sky light follows the sky over the fog, not the sky as a whole: the cloud that shades the sun at this
	// point also stands between the fog and most of the sky's light. Without this the fog keeps the brightness of
	// the average sky under a storm cloud and glows against the dark ground.
	float cloud = PhysSky::GetDirlightTransmittance(lightPos + FrameBuffer::CameraPosAdjust.xyz, LinearSampler, true).x;
#ifdef CLOUD_SHADOWS
	if ((ViewAtmosphere::Data[0].flags.z & 512u) && CloudShadows::IntersectCloudDist(lightPos, SharedData::DirLightDirection.xyz) >= 0.0)
		cloud *= CloudShadows::GetCloudShadowMult(lightPos, LinearSampler);
#endif
	const float skyDimming = lerp(1.0, sqrt(saturate(cloud)), GroundFog::CloudDarkening);
	diffuseVisibility *= skyDimming;

	float3 light = AtmosphereLight(0u, lightPos, dir, physSky.sunDir, physSky.sunlightColor, 0.0, albedo,
		visibility * GroundFog::SelfShadowing(tauAbove, physSky.sunDir.z), diffuseVisibility);
	light += AtmosphereLight(1u, lightPos, dir, physSky.masserDir, physSky.masserColor, 0.0, albedo,
		visibility * GroundFog::SelfShadowing(tauAbove, physSky.masserDir.z), diffuseVisibility);
	light += AtmosphereLight(2u, lightPos, dir, physSky.secundaDir, physSky.secundaColor, 0.0, albedo,
		visibility * GroundFog::SelfShadowing(tauAbove, physSky.secundaDir.z), diffuseVisibility);
	// What a white surface lying where the pool lies would send back of the sun and moons: that is what the pool is
	// lit like, a white diffuse body in the scene's light (the same colour after the atmosphere, the same shadow
	// from the sun map, its own depth over the point), and never more. Its lumpy top catches a low light better
	// than level ground does, hence the least angle. The thin layers further out keep the scattering of a haze
	// (the forward-peaked phase above) with a share of it sent out evenly.
	{
		const float3 planet = PhysSky::GetMediumPlanetPosition(lightPos);
		const float3 lightDirs[3] = { physSky.sunDir, physSky.masserDir, physSky.secundaDir };
		const float3 lightColors[3] = { physSky.sunlightColor, physSky.masserColor, physSky.secundaColor };
		float3 white = 0.0;
		[unroll] for (uint l = 0u; l < 3u; ++l)
		{
			if (any(lightColors[l] > 0.0) && dot(lightDirs[l], lightDirs[l]) >= 0.5) {
				const float3 arriving = lightColors[l] * PhysSky::SampleAtmosphereLightTr(PhysSky::TexTrLut, LinearSampler, planet, lightDirs[l], physSky.diskAngularRadius[l]) *
				                        (visibility * GroundFog::SelfShadowing(tauAbove, lightDirs[l].z));
				light += arriving * (albedo * GroundFog::SunWrap * 0.0795775);
				white += arriving * (albedo * 0.3183099 * max(lightDirs[l].z, 0.2));
			}
		}
		light = lerp(light, white, poolShare);
	}
	// The pool's lumpy top: the side of a billow toward the light is brighter, the side away from it darker.
	// (Both are about 0.5 on average, and these average 1: shading only, no light is lost.)
	light *= lerp(1.0, lerp(0.7, 1.3, pool.sunSide), poolShare);
	light *= GroundFog::SunScale;
	// Sky light reaches fog in the shadow of terrain and objects too, so it takes no direct visibility.
	// The sky's light on the fog is what the open sky gives, less what stands over the fog: under a canopy or
	// beside a wall it gets as little of it as the ground there does, or it would glow against that ground.
	// Where the Skylighting probes run, they say how much of the sky the fog sees, as they do for the ground.
	// Without them, how much of the ground a few meters around is in the sun's shadow (the sun map, read wide)
	// stands in, and only weakly: a shadow is not a roof (a street in the shadow of a wall at a low sun still has
	// the sky over it). Taking that stand-in at full strength (as before) made the fog in any shadow as dim as the
	// ground there, so it vanished against it.
	float skyOpen = 1.0;
	if (GroundFog::CanopyShade > 0.0) {
		bool probed = false;
#ifdef SKYLIGHTING
		if (ViewAtmosphere::Data[0].flags.z & 8u) {
			const float3 probeAt = lightPos + float3(0.0, 0.0, 20.0);
			const float seen = Skylighting::CosineLobeVisibility(Skylighting::SampleNoBias(probeAt), float3(0.0, 0.0, 1.0), Skylighting::GetFadeOutFactor(probeAt));
			skyOpen = max(Skylighting::MixDiffuse(saturate(seen)), 0.15);
			probed = true;
		}
#endif
		if (!probed) {
			const float2 shadeUv = GroundFog::SimUv(lightPos.xy);
			const float mapped = GroundFog::SimInside(shadeUv);
			if (mapped > 0.0) {
				uint slopeWidth, slopeHeight;
				GroundFog::FogSlope.GetDimensions(slopeWidth, slopeHeight);
				const float2 wide = 7.0 / float2(slopeWidth, slopeHeight);
				const float lit = 0.25 * (GroundFog::FogSlope.SampleLevel(LinearSampler, shadeUv + wide * float2(1.0, 0.4), 0).w + GroundFog::FogSlope.SampleLevel(LinearSampler, shadeUv + wide * float2(-0.4, 1.0), 0).w +
											 GroundFog::FogSlope.SampleLevel(LinearSampler, shadeUv + wide * float2(-1.0, -0.4), 0).w + GroundFog::FogSlope.SampleLevel(LinearSampler, shadeUv + wide * float2(0.4, -1.0), 0).w);
				skyOpen = lerp(1.0, lerp(0.65, 1.0, lit), mapped);
			}
		}
		skyOpen = lerp(1.0, skyOpen, GroundFog::CanopyShade);
	}
	// And the folds between its billows see less of the sky than their tops.
	const float folds = lerp(1.0, lerp(0.8, 1.2, pool.crest), poolShare);
	const float3 skyLight = albedo * GroundFog::SkyMean() * GroundFog::SkyAmbientScale * skyDimming * skyOpen * folds * lerp(1.0, poolSky, poolShare);
	// Fog in the sun is brighter than fog in the shade, but not without end: what it scatters of the sun and moons
	// levels off at SunLimit times its sky light (at the limit's own value it has reached half of that), keeping
	// its colour. So sunlit fog reads as lit fog, not as paint.
	if (GroundFog::SunLimit > 0.0) {
		static const float3 luma = float3(0.2126, 0.7152, 0.0722);
		const float cap = GroundFog::SunLimit * dot(skyLight, luma);
		// The pool needs no such limit: it is lit like a white surface, which is the most it can be.
		light *= lerp(cap / max(cap + dot(light, luma), 1e-6), 1.0, poolShare);
	}
	light += skyLight;

	// A uniform medium of this light per unit of extinction; the composite multiplies by the opacity at the depth
	// of each of its pixels.
#if GROUND_FOG_POOL_DEBUG == 5
	// 5: the light the fog adds to the scene at this pixel (all the fog this pass draws; near the camera that is the
	// pool), at the scene's exposure, opaque, so it can be set beside the ground it lies on.
	FogOut[pixel] = float4(min(max(light, 0.0), 65504.0) * GroundFog::OneMinusExp(seen.tau), 1e-4);
	FogAuxOut[pixel] = float4(auxDepths, 1e-4, 0.0);
	return;
#elif GROUND_FOG_POOL_DEBUG == 6
	// 6: how much of the scene behind the fog it takes away (1 - transmittance), as grey: white all of it.
	FogOut[pixel] = float4(GroundFog::OneMinusExp(seen.tau) * GroundFog::SkyMean() * 4.0, 1e-4);
	FogAuxOut[pixel] = float4(auxDepths, 1e-4, 0.0);
	return;
#endif
	FogOut[pixel] = float4(min(max(light, 0.0), 65504.0), exp(-seen.tau));
	// w: the share of the fog in front of the nearest depth that is the pool, which lies at the end of the ray and
	// not along it.
	FogAuxOut[pixel] = float4(auxDepths, exp(-tauNear), tauNear > 1e-5 ? saturate(pool.tauNear / tauNear) : 0.0);
}
