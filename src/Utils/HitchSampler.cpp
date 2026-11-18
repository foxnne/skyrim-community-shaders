#include "HitchSampler.h"

#include <atomic>
#include <cstring>
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
		constexpr size_t kStackWords = 768;  // 6 KB of stack above the stack pointer
		constexpr size_t kChain = 6;         // return addresses into this module kept per sample
		constexpr size_t kMaxSamples = 4096;

		struct Sample
		{
			uint64_t frame = 0;
			uintptr_t rip = 0;
			uintptr_t chain[kChain]{};  // innermost first, 0 = none
		};

		std::atomic<bool> g_active{ false };
		std::atomic<uint64_t> g_frame{ 0 };
		std::atomic<int64_t> g_frameStartQpc{ 0 };
		std::atomic<int64_t> g_lastFrameQpc{ 0 };  // duration of the frame that just ended
		std::atomic<uint64_t> g_lastFrameIndex{ 0 };
		HANDLE g_renderThread = nullptr;
		double g_qpcToMs = 0.0;
		uintptr_t g_base = 0, g_textBegin = 0, g_textEnd = 0;

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
			if (SuspendThread(g_renderThread) == static_cast<DWORD>(-1))
				return;
			// --- render thread suspended: only copy memory, nothing that can take a lock ---
			const bool ok = GetThreadContext(g_renderThread, &context) != 0;
			if (ok) {
				const auto* stack = reinterpret_cast<const uintptr_t*>(context.Rsp & ~uintptr_t{ 7 });
				// Copy at most 6 KB above the stack pointer, and never past the end of the committed region it is in
				// (VirtualQuery is a plain system call: it takes no user-mode lock).
				MEMORY_BASIC_INFORMATION info;
				if (VirtualQuery(stack, &info, sizeof(info)) != 0 && info.State == MEM_COMMIT) {
					const auto regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
					words = std::min(kStackWords, (regionEnd - reinterpret_cast<uintptr_t>(stack)) / sizeof(uintptr_t));
					std::memcpy(g_stackCopy, stack, words * sizeof(uintptr_t));
				}
			}
			ResumeThread(g_renderThread);
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
