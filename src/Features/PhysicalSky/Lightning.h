#pragma once

#include "Buffer.h"
#include "LightningPulse.h"

#include <atomic>

struct PhysicalSky;

/// One active discharge and one pending cloud source; lighting and thunder use only the active one.
class SkyLightning
{
public:
	struct Segment
	{
		float4 start;  // absolute game units, channel radius in w
		float4 end;    // absolute game units, relative energy in w
	};
	static_assert(sizeof(Segment) == 32);
	static constexpr uint32_t kCandidates = 32;
	static constexpr uint32_t kMaxSegments = 128;

	/// Installs the SE/AE weather-sound filter; unrelated weather sounds keep their normal path.
	static void InstallHooks();
	/// Creates the candidate readback and immutable-per-discharge channel upload.
	void SetupResources();
	/// Advances the pause-aware event and fills Physical Sky's shared shader data.
	/// `skyRendering`: Physical Sky draws this frame, so it may own the weather's lightning.
	/// `available`: the cloud renderer and lightning shaders can place and draw a discharge.
	void Update(PhysicalSky& sky, const std::string& worldspace, bool skyRendering, bool available);
	/// Submits a density-only placement query using the cloud renderer's current resources.
	void QuerySource(ID3D11ComputeShader* shader);
	/// Counts successful cloud updates, rather than display frames, for mask preparation.
	void CloudFrameComplete(bool historyValid);
	/// Replays the last channel seed for visual comparisons, without waiting for weather.
	void Replay();
	/// Searches for a new discharge using the current distance, size and type settings.
	void Trigger();
	/// Reports ownership even between flashes, so vanilla cannot start a second scheduler.
	bool OwnsWeather() const { return ownsWeather.load(std::memory_order_relaxed); }
	/// While owning the weather: clears the vanilla flash, zeroes the game settings that turn it into
	/// directional and ambient light, and stops a thunder sound already playing. Otherwise restores them.
	void SuppressVanilla(RE::Sky* sky);
	/// True while a flash lights the scene and for a few frames after it. Temporal histories that would keep
	/// the flash (GI accumulation, environment capture, exposure adaptation) hold or shorten on this.
	bool Transient() const { return transientFrames.load(std::memory_order_relaxed) > 0u; }

	/// The peak scene illuminance, in scene units, that one strike may deliver at the camera and at the strike itself.
	static constexpr float kMaxCameraIlluminance = 2.f;
	static constexpr float kMaxPeakIlluminance = 10.f;
	/// Core radius of the scene light's falloff, metres: E(d) = E0 * r^2 / (d^2 + r^2).
	static constexpr float kSoftRadius = 500.f;
	/// Frames after the last lit frame during which histories stay short.
	static constexpr uint32_t kTransientTail = 8;

	/// What the last frame of an active discharge resolved to; shown in the settings panel.
	struct Readout
	{
		float distance = 0;          // camera to the strike, km
		float cameraIlluminance = 0; // at full pulse, after limits
		float peakIlluminance = 0;   // at the emitter, after limits
		float sceneScale = 1, cloudScale = 1, boltScale = 1;  // what the limits removed
		float exposure = 1;          // pre-exposure the limits used; 1 = none available
		bool grounded = false, cloudBase = false;
		bool thunderWeather = false;
		uint32_t weatherFrequency = 0;  // raw vanilla thunder frequency byte
		uint32_t segments = 0;
	};
	const Readout& LastReadout() const { return readout; }
	ID3D11ShaderResourceView* Segments() const { return segments ? segments->SRV() : nullptr; }
	bool HasSource() const { return phase == Phase::Preparing || phase == Phase::Flashing; }

private:
	enum class Phase
	{
		Idle,
		Query,
		Readback,
		Preparing,
		Flashing
	};
	struct Thunder
	{
		RE::BSSoundHandle sound;
		RE::FormID descriptor = 0;
		RE::NiPoint3 position;
		double time = 0;
		float volume = 1.f;
		bool played = false;
	};
	std::atomic_bool ownsWeather = false;
	std::atomic_uint32_t transientFrames = 0;
	Readout readout;
	float flashExposure = 1.f;
	// Vanilla flash-to-light game settings, zeroed while this owns the weather.
	RE::Setting* flashDirectional = nullptr;
	RE::Setting* flashAmbient = nullptr;
	float savedFlashDirectional = 0, savedFlashAmbient = 0;
	bool flashSettingsResolved = false, flashSettingsZeroed = false;
	Phase phase = Phase::Idle;
	// The pending source owns the other mask lane, but never contributes light or sound.
	Phase pendingPhase = Phase::Idle;
	float4 pendingPositionRadius = {};
	float pendingHeight = 0, pendingChannelRadius = 0.12f;
	float2 pendingDistanceRange = {};
	bool pendingGrounded = false, pendingCloudBase = false;
	uint32_t pendingSeed = 0, pendingFrames = 0, activeLane = 0;
	std::string worldspace;
	uint32_t randomState = 0x82a1f37bu, eventSeed = 0, generation = 0, preparedFrames = 0;
	double time = 0, previousTime = 0, nextEvent = 0, flashStart = 0, queryStart = 0;
	double eventHazard = 0, targetHazard = 1;
	float sourceHeight = 0;
	float3 source = {}, ground = {}, color = { 0.78f, 0.82f, 1.f };
	float radius = 0;
	float channelRadius = 0.12f;
	float4 channelEmitter = {};
	float2 distanceRange = {};
	LightningPulse::Parameters pulseParameters;
	float lastPulse = 0;
	bool replay = false, grounded = false, cloudBase = false;
	std::vector<Segment> channel;
	std::vector<Thunder> thunder;
	eastl::unique_ptr<Buffer> candidates;
	eastl::unique_ptr<StructuredBuffer> segments;
	winrt::com_ptr<ID3D11Buffer> readback;

	float Random();
	void Cancel();
	void ReadSource(PhysicalSky& sky);
	void PromoteSource(PhysicalSky& sky);
	void BuildChannel(float bottomZ);
	void StartFlash(const PhysicalSky& physicalSky);
	/// Scene, cloud and bolt levels for the active discharge, with the absolute and exposure limits applied.
	void ResolvePhotometry(PhysicalSky& physicalSky, float intensity);
};
