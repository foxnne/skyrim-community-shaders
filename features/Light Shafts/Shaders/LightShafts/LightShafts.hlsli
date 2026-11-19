#ifndef LIGHT_SHAFTS_HLSLI
#define LIGHT_SHAFTS_HLSLI

// Light Shafts: beams of sunlight and moonlight in the air ("god rays").
//
// A beam is air that the light reaches next to air that it does not. The pass marches each pixel's view ray through
// the world and asks, at every sample, how much MORE light the air there gets than the air around it is expected
// to get:
// - near the camera (March.cs.hlsl, near part) from the directional shadow cascades, against the share of lit air
//   around the camera (Prepare.cs.hlsl). Open ground: everything is lit, the expectation is 1, nothing is added.
//   Under a canopy: most air is dark, the lit gaps stand out.
// - far away, while the light is on or near the screen, from the screen itself (Mask.cs.hlsl): how open the sky is
//   along the line from the pixel to the light, against how open the sky is on the whole. Clear or overcast sky:
//   nothing. Broken cloud: rays from the gaps. This works at any elevation of the light, sunrise and sunset included.
// - far away, while the light is off the screen, from the cloud shadow volume against the mean cloud transmittance
//   of the sky, and from the terrain shadow against the terrain shadow to both sides of the sample. The volume only
//   holds the shadows of cloud within a few kilometers, so this part needs the light well above the horizon.
// What is left is integrated with a weight that falls off with distance, turned into an optical depth by Density and
// lit with the light's colour after the atmosphere (the transmittance LUT), a phase function that concentrates it
// toward the light, and a ceiling. Nothing is accumulated over frames.
//
// Passes: Mask (quarter resolution) -> Prepare (one thread) -> March (half or quarter resolution) -> Resolve (same) -> Composite (full).
//
// Include after Common/ViewAtmosphere.hlsli, PhysicalSky/Common.hlsli and ViewAtmosphere/MediumLighting.hlsli.

#include "LightShafts/Constants.hlsli"

namespace LightShafts
{
	// Output of Prepare. Texel 0: rgb light colour at the camera after the atmosphere. Texel 1: x share of lit air
	// around the camera (cascades), y mean cloud transmittance toward the light, z terrain light and w cloud
	// transmittance at the camera. Texel 2: x how open the sky is on the whole screen.
	Texture2D<float4> Prepared : register(t44);
	// Output of Mask. x: light that gets through from that part of the screen, y: share of it that is sky.
	Texture2D<float2> Mask : register(t47);

	SamplerComparisonState ShadowSampler : register(s1);

	// 4 x 4 ordered offsets in [0, 1). Any 4 x 4 window of pixels holds each of the 16 values once, which is what
	// Resolve averages over.
	float Bayer(uint2 pixel)
	{
		static const float pattern[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
		return (pattern[(pixel.x & 3u) + ((pixel.y & 3u) << 2u)] + 0.5) / 16.0;
	}

	// Henyey-Greenstein, scaled to 1 straight toward the light: Strength is then the brightness there.
	float Phase(float cosine)
	{
		const float g = PhaseG;
		const float base = (1.0 - g) * (1.0 - g) / max(1.0 + g * g - 2.0 * g * cosine, 1e-4);
		return base * sqrt(base);
	}

	// Light through the directional cascades at a camera-relative point. `covered` is false where no cascade holds
	// the point: the caller decides what the air is worth there.
	float CascadeLight(float3 position, DirectionalShadowLightData light, bool byBounds, out bool covered)
	{
		covered = false;
		if (!(ViewAtmosphere::Data[0].flags.z & 1u) || light.EndSplitDistances.y <= 0.0)
			return 1.0;
		const float3 absolute = position + FrameBuffer::CameraPosAdjust.xyz;
		uint cascade = 0u;
		float3 posLS = 0.0;
		if (byBounds) {
			// Around the camera, where a view depth means nothing behind it: the first cascade that holds the point.
			posLS = mul(light.ShadowProj[0], float4(absolute, 1)).xyz;
			if (any(posLS.xy < 0.02) || any(posLS.xy > 0.98)) {
				cascade = 1u;
				posLS = mul(light.ShadowProj[1], float4(absolute, 1)).xyz;
			}
		} else {
			const float depth = SharedData::GetScreenDepth(FrameBuffer::GetShadowDepth(position));
			if (depth >= light.EndSplitDistances.y)
				return 1.0;
			cascade = depth >= light.EndSplitDistances.x ? 1u : 0u;
			posLS = mul(light.ShadowProj[cascade], float4(absolute, 1)).xyz;
		}
		if (any(posLS.xy < 0.0) || any(posLS.xy > 1.0))
			return 1.0;
		covered = true;
		return DirectionalShadowMap.SampleCmpLevelZero(ShadowSampler, float3(posLS.xy, cascade), posLS.z - ShadowBias);
	}

	// Terrain shadow at an absolute point, one read, with the edge widened by `blur` game units in height: air does
	// not need the sharp edge a surface does, and the wide one hides the steps of the height map.
	float TerrainLight(float3 absolute, float blur)
	{
#ifdef TERRAIN_SHADOWS
		if (!(ViewAtmosphere::Data[0].flags.z & 256u) || !SharedData::terraOccSettings.EnableTerrainShadow)
			return 1.0;
		const float2 uv = TerrainShadows::GetTerrainShadowUV(absolute.xy);
		if (any(uv < 0.0) || any(uv > 1.0))
			return 1.0;
		const float2 heights = TerrainShadows::GetTerrainZ(TerrainShadows::ShadowHeightTexture.SampleLevel(LinearSampler, uv, 0));
		const float zBlur = max(SharedData::terraOccSettings.ZBlur, blur);
		const float lower = heights.y - zBlur;
		return saturate((absolute.z - lower) / max(heights.x + zBlur - lower, 1.0));
#else
		return 1.0;
#endif
	}

	bool CloudsActive()
	{
		return CloudOcclusion > 0.0 && SharedData::physSkyData.enableVolumetricClouds && SharedData::physSkyData.volCloudLowThickness > 0.0;
	}

	// Mean transmittance of the cloud layer toward the light, at the ground, as stored. 1 without clouds.
	float CloudMeanStored()
	{
		if (!CloudsActive())
			return 1.0;
		// Row 0 is the header of the profile (w: valid), row 1 its lowest altitude.
		if (CloudShadowVolume::MeanProfile.Load(int3(0, 0, 0)).w == 0.0)
			return 1.0;
		return saturate(CloudShadowVolume::MeanProfile.Load(int3(0, 1, 0)).x);
	}

	// What the beams take a cloud transmittance for, at the current Cloud Occlusion.
	float CloudApplied(float transmittance)
	{
		return lerp(1.0, transmittance, CloudOcclusion);
	}

	// Cloud transmittance toward the light at an absolute point: the shadow volume where it holds the whole path
	// through the cloud, the mean of the sky where it does not, so that air there neither stands out nor goes dark.
	float CloudLight(float3 absolute)
	{
		if (!CloudsActive())
			return 1.0;
		return CloudApplied(saturate(PhysSky::GetDirlightTransmittance(absolute, LinearSampler, true).x));
	}

	// Mask at a screen position (0..1, undistorted by dynamic resolution).
	float2 SampleMask(float2 screenUv)
	{
		uint2 size;
		Mask.GetDimensions(size.x, size.y);
		const float2 active = max(floor(float2(size) * FrameBuffer::DynamicResolutionParams1.xy), 1.0);
		return Mask.SampleLevel(LinearSampler, clamp(screenUv * active, 0.5, active - 0.5) / float2(size), 0);
	}
}

#endif
