#define COMPUTESHADER
#define PS_LINEAR_SHADOW_SAMPLER
#include "Common/Random.hlsli"
#include "Common/ViewAtmosphere.hlsli"
#include "PhysicalSky/Common.hlsli"

SamplerComparisonState ShadowSampler : register(s1);
Texture3D<float4> MediumHistory : register(t0);
RWTexture3D<float4> MediumSource : register(u0);
cbuffer MediumDispatch : register(b0)
{
	uint MediumLevel;
	uint3 MediumPadding;
};
#include "ViewAtmosphere/MediumLighting.hlsli"

#ifdef GROUND_FOG
#	include "GroundFog/GroundFog.hlsli"
#endif

#ifdef EXP_HEIGHT_FOG
#	define VIEW_ATMOSPHERE_INJECTION
#	include "ExponentialHeightFog/VolumetricFogCommon.hlsli"
#	include "IBL/IBL.hlsli"
TextureCube<float3> MediumEnvironment : register(t31);
#	define SKYLIGHTING_PROBE_REGISTER t50
#	include "Skylighting/Skylighting.hlsli"
#	ifdef LIGHT_LIMIT_FIX
#		include "LightLimitFix/LightLimitFix.hlsli"
Texture2DArray<float> EngineLocalShadowMaps : register(t5);
cbuffer EngineLocalShadowCB : register(b1)
{
	LightLimitFix::LocalShadowData EngineLocalShadows[4];
};
#	endif
#	define VolumetricFogGridSize ViewAtmosphere::Data[0].grids[MediumLevel].size.xyz
#	define VolumetricFogInvGridSize (1.0 / VolumetricFogGridSize)
#	define VolumetricFogHasDirectionalShadowMap ((ViewAtmosphere::Data[0].flags.z & 1u) != 0u)
#	define VolumetricFogHasIBL ((ViewAtmosphere::Data[0].flags.z & 4u) != 0u)
#	define VolumetricFogHasSkylighting ((ViewAtmosphere::Data[0].flags.z & 8u) != 0u)
#	define VolumetricFogHasLocalLights ((ViewAtmosphere::Data[0].flags.z & 32u) != 0u)
#	define VolumetricFogHasLocalShadowCache ((ViewAtmosphere::Data[0].flags.z & 64u) != 0u)
#	define VolumetricFogHasEngineLocalShadows ((ViewAtmosphere::Data[0].flags.z & 128u) != 0u)
#	define VolumetricFogStateFrameIndexMod8 (SharedData::FrameCount & 7u)
uint4 Rand4DPCG32(int4 p)
{
	uint4 v = uint4(p) * 1664525u + 1013904223u;
	v.x += v.y * v.w;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	v.w += v.y * v.z;
	v ^= v >> 16u;
	v.x += v.y * v.w;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	v.w += v.y * v.z;
	return v;
}
namespace ExponentialHeightFog
{
	float3 ComputeCellWorldPosition(uint3 coord, float3 offset, out float viewDepth)
	{
		const float3 position = ViewAtmosphere::Position(ViewAtmosphere::Data[0].grids[MediumLevel], coord, offset);
		viewDepth = abs(mul(FrameBuffer::CameraView, float4(position, 1)).z);
		return position;
	}
}
#	include "ExponentialHeightFog/VolumetricFogLighting.hlsli"

float4 AnalyticalFogSource(float3 position, float3 materialPosition, float visibility)
{
	const SharedData::ExponentialHeightFogSettings fog = SharedData::exponentialHeightFogSettings;
	const bool weatherFog = fog.useVanillaFogSettings != 0;
	const float materialDistance = length(materialPosition);
	if (materialDistance < fog.startDistance)
		return 0.0;
	const float height = materialPosition.z + FrameBuffer::CameraPosAdjust.z;
	const float densityScale = weatherFog ? fog.volumetricFogExtinctionScale : 1.0;
	const float density = densityScale * (ExponentialHeightFog::GetHeightFogDensity() * exp2(-fog.fogHeightFalloff * 0.001 * max(height - fog.fogHeight, 0.0)) +
											 ExponentialHeightFog::GetHeightFogDensity2() * exp2(-fog.fogHeightFalloff2 * 0.001 * max(height - fog.fogHeight2, 0.0)));
	const float extinction = density * 0.48045301392;
	if (extinction <= 0.0)
		return 0.0;
	float3 color = fog.fogInscatteringColor.rgb * fog.fogInscatteringColor.a;
	if (weatherFog)
		color += ExponentialHeightFog::GetFogAmbientColor(materialDistance);
	const float3 direction = normalize(position);
	const float3 cubeDirection = normalize(lerp(position, float3(0, 0, 1), saturate((fog.cubemapMipLevel + 1.0) / 9.0)));
	float3 environment = 0.0;
	if (!weatherFog && !SharedData::InInterior && fog.useSkyIBL && VolumetricFogHasIBL)
		environment = ImageBasedLighting::GetSkyIBLColor(cubeDirection);
	if (!weatherFog && SharedData::InInterior && fog.useDynamicCubemaps && (ViewAtmosphere::Data[0].flags.z & 1024u))
		environment = MediumEnvironment.SampleLevel(LinearSampler, cubeDirection, fog.cubemapMipLevel).rgb;
	color += environment * fog.inscatteringTint.rgb * fog.inscatteringTint.a;
	const float directionalScale = fog.directionalInscatteringMultiplier * (weatherFog ? fog.fogLightingInfluence : 1.0);
	color += ComputeDirectionalLightColor(position) * ExponentialHeightFog::HenyeyGreenstein(dot(GetFogLightDirection(), direction), fog.directionalInscatteringAnisotropy) * directionalScale * visibility;
	return float4(max(ColorManagement::SceneToLinear(color), 0.0) * extinction, extinction);
}
#endif

[numthreads(8, 8, 4)] void main(uint3 coord : SV_DispatchThreadID) {
	const ViewAtmosphere::Parameters info = ViewAtmosphere::Data[0];
	const ViewAtmosphere::Grid grid = info.grids[MediumLevel];
	if (any(coord >= grid.size.xyz))
		return;
	const uint2 seed = Random::pcg2d(coord.xy + uint2(coord.z * 313u, MediumLevel * 997u));
	const float jitter = lerp(0.5, Random::R2Modified(SharedData::FrameCountAlwaysActive, seed / 4294967295.0).x, info.history.y);
	const float2 xyJitter = Random::R2Modified(SharedData::FrameCountAlwaysActive, seed.yx / 4294967295.0) - 0.5;
	const float3 offset = float3(0.5 + xyJitter * info.history.z, jitter);
	const float3 position = ViewAtmosphere::Position(grid, coord, offset);
	const float3 center = ViewAtmosphere::Position(grid, coord, 0.5);
	const float3 direction = normalize(position);
	float atmosphereVisibility;
	float3 diffuseVisibility;
	const float visibility = MediumVisibility(position, atmosphereVisibility, diffuseVisibility);
	const SharedData::PhysSkyData sky = SharedData::physSkyData;
	float3 rayleigh, mie, extinction;
	PhysSky::GetMediumCoefficients(position, rayleigh, mie, extinction);
	float3 light = AtmosphereLight(0u, position, direction, sky.sunDir, sky.sunlightColor, rayleigh, mie, atmosphereVisibility, diffuseVisibility);
	light += AtmosphereLight(1u, position, direction, sky.masserDir, sky.masserColor, rayleigh, mie, atmosphereVisibility, diffuseVisibility);
	light += AtmosphereLight(2u, position, direction, sky.secundaDir, sky.secundaColor, rayleigh, mie, atmosphereVisibility, diffuseVisibility);
	light *= sky.apLumMix;
	float fogExtinction = 0.0;
#ifdef EXP_HEIGHT_FOG
	if (info.flags.y != 0u) {
		float4 fog;
		if (SharedData::exponentialHeightFogSettings.volumetricFogEnabled) {
			fog = ComputeLightScattering(coord, offset, visibility, center);
			float fade = 1.0;
			if (SharedData::exponentialHeightFogSettings.useVanillaFogSettings == 0)
				fade = saturate((length(center) - SharedData::exponentialHeightFogSettings.volumetricFogStartDistance) /
								max(SharedData::exponentialHeightFogSettings.volumetricFogNearFadeInDistance, 1.0));
			fog *= fade;
		} else {
			fog = AnalyticalFogSource(position, center, visibility);
		}
		fog *= 1000.0 / GAME_UNIT_TO_M;
		light += fog.rgb;
		fogExtinction = fog.a;
	}
#endif
#ifdef GROUND_FOG
	// Added once the atmosphere light is final, so a later change to the atmosphere terms cannot scale ground fog.
	if (info.flags.z & 2048u) {
		GroundFog::Cell groundFog;
		if (GroundFog::EvaluateCell(grid, coord, MediumLevel, position, offset.z, groundFog)) {
			const float perKm = 1000.0 / GAME_UNIT_TO_M;
			float groundFogAtmosphere;
			float3 groundFogDiffuse;
			// Visibility inside the layer: the cell's own sample is often under the ground.
			const float groundFogVisibility = MediumVisibility(groundFog.lightPos, groundFogAtmosphere, groundFogDiffuse);
			const float3 scattering = groundFog.extinction * perKm * groundFog.albedo;
			const float3 viewDirection = normalize(center);
			float3 groundFogLight = AtmosphereLight(0u, groundFog.lightPos, viewDirection, sky.sunDir, sky.sunlightColor, 0.0, scattering,
				groundFogVisibility * GroundFog::SelfShadowing(groundFog.tauAbove, sky.sunDir.z), groundFogDiffuse);
			groundFogLight += AtmosphereLight(1u, groundFog.lightPos, viewDirection, sky.masserDir, sky.masserColor, 0.0, scattering,
				groundFogVisibility * GroundFog::SelfShadowing(groundFog.tauAbove, sky.masserDir.z), groundFogDiffuse);
			groundFogLight += AtmosphereLight(2u, groundFog.lightPos, viewDirection, sky.secundaDir, sky.secundaColor, 0.0, scattering,
				groundFogVisibility * GroundFog::SelfShadowing(groundFog.tauAbove, sky.secundaDir.z), groundFogDiffuse);
			groundFogLight *= GroundFog::SunScale;
			// Sky light reaches fog in shadow too, so it takes no direct visibility.
			groundFogLight += scattering * GroundFog::SkyMean() * GroundFog::SkyAmbientScale;
			light += groundFogLight;
			fogExtinction += groundFog.extinction * perKm;
		}
	}
#endif
	float4 source = float4(max(light, 0.0), max(fogExtinction, 0.0));
	const float3 previous = center + FrameBuffer::CameraPosAdjust.xyz - FrameBuffer::CameraPreviousPosAdjust.xyz;
	const float4 clip = mul(FrameBuffer::CameraPreviousViewProjUnjittered, float4(previous, 1));
	const float2 uv = clip.xy / max(clip.w, 1e-6) * float2(0.5, -0.5) + 0.5;
	const float previousDistance = length(previous);
	if (info.history.x > 0.0 && SharedData::ResetHistory == 0u && clip.w > 0.0 && all(uv > 0.0) && all(uv < 1.0) && previousDistance >= grid.depth.x && previousDistance < grid.depth.y) {
		const float slice = ViewAtmosphere::SourceSlice(grid, previousDistance);
		const float3 historyUv = ViewAtmosphere::AtlasUv(grid, uv, clamp(slice, 0.0, float(grid.size.z - 1u)));
		const float4 old = MediumHistory.SampleLevel(LinearSampler, historyUv, 0);
		const float extinctionChange = abs(old.a - source.a) / max(max(old.a, source.a), 1e-6);
		const float historyWeight = info.history.x * saturate(1.0 - extinctionChange * 4.0);
		if (all(isfinite(old)))
			source = lerp(source, old, historyWeight);
	}
	MediumSource[coord + grid.offset.xyz] = min(source, 65504.0);
}
