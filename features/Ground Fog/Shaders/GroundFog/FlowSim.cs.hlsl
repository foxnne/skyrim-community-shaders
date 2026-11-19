// Ground Fog: the reactive flow map. A small 2D simulation around the camera of how things moving through the fog
// push it about: the player, other actors, projectiles, and whatever ends a projectile's flight.
//
// The map is a square window of the world's xy plane, fixed to a world grid of TexelSize: as the camera moves the
// window steps by whole texels and the previous state is read with that step (Shift), so nothing swims or pops.
// It knows nothing about the ground: height maps, floors or interiors make no difference to it. The fog's own
// ground source decides where there is fog; this only says how it has been moved there.
//
// State per texel, in two textures that are swapped every frame:
//   Flow   xy: velocity of the fog, game units per second.  z: share of the fog cleared away.
//   Shift  xy: how far the fog has been displaced from rest, game units.  z: the same cleared share.
// The pixel pass reads Shift alone (GroundFog::Flow): it moves its pattern lookups by xy and thins by z.
//
// One step, one pass, no pressure solve:
// 1. Carry: the state is fetched from where the fog at this texel came from (semi-Lagrangian, bilinear, which
//    also spreads it a little), then decays: velocity over about a second, displacement and clearing over a few.
// 2. Movers: around each emitter the velocity is pulled toward the flow of an ideal fluid around a moving
//    cylinder: inside its radius the fog moves with it; outside, it is pushed aside ahead and drawn back in
//    behind, falling off with the square of distance. That is what parts the fog in front of a runner and closes
//    it behind. Emitters with a burst add an outward ring instead (an impact). Each also clears a share.
// 3. Displacement integrates the velocity and is limited.

struct Emitter
{
	float2 position;  // from the window's corner, game units
	float2 velocity;  // game units per second
	float radius;     // game units
	float burst;      // outward speed added once, game units per second
	float clear;      // share cleared at its centre this step
	float pad;
};

static const uint MaxEmitters = 24u;

cbuffer FlowCB : register(b3)
{
	int2 Shift;  // texels the window has moved since the previous state
	float TexelSize;
	float DeltaTime;
	uint EmitterCount;
	float VelocityKeep;
	float DisplacementKeep;
	float ClearKeep;
	float MaxDisplacement;
	float3 Pad;
	Emitter Emitters[MaxEmitters];
};

Texture2D<float4> PreviousFlow : register(t0);
Texture2D<float4> PreviousShift : register(t1);
RWTexture2D<float4> FlowOut : register(u0);
RWTexture2D<float4> ShiftOut : register(u1);
SamplerState LinearSampler : register(s0);

[numthreads(8, 8, 1)] void main(uint2 pixel : SV_DispatchThreadID) {
	uint2 size;
	FlowOut.GetDimensions(size.x, size.y);
	if (any(pixel >= size))
		return;

	// Where this texel was in the previous state.
	const float2 here = float2(int2(pixel) + Shift) + 0.5;
	float2 velocity = 0.0;
	float2 displacement = 0.0;
	float cleared = 0.0;
	if (all(here > 0.0) && all(here < float2(size))) {
		const float2 carried = PreviousFlow.SampleLevel(LinearSampler, here / float2(size), 0).xy;
		const float2 from = here - carried * DeltaTime / TexelSize;
		if (all(from > 0.5) && all(from < float2(size) - 0.5)) {
			const float4 flow = PreviousFlow.SampleLevel(LinearSampler, from / float2(size), 0);
			const float4 shift = PreviousShift.SampleLevel(LinearSampler, from / float2(size), 0);
			velocity = flow.xy * VelocityKeep;
			cleared = flow.z * ClearKeep;
			displacement = shift.xy * DisplacementKeep;
		}
	}

	const float2 position = (float2(pixel) + 0.5) * TexelSize;
	[loop] for (uint i = 0u; i < min(EmitterCount, MaxEmitters); ++i)
	{
		const Emitter emitter = Emitters[i];
		const float2 r = position - emitter.position;
		const float distance2 = dot(r, r) + 1.0;
		const float radius2 = emitter.radius * emitter.radius;
		const float2 outward = r * rsqrt(distance2);
		const float nearness = min(radius2 / distance2, 1.0);
		if (dot(emitter.velocity, emitter.velocity) > 1.0) {
			const float2 around = nearness * (2.0 * dot(emitter.velocity, outward) * outward - emitter.velocity);
			velocity = lerp(velocity, distance2 <= radius2 ? emitter.velocity : around, saturate(nearness * 12.0 * DeltaTime));
		}
		const float bell = exp(-distance2 / radius2);
		velocity += outward * emitter.burst * sqrt(bell) * saturate(distance2 / radius2 * 4.0);
		cleared += emitter.clear * bell;
	}

	displacement += velocity * DeltaTime;
	const float length2 = dot(displacement, displacement);
	if (length2 > MaxDisplacement * MaxDisplacement)
		displacement *= MaxDisplacement * rsqrt(length2);
	cleared = saturate(cleared);
	velocity = clamp(velocity, -2000.0, 2000.0);

	FlowOut[pixel] = float4(velocity, cleared, 0.0);
	ShiftOut[pixel] = float4(displacement, cleared, 0.0);
}
