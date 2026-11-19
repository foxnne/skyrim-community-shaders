// Light Shafts, step 2: the beams along each view ray, at half or quarter resolution.
//
// Near part, from the camera to NearRange or the surface: the directional cascades (trees, buildings, rocks), one
// compared read per step. Steps are placed so that each stands for the same share of the distance weight
// exp(-t / NearLength). The cloud and terrain light change slowly over this stretch and are read at its two ends.
// A step adds what the cascades give over the share of lit air around the camera.
//
// Far part, from NearRange to FarRange, the surface or the cloud base: the cloud shadow volume against the mean
// cloud transmittance, and the terrain shadow against the terrain shadow to both sides of the sample across the
// light. A step adds what it gets over that. This is where rays through gaps in the cloud and past ridges come
// from; under a clear or an overcast sky over open ground it adds nothing.
//
// Each pixel starts its steps at one of 16 ordered offsets; Resolve averages them out. The result depends on the
// pixel's ray, its depth and the world only: no history.

#define COMPUTESHADER
#define PS_LINEAR_SHADOW_SAMPLER
#include "Common/ViewAtmosphere.hlsli"
#include "PhysicalSky/Common.hlsli"
#include "ViewAtmosphere/MediumLighting.hlsli"

#include "LightShafts/LightShafts.hlsli"

Texture2D<float> SceneDepth : register(t40);
RWTexture2D<float4> ShaftsOut : register(u0);     // rgb: light to add, a: lit share of the near ray (debug)
RWTexture2D<float> ShaftsDepthOut : register(u1);  // the device depth each output pixel was computed for

[numthreads(8, 8, 1)] void main(uint2 pixel : SV_DispatchThreadID) {
	uint2 mainSize, outSize;
	SceneDepth.GetDimensions(mainSize.x, mainSize.y);
	ShaftsOut.GetDimensions(outSize.x, outSize.y);
	const uint2 activeMain = uint2(clamp(float2(mainSize) * FrameBuffer::DynamicResolutionParams1.xy, 1.0, float2(mainSize)));
	const uint divisor = LightShafts::Divisor;
	const uint2 activeOut = (activeMain + divisor - 1u) / divisor;
	if (any(pixel >= min(activeOut, outSize)))
		return;

	// One of the four scene pixels in the middle of the block stands for the output pixel: the nearest and the
	// farthest in a checkerboard, so both sides of a depth edge are there for the upsample to pick from.
	const uint2 first = pixel * divisor + (divisor / 2u - 1u);
	const bool nearest = ((pixel.x + pixel.y) & 1u) == 0u;
	uint2 scenePixel = min(first, activeMain - 1u);
	float depth = SceneDepth[scenePixel];
	[unroll] for (uint k = 1u; k < 4u; ++k)
	{
		const uint2 candidate = min(first + uint2(k & 1u, k >> 1u), activeMain - 1u);
		const float candidateDepth = SceneDepth[candidate];
		const bool nearer = FrameBuffer::ToStandardDepth(candidateDepth) < FrameBuffer::ToStandardDepth(depth);
		if (nearer == nearest && candidateDepth != depth) {
			depth = candidateDepth;
			scenePixel = candidate;
		}
	}
	ShaftsDepthOut[pixel] = depth;

	// The same reconstruction as the atmosphere composite: the jittered projection the depth was drawn with.
	const float2 uv = FrameBuffer::GetDynamicResolutionUnadjustedScreenPosition((float2(scenePixel) + 0.5) / float2(mainSize));
	float4 position = mul(FrameBuffer::CameraViewProjInverse, float4(uv * float2(2, -2) + float2(-1, 1), depth, 1));
	position.xyz /= position.w;
	const bool sky = FrameBuffer::ToStandardDepth(depth) >= 1.0 - 1e-6;
	const float sceneDistance = length(position.xyz);
	const float3 dir = position.xyz / max(sceneDistance, 1e-6);

	const float3 color = LightShafts::Prepared[uint2(0, 0)].rgb * LightShafts::Phase(dot(dir, LightShafts::LightDir));
	const bool debug = LightShafts::DebugMode != 0u;
	// Away from the light the phase function leaves nothing worth a march.
	if (!debug && dot(color, LightShafts::Luma) < 1.0e-5) {
		ShaftsOut[pixel] = 0.0;
		return;
	}

	const float4 prepared = LightShafts::Prepared[uint2(1, 0)];
	const float ambient = prepared.x;
	const float cloudMeanStored = prepared.y;
	const float cloudMean = LightShafts::CloudApplied(cloudMeanStored);
	const float3 camera = FrameBuffer::CameraPosAdjust.xyz;
	const float jitter = LightShafts::Bayer(pixel);
	const DirectionalShadowLightData light = DirectionalShadowLights[0];

	// Near part.
	const float nearEnd = sky ? LightShafts::NearRange : min(sceneDistance, LightShafts::NearRange);
	const float nearReach = 1.0 - exp(-nearEnd / LightShafts::NearLength);
	const float surroundStart = prepared.z * LightShafts::CloudApplied(prepared.w);
	const float surroundEnd = LightShafts::TerrainLight(camera + dir * nearEnd, 512.0) * LightShafts::CloudLight(camera + dir * nearEnd, cloudMeanStored);
	const float expected = LightShafts::VeilRemoval * ambient;
	float nearSum = 0.0;
	float litSum = 0.0;
	float distanceSum = 0.0;
	float weightSum = 0.0;
	[loop] for (uint i = 0u; i < LightShafts::NearSteps; ++i)
	{
		const float t = -LightShafts::NearLength * log(1.0 - (float(i) + jitter) / float(LightShafts::NearSteps) * nearReach);
		bool covered;
		float value = LightShafts::CascadeLight(dir * t, light, false, covered);
		// Past the cascades nothing is known: the air there is worth what is expected, and adds nothing.
		value = covered ? value : ambient;
		const float surround = lerp(surroundStart, surroundEnd, saturate(t / max(nearEnd, 1.0)));
		const float contrast = surround * max(value - expected, 0.0);
		nearSum += contrast;
		litSum += surround * value;
		distanceSum += contrast * t;
		weightSum += contrast;
	}
	const float steps = float(max(LightShafts::NearSteps, 1u));
	float depthOptical = nearReach * nearSum / steps;
	const float litShare = litSum / steps;

	// Far part.
	float farEnd = sky ? LightShafts::FarRange : min(sceneDistance, LightShafts::FarRange);
	if (LightShafts::CloudsActive()) {
		// Up to the cloud base: above it the ray is inside the cloud, which the sky pass draws.
		const float base = SharedData::physSkyData.zBottom + SharedData::physSkyData.volCloudLowBottom;
		if (camera.z < base && dir.z > 1e-4)
			farEnd = min(farEnd, (base - camera.z) / dir.z);
		else if (camera.z >= base)
			farEnd = 0.0;
	}
	const float farSpan = farEnd - LightShafts::NearRange;
	[branch] if (LightShafts::FarSteps > 0u && LightShafts::DistantStrength > 0.0 && farSpan > 1.0)
	{
		const float farReach = 1.0 - exp(-farSpan / LightShafts::FarLength);
		const float3 lightDir = PhysSky::GetCloudShadowLightDirection();
		const float horizontal = length(lightDir.xy);
		const float2 side = horizontal > 0.05 ? float2(-lightDir.y, lightDir.x) / horizontal : 0.0;
		float farSum = 0.0;
		[loop] for (uint j = 0u; j < LightShafts::FarSteps; ++j)
		{
			const float t = LightShafts::NearRange - LightShafts::FarLength * log(1.0 - (float(j) + jitter) / float(LightShafts::FarSteps) * farReach);
			const float3 where = camera + dir * t;
			const float blur = clamp(t * 0.05, 256.0, 4000.0);
			const float terrain = LightShafts::TerrainLight(where, blur);
			float expectedTerrain = 1.0;
			[branch] if (terrain > 0.0 && horizontal > 0.05)
			{
				const float3 offset = float3(side * max(LightShafts::RidgeRadius, 0.1 * t), 0.0);
				expectedTerrain = 0.5 * (LightShafts::TerrainLight(where + offset, blur) + LightShafts::TerrainLight(where - offset, blur));
			}
			const float cloud = terrain > 0.0 ? LightShafts::CloudLight(where, cloudMeanStored) : 0.0;
			const float contrast = max(terrain * cloud - LightShafts::VeilRemoval * expectedTerrain * cloudMean, 0.0);
			farSum += contrast;
			distanceSum += contrast * t * LightShafts::DistantStrength;
			weightSum += contrast * LightShafts::DistantStrength;
		}
		depthOptical += LightShafts::DistantStrength * farReach * farSum / float(LightShafts::FarSteps);
	}

	float3 beams = color * (1.0 - exp(-LightShafts::Density * depthOptical));
	[branch] if (weightSum > 0.0)
	{
		// The air between the camera and the beams dims them like anything else at that distance.
		const float3 centre = dir * (distanceSum / weightSum);
		const float4 clip = mul(FrameBuffer::CameraViewProjUnjittered, float4(centre, 1));
		const float2 volumeUv = clip.xy / max(clip.w, 1e-6) * float2(0.5, -0.5) + 0.5;
		beams *= ViewAtmosphere::Sample(volumeUv, length(centre), LinearSampler, 0.0).transmittance;
	}
	// A ceiling that is approached smoothly: dim beams keep their brightness, none exceeds it.
	const float luminance = dot(beams, LightShafts::Luma);
	if (luminance > 1.0e-6)
		beams *= LightShafts::MaxBrightness * (1.0 - exp(-luminance / max(LightShafts::MaxBrightness, 1e-4))) / luminance;
	ShaftsOut[pixel] = float4(min(max(beams, 0.0), 65504.0), litShare);
}
