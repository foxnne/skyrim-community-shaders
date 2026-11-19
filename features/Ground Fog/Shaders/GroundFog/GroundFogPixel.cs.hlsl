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
// Two layers share the march: the sheet (thin, everywhere, near the camera) and the valley layer (deeper, in
// hollows and over water, from where the sheet fades out to kilometers away).
//
// The output is light scattered toward the camera (rgb) and transmittance (a). ViewAtmosphere/Composite.cs.hlsl
// lays it over the scene before the atmosphere, upsampling by depth when this pass runs at half resolution.

#define COMPUTESHADER
#define PS_LINEAR_SHADOW_SAMPLER
#include "Common/Random.hlsli"
#include "Common/ViewAtmosphere.hlsli"
#include "PhysicalSky/Common.hlsli"
#include "ViewAtmosphere/MediumLighting.hlsli"

#include "GroundFog/GroundFog.hlsli"

Texture2D<float> SceneDepth : register(t40);
RWTexture2D<float4> FogOut : register(u0);
RWTexture2D<float> FogDepthOut : register(u1);  // the device depth each output pixel was computed for

namespace GroundFog
{
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
		Layer sheet;
		Layer valley;
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

	// How much a visible surface counts as the ground, by its height above the mapped ground. Rocks, roads, steps
	// and ground meshes do; roofs, bridges and tree crowns do not.
	float SurfaceWeight(float above)
	{
		if (SurfaceBlend <= 0.0 || SurfaceLift <= 0.0)
			return 0.0;
		return (1.0 - smoothstep(SurfaceLift, 2.0 * SurfaceLift, above)) * (1.0 - smoothstep(2.0 * SurfaceLift, 4.0 * SurfaceLift, -above));
	}

	// Hollows hold more, rises less. One read of the terrain averaged around the point.
	float Basin(float2 xy, float terrain)
	{
		const float hollow = clamp((GroundAround(xy, BasinRadius) - terrain) * InvBasinDepth, -1.0, 1.0);
		return clamp(1.0 + BasinStrength * hollow, 0.0, 1.0 + BasinStrength);
	}

	// No fog lies higher than this above the ground anywhere between two distances along the ray.
	float SlabTop(float from, float to, float lift)
	{
		float top = -1.0e30;
		if (from < FadeEnd)
			top = TopMax() + lift;
		if (FarExtinction > 0.0 && to > FadeStart)
			top = max(top, ValleyTopMax());
		return top;
	}

	// Fills in what a joint needs to hold fog. Joints clear of both layers cost nothing more than their ground read.
	void Shade(inout Joint joint, float2 xy, GroundSample ground, float reach, inout uint taps)
	{
		const float2 xyAbs = xy + FrameBuffer::CameraPosAdjust.xy;
		const float sheetFade = 1.0 - smoothstep(FadeStart, FadeEnd, joint.t);
		// The valley layer comes in exactly as the sheet goes out, so there is no ring where the sheet ends.
		const float valleyFade = FarExtinction > 0.0 ? (1.0 - sheetFade) * (1.0 - smoothstep(0.75 * FarEnd, FarEnd, joint.t)) : 0.0;
		joint.sheet.inside = sheetFade > 0.0 && joint.h < TopMax() && joint.h > -GroundFill - 64.0;
		joint.sheet.top = Thickness;
		joint.sheet.place = 0.0;
		joint.valley.inside = valleyFade > 0.0 && joint.h < ValleyTopMax() && joint.h > -ValleyFill() - 64.0;
		joint.valley.top = FarThickness;
		joint.valley.place = 0.0;
		if (!joint.sheet.inside && !joint.valley.inside)
			return;
		const float common = MapCoverage(MapUv(xy)) * DensityModifier(xyAbs);
		if (joint.sheet.inside) {
			float place = common * sheetFade * lerp(1.0, WaterPreference, ground.water);
			if (place > 0.0 && BasinStrength > 0.0) {
				place *= Basin(xy, ground.terrain);
				++taps;
			}
			joint.sheet.place = place;
			joint.sheet.top = LayerTop(xyAbs, reach);
		}
		if (joint.valley.inside) {
			// Only where the land lies lower than the country around it, or on water: pools in basins and on
			// lakes, nothing on rises and open slopes.
			const float hollow = saturate((GroundAround(xy, FarBasinRadius) - ground.terrain) * InvFarBasinDepth);
			++taps;
			const float pool = smoothstep(0.0, 1.0, max(hollow, ground.water));
			// Broad banks, travelling with the wind like the sheet's.
			const float bank = lerp(0.7, 1.25, ValueNoise((xyAbs - WindTravel) * InvBankScale * 0.25 + 7.3));
			joint.valley.place = common * valleyFade * pool;
			joint.valley.top = max(FarThickness * (0.35 + 0.65 * pool) * bank, 1.0);
		}
	}

	// Adds one layer's share of the segment between two joints.
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
		const float share = exp(-seen.tau) * OneMinusExp(tau);
		// Lit at the middle of the segment, but inside the layer even where the ray itself is under the ground
		// or above the top.
		const float height = clamp(0.5 * (a.h + b.h), 0.15 * top, top);
		seen.share += share;
		seen.t += share * 0.5 * (a.t + b.t);
		seen.z += share * (0.5 * (a.ground + b.ground) + height);
		seen.above += share * extinction * place * profile.scale * (exp(-height / profile.scale) - exp(-top / profile.scale));
		seen.valley += valley ? share : 0.0;
		seen.tau += tau;
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

	// At half resolution one of the four scene pixels stands for the output pixel: the nearest and the farthest
	// in a checkerboard, so both sides of a depth edge are there for the upsample to pick from.
	uint2 scenePixel = pixel;
	float depth = SceneDepth[pixel];
	if (halfResolution) {
		const bool nearest = ((pixel.x + pixel.y) & 1u) == 0u;
		scenePixel = min(pixel * 2u, activeMain - 1u);
		depth = SceneDepth[scenePixel];
		[unroll] for (uint i = 1u; i < 4u; ++i)
		{
			const uint2 candidate = min(pixel * 2u + uint2(i & 1u, i >> 1u), activeMain - 1u);
			const float candidateDepth = SceneDepth[candidate];
			const bool nearer = FrameBuffer::ToStandardDepth(candidateDepth) < FrameBuffer::ToStandardDepth(depth);
			if (nearer == nearest && candidateDepth != depth) {
				depth = candidateDepth;
				scenePixel = candidate;
			}
		}
	}
	FogDepthOut[pixel] = depth;

	// The same reconstruction as the atmosphere composite: the jittered projection the depth was drawn with.
	const float2 uv = FrameBuffer::GetDynamicResolutionUnadjustedScreenPosition((float2(scenePixel) + 0.5) / float2(mainSize));
	float4 position = mul(FrameBuffer::CameraViewProjInverse, float4(uv * float2(2, -2) + float2(-1, 1), depth, 1));
	position.xyz /= position.w;
	const bool sky = FrameBuffer::ToStandardDepth(depth) >= 1.0 - 1e-6;
	const float sceneDistance = length(position.xyz);
	const float3 dir = position.xyz / max(sceneDistance, 1e-6);
	const float range = GroundFog::FarExtinction > 0.0 ? max(GroundFog::FarEnd, GroundFog::FadeEnd) : GroundFog::FadeEnd;
	const bool surface = !sky && sceneDistance < range;
	const float tEnd = surface ? sceneDistance : range;
	const float2 endXY = dir.xy * tEnd;
	const float camZ = FrameBuffer::CameraPosAdjust.z;
	// The surface wrap (below) is for surfaces within the sheet's range; it lifts the sheet by less than this.
	const bool wrap = surface && tEnd < GroundFog::FadeEnd;
	const float lift = wrap ? 2.0 * GroundFog::SurfaceLift : 0.0;
	const bool debugTaps = GroundFog::DebugMode == 4u;
	const float4 clear = float4(0.0, 0.0, 0.0, 1.0);

	// The whole ray against the highest ground under it: one read settles short rays that end well above the ground.
	uint taps = 1u;
	if (camZ + min(dir.z * tEnd, 0.0) - GroundFog::GroundMaxAlong(0.0, endXY) > GroundFog::SlabTop(0.0, tEnd, lift)) {
		FogOut[pixel] = debugTaps ? float4(GroundFog::TapColor(taps) * GroundFog::SkyMean() * 2.0, 0.5) : clear;
		return;
	}

	// Near its end the ray is over whatever the pixel shows, not over the mapped terrain. The ground under the last
	// SurfaceBlend of the ray is moved toward the surface, fully at the surface itself, so the fog lies on rocks,
	// roads and meshes and thins out against them like against the terrain.
	float surfaceOffset = 0.0;
	float blendLength = 0.0;
	if (wrap) {
		const GroundFog::GroundSample endGround = GroundFog::GroundAt(endXY, 1.0);
		++taps;
		float above = position.z + camZ - endGround.z;
		// Under water the surface seen may be the bed; the fog stays on the water.
		above = endGround.water > 0.0 ? max(above, 0.0) : above;
		surfaceOffset = above * GroundFog::SurfaceWeight(above);
		blendLength = surfaceOffset != 0.0 ? min(GroundFog::SurfaceBlend, tEnd) : 0.0;
	}
	const float tBlend = tEnd - blendLength;

	// The ray in height map uv, to find where it crosses the texel grid.
	const float2 uvStart = GroundFog::MapUv(0.0);
	const float2 uvStep = dir.xy * GroundFog::MapScaleOffset.xy;

	GroundFog::Joint a;
	a.t = 0.0;
	{
		const GroundFog::GroundSample ground = GroundFog::GroundAt(0.0, GroundFog::StepMin);
		++taps;
		a.ground = ground.z;
		a.h = camZ - a.ground;
		GroundFog::Shade(a, 0.0, ground, GroundFog::StepMin, taps);
	}

	GroundFog::Seen seen;
	seen.tau = seen.share = seen.t = seen.z = seen.above = seen.valley = 0.0;
	[loop] for (uint i = 0u; i < GroundFog::MaxSteps; ++i)
	{
		if (a.t >= tEnd - 0.01)
			break;
		// Above the fog: pass over the stretches of the ray that no ground under them reaches up to, each three
		// times as long as the one before, at one read each. A ray that looks up goes through to its end this way.
		if (!a.sheet.inside && !a.valley.inside && a.h > 0.0) {
			float from = a.t;
			[loop] for (uint k = 0u; k < 10u && from < tEnd - 0.01; ++k)
			{
				const float to = min(max(3.0 * from, from + 4.0 * GroundFog::StepMin), tEnd);
				++taps;
				if (camZ + dir.z * (dir.z < 0.0 ? to : from) - GroundFog::GroundMaxAlong(dir.xy * from, dir.xy * to) <= GroundFog::SlabTop(from, to, lift))
					break;
				from = to;
			}
			if (from >= tEnd - 0.01)
				break;
			if (from > a.t) {
				const float reach = max(from * GroundFog::StepGrowth, GroundFog::StepMin);
				const GroundFog::GroundSample ground = GroundFog::GroundAt(dir.xy * from, reach);
				++taps;
				a.t = from;
				a.ground = ground.z + surfaceOffset * saturate((from - tBlend) / max(blendLength, 1e-3));
				a.h = camZ + dir.z * from - a.ground;
				GroundFog::Shade(a, dir.xy * from, ground, reach, taps);
			}
		}

		// The next joint: where the ray next crosses the texel grid of the ground it is over. The grid is fixed in
		// the world and coarsens with distance. Where the surface wrap starts there is a joint too.
		GroundFog::Joint b;
		{
			const float2 cells = GroundFog::GroundCells(max(a.t * GroundFog::StepGrowth, GroundFog::StepMin));
			const float from = a.t + 0.25;
			const float2 cell = (uvStart + uvStep * from) * cells - 0.5;
			const float2 rate = uvStep * cells;
			const float2 next = float2(rate.x > 0.0 ? floor(cell.x) + 1.0 : floor(cell.x), rate.y > 0.0 ? floor(cell.y) + 1.0 : floor(cell.y));
			const float2 crossing = float2(abs(rate.x) > 1e-12 ? (next.x - cell.x) / rate.x : 1.0e30, abs(rate.y) > 1e-12 ? (next.y - cell.y) / rate.y : 1.0e30);
			b.t = from + max(min(crossing.x, crossing.y), 0.0) + 0.25;
			const float stop = a.t < tBlend - 0.01 ? tBlend : tEnd;
			b.t = min(b.t, stop);
		}
		const float reach = max(b.t * GroundFog::StepGrowth, GroundFog::StepMin);
		const float2 xy = dir.xy * b.t;
		const GroundFog::GroundSample ground = GroundFog::GroundAt(xy, reach);
		++taps;
		b.ground = ground.z + surfaceOffset * saturate((b.t - tBlend) / max(blendLength, 1e-3));
		b.h = camZ + dir.z * b.t - b.ground;
		GroundFog::Shade(b, xy, ground, reach, taps);

		GroundFog::Integrate(seen, a, b, a.sheet, b.sheet, GroundFog::GroundExtinction, GroundFog::GroundFill, false);
		GroundFog::Integrate(seen, a, b, a.valley, b.valley, GroundFog::FarExtinction, GroundFog::ValleyFill(), true);
		a = b;
		if (seen.tau > 9.0)
			break;
	}

	if (debugTaps) {
		FogOut[pixel] = float4(GroundFog::TapColor(taps) * GroundFog::SkyMean() * 2.0, 0.5);
		return;
	}
	if (seen.share < 1.0 / 2048.0) {
		FogOut[pixel] = clear;
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
		const float valley = seen.valley / seen.share;
		albedo = 0.08 + 0.92 * float3(valley, 1.0 - valley, GroundFog::GroundAt(lightPos.xy, 1.0).water);
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

	// A uniform medium of this light per unit of extinction, seen through its own optical depth.
	FogOut[pixel] = float4(min(max(light, 0.0) * seen.share, 65504.0), 1.0 - seen.share);
}
