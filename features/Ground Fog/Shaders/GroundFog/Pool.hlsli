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
	static const float PoolTauMax = 2.5;
	// The finest structure of the pool, below the simulation's texels: size of its cells in game units (0.6 m and
	// a third of that), and how far the air's velocity bends it, in seconds of travel.
	static const float PoolGrain = 42.0;
	static const float PoolGrainDrag = 0.5;
	// THE HEIGHT RULE. The pool is a blanket a foot thick at its thickest. No surface more than PoolHardTop above
	// its floor ever receives pool fog, whatever the settings, the simulation or the floor map say: EvaluatePool
	// ends with that test (PoolHeightRule), after everything else. The fog itself is cut off at that height too:
	// its profile keeps the scale Near Thickness gives it (so it is dense nearly up to the cut), and nothing of it
	// is counted above PoolHardTop. Game units (0.35 m).
	static const float PoolHardTop = 24.5;
	// Fog that is struck may stand up to this many times taller for a moment, over the ground only (a plume is
	// seen against the ground behind it; it still puts nothing on a surface above PoolHardTop).
	static const float PoolPlumeMax = 2.0;
	// A pixel that faces up at least this much (normal z) and lies within PoolGroundBand of a floor level around it
	// is ground: the fog lies on it, in full. Game units (0.1 m), and as much again to fade out.
	static const float PoolGroundFacing = 0.5;
	static const float PoolGroundBand = 7.0;

	// The soft profile, cut off at `cap`: the mean density along a straight piece of ray whose height runs from ha
	// to hb, counting nothing above the cap; and the density integrated from h up to the cap.
	float ClippedLineMean(SoftProfile profile, float ha, float hb, float cap)
	{
		const float low = min(ha, hb);
		const float high = max(ha, hb);
		const float ceiling = min(profile.top, cap);
		if (high <= -profile.fill || low >= ceiling)
			return 0.0;
		if (high - low < 1.0) {
			const float mid = 0.5 * (low + high);
			return mid > ceiling ? 0.0 : SoftDensity(profile, mid);
		}
		return (SoftFirst(profile, min(high, ceiling)) - SoftFirst(profile, min(low, ceiling))) / (high - low);
	}

	float ClippedAbove(SoftProfile profile, float h, float cap)
	{
		const float ceiling = min(profile.top, cap);
		return h >= ceiling ? 0.0 : SoftFirst(profile, ceiling) - SoftFirst(profile, max(h, 0.0));
	}

	// The last word on every result of EvaluatePool: nothing for a surface higher than PoolHardTop over its floor.
	// height: of the surface over the floor under it, game units; 0 for ground.
	float PoolHeightRule(float height)
	{
		return height > PoolHardTop ? 0.0 : 1.0;
	}

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
		// The lumpy top, for shading: 0.5 and 0.5 where there is nothing to say.
		float crest;    // 0 in a fold between billows, 1 on top of one
		float sunSide;  // 0 on the side of a billow away from the light, 1 on the side toward it
	};

	// Texels of the floor map this far above the lowest beside them belong to another level (game units).
	static const float PoolOtherLevel = 60.0;

	// The floor at a simulation uv, from the four texels around it: the lowest of them, and the level filtered
	// between those that are no more than PoolOtherLevel above it. nearest: how far z lies from the nearest floor
	// level among the four (above or below it). False where none is known. One read.
	bool FloorAround(float2 uv, float z, out float level, out float lowest, out float nearest)
	{
		uint width, height;
		FogFloor.GetDimensions(width, height);
		const float2 f = frac(uv * float2(width, height) - 0.5);
		float4 levels = FogFloor.GatherRed(LinearSampler, uv);
		// A texel counts at least a little wherever the pixel is among the four, so one that is known is never lost.
		float4 weights = float4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y)) + 0.02;
		lowest = min(min(levels.x, levels.y), min(levels.z, levels.w));
		const float4 distances = levels < FloorUnknown ? abs(z - levels) : 1.0e30;
		nearest = min(min(distances.x, distances.y), min(distances.z, distances.w));
		const bool4 known = levels < min(lowest + PoolOtherLevel, FloorUnknown);
		weights = known ? weights : 0.0;
		levels = known ? levels : 0.0;
		const float sum = weights.x + weights.y + weights.z + weights.w;
		level = sum > 1e-4 ? dot(weights, levels) / sum : 0.0;
		return lowest < FloorUnknown;
	}

	// position: where the ray ends, camera-relative. normal: of what it ends on, from the depth buffer.
	// tNear: where the nearest of the output pixel's scene pixels lies along the ray.
	// lightDir: toward the sun or moon that lights the scene.
	Pool EvaluatePool(float3 position, float3 dir, float tEnd, float camZ, float3 normal, float tNear, float3 lightDir, inout uint taps)
	{
		Pool pool;
		pool.tau = pool.tauNear = pool.t = pool.z = pool.above = pool.amount = 0.0;
		pool.speed = 0.0;
		pool.floorZ = FloorEmpty;
		pool.range = pool.known = pool.ground = pool.over = pool.within = 0.0;
		pool.crest = pool.sunSide = 0.5;

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
		float levelHere, lowHere, levelToward, lowToward, nearHere, nearToward;
		FloorAround(uv, z, levelHere, lowHere, nearHere);
		FloorAround(SimUv(position.xy - dir.xy * (across > 1e-3 ? 1.5 * texel / across : 0.0)), z, levelToward, lowToward, nearToward);
		taps += 2u;
		const bool known = min(lowHere, lowToward) < FloorUnknown;
		// The floor here, unless everything here stands well above the floor toward the camera.
		const float floorZ = lowHere <= lowToward + PoolOtherLevel ? levelHere : levelToward;
		const float over = known ? z - floorZ : 0.0;
		// How far the top is thrown up or pressed down here.
		const float lift = clamp(FogLift.SampleLevel(LinearSampler, uv, 0), -0.6, 3.0);
		++taps;
		// The layer keeps the scale of Near Thickness (lifted where it is struck); what of it is counted stops at
		// PoolHardTop, or for a moment up to PoolPlumeMax times that over ground where it is thrown up.
		const float thick = PoolThickness * (1.0 + lift);
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
		// Ground: faces up and lies within PoolGroundBand of a floor level around it (the nearest of the eight
		// texels read here and toward the camera, so the edge of a road or a step counts on both sides), give or
		// take what the slope accounts for. With no known floor only what is very nearly level counts.
		const float nearest = max(min(nearHere, nearToward) - allowance, 0.0);
		const float ground = known ? smoothstep(PoolGroundFacing - 0.05, PoolGroundFacing + 0.1, normal.z) * (1.0 - smoothstep(PoolGroundBand, 2.0 * PoolGroundBand, nearest)) :
		                             smoothstep(0.88, 0.96, normal.z);
		pool.ground = ground;
		if (!known && ground <= 0.0)
			return pool;
		const float limit = PoolHardTop * lerp(1.0, clamp(1.0 + lift, 1.0, PoolPlumeMax), ground);

		// Height of the ray above the ground the fog lies on, s back from its end: start + rise * s. Ground is the
		// floor the fog lies on: it starts at 0. What is not ground starts at its height over the floor, and at or
		// above the cut there is nothing.
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
		float rise = lerp(riseOwn, riseFloor, slopeKnown);
		if (known && ground > 0.0) {
			const float reach = min(stretch, max(min(thick, limit) - start, 0.0) / max(rise, PoolMinSlope));
			float levelBack, lowBack, nearBack;
			++taps;
			// A floor back there that stands well above this one is a thing the ray passes over (a cart, a wall,
			// a porch), not the rim of a dip the fog fills: it says nothing about the ground the fog lies on.
			if (reach > 1.0 && FloorAround(SimUv(position.xy - dir.xy * reach), z, levelBack, lowBack, nearBack) && levelBack - floorZ < PoolHardTop + PoolStep)
				rise = -dir.z - (levelBack - floorZ) / reach;
		}
		// Seen from above, no ray is taken to run flatter than PoolMinSlope over the fog's ground: the floor map
		// is not trusted for more fog than that.
		if (dir.z < 0.0)
			rise = max(rise, PoolMinSlope);
		// What stands in the fog (a wall, a trunk, a leg, a pole) is hidden only by the fog that lies in front of its
		// foot, seen from above: its ray must come down onto it, over a floor that by its own slope does not rise
		// to meet the ray. The fog in front of it is then taken over a level floor or over the floor's own slope,
		// whichever gives less. Where that is not so (the camera is no higher than the thing's foot, or the floor
		// runs up with the ray) there is no telling how much fog is in front of it, and it gets none: if in doubt,
		// less.
		const float standing = dir.z < 0.0 && riseFloor >= PoolMinSlope ? 1.0 : 0.0;
		rise = lerp(max(max(riseFloor, -dir.z), PoolMinSlope), rise, ground);
		const float certain = lerp(standing, 1.0, ground);
		if (certain <= 0.0)
			return pool;

		// The stretch of the ray inside the layer, for where to read the simulation.
		const float within = rise > 1e-4 ? min(stretch, max(min(thick, limit) - start, 0.0) / rise) : stretch;
		const float4 state = 0.5 * (FogSim.SampleLevel(LinearSampler, SimUv(position.xy - dir.xy * (0.2 * within)), 0) +
									   FogSim.SampleLevel(LinearSampler, SimUv(position.xy - dir.xy * (0.65 * within)), 0));
		taps += 2u;
		pool.within = within;
		// Fine structure: two octaves of noise fixed in the world, riding the wind and bent by the air's own motion
		// (so a gust or a blow shears it). It takes most from where the fog is thin, so the rim of a wisp breaks
		// up into strands and holes, and little from a dense core.
		float amount = clamp(state.z, 0.0, PoolAmountMax);
		float billow = 0.55;
		{
			const float2 g = (position.xy + FrameBuffer::CameraPosAdjust.xy - WindTravel - state.xy * PoolGrainDrag) / PoolGrain;
			const float grain = 0.6 * ValueNoise(g) + 0.4 * ValueNoise(g * 2.9 + 31.0);
			amount = max(amount + (grain - 0.55) * 1.8 * min(amount, 0.7) * Wispiness, 0.0);
			// The same noise is the lumps of the top: billows 0.6 m across with smaller ones on them. Where it is
			// high the top stands up (a crest), where it is low there is a fold. And the same noise a hand's width
			// toward the light says which side of a billow this is: lower there, and this side faces the light.
			billow = grain;
			const float lightAcross = length(lightDir.xy);
			if (lightAcross > 1e-3) {
				const float2 gToward = g + lightDir.xy / lightAcross * (14.0 / PoolGrain);
				pool.sunSide = saturate(0.5 + 2.2 * (grain - (0.6 * ValueNoise(gToward) + 0.4 * ValueNoise(gToward * 2.9 + 31.0))));
			}
			pool.crest = smoothstep(0.3, 0.75, grain);
		}
		pool.amount = amount;
		pool.speed = state.xy;
		if (amount <= 0.0)
			return pool;

		// Where there is less fog the layer is lower too, so a wake is a dip and not only a thin place.
		// And the top is lumpy: lower in the folds, a little higher on the billows. This is the profile's scale;
		// what is counted of it stops at the cut (limit).
		const float top = max(thick * Breathing(position.xy + FrameBuffer::CameraPosAdjust.xy) * lerp(0.4, 1.0, saturate(amount)) * lerp(0.72, 1.12, billow), 1.0);
		const SoftProfile profile = MakeSoftProfile(top, 0.0, NearFalloff);
		// Fog thrown up is the same fog over more height: thinner, though not by all of it (it is stirred up too).
		const float extinction = PoolExtinction * amount * range * share * certain * rsqrt(1.0 + max(lift, 0.0));
		const float end = start + rise * stretch;
		const float tau = extinction * ClippedLineMean(profile, start, end, limit) * stretch;
		if (tau <= 0.0)
			return pool;
		// Levels off: no single place can turn the pool into a wall.
		pool.tau = tau <= PoolTauKnee ? tau : PoolTauKnee + (PoolTauMax - PoolTauKnee) * OneMinusExp((tau - PoolTauKnee) / (PoolTauMax - PoolTauKnee));
		// In front of the nearest scene pixel: the same integral over the part of the stretch beyond it.
		const float back = tEnd - tNear;
		if (back < stretch)
			pool.tauNear = min(extinction * ClippedLineMean(profile, start + rise * back, end, limit) * (stretch - back), tau) * pool.tau / tau;

		const float lit = clamp(start + 0.25 * top, 0.15 * top, min(top, limit));
		pool.t = tEnd - 0.4 * within;
		pool.z = z - start + lit;
		pool.above = PoolExtinction * amount * ClippedAbove(profile, lit, limit);

		// THE HEIGHT RULE, last: see PoolHardTop. The height of this surface over its floor: for what stands in the
		// fog, over the lowest floor around it and toward the camera; for ground, over the floor it lies on (the
		// nearest level, less the slope's share). 0 where no floor is known (only nearly level ground gets here then).
		const float lowFloor = min(lowHere, lowToward);
		const float standingHeight = known ? max(z - lowFloor, 0.0) : 0.0;
		const float allowed = PoolHeightRule(lerp(standingHeight, known ? nearest : 0.0, ground));
		pool.tau *= allowed;
		pool.tauNear *= allowed;
		return pool;
	}
}

#endif
