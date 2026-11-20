#ifndef GROUND_FOG_POOL_HLSLI
#define GROUND_FOG_POOL_HLSLI

// Ground Fog: the pool. A layer of fog below knee height around the camera, drawn on what the screen shows.
//
// A layer this thin cannot take its ground from a height map: an error of a hand's width switches it on and off.
// So nothing here reads one. For every pixel the fog is integrated in closed form along the last stretch of the
// view ray, above a plane:
// - Ground (the pixel faces up and lies at the level of the floor around it): the plane through the pixel with its
//   slope taken from the floor map around it (see below), so the facets of a mesh do not show. The ray leaves the
//   fog where it has risen PoolThickness above that plane.
// - Anything else (a wall, a trunk, a leg, a blade of grass, the top of a barrel): the thing stands in the fog. The
//   plane is level, at the floor under the pixel, and the ray ends on the thing at its height above that floor. So
//   the foot of a wall is in the fog and its top is not, and nothing is coated.
// The floor is the simulation's floor map: the lowest up-facing surface the screen has shown in and around each
// texel of the simulation window. The pixel pass writes what it sees into FloorSplat as it goes, and the simulation
// folds that into the map on its next step. It needs no terrain data, so it holds indoors as well.
//
// The rule that keeps the fog off everything that is not at floor level: the floor under a pixel is the floor map
// filtered between the texels around it, without those that stand well above the lowest of them (the inside of the
// wall or the roof the pixel belongs to); if all of them stand well above the floor a step toward the camera, it is
// that floor (what a wall stands on is on the camera's side of it). Then
// - a pixel at or above the layer's top over that floor gets nothing, whatever it faces. On sloping ground an
//   up-facing pixel is allowed the height the slope accounts for (at most PoolStep);
// - a pixel with no known floor gets fog only if it is very nearly level (far ground the map has not reached yet):
//   a roof, a bank or a wall with no known floor gets nothing.
//
// How much fog there is comes from the simulation (FogSim.z), read at two places along the stretch of the ray that
// is inside the layer. Nothing depends on the screen or the view except which ray is integrated.
//
// Include after GroundFog/GroundFog.hlsli.

namespace GroundFog
{
	// The most height a slope may account for between a pixel and the lowest floor around it; game units.
	static const float PoolStep = 25.0;
	// Nearer than this the depth buffer holds the first-person body and what it carries, not the world.
	static const float PoolNearStart = 40.0;
	static const float PoolNearFull = 75.0;
	// The plane through a pixel is trusted for a ray no flatter than this over it (rise per unit of length).
	static const float PoolMinSlope = 0.04;
	// The simulation's amount counts up to this, and the pool's optical depth along a ray levels off at PoolTauMax:
	// whatever the simulation or the floor map does in one place, the pool stays a veil.
	static const float PoolAmountMax = 4.0;
	static const float PoolTauKnee = 1.5;
	static const float PoolTauMax = 3.0;

	struct Pool
	{
		float tau;      // optical depth of the pool along the whole ray
		float tauNear;  // the part of it in front of tNear
		float t;        // where along the ray it is lit
		float z;        // absolute z of that point
		float above;    // optical depth of the pool above that point
		float amount;   // the simulation's amount there
		float2 speed;   // the simulation's velocity there, for the debug view
		float floorZ;   // what the pixel adds to the floor map: its absolute z, or FloorEmpty
		// For the debug views.
		float range;   // 0 outside the reach of the pool
		float known;   // 1 where the floor map has a floor for the pixel
		float ground;  // how much the pixel counts as ground
		float over;    // its height over the floor, game units
		float within;  // length of the stretch of the ray inside the layer, game units
	};

	// Texels of the floor map this far above the lowest beside them belong to another level (game units).
	static const float PoolOtherLevel = 60.0;

	// The floor at a simulation uv, from the four texels around it: the lowest of them, and the level filtered
	// between those that are no more than PoolOtherLevel above it. False where none is known. One read.
	bool FloorAround(float2 uv, out float level, out float lowest)
	{
		uint width, height;
		FogFloor.GetDimensions(width, height);
		const float2 f = frac(uv * float2(width, height) - 0.5);
		float4 levels = FogFloor.GatherRed(LinearSampler, uv);
		// A texel counts at least a little wherever the pixel is among the four, so one that is known is never lost.
		float4 weights = float4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y)) + 0.02;
		lowest = min(min(levels.x, levels.y), min(levels.z, levels.w));
		const bool4 known = levels < min(lowest + PoolOtherLevel, FloorUnknown);
		weights = known ? weights : 0.0;
		levels = known ? levels : 0.0;
		const float sum = weights.x + weights.y + weights.z + weights.w;
		level = sum > 1e-4 ? dot(weights, levels) / sum : 0.0;
		return lowest < FloorUnknown;
	}

	// position: where the ray ends, camera-relative. normal: of what it ends on, from the depth buffer.
	// tNear: where the nearest of the output pixel's scene pixels lies along the ray.
	Pool EvaluatePool(float3 position, float3 dir, float tEnd, float camZ, float3 normal, float tNear, inout uint taps)
	{
		Pool pool;
		pool.tau = pool.tauNear = pool.t = pool.z = pool.above = pool.amount = 0.0;
		pool.speed = 0.0;
		pool.floorZ = FloorEmpty;
		pool.range = pool.known = pool.ground = pool.over = pool.within = 0.0;

		const float2 uv = SimUv(position.xy);
		const float range = (1.0 - smoothstep(0.5 * PoolEnd, PoolEnd, tEnd)) * SimInside(uv) * smoothstep(PoolNearStart, PoolNearFull, tEnd);
		if (range <= 0.0)
			return pool;
		pool.range = range;

		const float z = position.z + camZ;
		// Every surface that faces up tells the floor map how high it is; the map keeps the lowest.
		if (normal.z > 0.8)
			pool.floorZ = z;

		// The floor under the pixel: the lowest known around it and one and a half texels toward the camera.
		uint width, height;
		FogFloor.GetDimensions(width, height);
		const float texel = 1.0 / (FlowWindow.z * float(width));
		const float across = length(dir.xy);
		float levelHere, lowHere, levelToward, lowToward;
		FloorAround(uv, levelHere, lowHere);
		FloorAround(SimUv(position.xy - dir.xy * (across > 1e-3 ? 1.5 * texel / across : 0.0)), levelToward, lowToward);
		taps += 2u;
		const bool known = min(lowHere, lowToward) < FloorUnknown;
		// The floor here, unless everything here stands well above the floor toward the camera.
		const float floorZ = lowHere <= lowToward + PoolOtherLevel ? levelHere : levelToward;
		const float over = known ? z - floorZ : 0.0;
		// How far the top is thrown up or pressed down here.
		const float lift = clamp(FogLift.SampleLevel(LinearSampler, uv, 0), -0.6, 3.0);
		++taps;
		const float thick = PoolThickness * (1.0 + lift);
		const float limit = PoolThickness * (1.0 + NearBreath) * (1.0 + max(lift, 0.0));
		pool.known = known ? 1.0 : 0.0;
		pool.over = over;
		// Well under every floor around: the map is not up to date there (it will be on the next frame).
		if (over <= -2.0 * PoolStep)
			return pool;

		// The slope of the floor around the pixel, over about a meter.
		const float4 slopeTap = FogSlope.SampleLevel(LinearSampler, uv, 0);
		++taps;
		const float slopeKnown = saturate(slopeTap.z);
		const float2 slope = slopeTap.xy / max(slopeTap.z, 1e-3);

		// Ground: faces up and lies at the level of the floor, give or take what the slope there accounts for
		// (the lowest floor within a texel or two of a place on a slope is lower than the place).
		const float allowance = min(min(length(slope), 0.6) * slopeKnown * 1.5 * texel, PoolStep);
		// With no known floor only what is very nearly level counts.
		const float ground = known ? smoothstep(0.6, 0.85, normal.z) * (1.0 - smoothstep(0.5 * PoolThickness, PoolThickness, over - allowance)) : smoothstep(0.88, 0.96, normal.z);
		pool.ground = ground;
		if (!known && ground <= 0.0)
			return pool;

		// Height of the ray above the ground the fog lies on, s back from its end: start + rise * s. On ground it
		// starts at 0, elsewhere at the height of the pixel over the floor. At or above the layer's top there is
		// nothing, for any surface.
		const float start = (1.0 - ground) * max(over, 0.0);
		if (start >= limit)
			return pool;
		// Without a floor only the ground share has fog.
		const float share = known ? 1.0 : ground;
		const float stretch = min(tEnd, 25.0 * PoolThickness);
		if (start >= thick * (1.0 + NearBreath))
			return pool;

		// How fast the ray rises above the ground going back toward the camera. The ground the fog lies on is the
		// floor at the scale of a stride, not the facets of the mesh the pixel is on: first from the slope of the
		// floor map around the pixel, then, where the map knows the floor under the place the ray would leave the
		// layer, from the actual drop of the floor between there and here. So fog in a dip a few steps wide is as
		// deep as the dip, and a single tilted triangle does not get a sheet of its own. Where the map has no
		// slope yet (new ground) the plane of the pixel itself stands in, measured vertically.
		const float riseFloor = -dir.z + dot(slope, dir.xy) * slopeKnown;
		const float riseOwn = -dot(normal, dir) / max(normal.z, 0.5);
		float rise = lerp(riseFloor, lerp(riseOwn, riseFloor, slopeKnown), ground);
		if (known) {
			const float reach = min(stretch, max(thick - start, 0.0) / max(rise, PoolMinSlope));
			float levelBack, lowBack;
			++taps;
			// A floor back there that stands well above this one is a thing the ray passes over (a cart, a wall,
			// a porch), not the rim of a dip the fog fills: it says nothing about the ground the fog lies on.
			if (reach > 1.0 && FloorAround(SimUv(position.xy - dir.xy * reach), levelBack, lowBack) && levelBack - floorZ < PoolThickness + PoolStep)
				rise = -dir.z - (levelBack - floorZ) / reach;
		}
		// Seen from above, no ray is taken to run flatter than PoolMinSlope over the fog's ground: the floor map
		// is not trusted for more fog than that, on ground or in front of what stands on it.
		if (dir.z < 0.0)
			rise = max(rise, PoolMinSlope);

		// The stretch of the ray inside the layer, for where to read the simulation.
		const float within = rise > 1e-4 ? min(stretch, max(thick - start, 0.0) / rise) : stretch;
		const float4 state = 0.5 * (FogSim.SampleLevel(LinearSampler, SimUv(position.xy - dir.xy * (0.2 * within)), 0) +
									   FogSim.SampleLevel(LinearSampler, SimUv(position.xy - dir.xy * (0.65 * within)), 0));
		taps += 2u;
		pool.within = within;
		const float amount = clamp(state.z, 0.0, PoolAmountMax);
		pool.amount = amount;
		pool.speed = state.xy;
		if (amount <= 0.0)
			return pool;

		// Where there is less fog the layer is lower too, so a wake is a dip and not only a thin place.
		const float top = max(thick * Breathing(position.xy + FrameBuffer::CameraPosAdjust.xy) * lerp(0.4, 1.0, saturate(amount)), 1.0);
		const SoftProfile profile = MakeSoftProfile(top, 0.0, NearFalloff);
		// Fog thrown up is the same fog over more height: thinner, though not by all of it (it is stirred up too).
		const float extinction = PoolExtinction * amount * range * share * rsqrt(1.0 + max(lift, 0.0));
		const float end = start + rise * stretch;
		const float tau = extinction * SoftLineMean(profile, start, end) * stretch;
		if (tau <= 0.0)
			return pool;
		// Levels off: no single place can turn the pool into a wall.
		pool.tau = tau <= PoolTauKnee ? tau : PoolTauKnee + (PoolTauMax - PoolTauKnee) * OneMinusExp((tau - PoolTauKnee) / (PoolTauMax - PoolTauKnee));
		// In front of the nearest scene pixel: the same integral over the part of the stretch beyond it.
		const float back = tEnd - tNear;
		if (back < stretch)
			pool.tauNear = min(extinction * SoftLineMean(profile, start + rise * back, end) * (stretch - back), tau) * pool.tau / tau;

		const float lit = clamp(start + 0.25 * top, 0.15 * top, top);
		pool.t = tEnd - 0.4 * within;
		pool.z = z - start + lit;
		pool.above = PoolExtinction * amount * SoftAbove(profile, lit);
		return pool;
	}
}

#endif
