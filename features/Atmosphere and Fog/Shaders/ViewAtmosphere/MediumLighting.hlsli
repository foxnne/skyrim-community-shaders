#ifndef VIEW_ATMOSPHERE_MEDIUM_LIGHTING_HLSLI
#define VIEW_ATMOSPHERE_MEDIUM_LIGHTING_HLSLI

// How the shared atmosphere volume lights a point of a medium: direct visibility of the sun (cascades, terrain
// shadow, cloud layer and cloud shadow volume) and the light of the sun and moons scattered by it. Shared by the
// volume's injection pass and by passes that light their own medium the same way (Ground Fog), so they cannot drift
// apart.
//
// Include after Common/ViewAtmosphere.hlsli and PhysicalSky/Common.hlsli. ViewAtmosphere::Data must be bound: its
// flags say which of the inputs below are valid this frame.

SamplerState LinearSampler : register(s0);
Texture2DArray<float4> DirectionalShadowMap : register(t1);
Texture2D<float4> MediumMsLut : register(t2);
struct DirectionalShadowLightData
{
	column_major float4x4 ShadowProj[2];
	column_major float4x4 InvShadowProj[2];
	float2 EndSplitDistances;
	float2 StartSplitDistances;
};
StructuredBuffer<DirectionalShadowLightData> DirectionalShadowLights : register(t98);

#if defined(TERRAIN_SHADOWS)
#	include "TerrainShadows/TerrainShadows.hlsli"
#endif

#ifdef TERRAIN_SHADOWS
// Terrain shadow for the medium. The surface version is a hard height test with a small blur, which in haze shows the
// shadow-height map's steps as terraces behind distant mountains at low sun. Haze does not need a sharp edge, so this
// widens the transition in height and averages a few height-map texels, both growing with distance from the camera.
float MediumTerrainShadow(float3 worldPos, float viewDistance)
{
	if (!SharedData::terraOccSettings.EnableTerrainShadow)
		return 1.0;
	const float2 uv = TerrainShadows::GetTerrainShadowUV(worldPos.xy);
	if (any(uv < 0.0) || any(uv > 1.0))
		return 1.0;
	uint width, height;
	TerrainShadows::ShadowHeightTexture.GetDimensions(width, height);
	const float2 texel = 1.0 / float2(width, height);
	const float radius = clamp(viewDistance / 6000.0, 1.0, 8.0);  // texels (128 units each on Tamriel)
	const float zBlur = max(SharedData::terraOccSettings.ZBlur, clamp(viewDistance * 0.05, 256.0, 4000.0));
	static const float2 taps[5] = { float2(0, 0), float2(1, 0.5), float2(-0.5, 1), float2(-1, -0.5), float2(0.5, -1) };
	float shadow = 0.0;
	[unroll] for (uint i = 0; i < 5; ++i)
	{
		const float2 heights = TerrainShadows::GetTerrainZ(TerrainShadows::ShadowHeightTexture.SampleLevel(LinearSampler, uv + taps[i] * radius * texel, 0));
		const float lower = heights.y - zBlur;
		shadow += saturate((worldPos.z - lower) / max(heights.x + zBlur - lower, 1.0));
	}
	return shadow * 0.2;
}
#endif

// The sun's shadow cascades at a camera-relative point: 1 lit, 0 in shadow, 1 where the cascades do not reach.
float MediumCascadeShadow(float3 position)
{
	float visibility = 1.0;
	if (ViewAtmosphere::Data[0].flags.z & 1u) {
		const DirectionalShadowLightData light = DirectionalShadowLights[0];
		const float depth = SharedData::GetScreenDepth(FrameBuffer::GetShadowDepth(position));
		if (light.EndSplitDistances.y > 0.0 && depth < light.EndSplitDistances.y) {
			const uint cascade = depth >= light.EndSplitDistances.x ? 1u : 0u;
			const float3 posLS = mul(light.ShadowProj[cascade], float4(position + FrameBuffer::CameraPosAdjust.xyz, 1)).xyz;
			if (all(posLS.xy >= 0.0) && all(posLS.xy <= 1.0)) {
				const float bias = SharedData::viewAtmosphereSettings.shadowBias;
#ifdef MEDIUM_SOFT_CASCADE
				// For a medium that lies on a surface (Ground Fog), where the edge of a shadow is seen at full
				// size: four taps a texel and a half apart, each filtered between its four texels, so the edge
				// is a smooth ramp three texels wide instead of steps the size of a shadow texel.
				uint mapWidth, mapHeight, mapLayers;
				DirectionalShadowMap.GetDimensions(mapWidth, mapHeight, mapLayers);
				const float2 mapSize = float2(mapWidth, mapHeight);
				float lit = 0.0;
				[unroll] for (uint tap = 0u; tap < 4u; ++tap)
				{
					const float2 tapUv = posLS.xy + (float2(tap & 1u, tap >> 1u) - 0.5) * 1.5 / mapSize;
					const float2 f = frac(tapUv * mapSize - 0.5);
					const float4 open = float4(DirectionalShadowMap.GatherRed(LinearSampler, float3(tapUv, cascade)) > posLS.z - bias);
					lit += lerp(lerp(open.w, open.z, f.x), lerp(open.x, open.y, f.x), f.y);
				}
				visibility *= 0.25 * lit;
#else
				visibility *= dot(float4(DirectionalShadowMap.GatherRed(LinearSampler, float3(posLS.xy, cascade)) > posLS.z - bias), 0.25);
#endif
			}
		}
	}
	return visibility;
}

// With MEDIUM_EXTERNAL_CASCADE the cascades are left out: the caller has its own, filtered, value of
// MediumCascadeShadow for the medium and multiplies the three results by it.
float MediumVisibility(float3 position, out float atmosphereVisibility, out float3 diffuseVisibility)
{
	float visibility = 1.0;
#ifndef MEDIUM_EXTERNAL_CASCADE
	visibility = MediumCascadeShadow(position);
#endif
#ifdef TERRAIN_SHADOWS
	if (ViewAtmosphere::Data[0].flags.z & 256u)
		visibility *= MediumTerrainShadow(position + FrameBuffer::CameraPosAdjust.xyz, length(position));
#endif
	atmosphereVisibility = visibility;
	float layerShadow = 1.0;
#ifdef CLOUD_SHADOWS
	if ((ViewAtmosphere::Data[0].flags.z & 512u) && CloudShadows::IntersectCloudDist(position, SharedData::DirLightDirection.xyz) >= 0.0)
		layerShadow = CloudShadows::GetCloudShadowMult(position, LinearSampler);
#endif
	const float volumeShadow = PhysSky::GetDirlightTransmittance(position + FrameBuffer::CameraPosAdjust.xyz, LinearSampler, true).x;
	const float2 remap = SharedData::physSkyData.cloudShadowRemapRange;
	// MS LUTs assume clear-sky illumination. Keep cloud transport on that source
	// beyond the direct-shadow box, including light scattered by the clouds.
	diffuseVisibility = atmosphereVisibility * PhysSky::Remap(layerShadow, remap.x, remap.y, 0, 1) * PhysSky::GetCloudDiffuseTransmittance(position + FrameBuffer::CameraPosAdjust.xyz);
	atmosphereVisibility *= PhysSky::Remap(layerShadow, remap.x, remap.y, 0, 1) * PhysSky::Remap(volumeShadow, remap.x, remap.y, 0, 1);
	visibility *= layerShadow * volumeShadow;
	return saturate(visibility);
}

#ifdef EXP_HEIGHT_FOG
#	include "ExponentialHeightFog/VolumetricFogCommon.hlsli"
#endif

float3 AtmosphereLight(uint lightIndex, float3 position, float3 direction, float3 lightDir, float3 color, float3 rayleigh, float3 mie, float directVisibility, float3 diffuseVisibility)
{
	if (!any(color > 0.0) || dot(lightDir, lightDir) < 0.5 || !any(rayleigh + mie > 0.0))
		return 0.0;
	const float3 planet = PhysSky::GetMediumPlanetPosition(position);
	const float radius = length(planet);
	const float cosine = dot(direction, lightDir);
	const float3 tr = PhysSky::SampleAtmosphereLightTr(PhysSky::TexTrLut, LinearSampler, planet, lightDir, SharedData::physSkyData.diskAngularRadius[lightIndex]);
	const float3 ms = MediumMsLut.SampleLevel(LinearSampler, PhysSky::MsLutUv(radius, dot(planet / radius, lightDir)), 0).rgb;
	float fogTransmittance = 1.0;
#ifdef EXP_HEIGHT_FOG
	if (ViewAtmosphere::Data[0].flags.y != 0u)
		fogTransmittance = ExponentialHeightFog::EvaluateHeightFogLightTransmittance(position, FrameBuffer::CameraPosAdjust.xyz, lightDir);
#endif
	return color * (directVisibility * fogTransmittance * tr * (rayleigh * PhysSky::DiskRayleigh(cosine, lightIndex) + mie * PhysSky::SampleDiskPhase(cosine, lightIndex).z) + diffuseVisibility * (rayleigh + mie) * ms);
}

#endif
