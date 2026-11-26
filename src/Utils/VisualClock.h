// Visual clock: lets another plugin show game time passing faster than the world runs.
//
// Sky Above's fast-travel flight shows hours passing (sun, sky colours, cloud drift) but no longer runs
// the live world at TimeScale 5000 for it: at 5000x every loaded and low-process actor, Papyrus game-time
// update and spawn timer ran ~15 game hours per flight in real time, and the game crashed in the Havok
// broadphase after 14-21 flights (harness/RESEARCH-flight-durability-2026-11-25.md). The plugin keeps the
// game's TimeScale normal, publishes a visual clock that runs ahead of the game's own, and steps the real
// clock up to it at the landing.
//
// Nothing outside this module ever sees a changed game value:
//   - the calendar globals are never written;
//   - Sky::currentGameHour holds the visual hour only for the duration of the engine's Sky::UpdateColors
//     call (ColorsScope: saved before, restored after, main thread), so the time-of-day colours follow it;
//   - our own consumers (Sky Sync's sun/shadow direction and fades, exposure, Effects11, night sky, ground fog,
//     ENB helper, clouds) read the visual time from the per-frame snapshot (Current(), SkyHour()) instead of
//     the engine's fields, and only while a visual clock is active; otherwise their original code runs.
// The snapshot is taken once per frame on the main thread at the start of Sky::Update.

#pragma once

#include <cstdint>

namespace Util::VisualClock
{
	/** @brief One frame's view of the visual clock, taken on the main thread at the start of Sky::Update. */
	struct Frame
	{
		bool          active = false;  // fresh, supported, and ahead of the game's clock
		float         hoursAhead = 0.0f;
		float         hour = 12.0f;    // the hour the sky shows (0-24); only meaningful while active
		float         daysPassed = 0.0f;
		double        rate = 20.0;     // time rate sky motion follows (TimeScale-like); only meaningful while active
		std::uint32_t sequence = 0;    // publication the frame was taken from
	};

	/** @brief The current frame's snapshot (any thread, no game reads). Use its values only when active. */
	[[nodiscard]] Frame Current();

	/** @brief The hour our sky consumers should use: the visual hour while active, else the sky's own field. */
	[[nodiscard]] float SkyHour(const RE::Sky* a_sky);

	/** @brief Hooks installed (Sky::Update and Sky::UpdateColors), calendar usable, Sky Sync enabled (as of the last frame). */
	[[nodiscard]] bool Supported();

	/** @brief Set once Sky Sync's Sky::Update hook is installed. */
	void SetSkyHookInstalled(bool a_installed);

	/** @brief Set once the Sky::UpdateColors hook is installed. */
	void SetColorsHookInstalled(bool a_installed);

	/** @brief Main thread, before the engine's Sky::Update: takes this frame's snapshot. */
	void BeginSkyUpdate();

	/** @brief Main thread, around the engine's Sky::UpdateColors: the visual hour in Sky::currentGameHour for that call only. */
	class ColorsScope
	{
	public:
		explicit ColorsScope(RE::Sky* a_sky);
		~ColorsScope();
		ColorsScope(const ColorsScope&) = delete;
		ColorsScope& operator=(const ColorsScope&) = delete;

	private:
		RE::Sky* sky = nullptr;
		float    savedHour = 0.0f;
	};
}

// Exports for other plugins (resolved with GetProcAddress, like BottledShaders_GetCloudDeck). Versioned: a caller
// checks BottledShaders_VisualClockVersion() >= 2 and resolves the whole set before using any of it. (The
// unversioned BottledShaders_SetVisualClock was dropped: an early build exported it returning void, so its
// result could not be trusted.)
inline constexpr int kVisualClockVersion = 2;
extern "C" __declspec(dllexport) int BottledShaders_VisualClockVersion();
// a_daysPassed: the visual clock as an absolute GameDaysPassed value. a_rate: its current rate as a TimeScale.
// Returns false, and activates nothing, when the sky cannot follow a visual clock.
extern "C" __declspec(dllexport) bool BottledShaders_SetVisualClock2(double a_daysPassed, float a_rate);
extern "C" __declspec(dllexport) void BottledShaders_ClearVisualClock();
extern "C" __declspec(dllexport) bool BottledShaders_VisualClockSupported();
