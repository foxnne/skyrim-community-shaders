#pragma once

/**
 * @brief Finds the DynDOLOD tree LOD shapes and builds their per-vertex bend data.
 *
 * Object LOD merges many trees into one shape with no per-tree origin, so the bend data comes from
 * the CPU copy of the vertices: trunk cards give each tree's base and top, and each crown vertex
 * takes the nearest trunk card of its LOD block. Billboard-only shapes bend each card or piece
 * from its own foot.
 *
 * Crown vertices that follow a trunk card also get crown data for the pixel shader (LOD tree crown
 * shading): where the vertex sits in its tree's crown, measured from the trunk and the crown's own extent.
 *
 * Tree-mod support covers 3D LOD built differently from DynDOLOD's own models (Happy Little Trees,
 * Nature of the Wild Lands, Ulvenwald): trunk cards mixed with 3D trunks and stumps in one shape,
 * and crowns without rim or soft lighting.
 *
 * Loaded (full-model) trees are separate: NearTrees finds the tree a plain BSTriShape belongs to and
 * builds its bend data from the geometry of the whole tree, so leaves and trunk bend together.
 *
 * A tree whose model is named in a bespoke profile (BespokeProfiles) and whose mesh carries a painted wind mask
 * in its vertex colours gets its own motion instead: still trunk, slow branches, leaf flutter, swinging strands.
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
	 * @brief Motion of one hand-masked tree model. Lengths are in units of the nif root node, angles in radians,
	 * speeds are multiples of the wind clock (TreeWind phase, about 0.6 rad/s in calm air and faster in wind).
	 *
	 * The mask is the mesh's vertex colour, read from the CPU copy of the vertices (the shader flag Vertex_Colors
	 * stays off, so the game never shades with it):
	 *   R  branch weight, 0 = trunk and main forks (still) .. 1 = twig tips; leaves and strands carry the weight
	 *      of the bark they are attached to
	 *   G  flutter weight where A < 128; on a strand (A >= 128) its free length, 16384 * (G / 255)^2 units
	 *   B  strand swing parameter, 0 at the pin .. 1 at the free tip
	 *   A  bit 7 = hanging strand, bits 0-6 = phase id of the strand or leaf card
	 */
	struct BespokeProfile
	{
		std::string name;
		std::vector<std::string> models;  // lower case, backslashes, relative to the meshes folder

		float branchAmplitude = 110.0f;  // travel of a branch with weight 1 in full wind
		float branchSpeed = 0.36f;       // slow: one swing takes about 2 pi / (0.6 * 0.36) = 29 s in calm air
		float branchSpread = 2.2f;       // phase difference across the crown, so limbs do not move as one
		float branchGust = 0.35f;        // share of the gust signal the ordinary trees follow
		float branchLift = 0.2f;         // vertical bob as a share of the sway
		float leafAmplitude = 10.0f;     // travel of the free edge of a leaf card
		float leafSpeed = 4.0f;
		float ivyAngle = 0.28f;          // tilt of a strand's tip in full wind, before gusts
		float ivyLean = 0.6f;            // steady downwind lean following the gust signal, as a share of ivyAngle
		float ivySpeed = 1.2f;           // swing speed of a strand of ivyReferenceLength; shorter strands swing faster
		float ivyCross = 0.25f;          // swing across the wind as a share of the swing along it
		float ivyGust = 1.0f;            // how much a gust adds to the tilt, 1 = as much as it adds to tree sway
		float ivyReferenceLength = 2000.0f;
		float ivyMaxAngle = 1.1f;        // the tilt never exceeds this
	};

	/**
	 * @brief The bespoke profiles, read from every *.json in Data\SKSE\Plugins\CommunityShaders\TreeWind.
	 *
	 * File format (every value but Models is optional and defaults as in BespokeProfile; a model is its path under
	 * the meshes folder as the base record gives it, any case, either slash):
	 *   { "Trees": [ { "Name": "Tree of Life", "Models": [ "landscape/trees/tree of life/tree_of_life.nif" ],
	 *       "Branch": { "Amplitude": 110, "Speed": 0.36, "Spread": 2.2, "Gust": 0.35, "Lift": 0.2 },
	 *       "Leaves": { "Amplitude": 10, "Speed": 4 },
	 *       "Ivy": { "Angle": 0.28, "Lean": 0.6, "Speed": 1.2, "Cross": 0.25, "Gust": 1, "ReferenceLength": 2000, "MaxAngle": 1.1 } } ] }
	 */
	class BespokeProfiles
	{
	public:
		/** @brief Reads the profile files again. Bend data built before must be cleared by the caller. */
		void Load();

		/** @brief The profile for the model of the reference that owns a_root, or nullptr. */
		const BespokeProfile* Find(RE::NiNode* a_root) const;

		size_t Count() const { return profiles.size(); }

	private:
		std::vector<BespokeProfile> profiles;
	};

	/**
	 * @brief Bend data for loaded trees, one buffer per mesh shape, shared by every placed copy of the tree.
	 *
	 * The data is measured in the space of the tree root: height above the tree base and distance from the trunk.
	 * Buffer layout (R32G32_FLOAT), mirrored by TreeWind.hlsli GetNearTree:
	 *   [0] tree origin x, y in the shape's own space
	 *   [1] tree origin z in the shape's own space, tree height in the shape's units
	 *   [2 + i] bend weight (0 base, 1 top), distance from the trunk (0 trunk, 1 crown edge) of vertex i
	 *
	 * Bespoke trees (R32G32B32A32_FLOAT), mirrored by TreeWind.hlsli GetBespokeTree:
	 *   [0] tree origin xyz in the shape's own space, tree height in the shape's units
	 *   [1] shape units per root unit, branch amplitude, branch speed, branch spread
	 *   [2] branch gust, branch lift, leaf amplitude, leaf speed
	 *   [3] ivy angle, ivy lean, ivy speed, ivy cross
	 *   [4] ivy gust, ivy reference length, ivy max angle, unused
	 *   [5 + i] branch weight, flutter weight (or minus the strand's free length in root units), strand swing
	 *           parameter, phase id / 128 of vertex i
	 */
	class NearTrees
	{
	public:
		/**
		 * @brief Returns the bend data for a shape of the tree under a_root, built for the whole tree on first sight.
		 * @param a_bespoke Set when the data is that of a bespoke tree (other buffer layout, other shader path).
		 * @return nullptr for a tree left still: shorter than a_minHeight, engine-animated (skinned) or unreadable.
		 */
		ID3D11ShaderResourceView* Get(RE::BSGeometry* a_geometry, RE::NiNode* a_root, uint a_frame, float a_minHeight, const BespokeProfiles& a_profiles, bool& a_bespoke);

		/** @brief Frees the bend data of shapes not drawn for a while. */
		void Evict(uint a_frame);

		/** @brief Frees all bend data, so it is rebuilt with new settings. */
		void Clear();

		/** @brief Shapes that sway (shapes left still are not counted). */
		size_t Count() const { return swayCount; }

		/** @brief Shapes that move with a bespoke profile (counted in Count too). */
		size_t BespokeCount() const { return bespokeCount; }

	private:
		struct Entry
		{
			winrt::com_ptr<ID3D11Buffer> buffer;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;  // nullptr for a shape checked once and left still
			uint32_t vertexCount = 0;
			float firstVertex[3]{};
			uint lastFrame = 0;
			bool bespoke = false;  // srv holds bespoke data
		};

		void Build(RE::NiNode* a_root, uint a_frame, float a_minHeight, const BespokeProfiles& a_profiles);
		void Forget(const Entry& a_entry);

		std::unordered_map<const void*, Entry> entries;  // key = the shape's vertex buffer
		size_t swayCount = 0;
		size_t bespokeCount = 0;
		std::unordered_set<std::string> warnedModels;  // profiles whose mesh has no mask, logged once
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

		/**
		 * @brief Crown data of a shape whose bend data Get has built, for the LOD tree crown shading.
		 *
		 * Buffer layout (R8G8B8A8_SNORM, one element per vertex), mirrored by TreeWind.hlsli GetCrownData:
		 *   x, y  horizontal offset from the tree's trunk, 1 = the widest point of that tree's crown
		 *   z     height inside the tree's crown, 0 lowest crown vertex, 1 top
		 *   w     0 = no data for this vertex, otherwise (1 + 126 * tree random) / 127
		 * @return nullptr for shapes without it: billboards, trunks, crowns with no trunk card to follow.
		 */
		ID3D11ShaderResourceView* GetCrownData(RE::BSGeometry* a_geometry) const;

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
			int tree = -1;        // index of the first card of this tree in its block: crossed cards share it
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
			winrt::com_ptr<ID3D11Buffer> crownBuffer;
			winrt::com_ptr<ID3D11ShaderResourceView> crownSrv;  // nullptr without crown data
		};

		// Bend data by shape content. The game re-creates the render data of object LOD shapes (new vertex buffer,
		// same vertices) whenever the loaded cell grid moves, so the data built for the old buffer is reused.
		struct Shared
		{
			winrt::com_ptr<ID3D11Buffer> buffer;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;  // nullptr for a shape left still
			uint32_t generation = 0;
			uint lastFrame = 0;
			winrt::com_ptr<ID3D11Buffer> crownBuffer;
			winrt::com_ptr<ID3D11ShaderResourceView> crownSrv;
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
