#ifndef GROUND_FOG_POOL_VOLUME_HLSLI
#define GROUND_FOG_POOL_VOLUME_HLSLI

// Ground Fog: the pool as a volume (Flags bit 2, setting Near Volume). GroundFog/PoolVolume.cs.hlsl fills a 3D
// texture over the simulation's window that follows the floor; here a pixel's ray is marched through it from the
// camera to the pixel's own depth (or to the sky). The result takes the place of EvaluatePool's: the same Pool
// fields, so the lighting and the composite are unchanged. Without the flag the pool is drawn on the visible surface
// as before (GroundFog/Pool.hlsli), for A/B.
//
// Include after GroundFog/Pool.hlsli.

namespace GroundFog
{
	// Height of the volume above the floor, game units (1.43 m): twice the pool's hard top, for fog thrown up.
	static const float VolumeHeight = 100.0;
	static const float VolumeUnitsPerMeter = 70.0;
	// Steps along a ray, over the stretch of it that can be inside the layer.
	static const uint VolumeSteps = 20u;

	bool VolumeOn()
	{
		return (Flags & 4u) != 0u;
	}

#ifndef GROUND_FOG_VOLUME_FILL
	Texture3D<float4> PoolVolume : register(t14);
	Texture2D<float> PoolVolumeFloor : register(t15);

	// dir: the ray's direction; tEnd: where it ends (the scene, or the end of the fog's range for sky); tNear: where
	// the nearest of the output pixel's scene pixels lies along it. jitter: 0 to 1, per pixel and frame, where in
	// each step the volume is read (the temporal antialiasing after the composite averages it).
	Pool MarchVolume(float3 dir, float tEnd, float camZ, float tNear, float jitter, inout uint taps, out float lightSun, out float lightSky)
	{
		lightSun = 1.0;
		lightSky = 1.0;
		Pool pool;
		pool.tau = pool.tauNear = pool.t = pool.z = pool.above = pool.amount = 0.0;
		pool.speed = 0.0;
		pool.floorZ = FloorEmpty;
		pool.range = pool.known = pool.ground = pool.over = pool.within = 0.0;
		pool.crest = pool.sunSide = pool.level = 0.5;
		if (FlowWindow.w <= 0.0 || PoolExtinction <= 0.0)
			return pool;

		// The stretch of the ray that can be inside the layer: no floor along it lies higher than the highest
		// terrain under it or the floor under the camera.
		const float reach = min(tEnd, PoolEnd);
		const float camFloor = PoolVolumeFloor.SampleLevel(LinearSampler, SimUv(0.0), 0);
		const float ceiling = max(GroundMaxAlong(0.0, dir.xy * reach), camFloor) + VolumeHeight;
		taps += 2u;
		float t0 = 0.0, t1 = reach;
		if (dir.z < -1e-4)
			t0 = max((camZ - ceiling) / -dir.z, 0.0);
		else if (camZ >= ceiling)
			return pool;
		if (dir.z > 1e-4)
			t1 = min(t1, (ceiling - camZ) / dir.z);
		if (t1 <= t0 + 1.0)
			return pool;
		pool.range = 1.0;

		const float step = (t1 - t0) / float(VolumeSteps);
		float transmittance = 1.0;
		float tau = 0.0, tauFront = 0.0, weightSum = 0.0, tSum = 0.0, zSum = 0.0, sunSum = 0.0, skySum = 0.0, levelSum = 0.0;
		[loop] for (uint i = 0u; i < VolumeSteps; ++i)
		{
			const float s = t0 + (float(i) + jitter) * step;
			const float2 xy = dir.xy * s;
			const float2 uv = SimUv(xy);
			const float inside = SimInside(uv) * (1.0 - smoothstep(0.5 * PoolEnd, PoolEnd, s));
			if (inside <= 0.0)
				continue;
			const float z = camZ + dir.z * s;
			const float h = z - PoolVolumeFloor.SampleLevel(LinearSampler, uv, 0);
			++taps;
			if (h < 0.0 || h >= VolumeHeight)
				continue;
			const float4 voxel = PoolVolume.SampleLevel(LinearSampler, float3(uv, h / VolumeHeight), 0);
			++taps;
			const float d = voxel.x / VolumeUnitsPerMeter * inside * step;
			if (d <= 0.0)
				continue;
			// The part of this step in front of the nearest scene pixel.
			tauFront += d * saturate((tNear - (s - jitter * step)) / step);
			const float weight = transmittance * OneMinusExp(d);
			weightSum += weight;
			tSum += weight * s;
			zSum += weight * z;
			sunSum += weight * voxel.y;
			skySum += weight * voxel.z;
			levelSum += weight * h;
			tau += d;
			transmittance *= exp(-d);
		}
		if (weightSum <= 1e-6)
			return pool;
		pool.tau = tau;
		pool.tauNear = min(tauFront, tau);
		pool.t = tSum / weightSum;
		pool.z = zSum / weightSum;
		pool.within = t1 - t0;
		pool.level = saturate(levelSum / weightSum / max(min(PoolThickness, PoolHardTop), 1.0));
		// The self shadow is in the volume already: the lighting is given no fog above.
		pool.above = 0.0;
		lightSun = sunSum / weightSum;
		lightSky = skySum / weightSum;
		return pool;
	}
#endif
}

#endif
