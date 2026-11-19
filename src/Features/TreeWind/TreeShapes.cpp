#include "TreeShapes.h"

#include <DirectXPackedVector.h>
#include <cfloat>
#include <chrono>
#include <cstring>
#include <filesystem>
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
		// Bend data of a shape not drawn for this many frames is freed
		constexpr uint kEvictFrames = 600;
		// Render-thread time spent building bend data per frame; a build that has started always finishes
		constexpr int64_t kBuildBudgetNs = 1500000;
		// A tree-mod crown waits this many frames after its block last gained trunk cards, so it is built once
		// with all of them instead of once per trunk shape
		constexpr uint kSettleFrames = 20;
		// Render-thread time spent measuring loaded trees per frame; a tree that has started always finishes
		constexpr int64_t kNearBuildBudgetNs = 1500000;
		// The tree root is at most this many nodes above its shapes
		constexpr int kNearTreeDepth = 8;
		// Most loaded tree shapes with bend data at once (8 bytes per vertex each)
		constexpr size_t kMaxNearShapes = 4096;

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
			int32_t colorOffset = -1;  // byte offset of the RGBA8 vertex colour, -1 without one

			RE::NiPoint3 operator()(uint32_t a_index) const
			{
				const auto* f = reinterpret_cast<const float*>(raw + static_cast<size_t>(a_index) * stride);
				return { f[0], f[1], f[2] };
			}

			const uint8_t* Color(uint32_t a_index) const
			{
				return raw + static_cast<size_t>(a_index) * stride + colorOffset;
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
			if (desc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_COLORS)) {
				const uint32_t colorOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_COLOR);
				if (colorOffset >= 16 && colorOffset + 4 <= stride)
					a_out.colorOffset = static_cast<int32_t>(colorOffset);
			}
			return a_out.count > 0;
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

		void AddCard(BendWeights::Block& a_block, BendWeights::TrunkCard a_card, float a_worldScale)
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
			} else {
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
				data[at + 3] = static_cast<int8_t>(1 + std::lround(std::clamp(card.random, 0.0f, 1.0f) * 126.0f));
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
					logger::info("[TreeWind] bespoke profile '{}' from {}: {} model(s), first '{}'", profile.name, file.filename().string(), profile.models.size(), profile.models.front());
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
			VertexView vertices;
			RE::NiTransform toRoot;            // shape space -> tree root space
			std::vector<RE::NiPoint3> points;  // vertices in tree root space
		};

		// Collects the plain tri shapes under a tree root. Returns false when the tree must stay still: a shape is
		// skinned (the engine animates the tree) or its vertices cannot be read (it would stay behind the others).
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
					if (!GetVertices(geometry, shape.vertices)) {
						sways = false;
						continue;
					}
					a_readable.push_back(shape.vertices);
					shape.toRoot = geometry->local;
					bool reachedRoot = false;
					int depth = 0;
					for (auto node = geometry->parent; node && depth < kNearTreeDepth; node = node->parent, ++depth) {
						if (node == a_root) {
							reachedRoot = true;
							break;
						}
						shape.toRoot = node->local * shape.toRoot;
					}
					if (!reachedRoot || !(shape.toRoot.scale > 1e-4f)) {
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

	void NearTrees::Forget(const Entry& a_entry)
	{
		swayCount -= a_entry.srv != nullptr;
		bespokeCount -= a_entry.srv != nullptr && a_entry.bespoke;
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
				return entry.srv.get();
			}
			// The vertex buffer address was reused by another shape
			Forget(entry);
			entries.erase(it);
		}

		// Out of time this frame: the tree stands still until its turn
		if (a_frame != budgetFrame) {
			budgetFrame = a_frame;
			budgetSpentNs = 0;
		}
		// The cap only matters if the game stops sharing one vertex buffer between the copies of a tree
		if (budgetSpentNs >= kNearBuildBudgetNs || swayCount >= kMaxNearShapes)
			return nullptr;

		Build(a_root, a_frame, a_minHeight, a_profiles);

		if (auto it = entries.find(vertices.vertexBuffer); it != entries.end()) {
			a_bespoke = it->second.bespoke;
			return it->second.srv.get();
		}

		// Not part of the tree as measured (for example a skinned shape): remembered as still
		Entry still;
		still.vertexCount = vertices.count;
		still.firstVertex[0] = firstVertex.x;
		still.firstVertex[1] = firstVertex.y;
		still.firstVertex[2] = firstVertex.z;
		still.lastFrame = a_frame;
		entries.insert_or_assign(vertices.vertexBuffer, std::move(still));
		return nullptr;
	}

	void NearTrees::Build(RE::NiNode* a_root, uint a_frame, float a_minHeight, const BespokeProfiles& a_profiles)
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

		// A bespoke tree needs its painted mask on every shape; without it (the original mesh) it sways like any other tree
		const BespokeProfile* profile = sways ? a_profiles.Find(a_root) : nullptr;
		if (profile) {
			const bool masked = std::all_of(shapes.begin(), shapes.end(), [](const NearShape& a_shape) { return a_shape.vertices.colorOffset >= 0; });
			if (!masked) {
				if (warnedModels.insert(profile->name).second)
					logger::warn("[TreeWind] bespoke profile '{}': not every shape of the loaded mesh has vertex colours, so the wind mask is missing; using the ordinary nearby-tree sway", profile->name);
				profile = nullptr;
			}
		}
		const bool bespoke = profile != nullptr;

		const auto insert = [&](const VertexView& a_vertices, winrt::com_ptr<ID3D11Buffer> a_buffer, winrt::com_ptr<ID3D11ShaderResourceView> a_srv) {
			Entry entry;
			entry.bespoke = bespoke && a_srv;
			entry.buffer = std::move(a_buffer);
			entry.srv = std::move(a_srv);
			entry.vertexCount = a_vertices.count;
			const auto first = a_vertices(0);
			entry.firstVertex[0] = first.x;
			entry.firstVertex[1] = first.y;
			entry.firstVertex[2] = first.z;
			entry.lastFrame = a_frame;
			if (const auto it = entries.find(a_vertices.vertexBuffer); it != entries.end())
				Forget(it->second);
			swayCount += entry.srv != nullptr;
			bespokeCount += entry.srv != nullptr && entry.bespoke;
			entries.insert_or_assign(a_vertices.vertexBuffer, std::move(entry));
		};

		if (!sways) {
			// Remembered as still, so the tree is not measured again
			for (const auto& vertices : readable)
				insert(vertices, nullptr, nullptr);
		} else {
			struct Built
			{
				winrt::com_ptr<ID3D11Buffer> buffer;
				winrt::com_ptr<ID3D11ShaderResourceView> srv;
			};
			std::vector<Built> built(shapes.size());
			bool created = true;
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
					const uint8_t* color = shape.vertices.Color(i);
					float* out = &data[static_cast<size_t>(i + kHeader) * 4];
					const bool strand = color[3] >= 128;
					const float g = color[1] / 255.0f;
					out[0] = color[0] / 255.0f;
					// A strand stores its free length (negative, in root units) where the others store their flutter weight
					out[1] = strand ? -std::max(16384.0f * g * g, 1.0f) : g;
					out[2] = strand ? color[2] / 255.0f : 0.0f;
					out[3] = (color[3] & 127) / 128.0f;
				}

				D3D11_BUFFER_DESC bufferDesc{};
				bufferDesc.ByteWidth = static_cast<UINT>(data.size() * sizeof(float));
				bufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
				bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				const D3D11_SUBRESOURCE_DATA initData{ data.data(), 0, 0 };
				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
				srvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
				srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
				srvDesc.Buffer.NumElements = elements;
				created = SUCCEEDED(globals::d3d::device->CreateBuffer(&bufferDesc, &initData, built[s].buffer.put())) &&
				          SUCCEEDED(globals::d3d::device->CreateShaderResourceView(built[s].buffer.get(), &srvDesc, built[s].srv.put()));
				if (created) {
					Util::SetResourceName(built[s].buffer.get(), "TreeWind::BespokeTree");
					Util::SetResourceName(built[s].srv.get(), "TreeWind::BespokeTree SRV");
				}
			}
			if (bespoke)
				logger::info("[TreeWind] bespoke tree '{}': {} shapes, height {:.0f}, trunk axis ({:.0f}, {:.0f}){}", profile->name, shapes.size(), height, axisX, axisY, created ? "" : ", buffers failed");
			for (size_t s = 0; s < shapes.size() && created && !bespoke; ++s) {
				const auto& shape = shapes[s];
				const uint32_t elements = shape.vertices.count + 2;
				std::vector<float> data(static_cast<size_t>(elements) * 2);
				const auto origin = shape.toRoot.Invert() * RE::NiPoint3{ axisX, axisY, base };
				data[0] = origin.x;
				data[1] = origin.y;
				data[2] = origin.z;
				data[3] = height / shape.toRoot.scale;
				for (uint32_t i = 0; i < shape.vertices.count; ++i) {
					const auto& p = shape.points[i];
					data[(i + 2) * 2 + 0] = std::clamp((p.z - base) / height, 0.0f, 1.0f);
					data[(i + 2) * 2 + 1] = std::clamp(std::hypot(p.x - axisX, p.y - axisY) / crownRadius, 0.0f, 1.0f);
				}

				D3D11_BUFFER_DESC bufferDesc{};
				bufferDesc.ByteWidth = static_cast<UINT>(data.size() * sizeof(float));
				bufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
				bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				const D3D11_SUBRESOURCE_DATA initData{ data.data(), 0, 0 };
				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
				srvDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
				srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
				srvDesc.Buffer.NumElements = elements;
				created = SUCCEEDED(globals::d3d::device->CreateBuffer(&bufferDesc, &initData, built[s].buffer.put())) &&
				          SUCCEEDED(globals::d3d::device->CreateShaderResourceView(built[s].buffer.get(), &srvDesc, built[s].srv.put()));
				if (created) {
					Util::SetResourceName(built[s].buffer.get(), "TreeWind::NearTree");
					Util::SetResourceName(built[s].srv.get(), "TreeWind::NearTree SRV");
				}
			}
			// All shapes of the tree or none, so the tree never moves in parts
			for (size_t s = 0; s < shapes.size(); ++s)
				insert(shapes[s].vertices, created ? built[s].buffer : nullptr, created ? built[s].srv : nullptr);
		}

		const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
		budgetSpentNs += ns;
		buildNs += ns;
	}

	void NearTrees::Clear()
	{
		entries.clear();
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
		if (builtTrees != loggedBuiltTrees) {
			logger::info("[TreeWind] near trees: {} measured (+{}, {:.1f} ms), {} shapes of which {} sway",
				builtTrees, builtTrees - loggedBuiltTrees, (buildNs - loggedBuildNs) / 1e6, entries.size(), swayCount);
			loggedBuiltTrees = builtTrees;
			loggedBuildNs = buildNs;
		}
	}

	ID3D11ShaderResourceView* BendWeights::GetCrownData(RE::BSGeometry* a_geometry) const
	{
		const auto rendererData = a_geometry ? a_geometry->GetGeometryRuntimeData().rendererData : nullptr;
		if (!rendererData || !rendererData->vertexBuffer)
			return nullptr;
		const auto it = entries.find(rendererData->vertexBuffer);
		return it != entries.end() ? it->second.crownSrv.get() : nullptr;
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
				return entry.srv.get();
			}
			if (sameShape) {
				// Stale: the old data stays in use until the rebuild
				staleEntry = &entry;
				previous = entry.srv.get();
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
			auto& record = sharedIt->second;
			const bool crownStale = a_treeModSupport && a_kind == Kind::Crown && record.generation != block.generation;
			if (!crownStale) {
				Entry reused;
				reused.buffer = record.buffer;
				reused.srv = record.srv;
				reused.crownBuffer = record.crownBuffer;
				reused.crownSrv = record.crownSrv;
				reused.vertexCount = vertices.count;
				reused.firstVertex[0] = firstVertex.x;
				reused.firstVertex[1] = firstVertex.y;
				reused.firstVertex[2] = firstVertex.z;
				reused.lastFrame = a_frame;
				reused.blockKey = blockKey;
				reused.signature = signature;
				reused.generation = record.generation;
				reused.crown = a_kind == Kind::Crown;
				record.lastFrame = a_frame;
				++reusedCount;
				return entries.insert_or_assign(vertices.vertexBuffer, std::move(reused)).first->second.srv.get();
			}
			record.lastFrame = a_frame;
			if (!previous)
				previous = record.srv.get();
		}

		// A crown that follows trunk cards has nothing to build from until its block's trunks are drawn
		if (a_kind == Kind::Crown && block.cards.empty() && !IsAllQuads(a_geometry) && !IsTreeShapeName(a_geometry) && !(a_treeModSupport && a_geometry->world.scale >= 6.0f))
			return previous;

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

		// Remembers a shape with nothing to sway, so it is not scanned again
		const auto keepStill = [&]() -> ID3D11ShaderResourceView* {
			entry.generation = block.generation;
			shared.insert_or_assign(signature, Shared{ nullptr, nullptr, block.generation, a_frame });
			entries.insert_or_assign(vertices.vertexBuffer, std::move(entry));
			return nullptr;
		};

		// Per vertex: bend weight (0 base, 1 top), tree random, tree sway scale, distance from the trunk (0 trunk, 1 crown edge)
		std::vector<uint16_t> data(static_cast<size_t>(vertices.count) * 4, 0);
		const auto put = [&](uint32_t a_index, float a_bend, float a_random, float a_scale, float a_radial) {
			data[a_index * 4 + 0] = DirectX::PackedVector::XMConvertFloatToHalf(std::clamp(a_bend, 0.0f, 1.0f));
			data[a_index * 4 + 1] = DirectX::PackedVector::XMConvertFloatToHalf(a_random);
			data[a_index * 4 + 2] = DirectX::PackedVector::XMConvertFloatToHalf(a_scale);
			data[a_index * 4 + 3] = DirectX::PackedVector::XMConvertFloatToHalf(std::clamp(a_radial, 0.0f, 1.0f));
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
				for (const auto& trunk : trunks)
					AddCard(block, trunk, worldScale);
				for (const auto c : cardStarts) {
					const auto q = GetQuadBounds(vertices, c);
					AddCard(block, { q.cx, q.cy, q.base, q.top }, worldScale);
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

		D3D11_BUFFER_DESC bufferDesc{};
		bufferDesc.ByteWidth = static_cast<UINT>(data.size() * sizeof(uint16_t));
		bufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
		bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		const D3D11_SUBRESOURCE_DATA initData{ data.data(), 0, 0 };
		if (FAILED(globals::d3d::device->CreateBuffer(&bufferDesc, &initData, entry.buffer.put())))
			return nullptr;
		Util::SetResourceName(entry.buffer.get(), "TreeWind::BendWeights");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.NumElements = vertices.count;
		if (FAILED(globals::d3d::device->CreateShaderResourceView(entry.buffer.get(), &srvDesc, entry.srv.put())))
			return nullptr;
		Util::SetResourceName(entry.srv.get(), "TreeWind::BendWeights SRV");

		// Without it (nothing followed a card, or the buffer could not be created) the crown keeps its plain shading
		if (!vertexTree.empty())
			BuildCrownData(vertices, vertexTree, block, entry.crownBuffer, entry.crownSrv);

		entry.generation = block.generation;
		Shared record{ entry.buffer, entry.srv, block.generation, a_frame };
		record.crownBuffer = entry.crownBuffer;
		record.crownSrv = entry.crownSrv;
		shared.insert_or_assign(signature, std::move(record));
		return entries.insert_or_assign(vertices.vertexBuffer, std::move(entry)).first->second.srv.get();
	}

	void BendWeights::Clear()
	{
		entries.clear();
		blocks.clear();
		shared.clear();
	}

	void BendWeights::Evict(uint a_frame)
	{
		std::erase_if(entries, [a_frame](const auto& a_entry) { return a_frame - a_entry.second.lastFrame > kEvictFrames; });
		// A shared record lives while a shape using it is drawn
		for (const auto& [vertexBuffer, entry] : entries)
			if (const auto it = shared.find(entry.signature); it != shared.end())
				it->second.lastFrame = std::max(it->second.lastFrame, entry.lastFrame);
		std::erase_if(shared, [a_frame](const auto& a_record) { return a_frame - a_record.second.lastFrame > kEvictFrames; });
		if (builtCount != loggedBuilt || reusedCount != loggedReused) {
			logger::info("[TreeWind] bend weights: {} built (+{}, {} of them rebuilds, {:.1f} ms, worst frame {:.1f} ms), {} reused (+{}), {} waits (+{}), {} shapes, {} shared, {} blocks",
				builtCount, builtCount - loggedBuilt, staleBuiltCount - loggedStaleBuilt, (buildNs - loggedBuildNs) / 1e6, worstFrameNs / 1e6,
				reusedCount, reusedCount - loggedReused, deferredCount, deferredCount - loggedDeferred, entries.size(), shared.size(), blocks.size());
			loggedBuilt = builtCount;
			loggedReused = reusedCount;
			loggedDeferred = deferredCount;
			loggedStaleBuilt = staleBuiltCount;
			loggedBuildNs = buildNs;
			worstFrameNs = 0;
		}
		// A block lives only while one of its shapes does, so its cards do not pile up
		std::unordered_set<uint64_t> liveBlocks;
		for (const auto& [vertexBuffer, entry] : entries)
			liveBlocks.insert(entry.blockKey);
		std::erase_if(blocks, [&liveBlocks](const auto& a_block) { return !liveBlocks.contains(a_block.first); });
	}
}
