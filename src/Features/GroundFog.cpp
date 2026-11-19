#include "GroundFog.h"

#include "Features/CloudShadows.h"
#include "Features/ExponentialHeightFog.h"
#include "Features/TerrainShadows.h"
#include "Features/ViewAtmosphere.h"
#include "Globals.h"
#include "Profiler.h"
#include "State.h"
#include "Util.h"
#include "Utils/DebugSection.h"
#include "Utils/Game.h"
#include "Utils/MathUtils.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.ground_fog."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	GroundFog::Settings,
	Enabled,
	Amount,
	Density,
	Thickness,
	Steepness,
	NoiseScale,
	NoiseStrength,
	BankScale,
	BankStrength,
	FadeStart,
	FadeEnd,
	WindDriftSpeed,
	WindResponse,
	BasinStrength,
	WaterPreference,
	Albedo,
	SunScale,
	SkyAmbientScale,
	SelfShadow,
	GroundFill,
	SurfaceBlendDistance,
	SurfaceMaxHeight,
	HalfResolution,
	StepGrowth,
	MaxSteps,
	FarAmount,
	FarDensity,
	FarThickness,
	FarEnd,
	CloudDarkening,
	VolumeInjection)

namespace
{
	constexpr float kMaxAmount = 4.0f;
	constexpr float kMinDensity = 0.005f;
	constexpr float kMaxDensity = 0.5f;
	constexpr float kMinThickness = 0.5f;
	constexpr float kMaxThickness = 3.0f;
	constexpr float kMaxSteepness = 8.0f;
	constexpr float kMinNoiseScale = 3.0f;
	constexpr float kMaxNoiseScale = 60.0f;
	constexpr float kMinBankScale = 30.0f;
	constexpr float kMaxBankScale = 400.0f;
	constexpr float kMaxBankStrength = 2.0f;
	constexpr float kMaxFadeStart = 500.0f;
	constexpr float kMaxFadeEnd = 1000.0f;
	constexpr float kMinFadeLength = 1.0f;
	constexpr float kMaxWindDriftSpeed = 6.0f;
	constexpr float kMaxBasinStrength = 4.0f;
	constexpr float kMaxWaterPreference = 4.0f;
	constexpr float kMaxLightScale = 4.0f;
	constexpr float kMaxGroundFill = 8.0f;
	constexpr float kMaxSurfaceBlend = 10.0f;
	constexpr float kMaxSurfaceHeight = 4.0f;
	constexpr int kMinSteps = 8;
	constexpr int kMaxSteps = 128;
	constexpr float kMinFarDensity = 0.0002f;
	constexpr float kMaxFarDensity = 0.05f;
	constexpr float kMinFarThickness = 5.0f;
	constexpr float kMaxFarThickness = 80.0f;
	constexpr float kMinFarEnd = 500.0f;
	constexpr float kMaxFarEnd = 12000.0f;
	// Valley layer: the terrain this far around a point (game units) against the point itself; a point this much
	// lower than its surroundings is a full hollow. Wider and deeper than the sheet's, to pick out valleys and lakes.
	constexpr float kFarBasinRadius = 24576.0f;
	constexpr float kFarBasinDepth = 1400.0f;
	constexpr float kMinStepGrowth = 0.1f;
	constexpr float kMaxStepGrowth = 0.5f;

	// Pixel pass march: the smallest terrain detail followed, game units (under one height map texel, so the
	// march walks the map's own texels near the camera). Farther out the detail followed grows with distance by
	// Step Growth and the march walks the texels of coarser copies of the map.
	constexpr float kStepMin = 80.0f;

	// The pattern keeps creeping in still air, so it never freezes.
	constexpr float kMinDriftWind = 0.15f;
	// Wind speeds between which Wind Response clears the fog.
	constexpr float kCalmWind = 0.3f;
	constexpr float kStrongWind = 0.8f;

	// Hollows: the terrain this far around a point (game units) against the point itself; a point this much lower
	// than its surroundings counts as a full hollow.
	constexpr float kBasinRadius = 2048.0f;
	constexpr float kBasinDepth = 350.0f;
	// The fog fades in over one cell from the edge of the height map.
	constexpr float kMapEdgeFade = 4096.0f;

	// Debug views: a 1 m hard-topped slab, dense enough to see its top and where it meets the ground.
	constexpr float kDebugThickness = 1.0f;
	constexpr float kDebugDensity = 0.25f;
}

void GroundFog::SetupResources()
{
	volumeConstants = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<VolumeData>(), "GroundFog::VolumeData");
}

GroundFog::Status GroundFog::Update(VolumeData& a_data, ID3D11ShaderResourceView*& a_heightMap)
{
	effectiveAmount = 0.0f;
	if (!settings.Enabled)
		return Status::kDisabled;
	if (Util::IsInterior())
		return Status::kInterior;
	if (auto water = RE::TESWaterSystem::GetSingleton(); water && water->playerUnderwater)
		return Status::kUnderwater;

	auto& terrain = globals::features::terrainShadows;
	TerrainShadows::HeightMapBinding map;
	heightMapFormat = DXGI_FORMAT_UNKNOWN;
	if (!terrain.loaded || !terrain.GetHeightMapBinding(map))
		return Status::kNoHeightMap;
	heightMapFormat = map.format;
	heightMapSize = { static_cast<float>(map.width), static_cast<float>(map.height) };
	// An 8-bit (BC4) map steps by tens of meters, far more than the layer is thick.
	if (map.format != DXGI_FORMAT_R16_UNORM)
		return Status::kHeightMapNot16Bit;

	const float unitsPerMeter = 1.0f / Util::Units::GAME_UNIT_TO_M;
	float2 windDirection{};
	windSpeed = 0.0f;
	if (const auto sky = globals::game::sky) {
		windSpeed = std::clamp(std::isfinite(sky->windSpeed) ? sky->windSpeed : 0.0f, 0.0f, 1.0f);
		// Same convention as Tree Wind, so fog drifts the way the trees lean.
		if (std::isfinite(sky->windAngle))
			windDirection = { std::sin(sky->windAngle), std::cos(sky->windAngle) };
	}
	if (lastUpdateFrame != globals::state->frameCount) {
		lastUpdateFrame = globals::state->frameCount;
		const float deltaTime = std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, 0.1f);
		const float speed = settings.WindDriftSpeed * unitsPerMeter * std::max(windSpeed, kMinDriftWind);
		windTravelX += static_cast<double>(windDirection.x) * speed * deltaTime;
		windTravelY += static_cast<double>(windDirection.y) * speed * deltaTime;
	}

	const float windFactor = 1.0f - settings.WindResponse * Util::Smoothstep(kCalmWind, kStrongWind, windSpeed);
	effectiveAmount = settings.Amount * windFactor;
	const bool debug = debugMode != DebugMode::kOff;
	const bool slab = debugMode == DebugMode::kSlab || debugMode == DebugMode::kGroundHeight;
	// The valley layer exists in the pixel pass only.
	effectiveFarAmount = settings.VolumeInjection || slab ? 0.0f : settings.FarAmount * windFactor;
	if (!debug && effectiveAmount * settings.Density <= 0.0f && effectiveFarAmount * settings.FarDensity <= 0.0f)
		return Status::kNoAmount;

	const float thickness = (slab ? kDebugThickness : std::max(settings.Thickness, 0.1f)) * unitsPerMeter;
	const float density = debug ? std::max(effectiveAmount * settings.Density, kDebugDensity) : effectiveAmount * settings.Density;
	const float fadeStart = settings.FadeStart * unitsPerMeter;

	a_data = {};
	a_data.MapScaleOffset = map.scaleOffset;
	a_data.MapZ = map.zBlackWhite;
	a_data.MapEdgeFade = { 1.0f / (kMapEdgeFade * std::abs(map.scaleOffset.x)), 1.0f / (kMapEdgeFade * std::abs(map.scaleOffset.y)) };
	a_data.Thickness = thickness;
	a_data.Steepness = slab ? 0.0f : settings.Steepness;
	a_data.GroundExtinction = density * Util::Units::GAME_UNIT_TO_M;  // per meter -> per game unit
	a_data.GroundFill = slab ? 0.0f : settings.GroundFill * thickness;
	a_data.FadeStart = fadeStart;
	a_data.FadeEnd = std::max(settings.FadeEnd * unitsPerMeter, fadeStart + kMinFadeLength * unitsPerMeter);
	a_data.WindTravel = { static_cast<float>(windTravelX), static_cast<float>(windTravelY) };
	a_data.InvNoiseScale = 1.0f / (std::max(settings.NoiseScale, kMinNoiseScale) * unitsPerMeter);
	a_data.NoiseStrength = slab ? 0.0f : settings.NoiseStrength;
	a_data.InvBankScale = 1.0f / (std::max(settings.BankScale, kMinBankScale) * unitsPerMeter);
	a_data.BankStrength = slab ? 0.0f : settings.BankStrength;
	a_data.BasinStrength = slab ? 0.0f : settings.BasinStrength;
	a_data.BasinRadius = kBasinRadius;
	a_data.InvBasinDepth = 1.0f / kBasinDepth;
	a_data.WaterPreference = slab ? 1.0f : settings.WaterPreference;
	a_data.Albedo = settings.Albedo;
	a_data.SunScale = settings.SunScale;
	a_data.SkyAmbientScale = settings.SkyAmbientScale;
	a_data.SelfShadow = settings.SelfShadow;
	a_data.Flags = 1u | (settings.HalfResolution ? 2u : 0u);
	a_data.DebugMode = static_cast<uint32_t>(debugMode);
	a_data.SurfaceBlend = slab ? 0.0f : settings.SurfaceBlendDistance * unitsPerMeter;
	a_data.SurfaceLift = settings.SurfaceMaxHeight * unitsPerMeter;
	a_data.StepMin = kStepMin;
	a_data.StepGrowth = std::clamp(settings.StepGrowth, kMinStepGrowth, kMaxStepGrowth);
	a_data.MaxSteps = static_cast<uint32_t>(std::clamp(settings.MaxSteps, kMinSteps, kMaxSteps));
	const float waterTop = globals::state->waterGridTop;
	a_data.WaterTop = std::isfinite(waterTop) ? std::max(waterTop, -1.0e30f) : -1.0e30f;
	a_data.MapTexel = 1.0f / std::max(std::abs(map.scaleOffset.x) * static_cast<float>(map.width), 1.0e-12f);
	a_data.MapSize = heightMapSize;
	a_data.FarExtinction = effectiveFarAmount * settings.FarDensity * Util::Units::GAME_UNIT_TO_M;
	a_data.FarThickness = settings.FarThickness * unitsPerMeter;
	a_data.FarEnd = settings.FarEnd * unitsPerMeter;
	a_data.FarBasinRadius = kFarBasinRadius;
	a_data.InvFarBasinDepth = 1.0f / kFarBasinDepth;
	a_data.CloudDarkening = settings.CloudDarkening;
	a_heightMap = map.srv;
	return Status::kActive;
}

bool GroundFog::BindForVolume(ID3D11DeviceContext* a_context)
{
	// The pixel pass draws the layer unless the volume is asked to; never both.
	if (!settings.VolumeInjection)
		return false;
	VolumeData data{};
	ID3D11ShaderResourceView* heightMap = nullptr;
	lastBindFrame = globals::state->frameCount;
	status = loaded && volumeConstants ? Update(data, heightMap) : Status::kDisabled;
	if (status != Status::kActive)
		return false;

	volumeConstants->Update(data);
	// The slots are free in the volume's own shader, but another pass may have left something in them.
	previousConstants = nullptr;
	previousHeightMap = nullptr;
	a_context->CSGetConstantBuffers(kConstantSlot, 1, previousConstants.put());
	a_context->CSGetShaderResources(kHeightMapSlot, 1, previousHeightMap.put());
	auto* constants = volumeConstants->CB();
	a_context->CSSetConstantBuffers(kConstantSlot, 1, &constants);
	a_context->CSSetShaderResources(kHeightMapSlot, 1, &heightMap);
	bound = true;
	return true;
}

void GroundFog::UnbindForVolume(ID3D11DeviceContext* a_context)
{
	if (!bound)
		return;
	auto* constants = previousConstants.get();
	auto* heightMap = previousHeightMap.get();
	a_context->CSSetConstantBuffers(kConstantSlot, 1, &constants);
	a_context->CSSetShaderResources(kHeightMapSlot, 1, &heightMap);
	previousConstants = nullptr;
	previousHeightMap = nullptr;
	bound = false;
}

void GroundFog::ClearShaderCache()
{
	pixelCS = nullptr;
	pyramidCS = nullptr;
	pyramidFirstCS = nullptr;
	shadersRequested = false;
}

bool GroundFog::EnsureShaders()
{
	if (!shadersRequested) {
		shadersRequested = true;
		// The lighting is the shared volume's, so the same features decide what it is compiled with.
		std::vector<std::pair<const char*, const char*>> defines{ { "PHYSICAL_SKY", "" } };
		if (globals::features::exponentialHeightFog.loaded)
			defines.emplace_back("EXP_HEIGHT_FOG", "");
		if (globals::features::terrainShadows.loaded)
			defines.emplace_back("TERRAIN_SHADOWS", "");
		if (globals::features::cloudShadows.loaded)
			defines.emplace_back("CLOUD_SHADOWS", "");
		pixelCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\GroundFog\\GroundFogPixel.cs.hlsl", defines, "cs_5_0")));
		pyramidCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\GroundFog\\HeightPyramid.cs.hlsl", {}, "cs_5_0")));
		pyramidFirstCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\GroundFog\\HeightPyramid.cs.hlsl", { { "FIRST_LEVEL", "" } }, "cs_5_0")));
	}
	return pixelCS && pyramidCS && pyramidFirstCS;
}

bool GroundFog::EnsureHeightPyramid(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_heightMap)
{
	if (heightPyramid && heightPyramidSource.get() == a_heightMap)
		return true;
	heightPyramid = nullptr;
	heightPyramidSource = nullptr;
	heightPyramidLevels = 0;

	constexpr DXGI_FORMAT format = DXGI_FORMAT_R16G16_UNORM;
	constexpr UINT needed = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE | D3D11_FORMAT_SUPPORT_MIP | D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
	UINT support = 0;
	auto* device = globals::d3d::device;
	if (FAILED(device->CheckFormatSupport(format, &support)) || (support & needed) != needed)
		return false;
	winrt::com_ptr<ID3D11Resource> mapResource;
	a_heightMap->GetResource(mapResource.put());
	winrt::com_ptr<ID3D11Texture2D> mapTexture;
	if (!mapResource || !mapResource.try_as(mapTexture))
		return false;
	D3D11_TEXTURE2D_DESC mapDesc{};
	mapTexture->GetDesc(&mapDesc);

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = std::max(mapDesc.Width / 2u, 1u);
	desc.Height = std::max(mapDesc.Height / 2u, 1u);
	desc.MipLevels = static_cast<UINT>(std::bit_width(std::max(desc.Width, desc.Height)));
	desc.ArraySize = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	auto pyramid = eastl::make_unique<Texture2D>(desc, "GroundFog::HeightPyramid");
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = desc.MipLevels;
	pyramid->CreateSRV(srvDesc);

	// Each level is written from the one before it; the first from the height map itself.
	globals::profiler->BeginPass("GroundFog::HeightPyramid");
	ID3D11UnorderedAccessView* nullUav = nullptr;
	ID3D11ShaderResourceView* nullSrv = nullptr;
	winrt::com_ptr<ID3D11ShaderResourceView> previousLevel;
	bool built = true;
	for (UINT level = 0; level < desc.MipLevels; ++level) {
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = level;
		winrt::com_ptr<ID3D11UnorderedAccessView> target;
		D3D11_SHADER_RESOURCE_VIEW_DESC levelDesc = srvDesc;
		levelDesc.Texture2D.MostDetailedMip = level;
		levelDesc.Texture2D.MipLevels = 1;
		winrt::com_ptr<ID3D11ShaderResourceView> thisLevel;
		if (FAILED(device->CreateUnorderedAccessView(pyramid->resource.get(), &uavDesc, target.put())) ||
			FAILED(device->CreateShaderResourceView(pyramid->resource.get(), &levelDesc, thisLevel.put()))) {
			built = false;
			break;
		}
		auto* source = level == 0 ? a_heightMap : previousLevel.get();
		auto* uav = target.get();
		a_context->CSSetShaderResources(0, 1, &source);
		a_context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		a_context->CSSetShader(level == 0 ? pyramidFirstCS.get() : pyramidCS.get(), nullptr, 0);
		a_context->Dispatch((std::max(desc.Width >> level, 1u) + 7u) / 8u, (std::max(desc.Height >> level, 1u) + 7u) / 8u, 1);
		a_context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
		a_context->CSSetShaderResources(0, 1, &nullSrv);
		previousLevel = thisLevel;
	}
	globals::profiler->EndPass();
	if (!built)
		return false;

	heightPyramid = std::move(pyramid);
	heightPyramidSource.copy_from(a_heightMap);
	heightPyramidLevels = desc.MipLevels;
	return true;
}

bool GroundFog::EnsureTargets(uint32_t a_width, uint32_t a_height)
{
	if (fogTexture && fogDepthTexture && fogTexture->desc.Width == a_width && fogTexture->desc.Height == a_height)
		return true;
	fogTexture = nullptr;
	fogDepthTexture = nullptr;
	auto make = [&](DXGI_FORMAT a_format, const char* a_name) {
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_width;
		desc.Height = a_height;
		desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
		desc.Format = a_format;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		auto texture = eastl::make_unique<Texture2D>(desc, a_name);
		D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = a_format;
		srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv.Texture2D.MipLevels = 1;
		texture->CreateSRV(srv);
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = a_format;
		uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		texture->CreateUAV(uav);
		return texture;
	};
	fogTexture = make(DXGI_FORMAT_R16G16B16A16_FLOAT, "GroundFog::Fog");
	fogDepthTexture = make(DXGI_FORMAT_R32_FLOAT, "GroundFog::FogDepth");
	return true;
}

bool GroundFog::RenderPixel(ID3D11DeviceContext* a_context)
{
	// Whatever happens below, the composite must not read a stale fog texture.
	static_assert(kCompositeDepthSlot == kCompositeFogSlot + 1 && kHeightPyramidSlot == kHeightMapSlot + 1);
	ID3D11ShaderResourceView* compositeInputs[2]{};
	a_context->CSSetShaderResources(kCompositeFogSlot, 2, compositeInputs);
	if (!loaded || !volumeConstants || settings.VolumeInjection)
		return false;

	// Everything that can rule the pass out is decided here on the CPU: disabled, interior, underwater, Amount 0
	// (after wind), no 16-bit height map. Nothing is dispatched then.
	VolumeData data{};
	ID3D11ShaderResourceView* heightMap = nullptr;
	lastBindFrame = globals::state->frameCount;
	status = Update(data, heightMap);
	if (status != Status::kActive)
		return false;
	// The lighting reads the shared volume's inputs and flags of this frame.
	auto& atmosphere = globals::features::viewAtmosphere;
	if (!atmosphere.IsReady()) {
		status = Status::kNoVolume;
		return false;
	}
	auto& main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	if (!main.texture || !EnsureShaders() || !EnsureHeightPyramid(a_context, heightMap)) {
		status = Status::kNoResources;
		return false;
	}
	D3D11_TEXTURE2D_DESC mainDesc{};
	main.texture->GetDesc(&mainDesc);
	const uint32_t divisor = settings.HalfResolution ? 2u : 1u;
	if (!EnsureTargets((mainDesc.Width + divisor - 1u) / divisor, (mainDesc.Height + divisor - 1u) / divisor)) {
		status = Status::kNoResources;
		return false;
	}

	data.PyramidSize = { static_cast<float>(heightPyramid->desc.Width), static_cast<float>(heightPyramid->desc.Height) };
	data.PyramidLevels = static_cast<float>(heightPyramidLevels);
	volumeConstants->Update(data);

	winrt::com_ptr<ID3D11Buffer> oldConstants;
	a_context->CSGetConstantBuffers(kConstantSlot, 1, oldConstants.put());
	auto* constants = volumeConstants->CB();
	a_context->CSSetConstantBuffers(kConstantSlot, 1, &constants);
	ID3D11ShaderResourceView* ground[] = { heightMap, heightPyramid->srv.get() };
	a_context->CSSetShaderResources(kHeightMapSlot, 2, ground);
	atmosphere.BindLightingInputs(a_context);
	ID3D11UnorderedAccessView* outputs[] = { fogTexture->uav.get(), fogDepthTexture->uav.get() };
	a_context->CSSetUnorderedAccessViews(0, 2, outputs, nullptr);
	a_context->CSSetShader(pixelCS.get(), nullptr, 0);

	// The same active area as the composite under dynamic resolution.
	const auto scale = globals::game::frameBufferCached.GetDynamicResolutionParams1();
	const uint32_t width = (static_cast<uint32_t>(std::clamp(mainDesc.Width * scale.x, 1.0f, static_cast<float>(mainDesc.Width))) + divisor - 1u) / divisor;
	const uint32_t height = (static_cast<uint32_t>(std::clamp(mainDesc.Height * scale.y, 1.0f, static_cast<float>(mainDesc.Height))) + divisor - 1u) / divisor;
	globals::profiler->BeginPass("GroundFog::Pixel");
	a_context->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1);
	globals::profiler->EndPass();

	ID3D11UnorderedAccessView* nullOutputs[2]{};
	a_context->CSSetUnorderedAccessViews(0, 2, nullOutputs, nullptr);
	ID3D11ShaderResourceView* nullGround[2]{};
	a_context->CSSetShaderResources(kHeightMapSlot, 2, nullGround);
	auto* previous = oldConstants.get();
	a_context->CSSetConstantBuffers(kConstantSlot, 1, &previous);
	compositeInputs[0] = fogTexture->srv.get();
	compositeInputs[1] = fogDepthTexture->srv.get();
	a_context->CSSetShaderResources(kCompositeFogSlot, 2, compositeInputs);
	return true;
}

void GroundFog::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	Util::HoverTip(T(TKEY("enabled_tooltip"), "A thin sheet of fog on the ground, outdoors. Needs Atmosphere and Fog with Physical Sky running, and a 16-bit terrain height map for the worldspace."));

	ImGui::SliderFloat(T(TKEY("amount"), "Amount"), &settings.Amount, 0.0f, kMaxAmount, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	Util::HoverTip(T(TKEY("amount_tooltip"), "How much fog there is. This is the control to drive by weather, time of day and location. 0 removes it."));

	ImGui::SliderFloat(T(TKEY("density"), "Density"), &settings.Density, kMinDensity, kMaxDensity, "%.3f /m", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
	Util::HoverTip(T(TKEY("density_tooltip"), "How opaque the fog is at the ground when Amount is 1. At 0.06 you see about 50 m through it."));

	if (ImGui::TreeNodeEx(T(TKEY("shape"), "Shape"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T(TKEY("thickness"), "Thickness"), &settings.Thickness, kMinThickness, kMaxThickness, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("thickness_tooltip"), "Height of the tallest mounds above the ground. With Mounds at 1 the fog averages under half of this."));

		ImGui::SliderFloat(T(TKEY("steepness"), "Thinning with Height"), &settings.Steepness, 0.0f, kMaxSteepness, "%.1f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("steepness_tooltip"), "How quickly the fog thins from the ground up to its top. 0 is an even slab with a hard top."));

		ImGui::SliderFloat(T(TKEY("noise_strength"), "Mounds"), &settings.NoiseStrength, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("noise_strength_tooltip"), "How uneven the top is. 0 is a level sheet at the full thickness."));

		ImGui::SliderFloat(T(TKEY("noise_scale"), "Mound Size"), &settings.NoiseScale, kMinNoiseScale, kMaxNoiseScale, "%.0f m", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("noise_scale_tooltip"), "Width of the mounds and the gaps between them."));

		ImGui::SliderFloat(T(TKEY("bank_strength"), "Banks"), &settings.BankStrength, 0.0f, kMaxBankStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("bank_strength_tooltip"), "How much the fog thickens and thins over long distances. High values with a low Amount leave separate patches."));

		ImGui::SliderFloat(T(TKEY("bank_scale"), "Bank Size"), &settings.BankScale, kMinBankScale, kMaxBankScale, "%.0f m", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("bank_scale_tooltip"), "Width of the banks."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("placement"), "Placement"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T(TKEY("basin_strength"), "Hollows"), &settings.BasinStrength, 0.0f, kMaxBasinStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("basin_strength_tooltip"), "Gathers fog in dips and thins it on rises. At 1 a hollow holds twice as much and a rise holds none."));

		ImGui::SliderFloat(T(TKEY("water_preference"), "Over Water"), &settings.WaterPreference, 0.0f, kMaxWaterPreference, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("water_preference_tooltip"), "Amount over water against land. 0 keeps water clear. Water is only known near you, so distant lakes get none."));

		ImGui::SliderFloat(T(TKEY("fade_start"), "Fade Start"), &settings.FadeStart, 0.0f, kMaxFadeStart, "%.0f m", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("fade_start_tooltip"), "Distance at which the fog starts to thin out."));

		ImGui::SliderFloat(T(TKEY("fade_end"), "Fade End"), &settings.FadeEnd, 0.0f, kMaxFadeEnd, "%.0f m", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("fade_end_tooltip"), "Distance beyond which there is none. Longer distances cost more steps per pixel near the horizon."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("far"), "Valleys"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T(TKEY("far_amount"), "Valley Amount"), &settings.FarAmount, 0.0f, kMaxAmount, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("far_amount_tooltip"), "Deeper fog that pools in valleys and over lakes beyond the sheet, out to Valley Distance. It takes over where the sheet fades out. 0 removes it and its cost."));

		ImGui::SliderFloat(T(TKEY("far_density"), "Valley Density"), &settings.FarDensity, kMinFarDensity, kMaxFarDensity, "%.4f /m", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
		Util::HoverTip(T(TKEY("far_density_tooltip"), "How opaque the valley fog is at the ground when Valley Amount is 1. At 0.003 you see about 1 km through it."));

		ImGui::SliderFloat(T(TKEY("far_thickness"), "Valley Thickness"), &settings.FarThickness, kMinFarThickness, kMaxFarThickness, "%.0f m", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("far_thickness_tooltip"), "Depth of the valley fog in the deepest hollows. Shallow hollows hold about a third of it."));

		ImGui::SliderFloat(T(TKEY("far_end"), "Valley Distance"), &settings.FarEnd, kMinFarEnd, kMaxFarEnd, "%.0f m", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("far_end_tooltip"), "Distance beyond which there is no valley fog. It thins out over the last quarter."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("wind"), "Wind"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T(TKEY("wind_drift_speed"), "Drift Speed"), &settings.WindDriftSpeed, 0.0f, kMaxWindDriftSpeed, "%.2f m/s", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("wind_drift_speed_tooltip"), "How fast the mounds and banks travel in a full wind. They creep a little in still air. 0 holds them in place."));

		ImGui::SliderFloat(T(TKEY("wind_response"), "Cleared by Wind"), &settings.WindResponse, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("wind_response_tooltip"), "Share of the fog a strong wind blows away."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("lighting"), "Lighting"))) {
		ImGui::SliderFloat(T(TKEY("albedo"), "Brightness"), &settings.Albedo, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("albedo_tooltip"), "Share of the light the fog scatters instead of absorbing."));

		ImGui::SliderFloat(T(TKEY("sun_scale"), "Sun and Moon Light"), &settings.SunScale, 0.0f, kMaxLightScale, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("sun_scale_tooltip"), "Scales the light the fog takes from the sun and moons. 1 matches the haze."));

		ImGui::SliderFloat(T(TKEY("sky_ambient_scale"), "Sky Light"), &settings.SkyAmbientScale, 0.0f, kMaxLightScale, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("sky_ambient_scale_tooltip"), "Scales the light the fog takes from the whole sky, which is what lights it in shadow."));

		ImGui::SliderFloat(T(TKEY("cloud_darkening"), "Darkening under Cloud"), &settings.CloudDarkening, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("cloud_darkening_tooltip"), "How much the cloud over the fog dims the sky light it takes, so it darkens under overcast and storm instead of keeping the brightness of the average sky. 0 lights it by the whole sky everywhere."));

		ImGui::SliderFloat(T(TKEY("self_shadow"), "Self Shadowing"), &settings.SelfShadow, 0.0f, kMaxLightScale, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("self_shadow_tooltip"), "How much the fog above dims the fog below at a low sun."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("surfaces"), "Surfaces"))) {
		ImGui::SliderFloat(T(TKEY("surface_blend_distance"), "Surface Wrap Distance"), &settings.SurfaceBlendDistance, 0.0f, kMaxSurfaceBlend, "%.1f m", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("surface_blend_distance_tooltip"), "Over this last stretch of each view ray the fog settles onto what you see instead of the mapped terrain, so it lies on rocks, roads and ground meshes and thins out against them. 0 uses the terrain map alone."));

		ImGui::SliderFloat(T(TKEY("surface_max_height"), "Surface Max Height"), &settings.SurfaceMaxHeight, 0.0f, kMaxSurfaceHeight, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("surface_max_height_tooltip"), "Surfaces up to this far above the mapped terrain count as ground; above twice this they do not, so roofs, bridges and tree crowns stay clear. Higher values let fog climb walls and trunks."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("advanced"), "Advanced"))) {
		ImGui::Checkbox(T(TKEY("half_resolution"), "Half Resolution"), &settings.HalfResolution);
		Util::HoverTip(T(TKEY("half_resolution_tooltip"), "Computes the fog for every second pixel in each direction and fills in the rest by depth. A quarter of the cost; turn off to compare."));

		ImGui::SliderFloat(T(TKEY("step_growth"), "Step Growth"), &settings.StepGrowth, kMinStepGrowth, kMaxStepGrowth, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("step_growth_tooltip"), "Size of the terrain detail the fog still follows, as a share of the distance. Lower follows distant ground more closely and costs more near the horizon."));

		ImGui::SliderInt(T(TKEY("max_steps"), "Max Steps"), &settings.MaxSteps, kMinSteps, kMaxSteps, "%d", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("max_steps_tooltip"), "Most ground reads along a view ray. Rays that need more lose their farthest fog; the Tap Count debug view shows them red."));

		ImGui::Checkbox(T(TKEY("volume_injection"), "Draw Through Atmosphere Volume"), &settings.VolumeInjection);
		Util::HoverTip(T(TKEY("volume_injection_tooltip"), "The first version: adds the fog to the shared atmosphere volume instead of drawing it per pixel. Coarse in the distance and it swims as the camera turns. For comparison."));

		ImGui::SliderFloat(T(TKEY("ground_fill"), "Fill Below Ground"), &settings.GroundFill, 0.0f, kMaxGroundFill, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("ground_fill_tooltip"), "Depth under the mapped ground that still holds fog, as a share of Thickness. Covers places where the real ground lies a little below the height map."));
		ImGui::TreePop();
	}
}

void GroundFog::DrawDebugSection()
{
	Util::DebugSection section;
	if (!section)
		return;

	const char* modes[] = {
		T(TKEY("debug_mode_off"), "Off"),
		T(TKEY("debug_mode_slab"), "Slab"),
		T(TKEY("debug_mode_ground_height"), "Ground Height"),
		T(TKEY("debug_mode_layer_mask"), "Layer Mask"),
		T(TKEY("debug_mode_tap_count"), "Tap Count")
	};
	int mode = static_cast<int>(debugMode);
	if (ImGui::Combo(T(TKEY("debug_mode"), "Debug View"), &mode, modes, static_cast<int>(std::size(modes))))
		debugMode = static_cast<DebugMode>(std::clamp(mode, 0, static_cast<int>(std::size(modes)) - 1));
	Util::HoverTip(T(TKEY("debug_mode_tooltip"), "Slab: a dense, level-topped 1 m layer with no noise, to check the height map against the ground.\nGround Height: the slab colored by the sampled height, one stripe per 64 units.\nLayer Mask: the real fog, dense, green where it is the sheet, red where it is valley fog, blue over water.\nTap Count: texture reads per pixel of the per-pixel pass. Blue is 4 or fewer, green 34, yellow 64, red 94 or more."));

	// Atmosphere and Fog asks for the layer every frame it composes (or, with volume injection, fills its volume).
	const bool volumeRunning = lastBindFrame != UINT32_MAX && globals::state->frameCount - lastBindFrame <= 2u;
	const char* state = T(TKEY("status_no_volume"), "The shared atmosphere volume is not running");
	if (volumeRunning) {
		switch (status) {
		case Status::kActive:
			state = T(TKEY("status_active"), "Active");
			break;
		case Status::kDisabled:
			state = T(TKEY("status_disabled"), "Disabled");
			break;
		case Status::kNoAmount:
			state = T(TKEY("status_no_amount"), "Amount is zero");
			break;
		case Status::kInterior:
			state = T(TKEY("status_interior"), "Interior");
			break;
		case Status::kUnderwater:
			state = T(TKEY("status_underwater"), "Underwater");
			break;
		case Status::kNoHeightMap:
			state = T(TKEY("status_no_height_map"), "No terrain height map for this worldspace");
			break;
		case Status::kHeightMapNot16Bit:
			state = T(TKEY("status_not_16_bit"), "The terrain height map is not 16-bit");
			break;
		case Status::kNoVolume:
			break;
		case Status::kNoResources:
			state = T(TKEY("status_no_resources"), "Shaders or textures could not be created");
			break;
		default:
			break;
		}
	}
	ImGui::Text("%s: %s", T(TKEY("debug_status"), "Status"), state);
	ImGui::Text("%s: %s", T(TKEY("debug_path"), "Drawn"), settings.VolumeInjection ? T(TKEY("debug_path_volume"), "through the atmosphere volume") : (settings.HalfResolution ? T(TKEY("debug_path_pixel_half"), "per pixel, half resolution") : T(TKEY("debug_path_pixel_full"), "per pixel, full resolution")));
	ImGui::Text("%s: %.3f", T(TKEY("debug_effective_amount"), "Amount after wind"), effectiveAmount);
	ImGui::Text("%s: %.3f", T(TKEY("debug_effective_far_amount"), "Valley amount after wind"), effectiveFarAmount);
	ImGui::Text("%s: %.2f", T(TKEY("debug_wind"), "Wind speed"), windSpeed);
	ImGui::Text("%s: %.0f x %.0f, DXGI %d", T(TKEY("debug_height_map"), "Height map"), heightMapSize.x, heightMapSize.y, static_cast<int>(heightMapFormat));
}

void GroundFog::LoadSettings(json& o_json)
{
	settings = o_json;
	const Settings defaults{};
	auto sanitize = [](float& a_value, float a_default, float a_min, float a_max) {
		a_value = std::clamp(std::isfinite(a_value) ? a_value : a_default, a_min, a_max);
	};
	sanitize(settings.Amount, defaults.Amount, 0.0f, kMaxAmount);
	sanitize(settings.Density, defaults.Density, kMinDensity, kMaxDensity);
	sanitize(settings.Thickness, defaults.Thickness, kMinThickness, kMaxThickness);
	sanitize(settings.Steepness, defaults.Steepness, 0.0f, kMaxSteepness);
	sanitize(settings.NoiseScale, defaults.NoiseScale, kMinNoiseScale, kMaxNoiseScale);
	sanitize(settings.NoiseStrength, defaults.NoiseStrength, 0.0f, 1.0f);
	sanitize(settings.BankScale, defaults.BankScale, kMinBankScale, kMaxBankScale);
	sanitize(settings.BankStrength, defaults.BankStrength, 0.0f, kMaxBankStrength);
	sanitize(settings.FadeStart, defaults.FadeStart, 0.0f, kMaxFadeStart);
	sanitize(settings.FadeEnd, defaults.FadeEnd, 0.0f, kMaxFadeEnd);
	sanitize(settings.WindDriftSpeed, defaults.WindDriftSpeed, 0.0f, kMaxWindDriftSpeed);
	sanitize(settings.WindResponse, defaults.WindResponse, 0.0f, 1.0f);
	sanitize(settings.BasinStrength, defaults.BasinStrength, 0.0f, kMaxBasinStrength);
	sanitize(settings.WaterPreference, defaults.WaterPreference, 0.0f, kMaxWaterPreference);
	sanitize(settings.Albedo, defaults.Albedo, 0.0f, 1.0f);
	sanitize(settings.SunScale, defaults.SunScale, 0.0f, kMaxLightScale);
	sanitize(settings.SkyAmbientScale, defaults.SkyAmbientScale, 0.0f, kMaxLightScale);
	sanitize(settings.SelfShadow, defaults.SelfShadow, 0.0f, kMaxLightScale);
	sanitize(settings.GroundFill, defaults.GroundFill, 0.0f, kMaxGroundFill);
	sanitize(settings.SurfaceBlendDistance, defaults.SurfaceBlendDistance, 0.0f, kMaxSurfaceBlend);
	sanitize(settings.SurfaceMaxHeight, defaults.SurfaceMaxHeight, 0.0f, kMaxSurfaceHeight);
	sanitize(settings.StepGrowth, defaults.StepGrowth, kMinStepGrowth, kMaxStepGrowth);
	sanitize(settings.FarAmount, defaults.FarAmount, 0.0f, kMaxAmount);
	sanitize(settings.FarDensity, defaults.FarDensity, kMinFarDensity, kMaxFarDensity);
	sanitize(settings.FarThickness, defaults.FarThickness, kMinFarThickness, kMaxFarThickness);
	sanitize(settings.FarEnd, defaults.FarEnd, kMinFarEnd, kMaxFarEnd);
	sanitize(settings.CloudDarkening, defaults.CloudDarkening, 0.0f, 1.0f);
	settings.MaxSteps = std::clamp(settings.MaxSteps, kMinSteps, kMaxSteps);
}

void GroundFog::SaveSettings(json& o_json)
{
	o_json = settings;
}

void GroundFog::RestoreDefaultSettings()
{
	settings = {};
}

#undef I18N_KEY_PREFIX
