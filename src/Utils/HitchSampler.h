#pragma once

/**
 * @brief Optional in-process sampler that says where the render thread was during a long frame (diagnostics).
 *
 * Active only when the marker file `HitchSample.on` exists in the CommunityShaders plugin directory
 * (Data/SKSE/Plugins/CommunityShaders). A watchdog thread wakes about once a millisecond; while the current
 * frame is older than the start threshold it suspends the render thread just long enough to copy its
 * instruction pointer and the top of its stack into a preallocated buffer, then resumes it. Nothing that can
 * take a lock (no unwinding, no allocation, no logging) happens while the thread is suspended, so the sampler
 * cannot deadlock the game. After the frame ends, frames over the report threshold are written to the log as
 * module-relative addresses of this DLL (resolve with llvm-symbolizer and the PDB): the innermost address and
 * the call chain found by scanning the stack for return addresses into this module.
 */
namespace HitchSampler
{
	/** @brief Starts the watchdog if the marker file exists. Call once after the device is created. */
	void Install();

	/** @brief Per-frame tick from the Present hook, on the render thread. Cheap no-op when inactive. */
	void OnPresent();
}
