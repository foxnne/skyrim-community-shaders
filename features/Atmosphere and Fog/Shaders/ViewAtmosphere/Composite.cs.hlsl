#define COMPUTESHADER
#define PS_SKY_SAMPLERS
#define MEDIUM_COMPOSITE
SamplerState SampColorSampler : register(s0);
#include "Common/SharedData.hlsli"
#ifdef PHYSICAL_SKY
#	include "PhysicalSky/Common.hlsli"
#endif
#ifdef EXP_HEIGHT_FOG
#	include "ExponentialHeightFog/ExponentialHeightFog.hlsli"
#endif
#include "Common/ViewMedium.hlsli"

Texture2D<float> TexMediumDepth : register(t40);
RWTexture2D<float4> MainRW : register(u0);

#ifdef GROUND_FOG
// Ground Fog's own pass: light scattered toward the camera (rgb) and transmittance (a), at full or half resolution,
// and the device depth each of its pixels was computed for. Unbound when there is none this frame.
Texture2D<float4> TexGroundFog : register(t42);
Texture2D<float> TexGroundFogDepth : register(t43);

float4 SampleGroundFog(uint2 pixel, uint2 size, uint2 activeSize, float depth)
{
	uint2 fogSize;
	TexGroundFog.GetDimensions(fogSize.x, fogSize.y);
	if (fogSize.x == 0u)
		return float4(0.0, 0.0, 0.0, 1.0);
	if (fogSize.x >= size.x)
		return TexGroundFog[pixel];

	const uint2 activeFog = (activeSize + 1u) / 2u;
	const float2 position = (float2(pixel) + 0.5) * 0.5 - 0.5;
	// Most of the screen holds no fog: one filtered read of the four texels below settles that.
	const float2 uv = clamp(position + 0.5, 0.5, float2(activeFog) - 0.5) / float2(fogSize);
	if (TexGroundFog.SampleLevel(SampColorSampler, uv, 0).a >= 0.9995)
		return float4(0.0, 0.0, 0.0, 1.0);

	// Bilinear weights, cut down for texels computed for another depth, so fog does not leak across the outline of
	// whatever stands in front of it. On one surface seen at a shallow angle the four depths differ too, row by
	// row; there the tolerance widens to their spread, so the result stays the smooth bilinear one and does not
	// snap to the nearest row, which would show as steps two pixels high.
	const int2 base = int2(floor(position));
	const float2 fraction = position - float2(base);
	const float viewDepth = SharedData::GetScreenDepth(depth);
	float4 fog[4];
	float depths[4];
	float nearest = 1.0e30;
	float farthest = 0.0;
	uint i;
	[unroll] for (i = 0u; i < 4u; ++i)
	{
		const int2 texel = clamp(base + int2(i & 1u, i >> 1u), 0, int2(activeFog) - 1);
		fog[i] = TexGroundFog[texel];
		depths[i] = SharedData::GetScreenDepth(TexGroundFogDepth[texel]);
		nearest = min(nearest, depths[i]);
		farthest = max(farthest, depths[i]);
	}
	const float tolerance = clamp((farthest - nearest) / max(nearest, 1.0), 0.03, 0.15);
	float4 sum = 0.0;
	float weightSum = 0.0;
	[unroll] for (i = 0u; i < 4u; ++i)
	{
		const float difference = abs(depths[i] - viewDepth) / max(min(depths[i], viewDepth), 1.0);
		const float2 side = lerp(1.0 - fraction, fraction, float2(i & 1u, i >> 1u));
		const float weight = (side.x * side.y + 0.02) / (tolerance * tolerance + difference * difference);
		sum += fog[i] * weight;
		weightSum += weight;
	}
	return sum / weightSum;
}
#endif

[numthreads(8, 8, 1)] void main(uint2 pixel : SV_DispatchThreadID) {
	uint2 size;
	MainRW.GetDimensions(size.x, size.y);
	const uint2 activeSize = uint2(clamp(float2(size) * FrameBuffer::DynamicResolutionParams1.xy, 1.0, float2(size)));
	if (any(pixel >= activeSize))
		return;
	const float2 uv = FrameBuffer::GetDynamicResolutionUnadjustedScreenPosition((float2(pixel) + 0.5) / float2(size));
	const float depth = TexMediumDepth[pixel];
	const bool fullCloudRay = FrameBuffer::ToStandardDepth(depth) >= 1.0 - 1e-6;
	float4 position = mul(FrameBuffer::CameraViewProjInverse, float4(uv * float2(2, -2) + float2(-1, 1), depth, 1));
	position.xyz /= position.w;
	float4 color = MainRW[pixel];
	float3 scene = PreExposure::Decode(color.rgb);
#ifdef GROUND_FOG
	// Under the atmosphere: the fog lies on the ground close by, so the haze between it and the camera goes over it.
	const float4 groundFog = SampleGroundFog(pixel, size, activeSize, depth);
	if (groundFog.a < 1.0 || any(groundFog.rgb > 0.0))
		scene = ColorManagement::SceneColor::ScaleAndAddLinear(scene, groundFog.a, groundFog.rgb);
#endif
	color.rgb = PreExposure::Encode(ViewMedium::CompositeViewMedium(scene, position.xyz, uv, SampColorSampler, fullCloudRay));
	MainRW[pixel] = color;
}
