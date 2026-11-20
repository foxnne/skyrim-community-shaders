#ifndef LIGHT_SHAFTS_CONSTANTS_HLSLI
#define LIGHT_SHAFTS_CONSTANTS_HLSLI

// Constants of the Light Shafts passes. Mirrors LightShafts::ShaftData in src/Features/LightShafts.h.

namespace LightShafts
{
	cbuffer LightShaftsCB : register(b2)
	{
		float3 LightDir;        // toward the body that lights the scene
		float DiskRadius;       // its angular radius, radians
		float3 LightColor;      // its colour above the atmosphere, strength and fades included
		float Density;          // optical depth of a fully contrasting ray
		float NearLength;       // distance over which the near weight falls to 1/e, game units
		float NearRange;        // end of the near march
		float FarLength;        // the same for the far part, measured from NearRange
		float FarRange;         // end of the far march
		float PhaseG;           // anisotropy of the phase function
		float CloudOcclusion;   // 0: clouds ignored, 1: clouds shade and shape the beams
		float MaxBrightness;    // ceiling of the added luminance, scene units
		float VeilRemoval;      // how much of the expected light is taken off: 1 leaves contrast only
		float DistantStrength;  // weight of the far part
		float ShadowBias;
		float AmbientHistory;  // weight of last frame's lit share around the camera, 0 without history
		float AmbientRadius;   // radius around the camera that share is taken over, game units
		uint NearSteps;
		uint FarSteps;
		uint Divisor;         // 2 or 4: the march runs at 1/Divisor of the scene resolution
		uint DebugMode;       // 0 off, 1 lit share of the ray, 2 beams only
		float RidgeRadius;    // least distance to the sides the terrain shadow is compared over, game units
		float AmbientHeight;  // height above the camera the lit share is taken over, game units
		float2 LightUv;        // where the light stands on the screen, 0..1, possibly outside
		float ScreenWeight;    // share of the distant rays taken from the screen (the light is on or near it)
		uint ScreenSteps;      // samples from each pixel toward the light on the screen
		float SilhouetteRays;  // how much terrain and objects against the sky count as contrast, like cloud does
		float Padding;
	};

	static const float3 Luma = float3(0.2126, 0.7152, 0.0722);
}

#endif
