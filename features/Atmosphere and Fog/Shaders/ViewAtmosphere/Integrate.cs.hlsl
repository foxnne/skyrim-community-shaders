#define COMPUTESHADER
#include "Common/ViewAtmosphere.hlsli"
#include "PhysicalSky/Common.hlsli"
#include "PhysicalSky/Lightning.hlsli"
#define SKYLIGHTING_PROBE_REGISTER t50
#include "Skylighting/Skylighting.hlsli"
#include "ViewAtmosphere/DistanceExaggeration.hlsli"

SamplerState LinearSampler : register(s0);
Texture3D<float4> MediumSource : register(t0);
Texture2D<float4> MediumMsLut : register(t2);
RWTexture3D<float4> IntegratedLuminance : register(u0);
RWTexture3D<float4> IntegratedTransmittance : register(u1);
cbuffer MediumDispatch : register(b0)
{
	uint MediumLevel;
	uint3 MediumPadding;
};

[numthreads(8, 8, 1)] void main(uint2 coord : SV_DispatchThreadID) {
	const ViewAtmosphere::Grid grid = ViewAtmosphere::Data[0].grids[MediumLevel];
	if (any(coord >= grid.size.xy))
		return;
	float3 light = 0.0;
	float3 tr = 1.0;
	IntegratedLuminance[uint3(coord, 0) + grid.offset.xyz] = 0.0;
	IntegratedTransmittance[uint3(coord, 0) + grid.offset.xyz] = 1.0;
	// Distance exaggeration: a second copy of the column, stacked above the physical one, with extra air in it.
	// Everything in this shader that is not inside an `exaggerate` branch is the physical integration, unchanged.
	const bool exaggerate = ViewAtmosphere::Data[0].history.w > 0.0;
	const uint3 surfaceOffset = grid.offset.xyz + uint3(0u, 0u, ViewAtmosphere::Data[0].atlas.z);
	float3 surfaceLight = 0.0;
	float3 surfaceTr = 1.0;
	float3 airGain = 0.0;
	float3 hazeGain = 0.0;
	float sinElevation = 0.0;
	if (exaggerate) {
		IntegratedLuminance[uint3(coord, 0) + surfaceOffset] = 0.0;
		IntegratedTransmittance[uint3(coord, 0) + surfaceOffset] = 1.0;
		// Unshadowed light scattered towards the camera per unit of air and per unit of haze along this column. Only
		// their ratio is used, to split the stored source (which has the shadows) between air, haze and fog.
		const SharedData::PhysSkyData sky = SharedData::physSkyData;
		const float3 direction = normalize(ViewAtmosphere::Position(grid, uint3(coord, 0u), 0.5));
		sinElevation = direction.z;
		const float3 planet = PhysSky::GetMediumPlanetPosition(0.0);
		const float radius = length(planet);
		const float3 lightDirections[3] = { sky.sunDir, sky.masserDir, sky.secundaDir };
		const float3 lightColors[3] = { sky.sunlightColor, sky.masserColor, sky.secundaColor };
		[unroll] for (uint lightIndex = 0u; lightIndex < 3u; ++lightIndex)
		{
			const float3 lightDir = lightDirections[lightIndex];
			if (any(lightColors[lightIndex] > 0.0) && dot(lightDir, lightDir) >= 0.5) {
				const float cosine = dot(direction, lightDir);
				const float3 lightTr = PhysSky::SampleAtmosphereLightTr(PhysSky::TexTrLut, LinearSampler, planet, lightDir, sky.diskAngularRadius[lightIndex]);
				const float3 ms = MediumMsLut.SampleLevel(LinearSampler, PhysSky::MsLutUv(radius, dot(planet / radius, lightDir)), 0).rgb;
				airGain += lightColors[lightIndex] * (lightTr * PhysSky::DiskRayleigh(cosine, lightIndex) + ms);
				hazeGain += lightColors[lightIndex] * (lightTr * PhysSky::SampleDiskPhase(cosine, lightIndex).z + ms);
			}
		}
	}
	[loop] for (uint layer = 0u; layer < grid.size.z; ++layer)
	{
		const float4 source = MediumSource[uint3(coord, layer) + grid.offset.xyz];
		const float3 position = ViewAtmosphere::Position(grid, uint3(coord, layer), 0.5);
		float3 rayleigh, mie, extinction;
		PhysSky::GetMediumCoefficients(position, rayleigh, mie, extinction);
		extinction = extinction * SharedData::physSkyData.apTrMix + source.a;
		const float step = (ViewAtmosphere::Distance(grid, layer + 1u) - ViewAtmosphere::Distance(grid, layer)) * (GAME_UNIT_TO_M / 1000.0);
		const float3 tau = max(extinction * step, 0.0);
		const float3 stepTr = exp(-tau);
		const float3 factor = float3(
			tau.x < 1e-3 ? step * (1.0 - 0.5 * tau.x) : (1.0 - stepTr.x) / extinction.x,
			tau.y < 1e-3 ? step * (1.0 - 0.5 * tau.y) : (1.0 - stepTr.y) / extinction.y,
			tau.z < 1e-3 ? step * (1.0 - 0.5 * tau.z) : (1.0 - stepTr.z) / extinction.z);
		// Extra air and haze scattering of this slice (per km); fog is never part of it.
		float3 extraAir = 0.0;
		float3 extraHaze = 0.0;
		if (exaggerate) {
			const float2 extraPath = ViewAtmosphere::ExtraPath(length(position), sinElevation);
			extraAir = extraPath.x * rayleigh;
			extraHaze = extraPath.y * mie;
		}
		float3 surfaceTransient = 0.0;
		float3 transient = 0.0;
		if (SharedData::physSkyData.lightningColorIntensity.w > 0.0) {
			const float3 worldPosition = position + FrameBuffer::CameraPosAdjust.xyz;
			sh2 visibilitySH = Skylighting::UNIT_SH;
			if (ViewAtmosphere::Data[0].flags.z & 8u)
				visibilitySH = Skylighting::Sample(position, 0.0);
			[unroll] for (uint emitter = 0u; emitter < 2u; ++emitter)
			{
				const float3 direction = normalize(Lightning::Emitter(emitter).xyz - worldPosition);
				const float cosine = dot(normalize(position), direction);
				float visibility = SharedData::InInterior ? 0.0 : 1.0;
				if (ViewAtmosphere::Data[0].flags.z & 8u)
					visibility *= Skylighting::EvaluateDiffuse(visibilitySH, direction, Skylighting::GetFadeOutFactor(position));
				transient += Lightning::MediumIncidentFrom(emitter, worldPosition) * visibility *
				             (rayleigh * PhysSky::Phase::Rayleigh(cosine) + mie * PhysSky::Phase::CornetteShanks(cosine, SharedData::physSkyData.aerosolPhaseG) + source.a * (1.0 / (4.0 * 3.14159265)));
				if (exaggerate)
					surfaceTransient += Lightning::MediumIncidentFrom(emitter, worldPosition) * visibility *
					                    (extraAir * PhysSky::Phase::Rayleigh(cosine) + extraHaze * PhysSky::Phase::CornetteShanks(cosine, SharedData::physSkyData.aerosolPhaseG));
			}
		}
		light += tr * (source.rgb + transient) * factor;
		tr *= stepTr;
		IntegratedLuminance[uint3(coord, layer + 1u) + grid.offset.xyz] = float4(min(light, 65504.0), 0);
		IntegratedTransmittance[uint3(coord, layer + 1u) + grid.offset.xyz] = float4(tr, 1);
		if (exaggerate) {
			const SharedData::PhysSkyData sky = SharedData::physSkyData;
			// The stored source holds air, haze and fog together, already lit and shadowed. The extra air takes the
			// share of it that this much more air and haze would have scattered.
			const float3 share = (extraAir * airGain + extraHaze * hazeGain) / max(rayleigh * airGain + (mie + source.a) * hazeGain, 1e-20);
			const float3 surfaceSource = source.rgb * (1.0 + share) + transient + surfaceTransient;
			const float3 hazeExtinction = extraHaze * (1.0 + sky.aerosolAbsorption / max(sky.aerosolScatter, 1e-20));
			const float3 surfaceExtinction = extinction + (extraAir + hazeExtinction) * sky.apTrMix;
			const float3 surfaceTau = max(surfaceExtinction * step, 0.0);
			const float3 surfaceStepTr = exp(-surfaceTau);
			const float3 surfaceFactor = float3(
				surfaceTau.x < 1e-3 ? step * (1.0 - 0.5 * surfaceTau.x) : (1.0 - surfaceStepTr.x) / surfaceExtinction.x,
				surfaceTau.y < 1e-3 ? step * (1.0 - 0.5 * surfaceTau.y) : (1.0 - surfaceStepTr.y) / surfaceExtinction.y,
				surfaceTau.z < 1e-3 ? step * (1.0 - 0.5 * surfaceTau.z) : (1.0 - surfaceStepTr.z) / surfaceExtinction.z);
			surfaceLight += surfaceTr * surfaceSource * surfaceFactor;
			surfaceTr *= surfaceStepTr;
			IntegratedLuminance[uint3(coord, layer + 1u) + surfaceOffset] = float4(min(surfaceLight, 65504.0), 0);
			IntegratedTransmittance[uint3(coord, layer + 1u) + surfaceOffset] = float4(surfaceTr, 1);
		}
	}
}
