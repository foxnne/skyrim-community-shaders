#include "GroundFog.h"

#include <DirectXTex.h>
#include <pystring/pystring.h>

#include "Features/CloudShadows.h"
#include "Features/ExponentialHeightFog.h"
#include "Features/Skylighting.h"
#include "Features/TerrainShadows.h"
#include "Features/ViewAtmosphere.h"
#include "Globals.h"
#include "Profiler.h"
#include "State.h"
#include "Util.h"
#include "Utils/Climate.h"
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
	ShoreSoftness,
	WaterPatchiness,
	Albedo,
	SunScale,
	SkyAmbientScale,
	SelfShadow,
	SunDiffusion,
	SunLimit,
	CanopyShade,
	GroundFill,
	SurfaceBlendDistance,
	SurfaceMaxHeight,
	SurfaceSoftness,
	FloorMaxHeight,
	NearDensity,
	NearThickness,
	NearFalloff,
	NearDistance,
	DriftScale,
	DriftStrength,
	Breathing,
	BurnOffStrength,
	ShadowRetention,
	MorningAmount,
	MiddayAmount,
	EveningAmount,
	Reactive,
	ReactiveStrength,
	ImpulseStrength,
	SettleTime,
	Wispiness,
	WispScale,
	DownhillFlow,
	Swirl,
	Pooling,
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
	constexpr float kMinShoreSoftness = 0.05f;
	constexpr float kMaxShoreSoftness = 3.0f;
	constexpr float kMaxLightScale = 4.0f;
	constexpr float kMaxGroundFill = 8.0f;
	constexpr float kMaxSurfaceBlend = 10.0f;
	constexpr float kMaxSurfaceHeight = 4.0f;
	constexpr float kMaxSoftness = 2.0f;
	constexpr float kMaxFloorHeight = 8.0f;
	constexpr float kMaxNearDensity = 8.0f;
	constexpr float kMinNearThickness = 0.15f;
	constexpr float kMaxNearThickness = 8.0f;
	constexpr float kMinNearFalloff = 0.5f;
	constexpr float kMaxNearFalloff = 6.0f;
	constexpr float kMinNearDistance = 15.0f;
	constexpr float kMaxNearDistance = 150.0f;
	constexpr float kMinDriftScale = 0.5f;
	constexpr float kMaxDriftScale = 40.0f;
	constexpr float kMaxBreathing = 0.6f;
	constexpr float kMaxTimeAmount = 3.0f;
	// The near volume's top rises and falls once in this many seconds.
	constexpr float kBreathPeriod = 11.0f;
	// The floor offset under the player follows changes over about this many seconds.
	constexpr float kFloorSmoothing = 0.6f;
	// Hours after sunrise ends over which the fog thins to its midday amount, and before sunset ends over which it
	// starts to return.
	constexpr float kBurnOffHours = 3.0f;
	constexpr float kReturnHours = 2.0f;
	// The near layer is the pool up to this thickness (meters) and the tall marched volume above it.
	constexpr float kPoolMaxThickness = 1.0f;
	// The simulation: texels across, and the smallest game units per texel (0.4 m; the window then covers 102 m).
	// With a longer Near Distance the texels grow so that the window still holds it.
	constexpr uint32_t kSimSize = 256;
	constexpr double kSimMinTexel = 28.0;
	constexpr double kSimTexelStep = 7.0;
	// Seconds over which the fog's motion relative to the wind dies down.
	constexpr float kSimVelocityTime = 2.5f;
	// Gusts at Swirl 1: speed (game units per second) and size of a cell of their noise (game units, 12 m).
	constexpr float kSimGustSpeed = 45.0f;
	constexpr float kSimGustSize = 840.0f;
	// Seconds over which the sun map follows the shadows (moving leaves, the cascades' own stepping).
	constexpr float kSimSunTime = 0.5f;
	// Seconds over which fog that was thrown up settles back.
	constexpr float kSimLiftTime = 1.4f;
	constexpr float kMaxImpulseStrength = 3.0f;
	// Share of the fog on a body's path that it pushes to the rim of its track, whatever its speed (the original
	// mod's ActorPush is 0.35), and the speeds between which that comes in (game units per second).
	constexpr float kActorPush = 0.7f;
	constexpr float kActorPushStart = 40.0f;
	constexpr float kActorPushFull = 90.0f;
	constexpr float kMinWispScale = 1.0f;
	constexpr float kMaxWispScale = 15.0f;
	constexpr float kMaxSunLimit = 8.0f;
	// Speed of a ripple in the fog, game units per second: how fast a hole closes from its rim.
	constexpr float kSimWaveSpeed = 50.0f;
	// Acceleration down a slope of 1 at Downhill Flow 1, game units per second^2 (0.5 m/s on a 1 in 5 slope).
	constexpr float kSimDownhill = 120.0f;
	// Seconds over which the floor map rises to a higher surface (it drops to a lower one at once).
	constexpr float kSimFloorRiseTime = 0.75f;
	constexpr float kMinSettleTime = 0.5f;
	constexpr float kMaxSettleTime = 30.0f;
	constexpr float kMaxDownhillFlow = 4.0f;
	constexpr float kMaxSwirl = 2.0f;
	// Movers slower than this leave the fog alone; faster than the cap they push it no harder. Game units per second.
	constexpr float kFlowMinSpeed = 20.0f;
	constexpr float kFlowMaxSpeed = 600.0f;
	// Things higher than this above the ground are over the fog, not in it. Game units: for the pool, and for the
	// tall volume.
	constexpr float kFlowHeightStartPool = 110.0f;
	constexpr float kFlowHeightEndPool = 230.0f;
	constexpr float kFlowHeightStart = 280.0f;
	constexpr float kFlowHeightEnd = 490.0f;
	constexpr float kMaxReactiveStrength = 2.0f;
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

	// The worldspace whose land (and height map) the current one uses, as Terrain Shadows picks it.
	RE::TESWorldSpace* LandWorldspace()
	{
		auto tes = globals::game::tes;
		auto worldspace = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
		while (worldspace && worldspace->parentWorld && worldspace->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseLandData))
			worldspace = worldspace->parentWorld;
		return worldspace;
	}
}

void GroundFog::SetupResources()
{
	volumeConstants = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<VolumeData>(), "GroundFog::VolumeData");
	flowConstants = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<FlowData>(), "GroundFog::FlowData");
	simData = std::make_unique<FlowData>();
	FindWaterMaps();
}

void GroundFog::FindWaterMaps()
{
	// Baked by terrain-heightmap/build_watermap.py. Not under textures\\Terrain, which Terrain Shadows scans for
	// height maps.
	waterMapFiles.clear();
	std::error_code error;
	for (auto const& entry : std::filesystem::directory_iterator{ std::filesystem::path{ L"Data\\textures\\GroundFog\\WaterMaps\\" }, error }) {
		const auto path = entry.path();
		if (path.extension() != ".dds")
			continue;
		const auto fields = pystring::split(path.stem().string(), ".");
		if (fields.size() != 6 || fields[1] != "WaterMap") {
			logger::debug("[Ground Fog] {} is not named like a water map", path.filename().string());
			continue;
		}
		try {
			WaterMapFile file;
			file.path = path;
			file.minX = std::stoi(fields[2]);
			file.minY = std::stoi(fields[3]);
			file.maxX = std::stoi(fields[4]);
			file.maxY = std::stoi(fields[5]);
			waterMapFiles[fields[0]] = file;
			logger::info("[Ground Fog] Water map found: {}", path.filename().string());
		} catch (const std::exception& e) {
			logger::debug("[Ground Fog] Failed to parse {}: {}", path.filename().string(), e.what());
		}
	}
}

ID3D11ShaderResourceView* GroundFog::EnsureWaterMap(const TerrainShadows::HeightMapBinding& a_map)
{
	const auto worldspace = LandWorldspace();
	const std::string name = worldspace ? worldspace->GetFormEditorID() : "";
	if (name == waterMapWorldspace)
		return waterMap ? waterMap->srv.get() : nullptr;
	// One attempt per worldspace: a map that fails to load is not tried again every frame.
	waterMapWorldspace = name;
	waterMap = nullptr;
	waterMapTexel = 0.0f;
	waterMapState = "none for this worldspace";
	const auto found = waterMapFiles.find(name);
	if (found == waterMapFiles.end())
		return nullptr;
	const auto& file = found->second;

	// It must cover the height map's rectangle: the shader reads both with one uv.
	const float west = (0.5f / a_map.width - a_map.scaleOffset.z) / a_map.scaleOffset.x;
	const float north = -(0.5f / a_map.height + a_map.scaleOffset.w) / a_map.scaleOffset.y;
	const float extentX = 1.0f / a_map.scaleOffset.x;
	const float extentY = -1.0f / a_map.scaleOffset.y;
	const float cellsX = static_cast<float>(file.maxX - file.minX + 1);
	const float cellsY = static_cast<float>(file.maxY - file.minY + 1);
	if (std::abs(west - file.minX * 4096.0f) > 64.0f || std::abs(north - (file.maxY + 1) * 4096.0f) > 64.0f ||
		std::abs(extentX - cellsX * 4096.0f) > 64.0f || std::abs(extentY - cellsY * 4096.0f) > 64.0f) {
		waterMapState = "does not cover the height map's cells";
		logger::warn("[Ground Fog] {} does not cover the same cells as the height map; not used", file.path.filename().string());
		return nullptr;
	}

	DirectX::ScratchImage image;
	ID3D11Resource* resource = nullptr;
	if (FAILED(DirectX::LoadFromDDSFile(file.path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image)) ||
		image.GetMetadata().format != DXGI_FORMAT_R16G16B16A16_UNORM ||
		FAILED(DirectX::CreateTexture(globals::d3d::device, image.GetImages(), image.GetImageCount(), image.GetMetadata(), &resource))) {
		waterMapState = "could not be loaded (needs RGBA16_UNORM)";
		logger::warn("[Ground Fog] {} could not be loaded as an RGBA16_UNORM texture; not used", file.path.filename().string());
		return nullptr;
	}
	waterMap = eastl::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(resource), "GroundFog::WaterMap");
	D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = waterMap->desc.Format;
	srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srv.Texture2D.MipLevels = waterMap->desc.MipLevels;
	waterMap->CreateSRV(srv);
	if (!waterMap->srv) {
		waterMap = nullptr;
		waterMapState = "could not be loaded";
		return nullptr;
	}
	waterMapTexel = cellsX * 4096.0f / static_cast<float>(waterMap->desc.Width);
	waterMapState = std::format("{} x {}, {} levels, {:.0f} units per texel", waterMap->desc.Width, waterMap->desc.Height, waterMap->desc.MipLevels, waterMapTexel);
	logger::info("[Ground Fog] Water map loaded for {}: {}", name, waterMapState);
	return waterMap->srv.get();
}

void GroundFog::UpdateFloorOffset(float a_deltaTime)
{
	// The height map knows the land, not what is built on it. Where the player stands on a road on fill, a bridge
	// or a town floor, the near volume's ground is raised by the difference around the player, so the fog is not
	// left buried under the floor. In the air the last value is kept; in water there is no floor.
	const float unitsPerMeter = 1.0f / Util::Units::GAME_UNIT_TO_M;
	float target = floorOffset;
	floorAnchor = {};
	if (auto player = RE::PlayerCharacter::GetSingleton(); player && player->Is3DLoaded()) {
		const auto position = player->GetPosition();
		const auto eye = Util::GetEyePosition();
		floorAnchor = { position.x - eye.x, position.y - eye.y };
		const auto state = player->AsActorState();
		if (state && state->IsSwimming()) {
			target = 0.0f;
		} else if (!player->IsInMidair()) {
			float ground = 0.0f;
			auto tes = RE::TES::GetSingleton();
			if (tes && tes->GetLandHeight(position, ground) && std::isfinite(ground)) {
				if (const auto cell = player->GetParentCell()) {
					const float water = cell->GetExteriorWaterHeight();
					if (std::isfinite(water) && water > -1.0e6f && water < 1.0e6f)
						ground = std::max(ground, water);
				}
				const float top = std::max(settings.FloorMaxHeight, settings.SurfaceMaxHeight) * unitsPerMeter;
				const float above = std::max(position.z - ground, 0.0f);
				// Higher than a floor can be: a roof, a tower, a cliff of rock meshes. The fog stays below.
				target = above * (1.0f - Util::Smoothstep(top, 1.5f * top, above));
			} else {
				target = 0.0f;
			}
		}
	} else {
		target = 0.0f;
	}
	floorOffset += (target - floorOffset) * (1.0f - std::exp(-a_deltaTime / kFloorSmoothing));
	if (!std::isfinite(floorOffset))
		floorOffset = 0.0f;
}

uint32_t GroundFog::GatherSimEmitters(FlowEmitter* a_emitters, double a_originX, double a_originY, float a_deltaTime)
{
	// Sources, all read from lists the game keeps anyway (no hooks):
	// - actors: the player and RE::ProcessLists::highActorHandles (the actors near the player that are fully
	//   simulated), as Grass Collision, Capsule Shadows and Footstep Particles do. Velocity is the change of
	//   position since the last frame, which also covers mounts, ragdolls and anything moved by script.
	// - projectiles: RE::Projectile::Manager's limited and unlimited lists, under its lock: arrows, bolts, spell
	//   missiles, cones, flames. Their runtime data has the velocity.
	// - impacts: a projectile that was flying last frame and is gone this frame ended there: a burst at its last
	//   position, larger for one that carried a spell or an explosion.
	// Not gathered yet. The emitter list is the whole interface, so each of these is an add() call here and nothing
	// in the shader changes:
	// - spell casts and shouts at the caster: a burst at the caster (add(position, {}, radius, burst, clear, 0));
	// - RE::Explosion references and hazards: a burst, or a standing clear for a hazard;
	// - heat (torches, campfires, flame spells): an emitter with no velocity and a small clear every frame, which
	//   keeps a hole open that refills over Settle Time when the source goes;
	// - falling objects and ragdolls that are not actors.
	struct Candidate
	{
		FlowEmitter emitter;
		float priority;  // lower comes first
	};
	eastl::vector<Candidate> candidates;
	const auto eye = Util::GetEyePosition();
	const float reach = static_cast<float>(kSimSize * simTexel) * 0.5f + 300.0f;
	// The height map knows the land; near the player the fog lies on the floor the player stands on.
	const float heightStart = poolActive ? kFlowHeightStartPool : kFlowHeightStart;
	const float heightEnd = poolActive ? kFlowHeightEndPool : kFlowHeightEnd;
	const float invDelta = a_deltaTime > 1.0e-4f ? 1.0f / a_deltaTime : 0.0f;
	auto tes = RE::TES::GetSingleton();
	// 1 in the fog's height range above the land, 0 well above it. Unknown land counts as in range.
	auto inFog = [&](const RE::NiPoint3& a_position) {
		float land = 0.0f;
		if (!tes || !tes->GetLandHeight(a_position, land) || !std::isfinite(land))
			return 1.0f;
		return 1.0f - Util::Smoothstep(heightStart, heightEnd, a_position.z - land - floorOffset);
	};
	// a_weight: how far the thing is in the fog, 0 to 1; negative to work it out from its height above the land.
	auto add = [&](const RE::NiPoint3& a_position, RE::NiPoint3 a_velocity, float a_radius, float a_burst, float a_clear, float a_priority, float a_weight = -1.0f, float a_push = 0.0f) -> FlowEmitter* {
		const float weight = a_weight >= 0.0f ? a_weight : inFog(a_position);
		if (weight <= 0.0f)
			return nullptr;
		const float speed = std::sqrt(a_velocity.x * a_velocity.x + a_velocity.y * a_velocity.y);
		if (speed > kFlowMaxSpeed)
			a_velocity *= kFlowMaxSpeed / speed;
		Candidate candidate{};
		candidate.emitter.Position = { static_cast<float>(a_position.x - a_originX), static_cast<float>(a_position.y - a_originY) };
		candidate.emitter.Velocity = { a_velocity.x * weight, a_velocity.y * weight };
		candidate.emitter.Radius = a_radius;
		candidate.emitter.Burst = a_burst * weight;
		candidate.emitter.Clear = a_clear * weight;
		candidate.emitter.Push = a_push * weight;
		candidate.priority = a_priority;
		candidates.push_back(candidate);
		return &candidates.back().emitter;
	};
	// Something that strikes the fog once: a kick in a direction, a ring outward and the fog thrown up.
	const float impulse = std::clamp(settings.ImpulseStrength, 0.0f, kMaxImpulseStrength);
	auto strike = [&](const RE::NiPoint3& a_position, float a_radius, float a_burst, float a_clear, const RE::NiPoint3& a_kick, float a_lift, float a_weight) {
		if (impulse <= 0.0f)
			return;
		if (auto* emitter = add(a_position, RE::NiPoint3(0.0f, 0.0f, 0.0f), a_radius, a_burst * impulse, a_clear, 0.5f, a_weight)) {
			const float weight = a_burst != 0.0f ? emitter->Burst / (a_burst * impulse) : 1.0f;
			emitter->Kick = { a_kick.x * impulse * weight, a_kick.y * impulse * weight };
			emitter->Lift = a_lift * impulse * weight;
		}
	};
	auto inWindow = [&](const RE::NiPoint3& a_position) {
		return std::abs(a_position.x - eye.x) < reach && std::abs(a_position.y - eye.y) < reach;
	};

	// Projectiles and what became of last frame's.
	std::unordered_map<uint32_t, FlowProjectile> projectiles;
	if (auto manager = RE::Projectile::Manager::GetSingleton()) {
		RE::BSSpinLockGuard lock(manager->projectileLock);
		auto visit = [&](const RE::BSTArray<RE::ProjectileHandle>& a_handles) {
			for (auto& handle : a_handles) {
				auto projectile = handle.get();
				if (!projectile)
					continue;
				const auto position = projectile->GetPosition();
				if (!inWindow(position))
					continue;
				const auto& runtime = projectile->GetProjectileRuntimeData();
				const auto velocity = runtime.linearVelocity;
				const float speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z);
				FlowProjectile entry;
				entry.position = position;
				entry.magic = runtime.spell != nullptr || runtime.explosion != nullptr;
				entry.moving = std::isfinite(speed) && speed > 5.0f * kFlowMinSpeed;
				projectiles[handle.native_handle()] = entry;
				if (entry.moving)
					add(position, velocity, entry.magic ? 70.0f : 30.0f, 0.0f, (entry.magic ? 2.5f : 0.6f) * a_deltaTime, 1.0f);
			}
		};
		visit(manager->limited);
		visit(manager->unlimited);
	}
	for (auto& [handle, last] : flowProjectiles) {
		if (last.moving && !projectiles.contains(handle))
			strike(last.position, last.magic ? 220.0f : 60.0f, last.magic ? 500.0f : 120.0f, last.magic ? 0.9f : 0.2f, RE::NiPoint3(0.0f, 0.0f, 0.0f), last.magic ? 1.8f : 0.5f, -1.0f);
	}
	flowProjectiles = std::move(projectiles);

	// Actors.
	std::unordered_map<uint32_t, FlowActor> actors;
	auto visitActor = [&](RE::Actor* a_actor, uint32_t a_handle) {
		if (!a_actor || !a_actor->Is3DLoaded() || actors.contains(a_handle))
			return;
		const auto position = a_actor->GetPosition();
		if (!inWindow(position))
			return;
		// What it is doing, read from state the game keeps anyway (no event sinks, no hooks).
		FlowActor now;
		now.position = position;
		now.midair = a_actor->IsInMidair();
		const auto state = a_actor->AsActorState();
		const auto attack = state ? state->GetAttackState() : RE::ATTACK_STATE_ENUM::kNone;
		now.swinging = attack == RE::ATTACK_STATE_ENUM::kSwing || attack == RE::ATTACK_STATE_ENUM::kBash;
		for (int source = 0; source < 3; ++source) {
			const auto caster = a_actor->GetMagicCaster(static_cast<RE::MagicSystem::CastingSource>(source));
			now.casting[source] = caster && caster->state.get() == RE::MagicCaster::State::kCasting;
		}
		const auto previous = flowActors.find(a_handle);
		const bool known = previous != flowActors.end() && invDelta > 0.0f;
		if (known)
			now.velocityZ = (position.z - previous->second.position.z) * invDelta;
		actors[a_handle] = now;
		if (!known)
			return;
		const FlowActor& before = previous->second;
		const float radius = std::clamp(0.3f * a_actor->GetHeight(), 30.0f, 250.0f);
		const float angle = a_actor->GetAngleZ();
		const RE::NiPoint3 heading{ std::sin(angle), std::cos(angle), 0.0f };
		const RE::NiPoint3 none{ 0.0f, 0.0f, 0.0f };
		// Whatever walks stands on the floor the fog lies on, whatever the land map says is under it. What is in
		// the air is weighed by its height.
		const float weight = now.midair ? -1.0f : 1.0f;

		// What it starts doing this frame.
		// Jump: a small puff as the feet leave. Landing: a ring outward and the fog thrown up, harder the faster it fell.
		if (now.midair && !before.midair)
			strike(position, radius * 1.6f, 70.0f, 0.05f, none, 0.7f, 1.0f);
		if (!now.midair && before.midair) {
			const float hard = std::clamp(-before.velocityZ / 600.0f, 0.4f, 2.0f);
			strike(position, radius * 2.4f, 160.0f * hard, 0.15f, none, hard, 1.0f);
		}
		// A swing or a bash: the fog in front is thrown the way the actor faces, and up.
		if (now.swinging && !before.swinging)
			strike(position + heading * (radius * 2.2f), radius * 2.6f, 60.0f, 0.2f, heading * 320.0f, 1.1f, weight);
		// A spell let go from a hand: a puff forward. The voice (a shout): a blast forward and outward.
		for (int source = 0; source < 3; ++source) {
			if (!now.casting[source] || before.casting[source])
				continue;
			if (source == 2)
				strike(position + heading * 100.0f, radius * 4.0f, 420.0f, 0.3f, heading * 260.0f, 1.6f, weight);
			else
				strike(position + heading * 60.0f, radius * 2.0f, 140.0f, 0.1f, heading * 120.0f, 0.8f, weight);
		}

		// The body moving through the fog.
		const auto velocity = (position - before.position) * invDelta;
		const float speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y);
		// Faster than anything walks or rides: it was moved, not moving.
		if (!std::isfinite(speed) || speed < kFlowMinSpeed || speed > 4000.0f)
			return;
		// A sprinter drags a wider, longer wake.
		const float sprint = state && state->IsSprinting() ? 1.4f : 1.0f;
		// A walker stirs away about half of the fog in its path each second, a runner nearly all of it.
		const float clear = std::min(speed / 400.0f, 1.5f) * 2.2f * sprint * a_deltaTime;
		const float dx = position.x - eye.x;
		const float dy = position.y - eye.y;
		// A Ricker source of radius R loses z * sqrt(pi) * R / 2 on a straight path through its middle at rate
		// z * speed, whatever the speed (Iwillbewarlock GroundFog, WindShaders.h Source): z for the share wanted.
		const float reachOut = radius * 2.0f * sprint;
		const float push = kActorPush / (0.8862f * reachOut) * speed * Util::Smoothstep(kActorPushStart, kActorPushFull, speed);
		add(position, velocity, reachOut, 0.0f, clear, 2.0f + (dx * dx + dy * dy) * 1.0e-8f, weight, push);
	};
	if (auto player = RE::PlayerCharacter::GetSingleton())
		visitActor(player, player->GetHandle().native_handle());
	if (auto lists = RE::ProcessLists::GetSingleton()) {
		for (auto& handle : lists->highActorHandles)
			visitActor(handle.get().get(), handle.native_handle());
	}
	flowActors = std::move(actors);

	std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.priority < b.priority; });
	const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(candidates.size()), kMaxFlowEmitters);
	for (uint32_t i = 0; i < count; ++i)
		a_emitters[i] = candidates[i].emitter;
	return count;
}

float4 GroundFog::PrepareSim(bool a_pool)
{
	if (flowFrame == globals::state->frameCount)
		return flowWindow;
	flowFrame = globals::state->frameCount;
	flowWindow = {};
	flowEmitterCount = 0;
	simPending = false;
	// The pool is drawn from the simulation. The tall volume and the sheet only react through it.
	if (!(a_pool || settings.Reactive) || !flowCS || !flowConstants || !simData) {
		flowValid = false;
		return flowWindow;
	}
	// The window holds the near layer's range with a margin. Its texels are fixed in the world for as long as
	// their size stays; a change of Near Distance that changes the size starts afresh.
	const float unitsPerMeter = 1.0f / Util::Units::GAME_UNIT_TO_M;
	const double nearEnd = static_cast<double>(std::clamp(settings.NearDistance, kMinNearDistance, kMaxNearDistance) * unitsPerMeter);
	const double texel = std::max(kSimMinTexel, std::ceil(nearEnd * 2.15 / kSimSize / kSimTexelStep) * kSimTexelStep);
	if (texel != simTexel) {
		simTexel = texel;
		flowValid = false;
	}
	if (!simState[0]) {
		auto make = [](DXGI_FORMAT a_format, const char* a_name) {
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = desc.Height = kSimSize;
			desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
			desc.Format = a_format;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = a_format;
			srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srv.Texture2D.MipLevels = 1;
			D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = a_format;
			uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			auto texture = eastl::make_unique<Texture2D>(desc, a_name);
			texture->CreateSRV(srv);
			texture->CreateUAV(uav);
			return texture;
		};
		simState[0] = make(DXGI_FORMAT_R16G16B16A16_FLOAT, "GroundFog::SimState0");
		simState[1] = make(DXGI_FORMAT_R16G16B16A16_FLOAT, "GroundFog::SimState1");
		simFloor[0] = make(DXGI_FORMAT_R32_FLOAT, "GroundFog::SimFloor0");
		simFloor[1] = make(DXGI_FORMAT_R32_FLOAT, "GroundFog::SimFloor1");
		floorSlope[0] = make(DXGI_FORMAT_R16G16B16A16_FLOAT, "GroundFog::FloorSlope0");
		floorSlope[1] = make(DXGI_FORMAT_R16G16B16A16_FLOAT, "GroundFog::FloorSlope1");
		floorSeen = make(DXGI_FORMAT_R32_FLOAT, "GroundFog::FloorSeen");
		simLift[0] = make(DXGI_FORMAT_R16_FLOAT, "GroundFog::SimLift0");
		simLift[1] = make(DXGI_FORMAT_R16_FLOAT, "GroundFog::SimLift1");
		flowValid = false;
	}

	// The window is fixed to a world grid of whole texels and follows the camera in whole steps.
	const auto eye = Util::GetEyePosition();
	const int64_t originX = static_cast<int64_t>(std::floor(static_cast<double>(eye.x) / simTexel)) - kSimSize / 2;
	const int64_t originY = static_cast<int64_t>(std::floor(static_cast<double>(eye.y) / simTexel)) - kSimSize / 2;
	const float deltaTime = std::clamp(frameDeltaTime, 0.0f, 0.05f);
	const float cornerX = static_cast<float>(static_cast<double>(originX) * simTexel - static_cast<double>(eye.x));
	const float cornerY = static_cast<float>(static_cast<double>(originY) * simTexel - static_cast<double>(eye.y));

	*simData = {};
	// A step larger than the window (first frame, cell change, teleport) reads nothing: a clean start at rest.
	const int64_t farStep = 4 * static_cast<int64_t>(kSimSize);
	simData->Shift[0] = static_cast<int32_t>(flowValid ? std::clamp(originX - flowOriginX, -farStep, farStep) : farStep);
	simData->Shift[1] = static_cast<int32_t>(flowValid ? std::clamp(originY - flowOriginY, -farStep, farStep) : farStep);
	simData->TexelSize = static_cast<float>(simTexel);
	simData->DeltaTime = deltaTime;
	simData->VelocityKeep = std::exp(-deltaTime / kSimVelocityTime);
	simData->RefillRate = 1.0f - std::exp(-deltaTime / std::clamp(settings.SettleTime, kMinSettleTime, kMaxSettleTime));
	simData->ClearStrength = settings.ReactiveStrength;
	simData->Wind = windVelocity;
	simData->Downhill = kSimDownhill * settings.DownhillFlow;
	simData->Spread = kSimWaveSpeed * kSimWaveSpeed;
	simData->Corner = { cornerX, cornerY };
	simData->Swirl = settings.Swirl;
	simData->Pooling = std::clamp(settings.Pooling, 0.0f, 1.0f);
	simData->SunRate = 1.0f - std::exp(-deltaTime / kSimSunTime);
	simData->LayerTop = std::max(settings.NearThickness, kMinNearThickness) * unitsPerMeter;
	simData->GustSpeed = kSimGustSpeed;
	simData->GustSize = kSimGustSize;
	simData->LiftKeep = std::exp(-deltaTime / kSimLiftTime);
	simData->SwirlTime = static_cast<float>(swirlTime);
	// Without terrain data the fog rests evenly. Interiors would set this from the cell (and a floor level).
	simData->RestUniform = 1.0f;
	simData->FloorRise = 1.0f - std::exp(-deltaTime / kSimFloorRiseTime);
	if (settings.Reactive)
		simData->EmitterCount = GatherSimEmitters(simData->Emitters, static_cast<double>(originX) * simTexel, static_cast<double>(originY) * simTexel, deltaTime);
	else {
		flowActors.clear();
		flowProjectiles.clear();
	}
	flowEmitterCount = simData->EmitterCount;
	simNextOriginX = originX;
	simNextOriginY = originY;
	simPending = true;

	// uv = camera-relative xy * z + xy.
	const float extent = static_cast<float>(kSimSize * simTexel);
	flowWindow = { -cornerX / extent, -cornerY / extent, 1.0f / extent, 1.0f };
	return flowWindow;
}

void GroundFog::StepSim(ID3D11DeviceContext* a_context, bool a_terrain)
{
	if (!simPending)
		return;
	simPending = false;
	simData->SimFlags = a_terrain ? 1u : 0u;
	flowConstants->Update(*simData);

	const uint32_t next = flowCurrent ^ 1u;
	winrt::com_ptr<ID3D11Buffer> oldConstants;
	a_context->CSGetConstantBuffers(kFlowConstantSlot, 1, oldConstants.put());
	auto* constants = flowConstants->CB();
	a_context->CSSetConstantBuffers(kFlowConstantSlot, 1, &constants);
	static_assert(kFloorSlopeSlot == kFloorMapSlot + 1 && kFloorSeenSlot == kFloorMapSlot + 2);
	winrt::com_ptr<ID3D11ShaderResourceView> oldInputs[4];
	winrt::com_ptr<ID3D11ShaderResourceView> oldLift;
	a_context->CSGetShaderResources(kLiftSlot, 1, oldLift.put());
	ID3D11ShaderResourceView* lift = simLift[flowCurrent]->srv.get();
	a_context->CSSetShaderResources(kLiftSlot, 1, &lift);
	a_context->CSGetShaderResources(kFlowMapSlot, 1, oldInputs[0].put());
	a_context->CSGetShaderResources(kFloorMapSlot, 1, oldInputs[1].put());
	a_context->CSGetShaderResources(kFloorSlopeSlot, 1, oldInputs[2].put());
	a_context->CSGetShaderResources(kFloorSeenSlot, 1, oldInputs[3].put());
	ID3D11ShaderResourceView* state = simState[flowCurrent]->srv.get();
	ID3D11ShaderResourceView* floors[] = { simFloor[flowCurrent]->srv.get(), floorSlope[flowCurrent]->srv.get(), floorSeen->srv.get() };
	a_context->CSSetShaderResources(kFlowMapSlot, 1, &state);
	a_context->CSSetShaderResources(kFloorMapSlot, 3, floors);
	ID3D11UnorderedAccessView* outputs[] = { simState[next]->uav.get(), simFloor[next]->uav.get(), floorSlope[next]->uav.get(), simLift[next]->uav.get() };
	a_context->CSSetUnorderedAccessViews(0, 4, outputs, nullptr);
	a_context->CSSetShader(flowCS.get(), nullptr, 0);
	globals::profiler->BeginPass("GroundFog::Simulation");
	a_context->Dispatch((kSimSize + 7u) / 8u, (kSimSize + 7u) / 8u, 1);
	globals::profiler->EndPass();
	ID3D11UnorderedAccessView* nullOutputs[4]{};
	a_context->CSSetUnorderedAccessViews(0, 4, nullOutputs, nullptr);
	ID3D11ShaderResourceView* restoredLift = oldLift.get();
	a_context->CSSetShaderResources(kLiftSlot, 1, &restoredLift);
	ID3D11ShaderResourceView* restoredState = oldInputs[0].get();
	ID3D11ShaderResourceView* restoredFloors[] = { oldInputs[1].get(), oldInputs[2].get(), oldInputs[3].get() };
	a_context->CSSetShaderResources(kFlowMapSlot, 1, &restoredState);
	a_context->CSSetShaderResources(kFloorMapSlot, 3, restoredFloors);
	auto* previous = oldConstants.get();
	a_context->CSSetConstantBuffers(kFlowConstantSlot, 1, &previous);
	// What was seen of the floor has been taken in; the pixel pass starts from nothing.
	const float empty[4] = { 1.0e30f, 1.0e30f, 1.0e30f, 1.0e30f };
	a_context->ClearUnorderedAccessViewFloat(floorSeen->uav.get(), empty);

	flowCurrent = next;
	flowOriginX = simNextOriginX;
	flowOriginY = simNextOriginY;
	flowValid = true;
}

float GroundFog::TimeOfDayAmount(float& a_daylight) const
{
	a_daylight = 1.0f;
	const auto sky = globals::game::sky;
	if (!sky || !sky->currentClimate)
		return settings.MiddayAmount;
	const auto calendar = Util::Climate::GetCalendar();
	float hour = calendar && calendar->gameHour ? calendar->gameHour->value : sky->currentGameHour;
	if (!std::isfinite(hour))
		return settings.MiddayAmount;
	hour = std::fmod(std::max(hour, 0.0f), 24.0f);
	const auto& timing = sky->currentClimate->timing;
	const float sunriseBegin = timing.sunrise.begin * Util::Climate::HoursPerTimingUnit;
	const float sunriseEnd = std::max(timing.sunrise.end * Util::Climate::HoursPerTimingUnit, sunriseBegin + 0.25f);
	const float sunsetBegin = std::max(timing.sunset.begin * Util::Climate::HoursPerTimingUnit, sunriseEnd + kBurnOffHours + kReturnHours + 0.5f);
	const float sunsetEnd = std::min(std::max(timing.sunset.end * Util::Climate::HoursPerTimingUnit, sunsetBegin + 0.25f), 23.75f);
	const float night = 24.0f - sunsetEnd + sunriseBegin;
	auto blend = [](float a_from, float a_to, float a_x) { return std::lerp(a_from, a_to, Util::Smoothstep(0.0f, 1.0f, a_x)); };

	a_daylight = Util::Smoothstep(sunriseBegin, sunriseEnd, hour) * (1.0f - Util::Smoothstep(sunsetBegin, sunsetEnd, hour));
	// Heaviest from the small hours through sunrise, thinning through the morning, light by day, returning from
	// late afternoon and building through the night.
	if (hour < sunriseBegin)
		return blend(settings.EveningAmount, settings.MorningAmount, (hour + 24.0f - sunsetEnd) / night);
	if (hour < sunriseEnd)
		return settings.MorningAmount;
	if (hour < sunriseEnd + kBurnOffHours)
		return blend(settings.MorningAmount, settings.MiddayAmount, (hour - sunriseEnd) / kBurnOffHours);
	if (hour < sunsetBegin - kReturnHours)
		return settings.MiddayAmount;
	if (hour < sunsetEnd)
		return blend(settings.MiddayAmount, settings.EveningAmount, (hour - (sunsetBegin - kReturnHours)) / (sunsetEnd - sunsetBegin + kReturnHours));
	return blend(settings.EveningAmount, settings.MorningAmount, (hour - sunsetEnd) / night);
}

GroundFog::Status GroundFog::Update(VolumeData& a_data, ID3D11ShaderResourceView*& a_heightMap, ID3D11ShaderResourceView*& a_waterMap)
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
		windVelocity = { windDirection.x * speed, windDirection.y * speed };
		swirlTime = std::fmod(swirlTime + deltaTime, 36000.0);
		frameDeltaTime = deltaTime;
		breathTime = std::fmod(breathTime + deltaTime, static_cast<double>(kBreathPeriod));
		UpdateFloorOffset(deltaTime);
		timeAmount = TimeOfDayAmount(daylight);
	}

	const float windFactor = 1.0f - settings.WindResponse * Util::Smoothstep(kCalmWind, kStrongWind, windSpeed);
	effectiveAmount = settings.Amount * windFactor;
	const bool debug = debugMode != DebugMode::kOff;
	const bool slab = debugMode == DebugMode::kSlab || debugMode == DebugMode::kGroundHeight;
	// The valley layer exists in the pixel pass only.
	effectiveFarAmount = settings.VolumeInjection || slab ? 0.0f : settings.FarAmount * windFactor;
	// The near volume exists in the pixel pass only, and not in the slab views.
	const float nearDensity = settings.VolumeInjection || slab ? 0.0f : effectiveAmount * settings.NearDensity;
	if (!debug && effectiveAmount * settings.Density <= 0.0f && nearDensity <= 0.0f && effectiveFarAmount * settings.FarDensity <= 0.0f)
		return Status::kNoAmount;

	const float thickness = (slab ? kDebugThickness : std::max(settings.Thickness, 0.1f)) * unitsPerMeter;
	// The simulation view shows the pool as it is, so the sheet over it is not made dense.
	const float density = debug && debugMode < DebugMode::kSimulation ? std::max(effectiveAmount * settings.Density, kDebugDensity) : effectiveAmount * settings.Density;
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
	a_data.SunWrap = settings.SunDiffusion;
	a_data.SunLimit = settings.SunLimit;
	a_data.CanopyShade = settings.CanopyShade;
	a_data.Wispiness = slab ? 0.0f : settings.Wispiness;
	a_data.InvWispScale = 1.0f / (std::clamp(settings.WispScale, kMinWispScale, kMaxWispScale) * unitsPerMeter);
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
	// Up to kPoolMaxThickness the near layer is the pool; RenderPixel moves the density over once it knows the
	// simulation runs.
	poolActive = nearDensity > 0.0f && settings.NearThickness <= kPoolMaxThickness;
	a_data.NearExtinction = nearDensity * Util::Units::GAME_UNIT_TO_M;
	a_data.NearThickness = std::max(settings.NearThickness, kMinNearThickness) * unitsPerMeter;
	a_data.NearFalloff = std::clamp(settings.NearFalloff, kMinNearFalloff, kMaxNearFalloff);
	a_data.NearEnd = std::clamp(settings.NearDistance, kMinNearDistance, kMaxNearDistance) * unitsPerMeter;
	a_data.InvDriftScale = 1.0f / (std::max(settings.DriftScale, kMinDriftScale) * unitsPerMeter);
	a_data.DriftStrength = slab ? 0.0f : settings.DriftStrength;
	a_data.BreathPhase = static_cast<float>(breathTime / kBreathPeriod * 6.283185307179586);
	a_data.NearBreath = settings.Breathing;
	a_data.FloorAnchor = floorAnchor;
	a_data.FloorOffset = slab || settings.FloorMaxHeight <= 0.0f ? 0.0f : floorOffset;
	a_data.FloorMax = settings.FloorMaxHeight * unitsPerMeter;
	a_data.SoftLength = slab ? 0.0f : settings.SurfaceSoftness * unitsPerMeter;
	// Burn-off. Sunlit ground holds the amount for the time of day; the valley layer is never thinned below what
	// it holds without burn-off, so its midday look on open ground stays. Ground in terrain shadow keeps up to the
	// morning amount by day. All 1 and no shadow read when the strength is 0.
	const float burn = slab ? 0.0f : std::clamp(settings.BurnOffStrength, 0.0f, 1.0f);
	a_data.BurnSunlitNear = std::lerp(1.0f, timeAmount, burn);
	a_data.BurnSunlitFar = std::lerp(1.0f, timeAmount, burn);  // the valley layer burns off with the day as well
	a_data.BurnShaded = std::lerp(1.0f, settings.MorningAmount, burn);
	a_data.BurnRetention = settings.VolumeInjection ? 0.0f : burn * settings.ShadowRetention * daylight;
	// Water: from the baked map where the worldspace has one, else from the engine's cells around the camera.
	a_waterMap = EnsureWaterMap(map);
	a_data.InvShoreDepth = 1.0f / (std::clamp(settings.ShoreSoftness, kMinShoreSoftness, kMaxShoreSoftness) * unitsPerMeter);
	a_data.WaterPatchiness = slab ? 0.0f : settings.WaterPatchiness;
	a_data.WaterTexel = a_waterMap ? waterMapTexel : 0.0f;
	a_data.FlowWindow = {};
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
	ID3D11ShaderResourceView* water = nullptr;
	lastBindFrame = globals::state->frameCount;
	status = loaded && volumeConstants ? Update(data, heightMap, water) : Status::kDisabled;
	if (status != Status::kActive)
		return false;

	volumeConstants->Update(data);
	// The slots are free in the volume's own shader, but another pass may have left something in them.
	previousConstants = nullptr;
	previousHeightMap = nullptr;
	a_context->CSGetConstantBuffers(kConstantSlot, 1, previousConstants.put());
	a_context->CSGetShaderResources(kHeightMapSlot, 1, previousHeightMap.put());
	previousWaterMap = nullptr;
	a_context->CSGetShaderResources(kWaterMapSlot, 1, previousWaterMap.put());
	auto* constants = volumeConstants->CB();
	a_context->CSSetConstantBuffers(kConstantSlot, 1, &constants);
	a_context->CSSetShaderResources(kHeightMapSlot, 1, &heightMap);
	a_context->CSSetShaderResources(kWaterMapSlot, 1, &water);
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
	auto* water = previousWaterMap.get();
	a_context->CSSetShaderResources(kWaterMapSlot, 1, &water);
	previousConstants = nullptr;
	previousHeightMap = nullptr;
	previousWaterMap = nullptr;
	bound = false;
}

void GroundFog::ClearShaderCache()
{
	pixelCS = nullptr;
	pyramidCS = nullptr;
	pyramidFirstCS = nullptr;
	flowCS = nullptr;
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
		// The simulation does not use it; an unused define changes nothing there.
		if (globals::features::skylighting.loaded)
			defines.emplace_back("SKYLIGHTING", "");
		pixelCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\GroundFog\\GroundFogPixel.cs.hlsl", defines, "cs_5_0")));
		pyramidCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\GroundFog\\HeightPyramid.cs.hlsl", {}, "cs_5_0")));
		pyramidFirstCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\GroundFog\\HeightPyramid.cs.hlsl", { { "FIRST_LEVEL", "" } }, "cs_5_0")));
		// Optional: without it nothing moves, and a near layer thin enough to be the pool is marched like the tall
		// volume. It places the fog by the same rule as the pixel pass, so it takes the same defines.
		flowCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\GroundFog\\FlowSim.cs.hlsl", defines, "cs_5_0")));
	}
	return pixelCS && pyramidCS && pyramidFirstCS;
}

bool GroundFog::EnsureHeightPyramid(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_heightMap, ID3D11ShaderResourceView* a_waterMap)
{
	// The pyramid's bound includes the water surfaces of the water map it was built with.
	if (heightPyramid && heightPyramidSource.get() == a_heightMap && heightPyramidWater.get() == a_waterMap)
		return true;
	heightPyramid = nullptr;
	heightPyramidSource = nullptr;
	heightPyramidWater = nullptr;
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
		// The first level takes in the water surfaces (slot 1; reads as "no water" when there is no map).
		if (level == 0)
			a_context->CSSetShaderResources(1, 1, &a_waterMap);
		a_context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		a_context->CSSetShader(level == 0 ? pyramidFirstCS.get() : pyramidCS.get(), nullptr, 0);
		a_context->Dispatch((std::max(desc.Width >> level, 1u) + 7u) / 8u, (std::max(desc.Height >> level, 1u) + 7u) / 8u, 1);
		a_context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
		a_context->CSSetShaderResources(0, 1, &nullSrv);
		if (level == 0)
			a_context->CSSetShaderResources(1, 1, &nullSrv);
		previousLevel = thisLevel;
	}
	globals::profiler->EndPass();
	if (!built)
		return false;

	heightPyramid = std::move(pyramid);
	heightPyramidSource.copy_from(a_heightMap);
	heightPyramidWater.copy_from(a_waterMap);
	heightPyramidLevels = desc.MipLevels;
	return true;
}

bool GroundFog::EnsureTargets(uint32_t a_width, uint32_t a_height)
{
	if (fogTexture && fogAuxTexture && fogTexture->desc.Width == a_width && fogTexture->desc.Height == a_height)
		return true;
	fogTexture = nullptr;
	fogAuxTexture = nullptr;
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
	fogAuxTexture = make(DXGI_FORMAT_R16G16B16A16_FLOAT, "GroundFog::FogAux");
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
	ID3D11ShaderResourceView* water = nullptr;
	lastBindFrame = globals::state->frameCount;
	status = Update(data, heightMap, water);
	if (status != Status::kActive)
		return false;
	// The lighting reads the shared volume's inputs and flags of this frame.
	auto& atmosphere = globals::features::viewAtmosphere;
	if (!atmosphere.IsReady()) {
		status = Status::kNoVolume;
		return false;
	}
	auto& main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	if (!main.texture || !EnsureShaders() || !EnsureHeightPyramid(a_context, heightMap, water)) {
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
	// The simulation of this frame. The slab views show the fog unmoved.
	const bool slabView = debugMode == DebugMode::kSlab || debugMode == DebugMode::kGroundHeight;
	data.FlowWindow = slabView ? float4{} : PrepareSim(poolActive);
	const bool simulated = data.FlowWindow.w > 0.0f;
	if (poolActive && simulated) {
		// The near layer is the pool: drawn from the simulation on what the screen shows, not marched.
		data.PoolExtinction = data.NearExtinction;
		data.PoolThickness = data.NearThickness;
		data.PoolEnd = std::min(data.NearEnd, 0.46f * static_cast<float>(kSimSize * simTexel));
		data.NearExtinction = 0.0f;
	}
	volumeConstants->Update(data);

	winrt::com_ptr<ID3D11Buffer> oldConstants;
	a_context->CSGetConstantBuffers(kConstantSlot, 1, oldConstants.put());
	auto* constants = volumeConstants->CB();
	a_context->CSSetConstantBuffers(kConstantSlot, 1, &constants);
	static_assert(kFlowMapSlot == kHeightPyramidSlot + 1 && kWaterMapSlot == kFlowMapSlot + 1 && kFloorMapSlot == kWaterMapSlot + 1 && kFloorSlopeSlot == kFloorMapSlot + 1);
	winrt::com_ptr<ID3D11ShaderResourceView> oldFloorMaps[2];
	winrt::com_ptr<ID3D11ShaderResourceView> oldLift;
	a_context->CSGetShaderResources(kLiftSlot, 1, oldLift.put());
	a_context->CSGetShaderResources(kFloorMapSlot, 1, oldFloorMaps[0].put());
	a_context->CSGetShaderResources(kFloorSlopeSlot, 1, oldFloorMaps[1].put());
	// The Skylighting probes, for what stands over the fog. The shader reads them only when the atmosphere's flags
	// say they are valid this frame (the same condition as here).
	winrt::com_ptr<ID3D11ShaderResourceView> oldSkylighting;
	a_context->CSGetShaderResources(kSkylightingSlot, 1, oldSkylighting.put());
	auto& skylighting = globals::features::skylighting;
	ID3D11ShaderResourceView* probes = skylighting.loaded && skylighting.texProbeArray ? skylighting.texProbeArray->srv.get() : nullptr;
	a_context->CSSetShaderResources(kSkylightingSlot, 1, &probes);
	ID3D11ShaderResourceView* ground[] = { heightMap, heightPyramid->srv.get(), nullptr, water, nullptr, nullptr };
	a_context->CSSetShaderResources(kHeightMapSlot, 6, ground);
	atmosphere.BindLightingInputs(a_context);
	// The simulation places its fog by the same rule as the pixel pass, from the same inputs.
	if (simulated) {
		StepSim(a_context, true);
		ground[2] = simState[flowCurrent]->srv.get();
		ground[4] = simFloor[flowCurrent]->srv.get();
		ground[5] = floorSlope[flowCurrent]->srv.get();
		a_context->CSSetShaderResources(kHeightMapSlot, 6, ground);
		ID3D11ShaderResourceView* lift = simLift[flowCurrent]->srv.get();
		a_context->CSSetShaderResources(kLiftSlot, 1, &lift);
	}
	ID3D11UnorderedAccessView* outputs[] = { fogTexture->uav.get(), fogAuxTexture->uav.get(), simulated && data.PoolExtinction > 0.0f ? floorSeen->uav.get() : nullptr };
	a_context->CSSetUnorderedAccessViews(0, 3, outputs, nullptr);
	a_context->CSSetShader(pixelCS.get(), nullptr, 0);

	// The same active area as the composite under dynamic resolution.
	const auto scale = globals::game::frameBufferCached.GetDynamicResolutionParams1();
	const uint32_t width = (static_cast<uint32_t>(std::clamp(mainDesc.Width * scale.x, 1.0f, static_cast<float>(mainDesc.Width))) + divisor - 1u) / divisor;
	const uint32_t height = (static_cast<uint32_t>(std::clamp(mainDesc.Height * scale.y, 1.0f, static_cast<float>(mainDesc.Height))) + divisor - 1u) / divisor;
	globals::profiler->BeginPass("GroundFog::Pixel");
	a_context->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1);
	globals::profiler->EndPass();

	ID3D11UnorderedAccessView* nullOutputs[3]{};
	a_context->CSSetUnorderedAccessViews(0, 3, nullOutputs, nullptr);
	ID3D11ShaderResourceView* nullGround[6]{};
	nullGround[4] = oldFloorMaps[0].get();
	nullGround[5] = oldFloorMaps[1].get();
	a_context->CSSetShaderResources(kHeightMapSlot, 6, nullGround);
	ID3D11ShaderResourceView* restoredLift = oldLift.get();
	a_context->CSSetShaderResources(kLiftSlot, 1, &restoredLift);
	ID3D11ShaderResourceView* restoredSkylighting = oldSkylighting.get();
	a_context->CSSetShaderResources(kSkylightingSlot, 1, &restoredSkylighting);
	auto* previous = oldConstants.get();
	a_context->CSSetConstantBuffers(kConstantSlot, 1, &previous);
	compositeInputs[0] = fogTexture->srv.get();
	compositeInputs[1] = fogAuxTexture->srv.get();
	a_context->CSSetShaderResources(kCompositeFogSlot, 2, compositeInputs);
	return true;
}

void GroundFog::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	Util::HoverTip(T(TKEY("enabled_tooltip"), "Fog on the ground, outdoors: a soft body of it around you, a thin sheet beyond and pools in far valleys. Needs Atmosphere and Fog with Physical Sky running, and a 16-bit terrain height map for the worldspace."));

	ImGui::SliderFloat(T(TKEY("amount"), "Amount"), &settings.Amount, 0.0f, kMaxAmount, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	Util::HoverTip(T(TKEY("amount_tooltip"), "How much fog there is. This is the control to drive by weather, time of day and location. 0 removes it."));

	ImGui::SliderFloat(T(TKEY("density"), "Density"), &settings.Density, kMinDensity, kMaxDensity, "%.3f /m", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
	Util::HoverTip(T(TKEY("density_tooltip"), "How opaque the fog is at the ground when Amount is 1. At 0.06 you see about 50 m through it."));

	if (ImGui::TreeNodeEx(T(TKEY("near"), "Around You"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T(TKEY("near_density"), "Near Density"), &settings.NearDensity, 0.0f, kMaxNearDensity, "%.3f /m", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
		Util::HoverTip(T(TKEY("near_density_tooltip"), "How opaque the fog around you is at the ground when Amount is 1. This is the fog you see in the air between you and things nearby. 0 removes it and leaves the thin sheet."));

		ImGui::SliderFloat(T(TKEY("near_thickness"), "Near Thickness"), &settings.NearThickness, kMinNearThickness, kMaxNearThickness, "%.1f m", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("near_thickness_tooltip"), "Height above the ground at which the fog around you has thinned to nothing. Up to 1 m it is a pool at your feet: it lies on whatever ground you see, and it flows, swirls and parts around what moves through it. Above 1 m it is the tall, still volume, laid on the terrain map."));
		ImGui::TextDisabled("%s", settings.NearThickness <= kPoolMaxThickness ? T(TKEY("near_mode_pool"), "Drawn as a pool, from the simulation") : T(TKEY("near_mode_volume"), "Drawn as the tall volume, on the terrain map"));

		ImGui::SliderFloat(T(TKEY("near_falloff"), "Near Thinning with Height"), &settings.NearFalloff, kMinNearFalloff, kMaxNearFalloff, "%.1f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("near_falloff_tooltip"), "How strongly the fog around you pools low. Low values fill the whole height evenly; high values keep it at your feet."));

		ImGui::SliderFloat(T(TKEY("near_distance"), "Near Distance"), &settings.NearDistance, kMinNearDistance, kMaxNearDistance, "%.0f m", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("near_distance_tooltip"), "How far from you the tall fog reaches. It fades into the thin sheet over the second half of this distance."));

		ImGui::SliderFloat(T(TKEY("drift_strength"), "Patchiness"), &settings.DriftStrength, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("drift_strength_tooltip"), "How uneven the fog is: thicker and thinner patches that travel with the wind and pass you as you move. 0 is even fog."));

		ImGui::SliderFloat(T(TKEY("drift_scale"), "Patch Size"), &settings.DriftScale, kMinDriftScale, kMaxDriftScale, "%.0f m", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("drift_scale_tooltip"), "Width of the drifting patches."));

		ImGui::SliderFloat(T(TKEY("breathing"), "Breathing"), &settings.Breathing, 0.0f, kMaxBreathing, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("breathing_tooltip"), "Share by which the top of the fog around you slowly rises and falls, out of step from place to place."));

		ImGui::Checkbox(T(TKEY("reactive"), "Reacts to Movement"), &settings.Reactive);
		Util::HoverTip(T(TKEY("reactive_tooltip"), "You, other actors, arrows and spell projectiles push the fog about within the Near Distance. It piles up ahead of a runner and thins behind, follows a fireball and puffs outward where a projectile ends, then flows back and settles."));

		ImGui::SliderFloat(T(TKEY("reactive_strength"), "Reaction Strength"), &settings.ReactiveStrength, 0.0f, kMaxReactiveStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("reactive_strength_tooltip"), "How much of the fog movers stir away. At 0 they still push it about but none is lost."));

		ImGui::SliderFloat(T(TKEY("impulse_strength"), "Impulse Strength"), &settings.ImpulseStrength, 0.0f, kMaxImpulseStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("impulse_strength_tooltip"), "How hard single events strike the pool: a jump and a landing puff it up around the feet, a weapon swing throws it forward and up, a spell cast and a shout blow it outward, a projectile or a spell that lands scatters it in a ring. The fog that is thrown up settles back in a second or two. 0 leaves only the push and wake of moving bodies."));

		ImGui::SliderFloat(T(TKEY("settle_time"), "Settle Time"), &settings.SettleTime, kMinSettleTime, kMaxSettleTime, "%.1f s", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
		Util::HoverTip(T(TKEY("settle_time_tooltip"), "How long the pool takes to come back to its resting amount after it has been disturbed, and to follow the time of day, shade and water as you move. Short heals wakes quickly; long leaves trails."));

		ImGui::SliderFloat(T(TKEY("wispiness"), "Wispiness"), &settings.Wispiness, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("wispiness_tooltip"), "How broken up the pool is. At 1 it lies in separate wisps and tendrils with clear air between them, dense enough to see them drift, curl and part. At 0 it is an even layer with soft patches."));

		ImGui::SliderFloat(T(TKEY("wisp_scale"), "Wisp Size"), &settings.WispScale, kMinWispScale, kMaxWispScale, "%.1f m", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("wisp_scale_tooltip"), "Size of the wisps and of the gaps between them."));

		ImGui::SliderFloat(T(TKEY("pooling"), "Pooling"), &settings.Pooling, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("pooling_tooltip"), "How much the pool keeps to the low places of the ground you see. At 1 it gathers in dips, gutters and at the foot of slopes and over water, leaves a thin veil on level open ground and none on rises. At 0 it lies evenly everywhere."));

		ImGui::SliderFloat(T(TKEY("downhill_flow"), "Downhill Flow"), &settings.DownhillFlow, 0.0f, kMaxDownhillFlow, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("downhill_flow_tooltip"), "How strongly the pool runs down slopes, off steps and rocks and into the low places of the ground you see. 0 leaves it where it rests."));

		ImGui::SliderFloat(T(TKEY("swirl"), "Swirl"), &settings.Swirl, 0.0f, kMaxSwirl, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("swirl_tooltip"), "Eddies in the pool: slow ones in still fog, stronger ones where it is moving, so wakes curl up as they close. 0 is smooth flow."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("burn_off"), "Time of Day and Sun"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T(TKEY("burn_off_strength"), "Burn-off Strength"), &settings.BurnOffStrength, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("burn_off_strength_tooltip"), "How much the time of day and the sun change the amount of fog. 0 turns this off: the same fog at every hour, sun or shade."));

		ImGui::SliderFloat(T(TKEY("morning_amount"), "Morning Amount"), &settings.MorningAmount, 0.0f, kMaxTimeAmount, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("morning_amount_tooltip"), "Amount from the small hours through sunrise, when fog is heaviest."));

		ImGui::SliderFloat(T(TKEY("midday_amount"), "Midday Amount"), &settings.MiddayAmount, 0.0f, kMaxTimeAmount, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("midday_amount_tooltip"), "Amount on sunlit ground by day, reached three hours after sunrise. Valley fog is never thinned below what it holds with burn-off turned off."));

		ImGui::SliderFloat(T(TKEY("evening_amount"), "Evening Amount"), &settings.EveningAmount, 0.0f, kMaxTimeAmount, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("evening_amount_tooltip"), "Amount at dusk. From here it builds through the night to the morning amount."));

		ImGui::SliderFloat(T(TKEY("shadow_retention"), "Kept in Shadow"), &settings.ShadowRetention, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("shadow_retention_tooltip"), "By day, ground the terrain keeps in shadow holds this share of the morning amount, so fog lingers where the sun has not reached. Needs Terrain Shadows. The Layer Mask debug view shows it in blue."));
		ImGui::TreePop();
	}

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
		Util::HoverTip(T(TKEY("water_preference_tooltip"), "How much more fog lies over water than over land. 1 is the same, 0 keeps water clear. It comes in with the water's depth at the shore and through the patches below. With a water map for the worldspace this works at any distance; without one, water is only known in the cells around you."));

		ImGui::SliderFloat(T(TKEY("shore_softness"), "Shore Softness"), &settings.ShoreSoftness, kMinShoreSoftness, kMaxShoreSoftness, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("shore_softness_tooltip"), "Water depth over which the fog over water comes in at a shore. The edge follows the lake bed, so shallow shores get a wide soft edge and steep banks a narrow one."));

		ImGui::SliderFloat(T(TKEY("water_patchiness"), "Water Patchiness"), &settings.WaterPatchiness, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("water_patchiness_tooltip"), "How uneven the fog over water is: broad thick and thin areas, patches and wisps that drift with the wind. 0 is an even sheet over the whole lake."));

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

		ImGui::SliderFloat(T(TKEY("sun_diffusion"), "Sun Diffusion"), &settings.SunDiffusion, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("sun_diffusion_tooltip"), "Share of the sun and moon light that lit fog sends out evenly to all sides, as dense fog does, on top of the glow toward the light. It gives lit fog the colour of the low sun from any direction. 0 leaves only the glow toward the light and the sky light."));

		ImGui::SliderFloat(T(TKEY("sun_limit"), "Sun Limit"), &settings.SunLimit, 0.0f, kMaxSunLimit, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("sun_limit_tooltip"), "How much brighter fog in the sun can get than fog in the shade: the sun and moon light it scatters levels off at this many times its sky light. At 1 sunlit fog is at most twice as bright as shaded fog. 0 removes the limit."));

		ImGui::SliderFloat(T(TKEY("canopy_shade"), "Shade under Cover"), &settings.CanopyShade, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("canopy_shade_tooltip"), "How much trees, walls and roofs over the fog dim the sky light it takes, so fog under a forest canopy is a dim grey like the ground there and not a white sheet. From the Skylighting probes where that feature runs, and from how much of the ground around is in the sun's shadow. 0 lights all fog by the open sky."));

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
		Util::HoverTip(T(TKEY("surface_blend_distance_tooltip"), "Where what you see faces up, the fog settles onto it over this last stretch of each view ray instead of the mapped terrain, so it lies on rocks, roads and ground meshes. Walls, trunks and the sides of rocks are never treated as ground. 0 uses the terrain map alone."));

		ImGui::SliderFloat(T(TKEY("surface_max_height"), "Surface Max Height"), &settings.SurfaceMaxHeight, 0.0f, kMaxSurfaceHeight, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("surface_max_height_tooltip"), "Up-facing surfaces up to this far above the mapped terrain count as ground for the thin sheet; above twice this they do not."));

		ImGui::SliderFloat(T(TKEY("floor_max_height"), "Floor Max Height"), &settings.FloorMaxHeight, 0.0f, kMaxFloorHeight, "%.1f m", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("floor_max_height_tooltip"), "The fog around you also follows wide level floors up to this far above the mapped terrain: roads on fill, bridges, town floors, and the floor you stand on. Tops of rocks, barrels and walls are not floors. 0 keeps it on the mapped terrain."));

		ImGui::SliderFloat(T(TKEY("surface_softness"), "Softness against Objects"), &settings.SurfaceSoftness, 0.0f, kMaxSoftness, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("surface_softness_tooltip"), "The fog fades over this last stretch in front of anything that is not ground, so it does not draw the outline of what stands in it."));
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
		T(TKEY("debug_mode_tap_count"), "Tap Count"),
		T(TKEY("debug_mode_simulation"), "Simulation"),
		T(TKEY("debug_mode_floor_map"), "Floor Map"),
		T(TKEY("debug_mode_pool_depth"), "Pool Depth")
	};
	int mode = static_cast<int>(debugMode);
	if (ImGui::Combo(T(TKEY("debug_mode"), "Debug View"), &mode, modes, static_cast<int>(std::size(modes))))
		debugMode = static_cast<DebugMode>(std::clamp(mode, 0, static_cast<int>(std::size(modes)) - 1));
	Util::HoverTip(T(TKEY("debug_mode_tooltip"), "Slab: a dense, level-topped 1 m layer with no noise, to check the height map against the ground.\nGround Height: the slab colored by the sampled height, one stripe per 64 units.\nLayer Mask: the real fog, dense, green where it is the sheet or the fog around you, red where it is valley fog, blue where terrain shadow keeps it from burning off.\nTap Count: texture and depth reads per pixel of the per-pixel pass. Blue is 4 or fewer, green 34, yellow 64, red 94 or more.\nSimulation: the pool coloured by its simulation. Red is how fast the fog moves, green how much there is, blue marks what stands in the fog instead of being its floor.\nFloor Map: what the pool takes each pixel for, within its reach. Green is ground, red stands on a known floor (bright at the floor, dark at and above the top of the fog), yellow is ground partly counted as standing, blue has no known floor yet.\nPool Depth: how much pool fog lies along each pixel's ray. Black none, blue 0.05, green 0.5, yellow 1, red 2 (86 % opaque, the most)."));

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
	ImGui::Text("%s: %.2f, %s %.2f", T(TKEY("debug_time_amount"), "Time of day amount"), timeAmount, T(TKEY("debug_daylight"), "daylight"), daylight);
	ImGui::Text("%s: %.2f m", T(TKEY("debug_floor_offset"), "Floor under you above the map"), floorOffset * Util::Units::GAME_UNIT_TO_M);
	ImGui::Text("%s: %u / %u", T(TKEY("debug_flow_emitters"), "Things moving the fog"), flowEmitterCount, kMaxFlowEmitters);
	ImGui::Text("%s: %s, %.2f m", T(TKEY("debug_simulation"), "Simulation"), flowValid ? (poolActive ? T(TKEY("debug_simulation_pool"), "running, pool") : T(TKEY("debug_simulation_reactive"), "running, reaction only")) : T(TKEY("debug_simulation_off"), "off"), simTexel * Util::Units::GAME_UNIT_TO_M);
	ImGui::Text("%s: %.0f x %.0f, DXGI %d", T(TKEY("debug_height_map"), "Height map"), heightMapSize.x, heightMapSize.y, static_cast<int>(heightMapFormat));
	ImGui::Text("%s: %s", T(TKEY("debug_water_map"), "Water map"), waterMapState.empty() ? "-" : waterMapState.c_str());
	if (!waterMap)
		ImGui::TextUnformatted(T(TKEY("debug_water_fallback"), "Water comes from the cells around you only"));
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
	sanitize(settings.ShoreSoftness, defaults.ShoreSoftness, kMinShoreSoftness, kMaxShoreSoftness);
	sanitize(settings.WaterPatchiness, defaults.WaterPatchiness, 0.0f, 1.0f);
	sanitize(settings.Albedo, defaults.Albedo, 0.0f, 1.0f);
	sanitize(settings.SunScale, defaults.SunScale, 0.0f, kMaxLightScale);
	sanitize(settings.SkyAmbientScale, defaults.SkyAmbientScale, 0.0f, kMaxLightScale);
	sanitize(settings.SelfShadow, defaults.SelfShadow, 0.0f, kMaxLightScale);
	sanitize(settings.SunDiffusion, defaults.SunDiffusion, 0.0f, 1.0f);
	sanitize(settings.SunLimit, defaults.SunLimit, 0.0f, kMaxSunLimit);
	sanitize(settings.CanopyShade, defaults.CanopyShade, 0.0f, 1.0f);
	sanitize(settings.Wispiness, defaults.Wispiness, 0.0f, 1.0f);
	sanitize(settings.WispScale, defaults.WispScale, kMinWispScale, kMaxWispScale);
	sanitize(settings.GroundFill, defaults.GroundFill, 0.0f, kMaxGroundFill);
	sanitize(settings.SurfaceBlendDistance, defaults.SurfaceBlendDistance, 0.0f, kMaxSurfaceBlend);
	sanitize(settings.SurfaceMaxHeight, defaults.SurfaceMaxHeight, 0.0f, kMaxSurfaceHeight);
	sanitize(settings.SurfaceSoftness, defaults.SurfaceSoftness, 0.0f, kMaxSoftness);
	sanitize(settings.FloorMaxHeight, defaults.FloorMaxHeight, 0.0f, kMaxFloorHeight);
	sanitize(settings.NearDensity, defaults.NearDensity, 0.0f, kMaxNearDensity);
	sanitize(settings.NearThickness, defaults.NearThickness, kMinNearThickness, kMaxNearThickness);
	sanitize(settings.NearFalloff, defaults.NearFalloff, kMinNearFalloff, kMaxNearFalloff);
	sanitize(settings.NearDistance, defaults.NearDistance, kMinNearDistance, kMaxNearDistance);
	sanitize(settings.DriftScale, defaults.DriftScale, kMinDriftScale, kMaxDriftScale);
	sanitize(settings.DriftStrength, defaults.DriftStrength, 0.0f, 1.0f);
	sanitize(settings.Breathing, defaults.Breathing, 0.0f, kMaxBreathing);
	sanitize(settings.BurnOffStrength, defaults.BurnOffStrength, 0.0f, 1.0f);
	sanitize(settings.ShadowRetention, defaults.ShadowRetention, 0.0f, 1.0f);
	sanitize(settings.MorningAmount, defaults.MorningAmount, 0.0f, kMaxTimeAmount);
	sanitize(settings.MiddayAmount, defaults.MiddayAmount, 0.0f, kMaxTimeAmount);
	sanitize(settings.EveningAmount, defaults.EveningAmount, 0.0f, kMaxTimeAmount);
	sanitize(settings.ReactiveStrength, defaults.ReactiveStrength, 0.0f, kMaxReactiveStrength);
	sanitize(settings.ImpulseStrength, defaults.ImpulseStrength, 0.0f, kMaxImpulseStrength);
	sanitize(settings.SettleTime, defaults.SettleTime, kMinSettleTime, kMaxSettleTime);
	sanitize(settings.DownhillFlow, defaults.DownhillFlow, 0.0f, kMaxDownhillFlow);
	sanitize(settings.Swirl, defaults.Swirl, 0.0f, kMaxSwirl);
	sanitize(settings.Pooling, defaults.Pooling, 0.0f, 1.0f);
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
