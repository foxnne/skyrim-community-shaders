#include "HitchSampler.h"

#include <atomic>
#include <cstring>
#include <fstream>
#include <map>
#include <thread>

#include "Utils/FileSystem.h"

namespace HitchSampler
{
	namespace
	{
		// A frame older than this is sampled; a finished frame longer than kReportMs is written to the log
		constexpr double kSampleStartMs = 38.0;
		constexpr double kReportMs = 50.0;
		// Frames this long also get every sample written to SKSE/HitchSamples. 120 ms (was 300) so the fast-travel
		// move frame at the top of the arc (220-300 ms since spread attach, 2026-11-25) and the frames after it are kept.
		constexpr double kDumpMs = 120.0;
		constexpr size_t kStackWords = 4096;  // 32 KB of stack above the stack pointer (6 KB missed the grid-attach callers)
		constexpr size_t kChain = 6;          // return addresses into this module kept per sample
		constexpr size_t kExeChain = 24;      // into SkyrimSE.exe: deep enough to reach Main::Update from a Havok/IO wait
		constexpr size_t kReportExe = 6;      // the log line groups by the innermost few only (the dumps keep all)
		constexpr size_t kMaxSamples = 4096;

		struct Sample
		{
			uint64_t frame = 0;
			uintptr_t rip = 0;
			uintptr_t chain[kChain]{};  // innermost first, 0 = none
			uintptr_t exeChain[kExeChain]{};  // return addresses into SkyrimSE.exe, innermost first (2026-11-25: waits)
		};

		std::atomic<bool> g_active{ false };
		std::atomic<uint64_t> g_frame{ 0 };
		std::atomic<int64_t> g_frameStartQpc{ 0 };
		std::atomic<int64_t> g_lastFrameQpc{ 0 };  // duration of the frame that just ended
		std::atomic<uint64_t> g_lastFrameIndex{ 0 };
		HANDLE g_renderThread = nullptr;
		std::atomic<HANDLE> g_mainThread{ nullptr };  // sampled instead of the render thread once known
		double g_qpcToMs = 0.0;
		uintptr_t g_base = 0, g_textBegin = 0, g_textEnd = 0;
		uintptr_t g_exeBegin = 0, g_exeEnd = 0;

		// Written only by the watchdog thread
		Sample g_samples[kMaxSamples];
		size_t g_sampleCount = 0;
		uintptr_t g_stackCopy[kStackWords];

		int64_t Qpc()
		{
			LARGE_INTEGER v;
			QueryPerformanceCounter(&v);
			return v.QuadPart;
		}

		// A return address follows a call instruction: E8 rel32, or an FF /2 indirect call of 2 to 7 bytes
		bool FollowsCall(uintptr_t a_address)
		{
			const auto* p = reinterpret_cast<const uint8_t*>(a_address);
			if (p[-5] == 0xE8)
				return true;
			for (const int length : { 2, 3, 6, 7 })
				if (p[-length] == 0xFF && ((p[-length + 1] >> 3) & 7) == 2)
					return true;
			return false;
		}

		void TakeSample(uint64_t a_frame)
		{
			if (g_sampleCount >= kMaxSamples)
				return;
			CONTEXT context;
			context.ContextFlags = CONTEXT_CONTROL;
			size_t words = 0;
			HANDLE target = g_mainThread.load(std::memory_order_acquire);
			if (!target)
				target = g_renderThread;
			if (SuspendThread(target) == static_cast<DWORD>(-1))
				return;
			// --- render thread suspended: only copy memory, nothing that can take a lock ---
			const bool ok = GetThreadContext(target, &context) != 0;
			if (ok) {
				const auto* stack = reinterpret_cast<const uintptr_t*>(context.Rsp & ~uintptr_t{ 7 });
				// Copy at most kStackWords above the stack pointer, and never past the end of the committed region it is in
				// (VirtualQuery is a plain system call: it takes no user-mode lock).
				MEMORY_BASIC_INFORMATION info;
				if (VirtualQuery(stack, &info, sizeof(info)) != 0 && info.State == MEM_COMMIT) {
					const auto regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
					words = std::min(kStackWords, (regionEnd - reinterpret_cast<uintptr_t>(stack)) / sizeof(uintptr_t));
					std::memcpy(g_stackCopy, stack, words * sizeof(uintptr_t));
				}
			}
			ResumeThread(target);
			// --- resumed ---
			if (!ok)
				return;

			auto& sample = g_samples[g_sampleCount++];
			sample.frame = a_frame;
			sample.rip = context.Rip;
			size_t found = 0;
			for (size_t i = 0; i < words && found < kChain; ++i) {
				const uintptr_t value = g_stackCopy[i];
				if (value > g_textBegin + 8 && value < g_textEnd && FollowsCall(value))
					sample.chain[found++] = value;
			}
			for (; found < kChain; ++found)
				sample.chain[found] = 0;
			size_t exeFound = 0;
			for (size_t i = 0; i < words && exeFound < kExeChain; ++i) {
				const uintptr_t value = g_stackCopy[i];
				if (value > g_exeBegin + 8 && value < g_exeEnd && FollowsCall(value))
					sample.exeChain[exeFound++] = value;
			}
			for (; exeFound < kExeChain; ++exeFound)
				sample.exeChain[exeFound] = 0;
		}

		std::string ModuleOf(uintptr_t a_address)
		{
			HMODULE module = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(a_address), &module) || !module)
				return "?";
			wchar_t path[MAX_PATH];
			const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
			const auto name = std::filesystem::path(std::wstring(path, length)).filename().string();
			return std::format("{}+{:X}", name, a_address - reinterpret_cast<uintptr_t>(module));
		}

		void Report(uint64_t a_frame, double a_frameMs)
		{
			// Group this frame's samples by call chain into this module (or by the module of the instruction pointer)
			std::map<std::string, int> groups;
			int count = 0;
			for (size_t i = 0; i < g_sampleCount; ++i) {
				const auto& sample = g_samples[i];
				if (sample.frame != a_frame)
					continue;
				++count;
				std::string key;
				const bool inside = sample.rip >= g_textBegin && sample.rip < g_textEnd;
				key = inside ? std::format("at +{:X}", sample.rip - g_base) : std::format("in {}", ModuleOf(sample.rip));
				for (const auto address : sample.chain) {
					if (!address)
						break;
					key += std::format(" < +{:X}", address - g_base);
				}
				if (sample.exeChain[0]) {
					key += " | exe";
					for (size_t k = 0; k < kReportExe; ++k) {
						const auto address = sample.exeChain[k];
						if (!address)
							break;
						key += std::format(" < {:X}", address - g_exeBegin);
					}
				}
				++groups[key];
			}
			if (count == 0)
				return;
			std::vector<std::pair<int, std::string>> sorted;
			for (auto& [key, n] : groups)
				sorted.emplace_back(n, key);
			std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
			std::string text;
			for (size_t i = 0; i < sorted.size() && i < 5; ++i)
				text += std::format("{}{}% {}", i ? " | " : "", sorted[i].first * 100 / count, sorted[i].second);
			logger::info("[HitchSampler] frame {} took {:.0f} ms, {} samples from {:.0f} ms on: {}", a_frame, a_frameMs, count, kSampleStartMs, text);

			// Long frames: every sample, one per line, for counting offline (harness/hitch_report.py --dump):
			// the five biggest groups above cover only a fifth of a fast-travel move frame (2026-11-25).
			if (a_frameMs >= kDumpMs) {
				const auto logDir = logger::log_directory();
				const auto dir = (logDir ? *logDir : std::filesystem::path{}) / "HitchSamples";
				std::error_code ec;
				std::filesystem::create_directories(dir, ec);
				// Local wall-clock time in the name, to match a dump to a line of another plugin's log (Sky Above "Move timing")
				SYSTEMTIME now;
				GetLocalTime(&now);
				const auto name = std::format("frame{}_{:.0f}ms_{:02}{:02}{:02}.{:03}.txt", a_frame, a_frameMs, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
				if (std::ofstream out(dir / name); out) {
					for (size_t i = 0; i < g_sampleCount; ++i) {
						const auto& sample = g_samples[i];
						if (sample.frame != a_frame)
							continue;
						const bool inside = sample.rip >= g_textBegin && sample.rip < g_textEnd;
						const bool inExe = sample.rip >= g_exeBegin && sample.rip < g_exeEnd;
						out << (inside ? std::format("cs:{:X}", sample.rip - g_base) : inExe ? std::format("exe:{:X}", sample.rip - g_exeBegin) : ModuleOf(sample.rip));
						out << " |";
						for (const auto address : sample.chain)
							if (address)
								out << std::format(" {:X}", address - g_base);
						out << " |";
						for (const auto address : sample.exeChain)
							if (address)
								out << std::format(" {:X}", address - g_exeBegin);
						out << '\n';
					}
				}
			}
		}

		void Watchdog()
		{
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
			uint64_t reported = 0;
			while (g_active.load(std::memory_order_relaxed)) {
				Sleep(1);
				const uint64_t frame = g_frame.load(std::memory_order_acquire);
				const double ageMs = (Qpc() - g_frameStartQpc.load(std::memory_order_acquire)) * g_qpcToMs;
				if (ageMs > kSampleStartMs && ageMs < 5000.0)  // not loading screens or a paused game
					TakeSample(frame);

				// A frame finished: report it if it was long, then drop its samples
				const uint64_t ended = g_lastFrameIndex.load(std::memory_order_acquire);
				if (ended != reported) {
					reported = ended;
					const double ms = g_lastFrameQpc.load(std::memory_order_acquire) * g_qpcToMs;
					if (ms >= kReportMs && ms < 5000.0)
						Report(ended, ms);
					size_t kept = 0;
					for (size_t i = 0; i < g_sampleCount; ++i)
						if (g_samples[i].frame > ended)
							g_samples[kept++] = g_samples[i];
					g_sampleCount = kept;
				}
			}
		}
	}

	void Install()
	{
		try {
			if (!std::filesystem::exists(Util::PathHelpers::GetCommunityShaderPath() / "HitchSample.on"))
				return;
			HMODULE self = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&Install), &self) || !self)
				return;
			g_base = reinterpret_cast<uintptr_t>(self);
			const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
			const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_base + dos->e_lfanew);
			g_textBegin = g_base + nt->OptionalHeader.BaseOfCode;
			g_textEnd = g_textBegin + nt->OptionalHeader.SizeOfCode;
			// The game image (its code is decrypted in memory; the whole image range is scanned, call-site checked)
			g_exeBegin = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
			if (g_exeBegin) {
				const auto edos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_exeBegin);
				const auto ent = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_exeBegin + edos->e_lfanew);
				g_exeEnd = g_exeBegin + ent->OptionalHeader.SizeOfImage;
			}
			LARGE_INTEGER frequency;
			QueryPerformanceFrequency(&frequency);
			g_qpcToMs = 1000.0 / static_cast<double>(frequency.QuadPart);
			g_active = true;
			logger::info("[HitchSampler] armed: frames over {:.0f} ms are reported, addresses are relative to this DLL (code +{:X}..+{:X})",
				kReportMs, g_textBegin - g_base, g_textEnd - g_base);
		} catch (const std::exception& e) {
			logger::warn("[HitchSampler] install failed: {}", e.what());
		}
	}

	void OnMainUpdate()
	{
		if (!g_active.load(std::memory_order_relaxed) || g_mainThread.load(std::memory_order_relaxed))
			return;
		if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId())) {
			g_mainThread.store(h, std::memory_order_release);
			logger::info("[HitchSampler] sampling the main thread {}", GetCurrentThreadId());
		}
	}

	void OnPresent()
	{
		if (!g_active.load(std::memory_order_relaxed))
			return;
		const int64_t now = Qpc();
		if (!g_renderThread) {
			// First Present: this is the render thread. Open a real handle to it and start the watchdog.
			g_renderThread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
			if (!g_renderThread) {
				g_active = false;
				return;
			}
			g_frameStartQpc.store(now, std::memory_order_release);
			std::thread(Watchdog).detach();
			return;
		}
		const int64_t start = g_frameStartQpc.load(std::memory_order_relaxed);
		const uint64_t frame = g_frame.load(std::memory_order_relaxed);
		g_lastFrameQpc.store(now - start, std::memory_order_release);
		g_frameStartQpc.store(now, std::memory_order_release);
		g_frame.store(frame + 1, std::memory_order_release);
		g_lastFrameIndex.store(frame, std::memory_order_release);
	}
}
