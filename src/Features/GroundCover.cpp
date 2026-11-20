#include "GroundCover.h"

#include "State.h"
#include "Util.h"
#include "Utils/FileSystem.h"
#include <DDSTextureLoader.h>
#include <pystring/pystring.h>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	GroundCover::Settings,
	Enabled,
	Strength,
	BandHeight,
	UseLevelMap,
	PlainLevel,
	PlainLevelOffset,
	RiseHeight,
	SlopeMin,
	SlopeMax,
	NoisePatchSize,
	NoiseDetailSize,
	NoiseAmount,
	Saturation,
	Brightness,
	LuminanceMatch,
	GlossReduction,
	CreaseDarkening,
	GreyLimit,
	WhiteLimit,
	RegionCoverage,
	MinObjectRadius,
	AffectObjectLOD,
	AffectTerrain,
	Scree,
	ScreeStrength,
	ScreeHeight,
	DebugView)

namespace
{
	/** The worldspace whose land the player stands on, or null in an interior. */
	RE::TESWorldSpace* LandWorldspace()
	{
		auto tes = globals::game::tes;
		if (!tes || tes->interiorCell)
			return nullptr;
		auto worldspace = tes->GetRuntimeData2().worldSpace;
		while (worldspace && worldspace->parentWorld && worldspace->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseLandData))
			worldspace = worldspace->parentWorld;
		return worldspace;
	}
}

void GroundCover::SetupResources()
{
	FindMaps();
	LoadRockModels();
}

void GroundCover::FindMaps()
{
	// Same naming as Ground Fog's water maps: <Worldspace>.<Kind>.<minX>.<minY>.<maxX>.<maxY>.dds, cells inclusive.
	colourMapFiles.clear();
	levelMapFiles.clear();
	std::error_code error;
	for (auto const& entry : std::filesystem::directory_iterator{ std::filesystem::path{ L"Data\\textures\\GrassColour\\GrassColourMaps\\" }, error }) {
		const auto path = entry.path();
		if (path.extension() != ".dds")
			continue;
		const auto fields = pystring::split(path.stem().string(), ".");
		if (fields.size() != 6 || (fields[1] != "GrassColourMap" && fields[1] != "GrassLevelMap")) {
			logger::debug("[Ground Cover] {} is not named like a grass colour or level map", path.filename().string());
			continue;
		}
		try {
			MapFile file;
			file.path = path;
			file.minX = std::stoi(fields[2]);
			file.minY = std::stoi(fields[3]);
			file.maxX = std::stoi(fields[4]);
			file.maxY = std::stoi(fields[5]);
			if (file.maxX < file.minX || file.maxY < file.minY)
				continue;
			(fields[1] == "GrassColourMap" ? colourMapFiles : levelMapFiles)[fields[0]] = file;
			logger::info("[Ground Cover] Map found: {}", path.filename().string());
		} catch (const std::exception& e) {
			logger::debug("[Ground Cover] Failed to parse {}: {}", path.filename().string(), e.what());
		}
	}
}

void GroundCover::LoadRockModels()
{
	rockModels.clear();
	rockBaseCache.clear();
	std::ifstream file(Util::PathHelpers::GetShadersPath() / "GroundCover" / "models.txt");
	std::string line;
	while (file && std::getline(file, line)) {
		if (const auto comment = line.find('#'); comment != std::string::npos)
			line.erase(comment);
		line = pystring::lower(pystring::strip(line));
		std::replace(line.begin(), line.end(), '/', '\\');
		if (!line.empty())
			rockModels.push_back(line);
	}
	if (rockModels.empty())
		rockModels = { "landscape\\mountains\\", "landscape\\rocks\\" };
	logger::info("[Ground Cover] {} rock model patterns", rockModels.size());
}

void GroundCover::EnsureMaps()
{
	const auto worldspace = LandWorldspace();
	const char* editorID = worldspace ? worldspace->GetFormEditorID() : nullptr;
	const std::string name = editorID ? editorID : "";
	if (name == mapWorldspace)
		return;
	// One attempt per worldspace: a map that fails to load is not tried again every frame.
	mapWorldspace = name;
	rockRadiusCache.clear();
	colourMap = nullptr;
	levelMap = nullptr;
	mapState = name.empty() ? "interior" : "no grass colour map for this worldspace";
	const auto found = colourMapFiles.find(name);
	if (found == colourMapFiles.end())
		return;

	if (FAILED(DirectX::CreateDDSTextureFromFile(globals::d3d::device, found->second.path.c_str(), nullptr, colourMap.put()))) {
		colourMap = nullptr;
		mapState = "grass colour map could not be loaded";
		logger::warn("[Ground Cover] {} could not be loaded; not used", found->second.path.filename().string());
		return;
	}
	colourMapCells = found->second;
	mapState = "grass colour map loaded, no level map (Plain Level is used)";

	const auto level = levelMapFiles.find(name);
	if (level == levelMapFiles.end())
		return;
	// The shader reads both maps with one uv.
	const auto& cells = level->second;
	if (cells.minX != colourMapCells.minX || cells.minY != colourMapCells.minY || cells.maxX != colourMapCells.maxX || cells.maxY != colourMapCells.maxY) {
		mapState = "grass colour map loaded; the level map covers other cells and is not used";
		logger::warn("[Ground Cover] {} does not cover the same cells as the colour map; not used", cells.path.filename().string());
		return;
	}
	if (FAILED(DirectX::CreateDDSTextureFromFile(globals::d3d::device, cells.path.c_str(), nullptr, levelMap.put()))) {
		levelMap = nullptr;
		mapState = "grass colour map loaded; the level map could not be loaded";
		logger::warn("[Ground Cover] {} could not be loaded; not used", cells.path.filename().string());
		return;
	}
	mapState = "grass colour map and level map loaded";
}

GroundCover::PerFrame GroundCover::GetCommonBufferData()
{
	PerFrame data{};
	active = false;
	if (!loaded || !settings.Enabled)
		return data;
	EnsureMaps();
	if (!colourMap)
		return data;
	active = true;

	const bool useLevelMap = settings.UseLevelMap && levelMap;
	data.Enabled = 1;
	data.Flags = (settings.AffectTerrain ? kTerrain : 0u) | (settings.AffectObjectLOD ? kObjectLOD : 0u) | (settings.Scree ? kScree : 0u) | (useLevelMap ? kLevelMap : 0u);
	data.Strength = settings.Strength;
	data.BandHeight = settings.BandHeight;
	data.SlopeMin = settings.SlopeMin;
	data.SlopeMax = settings.SlopeMax;
	data.InvPatchSize = 1.0f / std::max(settings.NoisePatchSize, 1.0f);
	data.InvDetailSize = 1.0f / std::max(settings.NoiseDetailSize, 1.0f);
	data.NoiseAmount = settings.NoiseAmount;
	data.Saturation = settings.Saturation;
	data.Brightness = settings.Brightness;
	data.LuminanceMatch = settings.LuminanceMatch;
	data.GlossReduction = settings.GlossReduction;
	data.CreaseDarkening = settings.CreaseDarkening;
	data.GreyLimit = settings.GreyLimit;
	data.WhiteLimit = settings.WhiteLimit;
	data.RiseHeight = settings.RiseHeight;
	data.RegionCoverage = settings.RegionCoverage;
	// The shader adds the level map's altitude to this when kLevelMap is set.
	data.PlainLevel = settings.PlainLevelOffset + (useLevelMap ? 0.0f : settings.PlainLevel);
	data.ScreeStrength = settings.ScreeStrength;
	data.ScreeHeight = settings.ScreeHeight;
	data.DebugView = settings.DebugView;

	// uv of a world position: u from the west edge of cell minX, v from the north edge of cell maxY (row 0 north).
	const float cellsX = static_cast<float>(colourMapCells.maxX - colourMapCells.minX + 1);
	const float cellsY = static_cast<float>(colourMapCells.maxY - colourMapCells.minY + 1);
	data.MapScale = { 1.0f / (cellsX * 4096.0f), -1.0f / (cellsY * 4096.0f) };
	data.MapOffset = { -static_cast<float>(colourMapCells.minX) / cellsX, static_cast<float>(colourMapCells.maxY + 1) / cellsY };
	return data;
}

void GroundCover::Prepass()
{
	if (!active)
		return;
	ID3D11ShaderResourceView* srvs[2] = { colourMap.get(), levelMap.get() };
	globals::d3d::context->PSSetShaderResources(kColourMapSlot, 2, srvs);
}

bool GroundCover::IsRock(RE::TESObjectREFR* a_ref)
{
	if (!a_ref)
		return false;
	auto base = a_ref->GetObjectReference();
	if (!base)
		return false;
	const auto id = base->GetFormID();
	if (const auto cached = rockBaseCache.find(id); cached != rockBaseCache.end())
		return cached->second;

	bool rock = false;
	if (base->GetFormType() == RE::FormType::Static) {
		if (auto model = base->As<RE::TESModel>(); model && model->GetModel()) {
			std::string path = model->GetModel();
			std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return static_cast<char>(c == '/' ? '\\' : std::tolower(c)); });
			rock = std::ranges::any_of(rockModels, [&](const std::string& a_part) { return path.find(a_part) != std::string::npos; });
		}
	}
	if (rockBaseCache.size() > 16384)
		rockBaseCache.clear();
	rockBaseCache.emplace(id, rock);
	return rock;
}

float GroundCover::GetRadius(RE::TESObjectREFR* a_ref)
{
	const auto id = a_ref->GetFormID();
	if (const auto cached = rockRadiusCache.find(id); cached != rockRadiusCache.end())
		return cached->second;
	const auto* root = a_ref->Get3D();
	if (!root)
		return 0.0f;  // not cached: it is asked again once the 3D is there
	const float radius = root->worldBound.radius;
	if (rockRadiusCache.size() > 16384)
		rockRadiusCache.clear();
	rockRadiusCache.emplace(id, radius);
	return radius;
}

void GroundCover::SetupGeometry(RE::BSRenderPass* a_pass)
{
	using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
	auto& descriptor = globals::state->permutationData.ExtraShaderDescriptor;
	const auto* property = a_pass->shaderProperty;
	// Decals on a rock belong to the rock's reference but are not rock. Small rock (boulders and piles on the
	// plain) is left alone: the cover belongs on the mountains' feet.
	auto* ref = a_pass->geometry ? a_pass->geometry->GetUserData() : nullptr;
	if (ref && !(property && property->flags.any(Flag::kDecal, Flag::kDynamicDecal)) && IsRock(ref) &&
		(settings.MinObjectRadius <= 0.0f || GetRadius(ref) >= settings.MinObjectRadius))
		descriptor |= static_cast<uint>(State::ExtraShaderDescriptors::GroundCover);
	else
		descriptor &= ~static_cast<uint>(State::ExtraShaderDescriptors::GroundCover);
}

void GroundCover::Hooks::BSLightingShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	if (globals::features::groundCover.active)
		globals::features::groundCover.SetupGeometry(Pass);
	func(This, Pass, RenderFlags);
}

void GroundCover::DrawSettings()
{
	ImGui::Checkbox("Enabled", &settings.Enabled);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Runs the colour of the grass on the plain up the lower slopes of rock next to it.\n"
			"Proof of concept. Needs a grass colour map for the worldspace.");
	}
	ImGui::Text("Maps: %s", mapState.c_str());
	if (!settings.Enabled)
		return;

	ImGui::SliderFloat("Strength", &settings.Strength, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("How much of the rock's colour is replaced where the cover is full.");
	}

	if (ImGui::TreeNodeEx("Height", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Band Height", &settings.BandHeight, 250.0f, 20000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Height above the plain over which the cover thins to nothing. Full at the plain's own level.");
		}
		ImGui::Checkbox("Use Level Map", &settings.UseLevelMap);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Takes the plain's altitude from the level map: the altitude grass grows at nearby.\n"
				"Off, or without a level map, Plain Level is used for the whole worldspace.");
		}
		ImGui::SliderFloat("Plain Level", &settings.PlainLevel, -20000.0f, 20000.0f, "%.0f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("World height of the plain when no level map is used. The Whiterun plain lies near -5000.");
		}
		ImGui::SliderFloat("Plain Level Offset", &settings.PlainLevelOffset, -10000.0f, 10000.0f, "%.0f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Raises or lowers the plain level, from the map or from Plain Level.");
		}
		ImGui::SliderFloat("Rise Height", &settings.RiseHeight, 0.0f, 3000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("No cover at or below the plain level; it fades in over this height above it. Keeps it off rocks lying on the plain. 0 = no fade.");
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Slope and Patches", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Slope Start", &settings.SlopeMin, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("How far a surface must face up before any cover holds on it. 0 = a wall, 1 = flat.");
		}
		ImGui::SliderFloat("Slope Full", &settings.SlopeMax, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("How far a surface must face up to hold full cover.");
		}
		ImGui::SliderFloat("Patch Size", &settings.NoisePatchSize, 100.0f, 10000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat("Edge Detail Size", &settings.NoiseDetailSize, 20.0f, 2000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat("Patchiness", &settings.NoiseAmount, 0.0f, 0.95f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("0: an even gradient. Higher: broken into patches with ragged edges.");
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Colour", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Saturation", &settings.Saturation, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat("Brightness", &settings.Brightness, 0.1f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat("Luminance Match", &settings.LuminanceMatch, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("0: the rock keeps its own light and dark and only takes the hue. 1: it takes the grass colour as it is.");
		}
		ImGui::SliderFloat("Gloss Reduction", &settings.GlossReduction, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat("Crease Darkening", &settings.CreaseDarkening, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Darkens the cover where little sky is visible. Needs Skylighting.");
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Surfaces", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Text("Placed rock: %u model patterns (Shaders\\GroundCover\\models.txt)", static_cast<uint32_t>(rockModels.size()));
		ImGui::SameLine();
		if (ImGui::Button("Reload"))
			LoadRockModels();
		ImGui::Checkbox("Object LOD", &settings.AffectObjectLOD);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("All object LOD that is grey and faces up. LOD cannot be told apart by model, so grey roofs and walls are included.");
		}
		ImGui::Checkbox("Terrain", &settings.AffectTerrain);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Sloping terrain and terrain LOD without grass of its own. Flat ground is left alone.");
		}
		ImGui::SliderFloat("Grey Limit", &settings.GreyLimit, 0.05f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("How colourful a surface may be and still count as rock. 1 = no test.");
		}
		ImGui::SliderFloat("White Limit", &settings.WhiteLimit, 0.05f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("How bright a surface may be and still count as rock. Keeps the cover off snow painted on a mesh. 1 = no test.");
		}
		ImGui::SliderFloat("Min Rock Size", &settings.MinObjectRadius, 0.0f, 8000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Placed rock smaller than this (bound radius, game units) is left alone: boulders and piles. 0 = all rock in the model list.");
		}
		ImGui::SliderFloat("Region Grass", &settings.RegionCoverage, 0.0f, 0.5f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Share of the surrounding cell that must grow grass. Keeps the cover out of regions without any.");
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Scree")) {
		ImGui::Checkbox("Scree Band", &settings.Scree);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("A lighter band of loose rock above the cover.");
		}
		ImGui::SliderFloat("Scree Strength", &settings.ScreeStrength, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat("Scree Height", &settings.ScreeHeight, 100.0f, 20000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::TreePop();
	}

	static constexpr const char* debugViews[] = { "Off", "Cover (magenta)", "Why not (first failing term)", "Slope term", "Height over plain",
		"Rise x band term", "Noise", "Snow term (1 - snow)", "Grey test", "White test", "Region test", "Surfaces it runs on" };
	int debugView = static_cast<int>(std::min<uint>(settings.DebugView, std::size(debugViews) - 1));
	if (ImGui::Combo("Debug View", &debugView, debugViews, static_cast<int>(std::size(debugViews))))
		settings.DebugView = static_cast<uint>(debugView);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Why not: red too steep, white snow, yellow too colourful, cyan too bright, orange no grass in the area,\n"
			"blue at or below the plain, purple above the band, green passes (brighter = more cover).\n"
			"Surfaces the layer does not run on are dark grey in every view from Why not on.\n"
			"Height: blue below the plain, green to red up the band, dark red above it. Single terms: black 0, white 1.");
	}
}

void GroundCover::LoadSettings(json& o_json)
{
	settings = o_json;
	const Settings defaults{};
	auto sanitize = [](float& a_value, float a_default, float a_min, float a_max) {
		a_value = std::clamp(std::isfinite(a_value) ? a_value : a_default, a_min, a_max);
	};
	sanitize(settings.Strength, defaults.Strength, 0.0f, 1.0f);
	sanitize(settings.BandHeight, defaults.BandHeight, 1.0f, 100000.0f);
	sanitize(settings.PlainLevel, defaults.PlainLevel, -100000.0f, 100000.0f);
	sanitize(settings.PlainLevelOffset, defaults.PlainLevelOffset, -100000.0f, 100000.0f);
	sanitize(settings.SlopeMin, defaults.SlopeMin, 0.0f, 1.0f);
	sanitize(settings.SlopeMax, defaults.SlopeMax, 0.0f, 1.0f);
	sanitize(settings.NoisePatchSize, defaults.NoisePatchSize, 1.0f, 100000.0f);
	sanitize(settings.NoiseDetailSize, defaults.NoiseDetailSize, 1.0f, 100000.0f);
	sanitize(settings.NoiseAmount, defaults.NoiseAmount, 0.0f, 0.95f);
	sanitize(settings.Saturation, defaults.Saturation, 0.0f, 4.0f);
	sanitize(settings.Brightness, defaults.Brightness, 0.0f, 8.0f);
	sanitize(settings.LuminanceMatch, defaults.LuminanceMatch, 0.0f, 1.0f);
	sanitize(settings.GlossReduction, defaults.GlossReduction, 0.0f, 1.0f);
	sanitize(settings.CreaseDarkening, defaults.CreaseDarkening, 0.0f, 1.0f);
	sanitize(settings.GreyLimit, defaults.GreyLimit, 0.01f, 1.0f);
	sanitize(settings.WhiteLimit, defaults.WhiteLimit, 0.05f, 1.0f);
	sanitize(settings.RiseHeight, defaults.RiseHeight, 0.0f, 100000.0f);
	sanitize(settings.MinObjectRadius, defaults.MinObjectRadius, 0.0f, 100000.0f);
	settings.DebugView = std::min(settings.DebugView, 11u);
	sanitize(settings.RegionCoverage, defaults.RegionCoverage, 0.0f, 1.0f);
	sanitize(settings.ScreeStrength, defaults.ScreeStrength, 0.0f, 1.0f);
	sanitize(settings.ScreeHeight, defaults.ScreeHeight, 1.0f, 100000.0f);
}

void GroundCover::SaveSettings(json& o_json)
{
	o_json = settings;
}

void GroundCover::RestoreDefaultSettings()
{
	settings = {};
}
