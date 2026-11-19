#pragma once

#include "Buffer.h"

/**
 * @brief A thin sheet of fog that follows the terrain.
 *
 * Owns no pass of its own: Atmosphere and Fog compiles the layer into its light-scattering shader and calls
 * BindForVolume around the dispatch. The ground comes from the Terrain Shadows height map, so the fog exists only
 * in worldspaces that ship a 16-bit one.
 */
struct GroundFog : Feature
{
	virtual inline std::string GetName() override { return "Ground Fog"; }
	virtual std::string GetDisplayName() override { return T("feature.ground_fog.name", "Ground Fog"); }
	virtual inline std::string GetShortName() override { return "GroundFog"; }
	virtual inline std::string_view GetShaderDefineName() override { return "GROUND_FOG"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kSkyAndWeather; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.ground_fog.description", "Lays a thin sheet of fog on the ground that follows hills and hollows instead of filling the world to one height."),
			{ T("feature.ground_fog.key_feature_1", "Hugs the terrain, gathers in hollows and over water, and drifts with the wind"),
				T("feature.ground_fog.key_feature_2", "Lit and shadowed with the atmosphere: sun, moons, sky, clouds and terrain"),
				T("feature.ground_fog.key_feature_3", "Every amount and shape control can follow weather, time of day and location"),
				T("feature.ground_fog.key_feature_4", "Needs Atmosphere and Fog with Physical Sky, and a 16-bit terrain height map for the worldspace") } };
	}

	/** Lengths are in meters (70 game units), as shown in the menu. */
	struct Settings
	{
		bool Enabled = true;
		float Amount = 1.0f;
		float Density = 0.06f;   // extinction at the ground per meter, at Amount 1
		float Thickness = 1.5f;  // top of the tallest mounds
		float Steepness = 3.0f;
		float NoiseScale = 13.0f;
		float NoiseStrength = 1.0f;
		float BankScale = 115.0f;
		float BankStrength = 1.0f;
		float FadeStart = 86.0f;
		float FadeEnd = 430.0f;
		float WindDriftSpeed = 1.5f;  // meters per second in a full wind
		float WindResponse = 0.7f;
		float BasinStrength = 0.5f;
		float WaterPreference = 1.5f;
		float Albedo = 0.97f;
		float SunScale = 1.0f;
		float SkyAmbientScale = 1.0f;
		float SelfShadow = 1.0f;
		float GroundFill = 0.25f;  // share of Thickness
	};

	/** Mirrors GroundFogCB in GroundFog/GroundFog.hlsli. */
	struct alignas(16) VolumeData
	{
		float4 MapScaleOffset;
		float2 MapZ;
		float2 MapEdgeFade;
		float Thickness;
		float Steepness;
		float GroundExtinction;
		float GroundFill;
		float FadeStart;
		float FadeEnd;
		float2 WindTravel;
		float InvNoiseScale;
		float NoiseStrength;
		float InvBankScale;
		float BankStrength;
		float BasinStrength;
		float BasinRadius;
		float InvBasinDepth;
		float WaterPreference;
		float Albedo;
		float SunScale;
		float SkyAmbientScale;
		float SelfShadow;
		uint32_t Flags;
		uint32_t DebugMode;
		float2 Padding;
	};
	static_assert(sizeof(VolumeData) == 128);
	static_assert(offsetof(VolumeData, MapZ) == 16);
	static_assert(offsetof(VolumeData, Thickness) == 32);
	static_assert(offsetof(VolumeData, FadeStart) == 48);
	static_assert(offsetof(VolumeData, WindTravel) == 56);
	static_assert(offsetof(VolumeData, InvNoiseScale) == 64);
	static_assert(offsetof(VolumeData, BasinStrength) == 80);
	static_assert(offsetof(VolumeData, Albedo) == 96);
	static_assert(offsetof(VolumeData, Flags) == 112);
	static_assert(offsetof(VolumeData, Padding) == 120);

	enum class DebugMode : uint32_t
	{
		kOff,
		kSlab,
		kGroundHeight,
		kLayerMask,
	};

	enum class Status
	{
		kActive,
		kDisabled,
		kNoAmount,
		kInterior,
		kUnderwater,
		kNoHeightMap,
		kHeightMapNot16Bit,
	};

	Settings settings;

	virtual void SetupResources() override;
	virtual void DrawSettings() override;
	virtual bool HasDebugSection() const override { return true; }
	virtual void DrawDebugSection() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	/**
	 * @brief Binds the layer's constants and the terrain height map for the atmosphere volume's compute pass.
	 * @return False when there is no fog to add this frame; nothing is bound then.
	 */
	bool BindForVolume(ID3D11DeviceContext* a_context);
	/** @brief Restores what the compute stage had bound before BindForVolume. */
	void UnbindForVolume(ID3D11DeviceContext* a_context);

	/** Constant buffer and height map slots in ViewAtmosphere/LightScattering.cs.hlsl. */
	static constexpr uint32_t kConstantSlot = 2;
	static constexpr uint32_t kHeightMapSlot = 6;

private:
	Status Update(VolumeData& a_data, ID3D11ShaderResourceView*& a_heightMap);

	eastl::unique_ptr<ConstantBuffer> volumeConstants;
	winrt::com_ptr<ID3D11Buffer> previousConstants;
	winrt::com_ptr<ID3D11ShaderResourceView> previousHeightMap;
	bool bound = false;

	// How far the wind has carried the noise, in game units.
	double windTravelX = 0.0;
	double windTravelY = 0.0;
	uint32_t lastUpdateFrame = UINT32_MAX;
	uint32_t lastBindFrame = UINT32_MAX;

	DebugMode debugMode = DebugMode::kOff;
	Status status = Status::kDisabled;
	float effectiveAmount = 0.0f;
	float windSpeed = 0.0f;
	DXGI_FORMAT heightMapFormat = DXGI_FORMAT_UNKNOWN;
	float2 heightMapSize{};
};
