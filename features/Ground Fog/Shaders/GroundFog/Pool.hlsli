#ifndef GROUND_FOG_POOL_HLSLI
#define GROUND_FOG_POOL_HLSLI

// Ground Fog: the pool. A layer of fog below knee height around the camera, drawn on what the screen shows.
//
// A layer this thin cannot take its ground from a height map: an error of a hand's width switches it on and off.
// So nothing here reads one. For every pixel the fog is integrated in closed form along the last stretch of the
// view ray, above a plane:
// - Ground (the pixel faces up and is not well above the floor around it): the plane through the pixel with its
//   slope taken from the floor map around it (see below), so the facets of a mesh do not show. The ray leaves the
//   fog where it has risen PoolThickness above that plane.
// - Anything else (a wall, a trunk, a leg, a blade of grass, the top of a barrel): the thing stands in the fog. The
//   plane is level, at the floor under the pixel, and the ray ends on the thing at its height above that floor. So
//   the foot of a wall is in the fog and its top is not, and nothing is coated.
// The floor is the simulation's floor map: the lowest up-facing surface the screen has shown in and around each
// texel of the simulation window. The pixel pass writes what it sees into FloorSplat as it goes, and the simulation
// folds that into the map on its next step. It needs no terrain data, so it holds indoors as well.
//
// How much fog there is comes from the simulation (FogSim.z), read at two places along the stretch of the ray that
// is inside the layer. Nothing depends on the screen or the view except which ray is integrated.
//
// Include after GroundFog/GroundFog.hlsli.

namespace GroundFog
{
	// A surface up to this far above the floor around it is ground (a kerb, a step, a root); game units.
	static const float PoolStep = 25.0;
	// Nearer than this the depth buffer holds the first-person body and what it carries, not the world.
	static const float PoolNearStart = 40.0;
	static const float PoolNearFull = 75.0;
	// The plane through a pixel is trusted for a ray no flatter than this over it (rise per unit of length).
	static const float PoolMinSlope = 0.04;

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
	};

	// The floor level at a simulation uv, from the four texels around it that have one. False where none has.
	bool FloorLevel(float2 uv, out float z)
	{
		uint width, height;
		FogFloor.GetDimensions(width, height);
		const float2 f = frac(uv * float2(width, height) - 0.5);
		float4 levels = FogFloor.GatherRed(LinearSampler, uv);
		float4 weights = float4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y));
		const bool4 known = levels < FloorUnknown;
		weights = known ? weights : 0.0;
		levels = known ? levels : 0.0;
		const float sum = weights.x + weights.y + weights.z + weights.w;
		z = sum > 1e-4 ? dot(weights, levels) / sum : 0.0;
		return sum > 1e-4;
	}

	// position: where the ray ends, camera-relative. normal: of what it ends on, from the depth buffer.
	// tNear: where the nearest of the output pixel's scene pixels lies along the ray.
	Pool EvaluatePool(float3 position, float3 dir, float tEnd, float camZ, float3 normal, float tNear, inout uint taps)
	{
		Pool pool;
		pool.tau = pool.tauNear = pool.t = pool.z = pool.above = pool.amount = 0.0;
		pool.speed = 0.0;
		pool.floorZ = FloorEmpty;

		const float2 uv = SimUv(position.xy);
		const float range = (1.0 - smoothstep(0.5 * PoolEnd, PoolEnd, tEnd)) * SimInside(uv) * smoothstep(PoolNearStart, PoolNearFull, tEnd);
		if (range <= 0.0)
			return pool;

		const float z = position.z + camZ;
		// Every surface that faces up tells the floor map how high it is; the map keeps the lowest.
		if (normal.z > 0.8)
			pool.floorZ = z;

		float floorZ;
		const bool known = FloorLevel(uv, floorZ);
		++taps;
		const float over = known ? max(z - floorZ, 0.0) : 0.0;
		// Ground: faces up and is no more than a step above the floor around it. Where the floor is not known
		// yet, facing up is enough.
		const float ground = smoothstep(0.6, 0.85, normal.z) * (1.0 - smoothstep(PoolStep, 2.4 * PoolStep, over));
		// Something standing in the fog needs the floor under it to say how deep it stands.
		const float weight = known ? 1.0 : ground;
		if (weight <= 0.0)
			return pool;

		// Height of the ray above the ground the fog lies on, s back from its end: start + rise * s. On ground it
		// starts at 0, elsewhere at the height of the pixel over the floor.
		const float start = (1.0 - ground) * over;
		if (start >= PoolThickness * (1.0 + NearBreath))
			return pool;
		const float stretch = min(tEnd, 25.0 * PoolThickness);

		// How fast the ray rises above the ground going back toward the camera. The ground the fog lies on is the
		// floor at the scale of a stride, not the facets of the mesh the pixel is on: first from the slope of the
		// floor map around the pixel, then, where the map knows the floor under the place the ray would leave the
		// layer, from the actual drop of the floor between there and here. So fog in a dip a few steps wide is as
		// deep as the dip, and a single tilted triangle does not get a sheet of its own. Where the map has no
		// slope yet (new ground, far away) the plane of the pixel itself stands in, measured vertically.
		const float4 slopeTap = FogSlope.SampleLevel(LinearSampler, uv, 0);
		++taps;
		const float slopeKnown = saturate(slopeTap.z);
		const float2 slope = slopeTap.xy / max(slopeTap.z, 1e-3);
		const float riseFloor = slopeKnown > 0.0 ? -dir.z + dot(slope, dir.xy) : -dir.z;
		const float riseOwn = -dot(normal, dir) / max(normal.z, 0.5);
		float rise = lerp(riseFloor, lerp(riseOwn, riseFloor, slopeKnown), ground);
		if (known) {
			const float reach = min(stretch, max(PoolThickness - start, 0.0) / max(rise, PoolMinSlope));
			float floorBack;
			++taps;
			if (reach > 1.0 && FloorLevel(SimUv(position.xy - dir.xy * reach), floorBack))
				rise = -dir.z - (floorBack - floorZ) / reach;
		}
		// The ground is trusted for a ray no flatter than PoolMinSlope over it.
		rise = lerp(rise, max(rise, PoolMinSlope), ground);

		// The stretch of the ray inside the layer, for where to read the simulation.
		const float within = rise > 1e-4 ? min(stretch, max(PoolThickness - start, 0.0) / rise) : stretch;
		const float4 state = 0.5 * (FogSim.SampleLevel(LinearSampler, SimUv(position.xy - dir.xy * (0.2 * within)), 0) +
									   FogSim.SampleLevel(LinearSampler, SimUv(position.xy - dir.xy * (0.65 * within)), 0));
		taps += 2u;
		const float amount = max(state.z, 0.0);
		pool.amount = amount;
		pool.speed = state.xy;
		if (amount <= 0.0)
			return pool;

		// Where there is less fog the layer is lower too, so a wake is a dip and not only a thin place.
		const float top = max(PoolThickness * Breathing(position.xy + FrameBuffer::CameraPosAdjust.xy) * lerp(0.6, 1.0, saturate(amount)), 1.0);
		const SoftProfile profile = MakeSoftProfile(top, 0.0, NearFalloff);
		const float extinction = PoolExtinction * amount * range * weight;
		const float end = start + rise * stretch;
		pool.tau = extinction * SoftLineMean(profile, start, end) * stretch;
		if (pool.tau <= 0.0)
			return pool;
		// In front of the nearest scene pixel: the same integral over the part of the stretch beyond it.
		const float back = tEnd - tNear;
		if (back < stretch)
			pool.tauNear = min(extinction * SoftLineMean(profile, start + rise * back, end) * (stretch - back), pool.tau);

		const float lit = clamp(start + 0.25 * top, 0.15 * top, top);
		pool.t = tEnd - 0.4 * within;
		pool.z = z - start + lit;
		pool.above = PoolExtinction * amount * SoftAbove(profile, lit);
		return pool;
	}
}

#endif
