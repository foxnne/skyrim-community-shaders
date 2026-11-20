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
// 4. Settle: the amount goes toward the resting amount over RefillTime. With Pooling the resting amount follows
//    the floor map at the scale of a few strides: a dip, a gutter or the foot of a slope holds the most, level open
//    ground a little, a rise none. So the settling keeps what the downhill flow gathers instead of evening it out. A floor that stands above the lowest floor
//    around it (the top of a wall, a cart, a rock, a steep bank) holds none. That is the one rule that places all of
//    the fog (GroundFog::RestingAmountAt: low ground, water, shade, time of day) times the drifting patches.
//    Without terrain data (SimFlags bit 0 clear) the resting amount is RestUniform: the hook for interiors.
// 5. Floor: the lowest up-facing surface the pixel pass saw in this texel last frame (FloorSeen) is
//    folded into the floor map. It drops to a lower sighting at once and rises to a higher one slowly, so a table
//    top or a shoulder passing over a texel does not become its floor; differences of a few units are left alone,
//    so with the camera still the map does not change from frame to frame. Each texel takes its own sightings; the
//    neighbours' only fill a texel nothing has been seen in. What is a raised thing (the floor stands well above the
//    lowest floor around it) is marked for the pool in GroundFog::FogSlope.z. The slope of the floor over about a meter
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
	float push;       // rate at which it pushes the fog from its middle to its rim, per second
	float2 kick;      // velocity added once at its centre, game units per second (a swing, a shout)
	float lift;       // how far it throws the top of the fog up at its centre, once, as a share of the pool's height
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
	float Pooling;  // 0: the fog rests evenly; 1: in the low places of the floor, little on level ground, none on rises
	float SunRate;   // share of the way to this step's sun visibility gone over this step
	float LayerTop;  // thickness of the pool, game units
	float GustSpeed;  // speed of the gusts, game units per second
	float GustSize;   // game units per cell of the gust noise
	float LiftKeep;   // share of the lift kept over this step
	float3 SimPad;
	Emitter Emitters[MaxEmitters];
};

// GroundFog::FogSim (t8), GroundFog::FogFloor (t10) and GroundFog::FogSlope (t11) are the previous state here.
Texture2D<float> FloorSeen : register(t12);
RWTexture2D<float4> StateOut : register(u0);
RWTexture2D<float> FloorOut : register(u1);
RWTexture2D<float4> SlopeOut : register(u2);
// GroundFog::FogLift (t13) is the previous lift.
RWTexture2D<float> LiftOut : register(u3);

static const float MaxAmount = 6.0;
// A wake (the clear a mover leaves) does not take the fog under this share of what rests there.
static const float WakeFloor = 0.15;
// The most a place holds relative to what rests there, and at least this much room for fog passing through.
static const float MaxPile = 3.0;
static const float MinRoom = 0.2;
// A floor this far above the lowest floor within a stride or so of it is a raised thing (the top of a wall, a cart,
// a rock, a steep bank): the fog does not rest on it. Game units; between the two it thins.
static const float RaisedStart = 25.0;
static const float RaisedEnd = 60.0;
// The floor map follows a lower sighting at once and a higher one slowly, but only beyond these (game units), so
// that with the camera still it comes to rest and stays.
// Pooling: what level ground holds of the even amount, the mean rise of the floor around a place (game units) that
// adds the whole even amount to it (and a place that much above its surroundings holds none), and the most.
// Level open ground holds little (RestFlat of the even amount: a few thin wisps), and the fog gathers where the
// floor lies lower than the floor around it: the mean rise of the floor 2, 4 and 8 m out (RestDepth of it gives the
// whole even amount more), and of the terrain map about 6 m out (RestDepthMap). A rise holds none.
static const float RestFlat = 0.12;
static const float RestDepth = 25.0;
static const float RestDepthMap = 50.0;
static const float RestMax = 1.8;
static const float FloorDropSlack = 1.0;
static const float FloorRiseSlack = 8.0;
static const float MaxSpeed = 1500.0;
static const float SwirlSize = 140.0;  // game units per cell of the swirl noise (2 m)

float GustPsi(float2 p)
{
	return (2.0 * GroundFog::ValueNoise(p) + GroundFog::ValueNoise(p * 2.1 + 17.0)) / 3.0;
}

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
	float water = 0.0;
	float groundZ = FrameBuffer::CameraPosAdjust.z - 120.0;
	// Where the sun map says the sun does not reach (trees, buildings, as well as terrain), the fog is kept as in
	// terrain shadow; in the sun it burns off. The map moves with the leaves; the amount follows it over SettleTime.
	const float shaded = had ? 1.0 - saturate(GroundFog::FogSlope.Load(int3(before, 0)).w) : 0.0;
	if (SimFlags & 1u)
		rest = GroundFog::RestingAmountAt(xy, TexelSize, GroundFog::BurnSunlitNear, shaded, water, groundZ);
	const float open = rest;

	// What the air does here with nothing in it: the wind's drift, and gusts. The gusts are the curl of a noise
	// that travels with the wind (a stream function, as in Iwillbewarlock's GroundFog, GustPsi in its
	// WindShaders.h, GPL-3.0: two octaves, weights 2/3 and 1/3), so they turn the fog about without piling it up.
	float2 drift = Wind;
	if (Swirl > 0.0 && GustSpeed > 0.0) {
		const float2 p = (xyAbs - GroundFog::WindTravel) / GustSize + SwirlTime * float2(0.011, -0.007);
		const float e = 0.25;
		const float2 gradient = float2(GustPsi(p + float2(e, 0.0)) - GustPsi(p - float2(e, 0.0)), GustPsi(p + float2(0.0, e)) - GustPsi(p - float2(0.0, e))) / (2.0 * e);
		drift += float2(gradient.y, -gradient.x) * (Swirl * GustSpeed);
	}

	float2 velocity = drift;
	// The fog is carried as its departure from what rests here (as in Iwillbewarlock's GroundFog, CSAdvectFog): the
	// resting pattern rides the wind exactly, and only the departure goes through the resampling, so wind alone
	// does not blur the wisps, and air that moves against the wind drags them along.
	float delta = 0.0;
	float gain = 1.0;
	float wear = 1.0;
	// The top of the fog: thrown up where it is struck, raised in the rim beside a track, pressed down in a wake.
	// Carried with the fog, and it settles back.
	float lift = 0.0;
	float level = GroundFog::FloorEmpty;
	float4 floorSlope = 0.0;
	float raised = 0.0;
	float lowness = RestFlat;
	float edge = 0.0;  // how steeply what rests changes here: the rim of a pool
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
		edge = saturate(length(float2(r.w - l.w, u.w - d.w)) * 2.0);

		// 1. Carry.
		const float2 from = clamp(float2(before) + 0.5 - carried * DeltaTime / TexelSize, 0.5, float2(size) - 0.5);
		const float4 state = GroundFog::FogSim.SampleLevel(LinearSampler, from / float2(size), 0);
		velocity = state.xy;
		delta = state.z - state.w;
		lift = GroundFog::FogLift.SampleLevel(LinearSampler, from / float2(size), 0) * LiftKeep;
		delta -= dot(velocity - Wind, float2(r.w - l.w, u.w - d.w)) / (2.0 * TexelSize) * DeltaTime;

		// 2. Flowing together or apart.
		const float divergence = ((r.x - l.x) + (u.y - d.y)) / (2.0 * TexelSize);
		gain *= clamp(1.0 - divergence * DeltaTime, 0.5, 1.5);

		// 3. Wind and gusts, and from thick to thin.
		velocity = drift + (velocity - drift) * VelocityKeep;
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
			floorSlope.xyz = float3(clamp(smooth, -2.0, 2.0), 1.0);
			// The fog runs down slopes. A step or a wall is not a slope: nothing accelerates through it, or the
			// fog would pile up without bound at its foot.
			slope *= 1.0 - smoothstep(0.35, 0.8, length(slope));
			velocity -= Downhill * slope * DeltaTime;
		}

		// How far this floor stands above the lowest floor around it, three texels out. A raised thing (a cart,
		// a table, a wall top, a big rock) stands above it everywhere within a texel of this one: the lowest floor
		// within 0.6 m is what is compared. Grass, ferns and plants are not raised things: their tops are seen at
		// all heights, and within 0.6 m of any of them there is a lower one or the ground. (Small things, a barrel,
		// a stump, are not marked either; the pool's height rule keeps fog off their tops.)
		if (level < GroundFog::FloorUnknown) {
			static const int2 ring[8] = { int2(3, 0), int2(-3, 0), int2(0, 3), int2(0, -3), int2(2, 2), int2(-2, 2), int2(2, -2), int2(-2, -2) };
			float lowest = min(min(floors.x, floors.y), min(floors.z, floors.w));
			[unroll] for (uint k = 0u; k < 8u; ++k)
				lowest = min(lowest, GroundFog::FogFloor.Load(int3(clamp(before + ring[k], 0, last), 0)));
			const float4 corners = float4(GroundFog::FogFloor.Load(int3(left.x, down.y, 0)), GroundFog::FogFloor.Load(int3(right.x, down.y, 0)),
				GroundFog::FogFloor.Load(int3(left.x, up.y, 0)), GroundFog::FogFloor.Load(int3(right.x, up.y, 0)));
			const float near = min(level, min(min(min(floors.x, floors.y), min(floors.z, floors.w)), min(min(corners.x, corners.y), min(corners.z, corners.w))));
			raised = smoothstep(RaisedStart, RaisedEnd, near - lowest);
			// And a floor far above the terrain is not one the fog lies on, however wide it is: a roof seen from
			// above, the top of a cliff of rock meshes. Floors up to Floor Max Height over the terrain map (roads on
			// fill, bridges, town floors; at least 2 m) still count.
			if (SimFlags & 1u) {
				const float floorMax = max(GroundFog::FloorMax, 140.0);
				raised = max(raised, smoothstep(floorMax, floorMax + 105.0, level - groundZ));
			}
			// For the drawing of the pool: z says how much this texel is a floor the fog lies on (known, and not
			// the top of a raised thing), and the slope is weighted by it.
			floorSlope.xyz = float3(floorSlope.xy, 1.0) * (1.0 - raised);

			// How low this floor lies among the floor around it, 2, 4 and 8 m out: the mean of how far the floor
			// there stands above it. What stands far above it is a wall or a thing, not ground, and says nothing;
			// what lies far below counts as a step down and no more.
			static const int2 around[24] = { int2(5, 0), int2(-5, 0), int2(0, 5), int2(0, -5), int2(4, 4), int2(-4, 4), int2(4, -4), int2(-4, -4),
				int2(10, 0), int2(-10, 0), int2(0, 10), int2(0, -10), int2(7, 7), int2(-7, 7), int2(7, -7), int2(-7, -7),
				int2(20, 0), int2(-20, 0), int2(0, 20), int2(0, -20), int2(14, 14), int2(-14, 14), int2(14, -14), int2(-14, -14) };
			float sum = 0.0;
			float count = 0.0;
			[unroll] for (uint m = 0u; m < 24u; ++m)
			{
				const float other = GroundFog::FogFloor.Load(int3(clamp(before + around[m], 0, last), 0));
				if (other < GroundFog::FloorUnknown) {
					const float above = other - level;
					sum += above > RaisedEnd ? 0.0 : max(above, -RaisedEnd);
					count += 1.0;
				}
			}
			if (count >= 4.0)
				lowness = RestFlat + sum / count / RestDepth;
		}
	}

	// And the terrain: how much lower than the land about 6 m around this place is (not over water, whose bed is
	// under its level surface). Then hold it to its range: a rise holds nothing.
	if (SimFlags & 1u)
		lowness += (1.0 - water) * clamp(GroundFog::GroundAround(xy, 420.0) - groundZ, -RaisedEnd, 2.0 * RaisedEnd) / RestDepthMap;
	lowness = clamp(lowness, 0.0, RestMax);

	// Swirl: the curl of a slowly changing noise carried with the wind.
	if (Swirl > 0.0) {
		const float2 p = (xyAbs - GroundFog::WindTravel) / SwirlSize + SwirlTime * float2(0.031, -0.023);
		const float e = 0.35;
		const float2 gradient = float2(GroundFog::ValueNoise(p + float2(e, 0.0)) - GroundFog::ValueNoise(p - float2(e, 0.0)),
										GroundFog::ValueNoise(p + float2(0.0, e)) - GroundFog::ValueNoise(p - float2(0.0, e))) /
		                        (2.0 * e);
		// The share that grows with the speed stays below what the damping takes, so eddies cannot feed themselves.
		// Three times as strong at the rim of a pool, so its edge curls and breaks up while its middle lies still.
		velocity += float2(gradient.y, -gradient.x) * Swirl * (6.0 + 0.25 * min(length(velocity - drift), 300.0)) * (1.0 + 2.0 * edge) * DeltaTime;
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
		wear *= saturate(1.0 - emitter.clear * ClearStrength * bell);
		// Pushed from the middle to the rim: a Ricker source (as in Iwillbewarlock's GroundFog, Source in its
		// WindShaders.h, GPL-3.0). Positive inside the radius, negative outside, and zero over the plane, so the
		// fog a body parts stands as a rim beside its track instead of vanishing.
		const float q = distance2 / radius2;
		const float ricker = (1.0 - q) * exp(-q);
		gain *= exp(clamp(-emitter.push * ricker * DeltaTime, -0.7, 0.7));
		// The same push raises the rim and presses the middle down; a blow adds its kick and throws the fog up.
		lift -= 0.8 * emitter.push * ricker * DeltaTime;
		velocity += emitter.kick * bell;
		lift += emitter.lift * bell;
	}

	// 4. Settle. Nothing rests on raised things, and no place holds more than MaxPile (a patch carried into
	// a gap stays a patch).
	// Water is level, and fog lies on it.
	lowness = lerp(lowness, max(lowness, 1.0), water);
	const float place = open * (1.0 - raised) * lerp(1.0, lowness, Pooling);
	rest = GroundFog::RestPattern(xyAbs, TexelSize, place, SwirlTime);
	float amount = max(rest + delta, 0.0) * gain;
	amount = max(amount * wear, min(amount, WakeFloor * rest));
	amount += (rest - amount) * RefillRate;
	amount = clamp(amount, 0.0, min(MaxAmount, (1.0 - raised) * (MinRoom + MaxPile * smoothstep(0.0, 0.15, place))));
	velocity = clamp(velocity, -MaxSpeed, MaxSpeed);
	if (!(amount >= 0.0) || !(dot(velocity, velocity) < 1.0e9)) {
		amount = rest;
		velocity = drift;
	}
	StateOut[pixel] = float4(velocity, amount, rest);
	LiftOut[pixel] = clamp(lift, -0.6, 3.0) * (1.0 - raised);

	// 5. Floor. Each texel's own floor: the lowest up-facing surface the screen showed in it. Taking the lowest in
	// the texels around as well (as this did before) put every texel's floor at the lowest thing within 0.6 m of it,
	// so a road lying a hand's width above the dirt beside it, a paved street, a step or a root was "above its
	// floor" and got little or no fog. The neighbours only fill a texel nothing has been seen in yet.
	float seen = GroundFog::FloorEmpty;
	if (had) {
		seen = FloorSeen.Load(int3(before, 0));
		if (seen >= GroundFog::FloorUnknown && level >= GroundFog::FloorUnknown) {
			[unroll] for (int dy = -1; dy <= 1; ++dy)
			{
				[unroll] for (int dx = -1; dx <= 1; ++dx)
					seen = min(seen, FloorSeen.Load(int3(clamp(before + int2(dx, dy), 0, last), 0)));
			}
		}
	}
	if (seen < GroundFog::FloorUnknown) {
		if (level >= GroundFog::FloorUnknown || seen < level - FloorDropSlack)
			level = seen;
		else if (seen > level + FloorRiseSlack)
			level += (seen - level) * FloorRise;
	}
	FloorOut[pixel] = level;

	// 6. Sun. How much of the sun reaches the fog over this texel: the shadow cascades at four places across the
	// texel and at four heights through the pool, so a shadow has neither a sharp edge nor a height of its own, then
	// eased toward over SunRate, so leaves moving in the wind and the cascades' own stepping come through as slow,
	// soft changes. With the camera still it comes to rest.
	{
		const float base = (level < GroundFog::FloorUnknown ? level : groundZ) - FrameBuffer::CameraPosAdjust.z;
		float sun = 0.0;
		[unroll] for (uint s = 0u; s < 4u; ++s)
			sun += MediumCascadeShadow(float3(xy + (float2(s & 1u, s >> 1u) - 0.5) * TexelSize, base + (0.1 + 0.3 * float(s)) * LayerTop));
		sun *= 0.25;
		if (had)
			sun = lerp(GroundFog::FogSlope.Load(int3(before, 0)).w, sun, SunRate);
		floorSlope.w = saturate(sun);
	}
	SlopeOut[pixel] = floorSlope;
}
