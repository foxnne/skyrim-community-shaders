#pragma once

#include "Buffer.h"
#include "LODShadowSettings.h"
#include "LODShadowUpdateBudget.h"
#include "Utils/GpuTimestamps.h"

/**
 * @brief Caches directional depth from terrain, distant objects and trees.
 *
 * Ported from open-shaders (LOD Shadows, #949 / #1029 / #1087). This fork keeps the depth cache,
 * the budgeted update and the history blend, and leaves out the receiver mask, the effect ray
 * bounds and the VR paths: the map data travels in the shared feature buffer and the depth array
 * takes one pixel shader slot.
 */
struct LODShadows : Feature
{
	std::string GetName() override { return "LOD Shadows"; }
	std::string GetShortName() override { return "LODShadows"; }
	std::string GetDisplayName() override { return T("feature.lod_shadows.name", "LOD Shadows"); }
	std::string_view GetCategory() const override { return FeatureCategories::kShadows; }
	std::string_view GetShaderDefineName() override { return "LOD_SHADOWS"; }
	/** @brief Only the opaque world shaders sample the maps: lighting, grass and tree LOD. */
	bool HasShaderDefine(RE::BSShader::Type a_type) override
	{
		return a_type == RE::BSShader::Type::Lighting || a_type == RE::BSShader::Type::Grass || a_type == RE::BSShader::Type::DistantTree;
	}
	bool IsCore() const override { return true; }
	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;

	using Settings = LODShadowSettings;
	Settings settings;

	static constexpr uint32_t kMapCount = 3;

	/** @brief One cached map as the shaders read it (LODShadowMapData in SharedData.hlsli). */
	struct alignas(16) MapData
	{
		DirectX::XMFLOAT4X4 worldToClip{};
		float4 parameters{};
		float4 filter{};
		DirectX::XMFLOAT4X4 historyWorldToClip{};
		float4 history{};
	};
	static_assert(sizeof(MapData) == 176);

	/** @brief Block of the shared feature buffer (LODShadowsSettings in SharedData.hlsli). */
	struct alignas(16) PerFrame
	{
		std::array<MapData, kMapCount> maps{};
	};
	static_assert(sizeof(PerFrame) == 528);

	/** @brief Returns the map data published by the last update. */
	PerFrame GetCommonBufferData() const { return published; }

	/** @brief Loads and validates runtime settings. */
	void LoadSettings(json& a_json) override;
	/** @brief Serializes runtime settings. */
	void SaveSettings(json& a_json) override;
	/** @brief Restores defaults and invalidates cached depth. */
	void RestoreDefaultSettings() override;
	/** @brief Draws the localized feature controls. */
	void DrawSettings() override;
	bool HasDebugSection() const override { return true; }
	/** @brief Draws cache state, caster counts and GPU timings. */
	void DrawDebugSection() override;
	/** @brief Creates the depth cache and standalone caster pipeline. */
	void SetupResources() override;
	/** @brief Recompiles the standalone caster shaders. */
	void ClearShaderCache() override;
	/** @brief Updates a bounded batch of casters and publishes completed maps. */
	void EarlyPrepass() override;
	/** @brief Binds the completed maps again before the world passes. */
	void Prepass() override;
	/** @brief Binds the completed maps for reflection shading. */
	void ReflectionsPrepass() override;
	/** @brief Invalidates depth when a saved game loads. */
	void GameLoaded() override;
	/** @brief Returns whether all maps are complete and hold the terrain, so heightmap terrain shadows can yield to them. */
	bool CoversTerrain() const;

private:
	static constexpr uint32_t kDepthSlicesPerMap = 3;
	static constexpr uint32_t kDepthSliceCount = kMapCount * kDepthSlicesPerMap;
	static constexpr uint32_t kResolution = 2048;
	/** @brief Pixel shader slot of the depth array (LODShadows.hlsli). */
	static constexpr uint32_t kDepthSlot = 49;
	static constexpr uint32_t kQueryFrames = 4;
	static constexpr std::array<float, kMapCount> kMinimumHalfExtents{ 18432.0f, 55296.0f, 165888.0f };
	static constexpr std::array<double, kMapCount> kHistoryIntervalsMs{ 600.0, 1500.0, 2500.0 };

	struct alignas(16) DrawData
	{
		DirectX::XMFLOAT4X4 worldToClip{};
		float4 uvTransform{ 1, 1, 0, 0 };
		float4 alpha{ 0, 1, 0, 0 };
		DirectX::XMFLOAT4X4 world{};
		float4 landBounds{};
	};
	static_assert(sizeof(DrawData) == 176);
	struct Caster
	{
		RE::NiPointer<RE::BSGeometry> geometry;
		winrt::com_ptr<ID3D11Buffer> vertices;
		winrt::com_ptr<ID3D11Buffer> indices;
		winrt::com_ptr<ID3D11Buffer> instances;
		winrt::com_ptr<ID3D11ShaderResourceView> texture;
		RE::NiTransform transform;
		RE::NiBound bound;
		float4 uvTransform{ 1, 1, 0, 0 };
		float alphaThreshold = 0;
		float materialAlpha = 1;
		uint64_t vertexDescriptor = 0;
		uint32_t stride = 0;
		uint32_t indexCount = 0;
		uint32_t firstIndex = 0;
		uint32_t instanceStride = 0;
		uint32_t instanceCount = 1;
		bool lodLandscape = false;
		bool nativeShadowCaster = false;
	};
	struct Map
	{
		MapData data;
		RE::NiPoint3 center{};
		RE::NiPoint3 direction{};
		float halfExtent = 0;
		float depthRange = 0;
		uint32_t slice = 0;
		uint64_t scene = 0;
		uint32_t frame = 0;
		uint32_t transitionGeneration = 0;
		double completedTimeMs = 0;
		double blendDurationMs = 0;
		double buildDurationMs = 0;
		uint64_t triangleCount = 0;
		bool valid = false;
		bool terrainValid = false;
	};
	struct Query
	{
		Util::TimestampQueryBatch batch;
		uint32_t triangles = 0;
		uint32_t frame = 0;
		bool immediateRefresh = false;
		bool budgetLimited = false;
		bool pending = false;
	};

	void CompileShaders();
	void Invalidate();
	void Publish();
	void BindMaps();
	void CollectTerrain();
	void CollectLoadedObjects(bool a_fullWalk);
	void CollectScene(bool a_refreshLoaded, bool a_fullWalk);
	void HashGeometry(RE::BSGeometry* a_geometry);
	void Collect(RE::NiAVObject* a_object, bool a_ignoreAppCulling = false, bool a_loadedObject = false);
	void AddCaster(RE::BSGeometry* a_geometry);
	void BeginMap(uint32_t a_index, const RE::NiPoint3& a_center, const RE::NiPoint3& a_direction);
	void UpdateMaps(const RE::NiPoint3& a_center, bool a_refreshImmediately);
	/** @brief Rebuilds the caster list of the collected scene; BeginMap filters it per map. */
	void BuildSceneCasters();
	/** @brief Moves the light direction each map is drawn for, in steps, and drops work a jump of the light made useless. */
	void HoldDirections(const RE::NiPoint3& a_direction, bool a_follow);
	/** @brief Angle in radians the light moves before the map takes a new direction; 0 follows it. */
	float DirectionStep(uint32_t a_index) const;
	/** @brief Angle between the light and the direction the map in use was drawn for. */
	float DirectionOffsetDegrees(uint32_t a_index) const;
	void RenderBatch(bool a_refreshImmediately);
	void CaptureDistantDepth();
	void ResolveTimings();
	float GetHistoryBlend(uint32_t a_index) const;
	void PublishCompletedMaps(bool a_refreshImmediately);
	bool NeedsUpdate(uint32_t a_index, const RE::NiPoint3& a_position, const RE::NiPoint3& a_direction) const;
	/** @brief True for a map drawn for a light direction the light has since jumped away from. */
	bool IsStale(const Map& a_map, uint32_t a_index) const;
	bool Intersects(const Caster& a_caster) const;
	ID3D11InputLayout* GetLayout(const Caster& a_caster);

	std::atomic_bool resetRequested{ true };
	bool ready = false;
	bool clearPending = false;
	bool distantDepthCaptured = false;
	bool wasEnabled = false;
	bool refreshEveryFrame = false;
	bool throttleCollect = true;
	uint32_t framesSinceCollect = 0;
	int32_t collectedGridX = 0;
	int32_t collectedGridY = 0;
	bool interpolateHistory = true;
	double frameTimeMs = 0;
	double frameDurationMs = 0;
	uint32_t worldSpaceID = 0;
	uint32_t transitionGeneration = 0;
	uint64_t sceneHash = 0;
	uint32_t terrainGeometryCount = 0;
	uint32_t loadedTerrainGeometryCount = 0;
	bool terrainAvailable = false;
	float4 landBounds{};
	float4 pendingLandBounds{};
	std::vector<RE::NiPointer<RE::BSGeometry>> visibleGeometry;
	std::vector<Caster> casters;
	/** @brief Every caster of the collected scene, kept until the scene hash changes or the cache expires. */
	struct SceneCasters
	{
		std::vector<Caster> casters;
		uint32_t loadedStart = 0;
		uint64_t scene = 0;
		uint32_t frame = 0;
		bool terrainSupported = true;
		bool valid = false;
	};
	SceneCasters sceneCasters;
	uint32_t sceneCasterBuilds = 0;
	RE::NiPoint3 lightDirection{};
	std::array<RE::NiPoint3, kMapCount> heldDirections{};
	std::array<float, kMapCount> mapFades{};
	double directionSpeed = 0;
	double directionWindowAngle = 0;
	double directionWindowMs = 0;
	uint32_t directionChanges = 0;
	std::vector<RE::NiPointer<RE::BSGeometry>> loadedGeometry;
	ankerl::unordered_dense::set<RE::BSGeometry*> collectedGeometry;
	/** @brief Geometry last collected for one static reference, reused until a full walk or its root changes. */
	struct LoadedRefEntry
	{
		RE::NiPointer<RE::NiAVObject> root;
		bool culled = false;
		uint64_t scanId = 0;
		std::vector<RE::NiPointer<RE::BSGeometry>> geometry;
	};
	ankerl::unordered_dense::map<RE::TESObjectREFR*, LoadedRefEntry> loadedRefCache;
	uint64_t loadedScanId = 0;
	uint32_t scansSinceFullWalk = 0;
	std::array<uint64_t, 4> sourceHashes{};
	std::array<uint32_t, 4> geometryCounts{};
	uint32_t lastUpdateFrame = 0;
	uint32_t lastRefreshFrame = 0;
	std::array<Map, kMapCount> maps;
	std::array<Map, kMapCount> previousMaps;
	std::array<Map, kMapCount> completedDepthMaps;
	Map pendingMap;
	int pendingIndex = -1;
	size_t casterIndex = 0;
	uint32_t indexOffset = 0;
	uint32_t instanceOffset = 0;
	uint32_t nextMap = 0;
	LODShadowUpdateBudget updateBudget;
	uint32_t lastDraws = 0;
	uint32_t lastTriangles = 0;
	uint32_t completedMaps = 0;
	uint32_t unsupportedCasters = 0;
	double lastUpdateMs = 0;
	double maxUpdateMs = 0;
	double lastRefreshMs = 0;
	double maxRefreshMs = 0;
	uint64_t timingSamples = 0;
	std::array<Query, kQueryFrames> queries;
	uint32_t queryCursor = 0;
	uint32_t publishedValidMaps = 0;
	PerFrame published{};
	winrt::com_ptr<ID3D11Texture2D> depthTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> depthSRV;
	std::array<winrt::com_ptr<ID3D11DepthStencilView>, kMapCount * kDepthSlicesPerMap> depthViews;
	winrt::com_ptr<ID3D11RasterizerState> rasterizer;
	winrt::com_ptr<ID3D11RasterizerState> terrainRasterizer;
	winrt::com_ptr<ID3D11DepthStencilState> depthState;
	winrt::com_ptr<ID3D11SamplerState> alphaSampler;
	std::unique_ptr<ConstantBuffer> drawBuffer;
	winrt::com_ptr<ID3D11VertexShader> vertexShader;
	winrt::com_ptr<ID3D11VertexShader> treeVertexShader;
	winrt::com_ptr<ID3D11PixelShader> alphaShader;
	winrt::com_ptr<ID3DBlob> vertexBytecode;
	winrt::com_ptr<ID3DBlob> treeBytecode;
	std::array<std::unordered_map<uint64_t, winrt::com_ptr<ID3D11InputLayout>>, 2> layouts;
};
