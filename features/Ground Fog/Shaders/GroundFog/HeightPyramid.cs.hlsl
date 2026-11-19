// Ground Fog: builds one level of the coarse copies of the terrain height map (see HeightPyramid in GroundFog.hlsli).
// Run once per level when the height map changes.
//
// x: the mean height under the texel. y: the highest height under the texel and one source texel beyond it on every
// side. Levels are built from one another, so the margins add up to at least half a texel of the level being
// written, which is what GroundFog::GroundMaxAlong relies on.

Texture2D<float2> Source : register(t0);  // the height map (FIRST_LEVEL, heights in x) or the level before this one
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
	Target[texel] = float2(mean, highest);
}
