#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace SnowDeformationMath
{
	constexpr float Pi = 3.14159265f;

	/** @brief Ground footprint of a sphere or round print of a_radius. */
	inline float CircleFootprint(float a_radius)
	{
		return Pi * a_radius * a_radius;
	}

	/** @brief Ground footprint of a box whose half extents along the ground axes are a_halfX and a_halfY. */
	inline float BoxFootprint(float a_halfX, float a_halfY)
	{
		return 4.0f * std::max(a_halfX, 0.0f) * std::max(a_halfY, 0.0f);
	}

	/** @brief Ground footprint of a capsule of a_radius whose axis covers a_groundLength on the ground. */
	inline float CapsuleFootprint(float a_groundLength, float a_radius)
	{
		return 2.0f * a_radius * (std::max(a_groundLength, 0.0f) + 2.0f * a_radius);
	}

	/** @brief Weight per footprint area for a body of a_weight resting on a_area; 0 when either is unknown. */
	inline float ContactPressure(float a_weight, float a_area)
	{
		return a_weight > 0.0f && a_area > 1e-3f ? a_weight / a_area : 0.0f;
	}

	/**
	 * @brief How much of a full print a body leaves, from 0 for something that settles on top of the snow to 1 for something that sinks fully.
	 * @details Smoothly interpolated on a log scale between a_light and a_heavy pressure. A pressure of 0 means the weight is unknown and leaves a full print.
	 */
	inline float WeightScale(float a_pressure, float a_light, float a_heavy)
	{
		if (!(a_pressure > 0.0f))
			return 1.0f;
		const float span = std::log(std::max(a_heavy, a_light * 1.001f) / std::max(a_light, 1e-9f));
		const float t = std::clamp(std::log(a_pressure / std::max(a_light, 1e-9f)) / span, 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	/** @brief Strength of an object's press, from 1 on the ground to 0 once its bottom is a_reach above it, so a falling item only dents the snow as it touches it. */
	inline float ObjectPressFactor(float a_clearance, float a_reach)
	{
		return std::clamp(1.0f - a_clearance / std::max(a_reach, 1e-3f), 0.0f, 1.0f);
	}

	/** @brief Print depth multiplier for an object landing at a_downSpeed: 1 at rest, rising to a_maxScale at a_refSpeed or faster. */
	inline float ImpactDepthScale(float a_downSpeed, float a_refSpeed, float a_maxScale)
	{
		return 1.0f + (std::max(a_maxScale, 1.0f) - 1.0f) * std::clamp(a_downSpeed / std::max(a_refSpeed, 1e-3f), 0.0f, 1.0f);
	}

	/** @brief Fraction of walking speed lost wading a_buried deep with legs a_legHeight long, reaching a_strength at full burial. */
	inline float WadeSlowdown(float a_buried, float a_legHeight, float a_strength)
	{
		return std::clamp(a_strength, 0.0f, 1.0f) * std::clamp(a_buried / std::max(a_legHeight, 1e-3f), 0.0f, 1.0f);
	}

	/** @brief Whether a race name has a word starting with frost, ice, icy, snow or ghost, so the creature is at home in the cold. */
	inline bool IsColdAdaptedName(std::string_view a_name)
	{
		constexpr std::string_view prefixes[] = { "frost", "ice", "icy", "snow", "ghost" };
		size_t start = 0;
		while (start < a_name.size()) {
			while (start < a_name.size() && !std::isalpha(static_cast<unsigned char>(a_name[start])))
				++start;
			size_t end = start;
			while (end < a_name.size() && std::isalpha(static_cast<unsigned char>(a_name[end])))
				++end;
			const std::string_view word = a_name.substr(start, end - start);
			for (const auto prefix : prefixes) {
				if (word.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), word.begin(), [](char a_prefix, char a_char) { return a_prefix == std::tolower(static_cast<unsigned char>(a_char)); }))
					return true;
			}
			start = end;
		}
		return false;
	}
}
