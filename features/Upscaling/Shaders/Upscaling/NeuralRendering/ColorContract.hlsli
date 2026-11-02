#include "Upscaling/NeuralRendering/ColorCompat.hlsli"
#ifndef __NR_COLOR_CONTRACT_HLSLI__
#define __NR_COLOR_CONTRACT_HLSLI__

// Include after Common/Color.hlsli: ENABLE_LL selects the gain's output domain.
// This tree selects linear lighting and ACEScg at compile time (ENABLE_LL / ENABLE_ACESCG are
// injected as defines when the Linear Lighting feature is on); Open Shaders reads them at runtime.
#if defined(ENABLE_LL)
#	define NR_LINEAR_LIGHTING 1
#else
#	define NR_LINEAR_LIGHTING 0
#endif
#if defined(ENABLE_ACESCG)
#	define NR_ACESCG 1
#else
#	define NR_ACESCG 0
#endif
namespace NR
{
	// Tone clamp in stops: bounded so a bad model frame cannot blow out the frame.
	static const float kMaxToneStops = 2.0f;

	// exp2 of the tone in the source's own domain: tone 0 stays an exact gain-1 passthrough.
	float CompositeGain(float tone, float maxToneStops)
	{
		tone = isfinite(tone) ? tone : 0.0;
		float gain = exp2(clamp(tone, -maxToneStops, maxToneStops));
		return NR_LINEAR_LIGHTING ? gain : NRColor::LinearToSkyrimGamma(gain);
	}
}

#endif  // __NR_COLOR_CONTRACT_HLSLI__
