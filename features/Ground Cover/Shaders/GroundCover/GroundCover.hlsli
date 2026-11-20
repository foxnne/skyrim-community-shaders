// Ground Cover: carries the colour of the grass on the plain up the lower slopes of rock next to it.
// The inverse of Snow Cover: up-facing rock near the level where grass grows takes the grass colour, in patches
// that thin out with height and with slope, and Snow Cover's snow wins over it.
//
// ColourMap: the grass colour map of the worldspace (grass-colour-map project). rgb: mean grass colour, filled
//   outward where no grass grows; a: how much grass grows there. sRGB format, so samples are linear. With mips.
// LevelMap: the altitude at which grass grows nearby, filled outward the same way (ground-cover/bake_level_map.py).
//   world z = value * LEVEL_RANGE + LEVEL_MIN. Optional: without it settings.PlainLevel is used everywhere.
// Both cover the rectangle of cells in their file name; uv = world xy * MapScale + MapOffset, row 0 north.
#include "Common/Color.hlsli"
#include "Common/ColorManagement.hlsli"
#include "Common/LightingCommon.hlsli"
#include "Common/SharedData.hlsli"
#if defined(PSHADER)

namespace GroundCover
{
	Texture2D<float4> ColourMap : register(t58);
	Texture2D<float> LevelMap : register(t59);

	static const float LEVEL_MIN = -65536.0;
	static const float LEVEL_RANGE = 131072.0;
	// Mip of ColourMap read for the colour (512 units per texel: no single grass patch shows) and for how much
	// grass grows in the area (four cells, 16 km, per texel).
	static const float COLOUR_MIP = 1.0;
	static const float REGION_MIP = 6.0;

	namespace Flags
	{
		static const uint Terrain = 1u;
		static const uint ObjectLOD = 2u;
		static const uint Scree = 4u;
		static const uint LevelMap = 8u;
	}

	float Hash(float2 p)
	{
		float3 p3 = frac(p.xyx * 0.1031);
		p3 += dot(p3, p3.yzx + 33.33);
		return frac((p3.x + p3.y) * p3.z);
	}

	// Value noise, 0..1, one period per unit of p.
	float Noise(float2 p)
	{
		float2 cell = floor(p);
		float2 f = p - cell;
		f = f * f * (3.0 - 2.0 * f);
		float a = Hash(cell);
		float b = Hash(cell + float2(1, 0));
		float c = Hash(cell + float2(0, 1));
		float d = Hash(cell + float2(1, 1));
		return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
	}

	float2 MapUV(float3 p)
	{
		return p.xy * SharedData::groundCoverSettings.MapScale + SharedData::groundCoverSettings.MapOffset;
	}

	// The altitude grass grows at next to p.
	float GetPlainLevel(float2 uv)
	{
		float level = SharedData::groundCoverSettings.PlainLevel;
		[branch] if (SharedData::groundCoverSettings.Flags & Flags::LevelMap)
			level += LevelMap.SampleLevel(SampColorSampler, uv, 0) * LEVEL_RANGE + LEVEL_MIN;
		return level;
	}

	// Patches: `amount` (0..1, how much cover the place can hold) against noise. 1 stays 1 and 0 stays 0, in
	// between the cover breaks into patches that shrink as amount falls.
	float Patches(float amount, float noise)
	{
		float strength = min(SharedData::groundCoverSettings.NoiseAmount, 0.95);
		return saturate((amount - noise * strength) / (1.0 - strength));
	}

	namespace Debug
	{
		static const uint Weight = 1u;     // final weight in magenta
		static const uint Why = 2u;        // the first term that rejects each pixel, see the colours in Apply
		static const uint Slope = 3u;      // 3, 5..10 show one term as grey, 0 black .. 1 white
		static const uint Height = 4u;     // height over the plain level: blue below, green..red up the band, dark red above
		static const uint Rise = 5u;       // rise term times band term
		static const uint NoiseTerm = 6u;  // the patch noise
		static const uint Snow = 7u;       // 1 - snow weight
		static const uint Grey = 8u;
		static const uint White = 9u;
		static const uint Region = 10u;
		static const uint Eligible = 11u;  // every draw that reaches the layer, coloured by draw path, see DebugDrawPath
	}

	// Surfaces the layer does not run on (no rock bit, object LOD or terrain switched off), in debug views 2 and up.
	float3 DebugNotEligible(float3 baseColor)
	{
		return Color::RGBToLuminance(baseColor) * 0.35;
	}

	/**
	 * Debug view 11. Colour = draw path: magenta placed mesh, orange object LOD, green terrain, blue terrain LOD;
	 * full where the layer runs on it, a third where it does not (no rock bit, or the toggle for it is off).
	 * Pattern = pass: solid deferred, horizontal stripes forward in the world, checker forward without the
	 * in-world flag. What keeps its normal colour does not reach the layer at all.
	 */
	float3 DebugDrawPath(uint path, bool eligible, bool deferred, bool inWorld, float2 pixel)
	{
		float3 colour = path == 0 ? float3(1.0, 0.0, 1.0) : path == 1 ? float3(1.0, 0.5, 0.0) :
		                                                path == 2     ? float3(0.0, 1.0, 0.0) :
		                                                                float3(0.1, 0.3, 1.0);
		if (!eligible)
			colour *= 0.33;
		const uint2 cell = uint2(pixel) / 8u;
		if (!deferred && inWorld && (cell.y & 1u))
			colour *= 0.15;
		if (!deferred && !inWorld && ((cell.x ^ cell.y) & 1u))
			colour *= 0.15;
		return colour;
	}

	float3 HeightRamp(float t)
	{
		if (t < 0.0)
			return float3(0.1, 0.2, 1.0);
		if (t > 1.0)
			return float3(0.35, 0.0, 0.0);
		return t < 0.5 ? lerp(float3(0.0, 1.0, 0.0), float3(1.0, 1.0, 0.0), t * 2.0) : lerp(float3(1.0, 1.0, 0.0), float3(1.0, 0.0, 0.0), t * 2.0 - 1.0);
	}

	/**
	 * Blends the material towards ground cover.
	 * @param worldNormal Shading normal, world space.
	 * @param p Absolute world position.
	 * @param snow Snow Cover's weight at this pixel, 0 without it.
	 * @param skyOpen 0 in creases and under overhangs, 1 under the open sky.
	 * @param terrain The surface is terrain: only its slopes without grass of their own take cover.
	 * @return The weight of the cover, 0..1.
	 */
	float Apply(inout MaterialProperties material, float3 worldNormal, float3 p, float viewDist, float snow, float skyOpen, bool terrain)
	{
		const uint debugView = SharedData::groundCoverSettings.DebugView;
		// Debug views 2 and up need every term, so they take no early out.
		const bool allTerms = debugView >= Debug::Why;
		const float bandHeight = max(SharedData::groundCoverSettings.BandHeight, 1.0);
		const bool scree = (SharedData::groundCoverSettings.Flags & Flags::Scree) != 0;

		float slopeTerm = smoothstep(SharedData::groundCoverSettings.SlopeMin, max(SharedData::groundCoverSettings.SlopeMax, SharedData::groundCoverSettings.SlopeMin + 0.01), worldNormal.z);
		// Flat terrain has its own texture, roads and grass.
		if (terrain)
			slopeTerm *= 1.0 - smoothstep(0.93, 0.985, worldNormal.z);
		const float snowTerm = 1.0 - saturate(snow);

		// Rock is grey. Wood, thatch, soil and anything painted are not, and stay as they are. Both tests run on
		// the square root of the albedo, so they mean about the same whether BaseColor is linear (PBR, linear
		// lighting) or gamma encoded.
		const float3 base = material.BaseColor;
		const float3 perceptual = sqrt(max(base, 0.0));
		const float baseMax = max(perceptual.r, max(perceptual.g, perceptual.b));
		const float baseMin = min(perceptual.r, min(perceptual.g, perceptual.b));
		const float greyLimit = SharedData::groundCoverSettings.GreyLimit;
		const float greyTerm = greyLimit < 1.0 ? 1.0 - smoothstep(0.6 * greyLimit, greyLimit, (baseMax - baseMin) / max(baseMax, 1e-4)) : 1.0;
		// Snow painted on a mesh or projected onto it is grey too, but brighter than any rock.
		const float whiteLimit = SharedData::groundCoverSettings.WhiteLimit;
		const float whiteTerm = whiteLimit < 1.0 ? 1.0 - smoothstep(whiteLimit - 0.12, whiteLimit, baseMax) : 1.0;

		float up = slopeTerm * snowTerm * greyTerm * whiteTerm;
		if (!allTerms && up <= 0.001)
			return 0;

		const float2 uv = MapUV(p);
		const float height = p.z - GetPlainLevel(uv);
		const float heightLimit = bandHeight + (scree ? max(SharedData::groundCoverSettings.ScreeHeight, 0.0) : 0.0);
		if (!allTerms && height >= heightLimit)
			return 0;
		// Rocks lying on the plain: nothing at or below the plain level, fading in over RiseHeight above it.
		const float rise = SharedData::groundCoverSettings.RiseHeight;
		const float riseTerm = rise > 0.0 ? smoothstep(0.0, rise, height) : 1.0;
		const float bandTerm = 1.0 - smoothstep(0.0, 1.0, saturate(height / bandHeight));

		// No grass anywhere in the area (four cells, about 16 km): snow fields, ash, glaciers. Coarse on purpose:
		// the mountains themselves grow no grass, the plain at their feet does.
		const float regionMin = max(SharedData::groundCoverSettings.RegionCoverage, 1e-3);
		const float regionTerm = smoothstep(0.25 * regionMin, regionMin, ColourMap.SampleLevel(SampColorSampler, uv, REGION_MIP).a);
		up *= regionTerm;
		if (!allTerms && up <= 0.001)
			return 0;

		// Two scales of noise: drifts the size of a rock face, and the ragged edge of each. Both fade to their
		// mean once a pixel covers more than half a period, so far slopes do not shimmer.
		const float2 noisePos = p.xy + p.z * float2(0.31, 0.17);
		const float footprint = viewDist * 0.001;
		const float invPatch = SharedData::groundCoverSettings.InvPatchSize;
		const float invDetail = SharedData::groundCoverSettings.InvDetailSize;
		float patch = 0.65 * Noise(noisePos * invPatch) + 0.35 * Noise(noisePos * invPatch * 2.7 + 17.3);
		patch = lerp(patch, 0.5, saturate(footprint * invPatch * 4.0 - 1.0));
		float detail = Noise(noisePos * invDetail + 41.7);
		detail = lerp(detail, 0.5, saturate(footprint * invDetail * 4.0 - 1.0));
		const float noise = saturate(0.65 * patch + 0.35 * detail);

		float cover = Patches(up * riseTerm * bandTerm, noise);

		[branch] if (allTerms)
		{
			float3 debugColour;
			if (debugView == Debug::Why) {
				// The first term that rejects the pixel, in this order; green where all pass, as bright as the cover.
				if (slopeTerm < 0.05)
					debugColour = float3(1.0, 0.0, 0.0);  // red: too steep
				else if (snowTerm < 0.05)
					debugColour = float3(1.0, 1.0, 1.0);  // white: under snow
				else if (greyTerm < 0.05)
					debugColour = float3(1.0, 1.0, 0.0);  // yellow: too colourful for rock
				else if (whiteTerm < 0.05)
					debugColour = float3(0.0, 1.0, 1.0);  // cyan: too bright, painted snow
				else if (regionTerm < 0.05)
					debugColour = float3(1.0, 0.5, 0.0);  // orange: no grass in the area
				else if (riseTerm < 0.05)
					debugColour = float3(0.1, 0.2, 1.0);  // blue: at or below the plain level
				else if (bandTerm < 0.05)
					debugColour = float3(0.6, 0.0, 1.0);  // purple: above the band
				else
					debugColour = float3(0.0, 0.15 + 0.85 * cover, 0.0);
			} else if (debugView == Debug::Slope)
				debugColour = slopeTerm;
			else if (debugView == Debug::Height)
				debugColour = HeightRamp(height / bandHeight);
			else if (debugView == Debug::Rise)
				debugColour = riseTerm * bandTerm;
			else if (debugView == Debug::NoiseTerm)
				debugColour = noise;
			else if (debugView == Debug::Snow)
				debugColour = snowTerm;
			else if (debugView == Debug::Grey)
				debugColour = greyTerm;
			else if (debugView == Debug::White)
				debugColour = whiteTerm;
			else if (debugView == Debug::Region)
				debugColour = regionTerm;
			else
				debugColour = float3(1.0, 0.0, 1.0);
			material.BaseColor = debugColour;
			return 0;
		}

		if (scree) {
			// Above the vegetation: loose rock, lighter and a little warmer than the faces it lies under.
			const float screeTop = bandHeight + max(SharedData::groundCoverSettings.ScreeHeight, 1.0);
			const float screeBand = smoothstep(0.4 * bandHeight, bandHeight, height) * (1.0 - smoothstep(bandHeight, screeTop, height));
			const float screeWeight = SharedData::groundCoverSettings.ScreeStrength * Patches(up * screeBand, 1.0 - noise) * (1.0 - cover);
			const float baseLuma = Color::RGBToLuminance(base);
			material.BaseColor = lerp(material.BaseColor, lerp(baseLuma, base, 0.6) * float3(1.3, 1.26, 1.18), screeWeight);
		}
		if (cover <= 0.001)
			return 0;

		const float4 map = ColourMap.SampleLevel(SampColorSampler, uv, COLOUR_MIP);
		// Terrain where grass grows already has the colour.
		if (terrain)
			cover *= 1.0 - smoothstep(0.3, 0.7, map.a);
		float3 colour = Color::Saturation(map.rgb, SharedData::groundCoverSettings.Saturation) * SharedData::groundCoverSettings.Brightness;
		// As Snow Cover brings its albedo into the space BaseColor is in.
#	if defined(TRUE_PBR)
		colour = Color::Albedo(Color::LinearSRGBToWorking(colour));
#	else
		colour = Color::Albedo(ColorManagement::DiffuseToWorking(TransferFunctions::LinearToGamma22(colour))) * Color::PBRLightingScale;
#	endif
		// LuminanceMatch 0: the rock keeps its own light and dark and takes only the hue. 1: it takes the grass
		// colour as it is.
		const float keepLuma = clamp(Color::RGBToLuminance(base) / max(Color::RGBToLuminance(colour), 1e-4), 0.25, 4.0);
		colour *= lerp(keepLuma, 1.0, SharedData::groundCoverSettings.LuminanceMatch);
		colour *= 1.0 - SharedData::groundCoverSettings.CreaseDarkening * (1.0 - skyOpen);

		const float weight = saturate(SharedData::groundCoverSettings.Strength * cover);
		if (debugView == Debug::Weight) {
			material.BaseColor = lerp(material.BaseColor, float3(1, 0, 1), weight);
			return weight;
		}
		material.BaseColor = lerp(material.BaseColor, colour, weight);

		// Moss and lichen do not shine.
		const float matte = weight * SharedData::groundCoverSettings.GlossReduction;
		material.Roughness = lerp(material.Roughness, max(material.Roughness, 0.9), matte);
		material.F0 = lerp(material.F0, min(material.F0, 0.02), matte);
#	if defined(TRUE_PBR)
		material.Metallic *= 1.0 - matte;
#	else
		material.SpecularColor *= 1.0 - matte;
#	endif
		return weight;
	}
}

#endif
