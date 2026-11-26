#include "TreeShapes.h"

#include <cfloat>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <numeric>

#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/FileSystem.h"

namespace TreeShapes
{
	namespace
	{
		// A tall LOD pine in world units sways at scale 1
		constexpr float kReferenceTreeHeight = 1600.0f;
		// Crown vertices sit within this many model units (at LOD4 scale) of their trunk
		constexpr float kCrownRadius = 100.0f;
		constexpr float kCellSize = 128.0f;
		// Bend data of a loaded tree shape not drawn for this many frames is freed
		constexpr uint kEvictFrames = 600;
		// The same for tree LOD shapes. A shape is not drawn while it is outside the view or hidden behind terrain,
		// and data freed then has to be built again when it comes back: the crowns show their plain shading until
		// their turn. So it is kept for about a minute and a half of looking elsewhere (at 60 frames per second),
		// at 12 bytes per vertex of the shapes that left in that time.
		constexpr uint kLodEvictFrames = 5400;
		// Crown data of a tree LOD shape that was drawn without it fades in over this many frames
		constexpr uint kCrownFadeFrames = 45;
		// Render-thread time spent building bend data per frame; a build that has started always finishes
		constexpr int64_t kBuildBudgetNs = 1500000;
		// A tree-mod crown waits this many frames after its block last gained trunk cards, so it is built once
		// with all of them instead of once per trunk shape
		constexpr uint kSettleFrames = 20;
		// Render-thread time spent measuring loaded trees per frame; a tree that has started always finishes
		constexpr int64_t kNearBuildBudgetNs = 1500000;
		// The tree root is at most this many nodes above its shapes
		constexpr int kNearTreeDepth = 8;
		// Data drawn within this many frames is never freed for the memory budget: it would be rebuilt next frame
		constexpr uint kBudgetGraceFrames = 120;
		// Data drawn within this many frames is resident whatever the position test says: the LOD sweep runs once a
		// second, so a block drawn since the last sweep is never freed on the strength of a distance alone
		constexpr uint kResidentFrames = 60;
		// Most loaded tree shapes with bend data at once: past it (or the memory budget) new trees are not measured
		constexpr size_t kMaxNearShapes = 4096;
		// Side of an object LOD block in world units per unit of its scale: a LOD4 block (scale 4) is 4 x 4 cells
		constexpr float kBlockSideAtScale1 = 4096.0f;

		bool IsTreeShapeName(const RE::BSGeometry* a_geometry)
		{
			// DynDOLOD names the tree LOD shape of an object LOD block 'treepassthru', whatever billboard type made it
			const char* name = a_geometry->name.c_str();
			return name && std::strcmp(name, "treepassthru") == 0;
		}

		bool IsAllQuads(RE::BSGeometry* a_geometry)
		{
			auto triShape = a_geometry->AsTriShape();
			if (!triShape)
				return false;
			const auto& data = triShape->GetTrishapeRuntimeData();
			return data.triangleCount > 0 && data.vertexCount == 2u * data.triangleCount;
		}

		// Sway scale from the tree height: the offset is in model units, so LOD8 and LOD16 shapes are scaled back to LOD4
		float TreeScale(float a_heightModel, float a_worldScale, float a_random)
		{
			const float worldScale = std::max(a_worldScale, 0.01f);
			return std::clamp(a_heightModel * worldScale / kReferenceTreeHeight, 0.25f, 1.25f) * (0.8f + 0.4f * a_random) * (4.0f / worldScale);
		}

		float Hash01(float a_x, float a_y)
		{
			uint32_t h = static_cast<uint32_t>(static_cast<int32_t>(std::floor(a_x))) * 73856093u ^
			             static_cast<uint32_t>(static_cast<int32_t>(std::floor(a_y))) * 19349663u;
			h ^= h >> 13;
			h *= 0x5bd1e995u;
			h ^= h >> 15;
			return static_cast<float>(h & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
		}

		uint64_t BlockKey(const RE::BSGeometry* a_geometry)
		{
			const auto x = static_cast<int32_t>(a_geometry->world.translate.x);
			const auto y = static_cast<int32_t>(a_geometry->world.translate.y);
			return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(y);
		}

		int64_t CellKey(int a_cx, int a_cy)
		{
			return (static_cast<int64_t>(a_cx) << 32) ^ static_cast<uint32_t>(a_cy);
		}

		int CellOf(float a_value)
		{
			return static_cast<int>(std::floor(a_value / kCellSize));
		}

		struct VertexView
		{
			const uint8_t* raw = nullptr;
			uint32_t stride = 0;
			uint32_t count = 0;
			const void* vertexBuffer = nullptr;

			RE::NiPoint3 operator()(uint32_t a_index) const
			{
				const auto* f = reinterpret_cast<const float*>(raw + static_cast<size_t>(a_index) * stride);
				return { f[0], f[1], f[2] };
			}

			// FNV-1a 64 over the position bytes of every vertex: the identity a bespoke wind mask is matched by
			uint64_t PositionHash() const
			{
				uint64_t h = 0xcbf29ce484222325ull;
				for (uint32_t i = 0; i < count; ++i) {
					const uint8_t* p = raw + static_cast<size_t>(i) * stride;
					for (int k = 0; k < 12; ++k) {
						h ^= p[k];
						h *= 0x100000001b3ull;
					}
				}
				return h;
			}
		};

		bool GetVertices(RE::BSGeometry* a_geometry, VertexView& a_out)
		{
			auto rendererData = a_geometry->GetGeometryRuntimeData().rendererData;
			auto triShape = a_geometry->AsTriShape();
			if (!rendererData || !rendererData->rawVertexData || !rendererData->vertexBuffer || !triShape)
				return false;

			// Positions must be full-precision float4 at offset 0
			auto& desc = rendererData->vertexDesc;
			const uint32_t stride = desc.GetSize();
			if (stride < 16 || desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_TEXCOORD0) != 16)
				return false;

			a_out = { rendererData->rawVertexData, stride, triShape->GetTrishapeRuntimeData().vertexCount, rendererData->vertexBuffer };
			return a_out.count > 0;
		}

		// An immutable raw buffer with a byte-address view: TreeWind.hlsli VertexData (t120). a_bytes is a multiple of 4.
		bool CreateRawBuffer(const void* a_data, size_t a_bytes, const char* a_name, winrt::com_ptr<ID3D11Buffer>& a_buffer, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv)
		{
			if (a_bytes == 0 || a_bytes % 4 != 0 || a_bytes > UINT32_MAX)
				return false;
			D3D11_BUFFER_DESC bufferDesc{};
			bufferDesc.ByteWidth = static_cast<UINT>(a_bytes);
			bufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
			bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
			const D3D11_SUBRESOURCE_DATA initData{ a_data, 0, 0 };
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
			srvDesc.BufferEx.FirstElement = 0;
			srvDesc.BufferEx.NumElements = static_cast<UINT>(a_bytes / 4);
			srvDesc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
			winrt::com_ptr<ID3D11Buffer> buffer;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;
			if (FAILED(globals::d3d::device->CreateBuffer(&bufferDesc, &initData, buffer.put())) ||
				FAILED(globals::d3d::device->CreateShaderResourceView(buffer.get(), &srvDesc, srv.put())))
				return false;
			Util::SetResourceName(buffer.get(), "%s", a_name);
			Util::SetResourceName(srv.get(), "%s SRV", a_name);
			a_buffer = std::move(buffer);
			a_srv = std::move(srv);
			return true;
		}

		uint8_t ToUnorm8(float a_value)
		{
			return static_cast<uint8_t>(std::lround(std::clamp(a_value, 0.0f, 1.0f) * 255.0f));
		}

		uint16_t ToUnorm16(float a_value)
		{
			return static_cast<uint16_t>(std::lround(std::clamp(a_value, 0.0f, 1.0f) * 65535.0f));
		}

		// Identity of a shape by content, stable when the game re-creates its buffers: block, kind, counts and three positions
		uint64_t ShapeSignature(uint64_t a_blockKey, Kind a_kind, const VertexView& a_vertices, uint32_t a_triangleCount)
		{
			uint64_t h = 0xcbf29ce484222325ull;
			const auto mix = [&h](uint64_t a_value) {
				h ^= a_value;
				h *= 0x100000001b3ull;
				h ^= h >> 29;
			};
			mix(a_blockKey);
			mix(static_cast<uint64_t>(a_kind));
			mix(a_vertices.count);
			mix(a_triangleCount);
			for (const uint32_t index : { 0u, a_vertices.count / 2, a_vertices.count - 1 }) {
				const auto p = a_vertices(index);
				uint32_t bits[3];
				std::memcpy(bits, &p, sizeof(bits));
				mix((static_cast<uint64_t>(bits[0]) << 32) | bits[1]);
				mix(bits[2]);
			}
			return h;
		}

		// Connected pieces of a shape from its CPU index copy: the root vertex of each vertex's piece.
		// Without an index copy every vertex is its own piece.
		std::vector<uint32_t> PieceRoots(const uint16_t* a_indices, uint32_t a_triangleCount, uint32_t a_vertexCount)
		{
			std::vector<uint32_t> parent(a_vertexCount);
			std::iota(parent.begin(), parent.end(), 0u);
			const auto find = [&](uint32_t a) {
				while (parent[a] != a) {
					parent[a] = parent[parent[a]];
					a = parent[a];
				}
				return a;
			};
			for (uint32_t t = 0; a_indices && t < a_triangleCount; ++t) {
				const uint32_t a = a_indices[t * 3], b = a_indices[t * 3 + 1], c = a_indices[t * 3 + 2];
				if (a >= a_vertexCount || b >= a_vertexCount || c >= a_vertexCount)
					continue;
				parent[find(b)] = find(a);
				parent[find(c)] = find(a);
			}
			for (uint32_t i = 0; i < a_vertexCount; ++i)
				parent[i] = find(i);
			return parent;
		}

		// Renumbers the piece roots to 0..n-1 in place and returns n, so per-piece data sits in a plain array
		uint32_t CompactPieces(std::vector<uint32_t>& a_roots)
		{
			std::vector<uint32_t> id(a_roots.size(), UINT32_MAX);
			uint32_t count = 0;
			for (auto& root : a_roots) {
				if (id[root] == UINT32_MAX)
					id[root] = count++;
				root = id[root];
			}
			return count;
		}

		// a_footX, a_footY: where the trunk meets the ground, in the shape's space; a_world places it in the world
		void AddCard(BendWeights::Block& a_block, BendWeights::TrunkCard a_card, float a_worldScale, const RE::NiTransform& a_world, float a_footX, float a_footY)
		{
			// Crossed card pairs of one tree share its random and scale
			const BendWeights::TrunkCard* twin = nullptr;
			if (const auto it = a_block.grid.find(CellKey(CellOf(a_card.x), CellOf(a_card.y))); it != a_block.grid.end()) {
				for (const int i : it->second) {
					const auto& other = a_block.cards[i];
					if ((other.x - a_card.x) * (other.x - a_card.x) + (other.y - a_card.y) * (other.y - a_card.y) < 64.0f) {
						twin = &other;
						break;
					}
				}
			}
			if (twin) {
				a_card.random = twin->random;
				a_card.scale = twin->scale;
				a_card.tree = twin->tree;
				a_card.tint = twin->tint;
			} else {
				// The loaded tree gets its tint from the position of its reference, which is where its trunk stands
				const auto footWS = a_world * RE::NiPoint3{ a_footX, a_footY, a_card.base };
				a_card.tint = GetTintCode(footWS.x, footWS.y);
				a_card.tree = static_cast<int>(a_block.cards.size());
				a_card.random = Hash01(a_card.x, a_card.y);
				a_card.scale = TreeScale(a_card.top - a_card.base, a_worldScale, Hash01(a_card.y * 1.37f + 11.0f, a_card.x * 0.71f - 5.0f));
			}
			a_block.grid[CellKey(CellOf(a_card.x), CellOf(a_card.y))].push_back(static_cast<int>(a_block.cards.size()));
			a_block.cards.push_back(a_card);
		}

		const BendWeights::TrunkCard* NearestCard(const BendWeights::Block& a_block, float a_x, float a_y)
		{
			const int cx = CellOf(a_x), cy = CellOf(a_y);
			const BendWeights::TrunkCard* best = nullptr;
			float bestDistance = FLT_MAX;
			for (int dy = -1; dy <= 1; ++dy) {
				for (int dx = -1; dx <= 1; ++dx) {
					const auto it = a_block.grid.find(CellKey(cx + dx, cy + dy));
					if (it == a_block.grid.end())
						continue;
					for (const int i : it->second) {
						const auto& card = a_block.cards[i];
						const float distance = (a_x - card.x) * (a_x - card.x) + (a_y - card.y) * (a_y - card.y);
						if (distance < bestDistance) {
							bestDistance = distance;
							best = &card;
						}
					}
				}
			}
			return best;
		}

		// Nearest card within a_maxDistance model units, or nullptr: a piece far from any trunk card stays still
		const BendWeights::TrunkCard* NearestCardWithin(const BendWeights::Block& a_block, float a_x, float a_y, float a_maxDistance)
		{
			const int cx = CellOf(a_x), cy = CellOf(a_y);
			const int reach = static_cast<int>(std::ceil(a_maxDistance / kCellSize));
			const BendWeights::TrunkCard* best = nullptr;
			float bestDistance = a_maxDistance * a_maxDistance;
			for (int dy = -reach; dy <= reach; ++dy) {
				for (int dx = -reach; dx <= reach; ++dx) {
					const auto it = a_block.grid.find(CellKey(cx + dx, cy + dy));
					if (it == a_block.grid.end())
						continue;
					for (const int i : it->second) {
						const auto& card = a_block.cards[i];
						const float distance = (a_x - card.x) * (a_x - card.x) + (a_y - card.y) * (a_y - card.y);
						if (distance < bestDistance) {
							bestDistance = distance;
							best = &card;
						}
					}
				}
			}
			return best;
		}

		// Is a_vertices[a_first .. a_first + 3] a trunk card? Two corners at the bottom and two at the top, the top
		// centred over the bottom and no wider, taller than wide. Used where tree mods merge the cards with 3D trunks
		// and stumps into one shape, so the shape is not all quads.
		bool IsCardQuad(const VertexView& a_vertices, uint32_t a_first)
		{
			RE::NiPoint3 p[4];
			for (uint32_t k = 0; k < 4; ++k)
				p[k] = a_vertices(a_first + k);
			// Trees are placed tilted by up to ~11 degrees, so sort by height: the two lowest corners are the bottom
			std::sort(std::begin(p), std::end(p), [](const RE::NiPoint3& a, const RE::NiPoint3& b) { return a.z < b.z; });
			const RE::NiPoint3* low = p;
			const RE::NiPoint3* high = p + 2;
			const float height = (high[0].z + high[1].z - low[0].z - low[1].z) * 0.5f;
			if (height < 32.0f || low[1].z - low[0].z > 0.1f * height || high[1].z - high[0].z > 0.1f * height)
				return false;
			const float widthLow = std::hypot(low[0].x - low[1].x, low[0].y - low[1].y);
			const float widthHigh = std::hypot(high[0].x - high[1].x, high[0].y - high[1].y);
			const float offsetX = (low[0].x + low[1].x - high[0].x - high[1].x) * 0.5f;
			const float offsetY = (low[0].y + low[1].y - high[0].y - high[1].y) * 0.5f;
			return widthLow > 0.5f && widthLow < height && widthHigh <= widthLow * 1.5f + 1.0f && std::hypot(offsetX, offsetY) < 0.25f * height + 1.0f;
		}

		struct QuadBounds
		{
			float base = FLT_MAX, top = -FLT_MAX, cx = 0.0f, cy = 0.0f;
			float footX = 0.0f, footY = 0.0f;  // centre of the lower edge: a leaning card stands there, not at its centre
		};

		QuadBounds GetQuadBounds(const VertexView& a_vertices, uint32_t a_first)
		{
			QuadBounds q;
			for (uint32_t k = 0; k < 4; ++k) {
				const auto p = a_vertices(a_first + k);
				q.base = std::min(q.base, p.z);
				q.top = std::max(q.top, p.z);
				q.cx += p.x * 0.25f;
				q.cy += p.y * 0.25f;
			}
			const float middle = 0.5f * (q.base + q.top);
			int low = 0;
			for (uint32_t k = 0; k < 4; ++k) {
				const auto p = a_vertices(a_first + k);
				if (p.z <= middle) {
					q.footX += p.x;
					q.footY += p.y;
					++low;
				}
			}
			q.footX = low > 0 ? q.footX / static_cast<float>(low) : q.cx;
			q.footY = low > 0 ? q.footY / static_cast<float>(low) : q.cy;
			return q;
		}

		// Crown data for the pixel shader, see BendWeights::GetCrownData. a_vertexTree gives each vertex the index
		// of its tree's first trunk card in the block, or -1. The crown extent of a tree is measured from the
		// vertices of this shape that follow it.
		void BuildCrownData(const VertexView& a_vertices, const std::vector<int32_t>& a_vertexTree, const BendWeights::Block& a_block,
			winrt::com_ptr<ID3D11Buffer>& a_buffer, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv)
		{
			struct Extent
			{
				float zMin = FLT_MAX, zMax = -FLT_MAX, radiusSq = 0.0f;
			};
			std::vector<Extent> extents(a_block.cards.size());
			bool any = false;
			for (uint32_t i = 0; i < a_vertices.count; ++i) {
				const int32_t tree = a_vertexTree[i];
				if (tree < 0 || static_cast<size_t>(tree) >= extents.size())
					continue;
				const auto p = a_vertices(i);
				const auto& card = a_block.cards[tree];
				auto& extent = extents[tree];
				extent.zMin = std::min(extent.zMin, p.z);
				extent.zMax = std::max(extent.zMax, p.z);
				extent.radiusSq = std::max(extent.radiusSq, (p.x - card.x) * (p.x - card.x) + (p.y - card.y) * (p.y - card.y));
				any = true;
			}
			if (!any)
				return;

			const auto toSnorm = [](float a_value) {
				return static_cast<int8_t>(std::lround(std::clamp(a_value, -1.0f, 1.0f) * 127.0f));
			};
			std::vector<int8_t> data(static_cast<size_t>(a_vertices.count) * 4, 0);
			for (uint32_t i = 0; i < a_vertices.count; ++i) {
				const int32_t tree = a_vertexTree[i];
				if (tree < 0 || static_cast<size_t>(tree) >= extents.size())
					continue;
				const auto p = a_vertices(i);
				const auto& card = a_block.cards[tree];
				const auto& extent = extents[tree];
				const float radius = std::max(std::sqrt(extent.radiusSq), 1.0f);
				const float height = std::max(extent.zMax - extent.zMin, 1.0f);
				const size_t at = static_cast<size_t>(i) * 4;
				data[at + 0] = toSnorm((p.x - card.x) / radius);
				data[at + 1] = toSnorm((p.y - card.y) / radius);
				data[at + 2] = toSnorm((p.z - extent.zMin) / height);
				data[at + 3] = static_cast<int8_t>(1 + std::min<uint32_t>(card.tint, kTintLevels * kTintLevels - 1));
			}

			D3D11_BUFFER_DESC bufferDesc{};
			bufferDesc.ByteWidth = static_cast<UINT>(data.size());
			bufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
			bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			const D3D11_SUBRESOURCE_DATA initData{ data.data(), 0, 0 };
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = DXGI_FORMAT_R8G8B8A8_SNORM;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			srvDesc.Buffer.NumElements = a_vertices.count;
			winrt::com_ptr<ID3D11Buffer> buffer;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;
			if (FAILED(globals::d3d::device->CreateBuffer(&bufferDesc, &initData, buffer.put())) ||
				FAILED(globals::d3d::device->CreateShaderResourceView(buffer.get(), &srvDesc, srv.put())))
				return;
			Util::SetResourceName(buffer.get(), "TreeWind::CrownData");
			Util::SetResourceName(srv.get(), "TreeWind::CrownData SRV");
			a_buffer = std::move(buffer);
			a_srv = std::move(srv);
		}
	}

	bool IsPassthruShape(const RE::BSGeometry* a_geometry)
	{
		const char* name = a_geometry ? a_geometry->name.c_str() : nullptr;
		if (!name)
			return false;
		std::string_view view(name);
		constexpr std::string_view largeRef = "-LargeRef";
		if (view.size() > largeRef.size() && _strnicmp(view.data() + view.size() - largeRef.size(), largeRef.data(), largeRef.size()) == 0)
			view.remove_suffix(largeRef.size());
		for (const std::string_view passthru : { "passthru"sv, "treepassthru"sv, "objpassthru"sv }) {
			if (view.size() == passthru.size() && _strnicmp(view.data(), passthru.data(), view.size()) == 0)
				return true;
		}
		return false;
	}

	Kind Classify(const RE::BSRenderPass* a_pass, bool a_treeModSupport)
	{
		const auto property = a_pass ? a_pass->shaderProperty : nullptr;
		const auto geometry = a_pass ? a_pass->geometry : nullptr;
		if (!property || !geometry)
			return Kind::None;

		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		if (!property->flags.all(Flag::kVertexColors) || !property->flags.none(Flag::kLODObjects, Flag::kHDLODObjects, Flag::kTreeAnim, Flag::kLODLandscape))
			return Kind::None;

		// DynDOLOD's merged object LOD geometry; loaded plants with the same flags are plain BSTriShapes
		static REL::Relocation<const RE::NiRTTI*> subIndexTriShapeRTTI{ RE::BSSubIndexTriShape::Ni_RTTI };
		if (geometry->GetRTTI() != subIndexTriShapeRTTI.get())
			return Kind::None;

		if (IsTreeShapeName(geometry))
			return Kind::Crown;

		// 3D LOD crowns
		if (property->flags.all(Flag::kTwoSided) && property->flags.any(Flag::kRimLighting, Flag::kSoftLighting))
			return Kind::Crown;

		const bool allQuads = IsAllQuads(geometry);
		if (property->flags.none(Flag::kTwoSided, Flag::kRimLighting, Flag::kSoftLighting)) {
			// 3D LOD trunks are flat crossed cards: quads only, which rocks and buildings with the same flags are not.
			// Tree mods merge the cards with 3D trunks and stumps; the bend data then looks for the cards.
			return allQuads || (a_treeModSupport && geometry->AsTriShape()) ? Kind::Trunk : Kind::None;
		}

		// Tree-mod crowns: two-sided without rim or soft lighting, never all quads, and only in the LOD4 ring;
		// at LOD8 and LOD16 the tree-mod trees are billboards and these shapes are not trees
		if (a_treeModSupport && geometry->AsTriShape() && !allQuads && property->flags.all(Flag::kTwoSided) && geometry->world.scale < 6.0f)
			return Kind::Crown;

		return Kind::None;
	}

	RE::NiNode* FindNearTreeRoot(const RE::BSRenderPass* a_pass)
	{
		const auto property = a_pass ? a_pass->shaderProperty : nullptr;
		const auto geometry = a_pass ? a_pass->geometry : nullptr;
		if (!property || !geometry)
			return nullptr;

		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		if (property->flags.any(Flag::kSkinned, Flag::kLODObjects, Flag::kHDLODObjects, Flag::kLODLandscape))
			return nullptr;

		static REL::Relocation<const RE::NiRTTI*> triShapeRTTI{ RE::BSTriShape::Ni_RTTI };
		static REL::Relocation<const RE::NiRTTI*> leafAnimNodeRTTI{ RE::BSLeafAnimNode::Ni_RTTI };
		static REL::Relocation<const RE::NiRTTI*> treeNodeRTTI{ RE::BSTreeNode::Ni_RTTI };
		if (geometry->GetRTTI() != triShapeRTTI.get())
			return nullptr;

		// The first fade node above a shape is the 3D root of its reference: one or two steps for almost everything drawn
		int depth = 0;
		for (auto node = geometry->parent; node && depth < kNearTreeDepth; node = node->parent, ++depth) {
			if (node->AsFadeNode()) {
				const auto rtti = node->GetRTTI();
				return rtti == leafAnimNodeRTTI.get() || rtti == treeNodeRTTI.get() ? node : nullptr;
			}
		}
		return nullptr;
	}

	RE::NiNode* FindNearTreeLeafRoot(const RE::BSRenderPass* a_pass)
	{
		const auto property = a_pass ? a_pass->shaderProperty : nullptr;
		const auto geometry = a_pass ? a_pass->geometry : nullptr;
		if (!property || !geometry)
			return nullptr;

		// The flags first: they rule out nearly every draw before the walk up the scene graph
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		if (property->flags.any(Flag::kLODObjects, Flag::kHDLODObjects, Flag::kLODLandscape))
			return nullptr;
		if (property->flags.none(Flag::kTreeAnim)) {
			if (!property->flags.all(Flag::kTwoSided, Flag::kVertexColors))
				return nullptr;
			const auto alphaProperty = geometry->GetGeometryRuntimeData().alphaProperty.get();
			if (!alphaProperty || !alphaProperty->GetAlphaTesting())
				return nullptr;
		}
		if (!geometry->AsTriShape())
			return nullptr;

		static REL::Relocation<const RE::NiRTTI*> leafAnimNodeRTTI{ RE::BSLeafAnimNode::Ni_RTTI };
		static REL::Relocation<const RE::NiRTTI*> treeNodeRTTI{ RE::BSTreeNode::Ni_RTTI };
		int depth = 0;
		for (auto node = geometry->parent; node && depth < kNearTreeDepth; node = node->parent, ++depth) {
			if (node->AsFadeNode()) {
				const auto rtti = node->GetRTTI();
				return rtti == leafAnimNodeRTTI.get() || rtti == treeNodeRTTI.get() ? node : nullptr;
			}
		}
		return nullptr;
	}

	uint8_t GetTintCode(float a_worldX, float a_worldY)
	{
		// Triangle wave 0-1 with period 1, as in TreeWind.hlsli: continuous, so a small error in the position is a
		// small error in the seed
		const auto triangle = [](double a_x) {
			return std::abs((a_x - std::floor(a_x)) * 2.0 - 1.0);
		};
		const double x = a_worldX, y = a_worldY;
		// Two waves each, the second bending the first, so the tint does not run in stripes across a forest
		const double brightness = triangle(x * 0.0021 + y * 0.0013 + 0.5 * triangle(y * 0.0019 - x * 0.0011));
		const double hue = triangle(x * 0.0009 - y * 0.0023 + 0.31 + 0.5 * triangle(x * 0.0017 + y * 0.0008));
		const auto level = [](double a_seed) {
			return std::isfinite(a_seed) ? std::min(static_cast<uint32_t>(std::clamp(a_seed, 0.0, 1.0) * kTintLevels), kTintLevels - 1) : kTintLevels / 2;
		};
		return static_cast<uint8_t>(level(brightness) * kTintLevels + level(hue));
	}

	namespace
	{
		// Model paths compare in lower case with backslashes and without a leading meshes folder
		std::string NormalizeModelPath(std::string_view a_path)
		{
			std::string path(a_path);
			for (auto& c : path)
				c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			while (!path.empty() && path.front() == '\\')
				path.erase(path.begin());
			constexpr std::string_view meshes = "meshes\\";
			if (path.starts_with(meshes))
				path.erase(0, meshes.size());
			return path;
		}

		// The wind clock wraps at 200 pi, so every speed that multiplies it must be a multiple of a_step (0.01, or
		// 0.02 where the shader also uses one and a half times the speed)
		float RoundSpeed(float a_speed, float a_step)
		{
			return std::max(std::round(a_speed / a_step), 1.0f) * a_step;
		}

		// Reads a wind mask file (format in TreeShapes.h). Returns false and leaves a_out empty when it is not one.
		bool ReadMaskFile(const std::filesystem::path& a_path, std::vector<BespokeProfile::MaskShape>& a_out)
		{
			a_out.clear();
			std::ifstream stream(a_path, std::ios::binary);
			if (!stream)
				return false;
			const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
			size_t at = 0;
			const auto take = [&](void* a_to, size_t a_size) {
				if (a_size > bytes.size() - at)
					return false;
				std::memcpy(a_to, bytes.data() + at, a_size);
				at += a_size;
				return true;
			};
			char magic[4];
			uint32_t version = 0, shapeCount = 0;
			if (!take(magic, 4) || std::memcmp(magic, "TWMK", 4) != 0 || !take(&version, 4) || version != 1 || !take(&shapeCount, 4) || shapeCount > 4096)
				return false;
			for (uint32_t s = 0; s < shapeCount; ++s) {
				BespokeProfile::MaskShape shape;
				uint32_t nameLength = 0;
				if (!take(&nameLength, 4) || nameLength > 1024 || nameLength > bytes.size() - at) {
					a_out.clear();
					return false;
				}
				shape.name.assign(reinterpret_cast<const char*>(bytes.data() + at), nameLength);
				at += nameLength;
				if (!take(&shape.vertexCount, 4) || !take(&shape.positionHash, 8) || shape.vertexCount == 0 || shape.vertexCount > 0xFFFFu ||
					static_cast<size_t>(shape.vertexCount) * 4 > bytes.size() - at) {
					a_out.clear();
					return false;
				}
				shape.rgba.assign(bytes.begin() + at, bytes.begin() + at + static_cast<size_t>(shape.vertexCount) * 4);
				at += static_cast<size_t>(shape.vertexCount) * 4;
				a_out.push_back(std::move(shape));
			}
			return true;
		}
	}

	void BespokeProfiles::Load()
	{
		profiles.clear();
		const auto directory = Util::PathHelpers::GetCommunityShaderPath() / "TreeWind";
		std::error_code error;
		std::vector<std::filesystem::path> files;
		for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
			if (it->is_regular_file(error) && _stricmp(it->path().extension().string().c_str(), ".json") == 0)
				files.push_back(it->path());
		}
		std::sort(files.begin(), files.end());

		for (const auto& file : files) {
			try {
				std::ifstream stream(file);
				const auto root = nlohmann::json::parse(stream, nullptr, true, true);
				const auto trees = root.find("Trees");
				if (trees == root.end() || !trees->is_array())
					continue;
				for (const auto& tree : *trees) {
					BespokeProfile profile;
					profile.name = tree.value("Name", file.stem().string());
					if (const auto models = tree.find("Models"); models != tree.end() && models->is_array()) {
						for (const auto& model : *models)
							if (model.is_string())
								profile.models.push_back(NormalizeModelPath(model.get<std::string>()));
					}
					if (profile.models.empty())
						continue;
					const auto read = [&tree](const char* a_section, const char* a_key, float& a_value, float a_min, float a_max) {
						const auto section = tree.find(a_section);
						if (section == tree.end() || !section->is_object())
							return;
						const auto value = section->find(a_key);
						if (value != section->end() && value->is_number())
							a_value = std::clamp(value->get<float>(), a_min, a_max);
					};
					read("Branch", "Amplitude", profile.branchAmplitude, 0.0f, 2000.0f);
					read("Branch", "Speed", profile.branchSpeed, 0.02f, 4.0f);
					read("Branch", "Spread", profile.branchSpread, 0.0f, 12.0f);
					read("Branch", "Gust", profile.branchGust, 0.0f, 2.0f);
					read("Branch", "Lift", profile.branchLift, 0.0f, 1.0f);
					read("Leaves", "Amplitude", profile.leafAmplitude, 0.0f, 400.0f);
					read("Leaves", "Speed", profile.leafSpeed, 0.01f, 20.0f);
					read("Ivy", "Angle", profile.ivyAngle, 0.0f, 1.5f);
					read("Ivy", "Lean", profile.ivyLean, 0.0f, 3.0f);
					read("Ivy", "Speed", profile.ivySpeed, 0.05f, 10.0f);
					read("Ivy", "Cross", profile.ivyCross, 0.0f, 1.0f);
					read("Ivy", "Gust", profile.ivyGust, 0.0f, 4.0f);
					read("Ivy", "ReferenceLength", profile.ivyReferenceLength, 10.0f, 100000.0f);
					read("Ivy", "MaxAngle", profile.ivyMaxAngle, 0.0f, 1.5f);
					// The mask sits next to the profile; a name with a folder in it is not accepted
					const auto maskName = std::filesystem::path(tree.value("Mask", std::string())).filename();
					size_t maskVertices = 0;
					if (maskName.empty()) {
						logger::warn("[TreeWind] bespoke profile '{}' names no Mask file", profile.name);
					} else if (!ReadMaskFile(directory / maskName, profile.mask)) {
						logger::warn("[TreeWind] bespoke profile '{}': mask file {} is missing or not a wind mask", profile.name, maskName.string());
					} else {
						for (const auto& shape : profile.mask)
							maskVertices += shape.vertexCount;
					}
					logger::info("[TreeWind] bespoke profile '{}' from {}: {} model(s), first '{}', mask {} shapes / {} vertices", profile.name, file.filename().string(),
						profile.models.size(), profile.models.front(), profile.mask.size(), maskVertices);
					profiles.push_back(std::move(profile));
				}
			} catch (const std::exception& e) {
				logger::warn("[TreeWind] bespoke profile file {} not read: {}", file.string(), e.what());
			}
		}
	}

	const BespokeProfile* BespokeProfiles::Find(RE::NiNode* a_root) const
	{
		if (profiles.empty() || !a_root)
			return nullptr;
		const auto reference = a_root->GetUserData();
		const auto base = reference ? reference->GetBaseObject() : nullptr;
		const auto model = base ? base->As<RE::TESModel>() : nullptr;
		const char* path = model ? model->GetModel() : nullptr;
		if (!path || !*path)
			return nullptr;
		const auto normalized = NormalizeModelPath(path);
		for (const auto& profile : profiles)
			for (const auto& candidate : profile.models)
				if (candidate == normalized)
					return &profile;
		return nullptr;
	}

	namespace
	{
		struct NearShape
		{
			RE::BSGeometry* geometry = nullptr;
			VertexView vertices;
			RE::NiTransform toRoot;            // shape space -> tree root space
			std::vector<RE::NiPoint3> points;  // vertices in tree root space
			uint64_t positionHash = 0;         // VertexView::PositionHash, for the shape id and the bespoke mask
		};

		// Shape space -> tree root space, or false when the root is not within kNearTreeDepth above the shape
		bool ShapeToRoot(const RE::BSGeometry* a_geometry, const RE::NiNode* a_root, RE::NiTransform& a_out)
		{
			a_out = a_geometry->local;
			int depth = 0;
			for (auto node = a_geometry->parent; node && depth < kNearTreeDepth; node = node->parent, ++depth) {
				if (node == a_root)
					return a_out.scale > 1e-4f;
				a_out = node->local * a_out;
			}
			return false;
		}

		uint64_t FnvString(const char* a_text)
		{
			uint64_t h = 0xcbf29ce484222325ull;
			for (const char* c = a_text; c && *c; ++c) {
				h ^= static_cast<unsigned char>(*c);
				h *= 0x100000001b3ull;
			}
			return h;
		}

		void MixHash(uint64_t& a_hash, uint64_t a_value)
		{
			a_hash ^= a_value;
			a_hash *= 0x100000001b3ull;
			a_hash ^= a_hash >> 29;
		}

		void MixTransform(uint64_t& a_hash, const RE::NiTransform& a_transform)
		{
			uint32_t bits[13];
			static_assert(sizeof(a_transform.rotate) == sizeof(float) * 9 && sizeof(a_transform.translate) == sizeof(float) * 3);
			std::memcpy(bits, &a_transform.rotate, sizeof(float) * 9);
			std::memcpy(bits + 9, &a_transform.translate, sizeof(float) * 3);
			std::memcpy(bits + 12, &a_transform.scale, sizeof(float));
			for (const auto b : bits)
				MixHash(a_hash, b);
		}

		// FNV-1a 64 of the model path of the reference under a_root (the root's name when there is none): the tree
		// the bend data belongs to
		uint64_t ModelHash(RE::NiNode* a_root)
		{
			const auto reference = a_root ? a_root->GetUserData() : nullptr;
			const auto base = reference ? reference->GetBaseObject() : nullptr;
			const auto model = base ? base->As<RE::TESModel>() : nullptr;
			const char* path = model ? model->GetModel() : nullptr;
			const char* rootName = a_root ? a_root->name.c_str() : nullptr;
			return FnvString(path && *path ? NormalizeModelPath(path).c_str() : (rootName ? rootName : ""));
		}

		// Identity of one shape within a tree record: its name, vertex and triangle counts, place in the tree and
		// every vertex position. Two shapes with different vertices never share a record slot.
		uint64_t NearShapeId(RE::BSGeometry* a_geometry, const RE::NiTransform& a_toRoot, const VertexView& a_vertices, uint64_t a_positionHash)
		{
			uint64_t h = 0xcbf29ce484222325ull;
			MixHash(h, FnvString(a_geometry->name.c_str()));
			MixHash(h, a_vertices.count);
			const auto triShape = a_geometry->AsTriShape();
			MixHash(h, triShape ? triShape->GetTrishapeRuntimeData().triangleCount : 0u);
			MixTransform(h, a_toRoot);
			MixHash(h, a_positionHash);
			return h;
		}

		void MixTreeShapes(RE::NiNode* a_root, RE::NiNode* a_node, int a_depth, uint64_t& a_hash)
		{
			static REL::Relocation<const RE::NiRTTI*> triShapeRTTI{ RE::BSTriShape::Ni_RTTI };
			for (const auto& child : a_node->GetChildren()) {
				if (!child)
					continue;
				if (auto geometry = child->AsGeometry()) {
					if (geometry->GetRTTI() != triShapeRTTI.get())
						continue;
					MixHash(a_hash, FnvString(geometry->name.c_str()));
					const auto& data = geometry->AsTriShape()->GetTrishapeRuntimeData();
					MixHash(a_hash, data.vertexCount);
					MixHash(a_hash, data.triangleCount);
					MixHash(a_hash, geometry->GetGeometryRuntimeData().skinInstance ? 1u : 0u);
					RE::NiTransform toRoot;
					if (ShapeToRoot(geometry, a_root, toRoot))
						MixTransform(a_hash, toRoot);
				} else if (auto node = child->AsNode(); node && a_depth < kNearTreeDepth) {
					MixTreeShapes(a_root, node, a_depth + 1, a_hash);
				}
			}
		}

		// Identity of a whole loaded tree: its model, its bespoke profile (if any) and every shape under its root
		// with its name, counts and place. Trees sharing a shape mesh at another height, or under another profile,
		// get other keys, so a record is only ever reused by a tree built exactly like it.
		uint64_t NearTreeKey(RE::NiNode* a_root, uint64_t a_modelHash, const BespokeProfile* a_profile)
		{
			uint64_t h = 0xcbf29ce484222325ull;
			MixHash(h, a_modelHash);
			MixHash(h, a_profile ? FnvString(a_profile->name.c_str()) : 0u);
			MixTreeShapes(a_root, a_root, 0, h);
			return h;
		}

		// Collects the plain tri shapes under a tree root. Returns false when the tree must stay still: a shape is
		// skinned (the engine animates the tree) or its vertices cannot be read (it would stay behind the others).
		// a_readable gets the shapes whose vertices are readable but which do not reach the root.
		bool CollectNearShapes(RE::NiNode* a_root, RE::NiNode* a_node, int a_depth, std::vector<NearShape>& a_shapes, std::vector<VertexView>& a_readable)
		{
			static REL::Relocation<const RE::NiRTTI*> triShapeRTTI{ RE::BSTriShape::Ni_RTTI };
			bool sways = true;
			for (const auto& child : a_node->GetChildren()) {
				if (!child)
					continue;
				if (auto geometry = child->AsGeometry()) {
					if (geometry->GetGeometryRuntimeData().skinInstance) {
						sways = false;
						continue;
					}
					// Particles and other geometry types are not part of the tree mesh
					if (geometry->GetRTTI() != triShapeRTTI.get())
						continue;
					NearShape shape;
					shape.geometry = geometry;
					if (!GetVertices(geometry, shape.vertices)) {
						sways = false;
						continue;
					}
					if (!ShapeToRoot(geometry, a_root, shape.toRoot)) {
						a_readable.push_back(shape.vertices);
						sways = false;
						continue;
					}
					a_shapes.push_back(std::move(shape));
				} else if (auto node = child->AsNode(); node && a_depth < kNearTreeDepth) {
					sways = CollectNearShapes(a_root, node, a_depth + 1, a_shapes, a_readable) && sways;
				}
			}
			return sways;
		}
	}

	NearTrees::Tree::~Tree()
	{
		if (accounted)
			*accounted -= bytes;
	}

	void NearTrees::Forget(const Entry& a_entry)
	{
		swayCount -= a_entry.srv != nullptr;
		bespokeCount -= a_entry.srv != nullptr && a_entry.bespoke;
	}

	void NearTrees::SetBudget(uint64_t a_bytes)
	{
		budgetBytes = a_bytes;
	}

	void NearTrees::DropEntriesOf(uint64_t a_treeKey)
	{
		std::erase_if(entries, [&](const auto& a_entry) {
			const bool gone = a_entry.second.treeKey == a_treeKey && a_entry.second.tree;
			if (gone)
				Forget(a_entry.second);
			return gone;
		});
	}

	ID3D11ShaderResourceView* NearTrees::Get(RE::BSGeometry* a_geometry, RE::NiNode* a_root, uint a_frame, float a_minHeight, const BespokeProfiles& a_profiles, bool& a_bespoke)
	{
		a_bespoke = false;
		VertexView vertices;
		if (!a_geometry || !a_root || !GetVertices(a_geometry, vertices))
			return nullptr;

		const auto firstVertex = vertices(0);
		if (auto it = entries.find(vertices.vertexBuffer); it != entries.end()) {
			auto& entry = it->second;
			if (entry.vertexCount == vertices.count && entry.firstVertex[0] == firstVertex.x && entry.firstVertex[1] == firstVertex.y && entry.firstVertex[2] == firstVertex.z) {
				entry.lastFrame = a_frame;
				a_bespoke = entry.bespoke;
				return entry.srv;
			}
			// The vertex buffer address was reused by another shape
			Forget(entry);
			entries.erase(it);
		}

		// The record of a tree built exactly like this one (same model, profile and shapes): the tree loaded again, or
		// a second copy of its mesh. A tree with a bespoke profile is never served from a record: it is measured
		// afresh and its mask matched to its own vertices, so the hand-masked tree cannot inherit anything.
		const uint64_t modelHash = ModelHash(a_root);
		const BespokeProfile* profile = a_profiles.Find(a_root);
		const uint64_t treeKey = NearTreeKey(a_root, modelHash, profile);
		if (!profile) {
			if (const auto treeIt = trees.find(treeKey); treeIt != trees.end()) {
				const auto& tree = treeIt->second;
				RE::NiTransform toRoot;
				if (ShapeToRoot(a_geometry, a_root, toRoot)) {
					const uint64_t shapeId = NearShapeId(a_geometry, toRoot, vertices, vertices.PositionHash());
					for (const auto& shape : tree->shapes) {
						if (shape.id != shapeId)
							continue;
						Entry reused;
						reused.tree = tree;
						reused.srv = shape.srv.get();
						reused.bespoke = tree->bespoke;
						reused.vertexCount = vertices.count;
						reused.firstVertex[0] = firstVertex.x;
						reused.firstVertex[1] = firstVertex.y;
						reused.firstVertex[2] = firstVertex.z;
						reused.lastFrame = a_frame;
						reused.treeKey = treeKey;
						tree->lastFrame = a_frame;
						swayCount += reused.srv != nullptr;
						bespokeCount += reused.srv != nullptr && reused.bespoke;
						++reusedShapes;
						a_bespoke = reused.bespoke;
						return entries.insert_or_assign(vertices.vertexBuffer, std::move(reused)).first->second.srv;
					}
				}
				// A shape the record does not know: the tree is measured again as a whole below
			}
		}

		// Out of time this frame: the tree stands still until its turn
		if (a_frame != budgetFrame) {
			budgetFrame = a_frame;
			budgetSpentNs = 0;
		}
		if (budgetSpentNs >= kNearBuildBudgetNs)
			return nullptr;
		// Past the memory budget or the shape cap nothing new is measured: the tree stands still until the sweep frees room
		if (treeBytes >= budgetBytes || swayCount >= kMaxNearShapes) {
			++refusedTrees;
			return nullptr;
		}

		Build(a_root, treeKey, profile, a_frame, a_minHeight);

		if (auto it = entries.find(vertices.vertexBuffer); it != entries.end()) {
			a_bespoke = it->second.bespoke;
			return it->second.srv;
		}

		// Not part of the tree as measured (for example a skinned shape): remembered as still, without a record
		Entry still;
		still.vertexCount = vertices.count;
		still.firstVertex[0] = firstVertex.x;
		still.firstVertex[1] = firstVertex.y;
		still.firstVertex[2] = firstVertex.z;
		still.lastFrame = a_frame;
		still.treeKey = treeKey;
		entries.insert_or_assign(vertices.vertexBuffer, std::move(still));
		return nullptr;
	}

	void NearTrees::Build(RE::NiNode* a_root, uint64_t a_treeKey, const BespokeProfile* a_profile, uint a_frame, float a_minHeight)
	{
		ZoneScopedN("TreeWind::BuildNearTree");
		const auto start = std::chrono::steady_clock::now();
		++builtTrees;

		std::vector<NearShape> shapes;
		std::vector<VertexView> readable;
		bool sways = CollectNearShapes(a_root, a_root, 0, shapes, readable) && !shapes.empty();

		// Tree extent in root space
		float zMin = FLT_MAX, zMax = -FLT_MAX;
		if (sways) {
			for (auto& shape : shapes) {
				shape.points.resize(shape.vertices.count);
				for (uint32_t i = 0; i < shape.vertices.count; ++i) {
					const auto p = shape.toRoot * shape.vertices(i);
					shape.points[i] = p;
					zMin = std::min(zMin, p.z);
					zMax = std::max(zMax, p.z);
				}
			}
		}
		// The base is the root origin (ground level) when the roots reach a little below it, so they stay planted;
		// a tree whose pivot is elsewhere bends from its lowest vertex
		const float base = (zMin < 0.0f && -zMin <= 0.15f * (zMax - zMin)) ? 0.0f : zMin;
		const float height = zMax - base;
		sways = sways && std::isfinite(height) && height >= std::max(a_minHeight, 1.0f);

		float axisX = 0.0f, axisY = 0.0f, crownRadius = 1.0f;
		if (sways) {
			// Trunk axis: centre of the vertices in a low slab above the root flare, widened if that slab is empty
			for (const auto& [low, high] : { std::pair{ 0.02f, 0.12f }, std::pair{ 0.0f, 0.3f } }) {
				double sumX = 0.0, sumY = 0.0;
				uint64_t count = 0;
				for (const auto& shape : shapes) {
					for (const auto& p : shape.points) {
						if (p.z >= base + low * height && p.z <= base + high * height) {
							sumX += p.x;
							sumY += p.y;
							++count;
						}
					}
				}
				if (count > 0) {
					axisX = static_cast<float>(sumX / static_cast<double>(count));
					axisY = static_cast<float>(sumY / static_cast<double>(count));
					break;
				}
			}
			float radiusSq = 1.0f;
			for (const auto& shape : shapes)
				for (const auto& p : shape.points)
					radiusSq = std::max(radiusSq, (p.x - axisX) * (p.x - axisX) + (p.y - axisY) * (p.y - axisY));
			crownRadius = std::sqrt(radiusSq);
		}

		// Every shape's identity in the record: the position hash is also what the bespoke mask is matched by
		for (auto& shape : shapes)
			shape.positionHash = shape.vertices.PositionHash();

		// A bespoke tree needs a mask for every shape, made for exactly these vertices (same count, same positions in
		// the same order); with another version of the mesh it sways like any other tree
		const BespokeProfile* profile = sways ? a_profile : nullptr;
		std::vector<const uint8_t*> masks(shapes.size(), nullptr);
		if (profile) {
			size_t matched = 0;
			for (size_t s = 0; s < shapes.size(); ++s) {
				const auto& vertices = shapes[s].vertices;
				for (const auto& candidate : profile->mask) {
					if (candidate.vertexCount != vertices.count || candidate.rgba.size() != static_cast<size_t>(vertices.count) * 4)
						continue;
					if (candidate.positionHash == shapes[s].positionHash) {
						masks[s] = candidate.rgba.data();
						++matched;
						break;
					}
				}
			}
			if (matched != shapes.size()) {
				if (warnedModels.insert(profile->name).second)
					logger::warn("[TreeWind] bespoke profile '{}': the wind mask fits {} of the {} shapes of the loaded mesh (it is made for one exact version of the mesh); using the ordinary nearby-tree sway",
						profile->name, matched, shapes.size());
				profile = nullptr;
			}
		}
		const bool bespoke = profile != nullptr;

		// The record of this tree: one slot per shape, built together and freed together
		auto tree = std::make_shared<Tree>();
		tree->bespoke = bespoke;
		tree->lastFrame = a_frame;
		tree->shapes.resize(shapes.size());
		for (size_t s = 0; s < shapes.size(); ++s)
			tree->shapes[s].id = NearShapeId(shapes[s].geometry, shapes[s].toRoot, shapes[s].vertices, shapes[s].positionHash);

		bool created = true;
		if (sways) {
			for (size_t s = 0; s < shapes.size() && created && bespoke; ++s) {
				// Layout in TreeShapes.h (NearTrees) and TreeWind.hlsli GetBespokeTree
				constexpr uint32_t kHeader = 5;
				const auto& shape = shapes[s];
				const uint32_t elements = shape.vertices.count + kHeader;
				std::vector<float> data(static_cast<size_t>(elements) * 4, 0.0f);
				const auto origin = shape.toRoot.Invert() * RE::NiPoint3{ axisX, axisY, base };
				const float header[kHeader][4] = {
					{ origin.x, origin.y, origin.z, height / shape.toRoot.scale },
					{ 1.0f / shape.toRoot.scale, profile->branchAmplitude, RoundSpeed(profile->branchSpeed, 0.02f), profile->branchSpread },
					{ profile->branchGust, profile->branchLift, profile->leafAmplitude, RoundSpeed(profile->leafSpeed, 0.01f) },
					{ profile->ivyAngle, profile->ivyLean, profile->ivySpeed, profile->ivyCross },
					{ profile->ivyGust, profile->ivyReferenceLength, profile->ivyMaxAngle, 0.0f },
				};
				std::memcpy(data.data(), header, sizeof(header));
				for (uint32_t i = 0; i < shape.vertices.count; ++i) {
					const uint8_t* color = masks[s] + static_cast<size_t>(i) * 4;
					float* out = &data[static_cast<size_t>(i + kHeader) * 4];
					const bool strand = color[3] >= 128;
					const float g = color[1] / 255.0f;
					out[0] = color[0] / 255.0f;
					// A strand stores its free length (negative, in root units) where the others store their flutter weight
					out[1] = strand ? -std::max(16384.0f * g * g, 1.0f) : g;
					out[2] = strand ? color[2] / 255.0f : 0.0f;
					out[3] = (color[3] & 127) / 128.0f;
				}

				// The same bytes as before, read raw by the shader (asfloat); the motion of the tree is unchanged
				const size_t bytes = data.size() * sizeof(float);
				created = CreateRawBuffer(data.data(), bytes, "TreeWind::BespokeTree", tree->shapes[s].buffer, tree->shapes[s].srv);
				tree->bytes += created ? bytes : 0;
			}
			if (bespoke)
				logger::info("[TreeWind] bespoke tree '{}': {} shapes, height {:.0f}, trunk axis ({:.0f}, {:.0f}){}", profile->name, shapes.size(), height, axisX, axisY, created ? "" : ", buffers failed");
			for (size_t s = 0; s < shapes.size() && created && !bespoke; ++s) {
				// Layout in TreeShapes.h (NearTrees) and TreeWind.hlsli GetNearTree: a 16-byte float header, then
				// R16G16_UNORM per vertex (both values are 0-1; 16 bits keep the full-model trees as smooth as before)
				const auto& shape = shapes[s];
				std::vector<uint8_t> data(16 + static_cast<size_t>(shape.vertices.count) * 4);
				const auto origin = shape.toRoot.Invert() * RE::NiPoint3{ axisX, axisY, base };
				const float header[4] = { origin.x, origin.y, origin.z, height / shape.toRoot.scale };
				std::memcpy(data.data(), header, sizeof(header));
				for (uint32_t i = 0; i < shape.vertices.count; ++i) {
					const auto& p = shape.points[i];
					const uint32_t packed = ToUnorm16((p.z - base) / height) | (static_cast<uint32_t>(ToUnorm16(std::hypot(p.x - axisX, p.y - axisY) / crownRadius)) << 16);
					std::memcpy(&data[16 + static_cast<size_t>(i) * 4], &packed, sizeof(packed));
				}
				created = CreateRawBuffer(data.data(), data.size(), "TreeWind::NearTree", tree->shapes[s].buffer, tree->shapes[s].srv);
				tree->bytes += created ? data.size() : 0;
			}
		}
		// All shapes of the tree or none, so the tree never moves in parts
		if (!created) {
			for (auto& shape : tree->shapes) {
				shape.buffer = nullptr;
				shape.srv = nullptr;
			}
			tree->bytes = 0;
		}
		tree->accounted = &treeBytes;
		treeBytes += tree->bytes;

		// The record replaces any older one of this tree, together with the entries drawing from the old one: they
		// find the new record on their next draw, so no entry keeps an orphaned buffer alive
		DropEntriesOf(a_treeKey);
		trees[a_treeKey] = tree;

		const auto insert = [&](const VertexView& a_vertices, const std::shared_ptr<Tree>& a_tree, ID3D11ShaderResourceView* a_srv) {
			Entry entry;
			entry.tree = a_tree;
			entry.srv = a_srv;
			entry.bespoke = bespoke && a_srv;
			entry.vertexCount = a_vertices.count;
			const auto first = a_vertices(0);
			entry.firstVertex[0] = first.x;
			entry.firstVertex[1] = first.y;
			entry.firstVertex[2] = first.z;
			entry.lastFrame = a_frame;
			entry.treeKey = a_treeKey;
			if (const auto it = entries.find(a_vertices.vertexBuffer); it != entries.end())
				Forget(it->second);
			swayCount += entry.srv != nullptr;
			bespokeCount += entry.srv != nullptr && entry.bespoke;
			entries.insert_or_assign(a_vertices.vertexBuffer, std::move(entry));
		};
		for (size_t s = 0; s < shapes.size(); ++s)
			insert(shapes[s].vertices, tree, tree->shapes[s].srv.get());
		// Readable shapes that do not reach the root: remembered as still by their vertex buffer alone
		for (const auto& vertices : readable)
			insert(vertices, nullptr, nullptr);

		const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
		budgetSpentNs += ns;
		buildNs += ns;
	}

	void NearTrees::Clear()
	{
		entries.clear();
		trees.clear();
		swayCount = 0;
		bespokeCount = 0;
		warnedModels.clear();
	}

	void NearTrees::Evict(uint a_frame)
	{
		std::erase_if(entries, [&](const auto& a_entry) {
			const bool old = a_frame - a_entry.second.lastFrame > kEvictFrames;
			if (old)
				Forget(a_entry.second);
			return old;
		});
		// A record lives while a shape of its tree is drawn, and as long again after the last draw; it goes with
		// every entry on it, so a tree never loses one shape's data and keeps another's
		for (const auto& [vertexBuffer, entry] : entries)
			if (entry.tree)
				entry.tree->lastFrame = std::max(entry.tree->lastFrame, entry.lastFrame);
		evicted.clear();
		for (auto it = trees.begin(); it != trees.end();) {
			if (a_frame - it->second->lastFrame > kEvictFrames) {
				evicted.insert(it->first);
				it = trees.erase(it);
			} else {
				++it;
			}
		}
		// Over the budget: the least recently drawn trees go first, never one drawn in the last kBudgetGraceFrames.
		// The bytes of a record are released when its entries go too, so the freed sum is kept here.
		size_t budgetEvicted = 0;
		if (treeBytes > budgetBytes) {
			evictOrder.clear();
			for (const auto& [key, tree] : trees)
				if (tree->bytes > 0 && a_frame - tree->lastFrame > kBudgetGraceFrames)
					evictOrder.emplace_back(tree->lastFrame, key);
			std::sort(evictOrder.begin(), evictOrder.end());
			uint64_t freed = 0;
			for (const auto& [lastFrame, key] : evictOrder) {
				if (treeBytes - freed <= budgetBytes)
					break;
				const auto it = trees.find(key);
				if (it == trees.end())
					continue;
				freed += it->second->bytes;
				trees.erase(it);
				evicted.insert(key);
				++budgetEvicted;
			}
		}
		if (!evicted.empty()) {
			std::erase_if(entries, [&](const auto& a_entry) {
				const bool gone = a_entry.second.tree && evicted.contains(a_entry.second.treeKey);
				if (gone)
					Forget(a_entry.second);
				return gone;
			});
		}
		if (builtTrees != loggedBuiltTrees || reusedShapes != loggedReusedShapes || refusedTrees != loggedRefusedTrees || budgetEvicted > 0) {
			logger::info("[TreeWind] near trees: {} measured (+{}, {:.1f} ms), {} shapes reused (+{}), {} refused for the budget (+{}), {} shapes of which {} sway, {} trees / {:.1f} MB{}",
				builtTrees, builtTrees - loggedBuiltTrees, (buildNs - loggedBuildNs) / 1e6, reusedShapes, reusedShapes - loggedReusedShapes, refusedTrees, refusedTrees - loggedRefusedTrees,
				entries.size(), swayCount, trees.size(), treeBytes / (1024.0 * 1024.0), budgetEvicted > 0 ? std::format(", {} trees freed for the budget", budgetEvicted) : std::string());
			loggedBuiltTrees = builtTrees;
			loggedReusedShapes = reusedShapes;
			loggedRefusedTrees = refusedTrees;
			loggedBuildNs = buildNs;
		}
	}

	ID3D11ShaderResourceView* BendWeights::GetCrownData(RE::BSGeometry* a_geometry, uint a_frame, float& a_weight) const
	{
		a_weight = 1.0f;
		const auto rendererData = a_geometry ? a_geometry->GetGeometryRuntimeData().rendererData : nullptr;
		if (!rendererData || !rendererData->vertexBuffer)
			return nullptr;
		const auto it = entries.find(rendererData->vertexBuffer);
		if (it == entries.end())
			return nullptr;
		const auto& entry = it->second;
		const auto crownSrv = entry.CrownSrv();
		if (entry.crownFade && crownSrv) {
			const float t = std::min(static_cast<float>(a_frame - entry.crownFadeFrame + 1) / kCrownFadeFrames, 1.0f);
			a_weight = t * t * (3.0f - 2.0f * t);
		}
		return crownSrv;
	}

	ID3D11ShaderResourceView* BendWeights::Get(RE::BSGeometry* a_geometry, Kind a_kind, uint a_frame, bool a_treeModSupport)
	{
		VertexView vertices;
		if (!a_geometry || a_kind == Kind::None || !GetVertices(a_geometry, vertices))
			return nullptr;

		const auto firstVertex = vertices(0);
		// Data to keep drawing with while a rebuild waits for its turn
		ID3D11ShaderResourceView* previous = nullptr;
		Entry* staleEntry = nullptr;
		if (auto it = entries.find(vertices.vertexBuffer); it != entries.end()) {
			auto& entry = it->second;
			// Tree-mod crowns rebuild when their block gains trunk cards
			const bool stale = a_treeModSupport && entry.crown && [&] {
				const auto blockIt = blocks.find(entry.blockKey);
				return blockIt != blocks.end() && blockIt->second.generation != entry.generation;
			}();
			const bool sameShape = entry.vertexCount == vertices.count && entry.firstVertex[0] == firstVertex.x && entry.firstVertex[1] == firstVertex.y && entry.firstVertex[2] == firstVertex.z;
			if (!stale && sameShape) {
				entry.lastFrame = a_frame;
				return entry.Srv();
			}
			if (sameShape) {
				// Stale: the old data stays in use until the rebuild
				staleEntry = &entry;
				previous = entry.Srv();
			} else {
				// The vertex buffer address was reused by another shape
				entries.erase(it);
			}
		}

		const uint64_t blockKey = BlockKey(a_geometry);
		auto& block = blocks[blockKey];
		const uint32_t triangleCount = a_geometry->AsTriShape()->GetTrishapeRuntimeData().triangleCount;
		const uint64_t signature = ShapeSignature(blockKey, a_kind, vertices, triangleCount);

		// Same shape with re-created buffers: reuse its bend data instead of scanning the vertices again
		if (const auto sharedIt = shared.find(signature); sharedIt != shared.end()) {
			const auto& record = sharedIt->second;
			const bool crownStale = a_treeModSupport && a_kind == Kind::Crown && record->generation != block.generation;
			if (!crownStale) {
				Entry reused;
				reused.record = record;
				reused.vertexCount = vertices.count;
				reused.firstVertex[0] = firstVertex.x;
				reused.firstVertex[1] = firstVertex.y;
				reused.firstVertex[2] = firstVertex.z;
				reused.lastFrame = a_frame;
				reused.blockKey = blockKey;
				reused.signature = signature;
				reused.generation = record->generation;
				reused.crown = a_kind == Kind::Crown;
				record->lastFrame = a_frame;
				++reusedCount;
				return entries.insert_or_assign(vertices.vertexBuffer, std::move(reused)).first->second.Srv();
			}
			record->lastFrame = a_frame;
			if (!previous)
				previous = record->srv.get();
		}

		// This draw goes out without crown data (no entry of its own to take it from): when the data arrives it
		// fades in instead of switching the crown's shading from one frame to the next
		const auto drawnWithoutCrownData = [&] {
			if (a_kind == Kind::Crown && !staleEntry)
				waiting.insert(vertices.vertexBuffer);
		};

		// A crown that follows trunk cards has nothing to build from until its block's trunks are drawn
		if (a_kind == Kind::Crown && block.cards.empty() && !IsAllQuads(a_geometry) && !IsTreeShapeName(a_geometry) && !(a_treeModSupport && a_geometry->world.scale >= 6.0f)) {
			drawnWithoutCrownData();
			return previous;
		}

		// Out of time this frame, or the block is still gaining trunk cards: draw with the previous data (or still)
		// and build in a later frame
		if (a_frame != budgetFrame) {
			worstFrameNs = std::max(worstFrameNs, budgetSpentNs);
			budgetFrame = a_frame;
			budgetSpentNs = 0;
		}
		const bool settling = a_treeModSupport && a_kind == Kind::Crown && !block.cards.empty() && a_frame - block.changedFrame < kSettleFrames;
		if (settling || budgetSpentNs >= kBuildBudgetNs) {
			++deferredCount;
			if (staleEntry)
				staleEntry->lastFrame = a_frame;
			drawnWithoutCrownData();
			return previous;
		}

		// Past the memory budget nothing new is built: the shape is drawn with what it had (or still) until the
		// sweep frees room, so the data never grows past the budget by more than one build
		if (sharedBytes >= budgetBytes) {
			++refusedCount;
			if (staleEntry)
				staleEntry->lastFrame = a_frame;
			drawnWithoutCrownData();
			return previous;
		}

		ZoneScopedN("TreeWind::BuildBendWeights");
		++builtCount;
		staleBuiltCount += previous != nullptr;
		struct BuildTimer
		{
			BendWeights& owner;
			std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
			~BuildTimer()
			{
				const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
				owner.budgetSpentNs += ns;
				owner.buildNs += ns;
			}
		} buildTimer{ *this };
		const float worldScale = a_geometry->world.scale;
		const bool allQuads = IsAllQuads(a_geometry);
		const auto indices = a_geometry->GetGeometryRuntimeData().rendererData->rawIndexData;

		Entry entry;
		entry.vertexCount = vertices.count;
		entry.firstVertex[0] = firstVertex.x;
		entry.firstVertex[1] = firstVertex.y;
		entry.firstVertex[2] = firstVertex.z;
		entry.lastFrame = a_frame;
		entry.blockKey = blockKey;
		entry.signature = signature;
		entry.crown = a_kind == Kind::Crown;

		// The shared record of this shape: where its block stands, at which LOD level and in which worldspace, for the
		// unload sweep. An older record of the same shape lives on in the entries still drawing from it (and stays
		// counted) until they are replaced or swept.
		const auto makeRecord = [&]() {
			auto record = std::make_shared<Shared>();
			record->generation = block.generation;
			record->lastFrame = a_frame;
			record->blockX = a_geometry->world.translate.x;
			record->blockY = a_geometry->world.translate.y;
			record->worldScale = a_geometry->world.scale;
			record->worldSpace = worldSpace;
			return record;
		};

		// Remembers a shape with nothing to sway, so it is not scanned again
		const auto keepStill = [&]() -> ID3D11ShaderResourceView* {
			entry.generation = block.generation;
			entry.record = makeRecord();
			shared[signature] = entry.record;
			entries.insert_or_assign(vertices.vertexBuffer, std::move(entry));
			return nullptr;
		};

		// Per vertex (R8G8B8A8_UNORM): bend weight (0 base, 1 top), tree random, tree sway scale / kLodScaleRange,
		// distance from the trunk (0 trunk, 1 crown edge). Layout in TreeShapes.h (BendWeights) and TreeWind.hlsli GetOffset.
		std::vector<uint8_t> data(static_cast<size_t>(vertices.count) * 4, 0);
		const auto put = [&](uint32_t a_index, float a_bend, float a_random, float a_scale, float a_radial) {
			data[a_index * 4 + 0] = ToUnorm8(a_bend);
			data[a_index * 4 + 1] = ToUnorm8(a_random);
			data[a_index * 4 + 2] = ToUnorm8(a_scale / kLodScaleRange);
			data[a_index * 4 + 3] = ToUnorm8(a_radial);
		};
		// Crown vertices that follow a trunk card: the first card of their tree, for the crown data
		std::vector<int32_t> vertexTree;

		if (a_kind == Kind::Trunk) {
			// First vertex of each trunk card
			std::vector<uint32_t> cardStarts;
			if (allQuads) {
				for (uint32_t c = 0; c + 3 < vertices.count; c += 4)
					cardStarts.push_back(c);
			} else {
				// Tree mods: find the cards among 3D trunks and stumps. A tree's cards cross at one centre, so a lone
				// tall quad (a pillar face, a post) without a partner is skipped.
				std::vector<uint32_t> candidates;
				for (uint32_t i = 0; i + 3 < vertices.count;) {
					if (IsCardQuad(vertices, i)) {
						candidates.push_back(i);
						i += 4;
					} else {
						++i;
					}
				}
				std::vector<QuadBounds> bounds;
				for (const auto c : candidates)
					bounds.push_back(GetQuadBounds(vertices, c));
				for (size_t a = 0; a < candidates.size(); ++a) {
					for (size_t b = 0; b < candidates.size(); ++b) {
						if (a != b && std::hypot(bounds[a].cx - bounds[b].cx, bounds[a].cy - bounds[b].cy) < 8.0f) {
							cardStarts.push_back(candidates[a]);
							break;
						}
					}
				}
			}

			// Tree mods: trees with a real 3D trunk instead of cards. A tall, narrow piece of a mixed shape is such a
			// trunk: it registers as its tree's card, so the crown follows it, and bends from its foot.
			std::vector<BendWeights::TrunkCard> trunks;
			std::vector<uint32_t> roots;
			std::unordered_map<uint32_t, size_t> trunkOfPiece;
			if (!allQuads && indices) {
				const float crownRadius = kCrownRadius * 4.0f / std::max(worldScale, 0.01f);
				std::vector<bool> onCard(vertices.count, false);
				for (const auto c : cardStarts)
					for (uint32_t k = 0; k < 4; ++k)
						onCard[c + k] = true;
				roots = PieceRoots(indices, triangleCount, vertices.count);
				const uint32_t pieceCount = CompactPieces(roots);
				struct Box
				{
					float sumX = 0.0f, sumY = 0.0f, minX = FLT_MAX, maxX = -FLT_MAX, minY = FLT_MAX, maxY = -FLT_MAX, minZ = FLT_MAX, maxZ = -FLT_MAX;
					int count = 0;
					bool card = false;
				};
				std::vector<Box> boxes(pieceCount);
				for (uint32_t i = 0; i < vertices.count; ++i) {
					const auto p = vertices(i);
					auto& box = boxes[roots[i]];
					box.sumX += p.x;
					box.sumY += p.y;
					box.minX = std::min(box.minX, p.x);
					box.maxX = std::max(box.maxX, p.x);
					box.minY = std::min(box.minY, p.y);
					box.maxY = std::max(box.maxY, p.y);
					box.minZ = std::min(box.minZ, p.z);
					box.maxZ = std::max(box.maxZ, p.z);
					box.card = box.card || onCard[i];
					++box.count;
				}
				for (uint32_t root = 0; root < pieceCount; ++root) {
					const auto& box = boxes[root];
					const float width = std::max(box.maxX - box.minX, box.maxY - box.minY);
					const float height = box.maxZ - box.minZ;
					if (!box.card && box.count >= 6 && width < 0.6f * crownRadius && height > 64.0f && height > 3.0f * width) {
						trunkOfPiece[root] = trunks.size();
						trunks.push_back({ box.sumX / box.count, box.sumY / box.count, box.minZ, box.maxZ });
					}
				}
			}

			// Each trunk registers its tree for the crowns of this block, once per shape
			if (block.trunkShapes.insert(signature).second && (!cardStarts.empty() || !trunks.empty())) {
				// Order-independent, so a block that comes back with the same trunk shapes gets the same generation
				block.generation ^= static_cast<uint32_t>(signature ^ (signature >> 32)) | 1u;
				block.changedFrame = a_frame;
				const auto& world = a_geometry->world;
				for (const auto& trunk : trunks)
					AddCard(block, trunk, worldScale, world, trunk.x, trunk.y);
				for (const auto c : cardStarts) {
					const auto q = GetQuadBounds(vertices, c);
					AddCard(block, { q.cx, q.cy, q.base, q.top }, worldScale, world, q.footX, q.footY);
				}
			}
			if (cardStarts.empty() && trunks.empty())
				return keepStill();

			for (uint32_t i = 0; i < vertices.count && !trunks.empty(); ++i) {
				const auto it = trunkOfPiece.find(roots[i]);
				if (it == trunkOfPiece.end())
					continue;
				const auto& trunk = trunks[it->second];
				const auto* card = NearestCard(block, trunk.x, trunk.y);
				put(i, (vertices(i).z - trunk.base) / std::max(trunk.top - trunk.base, 1.0f), card ? card->random : 0.5f, card ? card->scale : 1.0f, 0.0f);
			}
			// Each card bends from its own foot, with its tree's random and scale
			for (const auto c : cardStarts) {
				const auto q = GetQuadBounds(vertices, c);
				const auto* card = NearestCard(block, q.cx, q.cy);
				const float height = std::max(q.top - q.base, 1.0f);
				for (uint32_t k = 0; k < 4; ++k)
					put(c + k, (vertices(c + k).z - q.base) / height, card ? card->random : 0.5f, card ? card->scale : 1.0f, 0.0f);
			}
		} else if (allQuads) {
			// Billboard crowns (far LOD rings, billboard-only trees): each card bends from its bottom edge,
			// and crossed cards share the random through their rounded centre
			for (uint32_t c = 0; c + 3 < vertices.count; c += 4) {
				const auto q = GetQuadBounds(vertices, c);
				const float height = std::max(q.top - q.base, 1.0f);
				const float rx = std::round(q.cx / 8.0f), ry = std::round(q.cy / 8.0f);
				const float random = Hash01(rx, ry);
				const float scale = TreeScale(height, worldScale, Hash01(ry * 1.37f + 11.0f, rx * 0.71f - 5.0f));
				for (uint32_t k = 0; k < 4; ++k)
					put(c + k, (vertices(c + k).z - q.base) / height, random, scale, 0.6f);
			}
		} else if (IsTreeShapeName(a_geometry)) {
			// A 'treepassthru' shape that is not all quads (external billboards, LOD tree models): each connected
			// piece bends from its own foot, and the pieces of one tree share the random through their rounded centre
			if (!indices)
				return keepStill();
			auto roots = PieceRoots(indices, triangleCount, vertices.count);
			const uint32_t pieceCount = CompactPieces(roots);
			struct Piece
			{
				float sumX = 0.0f, sumY = 0.0f, base = FLT_MAX, top = -FLT_MAX, random = 0.5f, scale = 1.0f;
				int count = 0;
			};
			std::vector<Piece> pieces(pieceCount);
			for (uint32_t i = 0; i < vertices.count; ++i) {
				const auto p = vertices(i);
				auto& piece = pieces[roots[i]];
				piece.sumX += p.x;
				piece.sumY += p.y;
				piece.base = std::min(piece.base, p.z);
				piece.top = std::max(piece.top, p.z);
				++piece.count;
			}
			for (auto& piece : pieces) {
				const float rx = std::round(piece.sumX / piece.count / 8.0f), ry = std::round(piece.sumY / piece.count / 8.0f);
				piece.random = Hash01(rx, ry);
				piece.scale = TreeScale(std::max(piece.top - piece.base, 1.0f), worldScale, Hash01(ry * 1.37f + 11.0f, rx * 0.71f - 5.0f));
			}
			for (uint32_t i = 0; i < vertices.count; ++i) {
				const auto& piece = pieces[roots[i]];
				const float height = piece.top - piece.base;
				// Flat bits stay still
				put(i, height > 8.0f ? (vertices(i).z - piece.base) / height : 0.0f, piece.random, piece.scale, 0.6f);
			}
		} else if (a_treeModSupport) {
			// Tree-mod crowns: each connected piece (a branch, a leaf cluster) follows the ONE trunk card nearest its
			// centre, so a piece never tears between two trees; a piece with no card near stays still (full 3D LOD trees
			// with real trunks). In the far rings tree-mod billboards are not plain quads and have no trunk cards, so
			// each piece there bends from its own foot.
			const bool farRing = worldScale >= 6.0f;
			if (block.cards.empty() && !farRing)
				return nullptr;
			const float crownRadius = kCrownRadius * 4.0f / std::max(worldScale, 0.01f);
			const float maxDistance = 1.2f * crownRadius;
			auto roots = PieceRoots(indices, triangleCount, vertices.count);
			const uint32_t pieceCount = CompactPieces(roots);
			struct Piece
			{
				float sumX = 0.0f, sumY = 0.0f, minX = FLT_MAX, maxX = -FLT_MAX, minY = FLT_MAX, maxY = -FLT_MAX, minZ = FLT_MAX, maxZ = -FLT_MAX;
				int count = 0;
				bool ownFoot = false;
				bool perVertex = false;  // one piece spans several trees: nearest card per vertex
				const BendWeights::TrunkCard* card = nullptr;
			};
			std::vector<Piece> pieces(pieceCount);
			vertexTree.assign(vertices.count, -1);
			for (uint32_t i = 0; i < vertices.count; ++i) {
				const auto p = vertices(i);
				auto& piece = pieces[roots[i]];
				piece.sumX += p.x;
				piece.sumY += p.y;
				piece.minX = std::min(piece.minX, p.x);
				piece.maxX = std::max(piece.maxX, p.x);
				piece.minY = std::min(piece.minY, p.y);
				piece.maxY = std::max(piece.maxY, p.y);
				piece.minZ = std::min(piece.minZ, p.z);
				piece.maxZ = std::max(piece.maxZ, p.z);
				++piece.count;
			}
			uint64_t matched = 0;
			for (auto& piece : pieces) {
				const float cx = piece.sumX / piece.count, cy = piece.sumY / piece.count;
				if (farRing) {
					piece.card = block.cards.empty() ? nullptr : NearestCardWithin(block, cx, cy, maxDistance);
					piece.ownFoot = !piece.card && piece.maxZ - piece.minZ > 8.0f;
					matched += piece.card || piece.ownFoot;
					continue;
				}
				if (piece.maxX - piece.minX > 4.0f * crownRadius || piece.maxY - piece.minY > 4.0f * crownRadius) {
					piece.perVertex = true;
					continue;
				}
				piece.card = NearestCardWithin(block, cx, cy, maxDistance);
				matched += piece.card != nullptr;
			}
			for (uint32_t i = 0; i < vertices.count; ++i) {
				const auto p = vertices(i);
				const auto& piece = pieces[roots[i]];
				if (piece.ownFoot) {
					const float height = std::max(piece.maxZ - piece.minZ, 1.0f);
					const float rx = std::round(piece.sumX / piece.count / 8.0f), ry = std::round(piece.sumY / piece.count / 8.0f);
					put(i, (p.z - piece.minZ) / height, Hash01(rx, ry), TreeScale(height, worldScale, Hash01(ry * 1.37f + 11.0f, rx * 0.71f - 5.0f)), 0.6f);
					continue;
				}
				const auto* card = piece.perVertex ? NearestCardWithin(block, p.x, p.y, maxDistance) : piece.card;
				if (piece.perVertex && card)
					++matched;
				if (card) {
					const float height = std::max(card->top - card->base, 1.0f);
					const float radial = std::sqrt((p.x - card->x) * (p.x - card->x) + (p.y - card->y) * (p.y - card->y)) / crownRadius;
					put(i, (p.z - card->base) / height, card->random, card->scale, radial);
					vertexTree[i] = card->tree;
				}
			}
			if (matched == 0)
				return keepStill();
		} else {
			// 3D crowns follow the nearest trunk card of their block; until those trunks are drawn, the crown stays still
			if (block.cards.empty())
				return nullptr;
			const float crownRadius = kCrownRadius * 4.0f / std::max(worldScale, 0.01f);
			vertexTree.assign(vertices.count, -1);
			for (uint32_t i = 0; i < vertices.count; ++i) {
				const auto p = vertices(i);
				if (const auto* card = NearestCard(block, p.x, p.y)) {
					const float height = std::max(card->top - card->base, 1.0f);
					const float radial = std::sqrt((p.x - card->x) * (p.x - card->x) + (p.y - card->y) * (p.y - card->y)) / crownRadius;
					put(i, (p.z - card->base) / height, card->random, card->scale, radial);
					vertexTree[i] = card->tree;
				} else {
					put(i, 0.6f, Hash01(p.x * 0.01f, p.y * 0.01f), 1.0f, 0.5f);
				}
			}
		}

		auto record = makeRecord();
		if (!CreateRawBuffer(data.data(), data.size(), "TreeWind::BendWeights", record->buffer, record->srv))
			return nullptr;

		// Without it (nothing followed a card, or the buffer could not be created) the crown keeps its plain shading
		if (!vertexTree.empty())
			BuildCrownData(vertices, vertexTree, block, record->crownBuffer, record->crownSrv);

		// Fade the crown data in if the shape has been on screen without it; a rebuild keeps the fade it was in
		const bool waited = waiting.erase(vertices.vertexBuffer) > 0;
		entry.crownFade = record->crownSrv && (waited || (staleEntry && !staleEntry->CrownSrv()));
		entry.crownFadeFrame = a_frame;
		if (staleEntry && staleEntry->CrownSrv() && record->crownSrv) {
			entry.crownFade = staleEntry->crownFade;
			entry.crownFadeFrame = staleEntry->crownFadeFrame;
		}
		entry.generation = block.generation;
		record->bytes = data.size() + (record->crownBuffer ? static_cast<uint64_t>(vertices.count) * 4 : 0);
		record->accounted = &sharedBytes;
		sharedBytes += record->bytes;
		shared[signature] = record;
		entry.record = std::move(record);
		return entries.insert_or_assign(vertices.vertexBuffer, std::move(entry)).first->second.Srv();
	}

	BendWeights::Shared::~Shared()
	{
		if (accounted)
			*accounted -= bytes;
	}

	void BendWeights::SetBudget(uint64_t a_bytes)
	{
		budgetBytes = a_bytes;
	}

	void BendWeights::SetWorldSpace(RE::FormID a_worldSpace)
	{
		worldSpace = a_worldSpace;
	}

	void BendWeights::Clear()
	{
		entries.clear();
		blocks.clear();
		shared.clear();
		waiting.clear();
	}

	void BendWeights::Evict(uint a_frame, const LodBounds& a_bounds)
	{
		std::erase_if(entries, [a_frame](const auto& a_entry) { return a_frame - a_entry.second.lastFrame > kLodEvictFrames; });
		// A shape still waiting for its data is drawn, and puts itself back, every frame
		waiting.clear();
		// A shared record lives while a shape using it is drawn
		for (const auto& [vertexBuffer, entry] : entries)
			if (entry.record)
				entry.record->lastFrame = std::max(entry.record->lastFrame, entry.lastFrame);

		// Records of shapes not drawn for a while, of another worldspace, and of blocks the game has unloaded: a
		// block is loaded while it lies within its LOD ring (fBlockLevelNDistance from the player), and a LOD8 or
		// LOD16 block is replaced by the finer level when the player comes close. One block side of margin either
		// way, and nothing drawn since the last sweep (kResidentFrames) is judged by its position at all, so a
		// block that is still drawn is never freed and rebuilt in turn. In an interior the position says nothing
		// about the LOD: the position tests wait and the idle timer runs on (the LOD is not drawn there).
		evicted.clear();
		size_t unloaded = 0, otherWorld = 0;
		for (auto it = shared.begin(); it != shared.end();) {
			const auto& record = *it->second;
			bool gone = a_frame - record.lastFrame > kLodEvictFrames;
			if (!gone && a_bounds.valid && !a_bounds.interior && a_frame - record.lastFrame > kResidentFrames) {
				if (record.worldSpace != 0 && a_bounds.worldSpace != 0 && record.worldSpace != a_bounds.worldSpace) {
					gone = true;
					++otherWorld;
				} else {
					const int level = record.worldScale < 6.0f ? 0 : (record.worldScale < 12.0f ? 1 : 2);
					const float side = kBlockSideAtScale1 * record.worldScale;
					const float dx = std::max({ record.blockX - a_bounds.playerX, a_bounds.playerX - (record.blockX + side), 0.0f });
					const float dy = std::max({ record.blockY - a_bounds.playerY, a_bounds.playerY - (record.blockY + side), 0.0f });
					const float nearest = std::hypot(dx, dy);
					const float farX = std::max(std::abs(record.blockX - a_bounds.playerX), std::abs(record.blockX + side - a_bounds.playerX));
					const float farY = std::max(std::abs(record.blockY - a_bounds.playerY), std::abs(record.blockY + side - a_bounds.playerY));
					const float farthest = std::hypot(farX, farY);
					gone = nearest > a_bounds.ringDistance[level] + side || (level > 0 && farthest < 0.5f * a_bounds.ringDistance[level - 1]);
					unloaded += gone;
				}
			}
			if (gone) {
				evicted.insert(it->first);
				it = shared.erase(it);
			} else {
				++it;
			}
		}
		unloadedCount += unloaded;
		otherWorldCount += otherWorld;

		// Over the budget: the least recently drawn go first, never what was drawn in the last kBudgetGraceFrames.
		// The bytes of a record are released when its entries go too, so the freed sum is kept here.
		size_t budgetEvicted = 0;
		if (sharedBytes > budgetBytes) {
			evictOrder.clear();
			for (const auto& [signature, record] : shared)
				if (record->bytes > 0 && a_frame - record->lastFrame > kBudgetGraceFrames)
					evictOrder.emplace_back(record->lastFrame, signature);
			std::sort(evictOrder.begin(), evictOrder.end());
			uint64_t freed = 0;
			for (const auto& [lastFrame, signature] : evictOrder) {
				if (sharedBytes - freed <= budgetBytes)
					break;
				const auto it = shared.find(signature);
				if (it == shared.end())
					continue;
				freed += it->second->bytes;
				shared.erase(it);
				evicted.insert(signature);
				++budgetEvicted;
			}
			budgetCount += budgetEvicted;
		}
		// The entries of the records that went (and the stale ones of older records of the same shapes): a view
		// would keep the buffer alive
		if (!evicted.empty())
			std::erase_if(entries, [&](const auto& a_entry) { return evicted.contains(a_entry.second.signature); });

		if (builtCount != loggedBuilt || reusedCount != loggedReused || unloadedCount != loggedUnloaded || budgetCount != loggedBudget || refusedCount != loggedRefused || otherWorld > 0) {
			logger::info("[TreeWind] bend weights: {} built (+{}, {} of them rebuilds, {:.1f} ms, worst frame {:.1f} ms), {} reused (+{}), {} waits (+{}), {} refused for the budget (+{}), {} freed with their blocks (+{}), {} freed with their worldspace (+{}), {} freed for the budget (+{}), {} shapes, {} shared / {:.1f} MB, {} blocks",
				builtCount, builtCount - loggedBuilt, staleBuiltCount - loggedStaleBuilt, (buildNs - loggedBuildNs) / 1e6, worstFrameNs / 1e6,
				reusedCount, reusedCount - loggedReused, deferredCount, deferredCount - loggedDeferred, refusedCount, refusedCount - loggedRefused,
				unloadedCount, unloadedCount - loggedUnloaded, otherWorldCount, otherWorld, budgetCount, budgetCount - loggedBudget,
				entries.size(), shared.size(), sharedBytes / (1024.0 * 1024.0), blocks.size());
			loggedBuilt = builtCount;
			loggedReused = reusedCount;
			loggedDeferred = deferredCount;
			loggedStaleBuilt = staleBuiltCount;
			loggedUnloaded = unloadedCount;
			loggedBudget = budgetCount;
			loggedRefused = refusedCount;
			loggedBuildNs = buildNs;
			worstFrameNs = 0;
		}
		// A block lives only while one of its shapes does, so its cards do not pile up
		liveBlocks.clear();
		for (const auto& [vertexBuffer, entry] : entries)
			liveBlocks.insert(entry.blockKey);
		std::erase_if(blocks, [this](const auto& a_block) { return !liveBlocks.contains(a_block.first); });
	}
}
