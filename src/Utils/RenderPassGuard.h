#pragma once

#include "SehGuard.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <cstring>

/**
 * Guards against two use-after-free patterns seen in crash logs (2026-11-20 .. 2026-11-24, AE 1.6.1170):
 *
 *  A. BSBatchRenderer::RenderPassImmediately (ID 107644+0x8A, `call [rax+0x10]`) runs on a BSRenderPass that
 *     was already freed. With Engine Fixes' bOverrideRenderPassCache the pass is a malloc block, so the
 *     allocator's free-list link overwrites `pass->shader` (offset 0): the "shader" is another free block
 *     or null, and the engine calls through whatever that block holds.
 *  B. BSEffectShader::SetupGeometry (ID 107527+0x4EA, `mulss xmm2,[rbx+0x134]`) reads
 *     `pass->sceneLights[i]->light->fade` with no null test while the BSLight has already dropped its NiLight.
 *
 * Neither is caused here; both guards only turn the crash into a skipped draw / a shorter light list and
 * count it. The hot path of A is one compare against the last accepted shader.
 */
namespace Util::RenderPassGuard
{
	inline std::atomic<std::uint32_t> droppedPasses{ 0 };
	inline std::atomic<std::uint32_t> trimmedLightLists{ 0 };

	namespace detail
	{
		// The engine has about a dozen BSShader singletons; they are learned on first sight.
		inline std::array<std::atomic<const RE::BSShader*>, 24> knownShaders{};
		inline std::atomic<const RE::BSShader*> lastShader{ nullptr };

		struct Probe
		{
			const RE::BSRenderPass* pass = nullptr;
			std::uintptr_t shader = 0;
			bool isShader = false;
			char geometryName[64]{};
		};

		// Image that holds a_address, or 0. A heap pointer (a free-list link) is in no image.
		inline std::uintptr_t ImageOf(std::uintptr_t a_address) noexcept
		{
			HMODULE module = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(a_address), &module))
				return 0;
			return reinterpret_cast<std::uintptr_t>(module);
		}

		// Runs under SEH (see ProbePass): every read here may fault. No objects with destructors.
		// True when a_object starts with a vtable whose MSVC RTTI class name ends in a_suffix.
		inline bool HasRttiSuffix(std::uintptr_t a_object, const char* a_suffix) noexcept
		{
			if (a_object == 0 || (a_object & 7) != 0)
				return false;
			const auto vtable = *reinterpret_cast<const std::uintptr_t*>(a_object);
			if (vtable == 0 || (vtable & 7) != 0)
				return false;
			const auto image = ImageOf(vtable);
			if (image == 0)
				return false;
			// x64 RTTICompleteObjectLocator: signature 1, type descriptor RVA at +0xC; the name is at +0x10 of the descriptor.
			const auto locator = *reinterpret_cast<const std::uintptr_t*>(vtable - sizeof(std::uintptr_t));
			if (ImageOf(locator) != image)
				return false;
			const auto* fields = reinterpret_cast<const std::uint32_t*>(locator);
			if (fields[0] != 1)
				return false;
			const auto descriptor = image + fields[3];
			if (ImageOf(descriptor) != image)
				return false;
			const char* name = reinterpret_cast<const char*>(descriptor + 0x10);
			const auto length = strnlen(name, 128);
			const auto suffixLength = std::strlen(a_suffix);
			return length < 128 && length >= suffixLength && std::memcmp(name + length - suffixLength, a_suffix, suffixLength) == 0;
		}

		inline void ProbePass(Probe& a_probe) noexcept
		{
			Util::SehGuarded([&a_probe]() {
				a_probe.shader = reinterpret_cast<std::uintptr_t>(a_probe.pass->shader);
				// ".?AVBSLightingShader@@", ".?AVBSWaterShader@@", ...: every class drawn through a render pass.
				a_probe.isShader = HasRttiSuffix(a_probe.shader, "Shader@@");
				if (a_probe.isShader)
					return;
				// Name the geometry for the log; in every logged crash it was still alive.
				const auto* geometry = a_probe.pass->geometry;
				if (HasRttiSuffix(reinterpret_cast<std::uintptr_t>(geometry), "@@")) {
					if (const char* name = geometry->name.c_str())
						strncpy_s(a_probe.geometryName, name, _TRUNCATE);
				}
			});
		}

		__declspec(noinline) inline bool CheckUnknownShader(const RE::BSRenderPass* a_pass, std::uint32_t a_technique) noexcept
		{
			Probe probe{ a_pass };
			ProbePass(probe);
			if (probe.isShader) {
				const auto* shader = reinterpret_cast<const RE::BSShader*>(probe.shader);
				for (auto& known : knownShaders) {
					const RE::BSShader* expected = nullptr;
					if (known.compare_exchange_strong(expected, shader, std::memory_order_relaxed) || expected == shader)
						break;
				}
				lastShader.store(shader, std::memory_order_relaxed);
				return true;
			}

			const auto count = droppedPasses.fetch_add(1, std::memory_order_relaxed) + 1;
			if (count <= 8 || (count & 0x3FF) == 0)
				logger::warn("[RenderPassGuard] skipped freed render pass {:p} (shader field {:X}, technique {:X}, geometry '{}'); {} skipped so far",
					static_cast<const void*>(a_pass), probe.shader, a_technique, probe.geometryName, count);
			return false;
		}
	}

	/**
	 * @brief False when a_pass must not reach BSBatchRenderer::RenderPassImmediately (its shader field is not a BSShader).
	 *        Reads only a_pass->shader on the hot path; the pass itself was readable in every logged crash.
	 */
	[[nodiscard]] inline bool IsDrawable(const RE::BSRenderPass* a_pass, std::uint32_t a_technique) noexcept
	{
		if (!a_pass) [[unlikely]]
			return true;  // not ours to judge; the engine never passes null here
		const auto* shader = a_pass->shader;
		if (shader == detail::lastShader.load(std::memory_order_relaxed)) [[likely]]
			return true;
		for (const auto& known : detail::knownShaders) {
			const auto* candidate = known.load(std::memory_order_relaxed);
			if (candidate == nullptr)
				break;
			if (candidate == shader) {
				detail::lastShader.store(shader, std::memory_order_relaxed);
				return true;
			}
		}
		return detail::CheckUnknownShader(a_pass, a_technique);
	}

	/**
	 * @brief Cuts a_pass->numLights at the first scene light that is null or has lost its NiLight.
	 *        BSEffectShader::SetupGeometry reads up to five of them unchecked. Unlit passes cost one byte test.
	 */
	inline void TrimDeadSceneLights(RE::BSRenderPass* a_pass) noexcept
	{
		if (!a_pass || a_pass->numLights == 0) [[likely]]
			return;
		constexpr std::uint8_t MaxRead = 5;  // the engine clamps its loop to five lights (107527+0x46D)
		const std::uint8_t limit = a_pass->numLights < MaxRead ? a_pass->numLights : MaxRead;
		auto** lights = a_pass->sceneLights;
		std::uint8_t alive = 0;
		if (lights) {
			while (alive < limit && lights[alive] && lights[alive]->light)
				++alive;
		}
		if (alive == limit) [[likely]]
			return;

		const auto wanted = a_pass->numLights;
		a_pass->numLights = alive;
		const auto count = trimmedLightLists.fetch_add(1, std::memory_order_relaxed) + 1;
		if (count <= 8 || (count & 0x3FF) == 0)
			logger::warn("[RenderPassGuard] effect pass {:p} had a dead scene light at index {} of {}; list cut ({} so far)",
				static_cast<const void*>(a_pass), alive, wanted, count);
	}
}
