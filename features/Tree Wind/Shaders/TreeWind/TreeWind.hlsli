#ifndef __TREE_WIND_HLSLI__
#define __TREE_WIND_HLSLI__

// Wind sway for tree LOD and loaded trees. The Tree Wind feature binds the wind buffer for every Lighting
// and Utility draw, with Params.z set only while it draws a DynDOLOD tree LOD shape and Params.w only while
// it draws a shape of a loaded tree, and for every standard tree LOD (DistantTree) draw.
namespace TreeWind
{
	cbuffer TreeWindCB : register(b3)
	{
		float4 Wind;    // phase, previous phase, wind speed 0-1, wind angle (radians)
		float4 Params;  // amplitude, near distance, tree LOD draw, loaded tree draw
		float4 Shape;   // bend exponent, use bend weights, flutter strength, per-tree phase spread
		float4 Detail;  // flutter share, flutter speed, near scale, near ramp end
		float4 Gusts;   // gust strength, gust rarity, loaded tree scale, unused
	};

	// Tree LOD, per vertex: bend weight, tree random, tree scale, distance from the trunk.
	// Loaded trees: [0] tree origin xy and [1] tree origin z, tree height, both in the shape's own space;
	// then per vertex at [2 + vertex]: bend weight, distance from the trunk.
	Buffer<float4> VertexData : register(t120);

	bool IsEnabled()
	{
		return Params.z > 0.5;
	}

	// The draw is a shape of a loaded tree (never set together with IsEnabled)
	bool IsNearTree()
	{
		return Params.w > 0.5;
	}

	float2 Sway(float3 positionMS, float phase)
	{
		// Long-wavelength gusts travelling across the land, plus some sideways spread
		precise float p = dot(positionMS.xy, float2(0.00047, 0.00061)) + dot(positionMS.xy, float2(0.0013, -0.0011));
		precise float gust = sin(phase * 1.10 + p) + 0.35 * sin(phase * 2.70 + p * 1.7);
		precise float side = 0.30 * sin(phase * 1.90 + p * 1.3);
		precise float2 dir = float2(sin(Wind.w), cos(Wind.w));
		precise float2 perp = float2(-dir.y, dir.x);
		precise float strength = Params.x * (0.35 + 0.65 * saturate(Wind.z));
		precise float2 result = (dir * (0.5 + 0.5 * gust) + perp * side) * strength;
		return result;
	}

	// Gentler sway in the LOD band next to the loaded trees, full sway from the ramp end outwards
	float GetNearScale(float3x4 world, float3 positionMS)
	{
		precise float3 positionWS = mul(world, float4(positionMS, 1.0));
		precise float dist = length(positionWS);
		return lerp(Detail.z, 1.0, saturate((dist - Params.y) / max(Detail.w - Params.y, 1.0)));
	}

	float2 GetOffset(float3 positionMS, float phase, uint vertexID, float nearScale)
	{
		float4 data = VertexData.Load(vertexID);
		float bend = Shape.y > 0.5 ? pow(saturate(data.x), max(Shape.x, 0.05)) : 1.0;
		float scale = bend * data.z * nearScale;
		float2 offset = Sway(positionMS, phase + data.y * Shape.w) * scale;

		// A rare, short, strong gust per tree
		float gustPulse = pow(saturate(sin(phase * 0.23 + data.y * 37.0 + dot(positionMS.xy, float2(0.00031, 0.00027)))), max(Gusts.y, 1.0));
		offset *= 1.0 + Gusts.x * gustPulse;

		// Branch-tip flutter on a share of the trees, with its own fast timing per vertex
		float h = frac(vertexID * 0.61803399) * 6.2831853;
		float flutter = (frac(data.y * 7.31) < Detail.x) ? sin(phase * Detail.y + h) * Params.x * Shape.z * saturate(data.w) * scale : 0.0;
		return offset + float2(sin(h), cos(h)) * flutter;
	}

	// Standard tree LOD: one billboard per tree, so the instance gives the tree's foot and size directly.
	// a_heightMS is the vertex height above the foot and a_cameraDistance its distance from the camera.
	float2 GetBillboardOffset(float3 a_footWS, float a_heightMS, float a_instanceScale, float a_cameraDistance, float phase)
	{
		// precise throughout: the depth and colour shaders must compute the same offset to the bit
		// The quad's top vertices bend, its foot stays put
		precise float bend = saturate(a_heightMS * a_instanceScale / 64.0);
		precise float random = frac(sin(dot(floor(a_footWS.xy), float2(12.9898, 78.233))) * 43758.5453);
		// Same world-space sway as the DynDOLOD shapes: height-scaled, as at LOD4
		precise float scale = clamp(a_heightMS * a_instanceScale / 1600.0, 0.25, 1.25) * (0.8 + 0.4 * random) * 4.0;
		precise float nearScale = lerp(Detail.z, 1.0, saturate((a_cameraDistance - Params.y) / max(Detail.w - Params.y, 1.0)));
		precise float2 offset = Sway(a_footWS, phase + random * Shape.w) * scale * bend * nearScale;

		precise float gustPulse = pow(saturate(sin(phase * 0.23 + random * 37.0 + dot(a_footWS.xy, float2(0.00031, 0.00027)))), max(Gusts.y, 1.0));
		precise float2 result = offset * (1.0 + Gusts.x * gustPulse);
		return result;
	}

	// ---- Loaded trees ----------------------------------------------------------------------------------
	// One draw is one shape of one tree. The bend data gives the tree origin in the shape's space, so the
	// world matrix of the draw places the tree without any per-draw data from the CPU. precise throughout:
	// the colour, depth and shadow shaders must compute the same offset to the bit.

	struct NearTree
	{
		float3 originWS;     // tree origin, absolute world position
		float random;        // timing offset of the tree, 0-1
		float random2;       // size variation, gust timing and flutter choice, 0-1
		float scale;         // world units of sway per unit of Sway(): bend, tree height, near scale
		float flutter;       // world units of flutter
		float flutterPhase;  // varies smoothly across the crown, so neighbouring vertices move together
		float3 toModelX;     // world x and y axes in model space, for the offset
		float3 toModelY;
	};

	// Triangle wave 0-1 with period 1. Continuous, unlike a hash of the floored position, so the float
	// noise in a tree's world position cannot flip its timing from frame to frame.
	float Triangle(float x)
	{
		precise float result = abs(frac(x) * 2.0 - 1.0);
		return result;
	}

	// a_world: model to camera-relative world, a_posAdjust: the camera offset that makes it absolute
	NearTree GetNearTree(float3x4 a_world, float3 a_posAdjust, float3 a_positionMS, uint a_vertexID)
	{
		NearTree tree;

		precise float2 header0 = VertexData.Load(0).xy;
		precise float2 header1 = VertexData.Load(1).xy;
		precise float2 weights = VertexData.Load(a_vertexID + 2).xy;
		precise float3 originMS = float3(header0, header1.x);
		precise float3 originRel = mul(a_world, float4(originMS, 1.0));
		precise float3 originWS = originRel + a_posAdjust;
		tree.originWS = originWS;

		precise float3 row0 = a_world[0].xyz;
		precise float3 row1 = a_world[1].xyz;
		precise float3 row2 = a_world[2].xyz;
		precise float worldScale = length(float3(row0.x, row1.x, row2.x));
		precise float heightWS = header1.y * worldScale;

		precise float random = Triangle(dot(originWS.xy, float2(0.0077, 0.0113)) + 0.37 * Triangle(dot(originWS.xy, float2(-0.0091, 0.0053))));
		precise float random2 = Triangle(dot(originWS.xy, float2(0.0101, -0.0067)) + 0.5 * random);
		tree.random = random;
		tree.random2 = random2;

		// Same height scale as the tree LOD up to 2000 units. Taller trees have no LOD to match and would
		// barely move with that cap, so their sway keeps growing with the root of the height, up to 4.
		precise float heightScale = clamp(heightWS / 1600.0, 0.25, 1.25) * min(sqrt(max(heightWS / 2000.0, 1.0)), 3.2);
		// The LOD ramp from the near band outwards, for full models drawn far away
		precise float nearScale = lerp(Gusts.z, 1.0, saturate((length(originRel) - Params.y) / max(Detail.w - Params.y, 1.0)));
		precise float bend = pow(saturate(weights.x), max(Shape.x, 0.05));
		precise float scale = bend * heightScale * (0.8 + 0.4 * random2) * 4.0 * nearScale;
		tree.scale = scale;

		// Flutter moves a branch-sized part of the crown together, at half the strength of the per-vertex LOD flutter
		precise float3 fromOrigin = mul(float3x3(row0, row1, row2), a_positionMS - originMS);
		precise float flutterPhase = dot(fromOrigin, float3(13.0, 17.0, 23.0)) / max(heightWS, 1.0);
		precise float share = saturate((Detail.x - Triangle(random * 3.7 + random2 * 2.3)) * 10.0 + 0.5);
		precise float flutter = 0.5 * Params.x * Shape.z * saturate(weights.y) * scale * share;
		tree.flutterPhase = flutterPhase;
		tree.flutter = flutter;

		// Exact inverse of the world rotation and scale: columns of the adjugate over the determinant
		precise float3 inverse0 = cross(row1, row2);
		precise float3 inverse1 = cross(row2, row0);
		precise float determinant = dot(row0, inverse0);
		precise float inverseDeterminant = abs(determinant) > 1e-12 ? 1.0 / determinant : 0.0;
		precise float3 toModelX = inverse0 * inverseDeterminant;
		precise float3 toModelY = inverse1 * inverseDeterminant;
		tree.toModelX = toModelX;
		tree.toModelY = toModelY;
		return tree;
	}

	// Model-space offset of a loaded tree vertex at a wind phase (Wind.x now, Wind.y last frame)
	float3 GetNearOffset(NearTree a_tree, float a_phase)
	{
		precise float2 offset = Sway(a_tree.originWS, a_phase + a_tree.random * Shape.w) * a_tree.scale;

		// A rare, short, strong gust per tree
		precise float gustPulse = pow(saturate(sin(a_phase * 0.23 + a_tree.random2 * 6.2831853 + dot(a_tree.originWS.xy, float2(0.00031, 0.00027)))), max(Gusts.y, 1.0));
		precise float2 gusted = offset * (1.0 + Gusts.x * gustPulse);

		precise float direction = a_tree.flutterPhase * 0.41 + a_tree.random * 6.2831853;
		precise float2 total = gusted + float2(sin(direction), cos(direction)) * (sin(a_phase * Detail.y + a_tree.flutterPhase) * a_tree.flutter);

		// The sway is horizontal in the world; the model may be rotated, tilted and scaled
		precise float3 result = a_tree.toModelX * total.x + a_tree.toModelY * total.y;
		return result;
	}
}

#endif  // __TREE_WIND_HLSLI__
