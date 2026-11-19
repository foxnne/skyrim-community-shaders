#pragma once

#include "Buffer.h"

/**
 * @brief Beams of sunlight and moonlight in the air ("god rays").
 *
 * Its own passes, run right after Atmosphere and Fog's post-water composite (Deferred's post-water hook): opaque
 * surfaces, water, fog, air and clouds are in the scene, transparent surfaces and upscaling are not. Each pixel's
 * view ray is marched through the world at half or quarter resolution against the directional shadow cascades,
 * the terrain shadow and the cloud shadow volume, and only air that gets more light than the air around it adds
 * light: open ground and overcast add nothing. See LightShafts/LightShafts.hlsli.
 *
 * Needs Physical Sky and the shared atmosphere volume of Atmosphere and Fog (for the light's colour after the
 * atmosphere, the cloud shadow volume and the shadow inputs the volume already gathers).
 */
struct LightShafts : Feature
{
	virtual inline std::string GetName() override { return "Light Shafts"; }
	virtual std::string GetDisplayName() override { return T("feature.light_shafts.name", "Light Shafts"); }
	virtual inline std::string GetShortName() override { return "LightShafts"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kSkyAndWeather; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.light_shafts.description", "Beams of sunlight and moonlight between trees, past buildings and ridges and through gaps in the clouds, without brightening open air."),
			{ T("feature.light_shafts.key_feature_1", "Shaped by the real shadows: trees, buildings, terrain and the volumetric clouds"),
				T("feature.light_shafts.key_feature_2", "Only contrast adds light: nothing under an open sky or a closed one"),
				T("feature.light_shafts.key_feature_3", "Coloured by the sun or moon as the atmosphere leaves it: gold at a low sun"),
				T("feature.light_shafts.key_feature_4", "Strength, density and shape can follow weather and time of day"),
				T("feature.light_shafts.key_feature_5", "Needs Physical Sky and the shared volume of Atmosphere and Fog") } };
	}

	/** Lengths are in meters, as shown in the menu. */
	struct Settings
	{
		bool Enabled = false;  // off until it has been looked at in game
		float SunStrength = 0.1f;        // brightness of a full beam straight toward the sun, as a share of the sun's colour
		float MoonStrength = 0.1f;       // the same for the moon that lights the scene
		float Density = 1.0f;            // optical depth of a fully contrasting ray: the control for weather and time of day
		float Length = 50.0f;            // distance over which the near beams fade to 1/e
		float Tightness = 0.6f;          // how much the beams gather toward the light
		float CloudOcclusion = 1.0f;     // 0: clouds ignored. 1: clouds shade the beams and their gaps make them
		float MaxBrightness = 1.0f;      // ceiling of the added luminance, scene units (the sun is 10)
		int Quality = 1;                 // 0 low (quarter resolution), 1 medium, 2 high, 3 ultra
		int DebugView = 0;               // 0 off, 1 lit share of each ray, 2 beams only
		float DistantStrength = 1.0f;    // weight of the rays beyond the near range: cloud gaps and ridges
		float DistantLength = 1500.0f;   // distance over which those fade to 1/e
		float HighSunStrength = 0.5f;    // strength left when the light is high in the sky
		float VeilRemoval = 1.0f;        // 1: contrast only. 0: all lit air glows, like plain volumetric light
	};

	/** Mirrors LightShaftsCB in LightShafts/Constants.hlsli. */
	struct alignas(16) ShaftData
	{
		float3 LightDir;
		float DiskRadius;
		float3 LightColor;
		float Density;
		float NearLength;
		float NearRange;
		float FarLength;
		float FarRange;
		float PhaseG;
		float CloudOcclusion;
		float MaxBrightness;
		float VeilRemoval;
		float DistantStrength;
		float ShadowBias;
		float AmbientHistory;
		float AmbientRadius;
		uint32_t NearSteps;
		uint32_t FarSteps;
		uint32_t Divisor;
		uint32_t DebugMode;
		float RidgeRadius;
		float AmbientHeight;
		float2 Padding;
	};
	static_assert(sizeof(ShaftData) == 112);
	static_assert(offsetof(ShaftData, LightColor) == 16);
	static_assert(offsetof(ShaftData, NearLength) == 32);
	static_assert(offsetof(ShaftData, PhaseG) == 48);
	static_assert(offsetof(ShaftData, DistantStrength) == 64);
	static_assert(offsetof(ShaftData, NearSteps) == 80);
	static_assert(offsetof(ShaftData, RidgeRadius) == 96);

	enum class Status
	{
		kActive,
		kDisabled,
		kNoStrength,
		kInterior,
		kUnderwater,
		kNoSky,
		kNoVolume,
		kNoLight,
		kLightTooLow,
		kLightOutOfView,
		kNoResources,
	};

	struct Quality
	{
		uint32_t divisor;
		uint32_t nearSteps;
		uint32_t farSteps;
	};
	static constexpr Quality kQualities[] = { { 4u, 12u, 6u }, { 2u, 16u, 8u }, { 2u, 24u, 12u }, { 2u, 40u, 16u } };

	Settings settings;

	virtual void DrawSettings() override;
	virtual bool HasDebugSection() const override { return true; }
	virtual void DrawDebugSection() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void ClearShaderCache() override;

	/**
	 * @brief Draws the beams into the main target. Call once the atmosphere's post-water composite has run
	 * (Deferred's post-water hook). Does nothing, and no GPU work, when disabled, in interiors, underwater, without
	 * strength or density, when the light is below the horizon or far outside the view, or without Physical Sky and
	 * the shared atmosphere volume.
	 */
	void Render();

	/** Shader slots. The shadow inputs keep the slots of ViewAtmosphere/MediumLighting.hlsli. */
	static constexpr uint32_t kConstantSlot = 2;
	static constexpr uint32_t kDepthSlot = 40;
	static constexpr uint32_t kPreparedSlot = 44;
	static constexpr uint32_t kShaftsSlot = 45;
	static constexpr uint32_t kShaftsDepthSlot = 46;

private:
	Status Update(ShaftData& a_data);
	bool EnsureShaders();
	bool EnsureResources(uint32_t a_width, uint32_t a_height);

	winrt::com_ptr<ID3D11ComputeShader> prepareCS;
	winrt::com_ptr<ID3D11ComputeShader> marchCS;
	winrt::com_ptr<ID3D11ComputeShader> resolveCS;
	winrt::com_ptr<ID3D11ComputeShader> compositeCS;
	bool shadersRequested = false;

	eastl::unique_ptr<Texture2D> shaftsTexture;    // rgb: light to add, a: lit share of the ray
	eastl::unique_ptr<Texture2D> resolvedTexture;  // the same with the step offsets averaged out
	eastl::unique_ptr<Texture2D> depthTexture;     // device depth each of their pixels was computed for
	eastl::unique_ptr<Texture2D> preparedTextures[2];  // this frame's and last frame's shared values
	uint32_t preparedIndex = 0;
	eastl::unique_ptr<ConstantBuffer> constants;
	winrt::com_ptr<ID3D11SamplerState> linearSampler;
	winrt::com_ptr<ID3D11SamplerState> shadowSampler;

	uint32_t lastCallFrame = UINT32_MAX;
	uint32_t lastDrawFrame = UINT32_MAX;

	Status status = Status::kDisabled;
	float lightElevation = 0.0f;   // degrees
	float lightViewAngle = 0.0f;   // degrees from the view centre
	float effectiveStrength = 0.0f;
};
