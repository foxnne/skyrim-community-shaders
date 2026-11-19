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
// Three layers share the march:
// - the near volume: a few meters tall, soft-topped, within NearEnd of the camera. This is what reads as fog in the
//   air from any direction: a level ray at eye height runs through it. Near the camera the joints are one height
//   map texel (under 2 m) apart, and each segment is integrated in closed form.
// - the sheet: thin, on the ground, out to FadeEnd.
// - the valley layer: deeper, in hollows and over water, from where the sheet fades out to kilometers away.
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
#include "Common/Random.hlsli"
#include "Common/ViewAtmosphere.hlsli"
#include "PhysicalSky/Common.hlsli"
#include "ViewAtmosphere/MediumLighting.hlsli"

#include "GroundFog/GroundFog.hlsli"

Texture2D<float> SceneDepth : register(t40);
RWTexture2D<float4> FogOut : register(u0);
RWTexture2D<float4> FogAuxOut : register(u1);

namespace GroundFog
{
	// Distances from the player over which the floor offset under the player's feet stops applying, game units.
	static const float FloorNear = 1400.0;
	static const float FloorFar = 3150.0;
	// A level surface above the mapped ground is a floor if it continues this far around the pixel, game units.
	static const float FloorSupportRadius = 84.0;
	// Ground this far below the terrain shadow's surface has been out of the sun for long: it keeps all its fog.
	static const float ShadeDepth = 1050.0;

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
		// floorEnd over the last floorLength of the ray.
		float floorEnd;
		float tFloor;
		float floorLength;
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
		return max(GroundFill, 0.25 * NearThickness);
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

	// Hollows hold more, rises less. One read of the terrain averaged around the point.
	float Basin(float2 xy, float terrain)
	{
		const float hollow = clamp((GroundAround(xy, BasinRadius) - terrain) * InvBasinDepth, -1.0, 1.0);
		return clamp(1.0 + BasinStrength * hollow, 0.0, 1.0 + BasinStrength);
	}

	// 1 where the terrain has kept the sun off a point for long, 0 where the sun reaches it. From the Terrain
	// Shadows height field for the present sun: a point is in shadow when it lies below that field, and the deeper
	// below, the longer ago the shadow's edge passed over it (in the morning the field sinks as the sun climbs).
	// So depth below the field stands for time out of the sun, with no history stored, and the edge of the mask
	// moves smoothly as the sun does. One read.
	float TerrainShade(float2 xyAbs, float z, inout uint taps)
	{
#ifdef TERRAIN_SHADOWS
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

	// No fog lies higher than this above the mapped ground anywhere between two distances along the ray.
	float SlabTop(float from, float to, float lift, float floorLift)
	{
		float top = -1.0e30;
		if (from < FadeEnd)
			top = TopMax() + lift;
		if (NearExtinction > 0.0 && from < NearEnd)
			top = max(top, NearTopMax() + floorLift);
		if (FarExtinction > 0.0 && to > FadeStart)
			top = max(top, ValleyTopMax());
		return top;
	}

	// Fills in what a joint needs to hold fog. Joints clear of all layers cost nothing more than their ground read.
	void Shade(inout Joint joint, float2 xy, GroundSample ground, float reach, inout uint taps)
	{
		const float2 xyAbs = xy + FrameBuffer::CameraPosAdjust.xy;
		const float sheetFade = 1.0 - smoothstep(FadeStart, FadeEnd, joint.t);
		// The valley layer comes in exactly as the sheet goes out, so there is no ring where the sheet ends.
		const float valleyFade = FarExtinction > 0.0 ? (1.0 - sheetFade) * (1.0 - smoothstep(0.75 * FarEnd, FarEnd, joint.t)) : 0.0;
		const float nearFade = NearExtinction > 0.0 ? 1.0 - smoothstep(0.5 * NearEnd, NearEnd, joint.t) : 0.0;
		joint.shade = 0.0;
		joint.sheet.inside = sheetFade > 0.0 && joint.h < TopMax() && joint.h > -GroundFill - 64.0;
		joint.sheet.top = Thickness;
		joint.sheet.place = 0.0;
		joint.valley.inside = valleyFade > 0.0 && joint.h < ValleyTopMax() && joint.h > -ValleyFill() - 64.0;
		joint.valley.top = FarThickness;
		joint.valley.place = 0.0;
		joint.near.inside = nearFade > 0.0 && joint.hn < NearTopMax() && joint.hn > -NearFill() - 64.0;
		joint.near.top = NearThickness;
		joint.near.place = 0.0;
		if (!joint.sheet.inside && !joint.valley.inside && !joint.near.inside)
			return;
		const float coverage = MapCoverage(MapUv(xy));
		joint.shade = TerrainShade(xyAbs, ground.z + 70.0, taps);
		if (joint.sheet.inside || joint.near.inside) {
			// Shared by the sheet and the near volume: they are one body of fog, thin far away and tall close by.
			float place = coverage * lerp(1.0, WaterPreference, ground.water) * BurnOff(BurnSunlitNear, joint.shade);
			if (place > 0.0 && BasinStrength > 0.0) {
				place *= Basin(xy, ground.terrain);
				++taps;
			}
			float3 flow = 0.0;
			if (FlowWindow.w > 0.0) {
				flow = Flow(xy);
				++taps;
			}
			const float2 pattern = xyAbs - flow.xy;
			place *= DensityModifier(pattern, reach) * (1.0 - saturate(flow.z));
			joint.sheet.place = place * sheetFade;
			joint.near.place = place * nearFade;
			if (joint.sheet.inside)
				joint.sheet.top = LayerTop(pattern, reach);
			if (joint.near.inside)
				joint.near.top = max(NearThickness * Breathing(pattern), 1.0);
		}
		if (joint.valley.inside) {
			// Only where the land lies lower than the country around it, or on water: pools in basins and on
			// lakes, nothing on rises and open slopes.
			const float hollow = saturate((GroundAround(xy, FarBasinRadius) - ground.terrain) * InvFarBasinDepth);
			++taps;
			const float pool = smoothstep(0.0, 1.0, max(hollow, ground.water));
			// Broad banks, travelling with the wind like the sheet's.
			const float bank = lerp(0.7, 1.25, ValueNoise((xyAbs - WindTravel) * InvBankScale * 0.25 + 7.3));
			joint.valley.place = coverage * valleyFade * pool * BurnOff(BurnSunlitFar, joint.shade);
			joint.valley.top = max(FarThickness * (0.35 + 0.65 * pool) * bank, 1.0);
		}
	}

	// Reads the ground under a point of the ray and fills in the joint.
	Joint MakeJoint(Ray ray, float t, float reach, inout uint taps)
	{
		const float2 xy = ray.dir.xy * t;
		const GroundSample ground = GroundAt(xy, reach);
		++taps;
		Joint joint;
		joint.t = t;
		joint.ground = ground.z + SheetLiftAt(ray, t);
		const float z = ray.camZ + ray.dir.z * t;
		joint.h = z - joint.ground;
		joint.hn = z - ground.z - FloorAt(ray, t);
		Shade(joint, xy, ground, reach, taps);
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
	void Integrate(inout Seen seen, Joint a, Joint b, Layer la, Layer lb, float extinction, float fill, bool valley)
	{
		if (!la.inside && !lb.inside)
			return;
		const bool both = la.inside && lb.inside;
		const float top = both ? 0.5 * (la.top + lb.top) : (la.inside ? la.top : lb.top);
		const float place = both ? 0.5 * (la.place + lb.place) : (la.inside ? la.place : lb.place);
		const Profile profile = MakeProfile(top, fill);
		const float tau = extinction * place * LineMean(profile, a.h, b.h) * (b.t - a.t);
		if (tau <= 0.0)
			return;
		// Lit at the middle of the segment, but inside the layer even where the ray itself is under the ground
		// or above the top.
		const float height = clamp(0.5 * (a.h + b.h), 0.15 * top, top);
		Accumulate(seen, a, b, tau, 0.5 * (a.ground + b.ground), height,
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
	const bool debugTaps = GroundFog::DebugMode == 4u;
	const float4 clear = float4(0.0, 0.0, 0.0, 1.0);
	const float4 clearAux = float4(auxDepths, 1.0, 1.0);

	// The whole ray against the highest ground under it: one read settles short rays that end well above the ground.
	++taps;
	if (camZ + min(dir.z * tEnd, 0.0) - GroundFog::GroundMaxAlong(0.0, endXY) > GroundFog::SlabTop(0.0, tEnd, lift, floorLift)) {
		FogOut[pixel] = debugTaps ? float4(GroundFog::TapColor(taps) * GroundFog::SkyMean() * 4.0, 0.5) : clear;
		FogAuxOut[pixel] = debugTaps ? float4(auxDepths, 0.5, 1.0) : clearAux;
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
	// In front of anything that is not ground the fog fades out over the end of the ray, so it never draws the
	// outline of what stands in it.
	float softScale = 1.0;
	if (wrap) {
		// Near its end the ray is over whatever the pixel shows, not over the mapped terrain. If that faces up it
		// is ground: the ground under the end of the ray is moved to it, so the fog lies on rocks, roads and
		// floors and thins out against them like against the terrain. If it does not, nothing moves.
		const float3 normal = GroundFog::SurfaceNormal(scenePixel, depth, position, mainSize, activeMain);
		taps += 4u;
		const float facingUp = smoothstep(0.5, 0.85, normal.z);
		softScale = lerp(0.5, 1.0, facingUp);
		if (facingUp > 0.0 && GroundFog::SurfaceBlend > 0.0) {
			const GroundFog::GroundSample endGround = GroundFog::GroundAt(endXY, 1.0);
			++taps;
			float above = position.z + camZ - endGround.z;
			// Under water the surface seen may be the bed; the fog stays on the water.
			above = endGround.water > 0.0 ? max(above, 0.0) : above;
			ray.surfaceOffset = above * GroundFog::SurfaceWeight(above) * facingUp;
			ray.blendLength = ray.surfaceOffset != 0.0 ? min(GroundFog::SurfaceBlend, tEnd) : 0.0;
			if (GroundFog::NearExtinction > 0.0 && tEnd < GroundFog::NearEnd) {
				// The near volume also follows level floors well above the map: roads on fill, bridges, town
				// floors. Low ones are taken as they are; a high one must go on around the pixel, unless it is
				// the level the player stands on.
				float known = facingUp * GroundFog::FloorWeight(above);
				const float needsSupport = smoothstep(GroundFog::SurfaceLift, 2.0 * GroundFog::SurfaceLift, above);
				if (known > 0.0 && needsSupport > 0.0 && abs(above - ray.floorEnd) > GroundFog::SurfaceLift)
					known *= lerp(1.0, GroundFog::FloorSupport(position, normal, mainSize, activeMain, taps), needsSupport);
				const float base = ray.floorEnd;
				ray.floorEnd = lerp(base, above, known);
				// A floor far from the level the ray came over is met over a longer stretch.
				ray.floorLength = min(GroundFog::SurfaceBlend + 4.0 * abs(ray.floorEnd - base), tEnd);
			}
		}
	}
	ray.tBlend = tEnd - ray.blendLength;
	ray.tFloor = tEnd - ray.floorLength;
	const float tSoft = softScale < 1.0 && GroundFog::SoftLength > 0.0 ? max(tEnd - GroundFog::SoftLength, 0.0) : tEnd;
	// Where the nearest of the four scene pixels lies along this ray.
	const float tNear = viewNear < viewFar * 0.98 ? min(sceneDistance * viewNear / viewFar, tEnd) : tEnd;
	const float4 stops = float4(ray.tBlend, ray.tFloor, tSoft, tNear);

	// The ray in height map uv, to find where it crosses the texel grid.
	const float2 uvStart = GroundFog::MapUv(0.0);
	const float2 uvStep = dir.xy * GroundFog::MapScaleOffset.xy;

	GroundFog::Joint a = GroundFog::MakeJoint(ray, 0.0, GroundFog::StepMin, taps);

	GroundFog::Seen seen;
	seen.tau = seen.share = seen.t = seen.z = seen.above = seen.valley = seen.shade = 0.0;
	float tauNear = -1.0;
	[loop] for (uint i = 0u; i < GroundFog::MaxSteps; ++i)
	{
		if (a.t >= tEnd - 0.01)
			break;
		if (tauNear < 0.0 && a.t >= tNear - 0.01)
			tauNear = seen.tau;
		// Above the fog: pass over the stretches of the ray that no ground under them reaches up to, each three
		// times as long as the one before, at one read each. A ray that looks up goes through to its end this way.
		if (!a.sheet.inside && !a.valley.inside && !a.near.inside && a.h > 0.0 && a.hn > 0.0) {
			float from = a.t;
			[loop] for (uint k = 0u; k < 10u && from < tEnd - 0.01; ++k)
			{
				const float to = min(max(3.0 * from, from + 4.0 * GroundFog::StepMin), tEnd);
				++taps;
				if (camZ + dir.z * (dir.z < 0.0 ? to : from) - GroundFog::GroundMaxAlong(dir.xy * from, dir.xy * to) <= GroundFog::SlabTop(from, to, lift, floorLift))
					break;
				from = to;
			}
			// Nothing was added on the way, so the fog in front of the nearest scene pixel is what there is now.
			if (tauNear < 0.0 && from >= tNear - 0.01)
				tauNear = seen.tau;
			if (from >= tEnd - 0.01)
				break;
			if (from > a.t)
				a = GroundFog::MakeJoint(ray, from, max(from * GroundFog::StepGrowth, GroundFog::StepMin), taps);
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
		}
		const GroundFog::Joint b = GroundFog::MakeJoint(ray, tb, max(tb * GroundFog::StepGrowth, GroundFog::StepMin), taps);

		const float scale = a.t >= tSoft - 0.01 ? softScale : 1.0;
		GroundFog::IntegrateNear(seen, a, b, scale);
		GroundFog::Integrate(seen, a, b, a.sheet, b.sheet, GroundFog::GroundExtinction * scale, GroundFog::GroundFill, false);
		GroundFog::Integrate(seen, a, b, a.valley, b.valley, GroundFog::FarExtinction, GroundFog::ValleyFill(), true);
		a = b;
		if (seen.tau > 9.0)
			break;
	}
	if (tauNear < 0.0)
		tauNear = seen.tau;

	if (debugTaps) {
		FogOut[pixel] = float4(GroundFog::TapColor(taps) * GroundFog::SkyMean() * 4.0, 0.5);
		FogAuxOut[pixel] = float4(auxDepths, 0.5, 1.0);
		return;
	}
	if (seen.share < 1.0 / 2048.0) {
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
	}

	const SharedData::PhysSkyData physSky = SharedData::physSkyData;
	float atmosphereVisibility;
	float3 diffuseVisibility;
	const float visibility = MediumVisibility(lightPos, atmosphereVisibility, diffuseVisibility);

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
	light *= GroundFog::SunScale;
	// Sky light reaches fog in the shadow of terrain and objects too, so it takes no direct visibility.
	light += albedo * GroundFog::SkyMean() * GroundFog::SkyAmbientScale * skyDimming;

	// A uniform medium of this light per unit of extinction; the composite multiplies by the opacity at the depth
	// of each of its pixels.
	FogOut[pixel] = float4(min(max(light, 0.0), 65504.0), exp(-seen.tau));
	FogAuxOut[pixel] = float4(auxDepths, exp(-tauNear), 1.0);
}
