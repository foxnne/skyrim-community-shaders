#ifndef PHYSICAL_SKY_HORIZON_CLOUDS_HLSLI
#define PHYSICAL_SKY_HORIZON_CLOUDS_HLSLI

// Horizon clouds: banks and towers far beyond the ray-marched range (tens of kilometres past the map), seen edge-on
// where the sky meets the sea or the land.
//
// They are drawn as two upright shells around the camera, at the set distance and at a multiple of it (1.8 by default). Each sky ray
// meets a shell at a known height above the curved sea, so the band sits on the horizon from the ground and sinks
// below eye level from altitude, the way real distant cloud does. The shape is a noise function of direction and
// height (no textures, no marching, nothing stored between frames, so nothing can flicker or pop); the light is the
// same sun or moon colour through the same air as the ray-marched clouds, and the air in front comes from the same
// aerial perspective data as the sky behind. Evaluated once per sky pixel when the cloud result is filtered.
//
// Live in HorizonCloudExtra (Physical Sky's menu, Horizon Clouds):
// y: distance of the second shell, as a multiple of the first (1.8)
// z: radiance of a fully sunlit cloud face per unit of sun illuminance (0.22; a white card is 0.318)
// w: how fast light dies inside the cloud; higher = darker bases and far sides (4)

float3 CloudScreenRay(uint2 pixel);

namespace HorizonClouds
{
	float Hash(int2 cell, uint seed)
	{
		uint x = (uint(cell.x) * 0x9E3779B1u) ^ (uint(cell.y) * 0x85EBCA77u) ^ (seed * 0xC2B2AE3Du);
		x ^= x >> 16;
		x *= 0x7FEB352Du;
		x ^= x >> 15;
		x *= 0x846CA68Bu;
		x ^= x >> 16;
		return float(x >> 8) * (1.0 / 16777216.0);
	}

	int Wrap(int cell, int period)
	{
		const int wrapped = cell % period;
		return wrapped < 0 ? wrapped + period : wrapped;
	}

	float Fade(float x)
	{
		return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
	}

	/// Value noise along the horizon; repeats after `period` cells, so it closes around the full circle.
	float Noise1(float position, int period, uint seed)
	{
		const float cell = floor(position);
		const int index = int(cell);
		return lerp(Hash(int2(Wrap(index, period), 0), seed), Hash(int2(Wrap(index + 1, period), 0), seed), Fade(position - cell));
	}

	/// The same in two dimensions: x around the horizon (periodic), y up.
	float Noise2(float2 position, int period, uint seed)
	{
		const float2 cell = floor(position);
		const int2 index = int2(cell);
		const float2 weight = float2(Fade(position.x - cell.x), Fade(position.y - cell.y));
		const int x0 = Wrap(index.x, period), x1 = Wrap(index.x + 1, period);
		return lerp(lerp(Hash(int2(x0, index.y), seed), Hash(int2(x1, index.y), seed), weight.x),
			lerp(Hash(int2(x0, index.y + 1), seed), Hash(int2(x1, index.y + 1), seed), weight.x), weight.y);
	}

	float Fbm1(float turn, int period, uint seed, uint octaves)
	{
		float sum = 0.0, weight = 1.0, total = 0.0;
		[unroll] for (uint octave = 0u; octave < octaves; ++octave)
		{
			sum += weight * Noise1(turn * period, period, seed + octave * 17u);
			total += weight;
			weight *= 0.5;
			period *= 2;
		}
		return sum / total;
	}

	/// Rounded lumps with creases between them, 0..1.
	float Billow(float turn, float height, int period, uint seed)
	{
		float sum = 0.0, weight = 1.0, total = 0.0;
		[unroll] for (uint octave = 0u; octave < 4u; ++octave)
		{
			sum += weight * abs(2.0 * Noise2(float2(turn * period, height), period, seed + octave * 17u) - 1.0);
			total += weight;
			weight *= 0.55;
			period *= 2;
			height *= 2.0;
		}
		return saturate(sum / total * 1.6);
	}

	struct Shell
	{
		int groupPeriod;  // cloud groups around the horizon
		int towerPeriod;  // single towers
		uint seed;
		float coverage;
		float type;  // 0: separate cumulus towers, 1: one flat bank
	};

	/// Share of the open sky that has cloud groups in this direction.
	float GroupMask(float turn, Shell shell)
	{
		const float group = saturate((Fbm1(turn, shell.groupPeriod, shell.seed + 101u, 2u) - 0.25) * 2.0);
		const float threshold = 1.0 - shell.coverage;
		return smoothstep(threshold - 0.15, threshold + 0.15, group);
	}

	/// Height of the cloud top in a direction, as a fraction of the layer (0 = no cloud).
	float TopHeight(float turn, float mask, Shell shell)
	{
		const float tower = Fbm1(turn, shell.towerPeriod, shell.seed + 7u, 3u);
		const float tall = Fbm1(turn, max(shell.towerPeriod / 3, 4), shell.seed + 53u, 2u);
		const float cumulus = smoothstep(0.32, 0.6, tower) * (0.35 + 0.65 * smoothstep(0.2, 0.8, tall));
		const float bank = 0.45 + 0.25 * tower;
		return mask * lerp(cumulus, bank, shell.type);
	}

	/// Optical depth of the clear air between two distances along a ray, from the same profile as the sky LUTs.
	/// Heights follow the curve of the planet. Distances and heights in game units.
	float3 AirDepth(float cameraAltitude, float3 direction, float begin, float end, float planetRadius)
	{
		const SharedData::PhysSkyData data = SharedData::physSkyData;
		const float horizontal2 = 1.0 - direction.z * direction.z;
		float3 depth = 0.0;
		[unroll] for (uint i = 0u; i < 3u; ++i)
		{
			const float distance = lerp(begin, end, (i + 0.5) / 3.0);
			const float altitude = cameraAltitude + distance * direction.z + distance * distance * horizontal2 / (2.0 * planetRadius);
			float rayleigh, aerosol, ozone;
			SampleAtmosphere(max(altitude, 0.0), rayleigh, aerosol, ozone);
			depth += rayleigh * data.rayleighScatter + aerosol * (data.aerosolScatter + data.aerosolAbsorption) + ozone * data.ozoneAbsorption;
		}
		return depth * ((end - begin) / 3.0);
	}

	/// One shell. Returns radiance already multiplied by opacity (rgb) and opacity (a), as seen from the camera
	/// through the air that the cloud result is expected to carry (see Composite).
	float4 Sample(float3 direction, float3 eye, float distance, uint seed, float drift, float4 ap, bool unified)
	{
		const VolumetricCloudData info = VolumetricCloudBuffer[0];
		const float coverage = HorizonCloudShape.x;
		const float baseAltitude = HorizonCloudShape.y;
		const float span = max(HorizonCloudShape.z - baseAltitude, 1.0);
		const float horizontal = sqrt(saturate(1.0 - direction.z * direction.z));
		const float range = distance / horizontal;
		const float altitude = eye.z + range * direction.z + distance * distance / (2.0 * info.planetRadius);
		const float height = (altitude - baseAltitude) / span;
		if (height <= 0.0 || height >= 1.5)
			return 0.0;
		if (direction.z < 0.0) {
			// A ray that dips under the sea before the shell never reaches it.
			const float lowest = -direction.z * info.planetRadius / (horizontal * horizontal);
			if (lowest < range && eye.z - 0.5 * direction.z * direction.z * info.planetRadius / (horizontal * horizontal) < 0.0)
				return 0.0;
		}

		Shell shell;
		const float circumference = 6.2831853 * distance * GAME_UNIT_TO_M;
		shell.groupPeriod = max(int(round(circumference / 60000.0)), 3);
		shell.towerPeriod = max(int(round(circumference / 9000.0)), 8);
		shell.seed = seed;
		shell.coverage = coverage;
		shell.type = saturate(HorizonCloudShape.w);
		const int detailPeriod = max(int(round(circumference / 3000.0)), 16);

		const float turn = atan2(direction.y, direction.x) * (0.5 / Math::PI) + drift;
		const float mask = GroupMask(turn, shell);
		if (mask <= 0.0)
			return 0.0;
		const float detail = Billow(turn, (altitude - baseAltitude) * GAME_UNIT_TO_M / 1500.0, detailPeriod, seed + 31u);
		// The lumps push the outline sideways as well as up, so towers get overhangs and are not a height graph.
		const float warp = (detail - 0.5) * 0.7 / shell.towerPeriod;
		const float lumps = 0.55 + 0.9 * detail;
		const float top = TopHeight(turn + warp, mask, shell);
		// Positive inside the cloud; grows with depth below the top.
		const float inside = top * lumps - height;
		if (inside <= 0.0)
			return 0.0;
		const float softness = lerp(0.05, 0.22, shell.type);
		const float thickness = saturate(inside / 0.3);
		const float opacity = smoothstep(0.0, softness, inside) * smoothstep(0.0, 0.035, height) *
		                      (1.0 - exp(-max(HorizonCloudLayer.y, 0.0) * (0.6 + 4.0 * thickness)));
		if (opacity <= 0.0)
			return 0.0;

		// Light. How deep the point lies along the way to the light gives sunlit tops and flanks and dim bases;
		// facing the light or standing against it gives a bright body or a dark one with a glowing rim.
		const float3 light = info.dirlightDir;
		const float2 outward = direction.xy / max(horizontal, 1e-4);
		const float along = dot(light.xy, float2(-outward.y, outward.x));
		const float behind = dot(light.xy, outward);
		const float topTowardLight = TopHeight(turn + warp + along * 0.35 / shell.towerPeriod, mask, shell);
		const float depthTowardLight = max(topTowardLight * lumps - (height + light.z * 0.25), 0.0);
		const float lit = exp(-depthTowardLight * HorizonCloudExtra.w);
		const float facing = saturate(0.5 - 0.6 * behind);
		const float3 position = eye + direction * range;
		const float3 sun = GetCloudDirectionalLightColor() * sampleExternalSunTransmittance(position, light);
		const float rim = Phase::HG(dot(direction, light), 0.75) * 0.25 * (1.0 - thickness) * (1.0 - thickness) * CloudDisc().w;
		float3 radiance = sun * (HorizonCloudExtra.z * lerp(0.14, 1.0, facing) * (0.25 + 0.75 * lit) * (0.8 + 0.2 * detail) + rim);
		radiance += SampleCloudAmbientRadiance() * (0.55 + 0.45 * saturate(height / max(top, 1e-3)));
		radiance *= info.cloudLightingScale * max(HorizonCloudLayer.w, 0.0);

		// Air. The aerial perspective LUT ends at AP_MAX_DIST; past it the in-scattered light keeps growing in
		// step with what the air removes, which lets the far end be scaled from the LUT's last slice.
		const SharedData::PhysSkyData data = SharedData::physSkyData;
		const float3 nearT = lerp(1.0, exp(-AirDepth(eye.z, direction, 0.0, AP_MAX_DIST, info.planetRadius)), data.apTrMix);
		const float3 farT = nearT * lerp(1.0, exp(-AirDepth(eye.z, direction, AP_MAX_DIST, max(range, AP_MAX_DIST), info.planetRadius)), data.apTrMix);
		const float3 growth = clamp((1.0 - farT) / max(1.0 - nearT, 1e-4), 1.0, 8.0);
		float3 seen;
		if (unified)
			// The view volume adds the first AP_MAX_DIST of air when the image is composited: only the rest here.
			seen = radiance * (farT / max(nearT, 1e-4)) + ap.rgb * (growth - 1.0) / max(nearT, 0.05);
		else
			seen = radiance * farT + ap.rgb * growth;
		return float4(seen * opacity, opacity);
	}

	/// Adds the band behind the ray-marched clouds of one sky pixel. `transmittance`, `color` (rgb premultiplied,
	/// a = opacity) and `metadata` (x = cloud distance in km) are the filtered cloud result.
	void Composite(uint2 pixel, inout float transmittance, inout float4 color, inout float4 metadata)
	{
		const VolumetricCloudData info = VolumetricCloudBuffer[0];
		[branch] if (HorizonCloudShape.x <= 0.0 || transmittance <= 0.002) return;
		[branch] if (FrameBuffer::ToStandardDepth(TexDepth[pixel]) < 1.0 - 1e-6) return;
		const float3 direction = CloudScreenRay(pixel);
		const float3 eye = FrameBuffer::CameraPosAdjust.xyz - float3(0, 0, info.bottomZ);
		const float distance = HorizonCloudLayer.x;
		// Cheap reject: the band is a few degrees tall.
		const float horizontal = sqrt(saturate(1.0 - direction.z * direction.z));
		[branch] if (horizontal < 0.5) return;
		const float nearAltitude = eye.z + distance * direction.z / horizontal + distance * distance / (2.0 * info.planetRadius);
		const float farScale = max(HorizonCloudExtra.y, 1.0);
		const float farDistance = distance * farScale;
		const float farAltitude = eye.z + farDistance * direction.z / horizontal + farDistance * farDistance / (2.0 * info.planetRadius);
		const float limit = HorizonCloudShape.y + 1.5 * (HorizonCloudShape.z - HorizonCloudShape.y);
		[branch] if (min(nearAltitude, farAltitude) > limit || max(nearAltitude, farAltitude) < HorizonCloudShape.y) return;

		const bool unified = ViewAtmosphere::Enabled();
		const float4 ap = SampleCloudAerialPerspective(direction, AP_MAX_DIST, 0.0);
		const uint seed = uint(max(HorizonCloudExtra.x, 0.0));
		// The far shell turns more slowly: the same wind over a longer way round.
		const float4 front = Sample(direction, eye, distance, seed, HorizonCloudLayer.z, ap, unified);
		float4 band = front;
		[branch] if (front.a < 0.995)
		{
			const float4 back = Sample(direction, eye, farDistance, seed + 500u, HorizonCloudLayer.z / farScale + 0.37, ap, unified);
			band = float4(front.rgb + (1.0 - front.a) * back.rgb, 1.0 - (1.0 - front.a) * (1.0 - back.a));
		}
		if (band.a <= 0.0)
			return;
		// The band takes the place of the sky behind the ray-marched cloud. The composite reads one distance per
		// pixel for the air in front of the cloud: weigh the two by what each contributes.
		const float nearWeight = 1.0 - transmittance;
		const float bandWeight = transmittance * band.a;
		metadata.x = (metadata.x * nearWeight + EncodeCloudDepth(distance / horizontal) * bandWeight) / max(nearWeight + bandWeight, 1e-6);
		color.rgb = min(color.rgb + transmittance * band.rgb, 65504.0);
		transmittance *= 1.0 - band.a;
		color.a = 1.0 - transmittance;
	}
}

#endif
