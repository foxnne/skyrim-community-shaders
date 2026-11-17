#pragma once

#include <d3d11.h>

/**
 * @brief Optional D3D11 resource-creation tracker (diagnostics).
 *
 * Active only when the marker file `VramTrack.on` exists in the CommunityShaders plugin directory
 * (Data/SKSE/Plugins/CommunityShaders). It hooks CreateBuffer/CreateTexture1D/2D/3D, tags every
 * resource with its size/category/creating module, and once a second appends one JSON line to
 * `VramTrack.jsonl` in the same directory: live MB, created MB and freed MB per "module|category",
 * plus the DXGI-reported local video memory usage of the process. With no marker file nothing is hooked.
 */
namespace VramTracker
{
	/** @brief Installs the hooks on the device if the marker file exists. Call once after device creation. */
	void Install(ID3D11Device* a_device);

	/** @brief Per-frame tick; writes a report line about once per second. Cheap no-op when inactive. */
	void OnPresent();
}
