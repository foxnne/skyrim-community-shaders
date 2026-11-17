#include "VramTracker.h"

#include "Utils/FileSystem.h"

#include <DirectXTex.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <dxgi1_4.h>
#include <fstream>
#include <intrin.h>
#include <format>
#include <mutex>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <wrl/client.h>

namespace VramTracker
{
	namespace
	{
		constexpr int kMaxModules = 24;  // last slot collects everything beyond the table
		enum Cat : int
		{
			kRT,
			kUAV,
			kStaging,
			kTex512,
			kTex1K,
			kTex2K,
			kTex4KPlus,
			kCube,
			kTex3D,
			kTex1D,
			kTexDyn,
			kBufDyn,
			kBufGeom,
			kBufCB,
			kBufUAV,
			kBufOther,
			kCats
		};
		constexpr const char* kCatNames[kCats] = { "rt", "uav", "staging", "tex<=512", "tex1k", "tex2k", "tex4k+", "cube", "tex3d", "tex1d", "tex_dyn", "buf_dyn", "buf_geom", "buf_cb", "buf_uav", "buf_other" };

		struct Slot
		{
			std::atomic<int64_t> liveBytes{ 0 }, liveCount{ 0 }, newBytes{ 0 }, newCount{ 0 }, freedBytes{ 0 };
		};
		Slot g_slots[kMaxModules][kCats];

		std::mutex g_modMutex;
		std::vector<std::pair<HMODULE, std::string>> g_mods;

		bool g_active = false;

		// Engine call sites of large allocations: up to three SkyrimSE.exe return addresses (innermost first)
		struct Site
		{
			uint32_t rva[3]{};
			int64_t windowBytes = 0, windowCount = 0, totalBytes = 0;
			int cat = 0;
		};
		std::mutex g_siteMutex;
		std::unordered_map<uint64_t, Site> g_sites;
		uintptr_t g_exeBase = 0, g_exeEnd = 0;
		constexpr int64_t kSiteMinBytes = 64 * 1024;

		void RecordSite(int a_cat, int64_t a_bytes)
		{
			if (a_bytes < kSiteMinBytes || !g_exeBase)
				return;
			void* frames[24];
			const USHORT count = RtlCaptureStackBackTrace(2, 24, frames, nullptr);
			Site site;
			int found = 0;
			uint64_t key = static_cast<uint64_t>(a_cat) * 0x9E3779B97F4A7C15ull;
			for (USHORT i = 0; i < count && found < 3; ++i) {
				const auto address = reinterpret_cast<uintptr_t>(frames[i]);
				if (address >= g_exeBase && address < g_exeEnd) {
					site.rva[found++] = static_cast<uint32_t>(address - g_exeBase);
					key = (key ^ site.rva[found - 1]) * 0x100000001b3ull;
				}
			}
			if (!found)
				return;
			std::lock_guard lock(g_siteMutex);
			auto& entry = g_sites[key];
			if (entry.totalBytes == 0) {
				std::memcpy(entry.rva, site.rva, sizeof(site.rva));
				entry.cat = a_cat;
			}
			entry.windowBytes += a_bytes;
			entry.windowCount += 1;
			entry.totalBytes += a_bytes;
		}
		Microsoft::WRL::ComPtr<IDXGIAdapter3> g_adapter3;
		std::filesystem::path g_outPath;

		// {7d3b2f6e-4c1a-4d6e-9b0f-5a2c8e1f3a77}
		const GUID kTagGuid = { 0x7d3b2f6e, 0x4c1a, 0x4d6e, { 0x9b, 0x0f, 0x5a, 0x2c, 0x8e, 0x1f, 0x3a, 0x77 } };

		int ModuleId(void* a_addr)
		{
			HMODULE h = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(a_addr), &h))
				h = nullptr;
			std::lock_guard lock(g_modMutex);
			for (size_t i = 0; i < g_mods.size(); ++i)
				if (g_mods[i].first == h)
					return static_cast<int>(i);
			if (g_mods.size() >= kMaxModules - 1)
				return kMaxModules - 1;
			wchar_t buf[MAX_PATH] = {};
			std::string name = "unknown";
			if (h && GetModuleFileNameW(h, buf, MAX_PATH)) {
				auto p = std::filesystem::path(buf).filename().string();
				name = p;
			}
			g_mods.emplace_back(h, name);
			return static_cast<int>(g_mods.size() - 1);
		}

		struct Tag : IUnknown
		{
			std::atomic<ULONG> rc{ 1 };
			int mod = 0, cat = 0;
			int64_t bytes = 0;
			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_riid, void** a_out) override
			{
				if (a_riid == __uuidof(IUnknown)) {
					*a_out = this;
					AddRef();
					return S_OK;
				}
				*a_out = nullptr;
				return E_NOINTERFACE;
			}
			ULONG STDMETHODCALLTYPE AddRef() override { return ++rc; }
			ULONG STDMETHODCALLTYPE Release() override
			{
				const ULONG n = --rc;
				if (n == 0) {
					auto& s = g_slots[mod][cat];
					s.liveBytes -= bytes;
					s.liveCount -= 1;
					s.freedBytes += bytes;
					delete this;
				}
				return n;
			}
		};

		void Track(ID3D11DeviceChild* a_res, void* a_ret, int a_cat, int64_t a_bytes)
		{
			if (!a_res)
				return;
			auto* t = new Tag;
			t->mod = ModuleId(a_ret);
			t->cat = a_cat;
			t->bytes = a_bytes;
			auto& s = g_slots[t->mod][a_cat];
			s.liveBytes += a_bytes;
			s.liveCount += 1;
			s.newBytes += a_bytes;
			s.newCount += 1;
			a_res->SetPrivateDataInterface(kTagGuid, t);
			if (a_cat != kStaging)
				RecordSite(a_cat, a_bytes);
			t->Release();  // the resource now owns the reference
		}

		int64_t Texture2DBytes(const D3D11_TEXTURE2D_DESC& d)
		{
			UINT mips = d.MipLevels;
			if (mips == 0) {
				UINT m = std::max(d.Width, d.Height);
				mips = 1;
				while (m > 1) {
					m >>= 1;
					++mips;
				}
			}
			const bool comp = DirectX::IsCompressed(d.Format);
			const size_t bpp = DirectX::BitsPerPixel(d.Format);
			int64_t total = 0;
			for (UINT m = 0; m < mips; ++m) {
				UINT w = std::max(1u, d.Width >> m), h = std::max(1u, d.Height >> m);
				if (comp) {
					w = (w + 3) & ~3u;
					h = (h + 3) & ~3u;
				}
				total += static_cast<int64_t>(w) * h * bpp / 8;
			}
			return total * d.ArraySize * std::max(1u, d.SampleDesc.Count);
		}

		struct CreateBufferHook
		{
			static HRESULT STDMETHODCALLTYPE thunk(ID3D11Device* a_this, const D3D11_BUFFER_DESC* a_desc, const D3D11_SUBRESOURCE_DATA* a_init, ID3D11Buffer** a_out)
			{
				void* ret = _ReturnAddress();
				const HRESULT hr = func(a_this, a_desc, a_init, a_out);
				if (SUCCEEDED(hr) && a_out && *a_out && a_desc) {
					int cat = kBufOther;
					if (a_desc->Usage == D3D11_USAGE_STAGING)
						cat = kStaging;
					else if (a_desc->Usage == D3D11_USAGE_DYNAMIC)
						cat = kBufDyn;
					else if (a_desc->BindFlags & D3D11_BIND_UNORDERED_ACCESS)
						cat = kBufUAV;
					else if (a_desc->BindFlags & D3D11_BIND_CONSTANT_BUFFER)
						cat = kBufCB;
					else if (a_desc->BindFlags & (D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER))
						cat = kBufGeom;
					Track(*a_out, ret, cat, a_desc->ByteWidth);
				}
				return hr;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct CreateTexture1DHook
		{
			static HRESULT STDMETHODCALLTYPE thunk(ID3D11Device* a_this, const D3D11_TEXTURE1D_DESC* a_desc, const D3D11_SUBRESOURCE_DATA* a_init, ID3D11Texture1D** a_out)
			{
				void* ret = _ReturnAddress();
				const HRESULT hr = func(a_this, a_desc, a_init, a_out);
				if (SUCCEEDED(hr) && a_out && *a_out && a_desc)
					Track(*a_out, ret, a_desc->Usage == D3D11_USAGE_STAGING ? kStaging : kTex1D,
						static_cast<int64_t>(a_desc->Width) * DirectX::BitsPerPixel(a_desc->Format) / 8 * std::max(1u, a_desc->ArraySize) * 2);
				return hr;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct CreateTexture2DHook
		{
			static HRESULT STDMETHODCALLTYPE thunk(ID3D11Device* a_this, const D3D11_TEXTURE2D_DESC* a_desc, const D3D11_SUBRESOURCE_DATA* a_init, ID3D11Texture2D** a_out)
			{
				void* ret = _ReturnAddress();
				const HRESULT hr = func(a_this, a_desc, a_init, a_out);
				if (SUCCEEDED(hr) && a_out && *a_out && a_desc) {
					int cat;
					const UINT maxDim = std::max(a_desc->Width, a_desc->Height);
					if (a_desc->Usage == D3D11_USAGE_STAGING)
						cat = kStaging;
					else if (a_desc->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL))
						cat = kRT;
					else if (a_desc->BindFlags & D3D11_BIND_UNORDERED_ACCESS)
						cat = kUAV;
					else if (a_desc->MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)
						cat = kCube;
					else if (a_desc->Usage == D3D11_USAGE_DYNAMIC)
						cat = kTexDyn;
					else if (maxDim <= 512)
						cat = kTex512;
					else if (maxDim <= 1024)
						cat = kTex1K;
					else if (maxDim <= 2048)
						cat = kTex2K;
					else
						cat = kTex4KPlus;
					Track(*a_out, ret, cat, Texture2DBytes(*a_desc));
				}
				return hr;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct CreateTexture3DHook
		{
			static HRESULT STDMETHODCALLTYPE thunk(ID3D11Device* a_this, const D3D11_TEXTURE3D_DESC* a_desc, const D3D11_SUBRESOURCE_DATA* a_init, ID3D11Texture3D** a_out)
			{
				void* ret = _ReturnAddress();
				const HRESULT hr = func(a_this, a_desc, a_init, a_out);
				if (SUCCEEDED(hr) && a_out && *a_out && a_desc)
					Track(*a_out, ret, a_desc->Usage == D3D11_USAGE_STAGING ? kStaging : kTex3D,
						static_cast<int64_t>(a_desc->Width) * a_desc->Height * a_desc->Depth * DirectX::BitsPerPixel(a_desc->Format) / 8 * 4 / 3);
				return hr;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install(ID3D11Device* a_device)
	{
		try {
			const auto dir = Util::PathHelpers::GetCommunityShaderPath();
			if (!std::filesystem::exists(dir / "VramTrack.on"))
				return;
			g_outPath = dir / "VramTrack.jsonl";

			Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
			Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
			if (SUCCEEDED(a_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)))
				adapter.As(&g_adapter3);

			g_exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
			if (g_exeBase) {
				const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_exeBase);
				const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_exeBase + dos->e_lfanew);
				g_exeEnd = g_exeBase + nt->OptionalHeader.SizeOfImage;
			}

			stl::detour_vfunc<3, CreateBufferHook>(a_device);
			stl::detour_vfunc<4, CreateTexture1DHook>(a_device);
			stl::detour_vfunc<5, CreateTexture2DHook>(a_device);
			stl::detour_vfunc<6, CreateTexture3DHook>(a_device);
			g_active = true;
			logger::info("[VramTracker] active, writing {}", g_outPath.string());
		} catch (const std::exception& e) {
			logger::warn("[VramTracker] install failed: {}", e.what());
		}
	}

	void OnPresent()
	{
		if (!g_active)
			return;
		using clock = std::chrono::steady_clock;
		static auto last = clock::now();
		static uint64_t frames = 0;
		++frames;
		const auto now = clock::now();
		if (now - last < std::chrono::seconds(1))
			return;
		const double dt = std::chrono::duration<double>(now - last).count();
		last = now;

		constexpr double kMB = 1.0 / (1024.0 * 1024.0);
		nlohmann::json j;
		j["ts"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		j["dt"] = dt;
		j["frames"] = frames;
		if (g_adapter3) {
			DXGI_QUERY_VIDEO_MEMORY_INFO info{};
			if (SUCCEEDED(g_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
				j["procMB"] = info.CurrentUsage * kMB;
				j["budgetMB"] = info.Budget * kMB;
			}
		}
		double totalLive = 0, totalNew = 0, totalFreed = 0;
		nlohmann::json live = nlohmann::json::object(), created = nlohmann::json::object(), freed = nlohmann::json::object(), counts = nlohmann::json::object();
		{
			std::lock_guard lock(g_modMutex);
			for (int m = 0; m < kMaxModules; ++m) {
				const std::string mod = m < static_cast<int>(g_mods.size()) ? g_mods[m].second : (m == kMaxModules - 1 ? "other" : "");
				if (mod.empty())
					continue;
				for (int c = 0; c < kCats; ++c) {
					auto& s = g_slots[m][c];
					const int64_t lb = s.liveBytes.load(), nb = s.newBytes.exchange(0), fb = s.freedBytes.exchange(0), nc = s.newCount.exchange(0);
					const std::string key = mod + "|" + kCatNames[c];
					if (lb > 0) {
						live[key] = std::round(lb * kMB * 10) / 10;
						counts[key] = s.liveCount.load();
						if (c != kStaging)
							totalLive += lb * kMB;
					}
					if (nb > 0) {
						created[key] = std::round(nb * kMB * 10) / 10;
						if (c != kStaging)
							totalNew += nb * kMB;
					}
					if (fb > 0) {
						freed[key] = std::round(fb * kMB * 10) / 10;
						if (c != kStaging)
							totalFreed += fb * kMB;
					}
					(void)nc;
				}
			}
		}
		j["totalLiveMB"] = std::round(totalLive * 10) / 10;
		j["newMB"] = std::round(totalNew * 10) / 10;
		j["freedMB"] = std::round(totalFreed * 10) / 10;
		j["live"] = std::move(live);
		j["liveCount"] = std::move(counts);
		j["created"] = std::move(created);
		j["freed"] = std::move(freed);
		{
			// Top engine call sites of this second's large allocations
			std::vector<Site> top;
			{
				std::lock_guard lock(g_siteMutex);
				for (auto& [key, site] : g_sites) {
					if (site.windowBytes > 0) {
						top.push_back(site);
						site.windowBytes = 0;
						site.windowCount = 0;
					}
				}
			}
			std::sort(top.begin(), top.end(), [](const Site& a, const Site& b) { return a.windowBytes > b.windowBytes; });
			nlohmann::json sites = nlohmann::json::array();
			for (size_t i = 0; i < top.size() && i < 10; ++i) {
				const auto& site = top[i];
				if (site.windowBytes < 1024 * 1024)
					break;
				sites.push_back({ { "rva", std::format("{:X}>{:X}>{:X}", site.rva[0], site.rva[1], site.rva[2]) },
					{ "cat", kCatNames[site.cat] },
					{ "mb", std::round(site.windowBytes * kMB * 10) / 10 },
					{ "n", site.windowCount } });
			}
			j["sites"] = std::move(sites);
		}
		std::ofstream out(g_outPath, std::ios::app);
		out << j.dump() << '\n';
	}
}
