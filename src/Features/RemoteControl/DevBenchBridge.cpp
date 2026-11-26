#include "Features/NeuralRendering.h"
#include "Features/RemoteControl/DevBenchBridge.h"
#include "Features/RemoteControl/ReferenceClock.h"
#include "Features/LightLimitFix.h"
#include "Features/SkySync.h"
#include "Features/InteriorSun.h"
#include "Features/PostProcessing.h"
#include "Deferred.h"
#include "Features/PostProcessing/HistogramAutoExposure.h"

// Registers our tools into the devbench test bench over its C-ABI. Gated by
// DEVBENCH_BRIDGE_ENABLED (set by CMake when the devbench-api port is available);
// otherwise this file compiles to an empty Install(). Inert at runtime when no
// devbench plugin is present (GetDevBenchInterface001() returns null).
//
// The communityshaders.* tools below expose Community Shaders' graphics-feature, inspect,
// capture, nvperf, shadercache, settings and dev-flag operations through the single devbench
// host over both MCP and REST. Each is namespaced to avoid collisions in devbench's
// shared registry; the action / kind / inputSchema shapes are stable so clients can
// rely on them.

#ifdef DEVBENCH_BRIDGE_ENABLED

#	include "Feature.h"
#	include "Features/DeveloperTools.h"
#	include "Features/RemoteControl/DevFlags.h"
#	include "Features/RemoteControl/FrameRecorder.h"
#	include "Features/RemoteControl/NvPerf.h"
#	include "Features/RenderDoc.h"
#	include "Features/ScreenshotFeature.h"
#	include "Features/Upscaling.h"
#	include "Globals.h"
#	include "Profiler.h"
#	include "CSEditor/EditorWindow.h"
#	include "CSEditor/SceneManager/SceneSettingsManager.h"
#	include "ShaderCache.h"
#	include "State.h"
#	include "Utils/Climate.h"
#	include "Utils/FileSystem.h"
#	include "Utils/Game.h"

#	include <DevBenchAPI.h>
#	include <nlohmann/json.hpp>

#	include <algorithm>
#	include <chrono>
#	include <cmath>
#	include <cstring>
#	include <functional>
#	include <future>
#	include <memory>
#	include <mutex>
#	include <optional>
#	include <stdexcept>
#	include <thread>
#	include <unordered_set>

namespace
{
	using json = nlohmann::json;

	// Main thread only: the only features toggle may switch back on. One off for any other
	// reason was never loaded, or turned itself off during boot, and must stay off.
	std::unordered_set<Feature*> g_toggledOffFeatures;

	// Current render frame, used as a coarse "enqueued at" stamp so callers can poll
	// inspect(kind=state) until frame_count advances past it (i.e. a queued main-thread
	/**
	 * @brief Retrieves the current render frame count.
	 *
	 * Safe to call from any thread.
	 *
	 * @return uint The current frame count, or 0 if global state is unavailable.
	 */
	uint EnqueuedFrame()
	{
		return globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
	}

	// Shared C-ABI handler body. The whole request — parse, dispatch, dump — is wrapped
	// so NO exception ever crosses the DLL boundary, and a_write is called exactly once.
	// `a_build` is a plain function pointer (no captures) so this composes with the
	/**
	 * @brief Wraps a DevBench tool handler to safely marshal exceptions into JSON error responses.
	 *
	 * Parses JSON arguments from the input string, invokes the builder function, and writes
	 * the resulting JSON response to the sink via the provided callback. All exceptions are
	 * caught and converted to standardized JSON error objects to prevent C++ exceptions from
	 * crossing the DLL boundary.
	 *
	 * @param a_tool Registered tool name, recorded as the last request for the Developer Tools panel.
	 * @param a_build Function that builds a JSON response from parsed request arguments.
	 * @param a_argsJson JSON-formatted request arguments as a string; null or empty string
	 *                   defaults to an empty JSON object.
	 * @param a_sink Opaque destination handle passed to the write callback.
	 * @param a_write Callback function invoked exactly once to write the serialized JSON response.
	 */
	std::mutex g_lastRequestMutex;
	std::optional<DevBenchBridge::Request> g_lastRequest;

	// Exception text can hold ANSI-code-page bytes; a strict dump would throw past the C ABI.
	std::string DumpForDevBench(const json& a_json)
	{
		return a_json.dump(-1, ' ', false, json::error_handler_t::replace);
	}

	void RunHandler(const char* a_tool, json (*a_build)(const json&), const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		{
			std::scoped_lock lock(g_lastRequestMutex);
			g_lastRequest = DevBenchBridge::Request{ a_tool, std::filesystem::file_time_type::clock::now() };
		}
		json out;
		try {
			json args = json::object();
			if (a_argsJson && *a_argsJson)
				args = json::parse(a_argsJson);  // throws on malformed input
			if (!args.is_object())
				throw std::runtime_error("arguments must be a JSON object");
			out = a_build(args);
		} catch (const std::exception& e) {
			out = json{ { "error", "invalid request" }, { "detail", e.what() } };
		} catch (...) {
			out = json{ { "error", "unknown handler error" } };
		}
		const std::string dumped = DumpForDevBench(out);
		a_write(a_sink, dumped.c_str());
	}

	// Run a task on the main thread and return its result. Bridge handlers run on devbench's
	// listener thread; work that touches state the render/UI thread mutates (A/B fields, JSON
	// snapshots, feature settings, capture triggers) must marshal or it races. Blocks the
	// handler briefly, bounded so a stalled main thread (e.g. mid-load) can't hang it.
	//
	// The body may have SIDE EFFECTS (set/reset apply settings; capture triggers a frame), so a
	// `cancelled` flag is checked at task entry: if we already gave up waiting, the task skips
	// the body rather than mutating state after we reported a timeout. shared_ptr state so a
	// task that runs after we return doesn't dangle. (Best-effort: a task that starts exactly at
	/**
	 * @brief Executes a callable on the main thread and waits for its result.
	 *
	 * Schedules the callable to run on the main thread via SKSE's task interface with a 5-second
	 * timeout. If the timeout elapses, sets an internal flag to prevent delayed side effects.
	 *
	 * @param a_run Callable that returns a JSON result.
	 * @return The JSON result from `a_run()`, or an error JSON object if the task interface is
	 * unavailable, execution times out, or an exception is thrown.
	 */
	json RunOnMainThread(std::function<json()> a_run)
	{
		auto* task = SKSE::GetTaskInterface();
		if (!task)
			return json{ { "error", "SKSE task interface unavailable" } };
		auto prom = std::make_shared<std::promise<json>>();
		auto cancelled = std::make_shared<std::atomic<bool>>(false);
		auto fut = prom->get_future();
		task->AddTask([prom, cancelled, run = std::move(a_run)]() {
			if (cancelled->load(std::memory_order_acquire))
				return;  // handler already timed out and returned — don't run a side-effecting body late
			try {
				prom->set_value(run());
			} catch (const std::exception& e) {
				prom->set_value(json{ { "error", "task threw on main thread" }, { "detail", e.what() } });
			} catch (...) {
				prom->set_value(json{ { "error", "task threw on main thread" }, { "detail", "non-std exception" } });
			}
		});
		if (fut.wait_for(std::chrono::milliseconds(5000)) != std::future_status::ready) {
			cancelled->store(true, std::memory_order_release);
			return json{ { "error", "main thread did not run within 5000ms (mid-load?)" } };
		}
		return fut.get();
	}

	// ---- feature: list / get / set / reset / toggle -----------------------------------

	// Build one feature entry, including restart-gated metadata so `list` answers
	/**
	 * @brief Constructs a JSON object describing a feature's state and any pending restart-required changes.
	 *
	 * Includes feature metadata (name, version, category, load state, etc.) and, if the feature
	 * has restart-required fields, an array of those fields with their pending status determined
	 * by comparing boot values against current live settings.
	 *
	 * @return json Feature entry with metadata and optional restart fields array.
	 */
	json FeatureEntry(Feature* f)
	{
		json entry{
			{ "name", f->GetName() },
			{ "shortName", f->GetShortName() },
			{ "loaded", f->loaded },
			{ "version", f->version },
			{ "category", std::string(f->GetCategory()) },
			{ "isCore", f->IsCore() },
			{ "inMenu", f->IsInMenu() },
		};

		const auto fields = f->GetRestartRequiredFields();
		if (!fields.empty()) {
			json restartFields = json::array();
			const auto* liveBase = reinterpret_cast<const unsigned char*>(f->GetSettingsBlob());
			const size_t liveSize = f->GetSettingsBlobSize();
			for (const auto& field : fields) {
				bool pending = false;
				// Compare against remaining bytes (not offset+size, which can overflow and turn
				// a bad metadata entry into an out-of-bounds memcmp).
				if (liveBase && field.jsonKey && field.size != 0 &&
					field.offset <= liveSize && field.size <= liveSize - field.offset) {
					const void* boot = f->GetBootValue(field.jsonKey);
					if (boot && std::memcmp(boot, liveBase + field.offset, field.size) != 0)
						pending = true;
				}
				restartFields.push_back(json{
					{ "key", field.jsonKey ? field.jsonKey : "" },
					{ "label", field.label ? field.label : "" },
					{ "pending", pending },
				});
			}
			entry["restartFields"] = restartFields;
		}
		return entry;
	}

	/**
	 * @brief Dispatches feature management operations (list, get, set, reset, toggle).
	 *
	 * Validates parameters and executes the requested feature operation, returning
	 * success or error details in JSON format.
	 *
	 * @param a_args JSON object specifying the action and operation parameters.
	 * @return JSON object containing the operation result or error information.
	 */
	json BuildFeatureResult(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string("list"));

		if (action == "list") {
			// Marshal: FeatureEntry reads Feature::loaded and restart-gated settings bytes that
			// main-thread toggles / settings-loads mutate.
			return RunOnMainThread([]() {
				// One guard for the whole listing: FeatureEntry diffs live settings bytes against
				// boot values, which a scene override would otherwise report as a pending restart.
				SceneSettingsManager::SceneLayerGuard sceneLayerGuard;
				json out = json::array();
				for (auto* f : Feature::GetFeatureList())
					out.push_back(FeatureEntry(f));
				return out;
			});
		}

		const std::string shortName = a_args.value("shortName", std::string{});

		if (action == "toggle") {
			// Match over the full feature list (NOT FindFeatureByShortName, which only
			// matches *loaded* features — that makes toggle one-way: you could disable a
			// feature but never re-enable it).
			Feature* target = nullptr;
			if (!shortName.empty()) {
				for (auto* f : Feature::GetFeatureList()) {
					if (f->GetShortName() == shortName) {
						target = f;
						break;
					}
				}
			}
			if (!target)
				return json{ { "error", "unknown or missing shortName" }, { "shortName", shortName } };
			if (target->IsAlwaysEnabled())
				return json{ { "error", "feature is always enabled" }, { "shortName", shortName } };
			auto* task = SKSE::GetTaskInterface();
			if (!task)
				return json{ { "error", "SKSE task interface unavailable" }, { "shortName", shortName } };
			const uint frame = EnqueuedFrame();
			// If `enabled` is omitted we flip the CURRENT value — but that read must happen on
			// the main thread INSIDE the task: computing !target->loaded here on the listener
			// thread lets concurrent toggles all observe the same stale value and enqueue
			// identical results. With an explicit `enabled`, apply it verbatim.
			// Threading contract: Feature::loaded is a public flag the render pipeline reads
			// per-frame via ForEachLoadedFeature without synchronization, hot-toggled by direct
			// assignment — touch it ONLY on the main thread. The applied value is reported via
			// the communityshaders.feature.changed event (authoritative; the response can't know an
			// implicit flip's result synchronously).
			const bool hasExplicit = a_args.contains("enabled");
			const bool explicitVal = a_args.value("enabled", false);
			task->AddTask([target, hasExplicit, explicitVal, shortName]() {
				const bool applied = hasExplicit ? explicitVal : !target->loaded;
				if (applied && !target->loaded && !g_toggledOffFeatures.contains(target))
					return;
				SceneSettingsManager::SceneLayerGuard sceneLayerGuard;
				if (target->loaded && !applied)
					g_toggledOffFeatures.insert(target);
				target->loaded = applied;
				globals::shaderCache->ResetActiveShaderTracking();
				if (auto* dvb = DevBenchAPI::GetDevBenchInterface001()) {
					const std::string payload = DumpForDevBench(json{ { "shortName", shortName }, { "enabled", applied } });
					dvb->EmitEvent("communityshaders.feature.changed", payload.c_str());
				}
			});
			json r{ { "action", "toggle" }, { "shortName", shortName }, { "queued", true }, { "enqueued_at_frame", frame } };
			if (hasExplicit)
				r["requested"] = explicitVal;  // implicit flip's result arrives via the event
			return r;
		}

		if (shortName.empty())
			return json{ { "error", "missing required parameter 'shortName'" }, { "action", action } };

		// get / set / reset operate on a loaded feature. FindFeatureByShortName filters on
		// Feature::loaded, which queued toggle tasks mutate on the main thread — so resolve the
		// target INSIDE the main-thread path for each action, never on the listener thread.

		if (action == "get") {
			return RunOnMainThread([shortName]() -> json {
				auto* feature = Feature::FindFeatureByShortName(shortName);
				if (!feature)
					return json{ { "error", "feature not found or not loaded" }, { "shortName", shortName } };
				SceneSettingsManager::SceneLayerGuard sceneLayerGuard;
				json blob;
				feature->SaveSettings(blob);
				return blob;
			});
		}

		// set / reset resolve + apply on the main thread and report the real outcome: an invalid
		// shortName must NOT come back as a fake success. Synchronous (LoadSettings is fast), so
		// the lookup race is avoided AND the caller learns whether the mutation actually applied.
		if (action == "set") {
			if (!a_args.contains("settings") || !a_args["settings"].is_object())
				return json{ { "error", "missing required object parameter 'settings'" } };
			json blob = a_args["settings"];
			return RunOnMainThread([blob, shortName]() mutable -> json {
				auto* feature = Feature::FindFeatureByShortName(shortName);
				if (!feature)
					return json{ { "error", "feature not found or not loaded" }, { "shortName", shortName } };
				SceneSettingsManager::SceneLayerGuard sceneLayerGuard;
				try {
					feature->LoadSettings(blob);
					logger::info("DevBenchBridge: feature(set, {}) applied", shortName);
					return json{ { "action", "set" }, { "shortName", shortName }, { "applied", true } };
				} catch (const std::exception& e) {
					return json{ { "error", "LoadSettings threw" }, { "shortName", shortName }, { "detail", e.what() } };
				}
			});
		}

		if (action == "reset") {
			return RunOnMainThread([shortName]() -> json {
				auto* feature = Feature::FindFeatureByShortName(shortName);
				if (!feature)
					return json{ { "error", "feature not found or not loaded" }, { "shortName", shortName } };
				SceneSettingsManager::SceneLayerGuard sceneLayerGuard;
				try {
					feature->RestoreDefaultSettings();
					logger::info("DevBenchBridge: feature(reset, {}) applied", shortName);
					return json{ { "action", "reset" }, { "shortName", shortName }, { "applied", true } };
				} catch (const std::exception& e) {
					return json{ { "error", "RestoreDefaultSettings threw" }, { "shortName", shortName }, { "detail", e.what() } };
				}
			});
		}

		return json{ { "error", "unknown action (list|get|set|reset|toggle)" }, { "action", action } };
	}

	/**
	 * @brief Handles feature-related DevBench tool requests.
	 *
	 * Routes feature operations (list, get, set, reset, toggle) through the standard
	 * handler wrapper to ensure exceptions do not cross the DLL boundary and exactly
	 * one response is written to the output sink.
	 */
	void FeatureToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler("communityshaders.feature", &BuildFeatureResult, a_argsJson, a_sink, a_write);
	}

	/**
	 * @brief Builds inspection data for the specified kind query (state, profiler, or shader cache).
	 *
	 * @param a_args JSON object containing the inspection query; must include a required `kind` field.
	 *               Optional `filter` parameter narrows profiler results to entries whose name contains the filter string.
	 * @return JSON object with the requested inspection data, or an error object if `kind` is missing or unsupported.
	 *         For `kind="state"`, includes plugin name and frame count.
	 *         For `kind="profiler"`, includes GPU/CPU timing statistics and per-pass history samples.
	 *         For `kind="shadercache"`, includes compilation status and task/failure counts.
	 */

	json BuildInspectResult(const json& a_args)
	{
		const std::string kind = a_args.value("kind", std::string{});
		if (kind.empty())
			return json{ { "error", "missing required parameter 'kind'" } };

		if (kind == "state") {
			return json{
				{ "plugin", "CommunityShaders" },
				{ "frame_count", EnqueuedFrame() },
			};
		}
		if (kind == "profiler") {
			const std::string filter = a_args.value("filter", std::string{});
			return RunOnMainThread([filter]() -> json {
				auto* profiler = globals::profiler;
				if (!profiler)
					return json{ { "error", "profiler unavailable" } };
				profiler->KeepActive(3600);
				json passes = json::array();
				for (const auto& r : profiler->GetStatistics()) {
					if (!r.valid)
						continue;
					if (!filter.empty() && r.name.find(filter) == std::string::npos)
						continue;
					json entry{
						{ "name", r.name },
						{ "gpuMs", r.gpuTimeMs },
						{ "gpuAvgMs", r.avgMs },
						{ "gpuP95Ms", r.p95Ms },
						{ "gpuP99Ms", r.p99Ms },
						{ "cpuMs", r.cpuTimeMs },
						{ "cpuAvgMs", r.cpuAvgMs },
					};
					json hist = json::array();
					for (uint32_t i = 0; i < r.historyCount; ++i)
						hist.push_back(r.GetHistorySample(i));
					entry["gpuHistory"] = hist;
					passes.push_back(entry);
				}
				const auto frame = profiler->GetFrameStats();
				return json{
					{ "frame", {
								   { "ms", frame.lastMs },
								   { "avgMs", frame.avgMs },
								   { "p95Ms", frame.p95Ms },
								   { "p99Ms", frame.p99Ms },
								   { "samples", frame.count },
								   { "frameGeneration", globals::features::upscaling.IsFrameGenerationActive() },
							   } },
					{ "totalGpuMs", profiler->GetTotalTimeMs() },
					{ "totalCpuMs", profiler->GetCpuTotalTimeMs() },
					{ "frame_count", EnqueuedFrame() },
					{ "passes", passes },
				};
			});
		}
		if (kind == "drawcalls") {
			// Per-shader draw calls and CPU time as the performance overlay shows them; collection is
			// kept on for ~60 s after each query so the overlay need not be open.
			return RunOnMainThread([]() -> json {
				auto* state = globals::state;
				if (!state)
					return json{ { "error", "state unavailable" } };
				state->drawStatsUntilFrame.store(state->frameCount + 3600, std::memory_order_relaxed);
				json types = json::object();
				for (int i = 0; i <= magic_enum::enum_integer(RE::BSShader::Type::Total); ++i) {
					const auto name = i == magic_enum::enum_integer(RE::BSShader::Type::Total) ? std::string("Total") : std::string(magic_enum::enum_name(static_cast<RE::BSShader::Type>(i)));
					if (name.empty())
						continue;
					types[name] = { { "drawCalls", state->smoothDrawCalls[i] }, { "cpuMs", state->smoothFrameTimePerType[i] } };
				}
				json frame = json::object();
				if (auto* profiler = globals::profiler) {
					const auto stats = profiler->GetFrameStats();
					frame = { { "ms", stats.lastMs }, { "avgMs", stats.avgMs }, { "p95Ms", stats.p95Ms } };
				}
				// Utility draws by pixel descriptor, averaged over the frames since the previous query, then cleared.
				json utility = json::array();
				{
					std::vector<std::pair<uint64_t, State::UtilityDrawStat>> rows(state->utilityDrawStats.begin(), state->utilityDrawStats.end());
					const double frames = static_cast<double>(std::max<uint64_t>(state->utilityStatFrames, 1));
					std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.draws > b.second.draws; });
					for (size_t i = 0; i < rows.size() && i < 20; ++i)
						utility.push_back({ { "descriptor", std::format("{:#x}", rows[i].first & 0xFFFFFFFFull) }, { "depthTarget", std::string(magic_enum::enum_name(static_cast<decltype(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN)>(rows[i].first >> 32))) }, { "drawsPerFrame", static_cast<double>(rows[i].second.draws) / frames }, { "defines", rows[i].second.defines } });
					state->utilityDrawStats.clear();
					state->utilityStatFrames = 0;
				}
				return json{
					{ "frame_count", state->frameCount },
					{ "frame", frame },
					{ "frameGeneration", globals::features::upscaling.IsFrameGenerationActive() },
					{ "neuralRendering", globals::features::neuralRendering.loaded && globals::features::neuralRendering.settings.enabled },
					{ "utility", utility },
					{ "shaders", types },
				};
			});
		}
		if (kind == "sky") {
			return RunOnMainThread([]() -> json {
				const auto d = globals::features::skySync.GetDebugState(globals::game::sky);
				return json{
					{ "currentSkyRotation", d.currentSkyRotation }, { "cellNorthRotation", d.cellNorthRotation },
					{ "rootRotate", std::vector<float>(std::begin(d.rootRotate), std::end(d.rootRotate)) },
					{ "active", d.active }, { "skyRotationApplied", d.skyRotationApplied }, { "cellIsCurrent", d.cellIsCurrent },
					{ "cellInterior", d.cellInterior }, { "reasserts", d.reasserts },
					{ "reassertEnabled", globals::features::skySync.settings.ReassertSkyRotation },
					{ "interiorWithSun", globals::features::interiorSun.isInteriorWithSun.load() },
				};
			});
		}
		if (kind == "lights") {
			return RunOnMainThread([]() -> json {
				auto& llf = globals::features::lightLimitFix;
				const auto& t = llf.lightTally;
				return json{
					{ "loaded", llf.loaded },
					{ "localShadowCacheActive", llf.IsLocalShadowCacheActive() },
					{ "keepShadowlessSpots", llf.settings.KeepShadowlessSpots },
					{ "frame", t.frame }, { "seen", t.seen }, { "shadowLights", t.shadowLights }, { "spots", t.spots },
					{ "keptLocalShadow", t.keptLocalShadow }, { "keptShadowMask", t.keptShadowMask },
					{ "droppedShadowless", t.droppedShadowless }, { "droppedShadowlessSpots", t.droppedShadowlessSpots },
					{ "keptShadowless", t.keptShadowless }, { "droppedFaded", t.droppedFaded },
					{ "droppedFadedShadowLights", t.droppedFadedShadowLights }, { "pushed", t.pushed },
					{ "ignoreLightLODDimmer", llf.settings.IgnoreLightLODDimmer },
					{ "dropped", [&]() { json a = json::array(); for (const auto& d : llf.droppedLights) a.push_back(json{ { "name", d.name }, { "distance", d.distance }, { "radius", d.radius }, { "colorSum", d.colorSum }, { "niFade", d.niFade }, { "lodDimmer", d.lodDimmer }, { "finalFade", d.finalFade }, { "shadow", d.shadow }, { "spot", d.spot }, { "global", d.global } }); return a; }() },
				};
			});
		}
		if (kind == "exposure") {
			return RunOnMainThread([]() -> json {
				auto& post = globals::features::postProcessing;
				auto* e = post.GetPipelineFeature<HistogramAutoExposure>(PostProcessing::FeaturePipelineIndex::AutoExposure);
				if (!e)
					return json{ { "error", "auto exposure unavailable" } };
				const auto p = e->GetExposureParameters();
				const float lum = std::max(e->preExposureLuminance, 1e-12f);
				return json{
					{ "active", e->IsActive() },
					{ "pipelineRunning", post.IsPipelineRunning() },
					{ "meteredLuminance", e->preExposureLuminance },
					{ "meteredEV100", std::log2(lum) + 3.0f },
					{ "meteredFrame", e->preExposureCompletedFrame },
					{ "preExposure", globals::state->preExposure },
					{ "rangeEV100", json::array({ std::log2(std::max(p.LuminanceRange.x, 1e-12f)) + 3.0f, std::log2(std::max(p.LuminanceRange.y, 1e-12f)) + 3.0f }) },
					{ "compensationEV", p.CompensationEV },
					{ "exposureTarget", p.ExposureTarget },
					{ "frame_count", globals::state->frameCount },
				};
			});
		}
		if (kind == "shadercache") {
			// Built from thread-safe ShaderCache accessors. Poll completedTasks against a
			// pre-deploy snapshot to know a hot-reloaded shader finished; a rising
			// failedTasks / currentFailedCount surfaces an otherwise-invisible failed compile.
			auto* cache = globals::shaderCache;
			if (!cache)
				return json{ { "error", "shader cache unavailable" } };
			// A changed total means matrix work began while standalonePending was read, so idle can't be claimed.
			const uint64_t totalBefore = cache->GetTotalTasks();
			const bool compiling = cache->IsCompiling();
			const uint32_t standalonePending = cache->GetStandalonePending();
			const uint64_t totalTasks = cache->GetTotalTasks();
			return json{
				{ "compiling", compiling || totalTasks != totalBefore },
				{ "standalonePending", standalonePending },
				{ "completedTasks", cache->GetCompletedTasks() },
				{ "totalTasks", totalTasks },
				{ "failedTasks", cache->GetFailedTasks() },
				{ "currentFailedCount", cache->GetCurrentFailedCount() },
				{ "frame_count", EnqueuedFrame() },
			};
		}
		return json{ { "error", "unknown kind" }, { "kind", kind }, { "supported", json::array({ "state", "shadercache", "profiler", "drawcalls", "exposure" }) } };
	}

	/**
	 * @brief Handles DevBench inspection requests for plugin state, profiler metrics, and shader cache status.
	 */
	void InspectToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler("communityshaders.inspect", &BuildInspectResult, a_argsJson, a_sink, a_write);
	}

	/**
	 * @brief Dispatches shader cache operations (clear or deleteDisk) to the main thread.
	 *
	 * Validates that the shader cache and task interface are available, then enqueues
	 * the requested operation on the main thread. Returns immediately with a queued status
	 * or an error if preconditions are not met.
	 *
	 * @param a_args JSON object containing an optional "action" field ("clear" or "deleteDisk").
	 * @return JSON object with `{ action, queued, enqueued_at_frame, note }` on success,
	 *         or `{ error, ... }` if the shader cache is unavailable, task interface is unavailable,
	 *         or action is unrecognized.
	 */

	json BuildShadercacheResult(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string{});
		auto* cache = globals::shaderCache;
		if (!cache)
			return json{ { "error", "shader cache unavailable" } };
		auto* task = SKSE::GetTaskInterface();
		if (!task)
			return json{ { "error", "SKSE task interface unavailable" } };
		const uint frame = EnqueuedFrame();

		// Mutating cache ops touch the live ShaderCache (and, for deleteDisk, the filesystem)
		// — marshal to the main thread. Recompiles are observable via inspect(kind=shadercache)
		// + the communityshaders.shaderRecompiled event. NOTE clear vs deleteDisk: clear only drops
		// the in-memory maps, so with the disk cache enabled shaders reload from Data/ShaderCache
		// rather than recompiling — only deleteDisk guarantees a cold recompile.
		if (action == "clear") {
			task->AddTask([cache]() { cache->Clear(); });
			return json{ { "action", "clear" }, { "queued", true }, { "enqueued_at_frame", frame }, { "note", "in-memory cache dropped; shaders reload from the disk cache if present, else recompile (use deleteDisk to force a cold recompile)" } };
		}
		if (action == "deleteDisk") {
			// Delete on disk AND drop the in-memory cache — otherwise existing variants keep
			// serving from memory and the promised full recompile never happens (mirrors
			// PerformClearShaderCache and the ShaderCache invalidation path).
			task->AddTask([cache]() {
				cache->DeleteDiskCache();
				cache->Clear();
			});
			return json{ { "action", "deleteDisk" }, { "queued", true }, { "enqueued_at_frame", frame }, { "note", "on-disk + in-memory shader cache cleared; a full recompile follows (cold-compile benchmark)" } };
		}
		return json{ { "error", "unknown action (clear|deleteDisk)" }, { "action", action } };
	}

	/**
	 * @brief Processes DevBench requests for shader cache operations.
	 */
	void ShadercacheToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler("communityshaders.shadercache", &BuildShadercacheResult, a_argsJson, a_sink, a_write);
	}

	/**
	 * @brief Builds a JSON response for a capture request.
	 *
	 * Processes RenderDoc or screenshot capture operations based on the `kind` parameter.
	 * Validates feature availability and queues the capture on the main thread.
	 *
	 * @param a_args JSON object containing `kind` (required) and optional `frames` for RenderDoc.
	 * @return JSON response indicating queued state or error details.
	 */

	json BuildCaptureResult(const json& a_args)
	{
		const std::string kind = a_args.value("kind", std::string{});
		if (kind.empty())
			return json{ { "error", "missing required parameter 'kind'" } };
		const uint frame = EnqueuedFrame();

		if (kind == "renderdoc") {
			uint32_t frameCount = 1;
			if (a_args.contains("frames") && a_args["frames"].is_number())
				frameCount = static_cast<uint32_t>(std::clamp(a_args["frames"].get<int>(), 1, 120));
			// All on the main thread: Feature::loaded is mutated by queued toggle tasks there,
			// and TriggerCapture invalidates RenderDoc's non-atomic capture-list cache the
			// UI/render thread reads.
			return RunOnMainThread([frameCount, frame]() -> json {
				auto* renderDoc = &globals::features::renderDoc;
				if (!globals::features::developerTools.loaded)
					return json{ { "error", "Developer Tools feature is not loaded" }, { "hint", "communityshaders.feature(action='list') shows DeveloperTools.loaded" } };
				if (!renderDoc->IsAvailable())
					return json{ { "error", "RenderDoc API not available — attach RenderDoc or load the in-app DLL" } };
				if (frameCount == 1)
					renderDoc->TriggerCapture();
				else
					renderDoc->TriggerMultiFrameCapture(frameCount);
				return json{ { "queued", true }, { "kind", "renderdoc" }, { "frames", frameCount }, { "enqueued_at_frame", frame } };
			});
		}

		if (kind == "screenshot") {
			const std::string tag = a_args.value("tag", std::string{});
			const bool wait = a_args.value("wait", true);
			const int timeoutMs = std::clamp(a_args.value("timeoutMs", 15000), 1000, 60000);
			auto baseline = std::make_shared<std::pair<uint32_t, uint32_t>>(0u, 0u);
			// loaded read on the main thread (toggle tasks mutate it); the request flag is atomic.
			json queued = RunOnMainThread([frame, tag, baseline]() -> json {
				auto* shot = &globals::features::screenshotFeature;
				if (!shot->loaded)
					return json{ { "error", "Screenshot feature is not loaded" } };
				baseline->first = shot->savedCount.load(std::memory_order_acquire);
				baseline->second = shot->failedCount.load(std::memory_order_acquire);
				{
					std::lock_guard<std::mutex> lock(shot->captureInfoMutex);
					shot->pendingTag = tag;
				}
				shot->captureRequested.store(true, std::memory_order_release);
				return json{ { "queued", true }, { "kind", "screenshot" }, { "enqueued_at_frame", frame } };
			});
			if (!wait || queued.contains("error"))
				return queued;
			// Wait on the listener thread for the worker to write the file.
			auto* shot = &globals::features::screenshotFeature;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
			while (std::chrono::steady_clock::now() < deadline) {
				if (shot->savedCount.load(std::memory_order_acquire) != baseline->first) {
					std::lock_guard<std::mutex> lock(shot->captureInfoMutex);
					queued["saved"] = true;
					queued["path"] = shot->lastSavedPath;
					return queued;
				}
				if (shot->failedCount.load(std::memory_order_acquire) != baseline->second) {
					queued["saved"] = false;
					queued["error"] = "screenshot failed to save; see the CS log";
					return queued;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(25));
			}
			queued["saved"] = false;
			queued["error"] = "timed out waiting for the screenshot file";
			return queued;
		}

		return json{ { "error", "unknown kind" }, { "kind", kind }, { "supported", json::array({ "renderdoc", "screenshot" }) } };
	}

	/**
	 * @brief Processes capture requests for RenderDoc and screenshot operations.
	 *
	 * DevBench tool handler that triggers RenderDoc frame captures or screenshot
	 * captures based on the provided `kind` parameter.
	 */
	void CaptureToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler("communityshaders.capture", &BuildCaptureResult, a_argsJson, a_sink, a_write);
	}

	// ---- nvperf: status / metrics / capture / cancel / result ---------------------------

	const char* CaptureStateName(NvPerf::CaptureState a_state)
	{
		switch (a_state) {
		case NvPerf::CaptureState::Waiting:
			return "waiting";
		case NvPerf::CaptureState::Capturing:
			return "capturing";
		case NvPerf::CaptureState::Decoding:
			return "decoding";
		case NvPerf::CaptureState::Done:
			return "done";
		case NvPerf::CaptureState::Failed:
			return "failed";
		default:
			return "idle";
		}
	}

	json StatusJson(const NvPerf::Status& a_status)
	{
		return json{ { "state", CaptureStateName(a_status.state) }, { "captureId", a_status.captureId }, { "sample", a_status.sample },
			{ "samples", a_status.samples }, { "pass", a_status.pass }, { "passes", a_status.passes }, { "error", a_status.error } };
	}

	json MetricJson(const NvPerf::MetricRequest& a_metric)
	{
		return json{ { "metric", NvPerf::FormatMetric(a_metric) }, { "label", NvPerf::MetricLabel(a_metric) } };
	}

	/**
	 * @brief Serializes a capture result: the per-filter summary always, every range of every sample on request.
	 *
	 * Summary values are per frame and keyed by metric label, perSample holds them per sample; a value no replay pass completed is null.
	 */
	json CaptureResultJson(const NvPerf::CaptureResult& a_result, bool a_detail)
	{
		json metrics = json::array();
		std::vector<std::string> labels;
		for (const auto& metric : a_result.metrics) {
			metrics.push_back(MetricJson(metric));
			labels.push_back(NvPerf::MetricLabel(metric));
		}
		json unsupported = json::array();
		for (const auto& metric : a_result.unsupportedMetrics)
			unsupported.push_back(MetricJson(metric));
		const auto number = [](double a_value) { return std::isnan(a_value) ? json(nullptr) : json(a_value); };
		const auto values = [&](const std::vector<double>& a_values) {
			json out = json::object();
			for (std::size_t m = 0; m < labels.size() && m < a_values.size(); ++m)
				out[labels[m]] = number(a_values[m]);
			return out;
		};
		const auto perSample = [&](const std::vector<std::vector<double>>& a_series) {
			json out = json::object();
			for (std::size_t m = 0; m < labels.size() && m < a_series.size(); ++m) {
				json column = json::array();
				for (const double value : a_series[m])
					column.push_back(number(value));
				out[labels[m]] = std::move(column);
			}
			return out;
		};
		json summary = json::array();
		for (const auto& entry : a_result.summary)
			summary.push_back(json{ { "range", entry.name }, { "perFrame", entry.occurrencesPerFrame }, { "incomplete", entry.incomplete },
				{ "values", values(entry.values) }, { "perSample", perSample(entry.perSample) } });
		json out{ { "captureId", a_result.id }, { "success", a_result.success }, { "error", a_result.error }, { "chip", a_result.chipName },
			{ "startFrame", a_result.startFrame }, { "endFrame", a_result.endFrame }, { "samples", a_result.samples },
			{ "passesPerSample", a_result.passesPerSample }, { "droppedRanges", a_result.droppedRanges },
			{ "droppedTraceBytes", a_result.droppedTraceBytes }, { "ranges", a_result.ranges }, { "metrics", std::move(metrics) },
			{ "unsupportedMetrics", std::move(unsupported) }, { "summary", std::move(summary) }, { "csvPath", Util::PathHelpers::ToUtf8(a_result.csvPath) } };
		if (a_detail) {
			json ranges = json::array();
			for (const auto& range : a_result.rangeResults)
				ranges.push_back(json{ { "sample", range.sample }, { "range", range.name }, { "path", range.path }, { "occurrence", range.occurrence },
					{ "values", values(range.values) } });
			out["rangeResults"] = std::move(ranges);
		}
		return out;
	}

	/**
	 * @brief Builds the response of the communityshaders.nvperf tool.
	 *
	 * Captures run over the next frames on the render thread, so `capture` only queues one and returns its id; poll
	 * `result` (or wait for the communityshaders.nvperf.completed event) for its counters. No call here touches the
	 * immediate context, so none marshals to the main thread.
	 */
	json BuildNvPerfResult(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string{});
		if (action.empty())
			return json{ { "error", "missing required parameter 'action'" } };

		if (action == "status") {
			NvPerf::Probe();
			const auto device = NvPerf::GetDeviceInfo();
			return json{ { "built", device.built }, { "available", device.available }, { "gpuSupported", device.gpuSupported },
				{ "device", device.deviceName }, { "chip", device.chipName }, { "deviceError", device.error },
				{ "library", Util::PathHelpers::ToUtf8(device.libraryPath) }, { "capture", StatusJson(NvPerf::GetStatus()) } };
		}

		if (action == "metrics") {
			json metrics = json::array();
			for (const auto& metric : NvPerf::DefaultMetrics())
				metrics.push_back(MetricJson(metric));
			return json{ { "defaults", std::move(metrics) },
				{ "kinds", json::array({ "counter", "ratio", "throughput", "active", "elapsed", "percycle" }) } };
		}

		if (action == "capture") {
			NvPerf::CaptureConfiguration configuration;
			if (a_args.contains("metrics")) {
				if (!a_args["metrics"].is_array())
					return json{ { "error", "metrics must be an array of \"name[:kind]\" strings" } };
				for (const auto& entry : a_args["metrics"]) {
					if (!entry.is_string())
						return json{ { "error", "metrics must be an array of \"name[:kind]\" strings" } };
					NvPerf::MetricRequest metric;
					std::string error;
					if (!NvPerf::ParseMetric(entry.get<std::string>(), metric, error))
						return json{ { "error", error } };
					configuration.metrics.push_back(std::move(metric));
				}
			}
			if (a_args.contains("ranges")) {
				if (!a_args["ranges"].is_array())
					return json{ { "error", "ranges must be an array of strings" } };
				for (const auto& entry : a_args["ranges"]) {
					if (!entry.is_string() || entry.get<std::string>().empty())
						return json{ { "error", "ranges must be an array of non-empty strings" } };
					configuration.ranges.push_back(entry.get<std::string>());
				}
			}
			configuration.samples = a_args.value("samples", 1u);
			configuration.delayFrames = a_args.value("delayFrames", 0u);
			configuration.nestingLevels = static_cast<std::uint16_t>(std::clamp(a_args.value("nestingLevels", 1u), 0u, 0xFFFFu));
			std::uint64_t captureId = 0;
			std::string error;
			if (!NvPerf::RequestCapture(configuration, captureId, error))
				return json{ { "error", error } };
			return json{ { "queued", true }, { "captureId", captureId }, { "enqueued_at_frame", EnqueuedFrame() } };
		}

		if (action == "cancel") {
			NvPerf::CancelCapture();
			return json{ { "cancelRequested", true }, { "capture", StatusJson(NvPerf::GetStatus()) } };
		}

		if (action == "result") {
			const auto status = NvPerf::GetStatus();
			const auto result = NvPerf::LatestResult();
			const std::uint64_t wanted = a_args.value("captureId", result ? result->id : status.captureId);
			if (result && result->id == wanted)
				return CaptureResultJson(*result, a_args.value("detail", false));
			if (status.captureId == wanted && wanted)
				return json{ { "pending", true }, { "capture", StatusJson(status) } };
			return json{ { "error", "no such capture" }, { "captureId", wanted } };
		}

		return json{ { "error", "unknown action (status|metrics|capture|cancel|result)" }, { "action", action } };
	}

	void NvPerfToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler("communityshaders.nvperf", &BuildNvPerfResult, a_argsJson, a_sink, a_write);
	}

	// ---- frames: start / stop / status / read --------------------------------------------

	constexpr double kMsRounding = 1e4;  // 4 decimals keeps a 10 s window around 60 KB

	/** @brief Serializes the recorder state without the frames. */
	json FrameStatusJson(const FrameRecorder::Status& a_status)
	{
		return json{ { "id", a_status.id }, { "recording", a_status.recording }, { "frames", a_status.frames }, { "full", a_status.full } };
	}

	/** @brief Rounds a duration to kMsRounding precision. */
	json RoundedMs(double a_ms)
	{
		return json(std::round(a_ms * kMsRounding) / kMsRounding);
	}

	/** @brief Serializes a recording as one array per column, one entry per frame. */
	json RecordingJson(const FrameRecorder::Recording& a_recording)
	{
		json frameMs = json::array(), threadCpu = json::array(), processCpu = json::array(), frameGeneration = json::array();
		for (const auto& frame : a_recording.frames) {
			frameMs.push_back(RoundedMs(frame.frameMs));
			threadCpu.push_back(RoundedMs(frame.presentThreadCpuMs));
			processCpu.push_back(RoundedMs(frame.processCpuMs));
			frameGeneration.push_back(frame.frameGeneration);
		}
		auto out = FrameStatusJson({ .id = a_recording.id, .recording = a_recording.recording, .full = a_recording.full, .frames = a_recording.frames.size() });
		out["tscHz"] = a_recording.tscHz;
		out["presentThreadId"] = a_recording.presentThreadId;
		out["columns"] = json{ { "frameMs", std::move(frameMs) }, { "presentThreadCpuMs", std::move(threadCpu) },
			{ "processCpuMs", std::move(processCpu) }, { "frameGeneration", std::move(frameGeneration) } };
		return out;
	}

	/**
	 * @brief Builds the response of the communityshaders.frames tool.
	 *
	 * Start and stop take effect at the next Present on the render thread; nothing here touches game state, so none
	 * marshals to the main thread.
	 */
	json BuildFramesResult(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string{});
		if (action == "start") {
			std::uint64_t id = 0;
			std::string error;
			if (!FrameRecorder::Start(id, error))
				return json{ { "error", error } };
			return json{ { "id", id } };
		}
		if (action == "stop") {
			FrameRecorder::Stop();
			return FrameStatusJson(FrameRecorder::GetStatus());
		}
		if (action == "status")
			return FrameStatusJson(FrameRecorder::GetStatus());
		if (action == "read")
			return RecordingJson(*FrameRecorder::Read());
		return json{ { "error", "unknown action (start|stop|status|read)" }, { "action", action } };
	}

	/** @brief DevBench entry point of the communityshaders.frames tool. */
	void FramesToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler("communityshaders.frames", &BuildFramesResult, a_argsJson, a_sink, a_write);
	}

	/**
	 * @brief Builds a JSON response for global settings operations.
	 *
	 * Enqueues a main-thread task to perform the specified action on the global Community Shaders
	 * configuration. Returns immediately with queuing status and frame information.
	 *
	 * @param a_args JSON object containing an "action" field: `save`, `load`, or `reset`.
	 * @return JSON object with `action`, `queued`, and `enqueued_at_frame` on success;
	 *         `error` and optionally `action` on validation failure.
	 */

	json BuildSettingsResult(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string{});
		if (action.empty())
			return json{ { "error", "missing required parameter 'action'" } };
		auto* state = globals::state;
		if (!state)
			return json{ { "error", "State singleton unavailable" } };
		auto* task = SKSE::GetTaskInterface();
		if (!task)
			return json{ { "error", "SKSE task interface unavailable" } };
		const uint frame = EnqueuedFrame();

		// State::Save/Load read and write the on-disk USER config and touch every feature's
		// settings; both must run on the main thread for the same reason feature(set) does.
		// Contain failures inside the task: RunHandler's guard no longer applies once this runs
		// on SKSE's queue, so a malformed config / Save|Load throw would unwind on the game
		// thread after we already replied "queued". Degrade to a logged error instead.
		if (action == "save") {
			task->AddTask([state]() {
				try {
					state->Save();
					logger::info("DevBenchBridge: settings(save) applied");
				} catch (const std::exception& e) {
					logger::error("DevBenchBridge: settings(save) failed: {}", e.what());
				} catch (...) {
					logger::error("DevBenchBridge: settings(save) failed (unknown)");
				}
			});
			return json{ { "action", "save" }, { "queued", true }, { "enqueued_at_frame", frame } };
		}
		if (action == "load") {
			task->AddTask([state]() {
				try {
					state->Load(/*allowReload=*/true);
					logger::info("DevBenchBridge: settings(load) applied");
				} catch (const std::exception& e) {
					logger::error("DevBenchBridge: settings(load) failed: {}", e.what());
				} catch (...) {
					logger::error("DevBenchBridge: settings(load) failed (unknown)");
				}
			});
			return json{ { "action", "load" }, { "queued", true }, { "enqueued_at_frame", frame } };
		}
		if (action == "reset") {
			// Restore every feature to its defaults, then persist. Mirrors what the UI's
			// global reset does: per-feature RestoreDefaultSettings followed by a Save.
			task->AddTask([state]() {
				SceneSettingsManager::SceneLayerGuard sceneLayerGuard;
				for (auto* f : Feature::GetFeatureList()) {
					try {
						f->RestoreDefaultSettings();
					} catch (const std::exception& e) {
						logger::error("DevBenchBridge: settings(reset) {} threw: {}", f->GetShortName(), e.what());
					} catch (...) {
						logger::error("DevBenchBridge: settings(reset) {} threw (unknown)", f->GetShortName());
					}
				}
				try {
					state->Save();
					logger::info("DevBenchBridge: settings(reset) applied");
				} catch (const std::exception& e) {
					logger::error("DevBenchBridge: settings(reset) save failed: {}", e.what());
				} catch (...) {
					logger::error("DevBenchBridge: settings(reset) save failed (unknown)");
				}
			});
			return json{ { "action", "reset" }, { "queued", true }, { "enqueued_at_frame", frame } };
		}
		return json{ { "error", "unknown action (save|load|reset)" }, { "action", action } };
	}

	/**
	 * @brief Processes DevBench settings tool requests (save, load, reset).
	 */
	void SettingsToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler("communityshaders.settings", &BuildSettingsResult, a_argsJson, a_sink, a_write);
	}

	/** @brief Every known dev flag and its value, as {"flags": {name: value}}. */
	json DevFlagsJson()
	{
		json flags = json::object();
		for (const auto& [name, value] : DevFlags::List())
			flags[name] = value;
		return json{ { "flags", std::move(flags) } };
	}

	/**
	 * @brief Builds the response of the communityshaders.devflags tool.
	 *
	 * Flags are atomics read on any thread, so nothing marshals to the main thread.
	 */
	json BuildDevFlagsResult(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string{ "list" });
		if (action == "set") {
			const auto flags = a_args.find("flags");
			if (flags == a_args.end() || !flags->is_object())
				return json{ { "error", "set needs flags: {name: bool}" } };
			for (const auto& item : flags->items()) {
				if (!item.value().is_boolean())
					return json{ { "error", "flag values must be booleans" }, { "flag", item.key() } };
			}
			for (const auto& item : flags->items())
				DevFlags::Set(item.key(), item.value().get<bool>());
			return DevFlagsJson();
		}
		if (action == "reset")
			DevFlags::Reset();
		else if (action != "list")
			return json{ { "error", "unknown action (set|list|reset)" }, { "action", action } };
		return DevFlagsJson();
	}

	/** @brief DevBench entry point of the communityshaders.devflags tool. */
	void DevFlagsToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler("communityshaders.devflags", &BuildDevFlagsResult, a_argsJson, a_sink, a_write);
	}

	// ---- world: weather / time / position / console / save control ---------------------------
	// Lets a remote driver put the game in a known state (weather, hour, place) so look
	// changes can be compared like for like. All engine access runs on the main thread.

	json WeatherJson(const RE::TESWeather* a_weather)
	{
		if (!a_weather)
			return nullptr;
		const char* editorID = a_weather->GetFormEditorID();
		return json{ { "formID", std::format("0x{:08X}", a_weather->GetFormID()) }, { "editorID", editorID ? editorID : "" } };
	}

	RE::TESWeather* ResolveWeather(const json& a_args, std::string& a_error)
	{
		if (a_args.contains("id")) {
			uint32_t id = 0;
			try {
				id = a_args["id"].is_number() ? a_args["id"].get<uint32_t>() : static_cast<uint32_t>(std::stoul(a_args["id"].get<std::string>(), nullptr, 16));
			} catch (...) {
				a_error = "could not parse 'id' (use a full form ID such as 0x0010A236)";
				return nullptr;
			}
			if (auto* weather = RE::TESForm::LookupByID<RE::TESWeather>(id))
				return weather;
			a_error = "no weather with that form ID";
			return nullptr;
		}
		if (a_args.contains("name") && a_args["name"].is_string()) {
			const auto name = a_args["name"].get<std::string>();
			auto* dataHandler = RE::TESDataHandler::GetSingleton();
			if (!dataHandler) {
				a_error = "data handler unavailable";
				return nullptr;
			}
			for (auto* weather : dataHandler->GetFormArray<RE::TESWeather>()) {
				if (!weather)
					continue;
				const char* editorID = weather->GetFormEditorID();
				if (editorID && _stricmp(editorID, name.c_str()) == 0)
					return weather;
			}
			a_error = "no weather with that editor ID";
			return nullptr;
		}
		a_error = "provide 'id' (full form ID) or 'name' (editor ID)";
		return nullptr;
	}

	json BuildWorldState()
	{
		json out = json::object();
		if (auto* calendar = Util::Climate::GetCalendar()) {
			if (calendar->gameHour)
				out["gameHour"] = calendar->gameHour->value;
			if (calendar->timeScale)
				out["timeScale"] = calendar->timeScale->value;
		}
		if (auto* sky = globals::game::sky) {
			out["weather"] = WeatherJson(sky->currentWeather);
			out["outgoingWeather"] = WeatherJson(sky->lastWeather);
			out["weatherPct"] = sky->currentWeatherPct;
		}
		out["weatherLocked"] = EditorWindow::GetSingleton()->IsWeatherLocked();
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			const auto pos = player->GetPosition();
			out["position"] = json{ { "x", pos.x }, { "y", pos.y }, { "z", pos.z } };
			constexpr float kRadToDeg = 57.29577951f;
			out["angleDeg"] = json{ { "x", player->GetAngleX() * kRadToDeg }, { "z", player->GetAngleZ() * kRadToDeg } };
			if (auto* cell = player->GetParentCell()) {
				const char* editorID = cell->GetFormEditorID();
				const char* name = cell->GetName();
				out["cell"] = json{ { "formID", std::format("0x{:08X}", cell->GetFormID()) }, { "editorID", editorID ? editorID : "" }, { "name", name ? name : "" }, { "interior", cell->IsInteriorCell() } };
			}
			if (auto* worldspace = player->GetWorldspace()) {
				const char* editorID = worldspace->GetFormEditorID();
				out["worldspace"] = json{ { "formID", std::format("0x{:08X}", worldspace->GetFormID()) }, { "editorID", editorID ? editorID : "" } };
			}
		}
		return out;
	}

	// Console commands a remote driver may run. A prefix allowlist, not a free shell: the bridge
	// listens on loopback but any local process or web page can reach it.
	bool ConsoleCommandAllowed(const std::string& a_command)
	{
		std::string lower;
		for (char c : a_command)
			lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		static constexpr const char* kAllowed[] = {
			"coc ", "cow ", "player.setpos ", "player.setangle ", "player.moveto ", "tfc", "tcl", "tgm",
			"tm", "tai", "tcai", "tdetect", "set timescale to ", "set gamehour to ", "sucsm ", "fw ", "sw ",
			"forceweather ", "setweather ", "player.placeatme ", "tfc 1", "fov ", "tll", "tws", "showracemenu", "pcb", "tmm ", "player.unequipall", "player.equipitem ", "player.additem "
		};
		for (const char* prefix : kAllowed) {
			const std::string p = prefix;
			if (lower.compare(0, p.size(), p) == 0 && (p.back() == ' ' || lower.size() == p.size()))
				return true;
		}
		return false;
	}

	json BuildWorldResult(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string{});
		if (action.empty())
			return json{ { "error", "missing required parameter 'action'" }, { "supported", json::array({ "state", "list_weathers", "weather", "hour", "timescale", "teleport", "console", "load", "loadMostRecent", "quit" }) } };

		return RunOnMainThread([action, a_args]() -> json {
			if (action == "state")
				return BuildWorldState();

			if (action == "list_weathers") {
				auto* dataHandler = RE::TESDataHandler::GetSingleton();
				if (!dataHandler)
					return json{ { "error", "data handler unavailable" } };
				const std::string filter = a_args.value("filter", std::string{});
				json list = json::array();
				for (auto* weather : dataHandler->GetFormArray<RE::TESWeather>()) {
					if (!weather)
						continue;
					const char* editorID = weather->GetFormEditorID();
					const std::string id = editorID ? editorID : "";
					if (!filter.empty() && id.find(filter) == std::string::npos)
						continue;
					list.push_back(WeatherJson(weather));
				}
				return json{ { "count", list.size() }, { "weathers", list } };
			}

			if (action == "weather") {
				auto* sky = globals::game::sky;
				if (!sky)
					return json{ { "error", "sky unavailable (no world loaded?)" } };
				std::string error;
				auto* weather = ResolveWeather(a_args, error);
				if (!weather)
					return json{ { "error", error } };
				const bool instant = a_args.value("instant", true);
				if (instant)
					Util::ForceWeather(sky, weather, false);
				else
					sky->SetWeather(weather, true, false);
				auto* editor = EditorWindow::GetSingleton();
				if (a_args.contains("lock") && a_args["lock"].is_boolean()) {
					if (a_args["lock"].get<bool>())
						editor->LockWeatherForUser(weather);
					else
						editor->UnlockWeather();
				} else if (editor->IsWeatherLocked()) {
					editor->LockWeather(weather);
				}
				return json{ { "ok", true }, { "weather", WeatherJson(weather) }, { "instant", instant } };
			}

			if (action == "hour") {
				if (!a_args.contains("value") || !a_args["value"].is_number())
					return json{ { "error", "provide a numeric 'value' (0-24)" } };
				const float hour = std::clamp(a_args["value"].get<float>(), 0.0f, 23.999f);
				auto* calendar = Util::Climate::GetCalendar();
				if (!calendar || !calendar->gameHour)
					return json{ { "error", "calendar unavailable" } };
				calendar->gameHour->value = hour;
				if (auto* sky = globals::game::sky; sky && EditorWindow::GetSingleton()->IsWeatherLocked())
					sky->lastWeatherUpdate = hour;
				if (a_args.value("transition", true))
					Util::RequestTimeJumpTransition();
				return json{ { "ok", true }, { "gameHour", hour } };
			}

			if (action == "timescale") {
				if (!a_args.contains("value") || !a_args["value"].is_number())
					return json{ { "error", "provide a numeric 'value' (0 freezes time; 20 is the default)" } };
				const auto cmd = std::format("set timescale to {}", a_args["value"].get<float>());
				RE::Console::ExecuteCommand(cmd.c_str());
				return json{ { "ok", true }, { "command", cmd } };
			}

			// Reference shots: clouds, tree sway and the shader timer read a fixed clock (see ReferenceClock.h).
			if (action == "refclock") {
				if (a_args.contains("seconds") && a_args["seconds"].is_number())
					ReferenceClock::seconds.store(std::clamp(a_args["seconds"].get<float>(), 1.0f, 86400.0f));
				if (a_args.contains("on") && a_args["on"].is_boolean())
					ReferenceClock::on.store(a_args["on"].get<bool>());
				return json{ { "ok", true }, { "on", ReferenceClock::on.load() }, { "seconds", ReferenceClock::seconds.load() } };
			}

			// Stops the world (animation, physics, wind, weather) while rendering continues: identical frames for A/B shots.
			if (action == "freeze") {
				auto* main = RE::Main::GetSingleton();
				if (!main)
					return json{ { "error", "main unavailable" } };
				if (a_args.contains("on") && a_args["on"].is_boolean())
					main->GetRuntimeData().freezeTime = a_args["on"].get<bool>();
				return json{ { "ok", true }, { "frozen", main->GetRuntimeData().freezeTime } };
			}

			if (action == "teleport") {
				if (!RE::PlayerCharacter::GetSingleton())
					return json{ { "error", "player unavailable" } };
				json ran = json::array();
				auto run = [&](const std::string& cmd) {
					RE::Console::ExecuteCommand(cmd.c_str());
					ran.push_back(cmd);
				};
				for (const char* axis : { "x", "y", "z" })
					if (a_args.contains(axis) && a_args[axis].is_number())
						run(std::format("player.setpos {} {}", axis, a_args[axis].get<float>()));
				if (a_args.contains("rx") && a_args["rx"].is_number())
					run(std::format("player.setangle x {}", a_args["rx"].get<float>()));
				if (a_args.contains("rz") && a_args["rz"].is_number())
					run(std::format("player.setangle z {}", a_args["rz"].get<float>()));
				if (ran.empty())
					return json{ { "error", "provide any of x, y, z (units) and rx, rz (degrees)" } };
				return json{ { "ok", true }, { "ran", ran } };
			}

			if (action == "console") {
				const std::string command = a_args.value("command", std::string{});
				if (command.empty())
					return json{ { "error", "provide 'command'" } };
				if (!ConsoleCommandAllowed(command))
					return json{ { "error", "console command not allowed by the bridge allowlist" }, { "command", command } };
				RE::Console::ExecuteCommand(command.c_str());
				return json{ { "ok", true }, { "command", command } };
			}

			// A real fast travel to a map marker, by its map name (e.g. "Riften"): Game.FastTravel(marker), the same
			// call carriages make, so Sky Above flies it when its bScriptedFastTravel is on. Case-insensitive; an
			// exact name match wins over a prefix match. list=true returns the travel-able marker names instead.
			if (action == "fasttravel") {
				auto* player = RE::PlayerCharacter::GetSingleton();
				auto* world = player ? player->GetWorldspace() : nullptr;
				auto* cell = world ? world->persistentCell : nullptr;
				if (!cell)
					return json{ { "error", "player is not in a worldspace with a persistent cell" } };
				auto lower = [](std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
				const std::string want = lower(a_args.value("name", std::string{}));
				const bool listOnly = a_args.value("list", false);
				RE::TESObjectREFR* exact = nullptr;
				RE::TESObjectREFR* prefix = nullptr;
				json names = json::array();
				cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
					auto* marker = a_ref ? a_ref->extraList.GetByType<RE::ExtraMapMarker>() : nullptr;
					const char* full = marker && marker->mapData ? marker->mapData->locationName.GetFullName() : nullptr;
					if (!full || !*full)
						return RE::BSContainer::ForEachResult::kContinue;
					if (listOnly) {
						names.push_back(full);
						return RE::BSContainer::ForEachResult::kContinue;
					}
					const std::string n = lower(full);
					if (n == want) {
						exact = a_ref;
						return RE::BSContainer::ForEachResult::kStop;
					}
					if (!prefix && !want.empty() && n.compare(0, want.size(), want) == 0)
						prefix = a_ref;
					return RE::BSContainer::ForEachResult::kContinue;
				});
				if (listOnly)
					return json{ { "markers", names } };
				auto* dest = exact ? exact : prefix;
				if (!dest)
					return json{ { "error", "no map marker with that name in this worldspace" }, { "name", want } };
				auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
				if (!vm)
					return json{ { "error", "script VM unavailable" } };
				RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result;
				auto* fnArgs = RE::MakeFunctionArguments(std::move(dest));
				const bool queued = vm->DispatchStaticCall("Game", "FastTravel", fnArgs, result);
				const auto* marker = dest->extraList.GetByType<RE::ExtraMapMarker>();
				return json{ { "queued", queued }, { "marker", std::format("0x{:08X}", dest->GetFormID()) },
					{ "name", marker && marker->mapData ? marker->mapData->locationName.GetFullName() : "" },
					{ "x", dest->GetPositionX() }, { "y", dest->GetPositionY() } };
			}

			if (action == "load") {
				const std::string save = a_args.value("save", std::string{});
				if (save.empty())
					return json{ { "error", "provide 'save' (the save file name without extension)" } };
				auto* manager = RE::BGSSaveLoadManager::GetSingleton();
				if (!manager)
					return json{ { "error", "save/load manager unavailable" } };
				manager->Load(save.c_str());
				return json{ { "queued", true }, { "save", save }, { "note", "poll world state; loading takes a while" } };
			}

			if (action == "loadMostRecent") {
				auto* manager = RE::BGSSaveLoadManager::GetSingleton();
				if (!manager)
					return json{ { "error", "save/load manager unavailable" } };
				return json{ { "queued", manager->LoadMostRecentSaveGame() } };
			}

			if (action == "quit") {
				if (!a_args.value("confirm", false))
					return json{ { "error", "quit ends the game; pass confirm=true" } };
				RE::Console::ExecuteCommand("qqq");
				return json{ { "ok", true } };
			}

			return json{ { "error", "unknown action" }, { "action", action } };
		});
	}

	void WorldToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler("communityshaders.world", &BuildWorldResult, a_argsJson, a_sink, a_write);
	}

	// ---- Test bed: spawn / equip / move / remove objects and drive a free camera, for repeatable material and lighting shots ----
	std::vector<RE::ObjectRefHandle> g_testbedRefs;  // index = id; main thread only

	RE::TESForm* TestbedResolveForm(const json& a_value)
	{
		if (a_value.is_number_integer())
			return RE::TESForm::LookupByID(static_cast<RE::FormID>(a_value.get<std::uint64_t>()));
		if (a_value.is_string()) {
			const auto text = a_value.get<std::string>();
			try {
				std::size_t used = 0;
				const auto id = std::stoul(text, &used, 0);
				if (used == text.size())
					return RE::TESForm::LookupByID(static_cast<RE::FormID>(id));
			} catch (...) {
			}
			return RE::TESForm::LookupByEditorID(text);
		}
		return nullptr;
	}

	RE::TESObjectREFR* TestbedGetRef(const json& a_args)
	{
		if (!a_args.contains("id") || !a_args["id"].is_number_integer())
			return nullptr;
		const auto id = a_args["id"].get<std::int64_t>();
		if (id < 0 || static_cast<std::size_t>(id) >= g_testbedRefs.size())
			return nullptr;
		auto ref = g_testbedRefs[static_cast<std::size_t>(id)].get();
		return ref.get();
	}

	json TestbedRefInfo(std::size_t a_id, RE::TESObjectREFR* a_ref)
	{
		const auto pos = a_ref->GetPosition();
		json info{ { "id", a_id }, { "refFormID", std::format("0x{:08X}", a_ref->GetFormID()) }, { "x", pos.x }, { "y", pos.y }, { "z", pos.z },
			{ "rz", a_ref->GetAngleZ() * 57.29577951f }, { "loaded3D", a_ref->Is3DLoaded() } };
		if (auto* actor = a_ref->As<RE::Actor>()) {
			// Eye position, so a camera can frame the face whatever the actor's height or pose
			const auto eye = actor->GetLookingAtLocation();
			info["eye"] = json{ { "x", eye.x }, { "y", eye.y }, { "z", eye.z } };
		}
		return info;
	}

	void TestbedApplyTransform(RE::TESObjectREFR* a_ref, const json& a_args)
	{
		const auto pos = a_ref->GetPosition();
		const float x = a_args.contains("x") && a_args["x"].is_number() ? a_args["x"].get<float>() : pos.x;
		const float y = a_args.contains("y") && a_args["y"].is_number() ? a_args["y"].get<float>() : pos.y;
		const float z = a_args.contains("z") && a_args["z"].is_number() ? a_args["z"].get<float>() : pos.z;
		a_ref->SetPosition(x, y, z);
		if (a_args.contains("rz") && a_args["rz"].is_number()) {
			RE::NiPoint3 angle{ a_ref->GetAngleX(), a_ref->GetAngleY(), a_args["rz"].get<float>() * 0.01745329252f };
			a_ref->SetAngle(angle);
		}
	}

	json TestbedEquip(RE::TESObjectREFR* a_ref, const json& a_items)
	{
		auto* actor = a_ref->As<RE::Actor>();
		if (!actor)
			return json{ { "error", "reference is not an actor" } };
		json done = json::array();
		auto* equipManager = RE::ActorEquipManager::GetSingleton();
		for (const auto& item : a_items) {
			auto* form = TestbedResolveForm(item);
			auto* bound = form ? form->As<RE::TESBoundObject>() : nullptr;
			if (!bound) {
				done.push_back(json{ { "item", item }, { "error", "form not found or not an item" } });
				continue;
			}
			actor->AddObjectToContainer(bound, nullptr, 1, nullptr);
			if (equipManager)
				equipManager->EquipObject(actor, bound, nullptr, 1, nullptr, false, true, false, true);
			done.push_back(json{ { "item", item }, { "ok", true } });
		}
		return done;
	}

	// ---- view anchor: hold the player at a point while a detached camera frames a shot -----------
	// The engine draws distant land by where the PLAYER is, not the camera, and it lets a hovering player fall
	// as soon as the free camera state starts. A pinned player has no gravity and is put back on its point every
	// frame, so a view can be anchored anywhere, in the air included. The tick re-queues itself while the pin is on.
	struct PlayerPin
	{
		std::atomic<bool> on{ false };
		RE::NiPoint3 position{};
		float savedGravity = 1.0f;
		bool gravitySaved = false;
		std::atomic<bool> hide{ true };
		bool hidden = false;  // main thread only: we culled the third-person body and must show it again
	};
	PlayerPin gPlayerPin;
	std::atomic<bool> gPlayerPinQueued{ false };

	void PlayerPinTick()
	{
		if (!gPlayerPin.on.load(std::memory_order_acquire)) {
			gPlayerPinQueued.store(false, std::memory_order_release);
			return;
		}
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			if (auto* controller = player->GetCharController()) {
				if (!gPlayerPin.gravitySaved) {
					gPlayerPin.savedGravity = controller->gravity;
					gPlayerPin.gravitySaved = true;
				}
				controller->gravity = 0.0f;
				controller->SetLinearVelocityImpl(RE::hkVector4{});
				// No fall is building up while held, so putting the character down never hurts it.
				controller->fallStartHeight = gPlayerPin.position.z;
				controller->fallTime = 0.0f;
			}
			if (const float off = player->GetPosition().GetDistance(gPlayerPin.position); off > 1.0f) {
				// A long move must warp the Havok controller first: moved normally it sweeps from the old spot to
				// the new one, and a sweep over tens of thousands of units can overflow Havok's stack (the Sky Above
				// fast travel plugin hit this and does the same).
				if (off > 2000.0f)
					if (auto* controller = player->GetCharController()) {
						const float sc = RE::bhkWorld::GetWorldScale();
						controller->SetPositionImpl(RE::hkVector4(gPlayerPin.position.x * sc, gPlayerPin.position.y * sc, gPlayerPin.position.z * sc, 0.0f), false, true);
					}
				player->SetPosition(gPlayerPin.position, true);
			}
			// A pinned character is only there so the game draws the land; keep it out of the picture.
			if (gPlayerPin.hide.load(std::memory_order_acquire))
				if (auto* node = player->Get3D(false); node && !node->GetAppCulled()) {
					node->SetAppCulled(true);
					gPlayerPin.hidden = true;
				}
		}
		gPlayerPinQueued.store(false, std::memory_order_release);
	}

	// The tick must never queue itself: the game empties its task queue in one go, so a task that re-adds itself
	// keeps the queue full for ever and no frame is drawn again (2026-11-23: two hangs, closed by a watchdog).
	// A small thread posts one tick at a time instead, and only when the last one has run.
	void PlayerPinStart()
	{
		if (gPlayerPin.on.exchange(true))
			return;
		std::thread([] {
			while (gPlayerPin.on.load(std::memory_order_acquire)) {
				if (!gPlayerPinQueued.exchange(true))
					if (auto* task = SKSE::GetTaskInterface())
						task->AddTask(PlayerPinTick);
				std::this_thread::sleep_for(std::chrono::milliseconds(3));
			}
		}).detach();
	}

	void PlayerPinRelease()
	{
		gPlayerPin.on.store(false, std::memory_order_release);
		if (auto* player = RE::PlayerCharacter::GetSingleton(); player && gPlayerPin.hidden)
			if (auto* node = player->Get3D(false))
				node->SetAppCulled(false);
		gPlayerPin.hidden = false;
		if (auto* player = RE::PlayerCharacter::GetSingleton())
			if (auto* controller = player->GetCharController(); controller && gPlayerPin.gravitySaved) {
				controller->gravity = gPlayerPin.savedGravity;
				controller->fallStartHeight = player->GetPosition().z;
				controller->fallTime = 0.0f;
			}
		gPlayerPin.gravitySaved = false;
	}

	// View override: the method of the Sky Above fast travel plugin (GPL-3, same licence as this project). The game
	// stays in its normal camera state; after its own camera update the rendered camera is moved to the wanted pose.
	// No free-camera state, so nothing the game ties to that state changes (it dropped a held character, and it is the
	// suspected cause of far land going missing in view shots, 2026-11-23).
	struct ViewOverride
	{
		std::atomic<bool> on{ false };
		RE::NiPoint3 pos{};
		RE::NiPoint3 dir{ 0.0f, 1.0f, 0.0f };
		float fov = 0.0f;       // 0 = leave
		float savedFov = 0.0f;  // world FOV before the override, restored when it ends
		bool installed = false;
		bool failed = false;
	};
	ViewOverride gView;

	bool ViewBasis(const RE::NiPoint3& a_forward, RE::NiMatrix3& a_out)
	{
		const float len = a_forward.Length();
		if (len < 1e-4f)
			return false;
		const RE::NiPoint3 f = a_forward / len;
		RE::NiPoint3 r = f.Cross(RE::NiPoint3{ 0.0f, 0.0f, 1.0f });
		// Straight up or down has no level "right": use east.
		r = r.Length() < 0.02f ? RE::NiPoint3{ 1.0f, 0.0f, 0.0f } : r / r.Length();
		const RE::NiPoint3 u = r.Cross(f);
		const RE::NiPoint3 cols[3] = { f, r, u };  // column 0 = view direction, 1 = right, 2 = up
		for (int c = 0; c < 3; ++c) {
			a_out.entry[0][c] = cols[c].x;
			a_out.entry[1][c] = cols[c].y;
			a_out.entry[2][c] = cols[c].z;
		}
		return true;
	}

	void ViewOverrideApply(RE::PlayerCamera* a_cam)
	{
		if (!gView.on.load(std::memory_order_acquire) || !a_cam)
			return;
		auto* root = a_cam->cameraRoot.get();
		RE::NiCamera* lens = nullptr;
		if (root)
			for (auto& child : root->GetChildren())
				if (auto* cam = child ? netimmerse_cast<RE::NiCamera*>(child.get()) : nullptr) {
					lens = cam;
					break;
				}
		if (!root || !lens)
			return;
		const RE::NiPoint3 nativeDir{ lens->world.rotate.entry[0][0], lens->world.rotate.entry[1][0], lens->world.rotate.entry[2][0] };
		RE::NiMatrix3 from, to;
		if (!ViewBasis(nativeDir, from) || !ViewBasis(gView.dir, to))
			return;
		const RE::NiMatrix3 turn = to * from.Transpose();
		// The turned lens still carries whatever roll the game's camera has this frame (camera mods add handheld sway:
		// two takes of one view were 0.15 degrees apart in roll, 2026-11-24). Level it: forward stays the wanted
		// direction, up goes into the plane of forward and the world's up, right follows with its old sense.
		RE::NiMatrix3 lensRot = turn * lens->world.rotate;
		{
			auto col = [&](int c) { return RE::NiPoint3{ lensRot.entry[0][c], lensRot.entry[1][c], lensRot.entry[2][c] }; };
			auto setCol = [&](int c, const RE::NiPoint3& v) { lensRot.entry[0][c] = v.x; lensRot.entry[1][c] = v.y; lensRot.entry[2][c] = v.z; };
			RE::NiPoint3 f = gView.dir / gView.dir.Length();
			const int upCol = std::abs(col(1).z) >= std::abs(col(2).z) ? 1 : 2;
			const int rightCol = upCol == 1 ? 2 : 1;
			RE::NiPoint3 up = RE::NiPoint3{ 0.0f, 0.0f, 1.0f } - f * f.z;
			if (up.Length() > 0.02f) {
				up = up / up.Length();
				if (up.Dot(col(upCol)) < 0.0f)
					up = up * -1.0f;
				RE::NiPoint3 right = f.Cross(up);
				if (right.Dot(col(rightCol)) < 0.0f)
					right = right * -1.0f;
				setCol(0, f);
				setCol(upCol, up);
				setCol(rightCol, right);
			}
		}
		const RE::NiMatrix3 rotate = lensRot * lens->local.rotate.Transpose();
		const RE::NiPoint3 translate = gView.pos - rotate * lens->local.translate;
		if (root->parent) {
			const auto parent = root->parent->world.Invert();
			root->local.rotate = parent.rotate * rotate;
			root->local.translate = parent * translate;
		} else {
			root->local.rotate = rotate;
			root->local.translate = translate;
		}
		RE::NiUpdateData update{};
		root->Update(update);
		if (gView.fov > 1.0f)
			a_cam->GetRuntimeData2().worldFOV = gView.fov;
	}

	struct View_CameraUpdate
	{
		static void thunk(RE::PlayerCamera* a_cam)
		{
			func(a_cam);
			ViewOverrideApply(a_cam);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	bool ViewOverrideInstall()
	{
		if (gView.installed || gView.failed)
			return gView.installed;
		// The call inside PlayerCamera::Update that camera plugins wrap (Cinemacam, Sky Above); write_call chains.
		const std::uintptr_t site = REL::RelocationID(49852, 50784).address() + 0x1A6;
		if (*reinterpret_cast<const std::uint8_t*>(site) != 0xE8) {
			gView.failed = true;
			return false;
		}
		stl::write_thunk_call<View_CameraUpdate>(site);
		gView.installed = true;
		return true;
	}

	json BuildTestbedResult(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string{});
		if (action.empty())
			return json{ { "error", "missing required parameter 'action'" }, { "supported", json::array({ "spawn", "talk", "place", "equip", "remove", "clear", "list", "camera", "camera_get", "view_get", "pin", "unpin", "mipbias", "inspect_actor", "piece_cull", "debug_view" }) } };

		return RunOnMainThread([action, a_args]() -> json {
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player)
				return json{ { "error", "player unavailable" } };

			// Everything needed to reproduce the view on screen: the real camera pose whatever camera made it
			// (first or third person, free camera, a camera mod), and where the player stands.
			if (action == "view_get") {
				constexpr float kRadToDeg = 57.29577951f;
				json out = json::object();
				const auto pos = player->GetPosition();
				out["player"] = json{ { "x", pos.x }, { "y", pos.y }, { "z", pos.z }, { "yaw", player->GetAngleZ() * kRadToDeg }, { "pitch", player->GetAngleX() * kRadToDeg } };
				// Where the face is, for close-ups: the head bone and the way it faces (the bone's own +Y is forward).
				if (auto* body = player->Get3D(false))
					if (auto* head = body->GetObjectByName("NPC Head [Head]")) {
						const auto& hw = head->world;
						const RE::NiPoint3 hf = hw.rotate * RE::NiPoint3{ 0.0f, 1.0f, 0.0f };
						float hyaw = std::atan2(hf.x, hf.y) * kRadToDeg;
						if (hyaw < 0.0f)
							hyaw += 360.0f;
						out["head"] = json{ { "x", hw.translate.x }, { "y", hw.translate.y }, { "z", hw.translate.z }, { "yaw", hyaw }, { "fx", hf.x }, { "fy", hf.y }, { "fz", hf.z } };
					}
				if (auto* cell = player->GetParentCell()) {
					const char* editorID = cell->GetFormEditorID();
					out["cell"] = json{ { "editorID", editorID ? editorID : "" }, { "interior", cell->IsInteriorCell() }, { "attached", cell->IsAttached() } };
				}
				if (auto* worldspace = player->GetWorldspace()) {
					const char* editorID = worldspace->GetFormEditorID();
					out["worldspace"] = editorID ? editorID : "";
				}
				if (auto* camera = RE::PlayerCamera::GetSingleton()) {
					json cam = json::object();
					if (camera->currentState)
						cam["state"] = static_cast<int>(camera->currentState->id);
					cam["firstPerson"] = camera->IsInFirstPerson();
					cam["free"] = camera->IsInFreeCameraMode();
					cam["fov"] = camera->GetRuntimeData2().worldFOV;
					if (camera->cameraRoot) {
						const auto& world = camera->cameraRoot->world;
						cam["x"] = world.translate.x;
						cam["y"] = world.translate.y;
						cam["z"] = world.translate.z;
						// The camera looks along its local +Y. yaw: 0 north, 90 east. pitch: negative looks up (the testbed convention).
						const RE::NiPoint3 forward = world.rotate * RE::NiPoint3{ 0.0f, 1.0f, 0.0f };
						float yaw = std::atan2(forward.x, forward.y) * kRadToDeg;
						if (yaw < 0.0f)
							yaw += 360.0f;
						cam["yaw"] = yaw;
						cam["pitch"] = -std::asin(std::clamp(forward.z, -1.0f, 1.0f)) * kRadToDeg;
						// The lens itself: what is really drawn. roll 0 = level horizon; frustum = half-width and half-height at distance 1.
						for (auto& child : camera->cameraRoot->GetChildren())
							if (auto* lens = child ? netimmerse_cast<RE::NiCamera*>(child.get()) : nullptr) {
								const auto& lr = lens->world.rotate;
								const float z1 = lr.entry[2][1], z2 = lr.entry[2][2];
								cam["roll"] = std::atan2(std::abs(z1) >= std::abs(z2) ? z2 : z1, std::abs(z1) >= std::abs(z2) ? z1 : z2) * kRadToDeg;
								const auto& fr = lens->GetRuntimeData2().viewFrustum;
								cam["frustum"] = json{ { "right", fr.fRight }, { "top", fr.fTop } };
								cam["lens"] = json{ { "x", lens->world.translate.x }, { "y", lens->world.translate.y }, { "z", lens->world.translate.z } };
								break;
							}
					}
					out["camera"] = cam;
				}
				if (auto* ui = RE::UI::GetSingleton())
					out["loading"] = ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
				out["pinned"] = gPlayerPin.on.load();
				out["override"] = gView.on.load();
				return out;
			}

			// What an actor's body is drawn with, piece by piece (2026-11-25: Auri's body turns black on some loads):
			// textures by path and whether they are really loaded, shader type and flags, alpha, emissive.
			// actor: "player", a reference form ID ("0x5C3139C8"), or part of a loaded actor's name.
			if (action == "inspect_actor") {
				const std::string wanted = a_args.value("actor", std::string{ "player" });
				RE::Actor* actor = nullptr;
				if (wanted == "player")
					actor = player;
				else if (wanted.rfind("0x", 0) == 0) {
					if (auto* form = RE::TESForm::LookupByID(static_cast<RE::FormID>(std::stoul(wanted, nullptr, 16))))
						actor = form->As<RE::Actor>();
				} else if (auto* lists = RE::ProcessLists::GetSingleton()) {
					auto lower = [](std::string s) { std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); }); return s; };
					for (auto& handle : lists->highActorHandles)
						if (auto a = handle.get(); a && lower(a->GetDisplayFullName() ? a->GetDisplayFullName() : "").find(lower(wanted)) != std::string::npos) {
							actor = a.get();
							break;
						}
				}
				if (!actor)
					return json{ { "error", "actor not found (player, 0x reference ID, or part of a loaded actor's name)" } };
				auto texture = [](const RE::NiPointer<RE::NiSourceTexture>& t) -> json {
					if (!t)
						return nullptr;
					return json{ { "name", t->name.c_str() ? t->name.c_str() : "" }, { "loaded", t->rendererTexture != nullptr } };
				};
				json pieces = json::array();
				if (auto* root = actor->Get3D(false))
					RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* geometry) {
						const auto& rd = geometry->GetGeometryRuntimeData();
						json p{ { "name", geometry->name.c_str() ? geometry->name.c_str() : "" }, { "culled", geometry->GetAppCulled() } };
						if (auto* alpha = rd.alphaProperty.get())
							p["alpha"] = json{ { "flags", alpha->alphaFlags }, { "threshold", alpha->alphaThreshold } };
						if (auto* shader = rd.shaderProperty.get()) {
							p["shaderFlags"] = std::format("{:016X}", shader->flags.underlying());
							if (auto* lit = netimmerse_cast<RE::BSLightingShaderProperty*>(shader)) {
								if (lit->emissiveColor)
									p["emissive"] = json{ lit->emissiveColor->red, lit->emissiveColor->green, lit->emissiveColor->blue, lit->emissiveMult };
								if (auto* mat = static_cast<RE::BSLightingShaderMaterialBase*>(lit->material)) {
									p["feature"] = static_cast<int>(mat->GetFeature());
									// Body skin (FaceGen RGB tint): the skin colour multiplies the diffuse; all zero = black skin
									// (suspected for the player's hands / new coc characters' arms, 2026-11-25).
									if (mat->GetFeature() == RE::BSShaderMaterial::Feature::kFaceGenRGBTint) {
										const auto& tc = static_cast<RE::BSLightingShaderMaterialFacegenTint*>(mat)->tintColor;
										p["tintColor"] = json{ tc.red, tc.green, tc.blue };
									}
									p["materialAlpha"] = mat->materialAlpha;
									p["diffuse"] = texture(mat->diffuseTexture);
									p["normal"] = texture(mat->normalTexture);
									p["subsurfaceOrGlow"] = texture(mat->rimSoftLightingTexture);
									p["specular"] = texture(mat->specularBackLightingTexture);
									if (mat->textureSet) {
										json paths = json::array();
										for (int i = 0; i < 8; ++i) {
											const char* path = mat->textureSet->GetTexturePath(static_cast<RE::BSTextureSet::Texture>(i));
											paths.push_back(path ? path : "");
										}
										p["textureSet"] = paths;
									}
								}
							} else
								p["shader"] = shader->GetRTTI() ? shader->GetRTTI()->GetName() : "?";
						}
						pieces.push_back(std::move(p));
						return RE::BSVisit::BSVisitControl::kContinue;
					});
				const char* name = actor->GetDisplayFullName();
				return json{ { "actor", name ? name : "" }, { "formID", std::format("{:08X}", actor->GetFormID()) }, { "pieces", std::move(pieces) } };
			}

			// Deferred debug view (menu Advanced > Deferred Debug View), so a black-skin moment can be split into material
			// (Albedo) vs lighting (Diffuse Lighting) without the user opening the menu (2026-11-25). mode: 0 off .. 12.
			if (action == "debug_view") {
				auto* deferred = globals::deferred;
				if (!deferred)
					return json{ { "error", "deferred not available" } };
				const int mode = a_args.value("mode", 0);
				if (mode < 0 || mode >= static_cast<int>(Deferred::DebugView::Count))
					return json{ { "error", "mode out of range" }, { "count", static_cast<int>(Deferred::DebugView::Count) } };
				const bool wasOff = deferred->debugView == Deferred::DebugView::Off;
				deferred->debugView = static_cast<Deferred::DebugView>(mode);
				if (wasOff != (mode == 0))
					deferred->ClearShaderCache();  // the debug branch is a shader define, as in the menu
				return json{ { "action", "debug_view" }, { "mode", mode },
					{ "names", json::array({ "Off", "Albedo", "Reflectance", "Normal", "Roughness", "Mask", "Diffuse", "Specular", "SSGI", "SSR", "DynamicCubemaps", "DiffuseLighting", "SSGIAO" }) } };
			}

			// Hide or show an actor's 3D pieces by name (substring, case-insensitive), to tell overlapping meshes apart
			// (2026-11-25: two body meshes on a naked male, HIMBO's and TNG's, one renders black). actor as inspect_actor.
			if (action == "piece_cull") {
				const std::string wanted = a_args.value("actor", std::string{ "player" });
				const std::string part = a_args.value("name", std::string{});
				const bool culled = a_args.value("culled", true);
				RE::Actor* actor = wanted == "player" ? player : nullptr;
				if (!actor && wanted.rfind("0x", 0) == 0)
					if (auto* form = RE::TESForm::LookupByID(static_cast<RE::FormID>(std::stoul(wanted, nullptr, 16))))
						actor = form->As<RE::Actor>();
				if (!actor || part.empty())
					return json{ { "error", "give actor (player or 0x reference ID) and name (part of a piece name)" } };
				auto lower = [](std::string s) { std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); }); return s; };
				const std::string needle = lower(part);
				json changed = json::array();
				if (auto* root = actor->Get3D(false))
					RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* geometry) {
						const std::string pieceName = geometry->name.c_str() ? geometry->name.c_str() : "";
						if (lower(pieceName).find(needle) != std::string::npos) {
							geometry->SetAppCulled(culled);
							changed.push_back(pieceName);
						}
						return RE::BSVisit::BSVisitControl::kContinue;
					});
				return json{ { "ok", true }, { "culled", culled }, { "pieces", std::move(changed) } };
			}

			if (action == "pin") {
				if (!(a_args.contains("x") && a_args.contains("y") && a_args.contains("z")))
					return json{ { "error", "provide x, y, z (world units) to pin the player at" } };
				gPlayerPin.position = { a_args["x"].get<float>(), a_args["y"].get<float>(), a_args["z"].get<float>() };
				if (a_args.contains("yaw") && a_args["yaw"].is_number())
					player->SetAngle({ player->GetAngleX(), 0.0f, a_args["yaw"].get<float>() * 0.01745329252f });
				player->SetPosition(gPlayerPin.position, true);
				gPlayerPin.hide.store(a_args.value("hide", true));
				PlayerPinStart();
				return json{ { "ok", true }, { "pinned", true }, { "x", gPlayerPin.position.x }, { "y", gPlayerPin.position.y }, { "z", gPlayerPin.position.z } };
			}

			// Texture sharpness: offset added to the mip bias the upscaler sets (DLSS asks for log2(render/display) - 1).
			if (action == "mipbias") {
				if (a_args.contains("offset") && a_args["offset"].is_number())
					globals::state->mipBiasOffset.store(std::clamp(a_args["offset"].get<float>(), -2.0f, 3.0f));
				return json{ { "ok", true }, { "offset", globals::state->mipBiasOffset.load() } };
			}

			if (action == "unpin") {
				const bool was = gPlayerPin.on.load();
				// Put the player down first (where it stood before the shot), then give gravity back, so it never falls.
				if (a_args.contains("x") && a_args.contains("y") && a_args.contains("z")) {
					gPlayerPin.position = { a_args["x"].get<float>(), a_args["y"].get<float>(), a_args["z"].get<float>() };
					player->SetPosition(gPlayerPin.position, true);
				}
				PlayerPinRelease();
				return json{ { "ok", true }, { "pinned", false }, { "wasPinned", was } };
			}

			if (action == "talk") {
				// Starts dialogue with a spawned actor, as if the player had activated them
				auto* ref = TestbedGetRef(a_args);
				auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
				if (!actor)
					return json{ { "error", "unknown or expired 'id', or not an actor" } };
				const bool ok = actor->ActivateRef(player, 0, nullptr, 0, false);
				return json{ { "ok", ok } };
			}

			if (action == "spawn") {
				RE::TESForm* form = nullptr;
				if (a_args.contains("npc") && a_args["npc"].is_string()) {
					// By display name: the unique NPC of that name, else the first match (the spawned copy shares its face)
					const std::string wanted = a_args["npc"].get<std::string>();
					if (auto* data = RE::TESDataHandler::GetSingleton()) {
						for (auto* npc : data->GetFormArray<RE::TESNPC>()) {
							if (!npc || _stricmp(npc->GetName(), wanted.c_str()) != 0)
								continue;
							if (!form || npc->IsUnique()) {
								form = npc;
								if (npc->IsUnique())
									break;
							}
						}
					}
					if (!form)
						return json{ { "error", "no NPC with that name" }, { "npc", wanted } };
				} else {
					if (!a_args.contains("form"))
						return json{ { "error", "provide 'form' (form ID number, hex string or editor ID) or 'npc' (display name)" } };
					form = TestbedResolveForm(a_args["form"]);
				}
				auto* bound = form ? form->As<RE::TESBoundObject>() : nullptr;
				if (!bound)
					return json{ { "error", "form not found or not placeable" }, { "form", a_args.value("form", json()) } };
				auto ref = player->PlaceObjectAtMe(bound, false);
				if (!ref)
					return json{ { "error", "PlaceObjectAtMe failed" } };
				TestbedApplyTransform(ref.get(), a_args);
				if (auto* spawned = ref->As<RE::Actor>(); spawned && a_args.contains("ai") && a_args["ai"].is_boolean())
					spawned->EnableAI(a_args["ai"].get<bool>());  // ai=false: stands where placed (no packages, no walking home)
				g_testbedRefs.push_back(ref->GetHandle());
				json out = TestbedRefInfo(g_testbedRefs.size() - 1, ref.get());
				out["baseFormID"] = std::format("0x{:08X}", form->GetFormID());
				if (a_args.contains("equip") && a_args["equip"].is_array())
					out["equip"] = TestbedEquip(ref.get(), a_args["equip"]);
				return out;
			}

			if (action == "place" || action == "equip" || action == "remove") {
				auto* ref = TestbedGetRef(a_args);
				if (!ref)
					return json{ { "error", "unknown or expired 'id'" } };
				if (action == "place") {
					TestbedApplyTransform(ref, a_args);
					return TestbedRefInfo(static_cast<std::size_t>(a_args["id"].get<std::int64_t>()), ref);
				}
				if (action == "equip") {
					if (!a_args.contains("items") || !a_args["items"].is_array())
						return json{ { "error", "provide 'items' (array of forms)" } };
					return json{ { "ok", true }, { "items", TestbedEquip(ref, a_args["items"]) } };
				}
				ref->Disable();
				ref->SetDelete(true);
				g_testbedRefs[static_cast<std::size_t>(a_args["id"].get<std::int64_t>())] = {};
				return json{ { "ok", true } };
			}

			if (action == "clear") {
				std::size_t removed = 0;
				for (auto& handle : g_testbedRefs) {
					auto ref = handle.get();
					if (ref) {
						ref->Disable();
						ref->SetDelete(true);
						++removed;
					}
				}
				g_testbedRefs.clear();
				return json{ { "ok", true }, { "removed", removed } };
			}

			if (action == "list") {
				json arr = json::array();
				for (std::size_t i = 0; i < g_testbedRefs.size(); ++i) {
					auto ref = g_testbedRefs[i].get();
					if (ref)
						arr.push_back(TestbedRefInfo(i, ref.get()));
				}
				return json{ { "refs", arr } };
			}

			if (action == "camera" || action == "camera_get") {
				auto* camera = RE::PlayerCamera::GetSingleton();
				if (!camera)
					return json{ { "error", "player camera unavailable" } };
				const std::string mode = a_args.value("mode", std::string{ "free" });
				if (action == "camera" && mode != "override" && gView.on.exchange(false) && gView.savedFov > 1.0f)
					camera->GetRuntimeData2().worldFOV = gView.savedFov;
				if (action == "camera" && mode == "override") {
					// x y z pitch yaw fov as for the free camera: yaw 0 north, 90 east; positive pitch looks down.
					if (!ViewOverrideInstall())
						return json{ { "error", "camera update hook not installed: unexpected code at PlayerCamera::Update+0x1A6" } };
					auto* freeState = camera->GetRuntimeData().cameraStates[RE::CameraState::kFree].get();
					if (freeState && camera->currentState.get() == freeState)
						camera->ForceThirdPerson();
					for (const char* k : { "x", "y", "z", "pitch", "yaw" })
						if (!a_args.contains(k) || !a_args[k].is_number())
							return json{ { "error", "override needs x, y, z, pitch, yaw" } };
					const float pitch = a_args["pitch"].get<float>() * 0.01745329252f, yaw = a_args["yaw"].get<float>() * 0.01745329252f;
					gView.pos = { a_args["x"].get<float>(), a_args["y"].get<float>(), a_args["z"].get<float>() };
					gView.dir = { std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), -std::sin(pitch) };
					if (!gView.on.load())
						gView.savedFov = camera->GetRuntimeData2().worldFOV;
					gView.fov = a_args.contains("fov") && a_args["fov"].is_number() ? a_args["fov"].get<float>() : 0.0f;
					gView.on.store(true, std::memory_order_release);
					return json{ { "ok", true }, { "mode", "override" } };
				}
				if (action == "camera" && mode == "player") {
					camera->ForceThirdPerson();
					return json{ { "ok", true }, { "mode", "player-third" } };
				}
				if (action == "camera" && mode == "first") {
					camera->ForceFirstPerson();
					return json{ { "ok", true }, { "mode", "player-first" } };
				}
				auto* freeState = camera->GetRuntimeData().cameraStates[RE::CameraState::kFree].get();
				if (!freeState)
					return json{ { "error", "free camera state unavailable" } };
				if (camera->currentState.get() != freeState)
					camera->SetState(freeState);
				auto* free = static_cast<RE::FreeCameraState*>(freeState);
				if (action == "camera") {
					if (a_args.contains("x") && a_args["x"].is_number())
						free->translation.x = a_args["x"].get<float>();
					if (a_args.contains("y") && a_args["y"].is_number())
						free->translation.y = a_args["y"].get<float>();
					if (a_args.contains("z") && a_args["z"].is_number())
						free->translation.z = a_args["z"].get<float>();
					// rotation.x = pitch, rotation.y = yaw (radians); verify with camera_get against the player's own angles
					if (a_args.contains("pitch") && a_args["pitch"].is_number())
						free->rotation.x = a_args["pitch"].get<float>() * 0.01745329252f;
					if (a_args.contains("yaw") && a_args["yaw"].is_number())
						free->rotation.y = a_args["yaw"].get<float>() * 0.01745329252f;
					if (a_args.contains("fov") && a_args["fov"].is_number())
						camera->GetRuntimeData2().worldFOV = a_args["fov"].get<float>();
				}
				return json{ { "ok", true }, { "mode", "free" }, { "x", free->translation.x }, { "y", free->translation.y }, { "z", free->translation.z },
					{ "rotX", free->rotation.x }, { "rotY", free->rotation.y }, { "fov", camera->GetRuntimeData2().worldFOV } };
			}

			return json{ { "error", "unknown action" }, { "action", action } };
		});
	}

	void TestbedToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler("communityshaders.testbed", &BuildTestbedResult, a_argsJson, a_sink, a_write);
	}
}

namespace DevBenchBridge
{
	/**
	 * @brief Registers Community Shaders tools with the DevBench API if available.
	 *
	 * Registers tools for feature management, engine state inspection, shader cache control,
	 * frame capture, per-frame timing, settings persistence and dev flags, provided the DevBench interface is available.
	 */
	void Install()
	{
		auto* dvb = DevBenchAPI::GetDevBenchInterface001();
		if (!dvb) {
			logger::info("DevBenchBridge: devbench not present; CS tools not registered");
			return;
		}
		logger::info("DevBenchBridge: devbench build {} present — registering CS tools", dvb->GetBuildNumber());

		// Namespaced tool names — devbench's registry is shared across plugins, so bare
		// names ("feature", "inspect"…) could collide with devbench's own or another mod's.
		// Descriptors preserve the actions/kinds/inputSchema of CS's former embedded server
		// so existing MCP clients keep working under the new prefix.

		static constexpr const char* featureDesc =
			R"({"description":"All Community Shaders graphics-feature operations — enumerate, inspect settings, mutate settings, restore defaults, toggle on/off. Action-dispatched. list: returns an array of {name,shortName,loaded,version,category,isCore,inMenu}; features with restart-gated settings also include restartFields:[{key,label,pending}]. get: params shortName, returns the SaveSettings blob (null if the feature has no override; set/reset then no-op). set: params shortName, settings (object). reset: params shortName, calls RestoreDefaultSettings. toggle: params shortName, enabled (boolean, OPTIONAL — omit to flip the current loaded state); flips Feature::loaded.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["list","get","set","reset","toggle"]},"shortName":{"type":"string"},"settings":{"type":"object"},"enabled":{"type":"boolean"}}}})";
		dvb->RegisterTool("communityshaders.feature", featureDesc, &FeatureToolHandler, nullptr);

		static constexpr const char* inspectDesc =
			R"({"description":"Read non-feature Community Shaders engine state. Kind-dispatched; response is a JSON object. kind=state -> {plugin,frame_count}. kind=shadercache -> {compiling,standalonePending (on-demand compiles that compiling does not count),completedTasks,totalTasks,failedTasks,currentFailedCount,frame_count}. kind=profiler -> {frame:{ms,avgMs,p95Ms,p99Ms,samples,frameGeneration} (engine-frame wall clock; presents run ~2x when frameGeneration),totalGpuMs,totalCpuMs,frame_count,passes:[{name,gpuMs,gpuAvgMs,gpuP95Ms,gpuP99Ms,cpuMs,cpuAvgMs,gpuHistory:[...]}]}; optional filter param to match pass names.","readOnly":true,"inputSchema":{"type":"object","properties":{"kind":{"type":"string","enum":["state","shadercache","profiler","drawcalls"]},"filter":{"type":"string"}},"required":["kind"]}})";
		dvb->RegisterTool("communityshaders.inspect", inspectDesc, &InspectToolHandler, nullptr);

		static constexpr const char* shadercacheDesc =
			R"({"description":"Manage Community Shaders' compiled shader cache. Action-dispatched, fire-and-forget on the main thread. clear: drop the IN-MEMORY cache only; with the disk cache enabled shaders reload from Data/ShaderCache rather than recompiling, so this does NOT guarantee a recompile. deleteDisk: delete the on-disk cache AND drop the in-memory cache, forcing a full cold recompile (use this for compile benchmarks). Watch progress via communityshaders.inspect kind=shadercache and the communityshaders.shaderRecompiled event. Read-only status is communityshaders.inspect kind=shadercache.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["clear","deleteDisk"]}},"required":["action"]}})";
		dvb->RegisterTool("communityshaders.shadercache", shadercacheDesc, &ShadercacheToolHandler, nullptr);

		static constexpr const char* captureDesc =
			R"({"description":"Trigger a frame capture on the next render. Kind-dispatched. kind=renderdoc: RenderDoc multi-frame capture via the in-app API, honors frames (1-120, default 1); RenderDoc must be attached/loaded (check communityshaders.feature list for DeveloperTools.loaded). kind=screenshot: lossless screenshot via the Screenshot feature; frames is ignored. For screenshot, optional tag (filename-safe label added to the file name) and wait (default true: block until the file is written and return its path; timeoutMs 1000-60000, default 15000). RenderDoc captures are fire-and-forget.","inputSchema":{"type":"object","properties":{"kind":{"type":"string","enum":["renderdoc","screenshot"]},"frames":{"type":"number"},"tag":{"type":"string"},"wait":{"type":"boolean"},"timeoutMs":{"type":"number"}},"required":["kind"]}})";
		dvb->RegisterTool("communityshaders.capture", captureDesc, &CaptureToolHandler, nullptr);

		static constexpr const char* nvperfDesc =
			R"({"description":"NVIDIA Nsight Perf GPU hardware counters per render pass (D3D11; builds with NVPERF_SUPPORT, NVIDIA GPUs only). Ranges are the perf events Community Shaders emits (engine passes need Frame Annotations enabled at startup). Action-dispatched. status -> {built,available,gpuSupported,device,chip,deviceError,library,capture:{state,captureId,sample,samples,pass,passes,error}}. metrics -> {defaults:[{metric,label}],kinds}. capture: optional metrics (array of name[:counter|ratio|throughput|active|elapsed|percycle] strings, default the defaults), ranges (array of names, trailing * = prefix; default every outermost range), samples (1-64, default 1), delayFrames (default 0), nestingLevels (1-8, default 1; each level multiplies the passes) -> {queued,captureId}; runs over the next frames, one profiler pass per frame. cancel: stop the running capture. result: optional captureId (default latest), detail (boolean: include every range of every sample) -> {pending,capture} while running, else {captureId,success,error,chip,startFrame,endFrame,samples,passesPerSample,metrics:[{metric,label}],unsupportedMetrics,summary:[{range,perFrame,incomplete,values:{label:value},perSample:{label:[one value per sample]}}],csvPath}. Summary values are per frame: counters summed (gpu__time_duration in ns), rates weighted by range time. perSample: the same per sample (counters 0 in a sample without the range, rates null). Event communityshaders.nvperf.completed {captureId,success,error} fires when a capture ends.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["status","metrics","capture","cancel","result"]},"metrics":{"type":"array","items":{"type":"string"}},"ranges":{"type":"array","items":{"type":"string"}},"samples":{"type":"number"},"delayFrames":{"type":"number"},"nestingLevels":{"type":"number"},"captureId":{"type":"number"},"detail":{"type":"boolean"}},"required":["action"]}})";
		dvb->RegisterTool("communityshaders.nvperf", nvperfDesc, &NvPerfToolHandler, nullptr);
		NvPerf::SetCompletionCallback([](const NvPerf::CaptureResult& a_result) {
			if (auto* host = DevBenchAPI::GetDevBenchInterface001()) {
				const std::string payload = DumpForDevBench(json{ { "captureId", a_result.id }, { "success", a_result.success }, { "error", a_result.error } });
				host->EmitEvent("communityshaders.nvperf.completed", payload.c_str());
			}
		});

		static constexpr const char* framesDesc =
			R"({"description":"Per-frame timings for benchmarks: Present-to-Present frame time, CPU time of the Present thread and of the whole process, frame-generation flag. Idle until started. Action-dispatched. start -> {id} or {error} while a recording is pending or running; recording begins at the next Present and ends at stop or after 16384 frames (full). stop -> {id,recording,frames,full}; takes effect at the next Present, so poll status until recording is false. status -> {id,recording,frames,full}. read -> {id,recording,frames,full,tscHz,presentThreadId,columns:{frameMs,presentThreadCpuMs,processCpuMs,frameGeneration}}: equal-length arrays, one entry per frame, ms rounded to 4 decimals.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["start","stop","status","read"]}},"required":["action"]}})";
		dvb->RegisterTool("communityshaders.frames", framesDesc, &FramesToolHandler, nullptr);

		static constexpr const char* settingsDesc =
			R"({"description":"Save, load, or reset the GLOBAL Community Shaders user configuration (Data/SKSE/Plugins/CommunityShaders/*.json). Action-dispatched, all fire-and-forget on the main thread. save: persist current settings (State::Save). load: re-read settings from disk and apply (State::Load). reset: restore every feature to its defaults then persist. Use after communityshaders.feature set/reset to make changes durable, or to roll an A/B session back to the saved baseline.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["save","load","reset"]}},"required":["action"]}})";
		dvb->RegisterTool("communityshaders.settings", settingsDesc, &SettingsToolHandler, nullptr);

		static constexpr const char* devflagsDesc =
			R"({"description":"Named runtime booleans guarding temporary candidate code paths (DEV_FLAG(\"tmp::name\") in C++), for screening a change on and off in one launch. Flags read false until set and reset at every launch. Action-dispatched. set: params flags (object of name -> boolean; all validated before any is applied). list (default): every flag a call site has read or set. reset: every flag false. All return {flags:{name:bool}}.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["set","list","reset"]},"flags":{"type":"object","additionalProperties":{"type":"boolean"}}}}})";
		dvb->RegisterTool("communityshaders.devflags", devflagsDesc, &DevFlagsToolHandler, nullptr);
		static constexpr const char* worldDesc =
			R"({"description":"Put the game in a known state for look comparisons. Action-dispatched, runs on the main thread. state: gameHour, timeScale, weather (current/outgoing/percent/locked), player position and angles, cell, worldspace. list_weathers: weather records (optional filter on editor ID substring). weather: force a weather by full form ID ('id', e.g. 0x0010A236) or editor ID ('name'); instant defaults true; lock true/false pins or releases the weather. hour: set the game hour (0-24) with a sky transition refresh. timescale: set the timescale (0 freezes time). teleport: player.setpos / setangle via x,y,z (units) and rx,rz (degrees). console: an allowlisted console command (coc, cow, player.setpos, tfc, tgm, tcl, set timescale, set gamehour, fw, sw, ...). fasttravel: real fast travel (Game.FastTravel) to the map marker named name (list=true lists marker names). load: load a save by file name; loadMostRecent. quit: ends the game, requires confirm=true.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["state","list_weathers","weather","hour","timescale","freeze","refclock","teleport","console","fasttravel","load","loadMostRecent","quit"]},"id":{"type":"string"},"name":{"type":"string"},"instant":{"type":"boolean"},"lock":{"type":"boolean"},"value":{"type":"number"},"transition":{"type":"boolean"},"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"rx":{"type":"number"},"rz":{"type":"number"},"command":{"type":"string"},"save":{"type":"string"},"filter":{"type":"string"},"list":{"type":"boolean"},"confirm":{"type":"boolean"}},"required":["action"]}})";
		dvb->RegisterTool("communityshaders.world", worldDesc, &WorldToolHandler, nullptr);

		static constexpr const char* testbedDesc =
			R"({"description":"Test bed for repeatable material/lighting shots. Action-dispatched on the main thread. spawn: place any form at the player (form = form ID number, hex string like '0x000A2C8E' or editor ID) then move it with x,y,z (units) and rz (yaw degrees); optional equip = array of item forms for actors; returns an id. place: move/rotate a spawned object by id. equip: add and equip items on a spawned actor by id (items array). remove: delete one spawned object by id. clear: delete everything spawned. list: spawned objects. camera: mode free (default; free camera at x,y,z with pitch and yaw degrees and optional fov), player (third person) or first (first person). camera_get: current free-camera position/rotation in radians, to calibrate angles.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["spawn","place","equip","remove","clear","list","camera","camera_get"]},"form":{},"id":{"type":"number"},"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"rz":{"type":"number"},"equip":{"type":"array","items":{}},"items":{"type":"array","items":{}},"mode":{"type":"string"},"pitch":{"type":"number"},"yaw":{"type":"number"},"fov":{"type":"number"}},"required":["action"]}})";
		dvb->RegisterTool("communityshaders.testbed", testbedDesc, &TestbedToolHandler, nullptr);
	}

	std::optional<Request> GetLastRequest()
	{
		std::scoped_lock lock(g_lastRequestMutex);
		return g_lastRequest;
	}
}

#else

namespace DevBenchBridge
{
	/**
 * @brief Registers Community Shaders tools with the DevBench bridge if available.
 *
 * When the DevBench interface is present, registers tool handlers for feature management,
 * inspection, shader caching, capture operations, and settings persistence.
 * If DevBench is unavailable, no action is taken.
 */
	void Install() {}  // inert until built with DEVBENCH_BRIDGE_ENABLED

	std::optional<Request> GetLastRequest() { return std::nullopt; }
}

#endif
