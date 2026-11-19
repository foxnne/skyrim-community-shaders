// Light Shafts, step 0: how open the sky is at each part of the screen, at quarter resolution.
//
// x: light that gets through from that direction: the share of the block that is sky, times what the volumetric
//    clouds there let through toward the camera. Terrain, trees and buildings are 0.
// y: the share of the block that is sky.
//
// The distant rays of the march are read from this along the line from each pixel to the light on the screen. It is
// what gives rays through gaps in the cloud at sunrise and sunset, when the light comes in too flat for the cloud
// shadow volume, which only reaches a few kilometers, to hold the shadows that matter.

#define COMPUTESHADER
#include "Common/SharedData.hlsli"

#include "LightShafts/Constants.hlsli"

SamplerState LinearSampler : register(s0);
Texture2D<float> SceneDepth : register(t40);
Texture2D<float> CloudTransmittance : register(t110);  // main view, toward the camera; may be unbound
RWTexture2D<float2> MaskOut : register(u0);

[numthreads(8, 8, 1)] void main(uint2 pixel : SV_DispatchThreadID) {
	uint2 mainSize, outSize;
	SceneDepth.GetDimensions(mainSize.x, mainSize.y);
	MaskOut.GetDimensions(outSize.x, outSize.y);
	const uint2 activeMain = uint2(clamp(float2(mainSize) * FrameBuffer::DynamicResolutionParams1.xy, 1.0, float2(mainSize)));
	if (any(pixel >= min((activeMain + 3u) / 4u, outSize)))
		return;

	float sky = 0.0;
	[unroll] for (uint i = 0u; i < 4u; ++i)
	{
		const uint2 scenePixel = min(pixel * 4u + uint2(i & 1u, i >> 1u) * 2u + ((pixel.x + pixel.y + i) & 1u), activeMain - 1u);
		sky += FrameBuffer::ToStandardDepth(SceneDepth[scenePixel]) >= 1.0 - 1e-6 ? 0.25 : 0.0;
	}

	float cloud = 1.0;
	uint2 cloudSize;
	CloudTransmittance.GetDimensions(cloudSize.x, cloudSize.y);
	[branch] if (sky > 0.0 && cloudSize.x > 0u && LightShafts::CloudOcclusion > 0.0 && SharedData::physSkyData.enabled && SharedData::physSkyData.enableVolumetricClouds)
	{
		// The cloud targets share the scene's dynamic resolution but not always its size.
		const float2 uv = (float2(pixel) * 4.0 + 2.0) / float2(mainSize);
		const float2 maximum = max(floor(float2(cloudSize) * FrameBuffer::DynamicResolutionParams1.xy) - 0.5, 0.5);
		const float2 sampleUv = clamp(uv * float2(cloudSize), 0.5, maximum) / float2(cloudSize);
		cloud = lerp(1.0, saturate(CloudTransmittance.SampleLevel(LinearSampler, sampleUv, 0)), LightShafts::CloudOcclusion);
	}
	MaskOut[pixel] = float2(sky * cloud, sky);
}
