#pragma once

#include "Buffer.h"
#include "NRD.h"
#include "NRDReblurIntegration.h"

#include <NRDSettings.h>

/**
 * ScreenSpaceGI: indirect lighting and ambient occlusion via XeGTAO-style
 * horizon scanning, optionally directional via spherical harmonics, denoised
 * by NVIDIA REBLUR.
 *
 * Diffuse-only. Specular reflections live in the standalone
 * ScreenSpaceReflections feature; denoising guides come from the core NRD
 * service.
 */
struct ScreenSpaceGI : Feature
{
private:
	static constexpr std::string_view MOD_ID = "130375";

public:
	virtual inline std::string GetName() override { return "Screen Space GI"; }
	virtual std::string GetDisplayName() override { return T("feature.screen_space_gi.name", "Screen Space GI"); }
	virtual inline std::string GetShortName() override { return "ScreenSpaceGI"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }

	/** @brief Returns a localized description and list of key features for the UI summary panel. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		std::string desc = T("feature.screen_space_gi.description", "Screen Space Global Illumination adds realistic indirect lighting and ambient occlusion to the game. This technique simulates how light bounces off surfaces to illuminate other objects naturally.");
		return std::make_pair(
			desc,
			std::vector<std::string>{
				T("feature.screen_space_gi.key_feature_1", "Realistic indirect lighting"),
				T("feature.screen_space_gi.key_feature_2", "Enhanced ambient occlusion"),
				T("feature.screen_space_gi.key_feature_3", "Improved visual depth and atmosphere"),
				T("feature.screen_space_gi.key_feature_4", "NVIDIA REBLUR temporal denoising"),
				T("feature.screen_space_gi.key_feature_5", "Configurable quality and performance settings") });
	}

	/** @brief Resets all settings to their default values and flags shaders for recompilation. */
	virtual void RestoreDefaultSettings() override;
	/** @brief Draws the ImGui settings UI with quality presets, visual parameters, and denoising options. */
	virtual void DrawSettings() override;
	virtual bool HasDebugSection() const override { return true; }
	virtual void DrawDebugSection() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	/** @brief Registers the loading screen listener that resets the temporal history. */
	virtual void PostPostLoad() override;
	/** @brief Creates GPU textures, samplers, constant buffers, and compiles compute shaders. */
	virtual void SetupResources() override;
	/** @brief Releases and recompiles all SSGI compute shaders. */
	virtual void ClearShaderCache() override;
	/** @brief Compiles all SSGI compute shaders with current resolution and feature defines. */
	void CompileComputeShaders();
	/** @brief Checks whether all required compute shaders and the noise texture loaded successfully. */
	bool ShadersOK();
	bool CanDenoise() const;

	/** @brief Executes the full SSGI pipeline: depth prefilter, radiance fetch, GI, and denoising. */
	void DrawSSGI();
	/** @brief Composites this frame's SSGI indirect diffuse lighting into the main render target before downstream screen-space passes. */
	void Composite();
	/** @brief Updates the SSGI constant buffer with current camera, resolution, and settings data. */
	void UpdateSB();
	void SetupNRDResources();
	void GetNRDResourceSize(uint32_t& a_width, uint32_t& a_height) const;

	//////////////////////////////////////////////////////////////////////////////////

	bool recompileFlag = false;
	bool tracedHalfRes = false;
	bool tracedQuarterRes = false;

	// Loading screen radiance decays at only 1/MaxAccumFrames per frame, bleeding the old scene
	// into the new one for some frames.
	std::atomic<bool> queuedResetHistory{ true };

	class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;

		static bool Register();
	};

	struct Settings
	{
		bool Enabled = true;
		bool EnableGI = true;
		bool EnableVanillaSSAO = false;
		bool EnableSH = true;
		// performance/quality
		uint NumSteps = 8u;
		bool HalfRes = true;
		bool QuarterRes = false;
		// visual
		float Thickness = 0.1f;
		// mix
		float AOPower = 1.0f;
		float GIStrength = 1.0f;
		float ILSaturation = 1.0f;
		bool EnableMultiBounce = false;
		bool UseDynamicCubemapsAsFallback = true;
		float DiffuseCubemapMult = 1.0f;
		float FallbackSkyBoost = 1.0f;  // share of the up-facing sky boost in the ambient fallback
		// foxnne sky-lit shadows: outdoor fill from the Physical Sky's own radiance (see diffuseGI.cs.hlsl)
		bool PhysicalSkyFill = false;     // A/B switch; off = the IBL sky term as before
		float SkyFillScale = 1.0f;        // sky term brightness where Skylighting sees open sky (replaces Sky IBL Scale there)
		float SkyFillSaturation = 1.0f;   // sky term saturation (replaces Sky IBL Saturation); 1 = the sky's own blue
		float SkyFillEnvWeight = 0.25f;   // DALC/env share on skyward rays under open sky; occluded sky keeps all of it
		float BounceSkyLimit = 0.0f;      // traced bounce cap, in multiples of the ambient it hides; 0 = off
		uint SkyFillDebug = 0u;           // 0 off; 1 gates, 2 openness, 3 traced only, 4 fallback only (SSGI radiance)
		bool SkylightTint = false;         // needs PhysicalSkyFill; see diffuseGI.cs.hlsl
		float CoplanarBounceReject = 1.0f; // ground-on-ground bounce handed back to the sky on open ground
		float SkyMatchesSun = 1.0f;        // share of Linear Lighting's directional multiplier also on open sky
		uint SkyFillSource = 0u;           // open-sky radiance: 0 sky reflection cubemap, 1 Physical Sky sky-view LUT
		// NRD REBLUR
		bool EnableREBLUR = true;
		NRD::REBLURSettings Reblur;
		float DiffusePrepassBlurRadius = 30.0f;
		float E11AOScale = 0.5f;
		float E11ILScale = 0.2f;
		float E11ILSaturation = 0.8f;
	} settings;

	/** @brief The settings as the user and the scene layers left them, before an Effects 11 preset overlays its own. */
	Settings userSettings;

	/** @brief The values an active Effects 11 preset drives: [EFFECT] EnableAmbientOcclusion and [SSAO_SSIL]. */
	struct E11Overlay
	{
		bool active = false;
		bool enabled = false;
		bool indirectLighting = false;
		float aoPower = 1.0f;
		float giStrength = 1.0f;
		bool operator==(const E11Overlay&) const = default;
	};
	E11Overlay appliedE11Overlay;

	/** @brief Whether an active Effects 11 preset drives SSGI, i.e. it is not handed back to Bottled Shaders. */
	bool IsManagedByE11() const;
	/** @brief The overlay the active preset wants this frame, inactive when Effects 11 does not own SSGI. */
	E11Overlay GetE11Overlay() const;
	/** @brief Re-applies the settings when the preset's overlay changed since the last apply. Render thread. */
	void RefreshE11Overlay();

	struct alignas(16) SSGICB
	{
		float2 NDCToViewMul;
		float2 NDCToViewAdd;

		float2 TexDim;
		float2 RcpTexDim;
		float2 FrameDim;
		float2 RcpFrameDim;
		uint FrameIndex;

		uint NumSteps;

		float Thickness;
		float AOPower;

		float GIStrength;
		float DiffuseCubemapMult;
		uint UseDynamicCubemap;
		uint MultiBounceMode;
	};
	STATIC_ASSERT_ALIGNAS_16(SSGICB);
	eastl::unique_ptr<ConstantBuffer> ssgiCB;

	struct alignas(16) SharedData
	{
		uint Enabled;
		uint EnableIL;
		uint ReplaceAmbient;
		float AOPower;
		float ILSaturation;
		float FallbackSkyBoost;
		uint PhysicalSkyFill;
		float SkyFillScale;
		float SkyFillSaturation;
		float SkyFillEnvWeight;
		float BounceSkyLimit;
		uint SkyFillDebug;
		uint SkylightTint;
		float CoplanarBounceReject;
		float SkyMatchesSun;
		uint SkyFillSource;
	};
	static_assert(sizeof(SharedData) == 64);

	/** @brief Logs the conditions the Physical Sky Fill path depends on whenever they change. */
	void LogSkyFillGates(const SharedData& a_data);
	uint32_t lastSkyFillGates = 0xFFFFFFFFu;

	SharedData GetCommonBufferData();

	/** @brief Whether missed IL rays fall back to dynamic cubemaps; off while Effects 11 owns IBL or the multiplier is zero. */
	bool UsesCubemapFallback() const;
	bool lastUsedCubemapFallback = false;

	eastl::unique_ptr<Texture2D> texNoise = nullptr;
	eastl::unique_ptr<Texture2D> texWorkingDepth = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> uavWorkingDepth[5] = { nullptr };
	// Full-resolution geometry history is ping-ponged so radiance prefiltering can
	// read the previous frame while the normal pass writes the current frame.
	eastl::unique_ptr<Texture2D> texHistoryGeo[2] = { nullptr, nullptr };
	uint historyGeoWriteIndex = 0;
	bool hasMultiBounceHistory = false;
	bool hasFullResolutionMultiBounceHistory = false;
	bool multiBounceHistoryUsesNRDOutput = false;
	uint32_t lastMultiBounceHistoryFrame = 0;
	eastl::unique_ptr<Texture2D> texRadiance = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> uavRadiance[5] = { nullptr };
	eastl::unique_ptr<Texture2D> texNormal = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> uavNormal[5] = { nullptr };

	// REBLUR diffuse radiance in/out (RGBA16F, full-res)
	eastl::unique_ptr<Texture2D> texNRDInput = nullptr;
	eastl::unique_ptr<Texture2D> texNRDOutput = nullptr;
	eastl::unique_ptr<Texture2D> texNRDInputSH1 = nullptr;
	eastl::unique_ptr<Texture2D> texNRDOutputSH1 = nullptr;

	ID3D11ShaderResourceView* GetDiffuseOutputTexture();
	ID3D11ShaderResourceView* GetDiffuseSH1Texture();

	winrt::com_ptr<ID3D11SamplerState> linearClampSampler = nullptr;
	winrt::com_ptr<ID3D11SamplerState> pointClampSampler = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> prefilterDepthsCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> prefilterRadianceCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> prefilterNormalCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> giCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> compositeCompute = nullptr;

	NRDReblurIntegration nrdReblur;
	nrd::ReblurSettings reblurSettings{};
	bool resetReblurHistory = true;
	bool nrdReblurUsesSH = false;
	bool reblurInitAttempted = false;
	// Prevent downstream passes from sampling stale GI when this frame could not
	// produce a complete SSGI output (disabled feature, shader failure, etc.).
	bool outputReady = false;
};
