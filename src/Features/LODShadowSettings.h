#pragma once

#include <nlohmann/json.hpp>

struct LODShadowSettings
{
	bool Enabled = true;
	float UpdateBudgetMs = 0.1f;
	float DepthBias = 8.0f;
	/** @brief Extra depth bias in texels of each map. */
	float TexelBias = 1.0f;
	/** @brief Distance toward the light at which tree crowns in object LOD look up the maps, in game units. */
	float CrownBias = 256.0f;
	/** @brief Angle the light moves before the far map takes a new direction; 0 follows every movement. */
	float DirectionStepDegrees = 0.5f;
	/** @brief Largest half extent of the far map in game units; 0 for no limit. */
	float FarMapLimit = 0.0f;
};

inline void to_json(nlohmann::json& a_json, const LODShadowSettings& a_settings)
{
	a_json = {
		{ "EnableLODShadows", a_settings.Enabled },
		{ "UpdateBudgetMs", a_settings.UpdateBudgetMs },
		{ "DepthBias", a_settings.DepthBias },
		{ "TexelBias", a_settings.TexelBias },
		{ "CrownBias", a_settings.CrownBias },
		{ "DirectionStepDegrees", a_settings.DirectionStepDegrees },
		{ "FarMapLimit", a_settings.FarMapLimit }
	};
}

inline void from_json(const nlohmann::json& a_json, LODShadowSettings& a_settings)
{
	constexpr LODShadowSettings defaults;
	a_settings.Enabled = a_json.value("EnableLODShadows", defaults.Enabled);
	a_settings.UpdateBudgetMs = a_json.value("UpdateBudgetMs", defaults.UpdateBudgetMs);
	a_settings.DepthBias = a_json.value("DepthBias", defaults.DepthBias);
	a_settings.TexelBias = a_json.value("TexelBias", defaults.TexelBias);
	a_settings.CrownBias = a_json.value("CrownBias", defaults.CrownBias);
	a_settings.DirectionStepDegrees = a_json.value("DirectionStepDegrees", defaults.DirectionStepDegrees);
	a_settings.FarMapLimit = a_json.value("FarMapLimit", defaults.FarMapLimit);
}
