#ifndef VIEW_ATMOSPHERE_DISTANCE_EXAGGERATION_HLSLI
#define VIEW_ATMOSPHERE_DISTANCE_EXAGGERATION_HLSLI

#include "Common/Game.hlsli"
#include "Common/SharedData.hlsli"

// Distance exaggeration: the game world is a few kilometres across, so physically sized air gives almost no aerial
// perspective. Surfaces therefore see the air between them and the camera as if the path were longer. This is done
// by adding air (never fog) to each interval of the ray, in a second copy of the integrated medium that only
// surfaces read; sky, clouds and celestial bodies keep the physical copy.
namespace ViewAtmosphere
{
	/// Extra path per unit of real path at a distance (game units) along a camera ray: x for air (Rayleigh), y for
	/// aerosol haze. 0 is physical. sinElevation is the z component of the ray direction.
	float2 ExtraPath(float distance, float sinElevation)
	{
		const SharedData::ViewAtmosphereSettings settings = SharedData::viewAtmosphereSettings;
		const SharedData::PhysSkyData sky = SharedData::physSkyData;
		const float2 strength = (clamp(settings.distanceExaggeration, 1.0, 16.0) - 1.0) * float2(1.0, saturate(settings.distanceExaggerationHaze));
		const float start = max(settings.distanceExaggerationStart, 0.0) / GAME_UNIT_TO_M;
		const float span = max(settings.distanceExaggerationFull / GAME_UNIT_TO_M - start, 1.0 / GAME_UNIT_TO_M);
		const float t = saturate((distance - start) / span);
		const float ramp = t * t * (3.0 - 2.0 * t);
		// Integral of the ramp from the camera to here: the extra length an unlimited ray has gathered so far.
		const float gathered = span * t * t * t * (1.0 - 0.5 * t) + max(distance - start - span, 0.0);

		// A surface can never have more air in front of it than the sky behind it has in total, or it would end up
		// hazier than the sky it stands against. Lengths are weighted by density relative to the camera: the whole
		// column is one scale height over the sine of the elevation, bounded by the grazing value at the horizon.
		const float2 falloff = max(float2(sky.rayleighFalloff, sky.aerosolFalloff), 1e-9);
		float2 column = 1.0 / (falloff * max(sinElevation, sqrt(2.0 / (3.14159265 * max(sky.rPlanet, 1.0) * falloff))));
		// Near the horizon that column is hundreds of kilometres, far more than it takes to saturate. The added air
		// is cleaner (bluer) than the real mix whenever the haze share is below 1, so a surface buried in it would
		// settle on a colour slightly bluer and brighter than the horizon behind it. Stop at about one optical
		// depth in the strongest channel: the far range keeps a silhouette and stays under the sky.
		const float3 hazeExtinction = sky.aerosolScatter + sky.aerosolAbsorption;
		const float2 strongest = float2(max(sky.rayleighScatter.x, max(sky.rayleighScatter.y, sky.rayleighScatter.z)),
			max(hazeExtinction.x, max(hazeExtinction.y, hazeExtinction.z)));
		column = min(column, 1.0 / max(strongest, 1e-12));
		const float2 climb = clamp(distance * sinElevation * falloff, -4.0, 30.0);
		const float2 meanDensity = float2(
			abs(climb.x) < 1e-3 ? 1.0 - 0.5 * climb.x : (1.0 - exp(-climb.x)) / climb.x,
			abs(climb.y) < 1e-3 ? 1.0 - 0.5 * climb.y : (1.0 - exp(-climb.y)) / climb.y);
		const float2 used = (distance + strength * gathered) * meanDensity;
		// Ease out over the last quarter of the column instead of stopping dead.
		const float2 room = saturate((column - used) / (0.25 * column));
		return strength * ramp * room;
	}
}

#endif
