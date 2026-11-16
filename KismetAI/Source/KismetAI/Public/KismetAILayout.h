// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "CoreMinimal.h"
#include "KismetLayeredLayout.h"

class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

struct FNodeSize
{
	int32 Width = 220;
	int32 Height = 74;
};

// Which schema family a graph belongs to, selecting how layout treats the
// "spine" — the backbone of nodes the layout straightens and keeps in a clean
// horizontal corridor, with everything else pushed below as data helpers.
//   K2Exec       — Blueprint event/function graphs; spine = exec-pin nodes.
//   AnimPose     — Animation Blueprint AnimGraph; spine = pose-pin nodes.
//   MaterialData — Material graphs; pure data DAG into the Result node, no spine.
enum class EGraphLayoutProfile : uint8
{
	K2Exec,
	AnimPose,
	MaterialData,
};

/**
 * Deterministic auto-layout for Blueprint graph nodes.
 * Works headlessly -- no Slate widgets required.
 *
 * Two entry points with very different contracts:
 *  - PlaceNewNodes: containment placement. Positions ONLY the given new
 *    nodes; every pre-existing node and comment box is an immovable
 *    obstacle. This is what every mutating tool calls implicitly.
 *  - FormatNodes: explicit full reformat (layout_graph tool). Expands seeds
 *    to their connected components, runs a modified Sugiyama
 *    (hierarchical/layered) layout, then reflows comment boxes around their
 *    pre-format members.
 */
class KISMETAI_API FKismetAILayout
{
public:
	// Containment placement: position only NewNodes, anchored beside the
	// existing node they connect to (or beside the graph's content when the
	// cluster is an island). Never repositions a pre-existing node.
	//
	// OutIslandNodes (optional): receives the new nodes that had NO connection
	// to a pre-existing node and were therefore placed as an island (off to the
	// right of the graph's content). These are the candidates for
	// reposition-on-first-wire: a later edit_blueprint connect can pull them
	// next to the node they end up feeding.
	//
	// bForceInternalLayout: run the hierarchical layout over each cluster
	// unconditionally instead of only when its new nodes overlap. The default
	// (false) preserves authored relative placement from write_blueprint T3D;
	// the reposition-on-first-wire path passes true because the cluster's
	// current positions are throwaway island-parking coordinates, not authored.
	static void PlaceNewNodes(UEdGraph* Graph, const TSet<UEdGraphNode*>& NewNodes,
		TSet<UEdGraphNode*>* OutIslandNodes = nullptr, bool bForceInternalLayout = false);

	// Explicit full formatting. Empty SeedNodes formats the whole graph.
	// Deterministic: same graph topology -> same positions.
	// bReroute=false (default) — positions only.
	// bReroute=true — after coordinates are assigned, collapses existing
	//   UK2Node_Knot nodes in each component and inserts new ones where the
	//   solver produced dummy waypoints (long-edge knot materialization).
	// bGroup=false (default) — no comment creation.
	// bGroup=true — wraps each event subtree in a keyword-colored comment box.
	static void FormatNodes(UEdGraph* Graph, const TSet<UEdGraphNode*>& SeedNodes,
		bool bReroute = false, bool bGroup = false);

	// Opt-in: create keyword-colored comment boxes around each event subtree in
	// Subgraph. Only called by FormatNodes when bGroup=true.
	static void GroupEventSubtrees(UEdGraph* Graph, const TSet<UEdGraphNode*>& Subgraph);

	// Estimate pixel dimensions of a node without Slate rendering.
	// Uses pin count and node class heuristics.
	static FNodeSize EstimateNodeSize(const UEdGraphNode* Node);

	// Layout profile selection + spine queries (pure; safe to call from tests).
	// SelectProfile inspects the graph's schema (anim -> AnimPose, material ->
	// MaterialData, else K2Exec).
	static EGraphLayoutProfile SelectProfile(const UEdGraph* Graph);
	// True when the profile has a spine concept (K2Exec/AnimPose). MaterialData
	// is a pure data DAG — no spine, so the pull-back and corridor passes are
	// skipped (they would otherwise collapse/misplace an all-data graph).
	static bool ProfileHasSpine(EGraphLayoutProfile Profile);
	// True when Node is on the spine for this profile (exec node / pose node).
	static bool IsSpineNode(const UEdGraphNode* Node, EGraphLayoutProfile Profile);
	// True when Pin is a spine pin for this profile (exec pin / pose pin). Drives
	// the solver's straighten-the-spine preference (AddEdge bExec).
	static bool IsSpinePin(const UEdGraphPin* Pin, EGraphLayoutProfile Profile);

	// Layout constants
	static constexpr int32 HorizontalSpacing = 300;
	static constexpr int32 VerticalSpacing = 80;
	// Enable the balanced 4-direction Brandes-Köpf Y pass in the core solver
	// (straighter, better-centered wires on forked graphs). Off for now: it keeps
	// mixed-height exec chains straight to ~1px but not the exact top-Y equality
	// the LinearChain/MixedHeight tests require — needs that exactness pass before
	// enabling. The corridor fix below is independent of this.
	static constexpr bool bUseFourDirBalance = false;
	// Legacy crude pure-node corridor: shove ALL off-spine nodes below the lowest
	// spine node as a group. Disabled — it overrides the pin-aware solver's
	// per-node alignment and pushes pure feeders far from the nodes they feed
	// (BPAL aligns each provider to its consumer's pin instead). Kept behind a
	// flag for comparison only.
	static constexpr bool bUsePureNodeCorridor = false;
	static constexpr int32 SubgraphMargin = 200;
	// Gap between an anchor node and a newly placed neighbor.
	static constexpr int32 PlacementGapX = 100;
	// Padding when growing/reflowing a comment box around its members.
	static constexpr int32 CommentPad = 40;
	static constexpr int32 CommentTitlePad = 60;
	// Final positions snap to this grid so alignment reads as intentional rather
	// than landing on arbitrary pixels. Matches the Blueprint editor's grid.
	static constexpr int32 GridSize = 16;

private:
	// Bundles the solver, the node-to-vertex-id map, and a parallel array of
	// source/target pin pairs (one per AddEdge call, in call order).
	// FLayeredLayout has no copy/assignment guarantees; always fill in-place
	// via an Out reference — never copy or move FSolverInputs.
	struct FSolverInputs
	{
		FLayeredLayout Solver;
		TMap<UEdGraphNode*, int32> NodeToId;
		// EdgePins[e] = (SourcePin, TargetPin) for the e-th AddEdge call.
		TArray<TPair<UEdGraphPin*, UEdGraphPin*>> EdgePins;
	};
	struct FLayoutGraph
	{
		TArray<UEdGraphNode*> Nodes;
		TMap<UEdGraphNode*, TArray<UEdGraphNode*>> ForwardEdges;   // output -> input
		TMap<UEdGraphNode*, TArray<UEdGraphNode*>> BackwardEdges;  // input -> output
		TMap<UEdGraphNode*, int32> LayerAssignment;
		TMap<UEdGraphNode*, int32> OrderInLayer;
		TMap<UEdGraphNode*, FNodeSize> NodeSizes;
	};

	struct FRect
	{
		int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
		bool Intersects(const FRect& Other) const
		{
			return (MinX < Other.MaxX) && (MaxX > Other.MinX) &&
			       (MinY < Other.MaxY) && (MaxY > Other.MinY);
		}
	};

	// Populate Out with the solver vertices/seeds/edges derived from LG.
	// Records EdgePins in the same iteration that calls Solver.AddEdge so that
	// edge index ↔ pin pair stays aligned.
	static void BuildSolverInputs(const FLayoutGraph& LG, FSolverInputs& Out);

	// Opt-in: collapse any existing UK2Node_Knot within Subgraph, then insert
	// new reroute knots wherever the solver produced dummy waypoints.
	// Only called by FormatNodes when bReroute=true.
	static void InsertReroutes(UEdGraph* Graph, const TSet<UEdGraphNode*>& Subgraph);

	// Collect the full connected component containing any seed node
	static TSet<UEdGraphNode*> ExpandToSubgraph(const TSet<UEdGraphNode*>& SeedNodes);

	// Build adjacency lists from pin connections within the subgraph
	static FLayoutGraph BuildLayoutGraph(const TSet<UEdGraphNode*>& SubgraphNodes);

	// Sugiyama phases
	static void AssignLayers(FLayoutGraph& LG);
	static void OrderWithinLayers(FLayoutGraph& LG);
	static void AssignCoordinates(FLayoutGraph& LG);

	// Shift a freshly laid-out subgraph below any external node it overlaps.
	static void ResolveOverlaps(
		FLayoutGraph& LG,
		const TArray<UEdGraphNode*>& AllGraphNodes);

	// Internal convenience: pick the profile from the first node's owning graph.
	static EGraphLayoutProfile SelectProfile(const FLayoutGraph& LG);

	// Helpers
	static FRect NodeRect(const UEdGraphNode* Node);
	static bool HasExecPins(const UEdGraphNode* Node);
	static bool IsCommentNode(const UEdGraphNode* Node);
	static int32 CountVisiblePins(const UEdGraphNode* Node, EEdGraphPinDirection Direction);
	static TArray<UEdGraphNode*> SortNodesByName(const TSet<UEdGraphNode*>& Nodes);

	// Y offset of a pin's row from the node's top-left, matching EstimateNodeSize constants.
	// RowIndexWithinDirection = position among same-direction VISIBLE pins on that node.
	static float PinOffsetY(const UEdGraphNode* Node, const UEdGraphPin* Pin);

	// Map a comment title to a keyword-matched tint color (case-insensitive).
	// First match in the table wins; returns neutral grey if no keyword matches.
	static FLinearColor KeywordColorForTitle(const FString& Title);
};
