// Ground Fog: the simulation. A small 2D model, around the camera, of the fog lying on the ground as a shallow
// fluid: how much there is at each place and how it moves. The pool is drawn from it (GroundFog/Pool.hlsli); the
// sheet and the tall near volume take from it how far the fog is from rest.
//
// The map is a square window of the world's xy plane, fixed to a world grid of TexelSize: as the camera moves the
// window steps by whole texels and the previous state is read with that step (Shift), so nothing swims or pops.
//
// State per texel (two textures of each, swapped every frame):
//   State  xy: velocity of the fog, game units per second.  z: amount (1 = the mean resting amount at Amount 1).
//          w: the resting amount at this texel, which z settles to.
//   Floor  the level of the floor, absolute z; FloorEmpty where the screen has not shown any yet.
//
// One step, one pass over the window, no pressure solve:
// 1. Carry: amount and velocity are fetched from where the fog at this texel came from (semi-Lagrangian, bilinear).
// 2. The amount changes by what flows together or apart (the divergence of the velocity), so fog pushed ahead of
//    something piles up and thins out behind it.
// 3. Forces on the velocity:
//    - it settles to the wind's drift, which is also how fast the resting pattern travels;
//    - downhill, along the slope of the floor map: fog drains off rises and steps and gathers in the low places;
//    - from thick to thin (the fog's own weight, as in shallow water): a hole closes by fog flowing in from its
//      rim, and a heap spreads;
//    - swirl: a divergence-free noise field, weak in still fog and stronger the faster the fog is moving, so
//      wakes break up into eddies;
//    - movers (actors, projectiles, impacts): around each the velocity is pulled toward the flow of an ideal
//      fluid around a moving cylinder: inside its radius the fog moves with it; outside it is pushed aside ahead
//      and drawn back in behind. Emitters with a burst add an outward ring (an impact). Each also stirs a share
//      of the fog away. Heat sources, spells and explosions are further emitters: see GatherSimEmitters.
// 4. Settle: the amount goes toward the resting amount over RefillTime. That is the one rule that places all of
//    the fog (GroundFog::RestingAmountAt: low ground, water, shade, time of day) times the drifting patches.
//    Without terrain data (SimFlags bit 0 clear) the resting amount is RestUniform: the hook for interiors.
// 5. Floor: the lowest up-facing surface the pixel pass saw in and around this texel last frame (FloorSeen) is
//    folded into the floor map. It drops to a lower sighting at once and rises to a higher one slowly, so a table
//    top or a shoulder passing over a texel does not become its floor. The slope of the floor over about a meter
//    is written out beside it (GroundFog::FogSlope) for the drawing of the pool.

#define COMPUTESHADER
#define PS_LINEAR_SHADOW_SAMPLER
#include "Common/Random.hlsli"
#include "Common/ViewAtmosphere.hlsli"
#include "PhysicalSky/Common.hlsli"
#include "ViewAtmosphere/MediumLighting.hlsli"

#define GROUND_FOG_SHADE
#include "GroundFog/GroundFog.hlsli"

struct Emitter
{
	float2 position;  // from the window's corner, game units
	float2 velocity;  // game units per second
	float radius;     // game units
	float burst;      // outward speed added once, game units per second
	float clear;      // share stirred away at its centre this step
	float pad;
};

static const uint MaxEmitters = 24u;

cbuffer SimCB : register(b3)
{
	int2 Shift;  // texels the window has moved since the previous state
	float TexelSize;
	float DeltaTime;
	uint EmitterCount;
	float VelocityKeep;  // share of the velocity relative to the wind kept over this step
	float RefillRate;    // share of the way to the resting amount gone over this step
	float ClearStrength;
	float2 Wind;     // the wind's drift, game units per second
	float Downhill;  // acceleration down a slope of 1, game units per second^2
	float Spread;    // acceleration per unit of amount gradient, game units^2 per second^2
	float2 Corner;   // camera-relative xy of the window's corner
	float Swirl;
	float SwirlTime;
	float RestUniform;  // resting amount where there is no terrain data
	uint SimFlags;      // bit 0: terrain data is bound (height map, water, terrain shadow)
	float FloorRise;    // share of the way up to a higher floor sighting gone over this step
	float SimPad;
	Emitter Emitters[MaxEmitters];
};

// GroundFog::FogSim (t8) and GroundFog::FogFloor (t10) are the previous state here.
Texture2D<float> FloorSeen : register(t12);
RWTexture2D<float4> StateOut : register(u0);
RWTexture2D<float> FloorOut : register(u1);
RWTexture2D<float4> SlopeOut : register(u2);

static const float MaxAmount = 6.0;
static const float MaxSpeed = 1500.0;
static const float SwirlSize = 140.0;  // game units per cell of the swirl noise (2 m)

[numthreads(8, 8, 1)] void main(uint2 pixel : SV_DispatchThreadID) {
	uint2 size;
	StateOut.GetDimensions(size.x, size.y);
	if (any(pixel >= size))
		return;
	const int2 last = int2(size) - 1;

	// Where this texel was in the previous state.
	const int2 before = int2(pixel) + Shift;
	const bool had = all(before >= 0) && all(before <= last);

	// What rests here.
	const float2 xy = Corner + (float2(pixel) + 0.5) * TexelSize;
	const float2 xyAbs = xy + FrameBuffer::CameraPosAdjust.xy;
	float rest = RestUniform;
	if (SimFlags & 1u)
		rest = GroundFog::RestingAmountAt(xy, TexelSize, GroundFog::BurnSunlitNear);
	rest *= GroundFog::DensityModifier(xyAbs, TexelSize);

	float2 velocity = Wind;
	float amount = rest;
	float level = GroundFog::FloorEmpty;
	float4 floorSlope = 0.0;
	if (had) {
		const int2 left = max(before - int2(1, 0), 0);
		const int2 right = min(before + int2(1, 0), last);
		const int2 down = max(before - int2(0, 1), 0);
		const int2 up = min(before + int2(0, 1), last);
		const float4 l = GroundFog::FogSim.Load(int3(left, 0));
		const float4 r = GroundFog::FogSim.Load(int3(right, 0));
		const float4 d = GroundFog::FogSim.Load(int3(down, 0));
		const float4 u = GroundFog::FogSim.Load(int3(up, 0));
		const float2 carried = GroundFog::FogSim.Load(int3(before, 0)).xy;

		// 1. Carry.
		const float2 from = clamp(float2(before) + 0.5 - carried * DeltaTime / TexelSize, 0.5, float2(size) - 0.5);
		const float4 state = GroundFog::FogSim.SampleLevel(LinearSampler, from / float2(size), 0);
		velocity = state.xy;
		amount = state.z;

		// 2. Flowing together or apart.
		const float divergence = ((r.x - l.x) + (u.y - d.y)) / (2.0 * TexelSize);
		amount *= clamp(1.0 - divergence * DeltaTime, 0.5, 1.5);

		// 3. Wind, and from thick to thin.
		velocity = Wind + (velocity - Wind) * VelocityKeep;
		// Only what is off its resting amount pushes: fog at rest is in balance, patches and all.
		velocity -= Spread * float2((r.z - r.w) - (l.z - l.w), (u.z - u.w) - (d.z - d.w)) / (2.0 * TexelSize) * DeltaTime;

		// Downhill, where the floor is known on both sides.
		level = GroundFog::FogFloor.Load(int3(before, 0));
		const float4 floors = float4(GroundFog::FogFloor.Load(int3(left, 0)), GroundFog::FogFloor.Load(int3(right, 0)),
			GroundFog::FogFloor.Load(int3(down, 0)), GroundFog::FogFloor.Load(int3(up, 0)));
		if (all(floors < GroundFog::FloorUnknown)) {
			float2 slope = float2(floors.y - floors.x, floors.w - floors.z) / (2.0 * TexelSize);
			// For the drawing of the pool: over three texels each way where the corners are known too, so single
			// texels and the facets of the ground do not show in it.
			const float4 corners = float4(GroundFog::FogFloor.Load(int3(left.x, down.y, 0)), GroundFog::FogFloor.Load(int3(right.x, down.y, 0)),
				GroundFog::FogFloor.Load(int3(left.x, up.y, 0)), GroundFog::FogFloor.Load(int3(right.x, up.y, 0)));
			float2 smooth = slope;
			if (all(corners < GroundFog::FloorUnknown))
				smooth = 0.5 * slope + 0.25 * float2((corners.y - corners.x) + (corners.w - corners.z), (corners.z - corners.x) + (corners.w - corners.y)) / (2.0 * TexelSize);
			floorSlope = float4(clamp(smooth, -2.0, 2.0), 1.0, 0.0);
			slope *= min(1.0, rsqrt(max(dot(slope, slope), 1e-6)));
			velocity -= Downhill * slope * DeltaTime;
		}
	}

	// Swirl: the curl of a slowly changing noise carried with the wind.
	if (Swirl > 0.0) {
		const float2 p = (xyAbs - GroundFog::WindTravel) / SwirlSize + SwirlTime * float2(0.031, -0.023);
		const float e = 0.35;
		const float2 gradient = float2(GroundFog::ValueNoise(p + float2(e, 0.0)) - GroundFog::ValueNoise(p - float2(e, 0.0)),
										GroundFog::ValueNoise(p + float2(0.0, e)) - GroundFog::ValueNoise(p - float2(0.0, e))) /
		                        (2.0 * e);
		// The share that grows with the speed stays below what the damping takes, so eddies cannot feed themselves.
		velocity += float2(gradient.y, -gradient.x) * Swirl * (6.0 + 0.25 * min(length(velocity - Wind), 300.0)) * DeltaTime;
	}

	// Movers.
	const float2 position = (float2(pixel) + 0.5) * TexelSize;
	[loop] for (uint i = 0u; i < min(EmitterCount, MaxEmitters); ++i)
	{
		const Emitter emitter = Emitters[i];
		const float2 offset = position - emitter.position;
		const float distance2 = dot(offset, offset) + 1.0;
		const float radius2 = emitter.radius * emitter.radius;
		const float2 outward = offset * rsqrt(distance2);
		const float nearness = min(radius2 / distance2, 1.0);
		if (dot(emitter.velocity, emitter.velocity) > 1.0) {
			const float2 around = nearness * (2.0 * dot(emitter.velocity, outward) * outward - emitter.velocity);
			velocity = lerp(velocity, distance2 <= radius2 ? emitter.velocity : around, saturate(nearness * 12.0 * DeltaTime));
		}
		const float bell = exp(-distance2 / radius2);
		velocity += outward * emitter.burst * sqrt(bell) * saturate(distance2 / radius2 * 4.0);
		amount *= saturate(1.0 - emitter.clear * ClearStrength * bell);
	}

	// 4. Settle.
	amount += (rest - amount) * RefillRate;
	amount = clamp(amount, 0.0, MaxAmount);
	velocity = clamp(velocity, -MaxSpeed, MaxSpeed);
	if (!(amount >= 0.0) || !(dot(velocity, velocity) < 1.0e9)) {
		amount = rest;
		velocity = Wind;
	}
	StateOut[pixel] = float4(velocity, amount, rest);

	// 5. Floor.
	float seen = GroundFog::FloorEmpty;
	if (had) {
		[unroll] for (int dy = -1; dy <= 1; ++dy)
		{
			[unroll] for (int dx = -1; dx <= 1; ++dx)
				seen = min(seen, FloorSeen.Load(int3(clamp(before + int2(dx, dy), 0, last), 0)));
		}
	}
	if (seen < GroundFog::FloorUnknown)
		level = level >= GroundFog::FloorUnknown || seen < level ? seen : level + (seen - level) * FloorRise;
	FloorOut[pixel] = level;
	SlopeOut[pixel] = floorSlope;
}
