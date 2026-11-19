#pragma once

/**
 * @brief Finds the DynDOLOD tree LOD shapes and builds their per-vertex bend data.
 *
 * Object LOD merges many trees into one shape with no per-tree origin, so the bend data comes from
 * the CPU copy of the vertices: trunk cards give each tree's base and top, and each crown vertex
 * takes the nearest trunk card of its LOD block. Billboard-only shapes bend each card or piece
 * from its own foot.
 *
 * Tree-mod support covers 3D LOD built differently from DynDOLOD's own models (Happy Little Trees,
 * Nature of the Wild Lands, Ulvenwald): trunk cards mixed with 3D trunks and stumps in one shape,
 * and crowns without rim or soft lighting.
 */
namespace TreeShapes
{
	enum class Kind
	{
		None,
		Crown,
		Trunk
	};

	/** @brief Classifies a Lighting or Utility pass by its shader flags, geometry type, shape name and layout. */
	Kind Classify(const RE::BSRenderPass* a_pass, bool a_treeModSupport);

	/**
	 * @brief True for object LOD shapes that keep the shader flags of their LOD models: 'passthru', 'treepassthru',
	 * 'objpassthru', with or without a '-LargeRef' suffix. Tree crowns are among them; walls and rocks are 'obj'.
	 */
	bool IsPassthruShape(const RE::BSGeometry* a_geometry);

	class BendWeights
	{
	public:
		/**
		 * @brief Returns the bend data for a shape, built on first sight.
		 * @return nullptr when the shape has nothing to sway or its block's trunks have not been drawn yet.
		 */
		ID3D11ShaderResourceView* Get(RE::BSGeometry* a_geometry, Kind a_kind, uint a_frame, bool a_treeModSupport);

		/** @brief Frees the bend data of shapes not drawn for a while, and the blocks left without shapes. */
		void Evict(uint a_frame);

		/** @brief Frees all bend data, so it is rebuilt with new settings. */
		void Clear();

		size_t Count() const { return entries.size(); }

		struct TrunkCard
		{
			float x, y, base, top;
			float random = 0.0f;  // timing offset and flutter choice, shared by a tree's crossed cards
			float scale = 1.0f;   // sway scale from the tree height
		};

		struct Block
		{
			std::vector<TrunkCard> cards;
			std::unordered_map<int64_t, std::vector<int>> grid;  // cell -> card indices
			std::unordered_set<uint64_t> trunkShapes;            // signatures of the trunk shapes that registered their cards
			uint32_t generation = 0;  // changes when cards are added, so tree-mod crowns built before rebuild
			uint changedFrame = 0;    // frame of the last card change: crowns wait for the block to settle
		};

	private:
		struct Entry
		{
			winrt::com_ptr<ID3D11Buffer> buffer;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;  // nullptr for a shape checked once and left still
			uint32_t vertexCount = 0;
			float firstVertex[3]{};
			uint lastFrame = 0;
			uint64_t blockKey = 0;
			uint64_t signature = 0;
			uint32_t generation = 0;
			bool crown = false;
		};

		// Bend data by shape content. The game re-creates the render data of object LOD shapes (new vertex buffer,
		// same vertices) whenever the loaded cell grid moves, so the data built for the old buffer is reused.
		struct Shared
		{
			winrt::com_ptr<ID3D11Buffer> buffer;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;  // nullptr for a shape left still
			uint32_t generation = 0;
			uint lastFrame = 0;
		};

		std::unordered_map<uint64_t, Block> blocks;      // key = the block's world translate
		std::unordered_map<const void*, Entry> entries;  // key = the shape's vertex buffer
		std::unordered_map<uint64_t, Shared> shared;     // key = the shape's content signature
		uint64_t builtCount = 0, reusedCount = 0, loggedBuilt = 0, loggedReused = 0;
		// Builds scan every vertex of a merged LOD shape on the render thread. When the LOD grid moves, hundreds of
		// shapes arrive in one frame, so only a few milliseconds of builds run per frame and the rest wait
		// (drawn with their previous data, or still) for the following frames.
		uint budgetFrame = 0;
		int64_t budgetSpentNs = 0;
		uint64_t deferredCount = 0, loggedDeferred = 0, staleBuiltCount = 0, loggedStaleBuilt = 0;
		int64_t buildNs = 0, loggedBuildNs = 0, worstFrameNs = 0;
	};
}
