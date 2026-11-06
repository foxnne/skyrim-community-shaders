#include "LookGrade.h"

#include "Features/PostProcessing.h"
#include "I18n/I18n.h"
#include "RasterPass.h"
#include "ShaderCache.h"
#include "State.h"
#include "Util.h"

#include <fstream>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	LookGrade::Settings,
	Exposure,
	Highlights,
	Midtones,
	ShadowsLift,
	Warmth,
	Tint,
	Saturation,
	Vibrance,
	Contrast,
	ContrastPivot,
	BlackPoint,
	WhitePoint,
	ShadowHue,
	ShadowAmount,
	HighlightHue,
	HighlightAmount,
	ShoulderStrength,
	ShoulderStart,
	BandRed,
	BandOrange,
	BandYellow,
	BandGreen,
	BandAqua,
	BandBlue,
	BandPurple,
	BandMagenta)

void LookGrade::DrawSettings()
{
	ImGui::TextWrapped("%s", T("feature.post_processing.look_grade.help", "Runs on display-encoded colour before the LUT, with the same maths as the Look Lab preview."));

	ImGui::SliderFloat(T("feature.post_processing.look_grade.exposure", "Exposure"), &settings.Exposure, -2.f, 2.f, "%.2f");
	ImGui::SliderFloat(T("feature.post_processing.look_grade.highlights", "Highlights"), &settings.Highlights, 0.5f, 1.5f, "%.2f");
	ImGui::SliderFloat(T("feature.post_processing.look_grade.midtones", "Midtones"), &settings.Midtones, 0.5f, 2.f, "%.2f");
	ImGui::SliderFloat(T("feature.post_processing.look_grade.shadows_lift", "Shadows Lift"), &settings.ShadowsLift, -0.15f, 0.3f, "%.3f");
	ImGui::SliderFloat(T("feature.post_processing.look_grade.warmth", "Warmth"), &settings.Warmth, -1.f, 1.f, "%.2f");
	ImGui::SliderFloat(T("feature.post_processing.look_grade.tint", "Tint"), &settings.Tint, -1.f, 1.f, "%.2f");
	ImGui::SliderFloat(T("feature.post_processing.look_grade.saturation", "Saturation"), &settings.Saturation, 0.f, 2.f, "%.2f");
	ImGui::SliderFloat(T("feature.post_processing.look_grade.vibrance", "Vibrance"), &settings.Vibrance, -1.f, 1.f, "%.2f");
	ImGui::SliderFloat(T("feature.post_processing.look_grade.contrast", "Contrast"), &settings.Contrast, 0.5f, 2.f, "%.2f");
	ImGui::SliderFloat(T("feature.post_processing.look_grade.contrast_pivot", "Contrast Pivot"), &settings.ContrastPivot, 0.15f, 0.7f, "%.2f");
	ImGui::SliderFloat(T("feature.post_processing.look_grade.black_point", "Black Point"), &settings.BlackPoint, 0.f, 0.2f, "%.3f");
	ImGui::SliderFloat(T("feature.post_processing.look_grade.white_point", "White Point"), &settings.WhitePoint, 0.7f, 1.2f, "%.3f");
	ImGui::SliderFloat(T("feature.post_processing.look_grade.shadow_hue", "Shadow Hue"), &settings.ShadowHue, 0.f, 360.f, "%.0f");
	ImGui::SliderFloat(T("feature.post_processing.look_grade.shadow_amount", "Shadow Amount"), &settings.ShadowAmount, 0.f, 0.15f, "%.3f");
	ImGui::SliderFloat(T("feature.post_processing.look_grade.highlight_hue", "Highlight Hue"), &settings.HighlightHue, 0.f, 360.f, "%.0f");
	ImGui::SliderFloat(T("feature.post_processing.look_grade.highlight_amount", "Highlight Amount"), &settings.HighlightAmount, 0.f, 0.15f, "%.3f");
	ImGui::SliderFloat(T("feature.post_processing.look_grade.shoulder_strength", "Shoulder Strength"), &settings.ShoulderStrength, 0.f, 1.f, "%.2f");
	ImGui::SliderFloat(T("feature.post_processing.look_grade.shoulder_start", "Shoulder Start"), &settings.ShoulderStart, 0.4f, 0.95f, "%.2f");

	ImGui::SeparatorText(T("feature.post_processing.look_grade.hue_bands", "Hue Bands (hue, saturation, luminance)"));
	ImGui::SliderFloat3(T("feature.post_processing.look_grade.band_red", "Red"), &settings.BandRed.x, -1.f, 1.f, "%.2f");
	ImGui::SliderFloat3(T("feature.post_processing.look_grade.band_orange", "Orange"), &settings.BandOrange.x, -1.f, 1.f, "%.2f");
	ImGui::SliderFloat3(T("feature.post_processing.look_grade.band_yellow", "Yellow"), &settings.BandYellow.x, -1.f, 1.f, "%.2f");
	ImGui::SliderFloat3(T("feature.post_processing.look_grade.band_green", "Green"), &settings.BandGreen.x, -1.f, 1.f, "%.2f");
	ImGui::SliderFloat3(T("feature.post_processing.look_grade.band_aqua", "Aqua"), &settings.BandAqua.x, -1.f, 1.f, "%.2f");
	ImGui::SliderFloat3(T("feature.post_processing.look_grade.band_blue", "Blue"), &settings.BandBlue.x, -1.f, 1.f, "%.2f");
	ImGui::SliderFloat3(T("feature.post_processing.look_grade.band_purple", "Purple"), &settings.BandPurple.x, -1.f, 1.f, "%.2f");
	ImGui::SliderFloat3(T("feature.post_processing.look_grade.band_magenta", "Magenta"), &settings.BandMagenta.x, -1.f, 1.f, "%.2f");
}

void LookGrade::RestoreDefaultSettings()
{
	settings = {};
}

void LookGrade::LoadSettings(json& o_json)
{
	settings = o_json;
}

void LookGrade::SaveSettings(json& o_json)
{
	o_json = settings;
}

void LookGrade::SetupResources()
{
	auto renderer = globals::game::renderer;

	lookCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<LookGradeCB>(), "LookGrade::Constants");

	{
		auto gameTexMainCopy = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY];

		D3D11_TEXTURE2D_DESC texDesc;
		gameTexMainCopy.texture->GetDesc(&texDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};

		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 1;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		texDesc.MiscFlags = 0;

		texOutput = eastl::make_unique<Texture2D>(texDesc, "LookGrade::Output");
		texOutput->CreateSRV(srvDesc);
		texOutput->CreateRTV(rtvDesc);
	}

	CompileRasterShaders();
}

void LookGrade::ClearShaderCache()
{
	BumpShaderGeneration();
	const auto shaderPtrs = std::array{
		&lookPS
	};

	{
		std::lock_guard lock(shaderMutex);
		for (auto shader : shaderPtrs)
			if ((*shader)) {
				(*shader)->Release();
				shader->detach();
			}
	}

	globals::shaderCache->InvalidateShaderSources();
	CompileRasterShaders();
}

void LookGrade::CompileRasterShaders()
{
	const std::vector<PixelShaderCompileInfo> shaderInfos = {
		{ &lookPS, "lookgrade.ps.hlsl" },
	};

	CompileRasterShadersAsync(L"Data\\Shaders\\PostProcessing\\LookGrade", {}, shaderInfos);
}

namespace
{
	/// Writes every weather in the load order with its flags and fog, so the Look Lab can tag weathers from real data.
	void DumpWeathers()
	{
		static bool done = false;
		if (done)
			return;
		done = true;
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh)
			return;
		json out = json::array();
		for (auto* w : dh->GetFormArray<RE::TESWeather>()) {
			if (!w)
				continue;
			const auto flags = static_cast<std::uint32_t>(w->data.flags.underlying());
			out.push_back({ { "spid", Util::FormIdToSpid(w->GetFormID()) },
				{ "editorId", w->GetFormEditorID() ? w->GetFormEditorID() : "" },
				{ "flags", flags },
				{ "pleasant", (flags & 1u) != 0 },
				{ "cloudy", (flags & 2u) != 0 },
				{ "rainy", (flags & 4u) != 0 },
				{ "snow", (flags & 8u) != 0 },
				{ "fogDayNear", w->fogData.dayNear },
				{ "fogDayFar", w->fogData.dayFar },
				{ "fogNightNear", w->fogData.nightNear },
				{ "fogNightFar", w->fogData.nightFar },
				{ "precipBegin", (int)w->data.precipitationBeginFadeIn },
				{ "windSpeed", (int)w->data.windSpeed } });
		}
		try {
			std::filesystem::create_directories("Data/SKSE/Plugins/CommunityShaders");
			std::ofstream("Data/SKSE/Plugins/CommunityShaders/LookWeathers.json") << out.dump(1);
			logger::info("[LookGrade] wrote {} weathers to LookWeathers.json", out.size());

			// Location-type keywords, so the Look Lab can target dungeons, caves, crypts and the like in the Scene Manager.
			json kws = json::array();
			for (auto* k : dh->GetFormArray<RE::BGSKeyword>()) {
				if (!k || !k->GetFormEditorID())
					continue;
				const std::string id = k->GetFormEditorID();
				if (id.rfind("LocType", 0) == 0)
					kws.push_back({ { "spid", Util::FormIdToSpid(k->GetFormID()) }, { "editorId", id }, { "name", k->GetName() } });
			}
			std::ofstream("Data/SKSE/Plugins/CommunityShaders/LookLocTypes.json") << kws.dump(1);
			logger::info("[LookGrade] wrote {} location types", kws.size());
		} catch (const std::exception& e) {
			logger::warn("[LookGrade] weather dump failed: {}", e.what());
		}
	}
}

void LookGrade::Draw(TextureInfo& inout_tex)
{
	DumpWeathers();
	if (!owner || !owner->GetFullscreenVS())
		return;
	if (!AllShadersReady({ &lookPS }))
		return;

	globals::profiler->BeginPass("PostProcessing::LookGrade");
	auto context = globals::d3d::context;

	float2 res = { (float)texOutput->desc.Width, (float)texOutput->desc.Height };
	res = Util::ConvertToDynamic(res);

	const auto& s = settings;
	const auto band = [](const float3& b) { return float4(b.x, b.y, b.z, 0.f); };
	LookGradeCB data = {
		.p0 = { s.Exposure, s.Warmth, s.Tint, s.Contrast },
		.p1 = { s.ContrastPivot, s.BlackPoint, s.WhitePoint, s.ShadowsLift },
		.p2 = { s.Midtones, s.Highlights, s.Saturation, s.Vibrance },
		.p3 = { s.ShadowHue, s.ShadowAmount, s.HighlightHue, s.HighlightAmount },
		.p4 = { s.ShoulderStrength, s.ShoulderStart, 0.f, 0.f },
		.band = { band(s.BandRed), band(s.BandOrange), band(s.BandYellow), band(s.BandGreen), band(s.BandAqua), band(s.BandBlue), band(s.BandPurple), band(s.BandMagenta) }
	};
	lookCB->Update(data);

	{
		PostProcessingRaster::RasterPass pass(context);

		ID3D11ShaderResourceView* srv = inout_tex.srv;
		ID3D11Buffer* cb = lookCB->CB();

		context->PSSetConstantBuffers(1, 1, &cb);
		context->PSSetShaderResources(0, 1, &srv);
		pass.SetTargets({ texOutput->rtv.get() }, res.x, res.y);
		pass.SetShaders(owner->GetFullscreenVS(), lookPS.get());
		pass.Draw();

		srv = nullptr;
		cb = nullptr;
		context->PSSetShaderResources(0, 1, &srv);
		context->PSSetConstantBuffers(1, 1, &cb);
	}

	inout_tex = { texOutput->resource.get(), texOutput->srv.get() };
	globals::profiler->EndPass();
}
