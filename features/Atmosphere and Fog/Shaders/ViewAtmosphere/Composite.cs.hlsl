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
// Ground Fog's own pass, at full or half resolution. Unbound when there is none this frame.
// TexGroundFog     rgb: light scattered toward the camera per unit of opacity, a: transmittance to the far depth
// TexGroundFogAux  x, y: nearest and farthest view depth of the scene pixels the texel stands for (/ 64),
//                  z: transmittance to the nearest depth
Texture2D<float4> TexGroundFog : register(t42);
Texture2D<float4> TexGroundFogAux : register(t43);

// Returns light toward the camera (rgb) and transmittance (a) for a pixel at a device depth.
float4 SampleGroundFog(uint2 pixel, uint2 size, uint2 activeSize, float depth)
{
	uint2 fogSize;
	TexGroundFog.GetDimensions(fogSize.x, fogSize.y);
	if (fogSize.x == 0u)
		return float4(0.0, 0.0, 0.0, 1.0);
	if (fogSize.x >= size.x) {
		const float4 own = TexGroundFog[pixel];
		return float4(own.rgb * (1.0 - own.a), own.a);
	}

	const uint2 activeFog = (activeSize + 1u) / 2u;
	const float2 position = (float2(pixel) + 0.5) * 0.5 - 0.5;
	// Most of the screen holds no fog: one filtered read of the four texels below settles that (the transmittance
	// to the far depth is the lowest a texel holds).
	const float2 uv = clamp(position + 0.5, 0.5, float2(activeFog) - 0.5) / float2(fogSize);
	if (TexGroundFog.SampleLevel(SampColorSampler, uv, 0).a >= 0.9995)
		return float4(0.0, 0.0, 0.0, 1.0);

	// Every texel knows the fog in front of the nearest and of the farthest of its four scene pixels, from one
	// ray. That gives the optical depth as a function of depth along that ray: rising from nothing at the camera
	// to the near value, on to the far value, and level behind it. Each of the four texels around the pixel is
	// read off at the pixel's own depth and the four are blended bilinearly. So a thin blade in front of far
	// ground, or a gap of far ground between blades, gets the fog for its own depth from every texel, and the
	// result does not depend on which scene pixel a texel happened to pick. A texel whose range does not hold the
	// pixel's depth is only a guess and counts less, but never nothing: there is always a valid sample.
	const int2 base = int2(floor(position));
	const float2 fraction = position - float2(base);
	const float viewDepth = min(SharedData::GetScreenDepth(depth) / 64.0, 65000.0);
	float4 sum = 0.0;
	float weightSum = 0.0;
	[unroll] for (uint i = 0u; i < 4u; ++i)
	{
		const int2 texel = clamp(base + int2(i & 1u, i >> 1u), 0, int2(activeFog) - 1);
		const float4 fog = TexGroundFog[texel];
		const float4 aux = TexGroundFogAux[texel];
		const float tauNear = -log(max(aux.z, 1e-4));
		const float tauFar = -log(max(fog.a, 1e-4));
		float tau;
		if (viewDepth <= aux.x)
			tau = tauNear * viewDepth / max(aux.x, 1e-3);
		else if (viewDepth >= aux.y)
			tau = tauFar;
		else
			tau = lerp(tauNear, tauFar, (viewDepth - aux.x) / max(aux.y - aux.x, 1e-6));
		const float outside = max(max(aux.x - viewDepth, viewDepth - aux.y), 0.0) / max(viewDepth, 1e-3);
		const float2 side = lerp(1.0 - fraction, fraction, float2(i & 1u, i >> 1u));
		const float weight = (side.x * side.y + 0.02) / (1.0 + 400.0 * outside * outside);
		const float transmittance = exp(-tau);
		sum += float4(fog.rgb * (1.0 - transmittance), transmittance) * weight;
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
