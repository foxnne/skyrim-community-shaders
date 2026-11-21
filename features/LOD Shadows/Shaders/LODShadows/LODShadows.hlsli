#ifndef LOD_SHADOWS_HLSLI
#define LOD_SHADOWS_HLSLI

// Cached directional depth of terrain, object LOD and tree LOD, ported from open-shaders.
// Include after Common/SharedData.hlsli. The map data comes from the shared feature buffer
// (SharedData::lodShadowsSettings); the depth array is the only texture.

namespace LODShadows
{
	static const uint MapCount = 3;
	static const uint DepthSliceCount = MapCount * 3;
	static const float BorderWidth = 0.05;
	static const float MinReceiverPlaneCosine = 1e-4;
	static const float MinReceiverPlaneDepth = 1e-20;
	static const float MinCoverage = 1e-6;
	// The native cascades fade out over the last fifth of their range
	static const float NativeFadeStartFraction = 0.8;

	// Slices [0, DepthSliceCount): every caster. Slices [DepthSliceCount, 2 * DepthSliceCount): the same
	// maps without the loaded objects that the native cascades already shadow.
	Texture2DArray<float> DepthMaps : register(t49);

	// True while at least one map is complete
	bool IsActive()
	{
		return SharedData::lodShadowsSettings.Maps[0].Parameters.w > 0.0;
	}

	// True while all maps are complete and hold the terrain: the heightmap terrain shadow can yield inside them
	bool CoversTerrain()
	{
		return SharedData::lodShadowsSettings.Maps[1].Parameters.w > 0.0;
	}

	// 0 inside the native cascades (loaded objects are shadowed there already), 1 past them
	float GetObjectShadowWeight(float3 position)
	{
		float endDistance = SharedData::lodShadowsSettings.Maps[2].Parameters.w;
		float depth = SharedData::GetScreenDepth(FrameBuffer::GetShadowDepth(position));
		return endDistance > 0.0 ? smoothstep(endDistance * NativeFadeStartFraction, endDistance, depth) : 1.0;
	}

	float CompareBilinear(float2 uv, float slice, float depth, float2 depthGradient, float texelSize, SamplerState linearSampler)
	{
		float4 depths = DepthMaps.GatherRed(linearSampler, float3(uv, slice));
		float2 fraction = frac(uv / texelSize - 0.5);
		float2 baseOffset = -fraction * texelSize;
		float4 receiverDepths = depth + dot(depthGradient, baseOffset) +
		                        float4(depthGradient.y, depthGradient.x + depthGradient.y, depthGradient.x, 0.0) * texelSize;
		float4 lit = step(receiverDepths, depths);
		return lerp(lerp(lit.w, lit.z, fraction.x), lerp(lit.x, lit.y, fraction.x), fraction.y);
	}

	// x: visibility against every caster, y: against the casters outside the native cascades
	float2 SampleMap(float3 worldPosition, float3 positionDx, float3 positionDy, float4x4 worldToClip,
		float slice, float bias, float texelSize, SamplerState linearSampler, out float coverage, bool sampleDistant)
	{
		float3 clipPosition = mul(float4(worldPosition, 1), worldToClip).xyz;
		float2 uv = clipPosition.xy * float2(0.5, -0.5) + 0.5;
		float edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
		coverage = 0.0;
		float2 visibility = 1.0;
		[branch] if (edge > 0 && clipPosition.z > 0 && clipPosition.z < 1)
		{
			float3 shadowDx = mul(float4(positionDx, 0), worldToClip).xyz * float3(0.5, -0.5, 1.0);
			float3 shadowDy = mul(float4(positionDy, 0), worldToClip).xyz * float3(0.5, -0.5, 1.0);
			float3 receiverPlane = cross(shadowDx, shadowDy);
			float2 depthGradient = 0.0;
			[branch] if (abs(receiverPlane.z) > MinReceiverPlaneCosine * length(receiverPlane))
				depthGradient = -receiverPlane.xy * sign(receiverPlane.z) / max(abs(receiverPlane.z), MinReceiverPlaneDepth);
			coverage = saturate(edge / BorderWidth);
			visibility = CompareBilinear(uv, slice, clipPosition.z - bias, depthGradient, texelSize, linearSampler);
			[branch] if (sampleDistant)
				visibility.y = CompareBilinear(uv, slice + DepthSliceCount, clipPosition.z - bias, depthGradient, texelSize, linearSampler);
		}
		return visibility;
	}

	// Blends from the previous map of the level to the current one while a new map fades in
	float2 SampleMapHistory(float3 worldPosition, float3 positionDx, float3 positionDy, SharedData::LODShadowMapData map,
		SamplerState linearSampler, out float coverage, bool sampleDistant)
	{
		float2 visibility = SampleMap(worldPosition, positionDx, positionDy, map.WorldToClip,
			map.Parameters.x, map.Parameters.z, map.Filter.x, linearSampler, coverage, sampleDistant);
		[branch] if (map.History.z > 0.0 && map.History.y < 1.0)
		{
			float previousCoverage;
			float2 previousVisibility = SampleMap(worldPosition, positionDx, positionDy, map.HistoryWorldToClip,
				map.History.x, map.Filter.z, map.Filter.x, linearSampler, previousCoverage, sampleDistant);
			float2 occlusion = lerp((1.0 - previousVisibility) * previousCoverage, (1.0 - visibility) * coverage, map.History.y);
			coverage = lerp(previousCoverage, coverage, map.History.y);
			visibility = 1.0 - occlusion / max(coverage, MinCoverage);
		}
		return visibility;
	}

	// Sun or moon visibility at a world position (not camera relative). positionDx and positionDy are its
	// screen-space derivatives, or 0 for no receiver plane correction. objectWeight is GetObjectShadowWeight.
	// coverage is the share of the result that the maps account for: 0 outside all of them.
	float GetShadow(float3 worldPosition, float3 positionDx, float3 positionDy, float objectWeight, SamplerState linearSampler, out float coverage)
	{
		float shadow = 1.0;
		float remaining = 1.0;
		[loop] for (uint i = 0; i < MapCount && remaining > 0.0; ++i)
		{
			SharedData::LODShadowMapData map = SharedData::lodShadowsSettings.Maps[i];
			[branch] if (map.Parameters.y == 0) continue;
			[branch] if (objectWeight == 0.0)
			{
				map.Parameters.x += DepthSliceCount;
				map.History.x += DepthSliceCount;
			}
			float mapCoverage;
			bool blendObjects = objectWeight > 0.0 && objectWeight < 1.0;
			float2 visibilityPair = SampleMapHistory(worldPosition, positionDx, positionDy, map, linearSampler, mapCoverage, blendObjects);
			float visibility = blendObjects ? lerp(visibilityPair.y, visibilityPair.x, objectWeight) : visibilityPair.x;
			float occlusion = (1.0 - visibility) * mapCoverage;
			shadow -= occlusion * remaining;
			remaining *= 1.0 - mapCoverage;
		}
		coverage = 1.0 - remaining;
		return shadow;
	}
}
#endif
