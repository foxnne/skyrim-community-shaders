#include "VisualClock.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

#include "Globals.h"

namespace
{
	std::atomic<double>  gTargetDays{ 0.0 };
	std::atomic<float>   gRate{ 20.0f };
	std::atomic<int64_t> gStampMs{ 0 };  // 0 = cleared
	std::atomic_bool     gSkyHook{ false };

	constexpr int64_t kStaleMs = 2000;
	constexpr double  kMinAheadDays = 1.0 / (24.0 * 60.0 * 60.0);  // one game second

	int64_t NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

	RE::Calendar* Calendar()
	{
		auto* calendar = globals::game::calendar;
		return calendar && calendar->gameHour && calendar->gameDaysPassed ? calendar : nullptr;
	}

	bool Fresh()
	{
		const int64_t stamp = gStampMs.load(std::memory_order_acquire);
		return stamp != 0 && NowMs() - stamp <= kStaleMs;
	}

	// Days the visual clock is ahead of the game's own (<= 0: not ahead).
	double AheadDays()
	{
		const auto* calendar = Calendar();
		if (!calendar || !Fresh())
			return 0.0;
		const double ahead = gTargetDays.load(std::memory_order_relaxed) - static_cast<double>(calendar->gameDaysPassed->value);
		return std::isfinite(ahead) && ahead > kMinAheadDays ? std::min(ahead, 30.0) : 0.0;
	}
}

namespace Util::VisualClock
{
	bool Active() { return AheadDays() > 0.0; }

	void SetSkyHookInstalled(bool a_installed) { gSkyHook.store(a_installed, std::memory_order_release); }

	float HoursAhead() { return static_cast<float>(AheadDays() * 24.0); }

	float GameHour()
	{
		const auto* calendar = Calendar();
		if (!calendar)
			return 12.0f;
		const float hour = calendar->gameHour->value;
		const double ahead = AheadDays();
		return ahead > 0.0 ? static_cast<float>(std::fmod(static_cast<double>(hour) + ahead * 24.0, 24.0)) : hour;
	}

	float DaysPassed()
	{
		const auto* calendar = Calendar();
		if (!calendar)
			return 0.0f;
		return calendar->gameDaysPassed->value + static_cast<float>(AheadDays());
	}

	double TimeRate()
	{
		const auto* calendar = globals::game::calendar;
		const double game = calendar && calendar->timeScale && std::isfinite(calendar->timeScale->value) ? calendar->timeScale->value : 20.0;
		if (!Active())
			return game;
		const double rate = gRate.load(std::memory_order_relaxed);
		return std::isfinite(rate) ? std::max(rate, game) : game;
	}

	// Sky::Update reads the hour (sun, colours, weather timing) and the days passed (moon phase) from the
	// calendar globals; for its duration they show the visual clock, then the game's values come back
	// unchanged. Main thread, inside the engine's own Sky::Update call.
	SkyScope::SkyScope()
	{
		auto* calendar = Calendar();
		const double ahead = AheadDays();
		if (!calendar || ahead <= 0.0)
			return;
		savedHour = calendar->gameHour->value;
		savedDays = calendar->gameDaysPassed->value;
		calendar->gameHour->value = static_cast<float>(std::fmod(static_cast<double>(savedHour) + ahead * 24.0, 24.0));
		calendar->gameDaysPassed->value = static_cast<float>(static_cast<double>(savedDays) + ahead);
		swapped = true;
	}

	SkyScope::~SkyScope()
	{
		if (!swapped)
			return;
		if (auto* calendar = Calendar()) {
			calendar->gameHour->value = savedHour;
			calendar->gameDaysPassed->value = savedDays;
		}
	}
}

extern "C" __declspec(dllexport) void BottledShaders_SetVisualClock(double a_daysPassed, float a_rate)
{
	if (!std::isfinite(a_daysPassed) || !std::isfinite(a_rate)) {
		BottledShaders_ClearVisualClock();
		return;
	}
	gTargetDays.store(a_daysPassed, std::memory_order_relaxed);
	gRate.store(a_rate, std::memory_order_relaxed);
	gStampMs.store(NowMs(), std::memory_order_release);
}

extern "C" __declspec(dllexport) void BottledShaders_ClearVisualClock()
{
	gStampMs.store(0, std::memory_order_release);
}

extern "C" __declspec(dllexport) bool BottledShaders_VisualClockSupported()
{
	return gSkyHook.load(std::memory_order_acquire);
}
