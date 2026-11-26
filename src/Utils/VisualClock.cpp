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
	// What the other plugin published. Written by Set/Clear (any thread), read by BeginSkyUpdate, under gMutex.
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

	std::atomic_bool gSkyHook{ false };
	std::atomic_bool gColorsHook{ false };
	std::atomic_bool gCalendarOk{ false };     // main-thread snapshot, per frame
	std::atomic_bool gSkySyncEnabled{ false };  // main-thread snapshot, per frame

	// Main thread only.
	bool  gWasActive = false;
	float gLastShownHour = 0.0f;  // hour the sky showed in the last active frame

	constexpr int64_t kStaleMs = 2000;
	constexpr double  kMinAheadDays = 1.0 / (24.0 * 60.0 * 60.0);  // one game second
	constexpr float   kJumpHours = 1.0f / 120.0f;                   // a discontinuity over half a game minute resyncs the sky

	int64_t NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

	RE::Calendar* Calendar()
	{
		auto* calendar = globals::game::calendar;
		return calendar && calendar->gameHour && calendar->gameDaysPassed && calendar->timeScale ? calendar : nullptr;
	}

	void Store(const Util::VisualClock::Frame& a_frame)
	{
		std::lock_guard lock{ gMutex };
		gFrame = a_frame;
	}
}

namespace Util::VisualClock
{
	Frame Current()
	{
		std::lock_guard lock{ gMutex };
		return gFrame;
	}

	float SkyHour(const RE::Sky* a_sky)
	{
		const Frame frame = Current();
		return frame.active ? frame.hour : (a_sky ? a_sky->currentGameHour : 12.0f);
	}

	bool Supported()
	{
		return gSkyHook.load(std::memory_order_acquire) && gColorsHook.load(std::memory_order_acquire) &&
		       gCalendarOk.load(std::memory_order_acquire) && gSkySyncEnabled.load(std::memory_order_acquire);
	}

	void SetSkyHookInstalled(bool a_installed) { gSkyHook.store(a_installed, std::memory_order_release); }

	void SetColorsHookInstalled(bool a_installed) { gColorsHook.store(a_installed, std::memory_order_release); }

	void BeginSkyUpdate()
	{
		gSkySyncEnabled.store(globals::features::skySync.settings.Enabled, std::memory_order_release);
		const auto* calendar = Calendar();
		gCalendarOk.store(calendar != nullptr, std::memory_order_release);

		Published published;
		{
			std::lock_guard lock{ gMutex };
			published = gPublished;
		}
		// Normal play: nothing published and nothing to leave, so no game reads beyond the two above.
		if (published.stampMs == 0 && !gWasActive)
			return;

		Frame frame;
		frame.sequence = published.sequence;
		const bool fresh = published.stampMs != 0 && NowMs() - published.stampMs <= kStaleMs;
		double     ahead = 0.0;
		if (calendar && fresh && Supported()) {
			ahead = published.targetDays - static_cast<double>(calendar->gameDaysPassed->value);
			if (std::isfinite(ahead) && ahead > kMinAheadDays) {
				ahead = std::min(ahead, 30.0);
				frame.active = true;
				frame.hoursAhead = static_cast<float>(ahead * 24.0);
				frame.hour = static_cast<float>(std::fmod(static_cast<double>(calendar->gameHour->value) + ahead * 24.0, 24.0));
				frame.daysPassed = static_cast<float>(static_cast<double>(calendar->gameDaysPassed->value) + ahead);
				const double gameRate = std::isfinite(calendar->timeScale->value) ? calendar->timeScale->value : 20.0;
				frame.rate = std::isfinite(published.rate) ? std::max(static_cast<double>(published.rate), gameRate) : gameRate;
			}
		}

		// Leaving the visual clock: the sky goes from the last hour it showed to the game's own hour. If that is a
		// jump (cleared or lapsed while still ahead, a load), Sky Sync resyncs sun, shadows and fades at once;
		// after a landing the real clock has reached the shown time and nothing is requested.
		if (gWasActive && !frame.active) {
			const char* reason = published.stampMs == 0 ? "cleared" : !fresh ? "lapsed" : !Supported() ? "unsupported" : "caught up";
			const float realHour = calendar ? calendar->gameHour->value : gLastShownHour;
			const float jump = std::abs(std::remainder(gLastShownHour - realHour, 24.0f));
			if (jump > kJumpHours)
				Util::RequestTimeJumpTransition();
			logger::info("[Visual Clock] ended ({}): sky {:.3f} h -> game {:.3f} h, jump {:.3f} h{}", reason, gLastShownHour, realHour, jump,
				jump > kJumpHours ? ", sky resynced" : "");
		} else if (!gWasActive && frame.active) {
			logger::info("[Visual Clock] started: {:.2f} h ahead", frame.hoursAhead);
		}
		gWasActive = frame.active;
		if (frame.active)
			gLastShownHour = frame.hour;
		Store(frame);
	}

	ColorsScope::ColorsScope(RE::Sky* a_sky)
	{
		if (!a_sky)
			return;
		const Frame frame = Current();
		if (!frame.active)
			return;
		sky = a_sky;
		savedHour = a_sky->currentGameHour;
		a_sky->currentGameHour = frame.hour;
	}

	ColorsScope::~ColorsScope()
	{
		if (sky)
			sky->currentGameHour = savedHour;
	}
}

extern "C" __declspec(dllexport) int BottledShaders_VisualClockVersion()
{
	return kVisualClockVersion;
}

extern "C" __declspec(dllexport) bool BottledShaders_SetVisualClock2(double a_daysPassed, float a_rate)
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
	if (gPublished.stampMs != 0) {
		gPublished.stampMs = 0;
		++gPublished.sequence;
	}
}

extern "C" __declspec(dllexport) bool BottledShaders_VisualClockSupported()
{
	return Util::VisualClock::Supported();
}
