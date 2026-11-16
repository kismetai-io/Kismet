// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Ported from ibrews/blueprint-auto-layout (MIT License).
// Copyright (c) 2026 Alex Coulombe.

#pragma once

#include "CoreMinimal.h"

// Int-indexed, editor-free layered-graph solver. Callers add vertices and
// edges (with pin Y-offsets), seed the rank/order from their own ranking pass,
// then Solve() to read back per-vertex coordinates.
//
// Pipeline: InsertDummies -> AssignX -> AssignYBrandesKopf.
// Callers MUST supply seed ranks (SetSeedRank) and optionally seed order
// (SetSeedOrder). Ranking and crossing-minimization sweeps are intentionally
// skipped: the editor adapter owns those; this core only places coordinates.

/** Tuning knobs. Defaults mirror the editor's HorizontalSpacing / VerticalSpacing. */
struct KISMETLAYOUTCORE_API FLayeredConfig
{
	float RankSpacing  = 300.f;  // horizontal gap between rank columns (added to rank's max node width)
	float NodeSpacingY = 80.f;   // vertical gap between nodes in the same rank
	float DummyWidth   = 1.f;    // virtual width of a dummy (long-edge waypoint) node
	float DummyHeight  = 1.f;    // virtual height of a dummy node

	// When true, AssignYBrandesKopf runs the canonical 4-direction balanced pass
	// (up/down × left/right, median-of-two-middle averaging) instead of the single
	// upper-left pass. Straighter, better-centered wires on mixed-height / forked
	// graphs at ~4x the Y-assignment cost. Default false (single pass) for parity
	// with the original port; flip on once validated in-editor.
	bool  bFourDirBalance = false;
};

/**
 * Int-indexed layered-graph solver (Sugiyama / Brandes-Köpf).
 *
 * Usage:
 *   FLayeredLayout L;
 *   int32 A = L.AddVertex(200, 80);
 *   int32 B = L.AddVertex(200, 80);
 *   L.AddEdge(A, B, 40.f, 40.f, true);
 *   L.SetSeedRank(A, 0); L.SetSeedRank(B, 1);
 *   L.Solve();
 *   FVector2D PosA = L.GetPosition(A);
 */
class KISMETLAYOUTCORE_API FLayeredLayout
{
public:
	explicit FLayeredLayout(const FLayeredConfig& InConfig = FLayeredConfig()) : Config(InConfig) {}

	/** Replace the tuning config. Must be called before Solve(). */
	void SetConfig(const FLayeredConfig& InConfig) { Config = InConfig; }

	/** Add a real node; returns its stable vertex id. */
	int32 AddVertex(float Width, float Height);

	/** Add a directed edge between two previously-added vertex ids. */
	void AddEdge(int32 From, int32 To, float PortFromY = -1.f, float PortToY = -1.f, bool bExec = false);

	/**
	 * Pre-assign a vertex's rank (column). When ANY seed rank is set, the core
	 * skips longest-path ranking and uses these (normalized so min rank == 0).
	 * Real-vertex ranks are not recomputed.
	 */
	void SetSeedRank(int32 Vertex, int32 Rank);

	/**
	 * Pre-assign a vertex's within-rank order (top=0). Used for real nodes only.
	 * Dummies get interpolated order between their endpoint's seeds.
	 * No median crossing-minimization sweeps are run on real nodes.
	 */
	void SetSeedOrder(int32 Vertex, int32 Order);

	/** Run the full pipeline. Call once after all AddVertex/AddEdge/SetSeed calls. */
	void Solve();

	/** Return the top-left position assigned to vertex Id. Only valid after Solve(). */
	FVector2D GetPosition(int32 Vertex) const;

	/**
	 * For an original edge index (order of AddEdge calls), the ordered list of
	 * dummy vertex ids inserted along it (empty for short / same-rank edges).
	 * Useful for future knot materialization; callers may ignore it.
	 */
	const TArray<int32>& GetEdgeChain(int32 EdgeIndex) const;

private:
	// -------------------------------------------------------------------------
	// Internal data
	// -------------------------------------------------------------------------

	struct FCoreVertex
	{
		float  W         = 1.f;
		float  H         = 1.f;
		int32  Rank      = 0;
		int32  Order     = 0;
		bool   bDummy    = false;
		bool   bSeedRank  = false;
		bool   bSeedOrder = false;
		float  X         = 0.f;
		float  Y         = 0.f;
	};

	struct FCoreEdge
	{
		int32  From      = -1;
		int32  To        = -1;
		float  PortFromY = -1.f;
		float  PortToY   = -1.f;
		bool   bExec     = false;
	};

	// One proper-segment neighbor of a vertex in an adjacent rank, with the
	// center-Y offset that aligning to it implies (centerY(V) = centerY(Id) + Off).
	struct FNbr { int32 Id; float Off; bool bExec; };

	FLayeredConfig          Config;
	TArray<FCoreVertex>     Vertices;
	TArray<FCoreEdge>       Edges;
	TArray<TArray<int32>>   EdgeChains;  // per-original-edge dummy chains
	int32                   OriginalEdgeCount = 0;
	int32                   NumRanks          = 0;
	bool                    bAnySeedRank      = false;

	// -------------------------------------------------------------------------
	// Pipeline stages
	// -------------------------------------------------------------------------

	/** Normalize seed ranks to min=0, compute NumRanks, assign default ranks if no seeds. */
	void AssignRanks();

	/** Assign within-rank Order to real nodes (uses seed if present, else default), then to dummies. */
	void AssignOrders();

	/** Split every edge spanning >1 rank into a unit-width dummy chain. */
	void InsertDummies();

	/** X = Rank * RankSpacing. */
	void AssignX();

	/**
	 * Port-aware Brandes-Köpf Y assignment.
	 * RankOrders[r] = vertex ids in rank r, sorted by Order.
	 * Dispatches to the single upper-left pass or, when Config.bFourDirBalance,
	 * to the balanced 4-direction variant.
	 */
	void AssignYBrandesKopf(const TArray<TArray<int32>>& RankOrders);

	/**
	 * Shared BK setup: within-rank positions, per-vertex upper/lower neighbor
	 * lists (with center-Y offsets), and the Type-1 conflict set (marked once on
	 * the original ordering — valid for every directional pass).
	 */
	void ComputeBKAdjacency(const TArray<TArray<int32>>& RankOrders,
	                        TArray<int32>& OutPos,
	                        TArray<TArray<FNbr>>& OutUp,
	                        TArray<TArray<FNbr>>& OutDown,
	                        TSet<int64>& OutConflicts) const;

	/** Canonical 4-direction balanced Y assignment (Config.bFourDirBalance). */
	void AssignYBrandesKopf4Dir(const TArray<TArray<int32>>& RankOrders);

	/**
	 * One directional BK pass. bToUpper aligns to rank-1 neighbors (scanning ranks
	 * forward) vs rank+1 (scanning backward); bRightBias reverses within-rank order
	 * and negates the result so a single left-bias compaction serves both biases.
	 * Returns per-vertex center-Y (NOT top-left).
	 */
	TArray<float> RunBKPass(const TArray<TArray<int32>>& RankOrders,
	                        const TArray<TArray<FNbr>>& Up,
	                        const TArray<TArray<FNbr>>& Down,
	                        const TSet<int64>& Conflicts,
	                        bool bToUpper, bool bRightBias) const;

	// -------------------------------------------------------------------------
	// Helpers
	// -------------------------------------------------------------------------

	/** Build [rank] -> sorted vertex id list from current Rank/Order state. */
	TArray<TArray<int32>> BuildRankOrders() const;

	float PortFromYOf(const FCoreEdge& E) const;
	float PortToYOf(const FCoreEdge& E) const;
};
