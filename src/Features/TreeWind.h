#pragma once

#include "Buffer.h"
#include "Feature.h"
#include "TreeWind/TreeShapes.h"

/**
 * @brief Wind sway for DynDOLOD tree LOD.
 *
 * Keeps a wind clock that follows the weather and binds it to the vertex shader (b3) for Lighting and
 * Utility draws. The TREE_WIND define adds the matching vertex offset to Lighting.hlsl and Utility.hlsl,
 * so the colour, depth and shadow passes move together. The object LOD vertex shaders cannot tell tree
 * LOD apart at compile time, so each draw gets either the swaying (on) or the still (off) buffer.
 *
 * Loaded trees (TREE references drawn with their full model) get a third buffer (near): the same wind, with
 * bend data measured from the whole tree, so they lean and gust like the LOD next to them. Skinned trees keep
 * the engine's own animation.
 *
 * The same per-draw test marks the crowns of object LOD trees (ExtraShaderDescriptors::LODTreeCrown), which
 * Lighting.hlsl lights as foliage: wrapped two-sided diffuse, softer screen-space shadow, brighter base colour.
 * Crowns that follow a trunk card also get per-vertex crown data (VS t121, TreeShapes::BendWeights::GetCrownData),
 * which the vertex shader hands to the pixel shader for a rounded normal and a self-shadow gradient per tree.
 */
struct TreeWind : Feature
{
private:
	static constexpr std::string_view MOD_ID = "195280";

public:
	struct Settings
	{
		bool Enabled = true;
		float Amplitude = 80.0f;
		float BaseSpeed = 0.6f;
		float WindSpeedGain = 1.4f;
		float BendExponent = 1.5f;
		float TreeSpread = 2.5f;
		float Flutter = 0.25f;
		float FlutterShare = 0.6f;
		float FlutterSpeed = 7.0f;
		float MinDistance = 10000.0f;
		float NearScale = 0.6f;
		float NearRampEnd = 40000.0f;
		float GustStrengthCalm = 1.0f;
		float GustRarityCalm = 40.0f;
		float GustStrengthStorm = 4.0f;
		float GustRarityStorm = 4.0f;
		float GustWindCalm = 0.15f;
		float GustWindStorm = 0.6f;
		float GustRainy = 1.0f;
		float GustSnow = 0.85f;
		float GustCloudy = 0.35f;
		bool TreeModSupport = true;
		bool LODTreeLighting = true;
		bool LODTreeLightBillboards = true;
		float LODTreeWrap = 0.5f;
		float LODTreeTransmission = 0.5f;
		float LODTreeShadowSoftening = 0.7f;
		float LODTreeNormalUpBias = 0.3f;
		float LODTreeBrightness = 1.7f;
		float LODTreeRoundNormal = 0.6f;
		float LODTreeCrownOcclusion = 0.5f;
		float LODTreeWrapFrontLit = 0.2f;
		float LODTreeVariation = 0.08f;
		bool NearTrees = true;
		float NearTreeScale = 0.6f;  // same as NearScale, so both sides of the full-model/LOD swap match
		float NearTreeMinHeight = 250.0f;
	};

	/** @brief Mirrors TreeWindCB in TreeWind.hlsli. */
	struct alignas(16) WindCB
	{
		float4 Wind;    // phase, previous phase, wind speed 0-1, wind angle (radians)
		float4 Params;  // amplitude, near distance, tree LOD draw, loaded tree draw
		float4 Shape;   // bend exponent, use bend weights, flutter strength, per-tree phase spread
		float4 Detail;  // flutter share, flutter speed, near scale, near ramp end
		float4 Gusts;   // gust strength, gust rarity, loaded tree scale, crown data bound (t121)
	};
	static_assert(sizeof(WindCB) == 80);

	/** @brief Mirrors LODTreeLightingSettings in SharedData.hlsli. Read by the pixel shader for draws flagged LODTreeCrown. */
	struct alignas(16) LODTreeLightingData
	{
		float Wrap;             // diffuse wrap, 0 = plain Lambert
		float Transmission;     // share of the light on the far side that comes through the crown
		float ShadowSoftening;  // 0 = full screen-space shadow, 1 = none
		float NormalUpBias;     // added to the normal's up component before lighting
		float Brightness;       // base colour multiplier
		float RoundNormal;      // blend of the shading normal toward the rounded crown normal
		float CrownOcclusion;   // darkening of the lower and inner crown
		float WrapFrontLit;     // share of Wrap and NormalUpBias kept with the sun behind the viewer
		float Variation;        // per-tree brightness and hue variation
		float pad0[3];
	};
	static_assert(sizeof(LODTreeLightingData) == 48);

	enum class Preset
	{
		Gentle,
		Default,
		Windy
	};

	Settings settings;

	virtual inline std::string GetName() override { return "Tree Wind"; }
	virtual inline std::string GetDisplayName() override { return T("feature.tree_wind.name", "Tree Wind"); }
	virtual inline std::string GetShortName() override { return "TreeWind"; }
	virtual inline std::string_view GetShaderDefineName() override { return "TREE_WIND"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type t) override
	{
		return t == RE::BSShader::Type::Lighting || t == RE::BSShader::Type::Utility || t == RE::BSShader::Type::DistantTree;
	}
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLandscapeAndTextures; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.tree_wind.description", "Lets the distant tree LOD sway in the wind."),
			{ T("feature.tree_wind.key_feature_1", "Sway follows the weather: gentle in clear skies, strong gusts in storms."),
				T("feature.tree_wind.key_feature_2", "Each tree bends from its base and keeps its own timing.") } };
	}

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	/** @brief Stands down when the old Windswept Tree LOD plugin is installed, which sways the same trees. */
	virtual void PostPostLoad() override;
	virtual void SetupResources() override;

	/** @brief Advances the wind clock and uploads the on and off buffers, once per frame. */
	virtual void Reset() override;

	/** @brief Binds the wind buffer, and the bend data for a tree LOD draw, for the current Lighting or Utility draw. Called from State::Draw. */
	void SetShaderResources(ID3D11DeviceContext* a_context);

	void ApplyPreset(Preset a_preset);

	/** @brief The LOD tree lighting values for the shared feature buffer (FeatureBuffer.cpp). */
	LODTreeLightingData GetCommonBufferData() const;

	/**
	 * @brief Picks out the tree LOD and loaded tree draws between SetupGeometry and RestoreGeometry of the Lighting and Utility shaders.
	 * @param a_lighting True for a Lighting draw, which also gets the LODTreeCrown flag when it draws a tree crown.
	 */
	void BeginDraw(RE::BSRenderPass* a_pass, bool a_lighting);
	void EndDraw();

	struct Hooks;

private:
	/** @brief How stormy a weather is, 0 calm to 1 storm: the larger of its type and its own wind speed. */
	float GetStorminess(const RE::TESWeather* a_weather) const;

	std::unique_ptr<ConstantBuffer> windOnCB;
	std::unique_ptr<ConstantBuffer> windOffCB;
	std::unique_ptr<ConstantBuffer> windNearCB;
	// As on and off, for tree LOD crown draws with crown data bound
	std::unique_ptr<ConstantBuffer> windOnCrownCB;
	std::unique_ptr<ConstantBuffer> windOffCrownCB;

	TreeShapes::BendWeights bendWeights;
	TreeShapes::NearTrees nearTrees;
	// Bend data of the tree LOD or loaded tree shape being drawn, nullptr for every other draw
	ID3D11ShaderResourceView* drawBendWeights = nullptr;
	bool drawNearTree = false;  // drawBendWeights belongs to a loaded tree
	// Crown data of the tree LOD crown being drawn (Lighting draws flagged LODTreeCrown only), or nullptr
	ID3D11ShaderResourceView* drawCrownData = nullptr;
	bool swayActive = false;    // set once per frame in Reset: sway on and no map menu open
	bool crownActive = false;   // set once per frame in Reset: crown data is in use and no map menu open
	bool nearActive = false;    // set once per frame in Reset: loaded trees sway too (outdoors only)

	double phase = 0.0;
	double prevPhase = 0.0;

	// Shown on the settings page
	float statusWind = 0.0f;
	float statusStorminess = 0.0f;
	float statusGust = 0.0f;

	// Bend data built with the other tree-mod setting is freed
	bool bendWeightsTreeModSupport = true;
	// Loaded tree data built with another minimum height is freed
	float nearTreesMinHeight = 250.0f;
};
