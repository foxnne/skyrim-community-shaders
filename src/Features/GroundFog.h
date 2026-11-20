#pragma once

#include "Buffer.h"
#include "Features/TerrainShadows.h"

/**
 * @brief Fog that follows the terrain: a soft body of it around the camera, a thin sheet beyond, pools in far valleys.
 *
 * Drawn per screen pixel by its own compute pass (RenderPixel), which Atmosphere and Fog runs right before its
 * post-water composite and lays under the atmosphere there. With the setting VolumeInjection the layer is instead
 * compiled into the shared volume's light-scattering shader (BindForVolume), as in the first version. The ground
 * comes from the Terrain Shadows height map, so the fog exists only in exterior worldspaces that ship a 16-bit one.
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
		float WaterPreference = 1.5f;  // how much more fog over water than over land
		float ShoreSoftness = 0.6f;    // water depth over which the fog over water comes in at a shore, meters
		float WaterPatchiness = 0.7f;  // how uneven the fog over water is
		float Albedo = 0.97f;
		float SunScale = 1.0f;
		float SkyAmbientScale = 1.0f;
		float SelfShadow = 1.0f;
		float SunDiffusion = 0.5f;  // share of the sun and moon light the fog sends out evenly to all sides
		float GroundFill = 0.25f;  // share of Thickness
		// Pixel pass.
		float SurfaceBlendDistance = 3.0f;  // end of the view ray over which the ground becomes the visible surface
		float SurfaceMaxHeight = 1.0f;      // an up-facing surface up to this far above the mapped ground counts as ground
		float SurfaceSoftness = 0.5f;       // fog fades over this last stretch of a ray that ends on something not ground
		float FloorMaxHeight = 4.0f;        // level floors up to this far above the mapped ground carry the near volume
		// The near volume: a taller, softer body of fog around the camera.
		// Up to 1 m thick it is the pool: drawn on what the screen shows, from the simulation. Thicker, it is the
		// tall volume of the first versions, marched over the height map.
		float NearDensity = 0.5f;     // extinction at the ground per meter, at Amount 1; 0 = none
		float NearThickness = 0.45f;  // height at which it has thinned to nothing
		float NearFalloff = 1.5f;    // how quickly it thins with height
		float NearDistance = 40.0f;  // meters from the camera; fades out over the second half
		float DriftScale = 2.5f;     // size of the drifting patches, meters
		float DriftStrength = 0.9f;  // how uneven the density is
		float Breathing = 0.25f;     // share by which the near volume's top rises and falls
		// Burn-off: amount by time of day, and fog kept where terrain shadow holds the sun off.
		float BurnOffStrength = 1.0f;   // 0 = the time of day and the sun change nothing
		float ShadowRetention = 0.45f;  // share of the morning amount that ground in terrain shadow keeps by day
		float MorningAmount = 1.4f;
		float MiddayAmount = 0.1f;
		float EveningAmount = 0.8f;
		// Reactive fog: things moving through the fog around the camera push it aside.
		bool Reactive = true;
		float ReactiveStrength = 1.0f;  // how much of the fog a mover clears; 0 leaves only the displacement
		// The simulation the pool is drawn from.
		float SettleTime = 6.0f;    // seconds over which disturbed fog returns to its resting amount
		float DownhillFlow = 1.0f;  // how strongly the fog runs down slopes and steps into the low places
		float Swirl = 1.0f;         // eddies in still fog and, more, in fog that is moving
		float Pooling = 1.0f;       // 0: the pool rests evenly; 1: in dips and at the foot of slopes, little on level ground
		bool HalfResolution = true;
		float StepGrowth = 0.3f;  // size of terrain detail the march follows, as a share of the distance from the camera
		int MaxSteps = 72;
		// The valley layer: deeper fog in hollows and over water, beyond the sheet.
		float FarAmount = 1.0f;
		float FarDensity = 0.012f;     // extinction at the ground per meter, at FarAmount 1
		float FarThickness = 6.0f;     // meters
		float FarEnd = 6000.0f;        // meters
		float CloudDarkening = 0.75f;  // how much cloud overhead dims the sky light the fog takes
		bool VolumeInjection = false;  // draw through the shared atmosphere volume instead of per pixel
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
		float SurfaceBlend;
		float SurfaceLift;
		float StepMin;
		float StepGrowth;
		uint32_t MaxSteps;
		float WaterTop;
		float2 PyramidSize;
		float PyramidLevels;
		float MapTexel;
		float2 MapSize;
		float FarExtinction;
		float FarThickness;
		float FarEnd;
		float FarBasinRadius;
		float InvFarBasinDepth;
		float CloudDarkening;
		float NearExtinction;
		float NearThickness;
		float NearFalloff;
		float NearEnd;
		float InvDriftScale;
		float DriftStrength;
		float BreathPhase;
		float NearBreath;
		float2 FloorAnchor;
		float FloorOffset;
		float FloorMax;
		float SoftLength;
		float BurnSunlitNear;
		float BurnSunlitFar;
		float BurnShaded;
		float BurnRetention;
		float InvShoreDepth;
		float WaterPatchiness;
		float WaterTexel;
		float4 FlowWindow;
		float PoolExtinction;
		float PoolThickness;
		float PoolEnd;
		float SunWrap;
	};
	static_assert(sizeof(VolumeData) == 304);
	static_assert(offsetof(VolumeData, MapZ) == 16);
	static_assert(offsetof(VolumeData, Thickness) == 32);
	static_assert(offsetof(VolumeData, FadeStart) == 48);
	static_assert(offsetof(VolumeData, WindTravel) == 56);
	static_assert(offsetof(VolumeData, InvNoiseScale) == 64);
	static_assert(offsetof(VolumeData, BasinStrength) == 80);
	static_assert(offsetof(VolumeData, Albedo) == 96);
	static_assert(offsetof(VolumeData, Flags) == 112);
	static_assert(offsetof(VolumeData, SurfaceBlend) == 120);
	static_assert(offsetof(VolumeData, MaxSteps) == 136);
	static_assert(offsetof(VolumeData, PyramidSize) == 144);
	static_assert(offsetof(VolumeData, MapTexel) == 156);
	static_assert(offsetof(VolumeData, MapSize) == 160);
	static_assert(offsetof(VolumeData, FarEnd) == 176);
	static_assert(offsetof(VolumeData, CloudDarkening) == 188);
	static_assert(offsetof(VolumeData, NearExtinction) == 192);
	static_assert(offsetof(VolumeData, InvDriftScale) == 208);
	static_assert(offsetof(VolumeData, FloorAnchor) == 224);
	static_assert(offsetof(VolumeData, SoftLength) == 240);
	static_assert(offsetof(VolumeData, BurnRetention) == 256);
	static_assert(offsetof(VolumeData, InvShoreDepth) == 260);
	static_assert(offsetof(VolumeData, FlowWindow) == 272);
	static_assert(offsetof(VolumeData, PoolExtinction) == 288);

	/** One thing that moves the fog this frame. Mirrors Emitter in GroundFog/FlowSim.cs.hlsl. */
	struct FlowEmitter
	{
		float2 Position;  // from the flow window's corner, game units
		float2 Velocity;  // game units per second
		float Radius;
		float Burst;  // outward speed added once
		float Clear;  // share cleared at its centre this step
		float Pad;
	};
	static constexpr uint32_t kMaxFlowEmitters = 24;

	/** Mirrors SimCB in GroundFog/FlowSim.cs.hlsl. */
	struct alignas(16) FlowData
	{
		int32_t Shift[2];
		float TexelSize;
		float DeltaTime;
		uint32_t EmitterCount;
		float VelocityKeep;
		float RefillRate;
		float ClearStrength;
		float2 Wind;
		float Downhill;
		float Spread;
		float2 Corner;
		float Swirl;
		float SwirlTime;
		float RestUniform;
		uint32_t SimFlags;
		float FloorRise;
		float Pooling;
		FlowEmitter Emitters[kMaxFlowEmitters];
	};
	static_assert(sizeof(FlowEmitter) == 32);
	static_assert(sizeof(FlowData) == 80 + 32 * kMaxFlowEmitters);
	static_assert(offsetof(FlowData, Wind) == 32);
	static_assert(offsetof(FlowData, Corner) == 48);
	static_assert(offsetof(FlowData, RestUniform) == 64);
	static_assert(offsetof(FlowData, Emitters) == 80);

	enum class DebugMode : uint32_t
	{
		kOff,
		kSlab,
		kGroundHeight,
		kLayerMask,
		kTapCount,
		kSimulation,
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
		kNoVolume,
		kNoResources,
	};

	Settings settings;

	virtual void SetupResources() override;
	virtual void DrawSettings() override;
	virtual bool HasDebugSection() const override { return true; }
	virtual void DrawDebugSection() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void ClearShaderCache() override;

	/**
	 * @brief Draws the layer for the current scene depth into the feature's own targets and binds them for
	 * ViewAtmosphere/Composite.cs.hlsl. Call inside the post-water composite, with its depth (t40), linear sampler
	 * (s0), shared constant buffers and the shared volume's parameters already bound to the compute stage.
	 * @return False when there is nothing to draw this frame: disabled, interior, Amount 0, no height map. The
	 * composite slots are cleared then, and no GPU work is done.
	 */
	bool RenderPixel(ID3D11DeviceContext* a_context);

	/**
	 * @brief Binds the layer's constants and the terrain height map for the atmosphere volume's compute pass.
	 * @return False when the volume has no fog to add this frame, which is always unless VolumeInjection is set;
	 * nothing is bound then.
	 */
	bool BindForVolume(ID3D11DeviceContext* a_context);
	/** @brief Restores what the compute stage had bound before BindForVolume. */
	void UnbindForVolume(ID3D11DeviceContext* a_context);

	/** Constant buffer and height map slots in ViewAtmosphere/LightScattering.cs.hlsl. */
	static constexpr uint32_t kConstantSlot = 2;
	static constexpr uint32_t kHeightMapSlot = 6;
	/** Further slots of GroundFog/GroundFogPixel.cs.hlsl and of the fog in ViewAtmosphere/Composite.cs.hlsl. */
	static constexpr uint32_t kHeightPyramidSlot = 7;
	static constexpr uint32_t kFlowMapSlot = 8;
	static constexpr uint32_t kWaterMapSlot = 9;
	static constexpr uint32_t kFloorMapSlot = 10;
	/** The floor's slope, written by the simulation; and the simulation's own input in GroundFog/FlowSim.cs.hlsl: what the pixel pass saw of the floor last frame. */
	static constexpr uint32_t kFloorSlopeSlot = 11;
	static constexpr uint32_t kFloorSeenSlot = 12;
	static constexpr uint32_t kFlowConstantSlot = 3;
	static constexpr uint32_t kCompositeFogSlot = 42;
	static constexpr uint32_t kCompositeDepthSlot = 43;

private:
	Status Update(VolumeData& a_data, ID3D11ShaderResourceView*& a_heightMap, ID3D11ShaderResourceView*& a_waterMap);
	/** Lists the baked water maps in Data\\textures\\GroundFog\\WaterMaps (<Worldspace>.WaterMap.<minX>.<minY>.<maxX>.<maxY>.dds). */
	void FindWaterMaps();
	/**
	 * @brief The water map of the worldspace the height map belongs to, loaded on first use.
	 * @return Null when there is none or it does not fit the height map; the engine's per-cell water is used then.
	 */
	ID3D11ShaderResourceView* EnsureWaterMap(const TerrainShadows::HeightMapBinding& a_map);
	/** How far the floor under the player lies above the fog's ground there, smoothed over time. */
	void UpdateFloorOffset(float a_deltaTime);
	/** Amount by time of day for sunlit ground, from the Morning / Midday / Evening settings. */
	float TimeOfDayAmount(float& a_daylight) const;
	/**
	 * @brief Places the simulation's window for this frame and fills in its constants. Once per frame.
	 * @param a_pool Whether the near layer is drawn as the pool this frame (which needs the simulation).
	 * @return The window mapping for GroundFogCB::FlowWindow, or zero when there is no simulation this frame.
	 */
	float4 PrepareSim(bool a_pool);
	/**
	 * @brief Steps the simulation (GroundFog/FlowSim.cs.hlsl) with what PrepareSim set up. Call with the fog's
	 * constants, ground textures and the lighting inputs bound to the compute stage. No-op unless PrepareSim
	 * returned a window this frame.
	 * @param a_terrain Whether the height map, water and terrain shadow are bound: the resting amount comes from
	 * them. Without them it is uniform (the hook for interiors).
	 */
	void StepSim(ID3D11DeviceContext* a_context, bool a_terrain);
	/** Fills a_emitters with what moves near the camera, most important first. */
	uint32_t GatherSimEmitters(FlowEmitter* a_emitters, double a_originX, double a_originY, float a_deltaTime);
	bool EnsureShaders();
	/** Builds the coarse copies of the height map when it changes. */
	bool EnsureHeightPyramid(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_heightMap, ID3D11ShaderResourceView* a_waterMap);
	bool EnsureTargets(uint32_t a_width, uint32_t a_height);

	winrt::com_ptr<ID3D11ComputeShader> pixelCS;
	winrt::com_ptr<ID3D11ComputeShader> pyramidCS;
	winrt::com_ptr<ID3D11ComputeShader> pyramidFirstCS;
	winrt::com_ptr<ID3D11ComputeShader> flowCS;
	bool shadersRequested = false;
	// The simulation: two states, swapped every frame. simState: velocity, amount, resting amount. simFloor: the
	// floor level per texel. floorSeen: what the pixel pass saw of the floor this frame, folded in on the next step.
	eastl::unique_ptr<ConstantBuffer> flowConstants;
	eastl::unique_ptr<Texture2D> simState[2];
	eastl::unique_ptr<Texture2D> simFloor[2];
	eastl::unique_ptr<Texture2D> floorSlope;  // slope of simFloor over about a meter, with a validity channel
	eastl::unique_ptr<Texture2D> floorSeen;
	uint32_t flowCurrent = 0;
	bool flowValid = false;
	bool simPending = false;   // PrepareSim set up a step that StepSim has not run yet
	std::unique_ptr<FlowData> simData;  // the step PrepareSim set up
	double simTexel = 0.0;     // game units per texel; changes with Near Distance, which starts the simulation afresh
	int64_t flowOriginX = 0;  // the window's corner, in texels of the world grid
	int64_t flowOriginY = 0;
	int64_t simNextOriginX = 0;
	int64_t simNextOriginY = 0;
	uint32_t flowFrame = UINT32_MAX;
	float4 flowWindow{};
	uint32_t flowEmitterCount = 0;
	double swirlTime = 0.0;
	bool poolActive = false;  // the near layer is drawn as the pool this frame
	struct FlowProjectile
	{
		RE::NiPoint3 position;
		bool magic = false;
		bool moving = false;
	};
	std::unordered_map<uint32_t, RE::NiPoint3> flowActors;  // by handle: where each was last frame
	std::unordered_map<uint32_t, FlowProjectile> flowProjectiles;
	float frameDeltaTime = 0.0f;
	eastl::unique_ptr<Texture2D> fogTexture;     // rgb: light per unit of opacity, a: transmittance to the far depth
	eastl::unique_ptr<Texture2D> fogAuxTexture;  // near and far view depth of each fog pixel, transmittance to the near depth
	eastl::unique_ptr<Texture2D> heightPyramid;
	winrt::com_ptr<ID3D11ShaderResourceView> heightPyramidSource;
	winrt::com_ptr<ID3D11ShaderResourceView> heightPyramidWater;  // the water map whose surfaces are in the pyramid's bound
	struct WaterMapFile
	{
		std::filesystem::path path;
		int minX = 0;
		int minY = 0;
		int maxX = 0;
		int maxY = 0;
	};
	std::unordered_map<std::string, WaterMapFile> waterMapFiles;  // by worldspace editor ID
	eastl::unique_ptr<Texture2D> waterMap;
	std::string waterMapWorldspace;  // the worldspace waterMap was last looked up for, loaded or not
	float waterMapTexel = 0.0f;      // game units per texel; 0 = none
	std::string waterMapState;       // for the debug section
	uint32_t heightPyramidLevels = 0;

	eastl::unique_ptr<ConstantBuffer> volumeConstants;
	winrt::com_ptr<ID3D11Buffer> previousConstants;
	winrt::com_ptr<ID3D11ShaderResourceView> previousHeightMap;
	winrt::com_ptr<ID3D11ShaderResourceView> previousWaterMap;
	bool bound = false;

	// How far the wind has carried the noise, in game units.
	double windTravelX = 0.0;
	double windTravelY = 0.0;
	double breathTime = 0.0;
	float floorOffset = 0.0f;  // game units
	float2 floorAnchor{};      // camera-relative xy of the player
	float timeAmount = 1.0f;
	float daylight = 0.0f;
	uint32_t lastUpdateFrame = UINT32_MAX;
	uint32_t lastBindFrame = UINT32_MAX;

	DebugMode debugMode = DebugMode::kOff;
	Status status = Status::kDisabled;
	float effectiveAmount = 0.0f;
	float effectiveFarAmount = 0.0f;
	float windSpeed = 0.0f;
	DXGI_FORMAT heightMapFormat = DXGI_FORMAT_UNKNOWN;
	float2 heightMapSize{};
	float2 windVelocity{};  // the fog's drift with the wind, game units per second
};
