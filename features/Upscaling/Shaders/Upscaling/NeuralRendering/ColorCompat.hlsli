#ifndef __NR_COLOR_COMPAT_HLSLI__
#define __NR_COLOR_COMPAT_HLSLI__

#include "Common/Color.hlsli"

// Color helpers Neural Rendering takes from Open Shaders' Common/Color.hlsli. Kept local to NR
// so the shared Color.hlsli (and every lighting shader's cache entry) stays unchanged.
namespace NRColor
{
	static const float3 kRec709LuminanceWeights = float3(0.2125, 0.7154, 0.0721);

	// Skyrim's display gamma.
	static const float GameGamma = 1.6;

	float RGBToLuminance(float3 color, float3 luminanceWeights)
	{
		return dot(color, luminanceWeights);
	}

	float RGBToLuminance(float3 color)
	{
		return Color::RGBToLuminance(color);
	}

	float SkyrimGammaToLinear(float color)
	{
		return pow(abs(color), GameGamma);
	}

	float LinearToSkyrimGamma(float color)
	{
		return pow(abs(color), 1.0 / GameGamma);
	}

	float3 SkyrimGammaToLinear(float3 color)
	{
		return pow(abs(color), GameGamma);
	}

	float3 LinearToSkyrimGamma(float3 color)
	{
		return pow(abs(color), 1.0 / GameGamma);
	}
}

#endif  // __NR_COLOR_COMPAT_HLSLI__
