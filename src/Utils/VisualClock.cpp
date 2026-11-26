#include "VisualClock.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>

#include "Features/SkySync.h"
#include "Globals.h"
#include "Utils/Format.h"
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
	std::atomic_bool gSkyUpdateSeen{ false };   // our Sky::Update hook has actually run at least once
	std::atomic_int  gLastRefusal{ -1 };        // last refusal reason logged (once per change)

	enum Refusal : int
	{
		kOk = 0,
		kNoSkyHook,
		kNoColorsHook,
		kSkyHookNeverRan,
		kNoCalendar,
		kSkySyncOff,
		kBadArgs,
	};

	const char* RefusalText(int a_reason)
	{
		switch (a_reason) {
		case kNoSkyHook:
			return "Sky Sync's Sky::Update hook is not installed (Sky Sync not loaded, EVLaS conflict, or detour failed)";
		case kNoColorsHook:
			return "the Sky::UpdateColors hook is not installed (detour failed)";
		case kSkyHookNeverRan:
			return "the Sky::Update hook is installed but has never run (another plugin may have replaced Sky::Update after us)";
		case kNoCalendar:
			return "the calendar (GameHour/GameDaysPassed/TimeScale) is not available";
		case kSkySyncOff:
			return "Sky Sync is disabled";
		case kBadArgs:
			return "the caller passed a non-finite time or rate";
		default:
			return "ok";
		}
	}

	// Where the first instruction of Sky::Update jumps (Detours and SKSE branches start with E9 rel32 or FF25),
	// and which module that is in: tells whether our detour or another plugin's sits on the function.
	std::string SkyUpdateEntry()
	{
		const auto addr = REL::RelocationID(25682, 26229).address();
		const auto* code = reinterpret_cast<const std::uint8_t*>(addr);
		std::uintptr_t target = 0;
		if (code[0] == 0xE9)
			target = addr + 5 + *reinterpret_cast<const std::int32_t*>(code + 1);
		else if (code[0] == 0xFF && code[1] == 0x25)
			target = *reinterpret_cast<const std::uintptr_t*>(addr + 6 + *reinterpret_cast<const std::int32_t*>(code + 2));
		if (!target)
			return fmt::format("starts with {:02X} {:02X} (not a jump: unhooked)", code[0], code[1]);
		// A Detours trampoline jumps again from its own page; follow one more E9 / FF25 hop.
		const auto* hop = reinterpret_cast<const std::uint8_t*>(target);
		if (hop[0] == 0xFF && hop[1] == 0x25)
			target = *reinterpret_cast<const std::uintptr_t*>(target + 6 + *reinterpret_cast<const std::int32_t*>(hop + 2));
		else if (hop[0] == 0xE9)
			target = target + 5 + *reinterpret_cast<const std::int32_t*>(hop + 1);
		HMODULE module = nullptr;
		wchar_t name[MAX_PATH]{};
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(target), &module) &&
			module) {
			GetModuleFileNameW(module, name, MAX_PATH);
		}
		const std::wstring path = name;
		const auto slash = path.find_last_of(L"\\/");
		const std::wstring file = slash == std::wstring::npos ? path : path.substr(slash + 1);
		return fmt::format("jumps to 0x{:X} in {}", target, file.empty() ? std::string("(no module: a trampoline page)") : Util::WStringToString(file));
	}

	void LogRefusal(int a_reason)
	{
		if (gLastRefusal.exchange(a_reason) == a_reason)
			return;
		if (a_reason == kOk) {
			logger::info("[Visual Clock] accepted: the sky follows the visual clock");
			return;
		}
		logger::warn("[Visual Clock] refused: {} (Sky::Update {})", RefusalText(a_reason), SkyUpdateEntry());
	}

	// Main thread only.
	bool  gWasActive = false;
	float gLastShownHour = 0.0f;  // hour the sky showed in the last active frame

	constexpr int64_t kStaleMs = 2000;
	constexpr double  kMinAheadDays = 1.0 / (24.0 * 60.0 * 60.0);  // one game second
	constexpr float   kJumpHours = 1.0f / 120.0f;                   // a discontinuity over half a game minute resyncs the sky

	int Reason()
	{
		if (!gSkyHook.load(std::memory_order_acquire))
			return kNoSkyHook;
		if (!gColorsHook.load(std::memory_order_acquire))
			return kNoColorsHook;
		if (!gSkyUpdateSeen.load(std::memory_order_acquire))
			return kSkyHookNeverRan;
		if (!gCalendarOk.load(std::memory_order_acquire))
			return kNoCalendar;
		if (!gSkySyncEnabled.load(std::memory_order_acquire))
			return kSkySyncOff;
		return kOk;
	}

	int64_t NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

	// Resolved on every call: RE::Calendar::GetSingleton() dereferences the engine's Calendar* global, which is null
	// until the game has created the calendar. globals::game::calendar is cached once in globals::ReInit (plugin load /
	// renderer init, before that) and so stays null: the cause of "calendar NOT available" in game (2026-11-25).
	// What was missing is logged once per change.
	RE::Calendar* Calendar()
	{
		auto*     calendar = RE::Calendar::GetSingleton();
		const int missing = !calendar ? 1 : !calendar->gameHour ? 2 : !calendar->gameDaysPassed ? 3 : !calendar->timeScale ? 4 : 0;
		static std::atomic_int lastMissing{ -1 };
		if (lastMissing.exchange(missing) != missing) {
			static constexpr const char* kWhat[] = { "available", "Calendar singleton null", "GameHour global null",
				"GameDaysPassed global null", "TimeScale global null" };
			logger::info("[Visual Clock] calendar: {}", kWhat[missing]);
		}
		return missing == 0 ? calendar : nullptr;
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

	bool Supported() { return Reason() == kOk; }

	void SetSkyHookInstalled(bool a_installed)
	{
		gSkyHook.store(a_installed, std::memory_order_release);
		logger::info("[Visual Clock] Sky::Update hook (Sky Sync) {}; Sky::Update {}", a_installed ? "installed" : "NOT installed", SkyUpdateEntry());
	}

	void SetColorsHookInstalled(bool a_installed)
	{
		gColorsHook.store(a_installed, std::memory_order_release);
		logger::info("[Visual Clock] Sky::UpdateColors hook {}", a_installed ? "installed" : "NOT installed");
	}

	void BeginSkyUpdate()
	{
		if (!gSkyUpdateSeen.exchange(true, std::memory_order_acq_rel))
			logger::info("[Visual Clock] Sky::Update hook is live (first call)");
		const bool skySyncEnabled = globals::features::skySync.settings.Enabled;
		const auto* calendar = Calendar();
		if (gSkySyncEnabled.exchange(skySyncEnabled, std::memory_order_acq_rel) != skySyncEnabled || gCalendarOk.load() != (calendar != nullptr))
			logger::info("[Visual Clock] Sky Sync {}, calendar {}", skySyncEnabled ? "enabled" : "disabled", calendar ? "available" : "NOT available");
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
	const int reason = !std::isfinite(a_daysPassed) || !std::isfinite(a_rate) ? kBadArgs : Reason();
	LogRefusal(reason);
	if (reason != kOk) {
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
	const int reason = Reason();
	LogRefusal(reason);
	return reason == kOk;
}
