#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

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
}
