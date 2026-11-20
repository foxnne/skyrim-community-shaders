// Time-of-day period weights from the sun's elevation. Plain maths with no game types, so the Looks Lab tooling
// (lab_core.js, build.py, baseline_shoot.py) can mirror it line for line and a test can compile it on its own.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace ScenePeriodSun
{
	/// Dawn, Sunrise, Day, Sunset, Dusk, Night: SceneSettingsManager::TimeOfDayPeriod's order.
	inline constexpr int kPeriodCount = 6;
	using Weights = std::array<float, kPeriodCount>;

	/// Hours either side of the sun's highest and lowest point over which morning hands over to evening.
	/// Only seen when the sun never climbs into Day or never sinks into Night; otherwise both sides weigh nothing there.
	inline constexpr float kCulminationBlendHours = 0.25f;

	/// Sun elevations in degrees that separate the periods, with the blend centred on each.
	struct Model
	{
		float dayElevation = 10.0f;     // Sunrise | Day and Day | Sunset: above it the light has lost its warmth
		float horizonElevation = 0.0f;  // Dawn | Sunrise and Sunset | Dusk: the direct light comes and goes here
		float nightElevation = -5.0f;   // Night | Dawn and Dusk | Night: below it the twilight no longer lights the scene
		float blendDegrees = 3.0f;      // full width of each blend
		float ease = 1.0f;              // weight of smootherstep over a linear blend
	};

	/** @brief The blend width in use: the setting, capped so neighbouring blends never overlap. */
	inline float GetAppliedBlendDegrees(const Model& model)
	{
		const float room = std::min(model.dayElevation - model.horizonElevation, model.horizonElevation - model.nightElevation);
		return std::clamp(model.blendDegrees, 0.0f, std::max(room, 0.0f));
	}

	/** @brief 0 below a boundary's blend, 1 above it, eased in between. */
	inline float GetShareAbove(float elevation, float boundary, float blendDegrees, float ease)
	{
		if (blendDegrees <= 0.0f)
			return elevation >= boundary ? 1.0f : 0.0f;
		const float progress = std::clamp((elevation - boundary) / blendDegrees + 0.5f, 0.0f, 1.0f);
		const float smoother = progress * progress * progress * (progress * (progress * 6.0f - 15.0f) + 10.0f);
		return progress + (smoother - progress) * std::clamp(ease, 0.0f, 1.0f);
	}

	/**
	 * @brief Share of the low-sun and twilight weights that belongs to the morning: 1 from the sun's lowest point
	 * to its highest, 0 after, with a short linear handover at both so the weights stay continuous on any sun path.
	 * @param noonHour Hour the sun is highest; it is lowest twelve hours away.
	 */
	inline float GetRisingShare(float hour, float noonHour)
	{
		// Hours past noon, wrapped to [-12, 12): negative is the morning.
		float sinceNoon = std::fmod(hour - noonHour, 24.0f);
		if (sinceNoon < -12.0f)
			sinceNoon += 24.0f;
		else if (sinceNoon >= 12.0f)
			sinceNoon -= 24.0f;

		const float handover = kCulminationBlendHours;
		if (std::abs(sinceNoon) < handover)
			return 0.5f - sinceNoon / (2.0f * handover);
		const float toMidnight = 12.0f - std::abs(sinceNoon);
		if (toMidnight < handover)
			return sinceNoon < 0.0f ? 0.5f + toMidnight / (2.0f * handover) : 0.5f - toMidnight / (2.0f * handover);
		return sinceNoon < 0.0f ? 1.0f : 0.0f;
	}

	/**
	 * @brief Period weights for a sun elevation; they sum to 1.
	 * @param elevationDegrees The sun's geometric elevation, negative below the horizon.
	 * @param risingShare GetRisingShare's value for the hour.
	 */
	inline Weights GetWeights(float elevationDegrees, float risingShare, const Model& model)
	{
		const float blend = GetAppliedBlendDegrees(model);
		// The blends do not overlap, so aboveNight >= aboveHorizon >= aboveDay and every difference is a weight.
		const float aboveNight = GetShareAbove(elevationDegrees, model.nightElevation, blend, model.ease);
		const float aboveHorizon = GetShareAbove(elevationDegrees, model.horizonElevation, blend, model.ease);
		const float aboveDay = GetShareAbove(elevationDegrees, model.dayElevation, blend, model.ease);
		const float twilight = std::max(aboveNight - aboveHorizon, 0.0f);
		const float lowSun = std::max(aboveHorizon - aboveDay, 0.0f);
		const float rising = std::clamp(risingShare, 0.0f, 1.0f);
		return { twilight * rising, lowSun * rising, aboveDay, lowSun * (1.0f - rising), twilight * (1.0f - rising), 1.0f - aboveNight };
	}
}
