#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace SnowDeformationMath
{
	/** @brief Strength of an object's press, from 1 on the ground to 0 once its bottom is a_reach above it, so a falling item only dents the snow as it touches it. */
	inline float ObjectPressFactor(float a_clearance, float a_reach)
	{
		return std::clamp(1.0f - a_clearance / std::max(a_reach, 1e-3f), 0.0f, 1.0f);
	}
}
