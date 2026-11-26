#include "VisualClock.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

#include "Features/SkySync.h"
#include "Globals.h"
#include "Utils/Game.h"

namespace
{
	// What the other plugin published. Written by Set/Clear, read by BeginSkyUpdate, both under gMutex.
	struct Published
	{
		double        targetDays = 0.0;
		float         rate = 20.0f;
		int64_t       stampMs = 0;  // 0 = cleared
		std::uint32_t sequence = 0;
	};

	std::mutex               gMutex;
	Published                gPublished;  // guarded by gMutex
	Util::VisualClock::Frame gFrame;      // guarded by gMutex; written once per frame on the main thread
	std::atomic_bool         gSkyHook{ false };

	// Main thread only (Sky::Update hook).
	float gEngineHour = -1.0f;  // Sky::currentGameHour as the engine left it, before our override
	bool  gOverridden = false;  // the sky's hour holds the visual value right now
	float gLastAhead = 0.0f;    // hours ahead in the last active frame

	constexpr int64_t kStaleMs = 2000;
	constexpr double  kMinAheadDays = 1.0 / (24.0 * 60.0 * 60.0);  // one game second
	constexpr float   kJumpBackHours = 1.0f / 60.0f;                // a lapse further ahead than a game minute refreshes the sky

	int64_t NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

	RE::Calendar* Calendar()
	{
		auto* calendar = globals::game::calendar;
		return calendar && calendar->gameHour && calendar->gameDaysPassed && calendar->timeScale ? calendar : nullptr;
	}
}

namespace Util::VisualClock
{
	// While a visual clock runs, the frame's snapshot (taken once at the start of Sky::Update, so every reader in
	// the frame agrees; a Clear takes effect from the next frame). Otherwise the game's own live values, so
	// nothing changes without a visual clock, also where the Sky::Update hook is not installed.
	Frame Current()
	{
		Frame frame;
		{
			std::lock_guard lock{ gMutex };
			frame = gFrame;
		}
		if (!frame.active) {
			if (const auto* calendar = Calendar()) {
				frame.hour = calendar->gameHour->value;
				frame.daysPassed = calendar->gameDaysPassed->value;
				frame.rate = std::isfinite(calendar->timeScale->value) ? calendar->timeScale->value : 20.0;
			}
			frame.hoursAhead = 0.0f;
		}
		return frame;
	}

	bool Supported()
	{
		return gSkyHook.load(std::memory_order_acquire) && globals::features::skySync.settings.Enabled;
	}

	void SetSkyHookInstalled(bool a_installed) { gSkyHook.store(a_installed, std::memory_order_release); }

	void BeginSkyUpdate(RE::Sky* a_sky)
	{
		// The engine starts from its own value, whatever it does with the field.
		if (gOverridden && a_sky && gEngineHour >= 0.0f)
			a_sky->currentGameHour = gEngineHour;
		gOverridden = false;

		const auto* calendar = Calendar();
		Frame       frame;
		if (calendar) {
			frame.hour = calendar->gameHour->value;
			frame.daysPassed = calendar->gameDaysPassed->value;
			frame.rate = std::isfinite(calendar->timeScale->value) ? calendar->timeScale->value : 20.0;
		}
		Published published;
		{
			std::lock_guard lock{ gMutex };
			published = gPublished;
		}
		frame.sequence = published.sequence;
		if (calendar && published.stampMs != 0 && NowMs() - published.stampMs <= kStaleMs && Supported()) {
			const double ahead = published.targetDays - static_cast<double>(calendar->gameDaysPassed->value);
			if (std::isfinite(ahead) && ahead > kMinAheadDays) {
				const double clamped = std::min(ahead, 30.0);
				frame.active = true;
				frame.hoursAhead = static_cast<float>(clamped * 24.0);
				frame.hour = static_cast<float>(std::fmod(static_cast<double>(calendar->gameHour->value) + clamped * 24.0, 24.0));
				frame.daysPassed = static_cast<float>(static_cast<double>(calendar->gameDaysPassed->value) + clamped);
				if (std::isfinite(published.rate))
					frame.rate = std::max(static_cast<double>(published.rate), frame.rate);
			}
		}
		// Leaving the visual clock while still ahead (cleared, lapsed, or a load): the real clock is behind what
		// the sky showed, so Sky Sync takes the jump back as a time jump (sun, shadows and fades resync at once).
		if (!frame.active && gLastAhead > kJumpBackHours)
			Util::RequestTimeJumpTransition();
		gLastAhead = frame.active ? frame.hoursAhead : 0.0f;
		std::lock_guard lock{ gMutex };
		gFrame = frame;
	}

	void ApplyToSky(RE::Sky* a_sky)
	{
		if (!a_sky)
			return;
		const Frame frame = Current();
		if (!frame.active)
			return;
		// Remember the engine's own value: the first time this frame, or again if the engine wrote it since.
		if (!gOverridden || a_sky->currentGameHour != frame.hour) {
			gEngineHour = a_sky->currentGameHour;
			gOverridden = true;
		}
		a_sky->currentGameHour = frame.hour;
	}
}

extern "C" __declspec(dllexport) bool BottledShaders_SetVisualClock(double a_daysPassed, float a_rate)
{
	if (!Util::VisualClock::Supported() || !std::isfinite(a_daysPassed) || !std::isfinite(a_rate)) {
		BottledShaders_ClearVisualClock();
		return false;
	}
	std::lock_guard lock{ gMutex };
	gPublished.targetDays = a_daysPassed;
	gPublished.rate = a_rate;
	gPublished.stampMs = NowMs();
	++gPublished.sequence;
	return true;
}

extern "C" __declspec(dllexport) void BottledShaders_ClearVisualClock()
{
	std::lock_guard lock{ gMutex };
	gPublished.stampMs = 0;
	++gPublished.sequence;
}

extern "C" __declspec(dllexport) bool BottledShaders_VisualClockSupported()
{
	return Util::VisualClock::Supported();
}
