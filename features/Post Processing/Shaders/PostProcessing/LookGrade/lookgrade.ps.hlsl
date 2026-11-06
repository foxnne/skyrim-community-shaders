// Look Grade: a display-referred grade that mirrors the Look Lab preview shader one to one, so a look tuned
// remotely in the lab renders the same in game. Input and output are linear (like the LUT stage that follows);
// the grade itself runs on sRGB-encoded values, exactly as the lab does.

#include "PostProcessing/fullscreen.hlsli"

Texture2D<float3> TexColor : register(t0);

cbuffer LookGradeCB : register(b1)
{
	float4 P0;  // exposure, warmth, tint, contrast
	float4 P1;  // contrast pivot, black point, white point, shadow lift
	float4 P2;  // midtones (gamma), highlights (gain), saturation, vibrance
	float4 P3;  // shadow hue, shadow amount, highlight hue, highlight amount
	float4 P4;  // shoulder strength, shoulder start
	float4 Band[8];  // hue shift, saturation, luminance per hue band
};

static const float CEN[8] = { 0.0, 0.0833333, 0.1666667, 0.3333333, 0.5, 0.6666667, 0.75, 0.8333333 };

float3 L2S(float3 c)
{
	c = max(c, 0.0);
	return lerp(c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055, step(0.0031308, c));
}

float3 S2L(float3 c)
{
	return lerp(c / 12.92, pow((c + 0.055) / 1.055, 2.4), step(0.04045, c));
}

float3 RGB2HSV(float3 c)
{
	float4 K = float4(0., -1. / 3., 2. / 3., -1.);
	float4 p = lerp(float4(c.bg, K.wz), float4(c.gb, K.xy), step(c.b, c.g));
	float4 q = lerp(float4(p.xyw, c.r), float4(c.r, p.yzx), step(p.x, c.r));
	float d = q.x - min(q.w, q.y);
	float e = 1e-10;
	return float3(abs(q.z + (q.w - q.y) / (6. * d + e)), d / (q.x + e), q.x);
}

float3 HSV2RGB(float3 c)
{
	float4 K = float4(1., 2. / 3., 1. / 3., 3.);
	float3 p = abs(frac(c.xxx + K.xyz) * 6. - K.www);
	return c.z * lerp(K.xxx, clamp(p - K.xxx, 0., 1.), c.y);
}

float3 HueRGB(float h)
{
	float3 n = float3(5, 3, 1);
	float3 k = fmod(n + h / 60.0, 6.0);
	return 1.0 - max(0.0, min(min(k, 4.0 - k), 1.0));
}

float4 main(FullscreenTriangleVSOutput input) : SV_Target
{
	uint2 tid = uint2(input.Position.xy);
	float3 lin = max(TexColor[tid].rgb, 0.0);

	const float ev = P0.x, temp = P0.y, tint = P0.z, contrast = P0.w;
	const float pivot = P1.x, bp = P1.y, wp = P1.z, lift = P1.w;
	const float gamma = P2.x, gain = P2.y, sat = P2.z, vib = P2.w;

	lin *= exp2(ev);
	lin *= float3(1.0 + 0.30 * temp, 1.0 - 0.20 * tint, 1.0 - 0.30 * temp);
	float3 v = L2S(lin);
	v = (v - pivot) * contrast + pivot;
	v = max((v - bp) / max(wp - bp, 1e-3), 0.0);
	v *= gain;
	v = v + lift * (1.0 - clamp(v, 0., 1.));
	v = pow(max(v, 0.0), 1.0 / gamma);

	// Hue-preserving highlight shoulder: compress only the brightest channel so bright saturated colours roll off
	// toward white without shifting hue.
	if (P4.x > 0.0) {
		float m = max(v.r, max(v.g, v.b));
		float st = P4.y;
		if (m > st) {
			float r = 1.0 - st;
			float m2 = st + r * tanh((m - st) / r);
			v *= lerp(1.0, m2 / m, P4.x);
		}
	}

	float l = dot(v, float3(0.2126, 0.7152, 0.0722));
	float3 sh = (HueRGB(P3.x) - 0.5) * 2.0 * P3.y;
	float3 hi = (HueRGB(P3.z) - 0.5) * 2.0 * P3.w;
	v += sh * (1.0 - smoothstep(0.0, 0.5, l)) + hi * smoothstep(0.5, 1.0, l);
	l = dot(v, float3(0.2126, 0.7152, 0.0722));
	float3 h0 = RGB2HSV(clamp(v, 0., 1.));
	v = lerp(float3(l, l, l), v, sat * (1.0 + vib * (1.0 - h0.y)));

	float3 hsv = RGB2HSV(clamp(v, 0., 1.));
	float hs = 0., sa = 0., la = 0.;
	float gate = smoothstep(0.03, 0.25, hsv.y);
	[unroll] for (int i = 0; i < 8; i++) {
		float d = abs(hsv.x - CEN[i]);
		d = min(d, 1.0 - d);
		float w = max(0.0, 1.0 - d / 0.11) * gate;
		hs += w * Band[i].x;
		sa += w * Band[i].y;
		la += w * Band[i].z;
	}
	hsv.x = frac(hsv.x + hs * 0.0833333);
	hsv.y = clamp(hsv.y * (1.0 + sa), 0., 1.);
	hsv.z = clamp(hsv.z * (1.0 + la * 0.6), 0., 1.);
	v = HSV2RGB(hsv);

	return float4(S2L(clamp(v, 0., 1.)), 1);
}
