// Light Shafts, step 1: what every pixel of the march shares. One thread.
//
// - The light's colour at the camera after the atmosphere, from the sky's transmittance LUT: gold at a low sun.
// - The share of lit air around the camera, from the directional cascades at 64 points in a disc around it. This is
//   what the near march expects air to get; only air that gets more adds light. It is eased over time, so walking
//   from sun into shade does not switch the beams on in one frame.
// - The mean cloud transmittance of the sky, and the cloud and terrain light at the camera itself.
// - How open the sky is on the whole screen, from the mask: what the screen part of the march expects.

#define COMPUTESHADER
#define PS_LINEAR_SHADOW_SAMPLER
#include "Common/ViewAtmosphere.hlsli"
#include "PhysicalSky/Common.hlsli"
#include "ViewAtmosphere/MediumLighting.hlsli"

#include "LightShafts/LightShafts.hlsli"

Texture2D<float4> Previous : register(t45);
RWTexture2D<float4> PreparedOut : register(u0);

[numthreads(1, 1, 1)] void main() {
	const float3 planet = PhysSky::GetMediumPlanetPosition(0.0);
	const float3 transmittance = PhysSky::SampleAtmosphereLightTr(PhysSky::TexTrLut, LinearSampler, planet, LightShafts::LightDir, LightShafts::DiskRadius);
	PreparedOut[uint2(0, 0)] = float4(LightShafts::LightColor * transmittance, 1.0);

	const DirectionalShadowLightData light = DirectionalShadowLights[0];
	static const uint count = 64u;
	float lit = 0.0;
	float covered = 0.0;
	[loop] for (uint i = 0u; i < count; ++i)
	{
		// A sunflower disc at and above eye height: points under the ground would count as shade.
		const float radius = LightShafts::AmbientRadius * sqrt((float(i) + 0.5) / float(count));
		const float angle = float(i) * 2.39996323;
		const float3 position = float3(radius * cos(angle), radius * sin(angle), LightShafts::AmbientHeight * frac(float(i) * 0.61803399 + 0.5));
		bool inside;
		const float value = LightShafts::CascadeLight(position, light, true, inside);
		lit += inside ? value : 0.0;
		covered += inside ? 1.0 : 0.0;
	}
	const float4 previous = Previous[uint2(1, 0)];
	const bool history = LightShafts::AmbientHistory > 0.0;
	// Too few points inside the cascades to tell: hold the last value, or take open ground, which adds nothing.
	float share = covered >= 8.0 ? lit / covered : (history ? previous.x : 1.0);
	if (history)
		share = lerp(share, previous.x, LightShafts::AmbientHistory);

	const float3 camera = FrameBuffer::CameraPosAdjust.xyz;
	const float cloudMean = LightShafts::CloudMeanStored();
	PreparedOut[uint2(1, 0)] = float4(saturate(share), LightShafts::CloudApplied(cloudMean), LightShafts::TerrainLight(camera, 512.0), LightShafts::CloudLight(camera));

	// Light through the sky as a share of the sky on screen: 1 without cloud. Silhouette Rays moves it toward the
	// share of the whole screen, so that terrain and objects against the sky count as contrast too.
	float2 mask = 0.0;
	[loop] for (uint j = 0u; j < 144u; ++j)
		mask += LightShafts::SampleMask((float2(j % 16u, j / 16u) + 0.5) / float2(16.0, 9.0));
	mask /= 144.0;
	const float openSky = mask.y > 0.01 ? saturate(mask.x / mask.y) : 1.0;
	PreparedOut[uint2(2, 0)] = float4(lerp(openSky, mask.x, LightShafts::SilhouetteRays), mask.y, 0.0, 1.0);
}
