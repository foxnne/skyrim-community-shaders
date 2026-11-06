#pragma once

#include "Buffer.h"
#include "PostProcessFeature.h"

/**
 * Display-referred grade that mirrors the Look Lab preview shader, so a look tuned remotely in the lab renders the
 * same in game. It runs just before the LUT stage and every value is a plain number, which lets the Scene Manager
 * blend it by time of day, weather, interior and location.
 */
struct LookGrade : public PostProcessFeature
{
	virtual inline std::string GetType() const override { return "Look Grade"; }
	virtual inline std::string GetDisplayName() const override { return T("feature.post_processing.look_grade.name", "Look Grade"); }
	virtual inline std::string GetDesc() const override { return T("feature.post_processing.look_grade.description", "Display-referred colour grade that matches the Look Lab preview. Applied before the LUT."); }
	virtual inline bool DrawAfterColorGrading() const override { return true; }
	virtual inline bool DisableInMainLoadingMenu() const override { return true; }

	struct Settings
	{
		float Exposure = 0.f;
		float Highlights = 1.f;
		float Midtones = 1.f;
		float ShadowsLift = 0.f;
		float Warmth = 0.f;
		float Tint = 0.f;
		float Saturation = 1.f;
		float Vibrance = 0.f;
		float Contrast = 1.f;
		float ContrastPivot = 0.45f;
		float BlackPoint = 0.f;
		float WhitePoint = 1.f;
		float ShadowHue = 220.f;
		float ShadowAmount = 0.f;
		float HighlightHue = 40.f;
		float HighlightAmount = 0.f;
		// Hue-preserving highlight roll-off applied to the brightest channel: 0 = off, 1 = full.
		float ShoulderStrength = 0.f;
		float ShoulderStart = 0.7f;
		// Per hue band: hue shift, saturation, luminance.
		float3 BandRed{ 0.f };
		float3 BandOrange{ 0.f };
		float3 BandYellow{ 0.f };
		float3 BandGreen{ 0.f };
		float3 BandAqua{ 0.f };
		float3 BandBlue{ 0.f };
		float3 BandPurple{ 0.f };
		float3 BandMagenta{ 0.f };
	} settings;

	struct alignas(16) LookGradeCB
	{
		float4 p0;
		float4 p1;
		float4 p2;
		float4 p3;
		float4 p4;
		float4 band[8];
	};
	eastl::unique_ptr<ConstantBuffer> lookCB = nullptr;

	eastl::unique_ptr<Texture2D> texOutput = nullptr;

	winrt::com_ptr<ID3D11PixelShader> lookPS = nullptr;

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	void CompileRasterShaders();

	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json&) override;
	virtual void SaveSettings(json&) override;

	virtual void DrawSettings() override;

	virtual void Draw(TextureInfo&) override;
};
