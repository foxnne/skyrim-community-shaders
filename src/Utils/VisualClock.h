// Visual clock: lets another plugin show game time passing faster than the world runs.
//
// Sky Above's fast-travel flight shows hours passing (sun, sky colours, weather blend, cloud drift) but
// no longer runs the live world at TimeScale 5000 for it: at 5000x every loaded and low-process actor,
// Papyrus game-time update and spawn timer ran ~15 game hours per flight in real time, and the game
// crashed in the Havok broadphase after 14-21 flights (harness/RESEARCH-flight-durability-2026-11-25.md).
// The plugin keeps the game's TimeScale normal, publishes a visual clock that runs ahead of the game's
// own, and steps the real clock up to it at the landing. While the visual clock is ahead:
//   - Sky::Update runs with the calendar's GameHour/GameDaysPassed showing the visual time (SkySync's
//     Sky_Update hook), so the engine's own sun position, sky colours, moon phase and weather blend follow it;
//   - cloud drift and Sky Sync's fades follow the visual time rate instead of the game's TimeScale;
//   - the hour/day readers outside Sky::Update (ground fog, ENB helper, night sky palette) see the visual time.
// The plugin refreshes it every frame; it lapses by itself 2 s after the last refresh, and as soon as the
// real clock has caught up. Normal play is untouched.

#pragma once

namespace Util::VisualClock
{
	/** @brief True while a visual clock is published, fresh, and ahead of the game's own clock. */
	[[nodiscard]] bool Active();

	/** @brief Hours the visual clock runs ahead of the game's (0 when inactive). */
	[[nodiscard]] float HoursAhead();

	/** @brief The hour (0-24) the sky shows: the visual clock's while active, else the game's. */
	[[nodiscard]] float GameHour();

	/** @brief GameDaysPassed as the sky shows it. */
	[[nodiscard]] float DaysPassed();

	/** @brief The time rate (game seconds per real second, like TimeScale) sky motion should follow. */
	[[nodiscard]] double TimeRate();

	/** @brief Set once the Sky::Update hook that applies the visual clock is installed (Sky Sync). */
	void SetSkyHookInstalled(bool a_installed);

	/** @brief Swaps the calendar globals to the visual time for the duration of Sky::Update (main thread only). */
	class SkyScope
	{
	public:
		SkyScope();
		~SkyScope();
		SkyScope(const SkyScope&) = delete;
		SkyScope& operator=(const SkyScope&) = delete;

	private:
		bool  swapped = false;
		float savedHour = 0.0f;
		float savedDays = 0.0f;
	};
}

// Exports for other plugins (resolved with GetProcAddress, like BottledShaders_GetCloudDeck).
// a_daysPassed: the visual clock as an absolute GameDaysPassed value. a_rate: its current rate as a TimeScale.
extern "C" __declspec(dllexport) void BottledShaders_SetVisualClock(double a_daysPassed, float a_rate);
extern "C" __declspec(dllexport) void BottledShaders_ClearVisualClock();
// False when the sky cannot follow a visual clock (Sky Sync's Sky::Update hook is not installed).
extern "C" __declspec(dllexport) bool BottledShaders_VisualClockSupported();
