// Visual clock: lets another plugin show game time passing faster than the world runs.
//
// Sky Above's fast-travel flight shows hours passing (sun, sky colours, cloud drift) but no longer runs
// the live world at TimeScale 5000 for it: at 5000x every loaded and low-process actor, Papyrus game-time
// update and spawn timer ran ~15 game hours per flight in real time, and the game crashed in the Havok
// broadphase after 14-21 flights (harness/RESEARCH-flight-durability-2026-11-25.md). The plugin keeps the
// game's TimeScale normal, publishes a visual clock that runs ahead of the game's own, and steps the real
// clock up to it at the landing.
//
// The calendar globals are never written here. Only the sky's own hour field (Sky::currentGameHour) shows the
// visual hour, and only between the engine's reads of it:
//   - BeginSkyUpdate (before Sky::Update): puts back the value the engine itself left last frame, so
//     Sky::Update always starts from its own state, and takes this frame's snapshot;
//   - ApplyToSky before Sky::UpdateColors (the time-of-day colour interpolation) and after Sky::Update
//     (Sky Sync's sun and shadow direction, exposure, Effects11, night sky and the renderers read it then).
// Weather timing, the moons' phases and everything outside the sky keep the real clock. Consumers read one
// coherent per-frame snapshot (Current()). Normal play is untouched.

#pragma once

#include <cstdint>

namespace Util::VisualClock
{
	/** @brief One frame's view of the visual clock, taken on the main thread at the start of Sky::Update. */
	struct Frame
	{
		bool          active = false;  // fresh, supported, and ahead of the game's clock
		float         hoursAhead = 0.0f;
		float         hour = 12.0f;    // the hour the sky shows (0-24); the game's when inactive
		float         daysPassed = 0.0f;
		double        rate = 20.0;     // time rate sky motion follows (TimeScale-like)
		std::uint32_t sequence = 0;    // publication the frame was taken from
	};

	/** @brief The current frame's snapshot (any thread). Take it once per calculation. */
	[[nodiscard]] Frame Current();

	/** @brief Whether the sky can follow a visual clock (hooks installed and Sky Sync enabled). */
	[[nodiscard]] bool Supported();

	/** @brief Set once Sky Sync's Sky::Update hook is installed. */
	void SetSkyHookInstalled(bool a_installed);

	/** @brief Main thread, before the engine's Sky::Update: restore the sky's own hour and take this frame's snapshot. */
	void BeginSkyUpdate(RE::Sky* a_sky);

	/** @brief Main thread, inside Sky::Update before Sky::UpdateColors, and after Sky::Update: show the visual hour. */
	void ApplyToSky(RE::Sky* a_sky);
}

// Exports for other plugins (resolved with GetProcAddress, like BottledShaders_GetCloudDeck).
// a_daysPassed: the visual clock as an absolute GameDaysPassed value. a_rate: its current rate as a TimeScale.
// Returns false, and activates nothing, when the sky cannot follow a visual clock.
extern "C" __declspec(dllexport) bool BottledShaders_SetVisualClock(double a_daysPassed, float a_rate);
extern "C" __declspec(dllexport) void BottledShaders_ClearVisualClock();
extern "C" __declspec(dllexport) bool BottledShaders_VisualClockSupported();
