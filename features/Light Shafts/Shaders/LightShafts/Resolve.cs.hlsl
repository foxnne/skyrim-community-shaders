// Light Shafts, step 3: averages the 16 step offsets of the march.
//
// Each output pixel takes the 4 x 4 pixels around it, which hold every offset once, and leaves out those computed
// for another depth so beams do not leak across the outline of whatever stands in front of them.

#define COMPUTESHADER
#include "Common/SharedData.hlsli"

#include "LightShafts/Constants.hlsli"

Texture2D<float> SceneDepth : register(t40);
Texture2D<float4> Shafts : register(t45);
Texture2D<float> ShaftsDepth : register(t46);
RWTexture2D<float4> ResolvedOut : register(u0);

[numthreads(8, 8, 1)] void main(uint2 pixel : SV_DispatchThreadID) {
	uint2 mainSize, outSize;
	SceneDepth.GetDimensions(mainSize.x, mainSize.y);
	ResolvedOut.GetDimensions(outSize.x, outSize.y);
	const uint2 activeMain = uint2(clamp(float2(mainSize) * FrameBuffer::DynamicResolutionParams1.xy, 1.0, float2(mainSize)));
	const uint2 activeOut = min((activeMain + LightShafts::Divisor - 1u) / LightShafts::Divisor, outSize);
	if (any(pixel >= activeOut))
		return;

	const float centre = SharedData::GetScreenDepth(ShaftsDepth[pixel]);
	float4 sum = 0.0;
	float weightSum = 0.0;
	[unroll] for (int y = -2; y < 2; ++y)
	{
		[unroll] for (int x = -2; x < 2; ++x)
		{
			const int2 texel = clamp(int2(pixel) + int2(x, y), 0, int2(activeOut) - 1);
			const float depth = SharedData::GetScreenDepth(ShaftsDepth[texel]);
			const float difference = abs(depth - centre) / max(min(depth, centre), 1.0);
			// Full weight within 5 % of the depth, none beyond 10 %. The pixel itself always counts.
			const float weight = saturate(2.0 - difference * 20.0);
			sum += Shafts[texel] * weight;
			weightSum += weight;
		}
	}
	ResolvedOut[pixel] = sum / max(weightSum, 1e-4);
}
