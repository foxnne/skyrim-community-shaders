///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2016-2021, Intel Corporation
//
// SPDX-License-Identifier: MIT
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// XeGTAO is based on GTAO/GTSO "Jimenez et al. / Practical Real-Time Strategies for Accurate Indirect Occlusion",
// https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf
//
// Implementation:  Filip Strugar (filip.strugar@intel.com), Steve Mccalla <stephen.mccalla@intel.com>         (\_/)
// Version:         (see XeGTAO.h)                                                                            (='.'=)
// Details:         https://github.com/GameTechDev/XeGTAO                                                     (")_(")
//
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// with additional edits by FiveLimbedCat/ProfJack
//
// More references:
//
// Screen Space Indirect Lighting with Visibility Bitmask
//  https://arxiv.org/abs/2301.11376
//
// Exploring Raytraced Future in Metro Exodus
//  https://developer.download.nvidia.com/video/gputechconf/gtc/2019/presentation/s9985-exploring-ray-traced-future-in-metro-exodus.pdf
//
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// New version is based on SSRT 3
// https://github.com/cdrinmatane/SSRT3
//
// MIT License
//
// Copyright (c) 2024 CDRIN
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "Common/ColorManagement.hlsli"
#include "Common/FastMath.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/Game.hlsli"
#include "Common/Math.hlsli"
#include "NRD/NRDReblurSH.hlsli"
#include "ScreenSpaceGI/common.hlsli"
#if defined(IBL)
#	include "IBL/IBL.hlsli"
#endif

Texture2D<float> srcWorkingDepth : register(t0);
Texture2D<float4> srcMasks : register(t1);
Texture2D<float3> srcRadiance : register(t2);
Texture2D<unorm float2> srcNoise : register(t3);
#if defined(DYNAMIC_CUBEMAPS)
TextureCube<float3> EnvTexture : register(t4);
TextureCube<float3> ReflectionCubemap : register(t5);
#	if defined(SKYLIGHTING)
#		define SKYLIGHTING_PROBE_REGISTER t6
#		include "Skylighting/Skylighting.hlsli"
#	endif
#endif
Texture2D<float2> srcNormal : register(t8);
// Physical Sky's sky-view LUT (sky radiance only: no sun disc, no clouds, no ground), bound when Physical Sky renders.
Texture2D<float4> PhysSkyViewLut : register(t9);
// Physical Sky's volumetric cloud transmittance per direction (1 = clear line of sight to the sky), for weather.
TextureCube<float> PhysSkyCloudTr : register(t10);

RWTexture2D<float4> outRadianceHitDist : register(u0);
#ifdef SSGI_SH
RWTexture2D<float4> outSH1 : register(u2);
#endif

static const float DenoiserMaxExposedLuminance = 16.0;

float3 ClampDenoiserRadiance(float3 radiance)
{
	const float maxLuminance = DenoiserMaxExposedLuminance * PreExposure::InverseScale;
	const float luminance = Color::RGBToLuminance(radiance);
	return luminance > maxLuminance ? radiance * (maxLuminance / luminance) : radiance;
}

static const float SSGI_NORMAL_BIAS = 0.01;
static const uint SSGI_MAX_RAY = 32;
static const uint SSGI_ROTATION_COUNT = 1;
static const uint SSGI_FALLBACK_SAMPLE_COUNT = 4;
static const float SSGI_EXP_FACTOR = 2.0;
static const float SSGI_FALLBACK_POWER = 1.0;
static const float SSGI_FALLBACK_MIP = 3.0;
static const float SSGI_SKIN_FALLBACK_AO_SCALE = 0.5;

bool IsSkinPixel(uint2 pxCoord)
{
	const float2 mask = srcMasks[pxCoord].xy;
	return mask.x > 0.0 && mask.y < 1.5;
}

// foxnne sun balance: the ambient fallback raises sky light on up-facing surfaces by
// 1 + n.z * (1 - Min Diffuse Visibility) (x1.71 at the look pack's 0.29). That lifts open ground and the floor
// under a canopy alike and halves the direct : ambient ratio. ssgiSettings.FallbackSkyBoost (Sky Fill Boost in the
// menu) is the share of that boost in use: 1 keeps it, 0 removes it.

// foxnne sky-lit shadows (ssgiSettings.PhysicalSkyFill). Outdoors, sun shadow is lit by the sky. The fallback
// below already samples that sky (the reflection cubemap Physical Sky draws), but three things took its blue out:
// Sky IBL Saturation (0.6 in the look pack) greyed it, the DALC term (the weather's ambient colour, itself a
// stand-in for the same sky) was added on top of it on every ray, and IL Saturation in the composite greyed the
// whole fill, sky included. With the switch on: the sky term keeps its own colour (Sky Fill Saturation), its
// brightness on open sky is Open Sky Fill Scale, DALC on skyward rays under open sky drops to Ambient On Open Sky,
// and the open-sky fallback is spared IL Saturation (traced bounce keeps it). All of it is blended in by sky openness
// (Skylighting's visibility above its floor): openness 0 (a canopy, an overhang) is the legacy fill exactly, so
// forest shade holds. Without Skylighting compiled in the openness is unknown and taken as 0, i.e. legacy.
bool UsePhysicalSkyFill()
{
	return SharedData::ssgiSettings.PhysicalSkyFill != 0 && SharedData::physSkyData.enabled && !SharedData::InInterior;
}

// foxnne skylight shadow tint (ssgiSettings.SkylightTint, on top of Physical Sky Fill). Two physical corrections, both
// faded in by sky openness so a canopy keeps the legacy fill:
// 1. A plane does not light itself. On open ground the horizon tracer sees the ground around the receiver at coarse
//    depth mips (and normal-map relief) as occluders just above the tangent plane, takes those bits off the sky and
//    returns the sunlit ground's warm radiance for them instead. Hits that are coplanar with the receiver (similar
//    normal, on the tangent plane) give CoplanarBounceReject of their bits back: their bounce is dropped and the open
//    sky's mean radiance takes their place, and they no longer count as occlusion.
// 2. Linear Lighting multiplies the sun (directionalLightMult) but not the sky, so sun : sky is off Physical Sky's
//    own ratio by that factor. SkyMatchesSun applies that share of it to the open-sky term too.
bool UseSkylightTint()
{
	return SharedData::ssgiSettings.SkylightTint != 0 && UsePhysicalSkyFill();
}

// foxnne skylight colour (ssgiSettings.SkyFillSource 1): the open-sky term of the fill read from Physical Sky's
// sky-view LUT instead of the sky reflection cubemap. The cubemap holds what the reflection pass drew (clouds, the
// horizon haze, the ground below the horizon) and read neutral grey at the Whiterun road on a clear noon; the LUT is the
// clear sky's own scattered radiance, i.e. the physically right fill for open-sky shade on a clear day. Rays at or below
// the horizon keep the cubemap (the ground). Same scene units as the cubemap's sky (Sky.hlsl writes the LUT straight out).
// Clouds are not in the LUT, so this suits clear and fair weather; under overcast keep source 0.
float2 PhysSkyViewLutUv(float3 rayDir, uint lutHeight)
{
	const float radius = SharedData::physSkyData.rPlanet;
	const float viewRadius = max(SharedData::physSkyData.zCameraPlanet, radius);
	const float horizonDistance = sqrt(max(0.0, (viewRadius - radius) * (viewRadius + radius)));
	const float horizon = acos(clamp(-horizonDistance / viewRadius, -1.0, 0.0));
	const float u = atan2(rayDir.y, rayDir.x) * 0.5 * Math::INV_PI;
	const float zenith = acos(clamp(rayDir.z, -1.0, 1.0));
	const float v = zenith <= horizon ?
	                    0.5 * (1.0 - sqrt(saturate(1.0 - zenith / horizon))) :
	                    0.5 + 0.5 * sqrt(saturate((zenith - horizon) / (Math::PI - horizon)));
	return float2(frac(u), (0.5 + v * (lutHeight - 1.0)) / lutHeight);
}

/** @brief Physical Sky's clear-sky radiance toward rayDir; false when the ray is at/below the horizon or the LUT is unbound.
 *  Same mapping as PhysSky::SkyViewCoord / SkyViewLutTexel (the producer inverts it in InvSkyViewLutUv). Bilinear by hand
 *  so the azimuth wraps across the u = 0/1 seam (the shared sampler clamps). */
bool SamplePhysSkyLut(float3 rayDir, out float3 o_radiance)
{
	o_radiance = 0;
	if (rayDir.z <= 0.0 || !SharedData::physSkyData.enabled)
		return false;
	uint width, height;
	PhysSkyViewLut.GetDimensions(width, height);
	if (width == 0 || height == 0)
		return false;
	const float2 texel = PhysSkyViewLutUv(rayDir, height) * float2(width, height) - 0.5;
	const float2 base = floor(texel);
	const float2 f = texel - base;
	const uint x0 = (uint)((int)base.x + (int)width) % width;
	const uint x1 = (x0 + 1) % width;
	const uint y0 = (uint)clamp((int)base.y, 0, (int)height - 1);
	const uint y1 = min(y0 + 1, height - 1);
	const float3 top = lerp(PhysSkyViewLut.Load(int3(x0, y0, 0)).rgb, PhysSkyViewLut.Load(int3(x1, y0, 0)).rgb, f.x);
	const float3 bottom = lerp(PhysSkyViewLut.Load(int3(x0, y1, 0)).rgb, PhysSkyViewLut.Load(int3(x1, y1, 0)).rgb, f.x);
	o_radiance = max(lerp(top, bottom, f.y), 0.0);
	return true;
}

/** @brief Share of the clear sky (the LUT) in the open-sky fill toward rayDir: Physical Sky's volumetric cloud transmittance
 *  that way. Clouds in front of the sky (any weather, any transition) hand that direction back to the cubemap, which holds
 *  the lit clouds. No volumetric clouds or no cube: 0, i.e. the cubemap as before. */
float GetClearSkyShare(float3 rayDir)
{
	if (rayDir.z <= 0.0 || !SharedData::physSkyData.enabled || !SharedData::physSkyData.enableVolumetricClouds)
		return 0.0;
	uint width, height, levels;
	PhysSkyCloudTr.GetDimensions(0, width, height, levels);
	if (width == 0)
		return 0.0;
	return saturate(PhysSkyCloudTr.SampleLevel(samplerLinearClamp, rayDir, 0));
}

static const float SSGI_COPLANAR_NORMAL_COS = 0.9;  // hit normal within ~25 degrees of the receiver's
static const float SSGI_COPLANAR_SLOPE = 0.12;      // hit within ~7 degrees of the receiver's tangent plane

// sin/cos of the positive bit-center offsets from the projected receiver normal.
// The negative half is mirrored, reducing the table and avoiding per-bit sincos.
static const float2 SSGI_BIT_SIN_COS[SSGI_MAX_RAY / 2] = {
	float2(0.9987954562, 0.0490676743),
	float2(0.9891765100, 0.1467304745),
	float2(0.9700312532, 0.2429801799),
	float2(0.9415440652, 0.3368898534),
	float2(0.9039892931, 0.4275550934),
	float2(0.8577286100, 0.5141027442),
	float2(0.8032075315, 0.5956993045),
	float2(0.7409511254, 0.6715589548),
	float2(0.6715589548, 0.7409511254),
	float2(0.5956993045, 0.8032075315),
	float2(0.5141027442, 0.8577286100),
	float2(0.4275550934, 0.9039892931),
	float2(0.3368898534, 0.9415440652),
	float2(0.2429801799, 0.9700312532),
	float2(0.1467304745, 0.9891765100),
	float2(0.0490676743, 0.9987954562)
};

// Engine-specific screen & temporal noise loader
float2 SpatioTemporalNoise(uint2 pixCoord, uint temporalIndex)  // without TAA, temporalIndex is always 0
{
	// noise texture from https://github.com/electronicarts/fastnoise
	// 128x128x64
	uint2 noiseCoord = (pixCoord % 128) + uint2(0, (temporalIndex % 64) * 128);
	return srcNoise.Load(uint3(noiseCoord, 0));
}

uint ComputeOccludedBitfield(float minHorizon, float maxHorizon, inout uint globalOccludedBitfield)
{
	uint startHorizonInt = min((uint)(saturate(minHorizon) * SSGI_MAX_RAY), SSGI_MAX_RAY);
	uint angleHorizonInt = min((uint)ceil(saturate(maxHorizon - minHorizon) * SSGI_MAX_RAY), SSGI_MAX_RAY - startHorizonInt);

	if (angleHorizonInt == 0)
		return 0;

	uint angleHorizonBitfield = 0xFFFFFFFFu;
	if (angleHorizonInt < SSGI_MAX_RAY)
		angleHorizonBitfield = (1u << angleHorizonInt) - 1u;
	uint currentOccludedBitfield = angleHorizonBitfield << startHorizonInt;
	currentOccludedBitfield &= ~globalOccludedBitfield;
	globalOccludedBitfield |= currentOccludedBitfield;
	return currentOccludedBitfield;
}

// Integrate the newly covered angular zones using the spherical Jacobian for
// a view-axis slice. RotationCount uniformly samples plane azimuth over PI;
// Lambert's 1/PI therefore cancels the azimuthal Monte Carlo factor. The
// returned value is diffuse illumination divided by PI, ready for one albedo
// multiplication at composite time.
void IntegrateBitfield(
	uint bitfield,
	float3 projectedNormal, float3 projectedNormalTangent,
	float projectedNormalLength, float projectedNormalSin, float projectedNormalCos,
	float3 sourceNormal, bool requireSourceFacing,
	out float scalarWeight, out float3 directionWeight)
{
	scalarWeight = 0;
	directionWeight = 0;

	const float bitAngle = Math::PI / float(SSGI_MAX_RAY);
	// Midpoint integration of cos(theta) * |sin(theta)| over all bins yields
	// bitAngle / sin(bitAngle). Using sin(bitAngle) is the corresponding analytic
	// finite-bin normalization, so a constant-radiance white furnace returns 1.
	const float normalizedBitMeasure = sin(bitAngle);
	// Newly covered hit bits never overlap, so sparse iteration caps all hit
	// integration work across the ray march at 32 iterations per slice.
	[loop] while (bitfield != 0)
	{
		uint bit = (uint)firstbitlow(bitfield);
		bitfield &= bitfield - 1u;

		uint mirroredBit = bit < SSGI_MAX_RAY / 2 ? bit : SSGI_MAX_RAY - 1 - bit;
		float2 bitSinCos = SSGI_BIT_SIN_COS[mirroredBit];
		float bitSin = bit < SSGI_MAX_RAY / 2 ? bitSinCos.x : -bitSinCos.x;
		float bitCos = bitSinCos.y;

		float3 direction = projectedNormal * bitCos + projectedNormalTangent * bitSin;
		// Outgoing Lambertian radiance is angle-independent over the source's
		// front hemisphere. Gate the back hemisphere, but do not multiply by a
		// second source cosine.
		if (requireSourceFacing && dot(sourceNormal, -direction) <= 0.0)
			continue;

		float sineFromView = bitSin * projectedNormalCos + bitCos * projectedNormalSin;
		float weight = projectedNormalLength * bitCos * abs(sineFromView) * normalizedBitMeasure;

		scalarWeight += weight;
		directionWeight += direction * weight;
	}
}

#if defined(DYNAMIC_CUBEMAPS)
// o_fillBlend: the share of the Physical Sky Fill behaviour in this sample, 0 = legacy (switch off, interior,
// no Physical Sky, no Skylighting, or occluded sky), 1 = open sky.
float3 SampleDiffuseFallbackCubemap(float3 worldPos, float3 worldNormal, float3 worldDir, float3 iblRatio, out float o_fillBlend)
{
	o_fillBlend = 0.0;
	float3 envSampleRaw = EnvTexture.SampleLevel(samplerLinearClamp, worldDir, SSGI_FALLBACK_MIP);
	float3 envColor = envSampleRaw;

	// Sky openness over this point for Physical Sky Fill: 0 = occluded or unknown (legacy), 1 = open.
	float fillBlend = 0.0;
#	if defined(SKYLIGHTING)
	float skylightingDiffuse = 1.0;
	if (!SharedData::InInterior) {
		float fadeOutFactor = Skylighting::GetFadeOutFactor(worldPos);
		float3 skylightingNormal = normalize(float3(worldNormal.xy, max(0, worldNormal.z)));
		float skylightingBoost = 1.0 + SharedData::ssgiSettings.FallbackSkyBoost * saturate(worldNormal.z) * (1.0 - SharedData::skylightingSettings.MinDiffuseVisibility);
		sh2 skylightingSH = Skylighting::Sample(worldPos, worldDir);
		const float skylightingVisibility = Skylighting::EvaluateDiffuse(skylightingSH, skylightingNormal, fadeOutFactor);
		skylightingDiffuse = skylightingVisibility * skylightingBoost;
		[branch] if (UsePhysicalSkyFill() || SharedData::ssgiSettings.SkyFillDebug == 2)
		{
			const float minVisibility = SharedData::skylightingSettings.MinDiffuseVisibility;
			fillBlend = saturate((skylightingVisibility - minVisibility) / max(1.0 - minVisibility, 1e-3));
		}
	}
#	endif

#	if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		uint dalcMode = SharedData::iblSettings.DALCMode;

		if (dalcMode >= 2) {
			// Mode 2: DALC-normalized env scaled by DALCAmount
			float envLum = Color::RGBToLuminance(EnvTexture.SampleLevel(samplerLinearClamp, worldDir, 15));
			float3 dalc = Color::Ambient(max(0, SharedData::GetAmbient(worldDir)));
			envColor = (Color::RGBToLuminance(envSampleRaw) / max(envLum, 0.001)) * dalc * SharedData::iblSettings.DALCAmount;
		} else {
			// Mode 0/1: ratio-based
			float3 saturatedEnv = Color::Saturation(envSampleRaw, SharedData::iblSettings.EnvIBLSaturation);
			envColor = saturatedEnv * iblRatio * SharedData::iblSettings.EnvIBLScale;
		}

		float3 reflectionColor = 0;
		if (!SharedData::InInterior) {
			const uint skyFillDebug = SharedData::ssgiSettings.SkyFillDebug;
			[branch] if (skyFillDebug == 5 || skyFillDebug == 6)
			{
				// Debug: the fallback's open-sky radiance alone (rays above the horizon only), from the LUT (5) or the
				// cubemap (6), unsaturated and unscaled, with Skylighting visibility; no DALC/env. Compare their hue in the
				// same graded capture.
				float3 debugSky = 0.0;
				[branch] if (worldDir.z > 0.0)
				{
					if (skyFillDebug == 5) {
						float3 lutSample;
						if (SamplePhysSkyLut(worldDir, lutSample))
							debugSky = lutSample;
					} else
						debugSky = ReflectionCubemap.SampleLevel(samplerLinearClamp, worldDir, SSGI_FALLBACK_MIP);
				}
#		if defined(SKYLIGHTING)
				debugSky *= skylightingDiffuse;
#		endif
				o_fillBlend = 0.0;
				return ColorManagement::SceneToLinear(debugSky);
			}
			float3 skySample = ReflectionCubemap.SampleLevel(samplerLinearClamp, worldDir, SSGI_FALLBACK_MIP);
			float3 skyColor = Color::Saturation(skySample, SharedData::iblSettings.SkyIBLSaturation) * SharedData::iblSettings.SkyIBLScale;
			[branch] if (fillBlend > 0.0)
			{
				// Open sky: the sky term at its own colour and Open Sky Fill Scale, faded in by openness.
				float3 fillSkySample = skySample;
				[branch] if (SharedData::ssgiSettings.SkyFillSource == 1)
				{
					// Clear sky from the LUT where the line of sight is cloud-free, the cubemap (lit clouds) where it is not.
					const float clearShare = GetClearSkyShare(worldDir);
					float3 lutSample;
					if (clearShare > 0.0 && SamplePhysSkyLut(worldDir, lutSample))
						fillSkySample = lerp(skySample, lutSample, clearShare);
				}
				float3 fillSkyColor = Color::Saturation(fillSkySample, SharedData::ssgiSettings.SkyFillSaturation) * SharedData::ssgiSettings.SkyFillScale;
				if (UseSkylightTint())
					fillSkyColor *= lerp(1.0, SharedData::linearLightingSettings.directionalLightMult, SharedData::ssgiSettings.SkyMatchesSun);
				skyColor = lerp(skyColor, fillSkyColor, fillBlend);
				// A ray that looks up at open sky is lit by the sky term; the DALC term there counts the same sky twice
				// in the weather's grey. Rays at and below the horizon keep it as the stand-in for ground bounce.
				const float skyward = saturate(worldDir.z * 4.0);
				envColor *= lerp(1.0, SharedData::ssgiSettings.SkyFillEnvWeight, skyward * fillBlend);
			}
#		if defined(SKYLIGHTING)
			skyColor *= skylightingDiffuse;
			if (SharedData::iblSettings.SkylightingAffectsEnv != 0)
				envColor *= skylightingDiffuse;
#		endif
			reflectionColor = skyColor;
			envColor += skyColor;
		}
		envColor = ColorManagement::SceneToLinear(envColor);
		envColor = ImageBasedLighting::ApplyIBLReflectionFallback(envColor, ColorManagement::SceneToLinear(reflectionColor), worldPos);
	} else
#	endif
	{
		float3 directionalAmbient = Color::Ambient(max(0, SharedData::GetAmbient(worldDir)));
		envColor = ColorManagement::SceneToLinear(directionalAmbient);
#	if defined(SKYLIGHTING)
		if (!SharedData::InInterior)
			envColor *= skylightingDiffuse;
#	endif
		fillBlend = 0.0;  // no sky term without IBL: the fill stays legacy
	}

	o_fillBlend = fillBlend;
	return envColor;
}
#endif

void CalculateGI(
	uint2 dtid, float2 uv, float viewspaceZ, float3 viewspaceNormal,
	out float o_ao, out float3 o_radiance
#ifdef SSGI_SH
	,
	out float3 o_direction
#endif
)
{
	const float2 frameScale = FrameDim * RcpTexDim;

	float2 normalizedScreenPos = uv;
	const float rcpNumSteps = rcp((float)NumSteps);
	uint2 noiseCoord = uint2(normalizedScreenPos * OUT_FRAME_DIM);
	const float2 localNoise = SpatioTemporalNoise(noiseCoord, SharedData::FrameCount);
	const float noiseStep = localNoise.y;
	const float noiseDirection = localNoise.x;

	const float3 pixCenterPos = ScreenToViewPosition(normalizedScreenPos, viewspaceZ) + viewspaceNormal * SSGI_NORMAL_BIAS * viewspaceZ;
	const float3 viewVec = normalize(-pixCenterPos);

	if (dot(viewVec, pixCenterPos) > 0)
		viewspaceNormal = -viewspaceNormal;

	float normHitDist = 0;
	float3 totalRadiance = 0;
	float3 fallbackRadiance = 0;
	// Physical Sky Fill bookkeeping, only accumulated with the switch on (uniform branch).
	const bool physicalSkyFill = UsePhysicalSkyFill();
	const uint skyFillDebug = SharedData::ssgiSettings.SkyFillDebug;
	const bool trackFill = physicalSkyFill || skyFillDebug == 2;
	float hitWeightTotal = 0;   // angular measure of the traced hits (Bounce Limit)
	float openWeightTotal = 0;  // and of the open bits the fallback lit
	float fillBlendTotal = 0;   // fallback fill share, weighted by open measure
	// Skylight tint: the share of coplanar ground hits handed back to the sky (0 = legacy, also under a canopy)
	float coplanarReject = 0.0;
#if defined(DYNAMIC_CUBEMAPS) && defined(SKYLIGHTING)
	[branch] if (UseSkylightTint() && UseDynamicCubemap != 0 && SharedData::ssgiSettings.CoplanarBounceReject > 0.0)
	{
		const float3 pixelPosWS = ViewToWorldPosition(pixCenterPos, FrameBuffer::CameraViewInverse);
		const float3 pixelNormalWS = ViewToWorldVector(viewspaceNormal, FrameBuffer::CameraViewInverse);
		const float3 skylightingNormal = normalize(float3(pixelNormalWS.xy, max(0, pixelNormalWS.z)) + float3(0, 0, 1e-4));
		const float visibility = Skylighting::EvaluateDiffuse(Skylighting::Sample(pixelPosWS, pixelNormalWS), skylightingNormal, Skylighting::GetFadeOutFactor(pixelPosWS));
		const float minVisibility = SharedData::skylightingSettings.MinDiffuseVisibility;
		const float pixelOpen = saturate((visibility - minVisibility) / max(1.0 - minVisibility, 1e-3));
		coplanarReject = saturate(SharedData::ssgiSettings.CoplanarBounceReject) * pixelOpen;
	}
#endif
	float3 coplanarRadiance = 0;   // traced bounce from coplanar hits, kept apart
	float coplanarOpenWeight = 0;  // their angular measure as sky directions
	float coplanarBits = 0;        // and their share of the occluded bits
	float coplanarHitWeight = 0;   // their traced-hit measure (Bounce Limit), restored by (1 - reject)
#ifdef SSGI_SH
	float3 coplanarDirection = 0;     // traced bounce moment
	float3 coplanarSkyDirection = 0;  // their open-direction moment, for the reclaimed sky
#endif
#ifdef SSGI_SH
	float3 totalDirection = 0;
	float3 fallbackDirection = 0;
#endif

	[loop] for (uint rotation = 0; rotation < SSGI_ROTATION_COUNT; rotation++)
	{
		float phi = (rotation + noiseDirection) * (Math::PI / SSGI_ROTATION_COUNT);
		float3 directionVec = 0;
		sincos(phi, directionVec.y, directionVec.x);

		float2 omega_dir = float2(directionVec.x, -directionVec.y);
		float2 pixPos = dtid + 0.5;
		float2 absDir = max(abs(omega_dir), 1e-6);

		float3 planeNormal = normalize(cross(directionVec, viewVec));
		float3 tangent = cross(viewVec, planeNormal);
		float3 projectedNormalVec = viewspaceNormal - planeNormal * dot(viewspaceNormal, planeNormal);
		float projectedNormalLengthSq = dot(projectedNormalVec, projectedNormalVec);
		float projectedNormalLength = sqrt(max(projectedNormalLengthSq, 0.0));
		float3 projectedNormalNormalized = projectedNormalVec * rsqrt(max(projectedNormalLengthSq, EPSILON_LENGTH_SQ));
		float3 projectedNormalTangent = cross(projectedNormalNormalized, planeNormal);
		float cosN = clamp(dot(projectedNormalNormalized, viewVec), -1.0, 1.0);
		float sinN = clamp(dot(projectedNormalNormalized, tangent), -1.0, 1.0);
		float n = -sign(dot(projectedNormalVec, tangent)) * FastMath::ACos(cosN);

		uint globalOccludedBitfield = 0;

		[unroll] for (int sideSign = -1; sideSign <= 1; sideSign += 2)
		{
			float2 sideDir = omega_dir * sideSign;
			float2 edgeDist;
			edgeDist.x = sideDir.x >= 0 ? (OUT_FRAME_DIM.x - pixPos.x) : pixPos.x;
			edgeDist.y = sideDir.y >= 0 ? (OUT_FRAME_DIM.y - pixPos.y) : pixPos.y;
			float screenspaceRadius = min(edgeDist.x / absDir.x, edgeDist.y / absDir.y);

			[loop] for (uint step = 0; step < NumSteps; step++)
			{
				float s = (step + noiseStep) * rcpNumSteps;
				float sampleOffset = pow(abs(s), SSGI_EXP_FACTOR) * screenspaceRadius;
				sampleOffset = max(sampleOffset, 1.0 + step);

				float2 samplePxCoord = pixPos + omega_dir * sampleOffset * sideSign;
				float2 sampleUV = samplePxCoord * RCP_OUT_FRAME_DIM;

				float2 sampleScreenPos = sampleUV;
				[branch] if (any(sampleScreenPos > 1.0) || any(sampleScreenPos < 0.0)) break;

				float mipLevel = min((step + 1) / 2, 4);

				float SZ = srcWorkingDepth.SampleLevel(samplerPointClamp, sampleUV * frameScale, mipLevel);
				if (SZ <= FP_Z)
					continue;

				float3 samplePos = ScreenToViewPosition(sampleScreenPos, SZ);
				float3 sampleDelta = samplePos - pixCenterPos;
				float3 sampleHorizonVec = normalize(sampleDelta);

				float3 sampleBackHorizonVec = normalize(sampleDelta - viewVec * Thickness * viewspaceZ);

				float2 frontBackHorizon = float2(dot(sampleHorizonVec, viewVec), dot(sampleBackHorizonVec, viewVec));
				frontBackHorizon = clamp(frontBackHorizon, -1.0, 1.0);
				frontBackHorizon = float2(FastMath::ACos(frontBackHorizon.x), FastMath::ACos(frontBackHorizon.y));
				frontBackHorizon = saturate(((sideSign * -frontBackHorizon) - n + Math::HALF_PI) * Math::INV_PI);
				frontBackHorizon = sideSign > 0 ? frontBackHorizon.yx : frontBackHorizon.xy;

				uint currentOccludedBitfield = ComputeOccludedBitfield(
					frontBackHorizon.x, frontBackHorizon.y, globalOccludedBitfield);

#ifdef GI
				if (currentOccludedBitfield != 0) {
					float3 normalSample = GBuffer::DecodeNormal(
						srcNormal.SampleLevel(samplerPointClamp, sampleUV * OUT_FRAME_SCALE, mipLevel));
					if (dot(samplePos, normalSample) > 0.0)
						normalSample = -normalSample;

					bool coplanarHit = false;
					[branch] if (coplanarReject > 0.0)
					{
						const float heightAbovePlane = abs(dot(sampleDelta, viewspaceNormal));
						coplanarHit = dot(normalSample, viewspaceNormal) > SSGI_COPLANAR_NORMAL_COS && heightAbovePlane < SSGI_COPLANAR_SLOPE * length(sampleDelta);
					}

					float scalarWeight;
					float3 directionWeight;
					IntegrateBitfield(
						currentOccludedBitfield,
						projectedNormalNormalized, projectedNormalTangent,
						projectedNormalLength, sinN, cosN,
						normalSample, true,
						scalarWeight, directionWeight);

					float3 sampleRadiance = max(
						srcRadiance.SampleLevel(samplerPointClamp, sampleUV * OUT_FRAME_SCALE, mipLevel).rgb,
						0);
					float sampleLuminance = _NRD_LinearToYCoCg(sampleRadiance).x;
					if (coplanarHit) {
						if (scalarWeight > 0.0) {
							coplanarRadiance += sampleRadiance * scalarWeight * GIStrength;
							coplanarHitWeight += scalarWeight;
#	ifdef SSGI_SH
							coplanarDirection += directionWeight * sampleLuminance * GIStrength;
#	endif
						}
						float openWeight;
						float3 openDirectionWeight;
						IntegrateBitfield(
							currentOccludedBitfield,
							projectedNormalNormalized, projectedNormalTangent,
							projectedNormalLength, sinN, cosN,
							float3(0, 0, 0), false,
							openWeight, openDirectionWeight);
						coplanarOpenWeight += openWeight;
						coplanarBits += float(countbits(currentOccludedBitfield));
#	ifdef SSGI_SH
						coplanarSkyDirection += openDirectionWeight;
#	endif
					} else if (scalarWeight > 0.0) {
						float3 diffuseRadiance = sampleRadiance * scalarWeight * GIStrength;
						totalRadiance += diffuseRadiance;
						if (trackFill)
							hitWeightTotal += scalarWeight;
#	ifdef SSGI_SH
						totalDirection += directionWeight * sampleLuminance * GIStrength;
#	endif
					}
				}
#endif  // GI
			}
		}

		normHitDist += float(countbits(globalOccludedBitfield)) / float(SSGI_MAX_RAY);

#if defined(DYNAMIC_CUBEMAPS)
		if (UseDynamicCubemap != 0) {
			float3 worldPos = ViewToWorldPosition(pixCenterPos, FrameBuffer::CameraViewInverse);
			float3 worldNormal = ViewToWorldVector(viewspaceNormal, FrameBuffer::CameraViewInverse);
			float3 iblRatio = 1.0;
#	if defined(IBL)
			[branch] if (SharedData::iblSettings.EnableIBL && SharedData::iblSettings.DALCMode < 2)
				iblRatio = ImageBasedLighting::GetIBLRatio();
#	endif
			[unroll] for (uint j = 0; j < SSGI_FALLBACK_SAMPLE_COUNT; j++)
			{
				uint maskSize = SSGI_MAX_RAY / SSGI_FALLBACK_SAMPLE_COUNT;
				uint mask = 0xFFFFFFFFu >> (SSGI_MAX_RAY - maskSize);
				uint bitOffset = j * maskSize;
				uint openBitfield = (~globalOccludedBitfield) & (mask << bitOffset);
				float openWeight;
				float3 openDirectionWeight;
				IntegrateBitfield(
					openBitfield,
					projectedNormalNormalized, projectedNormalTangent,
					projectedNormalLength, sinN, cosN,
					float3(0, 0, 0), false,
					openWeight, openDirectionWeight);

				if (openWeight <= 0)
					continue;

				float3 rayDir = normalize(openDirectionWeight / openWeight);
				float3 worldDir = ViewToWorldVector(rayDir, FrameBuffer::CameraViewInverse);
				float sampleFillBlend;
				float3 fallbackSample = SampleDiffuseFallbackCubemap(worldPos, worldNormal, worldDir, iblRatio, sampleFillBlend);
				if (trackFill) {
					openWeightTotal += openWeight;
					fillBlendTotal += sampleFillBlend * openWeight;
				}
				float3 contrib = fallbackSample * openWeight;
				fallbackRadiance += contrib;
#	ifdef SSGI_SH
				fallbackDirection += openDirectionWeight * _NRD_LinearToYCoCg(fallbackSample).x;
#	endif
			}
		}
#endif
	}

	normHitDist /= SSGI_ROTATION_COUNT;
	totalRadiance /= SSGI_ROTATION_COUNT;
	fallbackRadiance /= SSGI_ROTATION_COUNT;
	hitWeightTotal /= SSGI_ROTATION_COUNT;
	openWeightTotal /= SSGI_ROTATION_COUNT;
	fillBlendTotal /= SSGI_ROTATION_COUNT;
#ifdef SSGI_SH
	totalDirection /= SSGI_ROTATION_COUNT;
	fallbackDirection /= SSGI_ROTATION_COUNT;
#endif

	// Skylight tint: coplanar ground hits go back to the sky by coplanarReject. Their bounce is dropped by that share,
	// the open sky's mean radiance (per unit measure, before the fallback's multipliers, so it gets them too) fills
	// their measure, and they stop counting as occlusion. Without any open bit there is no sky estimate: legacy.
	[branch] if (coplanarReject > 0.0 && coplanarBits > 0.0)
	{
		coplanarRadiance /= SSGI_ROTATION_COUNT;
		coplanarOpenWeight /= SSGI_ROTATION_COUNT;
		coplanarBits /= SSGI_ROTATION_COUNT;
		const float reject = openWeightTotal > 0.0 ? coplanarReject : 0.0;
		const float3 openSkyMean = openWeightTotal > 0.0 ? fallbackRadiance / openWeightTotal : 0.0;
		const float3 skyForCoplanar = openSkyMean * coplanarOpenWeight * reject;
		const float fillShareOpen = openWeightTotal > 0.0 ? fillBlendTotal / openWeightTotal : 0.0;
		fallbackRadiance += skyForCoplanar;
		fillBlendTotal += fillShareOpen * coplanarOpenWeight * reject;
		openWeightTotal += coplanarOpenWeight * reject;
		totalRadiance += coplanarRadiance * (1.0 - reject);
		if (trackFill)
			hitWeightTotal += coplanarHitWeight / SSGI_ROTATION_COUNT * (1.0 - reject);
		normHitDist = max(normHitDist - reject * coplanarBits / float(SSGI_MAX_RAY), 0.0);
#	ifdef SSGI_SH
		totalDirection += coplanarDirection / SSGI_ROTATION_COUNT * (1.0 - reject);
		// The reclaimed sky keeps the open-direction moment of those bits, weighted like the fallback's own samples.
		fallbackDirection += coplanarSkyDirection / SSGI_ROTATION_COUNT * _NRD_LinearToYCoCg(openSkyMean).x * reject;
#	endif
	} else {
		totalRadiance += coplanarRadiance / SSGI_ROTATION_COUNT;
		if (trackFill)
			hitWeightTotal += coplanarHitWeight / SSGI_ROTATION_COUNT;
#	ifdef SSGI_SH
		totalDirection += coplanarDirection / SSGI_ROTATION_COUNT;
#	endif
	}

	// foxnne sky-lit shadows: the composite still applies IL Saturation (and its clamp) to the whole denoised signal,
	// as in legacy. On open sky the fallback should keep the sky's own colour instead, i.e. end at saturation
	// lerp(ILSaturation, 1, fillShare). Saturation is linear and keeps luminance, so pre-scaling the fallback's chroma
	// (its difference from its own luminance) by k = lerp(ILSaturation, 1, fillShare) / ILSaturation makes the
	// composite's pass land there. fillShare 0 leaves the signal untouched, so it is legacy exactly. k is limited so no
	// channel goes below zero, and nothing is done at an IL Saturation near 0 (grey cannot be undone).
	[branch] if (physicalSkyFill)
	{
		const float ilSaturation = SharedData::ssgiSettings.ILSaturation;
		const float fillShare = openWeightTotal > 0.0 ? saturate(fillBlendTotal / openWeightTotal) : 0.0;
		[branch] if (fillShare > 0.0 && ilSaturation > 1e-3 && ilSaturation != 1.0)
		{
			const float fallbackLum = Color::RGBToLuminance(fallbackRadiance);
			const float3 chroma = fallbackRadiance - fallbackLum;
			float chromaScale = lerp(ilSaturation, 1.0, fillShare) / ilSaturation;
			// lum + chroma * k >= 0 on every channel with negative chroma
			const float3 negChroma = max(-chroma, 0.0);
			const float maxNeg = max(negChroma.x, max(negChroma.y, negChroma.z));
			if (maxNeg > 0.0)
				chromaScale = min(chromaScale, fallbackLum / maxNeg);
			fallbackRadiance = fallbackLum + chroma * chromaScale;
		}

#if defined(DYNAMIC_CUBEMAPS)
		// Bounce Limit: a traced hit takes the place of the ambient light that direction would have brought. Cap the
		// traced bounce at BounceSkyLimit times that ambient (the mean fallback radiance of the open bits, over the hit
		// measure). It scales, so the bounce keeps its colour, and trims the screen-space overestimates: thick-occluder
		// guesses behind thin geometry, lit sides seen from a shadowed receiver. Real bounce off a sunlit wall can
		// exceed the sky it hides several times over, so useful values are 2 to 6. Its strength fades with the open
		// share of the hemisphere and the fill share, so it goes to nothing continuously where the ambient estimate
		// does (fully enclosed pixels, occluded sky).
		[branch] if (UseDynamicCubemap != 0 && SharedData::ssgiSettings.BounceSkyLimit > 0.0 && hitWeightTotal > 0.0 && openWeightTotal > 0.0)
		{
			const float openShare = openWeightTotal / (openWeightTotal + hitWeightTotal);
			const float capStrength = saturate(openShare * 4.0) * fillShare;
			const float ambientMean = _NRD_LinearToYCoCg(fallbackRadiance).x / openWeightTotal;
			const float bounceCap = SharedData::ssgiSettings.BounceSkyLimit * ambientMean * hitWeightTotal * GIStrength;
			const float tracedLuminance = _NRD_LinearToYCoCg(totalRadiance).x;
			const float bounceScale = lerp(1.0, min(1.0, bounceCap / max(tracedLuminance, EPSILON_DIVISION)), capStrength);
			totalRadiance *= bounceScale;
#	ifdef SSGI_SH
			totalDirection *= bounceScale;
#	endif
		}
#endif
	}

	normHitDist = saturate(normHitDist);
	// REBLUR defines normalized hit distance as diffuse AO/visibility: nearby occlusion tends to 0,
	// while open or distant geometry tends to 1. Exact 0 is reserved for an invalid/skipped lobe.
	normHitDist = max(saturate(1 - normHitDist), NRD_EPS);
#ifdef SSGI_SH
	float fallbackLuminance = _NRD_LinearToYCoCg(fallbackRadiance).x;
#endif
	fallbackRadiance = pow(abs(fallbackRadiance), SSGI_FALLBACK_POWER) * DiffuseCubemapMult;
	// The open-bit integration supplies directional visibility, while the final
	// GTAO visibility also represents unresolved local occlusion. Apply both to
	// ambient cubemap fallback; screen-space hit radiance remains unchanged.
	// AOPower shapes only this fallback occlusion: traced GI already carries its own visibility.
	fallbackRadiance *= pow(normHitDist, AOPower * (IsSkinPixel(dtid) ? SSGI_SKIN_FALLBACK_AO_SCALE : 1.0));

	o_ao = normHitDist;
	o_radiance = totalRadiance + fallbackRadiance;

	// foxnne sky fill debug (ssgiSettings.SkyFillDebug), shown by the deferred debug view SSGI (8)
	[branch] if (skyFillDebug != 0)
	{
		if (skyFillDebug == 1) {
			float iblOn = 0.0;
#	if defined(IBL)
			iblOn = SharedData::iblSettings.EnableIBL != 0 ? 1.0 : 0.0;
#	endif
			o_radiance = float3(UseDynamicCubemap != 0 ? 1.0 : 0.0, iblOn, UsePhysicalSkyFill() ? 1.0 : 0.0);
		} else if (skyFillDebug == 2) {
			const float fillShareDebug = openWeightTotal > 0.0 ? saturate(fillBlendTotal / openWeightTotal) : 0.0;
			const float openShareDebug = (openWeightTotal + hitWeightTotal) > 0.0 ? openWeightTotal / (openWeightTotal + hitWeightTotal) : 0.0;
#	if defined(SKYLIGHTING)
			const float skylightingCompiled = 1.0;
#	else
			const float skylightingCompiled = 0.0;
#	endif
			o_radiance = float3(fillShareDebug, openShareDebug, skylightingCompiled);
		} else if (skyFillDebug == 3) {
			o_radiance = totalRadiance;
		} else {
			o_radiance = fallbackRadiance;  // 4, and 5/6 (the fallback was built from the debug sky term)
		}
	}
#ifdef SSGI_SH
	// NRD SH stores the first directional moment. Keep its magnitude (directionality) and use
	// world space so temporal history and the world-space resolve guides share one basis.
	float fallbackLuminanceScale = _NRD_LinearToYCoCg(fallbackRadiance).x / max(fallbackLuminance, EPSILON_DIVISION);
	float3 directionalMoment = totalDirection + fallbackDirection * fallbackLuminanceScale;
	float radianceLuminance = _NRD_LinearToYCoCg(o_radiance).x;
	float3 averageDirectionVS = directionalMoment / max(radianceLuminance, EPSILON_DIVISION);
	// Preserve the first-moment magnitude; normalizing here would falsely turn
	// diffuse or opposing illumination into a fully directional lobe.
	o_direction = ViewToWorldVector(averageDirectionVS, FrameBuffer::CameraViewInverse);
#endif
}

[numthreads(8, 8, 1)] void main(const uint2 dtid : SV_DispatchThreadID) {
	const float2 frameScale = FrameDim * RcpTexDim;

#if defined(SSGI_QUARTER)
	// Trace one pixel per 2x2 block. Cycle through all four phases so REBLUR's
	// hit-distance reconstruction and temporal accumulation receive fresh data.
	uint2 phaseOffset = uint2(FrameIndex & 1u, (FrameIndex >> 1u) & 1u);
	uint2 pxCoord = dtid * 2u + phaseOffset;
	if (any(pxCoord >= uint2(FrameDim)))
		return;
	uint2 outCoord = pxCoord;
#elif defined(SSGI_HALF)
	uint colOffset = (dtid.y + FrameIndex) & 1;
	uint2 pxCoord = uint2(dtid.x * 2 + colOffset, dtid.y);
	uint2 outCoord = dtid.xy;
#else
	uint2 pxCoord = dtid;
	uint2 outCoord = dtid;
#endif

	float2 uv = (pxCoord + .5) * RCP_OUT_FRAME_DIM;

	float viewspaceZ = READ_DEPTH(srcWorkingDepth, pxCoord);
	if (viewspaceZ <= FP_Z) {
#ifdef SSGI_SH
		float4 sh1;
		outRadianceHitDist[outCoord] = REBLUR_FrontEnd_PackSh(float3(0, 0, 0), 0, float3(0, 0, 0), sh1, true);
		outSH1[outCoord] = sh1;
#else
		outRadianceHitDist[outCoord] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(float3(0, 0, 0), 0, true);
#endif
		return;
	}

	float2 normalSample = FULLRES_LOAD(srcNormal, pxCoord, uv * OUT_FRAME_SCALE, samplerLinearClamp);
	float3 viewspaceNormal = GBuffer::DecodeNormal(normalSample);

	float normHitDist = 0;
	float3 radiance = 0;
#ifdef SSGI_SH
	float3 direction = 0;
#endif

	CalculateGI(pxCoord, uv, viewspaceZ, viewspaceNormal, normHitDist, radiance
#ifdef SSGI_SH
		,
		direction
#endif
	);

	radiance = ClampDenoiserRadiance(filterNaN(radiance));

#ifdef SSGI_QUARTER
	// One out of four pixels carries a sample. Compensate its energy so the
	// spatially and temporally filtered signal remains unbiased.
	radiance *= 4.0;
#endif

#ifdef SSGI_SH
	float4 sh1;
	outRadianceHitDist[outCoord] = REBLUR_FrontEnd_PackSh(radiance, normHitDist, direction, sh1, true);
	outSH1[outCoord] = sh1;
#else
	outRadianceHitDist[outCoord] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance, normHitDist, true);
#endif
}
