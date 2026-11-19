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
 *
 * Loaded (full-model) trees are separate: NearTrees finds the tree a plain BSTriShape belongs to and
 * builds its bend data from the geometry of the whole tree, so leaves and trunk bend together.
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

	/**
	 * @brief The root node of the loaded tree a pass draws, or nullptr.
	 *
	 * A loaded tree shape is a plain BSTriShape (not the BSSubIndexTriShape of object LOD), not skinned, whose
	 * nearest BSFadeNode ancestor is a BSLeafAnimNode or BSTreeNode: the 3D root of a TREE reference.
	 * Skinned trees are animated by the engine and are left alone.
	 */
	RE::NiNode* FindNearTreeRoot(const RE::BSRenderPass* a_pass);

	/**
	 * @brief Bend data for loaded trees, one buffer per mesh shape, shared by every placed copy of the tree.
	 *
	 * The data is measured in the space of the tree root: height above the tree base and distance from the trunk.
	 * Buffer layout (R32G32_FLOAT), mirrored by TreeWind.hlsli GetNearTree:
	 *   [0] tree origin x, y in the shape's own space
	 *   [1] tree origin z in the shape's own space, tree height in the shape's units
	 *   [2 + i] bend weight (0 base, 1 top), distance from the trunk (0 trunk, 1 crown edge) of vertex i
	 */
	class NearTrees
	{
	public:
		/**
		 * @brief Returns the bend data for a shape of the tree under a_root, built for the whole tree on first sight.
		 * @return nullptr for a tree left still: shorter than a_minHeight, engine-animated (skinned) or unreadable.
		 */
		ID3D11ShaderResourceView* Get(RE::BSGeometry* a_geometry, RE::NiNode* a_root, uint a_frame, float a_minHeight);

		/** @brief Frees the bend data of shapes not drawn for a while. */
		void Evict(uint a_frame);

		/** @brief Frees all bend data, so it is rebuilt with new settings. */
		void Clear();

		/** @brief Shapes that sway (shapes left still are not counted). */
		size_t Count() const { return swayCount; }

	private:
		struct Entry
		{
			winrt::com_ptr<ID3D11Buffer> buffer;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;  // nullptr for a shape checked once and left still
			uint32_t vertexCount = 0;
			float firstVertex[3]{};
			uint lastFrame = 0;
		};

		void Build(RE::NiNode* a_root, uint a_frame, float a_minHeight);

		std::unordered_map<const void*, Entry> entries;  // key = the shape's vertex buffer
		size_t swayCount = 0;
		uint budgetFrame = 0;
		int64_t budgetSpentNs = 0;
		uint64_t builtTrees = 0, loggedBuiltTrees = 0;
		int64_t buildNs = 0, loggedBuildNs = 0;
	};

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
