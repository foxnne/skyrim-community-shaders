// Ground Fog: the pool's volume. A 3D texture over the simulation's window that follows the floor: texel (x, y) is
// the simulation's texel (x, y), fixed in the world, and level k is the height (k + 0.5) / levels * VolumeHeight
// above the floor there. Filled once per frame, after the simulation has stepped, one thread per column, top down:
//   rgb: x  extinction, per meter (the march divides by the units per meter)
//        y  how much of the sun reaches the point: the sun map (the shadow cascades at the pool's heights over the
//           texel, eased over time), through the fog above it toward the sun
//        z  how much of the sky's light reaches it: less low down in thick fog
//   VolumeFloorOut: the floor the column stands on, absolute z (the floor map where the screen has shown it, else
//        the terrain height map, else the level of the camera's feet).
// The pixel pass marches every pixel's ray through it to the pixel's own depth (GroundFog/PoolVolume.hlsli), so a
// blade of grass, a leg or a trunk gets exactly the fog between the camera and it, the fog shows against the sky at
// knee height, and the layer has parallax. Nothing in it depends on the camera's view: only on the world, the
// simulation and the time.
//
// The fog's shapes are 3D noise carried by the simulation's flow (a flow map in two phases: Valve's water flow,
// after Vlachos 2010): where the fog moves against the air the noise streams with it and wakes churn in height as
// well; where it rests the noise stands still, so settled fog is still. The density profile, the soft top, the
// lumps and the height cut are the pool's (GroundFog/Pool.hlsli). The idea of a world-space fog volume over a
// ground that nothing standing on it moves, marched per pixel to the pixel's depth, is Iwillbewarlock's GroundFog
// (GPL-3.0, github.com/Iwillbewarlock/GroundFog, RaymarchShaders.h MarchPixel at 0e519e7); no code is shared.

#define COMPUTESHADER
#define PS_LINEAR_SHADOW_SAMPLER
#include "Common/Random.hlsli"
#include "Common/ViewAtmosphere.hlsli"
#include "PhysicalSky/Common.hlsli"
#include "ViewAtmosphere/MediumLighting.hlsli"

#define GROUND_FOG_VOLUME_FILL
#include "GroundFog/GroundFog.hlsli"
#include "GroundFog/Pool.hlsli"
#include "GroundFog/PoolVolume.hlsli"

// The start of the simulation's constants (GroundFog/FlowSim.cs.hlsl, SimCB), bound here too: the wind's drift.
cbuffer SimCB : register(b3)
{
	int2 SimShift;
	float SimTexelSize;
	float SimDeltaTime;
	uint SimEmitterCount;
	float SimVelocityKeep;
	float SimRefillRate;
	float SimClearStrength;
	float2 SimWind;  // game units per second
};

RWTexture3D<float4> VolumeOut : register(u0);
RWTexture2D<float> VolumeFloorOut : register(u1);

namespace GroundFog
{
	// The flow map: seconds per phase, and the most a phase may carry the noise (game units), so a fast blow
	// does not smear it to streaks.
	static const float FlowPeriod = 1.6;
	static const float FlowReachMax = 140.0;

	float VolumeNoise(float3 p)
	{
		return 0.65 * ValueNoise3(p) + 0.35 * ValueNoise3(p * 2.3 + 17.0);
	}

	// The pool's 3D texture (PoolTexture) carried by the flow: stir is the fog's velocity against the air.
	float VolumeTexture(float2 xyAbs, float h, float top, float2 stir, float time)
	{
		if (Wispiness <= 0.0)
			return 1.0;
		const float phase = frac(time / FlowPeriod);
		const float phase2 = frac(phase + 0.5);
		const float speed = length(stir);
		const float2 along = speed > 1e-3 ? stir * min(1.0, FlowReachMax / (speed * FlowPeriod)) : 0.0;
		// Stirred fog churns upward a little as it is carried: wakes roll in height, not only across.
		const float rise = min(speed, 200.0) * 0.25;
		const float3 base = float3(xyAbs - WindTravel, 1.6 * h);
		const float3 a = (base - float3(along, -rise) * (phase * FlowPeriod)) / PoolVoxel;
		const float3 b = (base - float3(along, -rise) * (phase2 * FlowPeriod)) / PoolVoxel + 7.31;
		const float weight = 1.0 - abs(2.0 * phase - 1.0);
		const float n = lerp(VolumeNoise(b), VolumeNoise(a), weight);
		// Two phases blended are less contrasted than one: give back what the blend takes (about 1.4 at the middle).
		const float spread = rsqrt(weight * weight + (1.0 - weight) * (1.0 - weight));
		const float sharp = 0.5 + (n - 0.5) * lerp(1.0, spread, saturate(speed / 20.0));
		const float cut = lerp(0.22, 0.6, saturate(h / max(top, 1.0)));
		return lerp(1.0, 1.7 * smoothstep(cut, cut + 0.3, sharp), Wispiness);
	}
}

[numthreads(8, 8, 1)] void main(uint2 texel : SV_DispatchThreadID) {
	uint3 size;
	VolumeOut.GetDimensions(size.x, size.y, size.z);
	if (any(texel >= size.xy))
		return;
	const float2 uv = (float2(texel) + 0.5) / float2(size.xy);
	const float2 xy = (uv - GroundFog::FlowWindow.xy) / GroundFog::FlowWindow.z;
	const float2 xyAbs = xy + FrameBuffer::CameraPosAdjust.xy;
	const int3 at = int3(texel, 0);

	// The floor.
	float floorZ = GroundFog::FogFloor.Load(at);
	float floorness = 1.0;
	const float4 slope = GroundFog::FogSlope.Load(at);
	bool known = floorZ < GroundFog::FloorUnknown;
	if (known)
		floorness = saturate(slope.z);
	else if (GroundFog::MapCoverage(GroundFog::MapUv(xy)) > 0.5) {
		floorZ = GroundFog::GroundAt(xy, GroundFog::StepMin).z;
		known = true;
	} else
		floorZ = FrameBuffer::CameraPosAdjust.z - 120.0;
	VolumeFloorOut[texel] = floorZ;

	const float4 state = GroundFog::FogSim.Load(at);
	float amount = known ? clamp(state.z, 0.0, GroundFog::PoolAmountMax) : 0.0;
	const float2 stir = state.xy - SimWind;
	const float lift = clamp(GroundFog::FogLift.Load(at), -0.6, 3.0);
	const float thick = GroundFog::PoolThickness * (1.0 + lift);
	const float limit = GroundFog::PoolHardTop * clamp(1.0 + lift, 1.0, GroundFog::PoolPlumeMax);

	// The lumps of the top and the fine wisps across, as in the pool (EvaluatePool).
	const float2 g = (xyAbs - GroundFog::WindTravel - stir * GroundFog::PoolGrainDrag) / GroundFog::PoolGrain;
	const float grain = 0.6 * GroundFog::ValueNoise(g) + 0.4 * GroundFog::ValueNoise(g * 2.9 + 31.0);
	amount = max(amount + (grain - 0.5) * 1.8 * min(amount, 0.7) * GroundFog::Wispiness, 0.0);
	const float shape = lerp(0.7, 1.3, grain);
	const float breath = 1.0 + (GroundFog::Breathing(xyAbs) - 1.0) * GroundFog::PoolBreathShare;
	const float top = max(min(thick, limit) * breath * lerp(0.3, 1.0, smoothstep(0.05, 1.3, amount)) * shape, 1.0);
	const GroundFog::SoftProfile profile = GroundFog::MakeSoftProfile(top, 0.0, GroundFog::NearFalloff);
	const float extinction = GroundFog::PoolExtinction * GroundFog::PoolDensityScale * amount * floorness * rsqrt(1.0 + max(lift, 0.0)) / shape;

	const float sun = saturate(slope.w);
	const float sunZ = max(SharedData::DirLightDirection.z, 0.15);
	const float time = SharedData::Timer;
	const float dz = GroundFog::VolumeHeight / float(size.z);
	float above = 0.0;  // optical depth of the column above, straight up
	[loop] for (int k = int(size.z) - 1; k >= 0; --k)
	{
		const float h = (float(k) + 0.5) * dz;
		float density = 0.0;
		if (extinction > 0.0 && h < limit && h < top)
			density = extinction * GroundFog::SoftDensity(profile, h) * GroundFog::VolumeTexture(xyAbs, h, top, stir, time);
		const float here = above + 0.5 * density * dz;
		const float sunReach = sun * exp(-GroundFog::SelfShadow * here / sunZ);
		const float skyReach = lerp(1.0, exp(-0.7 * here), 0.5);
		VolumeOut[uint3(texel, uint(k))] = float4(density * GroundFog::VolumeUnitsPerMeter, sunReach, skyReach, 0.0);
		above += density * dz;
	}
}
