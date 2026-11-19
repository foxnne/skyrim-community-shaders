// Ground Fog: builds one level of the coarse copies of the terrain height map (see HeightPyramid in GroundFog.hlsli).
// Run once per level when the height map changes.
//
// x: the mean height under the texel. y: the highest height under the texel and one source texel beyond it on every
// side. Levels are built from one another, so the margins add up to at least half a texel of the level being
// written, which is what GroundFog::GroundMaxAlong relies on.
//
// The fog lies on water where water covers the terrain. So that the bound holds for that surface too, the first
// level takes in the highest water surface around each texel from the baked water map, when there is one.

Texture2D<float2> Source : register(t0);  // the height map (FIRST_LEVEL, heights in x) or the level before this one
#ifdef FIRST_LEVEL
// z: the highest water surface in the texel and one texel around it, encoded like the height map; 0: none. Not
// bound (reads 0) when the worldspace has no water map.
Texture2D<float4> WaterMap : register(t1);
#endif
RWTexture2D<float2> Target : register(u0);
SamplerState LinearSampler : register(s0);

[numthreads(8, 8, 1)] void main(uint2 texel : SV_DispatchThreadID) {
	uint2 targetSize, sourceSize;
	Target.GetDimensions(targetSize.x, targetSize.y);
	Source.GetDimensions(sourceSize.x, sourceSize.y);
	if (any(texel >= targetSize))
		return;

	// By uv, not by doubling the index: sizes are not always even and the levels must stay aligned to the map.
	const float2 uvMin = float2(texel) / float2(targetSize);
	const float2 uvMax = float2(texel + 1u) / float2(targetSize);
	const float2 center = 0.5 * (uvMin + uvMax);
	const float2 quarter = 0.25 * (uvMax - uvMin);
	const float mean = 0.25 * (Source.SampleLevel(LinearSampler, center + quarter * float2(-1, -1), 0).x + Source.SampleLevel(LinearSampler, center + quarter * float2(1, -1), 0).x +
								  Source.SampleLevel(LinearSampler, center + quarter * float2(-1, 1), 0).x + Source.SampleLevel(LinearSampler, center + quarter * float2(1, 1), 0).x);

	const int2 last = int2(sourceSize) - 1;
	const int2 first = int2(floor(uvMin * float2(sourceSize))) - 1;
	const int2 end = int2(ceil(uvMax * float2(sourceSize)));  // one past the texels under this one, so the margin
	float highest = 0.0;
	[loop] for (int y = first.y; y <= end.y; ++y)
	{
		[loop] for (int x = first.x; x <= end.x; ++x)
		{
			const float2 value = Source.Load(int3(clamp(int2(x, y), 0, last), 0));
#ifdef FIRST_LEVEL
			highest = max(highest, value.x);
#else
			highest = max(highest, value.y);
#endif
		}
	}
#ifdef FIRST_LEVEL
	uint2 waterSize;
	WaterMap.GetDimensions(waterSize.x, waterSize.y);
	if (all(waterSize > 0u)) {
		// The water map's texels are at least as long as this level's, and its z channel reaches one of its texels
		// beyond each, which covers this texel and its margin. (The half height map texel by which the two maps'
		// uv differ is far inside that.)
		const int2 waterTexel = clamp(int2(center * float2(waterSize)), 0, int2(waterSize) - 1);
		highest = max(highest, WaterMap.Load(int3(waterTexel, 0)).z);
	}
#endif
	Target[texel] = float2(mean, highest);
}
