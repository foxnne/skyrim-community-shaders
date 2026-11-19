#include "GroundFog.h"

#include "Features/TerrainShadows.h"
#include "Globals.h"
#include "State.h"
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
	GroundFill)

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

	effectiveAmount = settings.Amount * (1.0f - settings.WindResponse * Util::Smoothstep(kCalmWind, kStrongWind, windSpeed));
	const bool debug = debugMode != DebugMode::kOff;
	const bool slab = debugMode == DebugMode::kSlab || debugMode == DebugMode::kGroundHeight;
	if (!debug && effectiveAmount * settings.Density <= 0.0f)
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
	a_data.Flags = 1u;
	a_data.DebugMode = static_cast<uint32_t>(debugMode);
	a_heightMap = map.srv;
	return Status::kActive;
}

bool GroundFog::BindForVolume(ID3D11DeviceContext* a_context)
{
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

void GroundFog::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	Util::HoverTip(T(TKEY("enabled_tooltip"), "A thin sheet of fog on the ground, outdoors. Needs the shared volume of Atmosphere and Fog with Physical Sky, and a 16-bit terrain height map for the worldspace."));

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
		Util::HoverTip(T(TKEY("fade_end_tooltip"), "Distance beyond which there is none. Far away the sheet is thinner than the volume can resolve and would read as a pale band."));
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

		ImGui::SliderFloat(T(TKEY("self_shadow"), "Self Shadowing"), &settings.SelfShadow, 0.0f, kMaxLightScale, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("self_shadow_tooltip"), "How much the fog above dims the fog below at a low sun."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("advanced"), "Advanced"))) {
		ImGui::SliderFloat(T(TKEY("ground_fill"), "Fill Below Ground"), &settings.GroundFill, 0.0f, kMaxGroundFill, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("ground_fill_tooltip"), "Depth under the mapped ground that still holds fog, as a share of Thickness. Covers places where the real ground lies below the height map, at the cost of denser fog in the distance."));
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
		T(TKEY("debug_mode_layer_mask"), "Layer Mask")
	};
	int mode = static_cast<int>(debugMode);
	if (ImGui::Combo(T(TKEY("debug_mode"), "Debug View"), &mode, modes, static_cast<int>(std::size(modes))))
		debugMode = static_cast<DebugMode>(std::clamp(mode, 0, static_cast<int>(std::size(modes)) - 1));
	Util::HoverTip(T(TKEY("debug_mode_tooltip"), "Slab: a dense, level-topped 1 m layer with no noise, to check the height map against the ground.\nGround Height: the slab colored by the sampled height, one stripe per 64 units.\nLayer Mask: the real layer, dense, with green for its thickness, red for hollows and blue for water."));

	// Atmosphere and Fog asks for the layer every frame its shared volume runs.
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
		default:
			break;
		}
	}
	ImGui::Text("%s: %s", T(TKEY("debug_status"), "Status"), state);
	ImGui::Text("%s: %.3f", T(TKEY("debug_effective_amount"), "Amount after wind"), effectiveAmount);
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
