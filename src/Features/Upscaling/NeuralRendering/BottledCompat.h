#pragma once

/**
 * @file BottledCompat.h
 * @brief Helpers Neural Rendering expects from Open Shaders (alandtse/open-shaders) that this
 *        Bottled Shaders based tree does not have. Ported from Open Shaders' Utils (GPL-3.0),
 *        trimmed to what the NR sources use. This tree is flat-screen only, so VR paths are off.
 */

#include "Globals.h"
#include "I18n/I18n.h"
#include "Utils/BoundPoints.h"
#include "Utils/FileSystem.h"
#include "Utils/UI.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <span>
#include <system_error>
#include <vector>

namespace globals::game
{
	/** @brief Bottled Shaders drops Skyrim VR support; NR's stereo paths compile but never run. */
	inline constexpr bool isVR = false;
}

namespace Util
{
	namespace PathHelpers
	{
		/** @brief std::filesystem::absolute that logs and returns the input on failure instead of throwing. */
		inline std::filesystem::path SafeAbsolute(const std::filesystem::path& path)
		{
			std::error_code ec;
			auto result = std::filesystem::absolute(path, ec);
			if (ec) {
				logger::warn("Failed to resolve absolute path for '{}': {}", path.string(), ec.message());
				return path;
			}
			return result;
		}

		/** @brief std::filesystem::exists that logs and returns false on failure instead of throwing. */
		inline bool SafeExists(const std::filesystem::path& path)
		{
			std::error_code ec;
			const bool result = std::filesystem::exists(path, ec);
			if (ec) {
				logger::warn("Failed to check existence of '{}': {}", path.string(), ec.message());
				return false;
			}
			return result;
		}

		/** @brief Absolute paths of the immediate subdirectories of @p root; empty on any error. */
		inline std::vector<std::filesystem::path> SafeChildDirectories(const std::filesystem::path& root)
		{
			std::vector<std::filesystem::path> children;
			std::error_code ec;
			for (auto it = std::filesystem::directory_iterator(root, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
				if (it->is_directory(ec))
					children.push_back(SafeAbsolute(it->path()));
			}
			return children;
		}
	}

	/** @brief Fills @p OutDesc with the description of the 2D texture behind @p View. */
	inline bool GetTexture2DDesc(ID3D11View* View, D3D11_TEXTURE2D_DESC& OutDesc)
	{
		if (!View)
			return false;
		winrt::com_ptr<ID3D11Resource> resource;
		View->GetResource(resource.put());
		if (!resource)
			return false;
		winrt::com_ptr<ID3D11Texture2D> texture;
		if (FAILED(resource->QueryInterface(IID_PPV_ARGS(texture.put()))))
			return false;
		texture->GetDesc(&OutDesc);
		return true;
	}

	/** @brief Call site inside the main update loop that features thunk to run once per game frame. */
	[[nodiscard]] inline std::uintptr_t MainUpdateCallSite()
	{
		const std::uintptr_t aeOffset = REL::Module::IsAtLeast(REL::Version(1, 7, 99, 0)) ? 0xC38 : 0xC26;
		return REL::RelocationID(35565, 36564).address() + REL::Relocate<std::uintptr_t>(0x748, aeOffset, 0x7EE);
	}

	namespace Colors
	{
		/** @brief Critical/error status colour for timers. */
		inline ImVec4 GetTimerCritical() { return GetError(); }
	}

	/** @brief Selectable that opens a Data-relative folder in Explorer. */
	inline void DrawOpenFolderSelectable(const char* label, const wchar_t* pluginDir)
	{
		if (ImGui::Selectable(label)) {
			auto realPath = Util::PathHelpers::GetRealPathFromDataRelative(pluginDir);
			ShellExecuteW(nullptr, L"open", realPath.empty() ? pluginDir : realPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		}
	}

	/** @brief "Select all" / "Select none" buttons over a span of 0/1 flags. */
	inline void DrawSelectionButtons(std::span<uint8_t> selected, const char* selectAll, const char* selectNone)
	{
		if (ImGui::SmallButton(selectAll))
			std::ranges::fill(selected, uint8_t{ 1 });
		ImGui::SameLine();
		if (ImGui::SmallButton(selectNone))
			std::ranges::fill(selected, uint8_t{ 0 });
	}

	/**
	 * @brief Dropdown editing an integral or enum setting in place, one item per value from 0.
	 *        A stored value past the last item shows as the last item and is left unchanged until the user picks.
	 */
	template <class Stored>
	bool Combo(const char* label, Stored* storedValue, const char* const* items, int itemCount)
	{
		if (!storedValue || itemCount <= 0)
			return false;
		const int stored = static_cast<int>(*storedValue);
		const int shown = std::clamp(stored, 0, itemCount - 1);
		int index = shown;
		if (!ImGui::Combo(label, &index, items, itemCount) || index == shown)
			return false;
		*storedValue = static_cast<Stored>(index);
		return true;
	}

	template <class Stored, size_t ItemCount>
	bool Combo(const char* label, Stored* storedValue, const std::array<const char*, ItemCount>& items)
	{
		return Combo(label, storedValue, items.data(), static_cast<int>(ItemCount));
	}

	template <class Stored, size_t ItemCount>
	bool Combo(const char* label, Stored* storedValue, const char* const (&items)[ItemCount])
	{
		return Combo(label, storedValue, items, static_cast<int>(ItemCount));
	}

	/** @brief Visits the player and every high-process actor. */
	inline void ForEachLoadedActor(const std::function<void(RE::Actor*)>& a_callback)
	{
		if (!a_callback)
			return;
		auto* player = globals::game::player;
		if (player)
			a_callback(player);
		if (const auto* processLists = RE::ProcessLists::GetSingleton()) {
			for (const auto& actorHandle : processLists->highActorHandles) {
				if (const auto actor = actorHandle.get(); actor && actor.get() != player)
					a_callback(actor.get());
			}
		}
	}

	/**
	 * @brief World-space points bounding an actor: its root bounding sphere as a cube.
	 *        Simplified from Open Shaders, which also walks the skeleton; that only matters for the
	 *        optional actor crop region (off by default), where it keeps swung limbs inside the box.
	 */
	[[nodiscard]] inline BoundPoints GetActorBoundPoints(RE::Actor& a_actor, [[maybe_unused]] bool a_includeSkeleton, float a_jointMargin = 0.0f)
	{
		BoundPoints result;
		auto* root = a_actor.Get3D(false);
		if (!root || root->worldBound.radius <= 0.0f)
			return result;
		const auto& bound = root->worldBound;
		const float r = bound.radius + a_jointMargin;
		const float3 center{ bound.center.x, bound.center.y, bound.center.z };
		const float3 extent{ r, r, r };
		result.AddBox(center - extent, center + extent);
		return result;
	}

	/**
	 * @brief Whether the actor is visible from @p a_eyePosition. Simplified to always true:
	 *        Open Shaders ray-casts against Havok here, which only refines the optional actor crop.
	 */
	[[nodiscard]] inline bool IsActorVisibleFromEye([[maybe_unused]] RE::Actor& a_actor, [[maybe_unused]] const RE::NiPoint3& a_eyePosition)
	{
		return true;
	}
}
