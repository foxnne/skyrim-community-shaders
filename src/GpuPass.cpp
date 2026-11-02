#include "GpuPass.h"

#include "Globals.h"
#include "State.h"

#include <mutex>
#include <set>
#include <string>

namespace
{
	struct GpuPassCapabilityRegistry
	{
		std::mutex mutex;
		std::set<std::string, std::less<>> featurePrefixes;
	};

	GpuPassCapabilityRegistry& GetGpuPassCapabilityRegistry()
	{
		static GpuPassCapabilityRegistry registry;
		return registry;
	}
}

bool GpuPassCapabilities::Register(std::string_view passName)
{
	const auto separator = passName.find("::");
	if (separator == std::string_view::npos || separator == 0)
		return false;

	// The registry is append-only, so successful registrations remain valid on every thread.
	thread_local std::set<std::string, std::less<>> registeredPrefixes;
	const auto prefix = passName.substr(0, separator);
	if (registeredPrefixes.contains(prefix))
		return true;

	auto& registry = GetGpuPassCapabilityRegistry();
	std::scoped_lock lock(registry.mutex);
	if (!registry.featurePrefixes.contains(prefix))
		registry.featurePrefixes.emplace(prefix);
	registeredPrefixes.emplace(prefix);
	return true;
}

bool GpuPassCapabilities::Contains(std::string_view featurePrefix)
{
	auto& registry = GetGpuPassCapabilityRegistry();
	std::scoped_lock lock(registry.mutex);
	return registry.featurePrefixes.contains(featurePrefix);
}

#ifdef TRACY_ENABLE
ScopedGpuPass::ScopedGpuPass(const tracy::SourceLocationData* srcloc, std::string_view name, GpuPassSpan span)
{
	auto* profiler = globals::profiler;
	auto* state = globals::state;

	if (profiler) {
		profiler->BeginPass(name);
		profilerActive = true;
	}

	// See GpuPassSpan's comment in GpuPass.h.
	if (span == GpuPassSpan::Scoped)
		cpuZone.emplace(srcloc, -1, true);

	if (state && state->tracyCtx) {
		gpuZone.emplace(state->tracyCtx, srcloc, true);
	}

	// This tree has no annotation-only call; BeginPerfEvent also opens a Tracy CPU zone.
	if (state && state->frameAnnotations) {
		state->BeginPerfEvent(name);
		annotationOpen = true;
	}
}
#endif

ScopedGpuPass::ScopedGpuPass(std::string_view name, [[maybe_unused]] GpuPassSpan span)
{
	auto* profiler = globals::profiler;
	auto* state = globals::state;

	if (profiler) {
		profiler->BeginPass(name);
		profilerActive = true;
	}

#ifdef TRACY_ENABLE
	// Each call allocates its own one-shot srcloc buffer; do not cache and reuse it
	// across calls, Tracy frees it after this zone is serialized.
	// See GpuPassSpan's comment in GpuPass.h.
	if (span == GpuPassSpan::Scoped) {
		cpuZone.emplace(
			uint32_t(0),
			"GpuPass", sizeof("GpuPass") - 1,
			"ScopedGpuPass", sizeof("ScopedGpuPass") - 1,
			name.data(), name.size(),
			uint32_t(0), -1, true);
	}

	if (state && state->tracyCtx) {
		gpuZone.emplace(state->tracyCtx,
			uint32_t(0),
			"GpuPass", sizeof("GpuPass") - 1,
			"ScopedGpuPass", sizeof("ScopedGpuPass") - 1,
			name.data(), name.size(),
			0, true);
	}
#endif

	// This tree has no annotation-only call; BeginPerfEvent also opens a Tracy CPU zone.
	if (state && state->frameAnnotations) {
		state->BeginPerfEvent(name);
		annotationOpen = true;
	}
}

ScopedGpuPass::~ScopedGpuPass()
{
	auto* state = globals::state;

	if (annotationOpen && state)
		state->EndPerfEvent();

#ifdef TRACY_ENABLE
	gpuZone.reset();
	cpuZone.reset();
#endif

	if (profilerActive)
		globals::profiler->EndPass();
}
