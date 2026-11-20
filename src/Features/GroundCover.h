#pragma once

#include "Feature.h"

/**
 * @brief Carries the colour of the grass on the plain up the lower slopes of the rock next to it.
 *
 * The inverse of Snow Cover, as a layer in the lighting pixel shader (GroundCover.hlsli): up-facing grey rock near
 * the altitude where grass grows takes the grass colour, in patches that thin out with height and slope; Snow
 * Cover's snow wins over it. Proof of concept.
 *
 * Colour comes from the grass colour map of the worldspace (textures\\GrassColour\\GrassColourMaps\\
 * <Worldspace>.GrassColourMap.<minX>.<minY>.<maxX>.<maxY>.dds). The altitude grass grows at comes from the level map
 * beside it (<Worldspace>.GrassLevelMap.<same cells>.dds) or, without one, from the setting PlainLevel. Without a
 * colour map for the worldspace the layer is off.
 *
 * Which surfaces: placed statics whose model path matches Shaders\\GroundCover\\models.txt (marked per draw with
 * ExtraShaderDescriptors::GroundCover), and by setting all object LOD and all terrain. The shader then only takes
 * what is grey and faces up.
 */
struct GroundCover : Feature
{
	virtual inline std::string GetName() override { return "Ground Cover"; }
	virtual inline std::string GetShortName() override { return "GroundCover"; }
	virtual inline std::string_view GetShaderDefineName() override { return "GROUND_COVER"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLandscapeAndTextures; }
	virtual bool HasShaderDefine(RE::BSShader::Type a_type) override { return a_type == RE::BSShader::Type::Lighting; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { "Runs the colour of the plain's grass up the lower slopes of the mountains beside it, so plain and mountain read as one surface. Proof of concept.",
			{ "Moss and lichen in patches on up-facing rock, thinner with height, none on steep faces",
				"Takes the colour of the grass growing nearby from the grass colour map",
				"Snow Cover's snow covers it; works on rock meshes, object LOD and optionally terrain",
				"Needs a grass colour map for the worldspace" } };
	}

	/** Lengths are in game units. */
	struct Settings
	{
		bool Enabled = false;
		float Strength = 0.75f;
		float BandHeight = 5000.0f;     // height above the plain level over which the cover fades out
		bool UseLevelMap = true;        // plain level from the level map when the worldspace has one
		float PlainLevel = -5000.0f;    // world z of the plain when no level map is used
		float PlainLevelOffset = 0.0f;  // added to the plain level from either source
		float SlopeMin = 0.5f;          // normal z where cover starts
		float SlopeMax = 0.85f;         // normal z of full cover
		float NoisePatchSize = 1600.0f;
		float NoiseDetailSize = 220.0f;
		float NoiseAmount = 0.6f;
		float Saturation = 1.0f;
		float Brightness = 1.0f;
		float LuminanceMatch = 0.5f;  // 0: rock keeps its own brightness, 1: takes the grass's
		float GlossReduction = 0.6f;
		float CreaseDarkening = 0.3f;
		float GreyLimit = 0.35f;       // albedo saturation above which a surface is not rock; 1 = no test
		float RegionCoverage = 0.05f;  // least share of the surrounding cell that must grow grass
		bool AffectObjectLOD = true;
		bool AffectTerrain = false;
		bool Scree = false;
		float ScreeStrength = 0.35f;
		float ScreeHeight = 2500.0f;
		uint DebugView = 0;  // 1: cover weight in magenta
	};
	Settings settings;

	/** Mirrors GroundCoverSettings in SharedData.hlsli. */
	struct alignas(16) PerFrame
	{
		uint Enabled = 0;
		uint Flags = 0;
		float Strength = 0;
		float BandHeight = 1;

		float SlopeMin = 0;
		float SlopeMax = 1;
		float InvPatchSize = 0;
		float InvDetailSize = 0;

		float NoiseAmount = 0;
		float Saturation = 1;
		float Brightness = 1;
		float LuminanceMatch = 0;

		float GlossReduction = 0;
		float CreaseDarkening = 0;
		float GreyLimit = 1;
		float RegionCoverage = 0;

		float PlainLevel = 0;
		float ScreeStrength = 0;
		float ScreeHeight = 0;
		uint DebugView = 0;

		float2 MapScale{};
		float2 MapOffset{};
	};
	static_assert(sizeof(PerFrame) == 96);

	enum Flags : uint
	{
		kTerrain = 1u,
		kObjectLOD = 2u,
		kScree = 4u,
		kLevelMap = 8u,
	};

	PerFrame GetCommonBufferData();

	virtual void SetupResources() override;
	virtual void Prepass() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual inline void PostPostLoad() override { Hooks::Install(); }

private:
	static constexpr uint32_t kColourMapSlot = 58;  // t58, t59: see GroundCover.hlsli

	struct MapFile
	{
		std::filesystem::path path;
		int minX = 0;
		int minY = 0;
		int maxX = 0;
		int maxY = 0;
	};
	std::unordered_map<std::string, MapFile> colourMapFiles;  // by worldspace editor ID
	std::unordered_map<std::string, MapFile> levelMapFiles;

	winrt::com_ptr<ID3D11ShaderResourceView> colourMap;
	winrt::com_ptr<ID3D11ShaderResourceView> levelMap;
	MapFile colourMapCells;
	std::string mapWorldspace = "\x01";  // the worldspace the maps were last looked up for, loaded or not
	std::string mapState = "not looked up yet";
	bool active = false;  // enabled, in an exterior with a colour map: the shader layer and the per-draw test run

	std::vector<std::string> rockModels;  // lower-case parts of model paths, see models.txt
	ankerl::unordered_dense::map<RE::FormID, bool> rockBaseCache;

	/** Lists the maps in Data\\textures\\GrassColour\\GrassColourMaps. */
	void FindMaps();
	void LoadRockModels();
	/** Loads the maps of the current worldspace once per worldspace. Render thread. */
	void EnsureMaps();
	bool IsRock(RE::TESObjectREFR* a_ref);
	void SetupGeometry(RE::BSRenderPass* a_pass);

	struct Hooks
	{
		struct BSLightingShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
			logger::info("[Ground Cover] Installed hooks");
		}
	};
};
