#include "LightShafts.h"

#include "Features/PhysicalSky.h"
#include "Features/SkySync.h"
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

#define I18N_KEY_PREFIX "feature.light_shafts."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	LightShafts::Settings,
	Enabled,
	SunStrength,
	MoonStrength,
	Density,
	Length,
	Tightness,
	CloudOcclusion,
	MaxBrightness,
	Quality,
	DebugView,
	DistantStrength,
	DistantLength,
	HighSunStrength,
	VeilRemoval,
	SilhouetteRays)

namespace
{
	constexpr float kMaxStrength = 1.0f;
	constexpr float kMaxDensity = 8.0f;
	constexpr float kMinLength = 5.0f;
	constexpr float kMaxLength = 300.0f;
	constexpr float kMinMaxBrightness = 0.01f;
	constexpr float kMaxMaxBrightness = 10.0f;
	constexpr float kMaxDistantStrength = 4.0f;
	constexpr float kMinDistantLength = 200.0f;
	constexpr float kMaxDistantLength = 6000.0f;
	constexpr int kQualityCount = static_cast<int>(std::size(LightShafts::kQualities));
	constexpr int kDebugViewCount = 3;

	// The near march ends where its weight has fallen to 5 %; the far one runs three of its lengths further.
	constexpr float kNearRangeLengths = 3.0f;
	constexpr float kFarRangeLengths = 3.0f;
	// Anisotropy of the phase function at Tightness 1.
	constexpr float kMaxPhaseG = 0.9f;
	// The lit share of the air around the camera: taken within this radius and height above the eye (meters) and
	// eased over this time (seconds).
	constexpr float kAmbientRadius = 12.0f;
	constexpr float kAmbientHeight = 3.0f;
	constexpr float kAmbientEaseTime = 0.6f;
	// Least distance to the sides over which the far march compares the terrain shadow (meters).
	constexpr float kRidgeRadius = 40.0f;
	// The light fades in as it clears the horizon, between these sines of its elevation.
	constexpr float kHorizonFadeStart = -0.05f;
	constexpr float kHorizonFadeEnd = 0.01f;
	// High Sun Strength applies in full above the second elevation and not at all below the first (sines of 25 and
	// 60 degrees).
	constexpr float kHighSunStart = 0.4226f;
	constexpr float kHighSunEnd = 0.8660f;
	// The beams are at full strength while the light is within 65 degrees of the view centre and off beyond 115
	// (cosines).
	constexpr float kViewFadeFull = 0.4226f;
	constexpr float kViewFadeOff = -0.4226f;
	// The distant rays come from the screen while the light is on it or less than an eighth of the screen outside,
	// and from the world once it is more than five eighths outside (in units of half the screen: 1 is the edge).
	constexpr float kScreenFull = 1.25f;
	constexpr float kScreenOff = 2.25f;
	constexpr uint32_t kMaskDivisor = 4u;
	// Below this added luminance straight toward the light the pass is not worth running.
	constexpr float kMinLuminance = 1.0e-5f;

	float Luminance(const float3& a_color)
	{
		return a_color.x * 0.2126f + a_color.y * 0.7152f + a_color.z * 0.0722f;
	}
}

LightShafts::Status LightShafts::Update(ShaftData& a_data)
{
	effectiveStrength = 0.0f;
	if (!settings.Enabled)
		return Status::kDisabled;
	if (settings.Density <= 0.0f || (settings.SunStrength <= 0.0f && settings.MoonStrength <= 0.0f))
		return Status::kNoStrength;
	if (Util::IsInterior())
		return Status::kInterior;
	if (auto water = RE::TESWaterSystem::GetSingleton(); water && water->playerUnderwater)
		return Status::kUnderwater;
	const auto& sky = globals::features::physicalSky;
	if (!sky.loaded || !sky.cbData.enabled || !sky.texTrLut)
		return Status::kNoSky;
	const auto& atmosphere = globals::features::viewAtmosphere;
	if (!atmosphere.IsReady())
		return Status::kNoVolume;

	// The body the scene's directional light, and with it every shadow read here, belongs to. While the light
	// swings from one body to the next its colour goes with it.
	const auto blend = globals::features::skySync.GetDirectionalLightBlend();
	auto bodyColor = [&](SkySync::Caster a_body) -> float3 {
		switch (a_body) {
		case SkySync::Caster::Sun:
			return sky.cbData.sunlightColor * settings.SunStrength;
		case SkySync::Caster::Masser:
			return sky.cbData.masserColor * settings.MoonStrength;
		case SkySync::Caster::Secunda:
			return sky.cbData.secundaColor * settings.MoonStrength;
		default:
			return float3(0.0f);  // no body is up: the scene has no direct light
		}
	};
	const float weight = std::clamp(std::isfinite(blend.targetWeight) ? blend.targetWeight : 1.0f, 0.0f, 1.0f);
	float3 color = bodyColor(blend.source) * (1.0f - weight) + bodyColor(blend.target) * weight;
	float3 direction{ blend.direction.x, blend.direction.y, blend.direction.z };
	const float length = direction.Length();
	if (!std::isfinite(length) || length < 0.5f || !(Luminance(color) > 0.0f))
		return Status::kNoLight;
	direction /= length;

	lightElevation = DirectX::XMConvertToDegrees(std::asin(std::clamp(direction.z, -1.0f, 1.0f)));
	const float horizonFade = Util::Smoothstep(kHorizonFadeStart, kHorizonFadeEnd, direction.z);
	if (horizonFade <= 0.0f)
		return Status::kLightTooLow;
	const float highFade = std::lerp(1.0f, settings.HighSunStrength, Util::Smoothstep(kHighSunStart, kHighSunEnd, direction.z));

	// Cosine of the angle between the light and the view centre: the w of the direction through the view-projection.
	const auto viewProj = globals::game::frameBufferCached.GetCameraViewProj().Transpose();
	const auto clip = DirectX::SimpleMath::Vector4::Transform(DirectX::SimpleMath::Vector4(direction.x, direction.y, direction.z, 0.0f), viewProj);
	const float cosine = std::clamp(std::isfinite(clip.w) ? clip.w : 1.0f, -1.0f, 1.0f);
	lightViewAngle = DirectX::XMConvertToDegrees(std::acos(cosine));
	const float viewFade = Util::Smoothstep(kViewFadeOff, kViewFadeFull, cosine);
	// Where the light stands on the screen. Behind the camera plane it has no place there.
	float2 lightUv{ 0.5f, 0.5f };
	screenWeight = 0.0f;
	if (std::isfinite(clip.x) && std::isfinite(clip.y) && clip.w > 0.05f) {
		const float2 ndc{ clip.x / clip.w, clip.y / clip.w };
		lightUv = { ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f };
		screenWeight = 1.0f - Util::Smoothstep(kScreenFull, kScreenOff, std::max(std::abs(ndc.x), std::abs(ndc.y)));
	}

	const float fade = horizonFade * highFade * viewFade;
	color *= fade;
	effectiveStrength = fade;
	if (settings.DebugView == 0 && Luminance(color) < kMinLuminance)
		return viewFade <= 0.0f ? Status::kLightOutOfView : Status::kNoStrength;

	const float unitsPerMeter = 1.0f / Util::Units::GAME_UNIT_TO_M;
	const auto& quality = kQualities[std::clamp(settings.Quality, 0, kQualityCount - 1)];
	const float nearLength = settings.Length * unitsPerMeter;
	const float farLength = settings.DistantLength * unitsPerMeter;

	a_data = {};
	a_data.LightDir = direction;
	a_data.DiskRadius = sky.cbData.diskAngularRadius.w;
	a_data.LightColor = color;
	a_data.Density = settings.Density;
	a_data.NearLength = nearLength;
	a_data.NearRange = kNearRangeLengths * nearLength;
	a_data.FarLength = farLength;
	a_data.FarRange = a_data.NearRange + kFarRangeLengths * farLength;
	a_data.PhaseG = kMaxPhaseG * settings.Tightness;
	a_data.CloudOcclusion = settings.CloudOcclusion;
	a_data.MaxBrightness = settings.MaxBrightness;
	a_data.VeilRemoval = settings.VeilRemoval;
	a_data.DistantStrength = settings.DistantStrength;
	a_data.ShadowBias = atmosphere.settings.shadowBias;
	// History only from the frame before: after a pause or a cell change the lit share starts over.
	const bool history = lastDrawFrame != UINT32_MAX && lastDrawFrame + 1u == globals::state->frameCount;
	const float deltaTime = std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, 0.25f);
	a_data.AmbientHistory = history ? std::exp(-deltaTime / kAmbientEaseTime) : 0.0f;
	a_data.AmbientRadius = kAmbientRadius * unitsPerMeter;
	a_data.NearSteps = quality.nearSteps;
	a_data.FarSteps = settings.DistantStrength > 0.0f && screenWeight < 1.0f ? quality.farSteps : 0u;
	a_data.ScreenSteps = settings.DistantStrength > 0.0f && screenWeight > 0.0f ? quality.screenSteps : 0u;
	a_data.ScreenWeight = screenWeight;
	a_data.LightUv = lightUv;
	a_data.SilhouetteRays = settings.SilhouetteRays;
	a_data.Divisor = quality.divisor;
	a_data.DebugMode = static_cast<uint32_t>(std::clamp(settings.DebugView, 0, kDebugViewCount - 1));
	a_data.RidgeRadius = kRidgeRadius * unitsPerMeter;
	a_data.AmbientHeight = kAmbientHeight * unitsPerMeter;
	return Status::kActive;
}

void LightShafts::ClearShaderCache()
{
	maskCS = nullptr;
	prepareCS = nullptr;
	marchCS = nullptr;
	resolveCS = nullptr;
	compositeCS = nullptr;
	shadersRequested = false;
}

bool LightShafts::EnsureShaders()
{
	if (!shadersRequested) {
		shadersRequested = true;
		// The shadow inputs are the shared volume's, so the same features decide what they are compiled with.
		std::vector<std::pair<const char*, const char*>> defines{ { "PHYSICAL_SKY", "" } };
		if (globals::features::terrainShadows.loaded)
			defines.emplace_back("TERRAIN_SHADOWS", "");
		prepareCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\LightShafts\\Prepare.cs.hlsl", defines, "cs_5_0")));
		marchCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\LightShafts\\March.cs.hlsl", defines, "cs_5_0")));
		maskCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\LightShafts\\Mask.cs.hlsl", {}, "cs_5_0")));
		resolveCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\LightShafts\\Resolve.cs.hlsl", {}, "cs_5_0")));
		compositeCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\LightShafts\\Composite.cs.hlsl", {}, "cs_5_0")));
	}
	return maskCS && prepareCS && marchCS && resolveCS && compositeCS;
}

bool LightShafts::EnsureResources(uint32_t a_width, uint32_t a_height, uint32_t a_maskWidth, uint32_t a_maskHeight)
{
	auto make = [](uint32_t a_w, uint32_t a_h, DXGI_FORMAT a_format, const char* a_name) {
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_w;
		desc.Height = a_h;
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
	if (!constants)
		constants = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<ShaftData>(), "LightShafts::ShaftData");
	if (!preparedTextures[0]) {
		preparedTextures[0] = make(4, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, "LightShafts::Prepared0");
		preparedTextures[1] = make(4, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, "LightShafts::Prepared1");
		lastDrawFrame = UINT32_MAX;
	}
	if (!linearSampler || !shadowSampler) {
		D3D11_SAMPLER_DESC desc{};
		desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(globals::d3d::device->CreateSamplerState(&desc, linearSampler.put())))
			return false;
		Util::SetResourceName(linearSampler.get(), "LightShafts::LinearSampler");
		// Lit where the point is nearer the light than the map: the same test as the atmosphere volume's.
		desc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
		desc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
		if (FAILED(globals::d3d::device->CreateSamplerState(&desc, shadowSampler.put())))
			return false;
		Util::SetResourceName(shadowSampler.get(), "LightShafts::ShadowSampler");
	}
	if (!shaftsTexture || shaftsTexture->desc.Width != a_width || shaftsTexture->desc.Height != a_height) {
		shaftsTexture = make(a_width, a_height, DXGI_FORMAT_R16G16B16A16_FLOAT, "LightShafts::Shafts");
		resolvedTexture = make(a_width, a_height, DXGI_FORMAT_R16G16B16A16_FLOAT, "LightShafts::Resolved");
		depthTexture = make(a_width, a_height, DXGI_FORMAT_R32_FLOAT, "LightShafts::Depth");
	}
	if (!maskTexture || maskTexture->desc.Width != a_maskWidth || maskTexture->desc.Height != a_maskHeight)
		maskTexture = make(a_maskWidth, a_maskHeight, DXGI_FORMAT_R16G16_FLOAT, "LightShafts::Mask");
	return maskTexture && shaftsTexture && resolvedTexture && depthTexture && preparedTextures[0] && preparedTextures[1];
}

void LightShafts::Render()
{
	// The same frames as the atmosphere's post-water composite: the main view, once.
	if (!loaded || !globals::state->inWorld || globals::state->isMapMenuOpen ||
		(globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)) != 0 ||
		lastCallFrame == globals::state->frameCount)
		return;
	lastCallFrame = globals::state->frameCount;

	// Everything that can rule the pass out is decided here on the CPU. Nothing is dispatched then.
	ShaftData data{};
	status = Update(data);
	if (status != Status::kActive)
		return;
	auto* renderer = globals::game::renderer;
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto* depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV;
	if (!main.texture || !main.UAV || !depth || !EnsureShaders()) {
		status = Status::kNoResources;
		return;
	}
	D3D11_TEXTURE2D_DESC mainDesc{};
	main.texture->GetDesc(&mainDesc);
	const uint32_t divisor = data.Divisor;
	if (!EnsureResources((mainDesc.Width + divisor - 1u) / divisor, (mainDesc.Height + divisor - 1u) / divisor,
			(mainDesc.Width + kMaskDivisor - 1u) / kMaskDivisor, (mainDesc.Height + kMaskDivisor - 1u) / kMaskDivisor)) {
		status = Status::kNoResources;
		return;
	}
	if (lastDrawFrame == UINT32_MAX)
		data.AmbientHistory = 0.0f;
	constants->Update(data);

	auto* context = globals::d3d::context;
	auto& atmosphere = globals::features::viewAtmosphere;
	context->OMSetRenderTargets(0, nullptr, nullptr);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

	ID3D11Buffer* perFrame = *globals::game::perFrame;
	context->CSSetConstantBuffers(12, 1, &perFrame);
	ID3D11Buffer* sharedBuffers[] = { globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	context->CSSetConstantBuffers(5, 2, sharedBuffers);
	winrt::com_ptr<ID3D11Buffer> oldConstants;
	context->CSGetConstantBuffers(kConstantSlot, 1, oldConstants.put());
	auto* constantBuffer = constants->CB();
	context->CSSetConstantBuffers(kConstantSlot, 1, &constantBuffer);
	ID3D11SamplerState* samplers[] = { linearSampler.get(), shadowSampler.get() };
	context->CSSetSamplers(0, 2, samplers);
	atmosphere.Bind(true);
	atmosphere.BindLightingInputs(context);
	auto bind = [&](UINT a_slot, ID3D11ShaderResourceView* a_srv) { context->CSSetShaderResources(a_slot, 1, &a_srv); };
	ID3D11UnorderedAccessView* nullOutputs[2]{};
	bind(kDepthSlot, depth);

	// The same active area as the composite under dynamic resolution.
	const auto scale = globals::game::frameBufferCached.GetDynamicResolutionParams1();
	const uint32_t width = static_cast<uint32_t>(std::clamp(mainDesc.Width * scale.x, 1.0f, static_cast<float>(mainDesc.Width)));
	const uint32_t height = static_cast<uint32_t>(std::clamp(mainDesc.Height * scale.y, 1.0f, static_cast<float>(mainDesc.Height)));
	const uint32_t lowWidth = (width + divisor - 1u) / divisor;
	const uint32_t lowHeight = (height + divisor - 1u) / divisor;

	globals::state->BeginPerfEvent("Light Shafts");

	ID3D11UnorderedAccessView* output = maskTexture->uav.get();
	globals::profiler->BeginPass("LightShafts::Mask");
	const auto& sky = globals::features::physicalSky;
	bind(kCloudSlot, sky.texVolFilteredTr ? sky.texVolFilteredTr->srv.get() : nullptr);
	context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
	context->CSSetShader(maskCS.get(), nullptr, 0);
	context->Dispatch(((width + kMaskDivisor - 1u) / kMaskDivisor + 7u) / 8u, ((height + kMaskDivisor - 1u) / kMaskDivisor + 7u) / 8u, 1);
	context->CSSetUnorderedAccessViews(0, 2, nullOutputs, nullptr);
	bind(kCloudSlot, nullptr);
	bind(kMaskSlot, maskTexture->srv.get());
	globals::profiler->EndPass();

	auto& prepared = preparedTextures[preparedIndex];
	auto& previous = preparedTextures[preparedIndex ^ 1u];
	globals::profiler->BeginPass("LightShafts::Prepare");
	bind(kShaftsSlot, previous->srv.get());
	output = prepared->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
	context->CSSetShader(prepareCS.get(), nullptr, 0);
	context->Dispatch(1, 1, 1);
	context->CSSetUnorderedAccessViews(0, 2, nullOutputs, nullptr);
	bind(kShaftsSlot, nullptr);
	globals::profiler->EndPass();

	globals::profiler->BeginPass("LightShafts::March");
	bind(kPreparedSlot, prepared->srv.get());
	ID3D11UnorderedAccessView* marchOutputs[] = { shaftsTexture->uav.get(), depthTexture->uav.get() };
	context->CSSetUnorderedAccessViews(0, 2, marchOutputs, nullptr);
	context->CSSetShader(marchCS.get(), nullptr, 0);
	context->Dispatch((lowWidth + 7u) / 8u, (lowHeight + 7u) / 8u, 1);
	context->CSSetUnorderedAccessViews(0, 2, nullOutputs, nullptr);
	globals::profiler->EndPass();

	globals::profiler->BeginPass("LightShafts::Resolve");
	bind(kShaftsSlot, shaftsTexture->srv.get());
	bind(kShaftsDepthSlot, depthTexture->srv.get());
	output = resolvedTexture->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
	context->CSSetShader(resolveCS.get(), nullptr, 0);
	context->Dispatch((lowWidth + 7u) / 8u, (lowHeight + 7u) / 8u, 1);
	context->CSSetUnorderedAccessViews(0, 2, nullOutputs, nullptr);
	globals::profiler->EndPass();

	globals::profiler->BeginPass("LightShafts::Composite");
	bind(kShaftsSlot, resolvedTexture->srv.get());
	context->CSSetUnorderedAccessViews(0, 1, &main.UAV, nullptr);
	context->CSSetShader(compositeCS.get(), nullptr, 0);
	context->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1);
	context->CSSetUnorderedAccessViews(0, 2, nullOutputs, nullptr);
	globals::profiler->EndPass();

	globals::state->EndPerfEvent();

	// Leave the compute stage as the atmosphere's composite does: nothing of this pass bound.
	for (UINT slot : { 1u, 2u, 25u, kDepthSlot, kPreparedSlot, kShaftsSlot, kShaftsDepthSlot, kMaskSlot, 60u, 61u, 69u, 70u, 98u, 112u, 125u, 126u, 127u })
		bind(slot, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);
	auto* restored = oldConstants.get();
	context->CSSetConstantBuffers(kConstantSlot, 1, &restored);
	ID3D11Buffer* nullBuffer = nullptr;
	context->CSSetConstantBuffers(12, 1, &nullBuffer);
	ID3D11SamplerState* nullSamplers[2]{};
	context->CSSetSamplers(0, 2, nullSamplers);

	preparedIndex ^= 1u;
	lastDrawFrame = globals::state->frameCount;
}

void LightShafts::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	Util::HoverTip(T(TKEY("enabled_tooltip"), "Beams of sunlight and moonlight in the air, outdoors. Needs Physical Sky and the shared volume of Atmosphere and Fog. Shadows of trees and buildings come from the game's sun shadows, so the beams reach as far as those do."));

	ImGui::SliderFloat(T(TKEY("density"), "Density"), &settings.Density, 0.0f, kMaxDensity, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	Util::HoverTip(T(TKEY("density_tooltip"), "How much the air shows the light. This is the control to drive by weather and time of day: higher in moist or dusty air. 0 removes the beams and their cost. Beams saturate at the strength below, so high values fill them out without brightening them further."));

	ImGui::SliderFloat(T(TKEY("sun_strength"), "Sun Strength"), &settings.SunStrength, 0.0f, kMaxStrength, "%.3f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
	Util::HoverTip(T(TKEY("sun_strength_tooltip"), "Brightness of a full beam seen straight toward the sun, as a share of the sun's own colour after the atmosphere."));

	ImGui::SliderFloat(T(TKEY("moon_strength"), "Moon Strength"), &settings.MoonStrength, 0.0f, kMaxStrength, "%.3f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
	Util::HoverTip(T(TKEY("moon_strength_tooltip"), "The same for the moon that lights the scene at night. The moons are far dimmer than the sun, so equal strengths give far fainter beams."));

	ImGui::SliderFloat(T(TKEY("max_brightness"), "Max Brightness"), &settings.MaxBrightness, kMinMaxBrightness, kMaxMaxBrightness, "%.2f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
	Util::HoverTip(T(TKEY("max_brightness_tooltip"), "Ceiling of the light the beams add to any pixel, in scene units where the sun is 10. Keeps the beams from driving the exposure."));

	if (ImGui::TreeNodeEx(T(TKEY("shape"), "Shape"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T(TKEY("length"), "Length"), &settings.Length, kMinLength, kMaxLength, "%.0f m", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("length_tooltip"), "Distance over which beams among trees and buildings fade. They are followed to three times this, or as far as the sun shadows reach."));

		ImGui::SliderFloat(T(TKEY("tightness"), "Tightness"), &settings.Tightness, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("tightness_tooltip"), "How much the beams gather toward the light. 0 shows them equally in every direction; high values keep them near the sun or moon."));

		ImGui::SliderFloat(T(TKEY("high_sun_strength"), "High Sun Strength"), &settings.HighSunStrength, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("high_sun_strength_tooltip"), "Strength left when the light stands high: full below 25 degrees of elevation, this share above 60."));

		ImGui::SliderFloat(T(TKEY("veil_removal"), "Veil Removal"), &settings.VeilRemoval, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("veil_removal_tooltip"), "At 1 only air that gets more light than the air around it adds light, so open ground and open sky stay as they are. Lower values let all lit air glow, like plain volumetric light."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("clouds"), "Clouds and Distance"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T(TKEY("cloud_occlusion"), "Cloud Occlusion"), &settings.CloudOcclusion, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("cloud_occlusion_tooltip"), "How much the volumetric clouds shade the beams. At 1 a cloud over the sun puts them out and gaps in the cloud make rays of their own. 0 ignores the clouds."));

		ImGui::SliderFloat(T(TKEY("distant_strength"), "Distant Rays"), &settings.DistantStrength, 0.0f, kMaxDistantStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("distant_strength_tooltip"), "Weight of the rays beyond the near range: through gaps in the cloud and past ridges. 0 removes them and their cost."));

		ImGui::SliderFloat(T(TKEY("distant_length"), "Distant Length"), &settings.DistantLength, kMinDistantLength, kMaxDistantLength, "%.0f m", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("distant_length_tooltip"), "Depth of air a distant surface needs in front of it to show distant rays in full, and the distance over which rays followed through the world fade while the light is off the screen."));

		ImGui::SliderFloat(T(TKEY("silhouette_rays"), "Silhouette Rays"), &settings.SilhouetteRays, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("silhouette_rays_tooltip"), "At 0 distant rays need cloud: a clear sky adds nothing however much of it mountains and trees cover. Higher values count what stands against the sky as contrast too, which gives rays past ridges and towers on a clear day and brightens the open sky near the light by as much."));
		ImGui::TreePop();
	}

	const char* qualities[] = {
		T(TKEY("quality_low"), "Low (quarter resolution, 12 + 12 steps)"),
		T(TKEY("quality_medium"), "Medium (half resolution, 16 + 16 steps)"),
		T(TKEY("quality_high"), "High (half resolution, 24 + 24 steps)"),
		T(TKEY("quality_ultra"), "Ultra (half resolution, 40 + 32 steps)")
	};
	settings.Quality = std::clamp(settings.Quality, 0, kQualityCount - 1);
	ImGui::Combo(T(TKEY("quality"), "Quality"), &settings.Quality, qualities, kQualityCount);
	Util::HoverTip(T(TKEY("quality_tooltip"), "Resolution of the beams and steps along each view ray, near + distant. More steps resolve thin shadows such as trunks and branches."));

	const char* views[] = {
		T(TKEY("debug_view_off"), "Off"),
		T(TKEY("debug_view_occlusion"), "Occlusion"),
		T(TKEY("debug_view_beams"), "Beams Only")
	};
	settings.DebugView = std::clamp(settings.DebugView, 0, kDebugViewCount - 1);
	ImGui::Combo(T(TKEY("debug_view"), "Debug View"), &settings.DebugView, views, kDebugViewCount);
	Util::HoverTip(T(TKEY("debug_view_tooltip"), "Occlusion: the share of each near view ray the light reaches, white where all of it is lit.\nBeams Only: the light the pass adds, on black."));
}

void LightShafts::DrawDebugSection()
{
	Util::DebugSection section;
	if (!section)
		return;

	// The post-water hook asks for the beams every frame the main view is drawn.
	const bool running = lastCallFrame != UINT32_MAX && globals::state->frameCount - lastCallFrame <= 2u;
	const char* state = T(TKEY("status_not_called"), "The post-water composite is not running");
	if (running) {
		switch (status) {
		case Status::kActive:
			state = T(TKEY("status_active"), "Active");
			break;
		case Status::kDisabled:
			state = T(TKEY("status_disabled"), "Disabled");
			break;
		case Status::kNoStrength:
			state = T(TKEY("status_no_strength"), "Strength or density is zero");
			break;
		case Status::kInterior:
			state = T(TKEY("status_interior"), "Interior");
			break;
		case Status::kUnderwater:
			state = T(TKEY("status_underwater"), "Underwater");
			break;
		case Status::kNoSky:
			state = T(TKEY("status_no_sky"), "Physical Sky is not running");
			break;
		case Status::kNoVolume:
			state = T(TKEY("status_no_volume"), "The shared atmosphere volume is not running");
			break;
		case Status::kNoLight:
			state = T(TKEY("status_no_light"), "No sun or moon lights the scene");
			break;
		case Status::kLightTooLow:
			state = T(TKEY("status_light_too_low"), "The light is below the horizon");
			break;
		case Status::kLightOutOfView:
			state = T(TKEY("status_light_out_of_view"), "The light is too far outside the view");
			break;
		case Status::kNoResources:
			state = T(TKEY("status_no_resources"), "Shaders or textures could not be created");
			break;
		default:
			break;
		}
	}
	ImGui::Text("%s: %s", T(TKEY("debug_status"), "Status"), state);
	ImGui::Text("%s: %.1f", T(TKEY("debug_elevation"), "Light elevation (degrees)"), lightElevation);
	ImGui::Text("%s: %.1f", T(TKEY("debug_view_angle"), "Light from view centre (degrees)"), lightViewAngle);
	ImGui::Text("%s: %.3f", T(TKEY("debug_fade"), "Strength after elevation and view fades"), effectiveStrength);
	ImGui::Text("%s: %.2f", T(TKEY("debug_screen_weight"), "Distant rays taken from the screen"), screenWeight);
}

void LightShafts::LoadSettings(json& o_json)
{
	settings = o_json;
	const Settings defaults{};
	auto sanitize = [](float& a_value, float a_default, float a_min, float a_max) {
		a_value = std::clamp(std::isfinite(a_value) ? a_value : a_default, a_min, a_max);
	};
	sanitize(settings.SunStrength, defaults.SunStrength, 0.0f, kMaxStrength);
	sanitize(settings.MoonStrength, defaults.MoonStrength, 0.0f, kMaxStrength);
	sanitize(settings.Density, defaults.Density, 0.0f, kMaxDensity);
	sanitize(settings.Length, defaults.Length, kMinLength, kMaxLength);
	sanitize(settings.Tightness, defaults.Tightness, 0.0f, 1.0f);
	sanitize(settings.CloudOcclusion, defaults.CloudOcclusion, 0.0f, 1.0f);
	sanitize(settings.MaxBrightness, defaults.MaxBrightness, kMinMaxBrightness, kMaxMaxBrightness);
	sanitize(settings.DistantStrength, defaults.DistantStrength, 0.0f, kMaxDistantStrength);
	sanitize(settings.DistantLength, defaults.DistantLength, kMinDistantLength, kMaxDistantLength);
	sanitize(settings.HighSunStrength, defaults.HighSunStrength, 0.0f, 1.0f);
	sanitize(settings.VeilRemoval, defaults.VeilRemoval, 0.0f, 1.0f);
	sanitize(settings.SilhouetteRays, defaults.SilhouetteRays, 0.0f, 1.0f);
	settings.Quality = std::clamp(settings.Quality, 0, kQualityCount - 1);
	settings.DebugView = std::clamp(settings.DebugView, 0, kDebugViewCount - 1);
}

void LightShafts::SaveSettings(json& o_json)
{
	o_json = settings;
}

void LightShafts::RestoreDefaultSettings()
{
	settings = {};
}

#undef I18N_KEY_PREFIX
