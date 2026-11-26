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
 *
 * Resources our own code names (Util::SetResourceName, every Buffer/Texture wrapper) are also attributed by
 * that debug name: `named` (live MB), `namedCount` and `namedNew` (MB created this second) per name on every
 * report line, and `VramTrackNames.txt` lists the live targets and buffers by name and size every 30 s. The
 * module|category totals are unchanged by this; the names are a second breakdown of the same bytes.
 */
namespace VramTracker
{
	/** @brief Installs the hooks on the device if the marker file exists. Call once after device creation. */
	void Install(ID3D11Device* a_device);

	/** @brief Per-frame tick; writes a report line about once per second. Cheap no-op when inactive. */
	void OnPresent();

	/**
	 * @brief Records the debug name of a tracked resource (called by Util::SetResourceName): the resource joins the
	 * per-name totals of the report line, and `VramTrackNames.txt`, rewritten every 30 s while tracking, lists the
	 * live render targets, UAV textures, volumes and large buffers by name and size. Views, shaders and states carry
	 * no tag and are ignored. Cheap no-op when inactive.
	 */
	void OnNamed(ID3D11DeviceChild* a_resource, const char* a_name);
}
