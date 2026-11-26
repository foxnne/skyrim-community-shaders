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
 * A tree whose model is named in a bespoke profile (BespokeProfiles) with a painted wind mask (a sidecar file, the
 * mesh itself is not changed) gets its own motion instead: still trunk, slow branches, leaf flutter, swinging strands.
 *
 * Video memory: every buffer here is an immutable raw buffer (TreeWind.hlsli VertexData, t120) or a typed one
 * (CrownData, t121), owned by a shared record keyed by the shape's content, so the copies the game makes of one
 * mesh share it. Records go when their LOD block leaves the loaded rings (LodBounds), when a shape has not been
 * drawn for a while, and least-recently-drawn first when the total passes the budget the caller hands to Evict.
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
	 * @brief The root node of the loaded tree whose leaves a Lighting pass draws, or nullptr.
	 *
	 * Same tree roots as FindNearTreeRoot, but skinned shapes count too (the trees the engine animates have leaves
	 * like any other), and only leaf shapes: tree-animated, or two-sided, alpha tested and vertex coloured (the static
	 * copies of skinned trees and the leaves of tree mods that drop the tree-anim flag). Trunks and branches are
	 * neither. The lighting shader skips PBR materials by itself.
	 */
	RE::NiNode* FindNearTreeLeafRoot(const RE::BSRenderPass* a_pass);

	/** @brief Levels of each of the two tint seeds (brightness, hue) in a tint code. */
	inline constexpr uint32_t kTintLevels = 11;
	/** @brief The tint code of a tree without one: both seeds in the middle, no tint. */
	inline constexpr uint8_t kNeutralTintCode = (kTintLevels / 2) * kTintLevels + kTintLevels / 2;

	/**
	 * @brief The tint code of the tree standing at a world position: brightness level * kTintLevels + hue level.
	 *
	 * The LOD crown of a tree and its full model both take their tint from this, so the tree keeps it across the swap.
	 * Each seed is a smooth wave over the land with a period of about 400 units: trees a trunk apart differ, while the
	 * few units between a LOD trunk card and the placed tree's origin change it by at most one level.
	 */
	uint8_t GetTintCode(float a_worldX, float a_worldY);

	/**
	 * @brief Where the player stands and how far each object LOD ring reaches, for freeing the data of LOD blocks the
	 * game has unloaded. ringDistance[0] is fBlockLevel0Distance (LOD4 blocks), [1] fBlockLevel1Distance (LOD8),
	 * [2] fBlockMaximumDistance (LOD16). Not valid in interiors: the position there says nothing about the LOD.
	 */
	struct LodBounds
	{
		float playerX = 0.0f, playerY = 0.0f;
		float ringDistance[3] = { 100000.0f, 180500.0f, 327680.0f };
		RE::FormID worldSpace = 0;  // the worldspace the player stands in; records of another one are freed
		bool interior = false;      // the player is in an interior: no LOD is drawn, and the position tests wait
		bool valid = false;
	};

	/**
	 * @brief Motion of one hand-masked tree model. Lengths are in units of the nif root node, angles in radians,
	 * speeds are multiples of the wind clock (TreeWind phase, about 0.6 rad/s in calm air and faster in wind).
	 *
	 * The mask is four bytes per vertex in a sidecar file next to the profile, never in the mesh: the game shades
	 * with any vertex colour a mesh carries (tint from RGB, alpha test from A), whatever its shader flags say.
	 *   R  branch weight, 0 = trunk and main forks (still) .. 1 = twig tips; leaves and strands carry the weight
	 *      of the bark they are attached to
	 *   G  flutter weight where A < 128; on a strand (A >= 128) its free length, 16384 * (G / 255)^2 units
	 *   B  strand swing parameter, 0 at the pin .. 1 at the free tip
	 *   A  bit 7 = hanging strand, bits 0-6 = phase id of the strand or leaf card
	 */
	struct BespokeProfile
	{
		/** @brief The mask of one shape of the model, matched to the loaded shape by vertex count and position hash. */
		struct MaskShape
		{
			std::string name;           // shape name in the nif, for the log only
			uint32_t vertexCount = 0;
			uint64_t positionHash = 0;  // FNV-1a 64 over the 12 position bytes of every vertex, in order
			std::vector<uint8_t> rgba;  // 4 bytes per vertex
		};

		std::string name;
		std::vector<std::string> models;  // lower case, backslashes, relative to the meshes folder
		std::vector<MaskShape> mask;      // empty when the mask file is missing or unreadable

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
	 * File format (every value but Models and Mask is optional and defaults as in BespokeProfile; a model is its path
	 * under the meshes folder as the base record gives it, any case, either slash; Mask is a file in the same folder):
	 *   { "Trees": [ { "Name": "Tree of Life", "Models": [ "landscape/trees/tree of life/tree_of_life.nif" ],
	 *       "Mask": "TreeOfLife.windmask",
	 *       "Branch": { "Amplitude": 110, "Speed": 0.36, "Spread": 2.2, "Gust": 0.35, "Lift": 0.2 },
	 *       "Leaves": { "Amplitude": 10, "Speed": 4 },
	 *       "Ivy": { "Angle": 0.28, "Lean": 0.6, "Speed": 1.2, "Cross": 0.25, "Gust": 1, "ReferenceLength": 2000, "MaxAngle": 1.1 } } ] }
	 *
	 * Mask file (little endian): "TWMK", uint32 version 1, uint32 shape count, then per shape: uint32 name length,
	 * name bytes, uint32 vertex count, uint64 position hash, vertex count x 4 bytes RGBA.
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
	 * Raw buffer layout, mirrored by TreeWind.hlsli GetNearTree:
	 *   bytes 0-15   tree origin xyz in the shape's own space, tree height in the shape's units (four floats)
	 *   16 + 4 i     vertex i as R16G16_UNORM: bend weight (0 base, 1 top), distance from the trunk (0 trunk, 1 crown edge)
	 *
	 * Bespoke trees, one float4 (16 bytes) per element, mirrored by TreeWind.hlsli GetBespokeTree:
	 *   [0] tree origin xyz in the shape's own space, tree height in the shape's units
	 *   [1] shape units per root unit, branch amplitude, branch speed, branch spread
	 *   [2] branch gust, branch lift, leaf amplitude, leaf speed
	 *   [3] ivy angle, ivy lean, ivy speed, ivy cross
	 *   [4] ivy gust, ivy reference length, ivy max angle, unused
	 *   [5 + i] branch weight, flutter weight (or minus the strand's free length in root units), strand swing
	 *           parameter, phase id / 128 of vertex i
	 *
	 * The data of a tree is one record, keyed by the whole tree (its model, its bespoke profile and every shape under
	 * its root with name, counts and place); each shape in it is identified by name, counts, place and the hash of all
	 * its positions. A tree loaded again after its cell unloaded finds its record instead of being measured again, two
	 * copies alive at once share one, and a record is built, kept and freed as a whole, so a tree never sways in parts.
	 * A tree with a bespoke profile is never served from a record: it is measured and mask-matched on its own vertices.
	 */
	class NearTrees
	{
	public:
		/**
		 * @brief Returns the bend data for a shape of the tree under a_root, built for the whole tree on first sight.
		 * @param a_bespoke Set when the data is that of a bespoke tree (other buffer layout, other shader path).
		 * @return nullptr for a tree left still: shorter than a_minHeight, engine-animated (skinned) or unreadable,
		 * or not measured yet because the frame's build time, the memory budget or the shape cap is spent.
		 */
		ID3D11ShaderResourceView* Get(RE::BSGeometry* a_geometry, RE::NiNode* a_root, uint a_frame, float a_minHeight, const BespokeProfiles& a_profiles, bool& a_bespoke);

		/** @brief Video memory the records may hold. At or above it no new tree is measured until the sweep frees room. */
		void SetBudget(uint64_t a_bytes);

		/**
		 * @brief Frees the records of trees not drawn for a while, then the least recently drawn until the buffers fit
		 * the budget. A record drawn in the last few frames is never freed for the budget.
		 */
		void Evict(uint a_frame);

		/** @brief Frees all bend data, so it is rebuilt with new settings. */
		void Clear();

		/** @brief Shapes that sway (shapes left still are not counted). */
		size_t Count() const { return swayCount; }

		/** @brief Shapes that move with a bespoke profile (counted in Count too). */
		size_t BespokeCount() const { return bespokeCount; }

		/** @brief Video memory held by the bend data of loaded trees, every live record counted. */
		uint64_t Bytes() const { return treeBytes; }

	private:
		// The data of one shape of a tree record; srv is null for a shape the measurement left still
		struct Shape
		{
			uint64_t id = 0;  // NearShapeId
			winrt::com_ptr<ID3D11Buffer> buffer;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;
		};

		// One record per tree, built and freed as a whole. Its bytes are counted in treeBytes until the last
		// reference (the map or an entry) lets go of it.
		struct Tree
		{
			std::vector<Shape> shapes;
			uint64_t bytes = 0;
			uint lastFrame = 0;
			bool bespoke = false;
			uint64_t* accounted = nullptr;
			~Tree();
		};

		struct Entry
		{
			std::shared_ptr<Tree> tree;               // null for a shape remembered as still without a record
			ID3D11ShaderResourceView* srv = nullptr;  // owned by tree; nullptr for a shape left still
			uint32_t vertexCount = 0;
			float firstVertex[3]{};
			uint lastFrame = 0;
			uint64_t treeKey = 0;
			bool bespoke = false;  // srv holds bespoke data
		};

		void Build(RE::NiNode* a_root, uint64_t a_treeKey, const BespokeProfile* a_profile, uint a_frame, float a_minHeight);
		void Forget(const Entry& a_entry);
		void DropEntriesOf(uint64_t a_treeKey);

		uint64_t treeBytes = 0;  // declared before the maps: their records subtract from it as they go
		std::unordered_map<const void*, Entry> entries;             // key = the shape's vertex buffer
		std::unordered_map<uint64_t, std::shared_ptr<Tree>> trees;  // key = NearTreeKey
		uint64_t budgetBytes = UINT64_MAX;
		size_t swayCount = 0;
		size_t bespokeCount = 0;
		std::unordered_set<std::string> warnedModels;  // profiles whose mask does not fit the loaded mesh, logged once
		uint budgetFrame = 0;
		int64_t budgetSpentNs = 0;
		uint64_t builtTrees = 0, loggedBuiltTrees = 0, reusedShapes = 0, loggedReusedShapes = 0, refusedTrees = 0, loggedRefusedTrees = 0;
		int64_t buildNs = 0, loggedBuildNs = 0;
		// Scratch for Evict, kept so the sweep allocates nothing
		std::vector<std::pair<uint, uint64_t>> evictOrder;
		std::unordered_set<uint64_t> evicted;
	};

	/**
	 * @brief Bend data for tree LOD shapes.
	 *
	 * Raw buffer layout, one R8G8B8A8_UNORM element (4 bytes) per vertex, mirrored by TreeWind.hlsli GetOffset:
	 *   bend weight (0 base, 1 top), tree random, tree sway scale / kLodScaleRange, distance from the trunk (0 trunk,
	 *   1 crown edge). Random and scale are the same for every vertex of a tree, so their 8 bits move a whole tree a
	 *   little, never one vertex against its neighbours.
	 */
	class BendWeights
	{
	public:
		/** @brief The sway scale is stored as scale / this; TreeScale never exceeds 1.5 at the object LOD scales (4, 8, 16). */
		static constexpr float kLodScaleRange = 2.0f;

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
		 *   w     0 = no data for this vertex, otherwise (1 + the tree's tint code) / 127 (GetTintCode)
		 * @param a_weight Receives how much of the data to apply: 1, or less while the data of a shape that was
		 * drawn without it (its block's trunks not registered yet, or its build waiting for its turn) fades in.
		 * @return nullptr for shapes without it: billboards, trunks, crowns with no trunk card to follow.
		 */
		ID3D11ShaderResourceView* GetCrownData(RE::BSGeometry* a_geometry, uint a_frame, float& a_weight) const;

		/** @brief Video memory the records may hold. At or above it no new shape is built until the sweep frees room. */
		void SetBudget(uint64_t a_bytes);

		/** @brief The worldspace new records belong to; records of another worldspace are freed by Evict. */
		void SetWorldSpace(RE::FormID a_worldSpace);

		/**
		 * @brief Frees the data of LOD blocks outside the loaded rings or in another worldspace (a_bounds, when valid
		 * and not in an interior; never anything drawn since the last sweep), of shapes not drawn for a while, and the
		 * blocks left without shapes; then the least recently drawn data until the buffers fit the budget. Data drawn in
		 * the last few frames is never freed for the budget.
		 */
		void Evict(uint a_frame, const LodBounds& a_bounds);

		/** @brief Frees all bend data, so it is rebuilt with new settings. */
		void Clear();

		size_t Count() const { return entries.size(); }

		/** @brief Video memory held by the bend and crown data of tree LOD, every live record counted. */
		uint64_t Bytes() const { return sharedBytes; }

		struct TrunkCard
		{
			float x, y, base, top;
			float random = 0.0f;  // timing offset and flutter choice, shared by a tree's crossed cards
			float scale = 1.0f;   // sway scale from the tree height
			int tree = -1;        // index of the first card of this tree in its block: crossed cards share it
			uint8_t tint = kNeutralTintCode;  // GetTintCode of the tree's foot in the world, shared by its crossed cards
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
		// Bend data by shape content. The game re-creates the render data of object LOD shapes (new vertex buffer,
		// same vertices) whenever the loaded cell grid moves, so the data built for the old buffer is reused. The
		// record owns the buffers and is counted in sharedBytes until the last reference (the map or an entry) lets
		// go of it, so a record replaced by a rebuild stays counted while a stale entry still draws from it.
		struct Shared
		{
			winrt::com_ptr<ID3D11Buffer> buffer;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;  // nullptr for a shape left still
			uint32_t generation = 0;
			uint lastFrame = 0;
			winrt::com_ptr<ID3D11Buffer> crownBuffer;
			winrt::com_ptr<ID3D11ShaderResourceView> crownSrv;
			uint64_t bytes = 0;
			float blockX = 0.0f, blockY = 0.0f;  // world position of the block's origin
			float worldScale = 4.0f;             // 4, 8 or 16: the LOD level of the block
			RE::FormID worldSpace = 0;           // the worldspace the block belongs to, 0 unknown
			uint64_t* accounted = nullptr;
			~Shared();
		};

		struct Entry
		{
			std::shared_ptr<Shared> record;  // the data drawn with; its srv is nullptr for a shape checked once and left still
			ID3D11ShaderResourceView* Srv() const { return record ? record->srv.get() : nullptr; }
			ID3D11ShaderResourceView* CrownSrv() const { return record ? record->crownSrv.get() : nullptr; }
			uint32_t vertexCount = 0;
			float firstVertex[3]{};
			uint lastFrame = 0;
			uint64_t blockKey = 0;
			uint64_t signature = 0;
			uint32_t generation = 0;
			bool crown = false;
			bool crownFade = false;   // the shape was drawn without crown data before the build
			uint crownFadeFrame = 0;  // ... and this is the frame the data arrived
		};

		uint64_t sharedBytes = 0;  // declared before the maps: their records subtract from it as they go
		std::unordered_map<uint64_t, Block> blocks;                    // key = the block's world translate
		std::unordered_map<const void*, Entry> entries;                // key = the shape's vertex buffer
		std::unordered_map<uint64_t, std::shared_ptr<Shared>> shared;  // key = the shape's content signature
		std::unordered_set<const void*> waiting;                       // vertex buffers of crowns drawn without crown data so far
		uint64_t budgetBytes = UINT64_MAX;
		RE::FormID worldSpace = 0;
		uint64_t builtCount = 0, reusedCount = 0, loggedBuilt = 0, loggedReused = 0;
		uint64_t unloadedCount = 0, loggedUnloaded = 0, budgetCount = 0, loggedBudget = 0, refusedCount = 0, loggedRefused = 0, otherWorldCount = 0;
		// Builds scan every vertex of a merged LOD shape on the render thread. When the LOD grid moves, hundreds of
		// shapes arrive in one frame, so only a few milliseconds of builds run per frame and the rest wait
		// (drawn with their previous data, or still) for the following frames.
		uint budgetFrame = 0;
		int64_t budgetSpentNs = 0;
		uint64_t deferredCount = 0, loggedDeferred = 0, staleBuiltCount = 0, loggedStaleBuilt = 0;
		int64_t buildNs = 0, loggedBuildNs = 0, worstFrameNs = 0;
		// Scratch for Evict, kept so the sweep allocates nothing
		std::vector<std::pair<uint, uint64_t>> evictOrder;
		std::unordered_set<uint64_t> evicted;
		std::unordered_set<uint64_t> liveBlocks;
	};
}
