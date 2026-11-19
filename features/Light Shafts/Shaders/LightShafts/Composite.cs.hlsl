// Light Shafts, step 4: adds the beams to the scene, at full resolution.
//
// Runs right after the atmosphere's post-water composite: the scene holds opaque surfaces, water, fog, air and
// clouds, and nothing transparent or upscaled yet. The beams are light added on top, in scene-linear units.

#define COMPUTESHADER
#include "Common/SharedData.hlsli"
#include "Common/ColorManagement.hlsli"
#include "Common/PreExposure.hlsli"

#include "LightShafts/Constants.hlsli"

SamplerState LinearSampler : register(s0);
Texture2D<float> SceneDepth : register(t40);
Texture2D<float4> Shafts : register(t45);
Texture2D<float> ShaftsDepth : register(t46);
RWTexture2D<float4> MainRW : register(u0);

[numthreads(8, 8, 1)] void main(uint2 pixel : SV_DispatchThreadID) {
	uint2 size, shaftsSize;
	MainRW.GetDimensions(size.x, size.y);
	Shafts.GetDimensions(shaftsSize.x, shaftsSize.y);
	const uint2 activeSize = uint2(clamp(float2(size) * FrameBuffer::DynamicResolutionParams1.xy, 1.0, float2(size)));
	if (any(pixel >= activeSize))
		return;

	const float divisor = float(LightShafts::Divisor);
	const uint2 activeShafts = min((activeSize + LightShafts::Divisor - 1u) / LightShafts::Divisor, shaftsSize);
	const float2 position = (float2(pixel) + 0.5) / divisor - 0.5;
	const bool debug = LightShafts::DebugMode != 0u;
	// Most of the screen holds no beams: one filtered read of the four pixels below settles that.
	const float2 uv = clamp(position + 0.5, 0.5, float2(activeShafts) - 0.5) / float2(shaftsSize);
	if (!debug && !any(Shafts.SampleLevel(LinearSampler, uv, 0).rgb > 1.0e-5))
		return;

	// Bilinear weights, cut down for pixels computed for another depth. On one surface seen at a shallow angle the
	// four depths differ too; there the tolerance widens to their spread, so the result stays the smooth one.
	const int2 base = int2(floor(position));
	const float2 fraction = position - float2(base);
	const float viewDepth = SharedData::GetScreenDepth(SceneDepth[pixel]);
	float4 shafts[4];
	float depths[4];
	float nearest = 1.0e30;
	float farthest = 0.0;
	uint i;
	[unroll] for (i = 0u; i < 4u; ++i)
	{
		const int2 texel = clamp(base + int2(i & 1u, i >> 1u), 0, int2(activeShafts) - 1);
		shafts[i] = Shafts[texel];
		depths[i] = SharedData::GetScreenDepth(ShaftsDepth[texel]);
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
		sum += shafts[i] * weight;
		weightSum += weight;
	}
	const float4 result = sum / weightSum;

	float4 color = MainRW[pixel];
	float3 scene = PreExposure::Decode(color.rgb);
	if (LightShafts::DebugMode == 1u)
		scene = ColorManagement::LinearToScene(result.aaa);  // lit share of the near ray
	else if (LightShafts::DebugMode == 2u)
		scene = ColorManagement::LinearToScene(result.rgb);  // the beams alone
	else
		scene = ColorManagement::SceneColor::ScaleAndAddLinear(scene, 1.0, result.rgb);
	color.rgb = PreExposure::Encode(scene);
	MainRW[pixel] = color;
}
