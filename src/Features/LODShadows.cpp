#include "LODShadows.h"

#include <bit>

#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "Utils/D3D.h"
#include "I18n/I18n.h"
#include "Utils/DebugSection.h"
#include "Utils/Game.h"
#include "Utils/PerfUtils.h"
#include "Utils/UI.h"

namespace
{
	constexpr uint32_t kIndicesPerTriangle = 3;
	constexpr float kMinimumUpdateBudgetMs = 0.01f;
	constexpr float kMaximumUpdateBudgetMs = 0.4f;
	constexpr uint32_t kMaximumCacheFrames = 600;
	constexpr float kCenterMargin = 0.125f;
	constexpr float kDirectionThreshold = 0.001f;
	constexpr float kMinimumDirectionLength = 1e-6f;
	constexpr float kCellSize = 4096.0f;
	constexpr float kMinimumDepthPadding = 1.0f;
	constexpr int kRasterDepthBias = 1;
	constexpr float kTerrainSlopeScaledDepthBias = 1.0f;
	constexpr uint32_t kCollectIntervalFrames = 4;
	constexpr uint32_t kFullWalkIntervalScans = 8;
	constexpr uint64_t kHashSeed = 14695981039346656037ull;
	constexpr uint64_t kHashPrime = 1099511628211ull;

	constexpr float kDefaultTimeScale = 20.0f;
	constexpr uint32_t kMeshLODLevels = 3;

	// Engine globals and members that this fork's CommonLib revision does not name yet. The ids and
	// offsets are those of CommonLibSSE-NG 15.2 (BGSTerrainManager, BGSDistantTreeBlock, NiAVObject,
	// BSMeshLODTriShape).
	RE::BGSTerrainManager* ActiveTerrainManager()
	{
		static REL::Relocation<RE::BGSTerrainManager**> manager{ REL::RelocationID(516111, 402262) };
		return *manager;
	}

	RE::BSSpinLock& CameraTerrainLock()
	{
		static REL::Relocation<RE::BSSpinLock*> lock{ REL::RelocationID(516179, 402330) };
		return *lock;
	}

	RE::NiPointer<RE::NiNode>& LODLandRoot()
	{
		static REL::Relocation<RE::NiPointer<RE::NiNode>*> root{ REL::RelocationID(516173, 402324) };
		return *root;
	}

	RE::NiPointer<RE::NiNode>& LODObjectRoot()
	{
		static REL::Relocation<RE::NiPointer<RE::NiNode>*> root{ REL::RelocationID(516174, 402325) };
		return *root;
	}

	RE::NiPointer<RE::NiNode>& LODTreeRoot()
	{
		static REL::Relocation<RE::NiPointer<RE::NiNode>*> root{ REL::RelocationID(516170, 402321) };
		return *root;
	}

	bool LoadScreenUp()
	{
		static REL::Relocation<bool*> up{ REL::RelocationID(516108, 402259) };
		return *up;
	}

	RE::NiPointer<RE::NiTexture>& TreeAtlasTexture()
	{
		static REL::Relocation<RE::NiPointer<RE::NiTexture>*> texture{ REL::RelocationID(516086, 402199) };
		return *texture;
	}

	/** @brief BGSTerrainManager::mapMode, the first byte of the manager. */
	bool IsTerrainMapMode(const RE::BGSTerrainManager* a_manager)
	{
		return *reinterpret_cast<const bool*>(a_manager);
	}

	/** @brief NiAVObject LOD mode byte: bit 7 single level, bits 0-6 the level. */
	uint8_t GetLODMode(const RE::NiAVObject* a_object)
	{
		return *(reinterpret_cast<const uint8_t*>(a_object) + (REL::Module::IsVR() ? 0x120 : 0x108));
	}

	/** @brief BSMeshLODTriShape triangle counts, one per LOD level. */
	const uint32_t* MeshLODTriangleCounts(const RE::BSGeometry* a_geometry)
	{
		return reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(a_geometry) + (REL::Module::IsVR() ? 0x1A0 : 0x160));
	}

	bool IsMapView()
	{
		const auto* manager = ActiveTerrainManager();
		return globals::state->isMapMenuOpen || (manager && IsTerrainMapMode(manager));
	}

	float ShadowClockRate()
	{
		const auto* calendar = globals::game::calendar;
		const float timeScale = calendar ? calendar->GetTimescale() : kDefaultTimeScale;
		return std::isfinite(timeScale) ? std::max(1.0f, timeScale / kDefaultTimeScale) : 1.0f;
	}

	uint32_t BatchTriangleBudget(uint32_t a_triangleBudget)
	{
		return uint32_t(std::min(double(a_triangleBudget) * ShadowClockRate(), double(std::numeric_limits<uint32_t>::max() / kIndicesPerTriangle)));
	}

	void HashBytes(uint64_t& a_hash, const void* a_data, size_t a_size)
	{
		const auto* bytes = static_cast<const uint8_t*>(a_data);
		size_t i = 0;
		for (; i + sizeof(uint64_t) <= a_size; i += sizeof(uint64_t)) {
			uint64_t word;
			std::memcpy(&word, bytes + i, sizeof(word));
			a_hash = (a_hash ^ word) * kHashPrime;
			a_hash ^= a_hash >> 32;
		}
		for (; i < a_size; ++i)
			a_hash = (a_hash ^ bytes[i]) * kHashPrime;
	}

	/** @brief Row-vector world matrix of a transform: scale, then rotation, then translation. */
	DirectX::XMMATRIX WorldMatrix(const RE::NiTransform& a_transform)
	{
		const auto& r = a_transform.rotate.entry;
		const auto& t = a_transform.translate;
		const float s = a_transform.scale;
		return DirectX::XMMatrixSet(
			r[0][0] * s, r[1][0] * s, r[2][0] * s, 0,
			r[0][1] * s, r[1][1] * s, r[2][1] * s, 0,
			r[0][2] * s, r[1][2] * s, r[2][2] * s, 0,
			t.x, t.y, t.z, 1);
	}

	/** @brief Saves the pipeline state the caster pass touches and restores it on scope exit. */
	class CasterPassScope
	{
	public:
		explicit CasterPassScope(ID3D11DeviceContext* a_context) :
			context(a_context)
		{
			context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depth);
			context->RSGetViewports(&viewportCount, viewports);
			context->OMGetBlendState(&blend, blendFactor, &sampleMask);
			context->OMGetDepthStencilState(&depthState, &stencilRef);
			context->RSGetState(&rasterizer);
			context->VSGetShader(&vertexShader, nullptr, nullptr);
			context->PSGetShader(&pixelShader, nullptr, nullptr);
			context->GSGetShader(&geometryShader, nullptr, nullptr);
			context->HSGetShader(&hullShader, nullptr, nullptr);
			context->DSGetShader(&domainShader, nullptr, nullptr);
			context->VSGetConstantBuffers(0, 1, &vertexBuffer0);
			context->PSGetConstantBuffers(0, 1, &pixelBuffer0);
			context->PSGetSamplers(0, 1, &sampler0);
			context->PSGetShaderResources(0, 1, &texture0);
			context->IAGetInputLayout(&layout);
			context->IAGetVertexBuffers(0, kVertexStreams, vertexStreams, strides, offsets);
			context->IAGetIndexBuffer(&indices, &indexFormat, &indexOffset);
			context->IAGetPrimitiveTopology(&topology);
		}

		~CasterPassScope()
		{
			context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, depth);
			context->RSSetViewports(viewportCount, viewports);
			context->OMSetBlendState(blend, blendFactor, sampleMask);
			context->OMSetDepthStencilState(depthState, stencilRef);
			context->RSSetState(rasterizer);
			context->VSSetShader(vertexShader, nullptr, 0);
			context->PSSetShader(pixelShader, nullptr, 0);
			context->GSSetShader(geometryShader, nullptr, 0);
			context->HSSetShader(hullShader, nullptr, 0);
			context->DSSetShader(domainShader, nullptr, 0);
			context->VSSetConstantBuffers(0, 1, &vertexBuffer0);
			context->PSSetConstantBuffers(0, 1, &pixelBuffer0);
			context->PSSetSamplers(0, 1, &sampler0);
			context->PSSetShaderResources(0, 1, &texture0);
			context->IASetInputLayout(layout);
			context->IASetVertexBuffers(0, kVertexStreams, vertexStreams, strides, offsets);
			context->IASetIndexBuffer(indices, indexFormat, indexOffset);
			context->IASetPrimitiveTopology(topology);
			for (auto* target : targets)
				Release(target);
			for (auto* stream : vertexStreams)
				Release(stream);
			Release(depth);
			Release(blend);
			Release(depthState);
			Release(rasterizer);
			Release(vertexShader);
			Release(pixelShader);
			Release(geometryShader);
			Release(hullShader);
			Release(domainShader);
			Release(vertexBuffer0);
			Release(pixelBuffer0);
			Release(sampler0);
			Release(texture0);
			Release(layout);
			Release(indices);
		}

		CasterPassScope(const CasterPassScope&) = delete;
		CasterPassScope& operator=(const CasterPassScope&) = delete;

	private:
		static void Release(IUnknown* a_object)
		{
			if (a_object)
				a_object->Release();
		}

		static constexpr UINT kVertexStreams = 2;
		ID3D11DeviceContext* context;
		ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* depth = nullptr;
		UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		ID3D11BlendState* blend = nullptr;
		FLOAT blendFactor[4]{};
		UINT sampleMask = 0;
		ID3D11DepthStencilState* depthState = nullptr;
		UINT stencilRef = 0;
		ID3D11RasterizerState* rasterizer = nullptr;
		ID3D11VertexShader* vertexShader = nullptr;
		ID3D11PixelShader* pixelShader = nullptr;
		ID3D11GeometryShader* geometryShader = nullptr;
		ID3D11HullShader* hullShader = nullptr;
		ID3D11DomainShader* domainShader = nullptr;
		ID3D11Buffer* vertexBuffer0 = nullptr;
		ID3D11Buffer* pixelBuffer0 = nullptr;
		ID3D11SamplerState* sampler0 = nullptr;
		ID3D11ShaderResourceView* texture0 = nullptr;
		ID3D11InputLayout* layout = nullptr;
		ID3D11Buffer* vertexStreams[kVertexStreams]{};
		UINT strides[kVertexStreams]{};
		UINT offsets[kVertexStreams]{};
		ID3D11Buffer* indices = nullptr;
		DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
		UINT indexOffset = 0;
		D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	};

	template <class F>
	void ForEachCasterBound(RE::BSGeometry* a_geometry, F&& a_callback)
	{
		if (a_geometry->GetType() == RE::BSGeometry::Type::kMultiStreamInstanceTriShape) {
			const auto& instances = static_cast<RE::BSMultiStreamInstanceTriShape*>(a_geometry)->GetMultiStreamTrishapeRuntimeData();
			for (auto* group : instances.instanceGroups) {
				if (!group || !group->vertexBuffer || !group->vertexBuffer->buffer || !group->instanceCount)
					continue;
				RE::NiBound bound;
				group->GetNiBound(&bound);
				a_callback(bound);
			}
		} else {
			a_callback(a_geometry->worldBound);
		}
	}
}

std::pair<std::string, std::vector<std::string>> LODShadows::GetFeatureSummary()
{
	return { T("feature.lod_shadows.description", "Casts cached sun and moon shadows from terrain, distant objects and trees."), {} };
}

void LODShadows::LoadSettings(json& a_json)
{
	const bool wasSettingsEnabled = settings.Enabled;
	settings = a_json;
	if (!std::isfinite(settings.UpdateBudgetMs))
		settings.UpdateBudgetMs = Settings{}.UpdateBudgetMs;
	if (!std::isfinite(settings.DepthBias))
		settings.DepthBias = Settings{}.DepthBias;
	settings.UpdateBudgetMs = std::clamp(settings.UpdateBudgetMs, kMinimumUpdateBudgetMs, kMaximumUpdateBudgetMs);
	settings.DepthBias = std::clamp(settings.DepthBias, 0.0f, 64.0f);
	if (wasSettingsEnabled != settings.Enabled)
		resetRequested = true;
}

void LODShadows::SaveSettings(json& a_json) { a_json = settings; }

void LODShadows::RestoreDefaultSettings()
{
	settings = {};
	resetRequested = true;
}
void LODShadows::GameLoaded() { resetRequested = true; }

#define I18N_KEY_PREFIX "feature.lod_shadows."

void LODShadows::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enable LOD shadows"), &settings.Enabled);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("enabled_tooltip"), "Distant terrain, object LOD and tree LOD cast and receive sun and moon shadows past the shadow cascades. Three cached depth maps follow the camera and refresh a little every frame."));
	ImGui::SliderFloat(T(TKEY("update_budget"), "GPU update budget (ms)"), &settings.UpdateBudgetMs, kMinimumUpdateBudgetMs, kMaximumUpdateBudgetMs, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("update_budget_tooltip"), "GPU time spent per frame on redrawing the cached maps. Lower costs less and makes the shadows follow the sun and the camera more slowly."));
	ImGui::SliderFloat(T(TKEY("depth_bias"), "Depth bias"), &settings.DepthBias, 0.0f, 64.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("depth_bias_tooltip"), "Distance in game units a surface has to be behind a caster to count as shadowed. Raise it against speckled self shadowing, lower it if shadows detach from their casters."));
}

void LODShadows::DrawDebugSection()
{
	Util::DebugSection section;
	if (!section)
		return;

	ImGui::Text(T(TKEY("debug_state"), "Ready: %d, valid maps: %u, covers terrain: %d"), ready ? 1 : 0, publishedValidMaps, CoversTerrain() ? 1 : 0);
	ImGui::Text(T(TKEY("debug_geometry"), "Geometry: terrain %u (loaded %u), object LOD %u, tree LOD %u, loaded objects %u"),
		geometryCounts[0], loadedTerrainGeometryCount, geometryCounts[1], geometryCounts[2], geometryCounts[3]);
	ImGui::Text(T(TKEY("debug_casters"), "Pending map: %d, pending casters: %u, unsupported casters: %u, completed maps: %u"),
		pendingIndex, uint32_t(pendingIndex >= 0 ? casters.size() - casterIndex : 0), unsupportedCasters, completedMaps);
	ImGui::Text(T(TKEY("debug_update"), "Last update: %u draws, %u triangles, budget %u triangles"), lastDraws, lastTriangles, updateBudget.Triangles());
	ImGui::Text(T(TKEY("debug_timing"), "GPU update: last %.3f ms, max %.3f ms; full refresh: last %.3f ms, max %.3f ms"), lastUpdateMs, maxUpdateMs, lastRefreshMs, maxRefreshMs);
	for (uint32_t i = 0; i < kMapCount; ++i)
		ImGui::Text(T(TKEY("debug_map"), "Map %u: valid %d, terrain %d, half extent %.0f, depth range %.0f, triangles %llu, history blend %.2f"),
			i, maps[i].valid ? 1 : 0, maps[i].terrainValid ? 1 : 0, maps[i].halfExtent, maps[i].depthRange, maps[i].triangleCount, GetHistoryBlend(i));
	ImGui::Checkbox(T(TKEY("debug_refresh_every_frame"), "Redraw all maps every frame"), &refreshEveryFrame);
	ImGui::Checkbox(T(TKEY("debug_history"), "Blend from the previous map"), &interpolateHistory);
	ImGui::Checkbox(T(TKEY("debug_throttle"), "Throttle the loaded object scan"), &throttleCollect);
	if (Util::Button(T(TKEY("debug_rebuild"), "Rebuild maps")))
		resetRequested = true;
}

#undef I18N_KEY_PREFIX

void LODShadows::CompileShaders()
{
	vertexShader = nullptr;
	treeVertexShader = nullptr;
	alphaShader = nullptr;
	for (auto& cache : layouts)
		cache.clear();
	vertexBytecode = nullptr;
	treeBytecode = nullptr;
	const wchar_t* path = L"Data\\Shaders\\LODShadows\\Caster.hlsl";
	vertexShader.attach(static_cast<ID3D11VertexShader*>(Util::CompileShader(path, { { "VSHADER", "1" } }, "vs_5_0", "main", vertexBytecode.put())));
	treeVertexShader.attach(static_cast<ID3D11VertexShader*>(Util::CompileShader(path, { { "VSHADER", "1" }, { "INSTANCED_TREE", "1" } }, "vs_5_0", "main", treeBytecode.put())));
	alphaShader.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(path, {}, "ps_5_0")));
	if (!vertexShader || !treeVertexShader || !alphaShader || !vertexBytecode || !treeBytecode) {
		vertexShader = nullptr;
		treeVertexShader = nullptr;
		alphaShader = nullptr;
		return;
	}
	Util::SetResourceName(vertexShader.get(), "LODShadows::Caster VS");
	Util::SetResourceName(treeVertexShader.get(), "LODShadows::TreeCaster VS");
	Util::SetResourceName(alphaShader.get(), "LODShadows::AlphaTest PS");
}

void LODShadows::SetupResources()
{
	ready = false;
	try {
		auto device = globals::d3d::device;
		D3D11_TEXTURE2D_DESC textureDesc{};
		textureDesc.Width = textureDesc.Height = kResolution;
		textureDesc.MipLevels = 1;
		textureDesc.ArraySize = kDepthSliceCount * 2;
		textureDesc.Format = DXGI_FORMAT_R16_TYPELESS;
		textureDesc.SampleDesc.Count = 1;
		textureDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
		DX::ThrowIfFailed(device->CreateTexture2D(&textureDesc, nullptr, depthTexture.put()));
		Util::SetResourceName(depthTexture.get(), "LODShadows::DepthMaps");
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_R16_UNORM;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		srvDesc.Texture2DArray.MipLevels = 1;
		srvDesc.Texture2DArray.ArraySize = textureDesc.ArraySize;
		DX::ThrowIfFailed(device->CreateShaderResourceView(depthTexture.get(), &srvDesc, depthSRV.put()));
		Util::SetResourceName(depthSRV.get(), "LODShadows::DepthMaps SRV");
		for (uint32_t i = 0; i < depthViews.size(); ++i) {
			D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
			desc.Format = DXGI_FORMAT_D16_UNORM;
			desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
			desc.Texture2DArray.FirstArraySlice = i;
			desc.Texture2DArray.ArraySize = 1;
			DX::ThrowIfFailed(device->CreateDepthStencilView(depthTexture.get(), &desc, depthViews[i].put()));
			Util::SetResourceName(depthViews[i].get(), "LODShadows::DepthMap %u DSV", i);
		}
		D3D11_DEPTH_STENCIL_DESC depthDesc{};
		depthDesc.DepthEnable = true;
		depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		DX::ThrowIfFailed(device->CreateDepthStencilState(&depthDesc, depthState.put()));
		Util::SetResourceName(depthState.get(), "LODShadows::DepthState");
		D3D11_RASTERIZER_DESC rasterDesc{};
		rasterDesc.FillMode = D3D11_FILL_SOLID;
		rasterDesc.CullMode = D3D11_CULL_NONE;
		rasterDesc.DepthClipEnable = true;
		rasterDesc.DepthBias = kRasterDepthBias;
		DX::ThrowIfFailed(device->CreateRasterizerState(&rasterDesc, rasterizer.put()));
		Util::SetResourceName(rasterizer.get(), "LODShadows::Rasterizer");
		rasterDesc.SlopeScaledDepthBias = kTerrainSlopeScaledDepthBias;
		DX::ThrowIfFailed(device->CreateRasterizerState(&rasterDesc, terrainRasterizer.put()));
		Util::SetResourceName(terrainRasterizer.get(), "LODShadows::TerrainRasterizer");
		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, alphaSampler.put()));
		Util::SetResourceName(alphaSampler.get(), "LODShadows::AlphaSampler");
		drawBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<DrawData>(), "LODShadows::CasterData");
		for (auto& query : queries) {
			query.batch.Configure(1, "LODShadows::Update");
			if (!query.batch.Preallocate(device))
				return;
		}
		CompileShaders();
		ready = vertexShader && treeVertexShader && alphaShader;
		Invalidate();
		Publish();
	} catch (const std::exception& error) {
		logger::error("LOD Shadows resource creation failed: {}", error.what());
	}
}

void LODShadows::ClearShaderCache()
{
	ready = false;
	Invalidate();
	try {
		CompileShaders();
		ready = depthTexture && vertexShader && treeVertexShader && alphaShader;
	} catch (const std::exception& error) {
		logger::error("LOD Shadows shader creation failed: {}", error.what());
	}
}

void LODShadows::Invalidate()
{
	for (auto& map : maps)
		map.valid = false;
	for (auto& map : previousMaps)
		map.valid = false;
	for (auto& map : completedDepthMaps)
		map.valid = false;
	casters.clear();
	visibleGeometry.clear();
	loadedRefCache.clear();
	terrainGeometryCount = 0;
	terrainAvailable = false;
	pendingIndex = -1;
	nextMap = 0;
	clearPending = false;
}

void LODShadows::Prepass() { BindMaps(); }

void LODShadows::HashGeometry(RE::BSGeometry* a_geometry)
{
	auto* geometry = a_geometry;
	HashBytes(sceneHash, &geometry, sizeof(geometry));
	HashBytes(sceneHash, &geometry->world, sizeof(geometry->world));
	const auto lodMode = GetLODMode(geometry);
	HashBytes(sceneHash, &lodMode, sizeof(lodMode));
	auto& data = geometry->GetGeometryRuntimeData();
	HashBytes(sceneHash, &data.rendererData, sizeof(data.rendererData));
	if (auto segmented = geometry->AsSubIndexTriShape()) {
		auto& segments = segmented->GetSubIndexedTrishapeRuntimeData();
		if (segments.segmentData) {
			for (uint32_t i = 0; i < segments.numSegments; ++i) {
				const auto& range = segments.segmentData[i];
				HashBytes(sceneHash, &range.index, sizeof(range.index));
				HashBytes(sceneHash, &range.numTris, sizeof(range.numTris));
				HashBytes(sceneHash, &range.flags, sizeof(range.flags));
			}
		}
	} else if (geometry->GetType() == RE::BSGeometry::Type::kMultiStreamInstanceTriShape) {
		auto& instances = static_cast<RE::BSMultiStreamInstanceTriShape*>(geometry)->GetMultiStreamTrishapeRuntimeData();
		HashBytes(sceneHash, &instances.instanceCount, sizeof(instances.instanceCount));
		for (auto group : instances.instanceGroups) {
			if (group && group->vertexBuffer) {
				HashBytes(sceneHash, &group->vertexBuffer->buffer, sizeof(group->vertexBuffer->buffer));
				HashBytes(sceneHash, &group->instanceCount, sizeof(group->instanceCount));
				HashBytes(sceneHash, &group->center, sizeof(group->center));
				HashBytes(sceneHash, &group->size, sizeof(group->size));
			}
		}
	}
}

void LODShadows::Collect(RE::NiAVObject* a_object, bool a_ignoreAppCulling, bool a_loadedObject)
{
	if (!a_object)
		return;
	if (!a_ignoreAppCulling && a_object->GetAppCulled())
		return;
	if (auto node = a_object->AsNode()) {
		for (auto& child : node->GetChildren())
			Collect(child.get(), a_ignoreAppCulling, a_loadedObject);
	} else if (auto geometry = a_object->AsGeometry()) {
		if (a_loadedObject) {
			const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
			if (!property || !property->material || property->material->GetType() != RE::BSShaderMaterial::Type::kLighting ||
				!property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kZBufferWrite) ||
				property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kRefraction, RE::BSShaderProperty::EShaderPropertyFlag::kTempRefraction) ||
				!collectedGeometry.insert(geometry).second)
				return;
		}
		visibleGeometry.emplace_back(geometry);
		HashGeometry(geometry);
	}
}

void LODShadows::CollectTerrain()
{
	Collect(LODLandRoot().get(), true);
	terrainAvailable = !visibleGeometry.empty();
	const auto lodTerrainCount = visibleGeometry.size();
	landBounds = float4{};
	const auto* tes = globals::game::tes;
	const auto* grid = tes ? tes->gridCells : nullptr;
	if (grid && grid->cells) {
		landBounds = float4{ (tes->currentGridX + 0.5f) * kCellSize, (tes->currentGridY + 0.5f) * kCellSize,
			grid->length * kCellSize * 0.5f, grid->length * kCellSize * 0.5f };
		for (uint32_t x = 0; x < grid->length; ++x) {
			for (uint32_t y = 0; y < grid->length; ++y) {
				const auto* cell = grid->GetCell(x, y);
				if (!cell || !cell->IsAttached() || !cell->IsExteriorCell())
					continue;
				const auto* land = cell->GetRuntimeData().cellLand;
				if (land && land->loadedData)
					for (auto* mesh : land->loadedData->mesh)
						Collect(mesh);
			}
		}
	}
	terrainGeometryCount = uint32_t(visibleGeometry.size());
	loadedTerrainGeometryCount = uint32_t(visibleGeometry.size() - lodTerrainCount);
	HashBytes(sceneHash, &landBounds, sizeof(landBounds));
}

void LODShadows::CollectLoadedObjects(bool a_fullWalk)
{
	collectedGeometry.clear();
	const auto lodGeometryCount = visibleGeometry.size();
	bool lodGeometrySeeded = false;
	++loadedScanId;
	if (auto* tes = globals::game::tes) {
		tes->ForEachReference([&](RE::TESObjectREFR* reference) {
			const auto* base = reference ? reference->GetBaseObject() : nullptr;
			const auto* staticObject = base ? base->As<RE::TESObjectSTAT>() : nullptr;
			if (!base || !base->Is(RE::FormType::Static, RE::FormType::StaticCollection, RE::FormType::Tree) ||
				(staticObject && staticObject->IsSkyObject()) || reference->IsDisabled() || reference->IsDeleted())
				return RE::BSContainer::ForEachResult::kContinue;
			auto* root = reference->Get3D();
			if (!root)
				return RE::BSContainer::ForEachResult::kContinue;
			auto& entry = loadedRefCache[reference];
			entry.scanId = loadedScanId;
			const bool culled = root->GetAppCulled();
			if (!a_fullWalk && entry.root.get() == root && entry.culled == culled) {
				for (const auto& geometry : entry.geometry) {
					visibleGeometry.emplace_back(geometry);
					HashGeometry(geometry.get());
				}
				return RE::BSContainer::ForEachResult::kContinue;
			}
			if (!lodGeometrySeeded) {
				for (size_t i = 0; i < lodGeometryCount; ++i)
					collectedGeometry.insert(visibleGeometry[i].get());
				lodGeometrySeeded = true;
			}
			const auto first = visibleGeometry.size();
			Collect(root, false, true);
			entry.root.reset(root);
			entry.culled = culled;
			entry.geometry.assign(visibleGeometry.begin() + first, visibleGeometry.end());
			return RE::BSContainer::ForEachResult::kContinue;
		});
	}
	std::erase_if(loadedRefCache, [this](const auto& item) { return item.second.scanId != loadedScanId; });
}

void LODShadows::AddCaster(RE::BSGeometry* a_geometry)
{
	auto shape = a_geometry->AsTriShape();
	auto& data = a_geometry->GetGeometryRuntimeData();
	auto mesh = static_cast<RE::BSGraphics::TriShape*>(data.rendererData);
	auto property = data.shaderProperty.get();
	if (!shape || !mesh || !mesh->vertexBuffer || !mesh->indexBuffer || data.skinInstance || !property) {
		++unsupportedCasters;
		return;
	}
	const auto type = a_geometry->GetType();
	const bool instanced = type == RE::BSGeometry::Type::kMultiStreamInstanceTriShape;
	if (type != RE::BSGeometry::Type::kTriShape && type != RE::BSGeometry::Type::kSubIndexTriShape &&
		type != RE::BSGeometry::Type::kMeshLODTriShape && type != RE::BSGeometry::Type::kMultiIndexTriShape && !instanced) {
		++unsupportedCasters;
		return;
	}
	Caster caster;
	caster.lodLandscape = property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kLODLandscape);
	caster.geometry.reset(a_geometry);
	caster.vertices.copy_from(reinterpret_cast<ID3D11Buffer*>(mesh->vertexBuffer));
	caster.indices.copy_from(reinterpret_cast<ID3D11Buffer*>(mesh->indexBuffer));
	caster.vertexDescriptor = std::bit_cast<uint64_t>(data.vertexDesc);
	constexpr uint64_t kStrideMask = 0xf;
	caster.stride = uint32_t(caster.vertexDescriptor & kStrideMask) * sizeof(float);
	caster.indexCount = shape->GetTrishapeRuntimeData().triangleCount * kIndicesPerTriangle;
	if (type == RE::BSGeometry::Type::kMeshLODTriShape) {
		constexpr uint8_t kSingleLevel = 0x80;
		const auto mode = GetLODMode(a_geometry);
		const uint32_t level = mode & ~kSingleLevel;
		const auto* counts = MeshLODTriangleCounts(a_geometry);
		if (level > kMeshLODLevels || ((mode & kSingleLevel) && level == kMeshLODLevels)) {
			++unsupportedCasters;
			return;
		}
		uint32_t prefix = 0;
		for (uint32_t i = 0; i < level; ++i)
			prefix += counts[i] * kIndicesPerTriangle;
		caster.firstIndex = mode & kSingleLevel ? prefix : 0;
		caster.indexCount = mode & kSingleLevel ? counts[level] * kIndicesPerTriangle : prefix;
	}
	caster.transform = a_geometry->world;
	caster.bound = a_geometry->worldBound;
	if (caster.stride < sizeof(float) * 3) {
		++unsupportedCasters;
		return;
	}
	if (!caster.indexCount)
		return;
	if (auto material = property->material) {
		caster.uvTransform = float4{ material->texCoordScale[0].x, material->texCoordScale[0].y, material->texCoordOffset[0].x, material->texCoordOffset[0].y };
		if (material->GetType() == RE::BSShaderMaterial::Type::kLighting)
			caster.materialAlpha = static_cast<RE::BSLightingShaderMaterialBase*>(material)->materialAlpha;
	}
	if (auto alpha = data.alphaProperty.get(); alpha && alpha->GetAlphaTesting())
		caster.alphaThreshold = alpha->alphaThreshold / 255.0f;
	if (instanced)
		caster.alphaThreshold = std::max(caster.alphaThreshold, 0.5f);
	if (caster.alphaThreshold > 0) {
		auto texture = instanced ? skyrim_cast<RE::NiSourceTexture*>(TreeAtlasTexture().get()) : property->GetBaseTexture();
		if (!data.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_UV) || !texture || !texture->rendererTexture || !texture->rendererTexture->resourceView) {
			++unsupportedCasters;
			return;
		}
		caster.texture.copy_from(reinterpret_cast<ID3D11ShaderResourceView*>(texture->rendererTexture->resourceView));
	}
	D3D11_BUFFER_DESC indexDesc{};
	caster.indices->GetDesc(&indexDesc);
	if ((uint64_t(caster.firstIndex) + caster.indexCount) * sizeof(uint16_t) > indexDesc.ByteWidth) {
		++unsupportedCasters;
		return;
	}
	if (instanced) {
		auto& instances = static_cast<RE::BSMultiStreamInstanceTriShape*>(a_geometry)->GetMultiStreamTrishapeRuntimeData();
		caster.instanceStride = uint32_t((caster.vertexDescriptor >> 4) & kStrideMask) * sizeof(float);
		if (caster.instanceStride < sizeof(uint16_t) * 8) {
			++unsupportedCasters;
			return;
		}
		for (auto group : instances.instanceGroups) {
			if (!group || !group->vertexBuffer || !group->vertexBuffer->buffer || !group->instanceCount)
				continue;
			caster.instances.copy_from(reinterpret_cast<ID3D11Buffer*>(group->vertexBuffer->buffer));
			caster.instanceCount = uint32_t(std::min(size_t(group->instanceCount), group->vertexBuffer->byteWidth / caster.instanceStride));
			group->GetNiBound(&caster.bound);
			if (caster.instanceCount && Intersects(caster))
				casters.push_back(caster);
		}
	} else if (auto segmented = a_geometry->AsSubIndexTriShape()) {
		auto& segments = segmented->GetSubIndexedTrishapeRuntimeData();
		if (!segments.segmentData) {
			++unsupportedCasters;
			return;
		}
		for (uint32_t i = 0; i < (segments.nonSegmented ? 1u : segments.numSegments); ++i) {
			const auto& range = segments.segmentData[i];
			if (!range.flags)
				continue;
			caster.firstIndex = segments.nonSegmented ? 0 : range.index;
			caster.indexCount = (segments.nonSegmented ? shape->GetTrishapeRuntimeData().triangleCount : range.numTris) * kIndicesPerTriangle;
			if ((uint64_t(caster.firstIndex) + caster.indexCount) * sizeof(uint16_t) > indexDesc.ByteWidth) {
				++unsupportedCasters;
				continue;
			}
			if (caster.indexCount && Intersects(caster))
				casters.push_back(caster);
		}
	} else if (Intersects(caster)) {
		casters.push_back(std::move(caster));
	}
}

bool LODShadows::Intersects(const Caster& a_caster) const
{
	using namespace DirectX;
	const auto& center = a_caster.bound.center;
	const auto point = XMVector3TransformCoord(XMVectorSet(center.x, center.y, center.z, 1), XMLoadFloat4x4(&pendingMap.data.worldToClip));
	const float radiusXY = a_caster.bound.radius / pendingMap.halfExtent;
	const float radiusZ = a_caster.bound.radius / pendingMap.depthRange;
	return std::abs(XMVectorGetX(point)) <= 1 + radiusXY && std::abs(XMVectorGetY(point)) <= 1 + radiusXY && XMVectorGetZ(point) >= -radiusZ && XMVectorGetZ(point) <= 1 + radiusZ;
}

void LODShadows::BeginMap(uint32_t a_index, const RE::NiPoint3& a_center, const RE::NiPoint3& a_direction)
{
	using namespace DirectX;
	pendingIndex = a_index;
	pendingMap = {};
	pendingMap.center = a_center;
	pendingMap.direction = a_direction;
	for (uint32_t slice = a_index * kDepthSlicesPerMap; slice < (a_index + 1) * kDepthSlicesPerMap; ++slice) {
		if ((!maps[a_index].valid || slice != maps[a_index].slice) &&
			(!previousMaps[a_index].valid || slice != previousMaps[a_index].slice)) {
			pendingMap.slice = slice;
			break;
		}
	}
	assert(!maps[a_index].valid || pendingMap.slice != maps[a_index].slice);
	assert(!previousMaps[a_index].valid || pendingMap.slice != previousMaps[a_index].slice);
	pendingMap.scene = sceneHash;
	pendingMap.frame = uint32_t(globals::state->frameCount);
	pendingMap.transitionGeneration = transitionGeneration;
	pendingMap.terrainValid = terrainAvailable;
	pendingLandBounds = landBounds;
	const auto direction = XMVectorSet(a_direction.x, a_direction.y, a_direction.z, 0);
	const auto up = std::abs(a_direction.z) > 0.99f ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(0, 0, 1, 0);
	const auto rotation = XMMatrixLookToLH(XMVectorZero(), direction, up);
	const auto center = XMVector3TransformCoord(XMVectorSet(a_center.x, a_center.y, a_center.z, 1), rotation);
	float extent = kMinimumHalfExtents[a_index];
	if (a_index == kMapCount - 1) {
		for (const auto& geometry : visibleGeometry) {
			ForEachCasterBound(geometry.get(), [&](const RE::NiBound& bound) {
				const auto offset = bound.center - a_center;
				const auto point = XMVector3TransformNormal(XMVectorSet(offset.x, offset.y, offset.z, 0), rotation);
				const float requiredExtent = std::max(std::abs(XMVectorGetX(point)), std::abs(XMVectorGetY(point))) + bound.radius;
				if (std::isfinite(requiredExtent))
					extent = std::max(extent, std::ceil(requiredExtent / ((1.0f - kCenterMargin) * kCellSize)) * kCellSize);
			});
		}
	}
	const float texel = extent * 2 / kResolution;
	const float centerX = std::round(XMVectorGetX(center) / texel) * texel;
	const float centerY = std::round(XMVectorGetY(center) / texel) * texel;
	float depthNear = -kCellSize;
	float depthFar = kCellSize;
	for (const auto& geometry : visibleGeometry) {
		ForEachCasterBound(geometry.get(), [&](const RE::NiBound& bound) {
			const auto offset = bound.center - a_center;
			const auto point = XMVector3TransformNormal(XMVectorSet(offset.x, offset.y, offset.z, 0), rotation);
			const float x = XMVectorGetX(point) + XMVectorGetX(center) - centerX;
			const float y = XMVectorGetY(point) + XMVectorGetY(center) - centerY;
			const float z = XMVectorGetZ(point);
			if (std::abs(x) <= extent + bound.radius && std::abs(y) <= extent + bound.radius &&
				std::isfinite(z) && std::isfinite(bound.radius)) {
				depthNear = std::min(depthNear, z - bound.radius);
				depthFar = std::max(depthFar, z + bound.radius);
			}
		});
	}
	const float padding = std::max(kMinimumDepthPadding, settings.DepthBias);
	depthNear -= padding;
	depthFar += padding;
	pendingMap.halfExtent = extent;
	pendingMap.depthRange = depthFar - depthNear;
	const auto translation = XMMatrixTranslation(-centerX, -centerY, -XMVectorGetZ(center) - depthNear);
	XMStoreFloat4x4(&pendingMap.data.worldToClip, rotation * translation * XMMatrixOrthographicLH(extent * 2, extent * 2, 0, pendingMap.depthRange));
	casters.clear();
	unsupportedCasters = 0;
	const auto* scene = globals::game::smState->shadowSceneNode[0];
	const auto* nativeLight = scene ? scene->GetRuntimeData().sunShadowDirLight : nullptr;
	const float nativeDistance = nativeLight ? nativeLight->GetShadowDirectionalLightRuntimeData().endSplitDistances[1] : 0.0f;
	const auto loadedObjectStart = visibleGeometry.size() - geometryCounts[3];
	for (uint32_t i = 0; i < visibleGeometry.size(); ++i) {
		const auto unsupported = unsupportedCasters;
		const auto firstCaster = casters.size();
		AddCaster(visibleGeometry[i].get());
		for (size_t caster = firstCaster; caster < casters.size(); ++caster) {
			const auto& bound = casters[caster].bound;
			casters[caster].nativeShadowCaster = i >= loadedObjectStart && nativeDistance > 0.0f &&
			                                     (bound.center - a_center).Length() - bound.radius < nativeDistance;
		}
		if (i < terrainGeometryCount && unsupportedCasters != unsupported)
			pendingMap.terrainValid = false;
	}
	std::ranges::sort(casters, {}, [](const Caster& caster) { return std::tuple{ caster.nativeShadowCaster, caster.alphaThreshold > 0 }; });
	casterIndex = 0;
	indexOffset = instanceOffset = 0;
	clearPending = true;
	distantDepthCaptured = false;
}

ID3D11InputLayout* LODShadows::GetLayout(const Caster& a_caster)
{
	const bool instanced = bool(a_caster.instances);
	auto& cache = layouts[instanced];
	if (auto it = cache.find(a_caster.vertexDescriptor); it != cache.end())
		return it->second.get();
	const auto desc = std::bit_cast<RE::BSGraphics::VertexDesc>(a_caster.vertexDescriptor);
	std::array<D3D11_INPUT_ELEMENT_DESC, 4> elements{
		D3D11_INPUT_ELEMENT_DESC{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		D3D11_INPUT_ELEMENT_DESC{ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 0, desc.HasFlag(RE::BSGraphics::Vertex::VF_UV) ? desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0) : 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		D3D11_INPUT_ELEMENT_DESC{ "TEXCOORD", 4, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
		D3D11_INPUT_ELEMENT_DESC{ "TEXCOORD", 5, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 8, D3D11_INPUT_PER_INSTANCE_DATA, 1 }
	};
	auto blob = instanced ? treeBytecode.get() : vertexBytecode.get();
	winrt::com_ptr<ID3D11InputLayout> layout;
	DX::ThrowIfFailed(globals::d3d::device->CreateInputLayout(elements.data(), instanced ? 4 : 2, blob->GetBufferPointer(), blob->GetBufferSize(), layout.put()));
	Util::SetResourceName(layout.get(), "LODShadows::CasterLayout");
	return cache.emplace(a_caster.vertexDescriptor, std::move(layout)).first->second.get();
}

void LODShadows::ResolveTimings()
{
	for (auto& query : queries) {
		if (!query.pending)
			continue;
		const auto status = query.batch.TryResolve(globals::d3d::context, [&](uint32_t, uint64_t a_ticks, uint64_t a_frequency) {
			const double elapsedMs = double(a_ticks) * 1000.0 / double(a_frequency);
			if (query.immediateRefresh) {
				lastRefreshMs = elapsedMs;
				lastRefreshFrame = query.frame;
				maxRefreshMs = std::max(maxRefreshMs, elapsedMs);
				return;
			}
			lastUpdateMs = elapsedMs;
			lastUpdateFrame = query.frame;
			maxUpdateMs = std::max(maxUpdateMs, lastUpdateMs);
			++timingSamples;
			if (query.budgetLimited || lastUpdateMs > settings.UpdateBudgetMs)
				updateBudget.Record(query.triangles, lastUpdateMs, settings.UpdateBudgetMs);
		});
		if (status != Util::TimestampQueryBatch::Status::NotReady) {
			query.pending = false;
			query.batch.Reset();
		}
	}
}

void LODShadows::UpdateMaps(const RE::NiPoint3& a_center, const RE::NiPoint3& a_direction, bool a_refreshImmediately)
{
	if (a_refreshImmediately) {
		for (auto& map : completedDepthMaps)
			map.valid = false;
	} else {
		PublishCompletedMaps(false);
	}
	auto& query = queries[queryCursor];
	if (query.pending && !a_refreshImmediately)
		return;
	if (!a_refreshImmediately && pendingIndex < 0) {
		RE::BSSpinLockGuard lock(CameraTerrainLock());
		std::array<uint32_t, kMapCount> candidates{};
		uint32_t candidateCount = 0;
		std::array<double, kMapCount> deadlines{};
		for (uint32_t i = 0; i < kMapCount; ++i) {
			const auto index = (nextMap + i) % kMapCount;
			if (completedDepthMaps[index].valid)
				continue;
			const auto& map = maps[index];
			const bool blending = interpolateHistory && previousMaps[index].valid;
			if (!NeedsUpdate(index, a_center, a_direction) && (!blending || map.direction == a_direction))
				continue;
			deadlines[index] = map.valid ? map.completedTimeMs + (blending ? map.blendDurationMs : 0.0) : 0.0;
			candidates[candidateCount++] = index;
		}
		std::sort(candidates.begin(), candidates.begin() + candidateCount, [&](uint32_t left, uint32_t right) {
			return deadlines[left] < deadlines[right];
		});
		double buildTimeMs = 0;
		const auto batchTriangles = BatchTriangleBudget(updateBudget.Triangles());
		for (uint32_t i = 0; i < candidateCount; ++i) {
			const auto index = candidates[i];
			const auto& map = maps[index];
			const double workloadTimeMs = std::ceil(double(map.triangleCount) / batchTriangles) * frameDurationMs;
			buildTimeMs += std::max(map.buildDurationMs, workloadTimeMs) + frameDurationMs;
			if (frameTimeMs + buildTimeMs >= deadlines[index]) {
				BeginMap(candidates[0], a_center, a_direction);
				break;
			}
		}
		if (pendingIndex < 0)
			return;
	}
	CS_GPU_PASS_SELECT(a_refreshImmediately, "LODShadows::FullRefresh", "LODShadows::Update");
	auto context = globals::d3d::context;
	auto device = globals::d3d::device;
	const bool measured = !query.pending && query.batch.BeginBatch(device, context);
	if (measured)
		query.batch.BeginInterval(device, context);
	const SKSE::stl::scope_exit finishTiming([&]() noexcept {
		if (!measured)
			return;
		query.batch.CommitInterval(context);
		query.batch.EndBatch(context);
		query.pending = true;
		query.frame = uint32_t(globals::state->frameCount);
		query.triangles = lastTriangles;
		query.budgetLimited = pendingIndex >= 0;
		query.immediateRefresh = a_refreshImmediately;
		queryCursor = (queryCursor + 1) % kQueryFrames;
	});
	if (a_refreshImmediately) {
		for (uint32_t i = 0; i < kMapCount; ++i) {
			{
				RE::BSSpinLockGuard lock(CameraTerrainLock());
				BeginMap(i, a_center, a_direction);
			}
			RenderBatch(true);
		}
	} else {
		RenderBatch(false);
	}
}

void LODShadows::CaptureDistantDepth()
{
	if (distantDepthCaptured || (casterIndex < casters.size() && !casters[casterIndex].nativeShadowCaster))
		return;
	auto* context = globals::d3d::context;
	context->OMSetRenderTargets(0, nullptr, nullptr);
	context->CopySubresourceRegion(depthTexture.get(), kDepthSliceCount + pendingMap.slice, 0, 0, 0,
		depthTexture.get(), pendingMap.slice, nullptr);
	auto* depth = depthViews[pendingMap.slice].get();
	context->OMSetRenderTargets(0, nullptr, depth);
	distantDepthCaptured = true;
}

void LODShadows::RenderBatch(bool a_refreshImmediately)
{
	auto context = globals::d3d::context;
	const auto firstTriangle = lastTriangles;
	if (!a_refreshImmediately)
		pendingMap.buildDurationMs += frameDurationMs;
	const auto batchTriangles = BatchTriangleBudget(updateBudget.Triangles());
	{
		CasterPassScope scope(context);
		ID3D11ShaderResourceView* nullView = nullptr;
		context->PSSetShaderResources(kDepthSlot, 1, &nullView);
		auto depth = depthViews[pendingMap.slice].get();
		context->OMSetRenderTargets(0, nullptr, depth);
		context->OMSetDepthStencilState(depthState.get(), 0);
		context->OMSetBlendState(nullptr, nullptr, ~0u);
		ID3D11RasterizerState* activeRasterizer = nullptr;
		const D3D11_VIEWPORT viewport{ 0, 0, float(kResolution), float(kResolution), 0, 1 };
		context->RSSetViewports(1, &viewport);
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		auto buffer = drawBuffer->CB();
		context->VSSetConstantBuffers(0, 1, &buffer);
		context->PSSetConstantBuffers(0, 1, &buffer);
		auto sampler = alphaSampler.get();
		context->PSSetSamplers(0, 1, &sampler);
		if (clearPending) {
			context->ClearDepthStencilView(depth, D3D11_CLEAR_DEPTH, 1, 0);
			clearPending = false;
		}
		while (casterIndex < casters.size() && (a_refreshImmediately || lastTriangles < batchTriangles)) {
			CaptureDistantDepth();
			const auto& caster = casters[casterIndex];
			auto* casterRasterizer = caster.lodLandscape ? terrainRasterizer.get() : rasterizer.get();
			if (activeRasterizer != casterRasterizer) {
				context->RSSetState(casterRasterizer);
				activeRasterizer = casterRasterizer;
			}
			DrawData data;
			DirectX::XMStoreFloat4x4(&data.worldToClip, WorldMatrix(caster.transform) * DirectX::XMLoadFloat4x4(&pendingMap.data.worldToClip));
			DirectX::XMStoreFloat4x4(&data.world, DirectX::XMMatrixTranspose(WorldMatrix(caster.transform)));
			data.uvTransform = caster.uvTransform;
			data.alpha = float4{ caster.alphaThreshold, caster.materialAlpha, caster.lodLandscape ? 1.0f : 0.0f, 0 };
			data.landBounds = pendingLandBounds;
			drawBuffer->Update(data);
			context->VSSetShader(caster.instances ? treeVertexShader.get() : vertexShader.get(), nullptr, 0);
			context->PSSetShader(caster.alphaThreshold > 0 ? alphaShader.get() : nullptr, nullptr, 0);
			auto texture = caster.texture.get();
			context->PSSetShaderResources(0, 1, &texture);
			context->IASetInputLayout(GetLayout(caster));
			ID3D11Buffer* vertices[]{ caster.vertices.get(), caster.instances.get() };
			const UINT strides[]{ caster.stride, caster.instanceStride };
			const UINT offsets[]{ 0, 0 };
			context->IASetVertexBuffers(0, caster.instances ? 2 : 1, vertices, strides, offsets);
			context->IASetIndexBuffer(caster.indices.get(), DXGI_FORMAT_R16_UINT, 0);
			const uint32_t count = a_refreshImmediately ? caster.indexCount - indexOffset :
			                                              std::min(caster.indexCount - indexOffset, (batchTriangles - lastTriangles) * kIndicesPerTriangle);
			const uint32_t instances = caster.instances && indexOffset == 0 && count == caster.indexCount ?
			                               (a_refreshImmediately ? caster.instanceCount - instanceOffset :
																   std::min(caster.instanceCount - instanceOffset, (batchTriangles - lastTriangles) / (count / kIndicesPerTriangle))) :
			                               1;
			if (caster.instances)
				context->DrawIndexedInstanced(count, instances, caster.firstIndex + indexOffset, 0, instanceOffset);
			else
				context->DrawIndexed(count, caster.firstIndex + indexOffset, 0);
			++lastDraws;
			lastTriangles += count / kIndicesPerTriangle * instances;
			indexOffset += count;
			if (indexOffset == caster.indexCount) {
				indexOffset = 0;
				instanceOffset += instances;
				if (instanceOffset == caster.instanceCount) {
					instanceOffset = 0;
					++casterIndex;
				}
			}
		}
		CaptureDistantDepth();
	}
	pendingMap.triangleCount += lastTriangles - firstTriangle;
	if (casterIndex == casters.size()) {
		pendingMap.valid = true;
		completedDepthMaps[pendingIndex] = pendingMap;
		nextMap = (pendingIndex + 1) % kMapCount;
		pendingIndex = -1;
		casters.clear();
		PublishCompletedMaps(a_refreshImmediately);
	}
}

void LODShadows::PublishCompletedMaps(bool a_refreshImmediately)
{
	for (uint32_t i = 0; i < kMapCount; ++i) {
		auto& completed = completedDepthMaps[i];
		if (!completed.valid || (!a_refreshImmediately && GetHistoryBlend(i) < 1.0f))
			continue;
		previousMaps[i] = maps[i];
		previousMaps[i].valid &= !a_refreshImmediately;
		completed.completedTimeMs = frameTimeMs;
		completed.blendDurationMs = kHistoryIntervalsMs[i] / ShadowClockRate();
		maps[i] = completed;
		completed.valid = false;
		++completedMaps;
	}
}

float LODShadows::GetHistoryBlend(uint32_t a_index) const
{
	const auto& map = maps[a_index];
	if (!interpolateHistory || !previousMaps[a_index].valid || map.blendDurationMs <= 0)
		return 1.0f;
	return float(std::clamp((frameTimeMs - map.completedTimeMs) / map.blendDurationMs, 0.0, 1.0));
}

bool LODShadows::NeedsUpdate(uint32_t a_index, const RE::NiPoint3& a_position, const RE::NiPoint3& a_direction) const
{
	const auto& map = maps[a_index];
	return !map.valid || map.scene != sceneHash || map.transitionGeneration != transitionGeneration ||
	       (map.center - a_position).Length() > map.halfExtent * kCenterMargin ||
	       (map.direction - a_direction).Length() > kDirectionThreshold || uint32_t(globals::state->frameCount) - map.frame > kMaximumCacheFrames;
}

bool LODShadows::CoversTerrain() const
{
	if (!ready || !settings.Enabled || resetRequested || IsMapView() || !globals::state->HasDirectionalShadows())
		return false;
	const auto* manager = ActiveTerrainManager();
	return manager && manager->worldSpace && manager->worldSpace->GetFormID() == worldSpaceID &&
	       !LoadScreenUp() && std::ranges::all_of(maps, [](const Map& map) { return map.valid && map.terrainValid; });
}

void LODShadows::Publish()
{
	PerFrame data{};
	uint32_t validMaps = 0;
	const bool active = ready && settings.Enabled && !IsMapView();
	for (uint32_t i = 0; i < kMapCount; ++i) {
		data.maps[i] = maps[i].data;
		const bool valid = active && maps[i].valid;
		const float bias = settings.DepthBias / std::max(kMinimumDepthPadding, maps[i].depthRange);
		const float historyBias = settings.DepthBias / std::max(kMinimumDepthPadding, previousMaps[i].depthRange);
		data.maps[i].parameters = float4{ float(maps[i].slice), valid ? 1.0f : 0.0f, bias, 0 };
		data.maps[i].filter = float4{ 1.0f / kResolution, maps[i].halfExtent * 2 / kResolution, historyBias, 0 };
		data.maps[i].historyWorldToClip = previousMaps[i].data.worldToClip;
		data.maps[i].history = float4{ float(previousMaps[i].slice), GetHistoryBlend(i), previousMaps[i].valid ? 1.0f : 0.0f, 0 };
		validMaps += valid;
	}
	data.maps[0].parameters.w = float(validMaps);
	data.maps[1].parameters.w = CoversTerrain() ? 1.0f : 0.0f;
	const auto* shadowScene = globals::game::smState->shadowSceneNode[0];
	const auto* nativeLight = shadowScene ? shadowScene->GetRuntimeData().sunShadowDirLight : nullptr;
	data.maps[2].parameters.w = nativeLight ? nativeLight->GetShadowDirectionalLightRuntimeData().endSplitDistances[1] : 0.0f;
	published = data;
	publishedValidMaps = validMaps;
	BindMaps();
}

void LODShadows::BindMaps()
{
	auto* view = depthSRV.get();
	globals::d3d::context->PSSetShaderResources(kDepthSlot, 1, &view);
}

void LODShadows::ReflectionsPrepass() { BindMaps(); }

void LODShadows::CollectScene(bool a_refreshLoaded, bool a_fullWalk)
{
	loadedGeometry.clear();
	if (!a_refreshLoaded && geometryCounts[3] <= visibleGeometry.size())
		loadedGeometry.assign(std::make_move_iterator(visibleGeometry.end() - geometryCounts[3]), std::make_move_iterator(visibleGeometry.end()));
	sceneHash = kHashSeed;
	visibleGeometry.clear();
	{
		RE::BSSpinLockGuard lock(CameraTerrainLock());
		CollectTerrain();
		sourceHashes[0] = sceneHash;
		geometryCounts[0] = uint32_t(visibleGeometry.size());
		sceneHash = kHashSeed;
		Collect(LODObjectRoot().get(), true);
		sourceHashes[1] = sceneHash;
		geometryCounts[1] = uint32_t(visibleGeometry.size()) - geometryCounts[0];
		sceneHash = kHashSeed;
		Collect(LODTreeRoot().get(), true);
		sourceHashes[2] = sceneHash;
		geometryCounts[2] = uint32_t(visibleGeometry.size()) - geometryCounts[0] - geometryCounts[1];
	}
	if (a_refreshLoaded) {
		sceneHash = kHashSeed;
		const auto lodGeometryCount = visibleGeometry.size();
		CollectLoadedObjects(a_fullWalk);
		sourceHashes[3] = sceneHash;
		geometryCounts[3] = uint32_t(visibleGeometry.size() - lodGeometryCount);
	} else {
		visibleGeometry.insert(visibleGeometry.end(), std::make_move_iterator(loadedGeometry.begin()), std::make_move_iterator(loadedGeometry.end()));
		loadedGeometry.clear();
	}
	sceneHash = kHashSeed;
	HashBytes(sceneHash, sourceHashes.data(), sizeof(sourceHashes));
}

void LODShadows::EarlyPrepass()
{
	const double nowMs = Util::GetNowSecs() * 1000.0;
	frameDurationMs = frameTimeMs > 0 ? nowMs - frameTimeMs : 0;
	frameTimeMs = nowMs;
	lastDraws = lastTriangles = 0;
	ResolveTimings();
	if (resetRequested.exchange(false)) {
		Invalidate();
		lastUpdateMs = maxUpdateMs = 0;
		lastRefreshMs = maxRefreshMs = 0;
		timingSamples = completedMaps = 0;
	}
	if (IsMapView()) {
		Publish();
		return;
	}
	if (!ready || !settings.Enabled || !globals::state->HasDirectionalShadows()) {
		if (wasEnabled)
			Invalidate();
		wasEnabled = false;
		Publish();
		return;
	}
	wasEnabled = true;
	try {
		auto manager = ActiveTerrainManager();
		auto scene = globals::game::smState->shadowSceneNode[0];
		auto sun = scene ? scene->GetRuntimeData().sunLight : nullptr;
		auto light = sun ? skyrim_cast<RE::NiDirectionalLight*>(sun->light.get()) : nullptr;
		if (!manager || !manager->worldSpace || LoadScreenUp() || !light) {
			Invalidate();
			Publish();
			return;
		}
		auto direction = light->GetWorldDirection();
		const auto directionLength = direction.Length();
		if (!std::isfinite(directionLength) || directionLength < kMinimumDirectionLength) {
			Invalidate();
			Publish();
			return;
		}
		direction /= directionLength;
		const auto generation = Util::GetCompletedCelestialTransitionGeneration();
		const float clockRate = ShadowClockRate();
		const bool subframeUpdates = clockRate > 1.0f && frameDurationMs * clockRate >= kHistoryIntervalsMs[0];
		const bool refreshImmediately = refreshEveryFrame || subframeUpdates || generation != transitionGeneration;
		const auto worldID = manager->worldSpace->GetFormID();
		if (worldID != worldSpaceID)
			Invalidate();
		worldSpaceID = worldID;
		transitionGeneration = generation;
		const auto* tes = globals::game::tes;
		const int32_t gridX = tes ? tes->currentGridX : 0;
		const int32_t gridY = tes ? tes->currentGridY : 0;
		const bool refreshLoaded = !throttleCollect || refreshImmediately || visibleGeometry.empty() || gridX != collectedGridX ||
		                           gridY != collectedGridY || ++framesSinceCollect >= kCollectIntervalFrames;
		const bool fullWalk = !throttleCollect || refreshImmediately || visibleGeometry.empty() || gridX != collectedGridX ||
		                      gridY != collectedGridY || (refreshLoaded && ++scansSinceFullWalk >= kFullWalkIntervalScans);
		CollectScene(refreshLoaded, fullWalk);
		if (fullWalk)
			scansSinceFullWalk = 0;
		if (refreshLoaded) {
			framesSinceCollect = 0;
			collectedGridX = gridX;
			collectedGridY = gridY;
		}
		UpdateMaps(Util::GetEyePosition(), direction, refreshImmediately);
	} catch (const std::exception& error) {
		logger::error("LOD Shadows disabled after rendering failure: {}", error.what());
		ready = false;
		Invalidate();
	}
	Publish();
}
