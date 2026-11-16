// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAILayout.h"
#include "KismetAILog.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphNode_Comment.h"
#include "AnimationGraphSchema.h"
#include "KismetLayeredLayout.h"
#include "K2Node_Knot.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"

DEFINE_LOG_CATEGORY_STATIC(LogKismetAILayout, Log, All);

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

bool FKismetAILayout::HasExecPins(const UEdGraphNode* Node)
{
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			return true;
		}
	}
	return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Layout profile (schema-driven spine selection)
// ─────────────────────────────────────────────────────────────────────────────

EGraphLayoutProfile FKismetAILayout::SelectProfile(const UEdGraph* Graph)
{
	if (!Graph) return EGraphLayoutProfile::K2Exec;
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (!Schema) return EGraphLayoutProfile::K2Exec;

	// AnimationGraphSchema derives from the K2 schema, so test it first.
	if (Schema->IsA<UAnimationGraphSchema>())
	{
		return EGraphLayoutProfile::AnimPose;
	}
	// Detect material graphs by schema class name to avoid a hard dependency on
	// the material-editor module (UMaterialGraphSchema lives in UnrealEd).
	if (Schema->GetClass()->GetName().Contains(TEXT("Material")))
	{
		return EGraphLayoutProfile::MaterialData;
	}
	return EGraphLayoutProfile::K2Exec;
}

EGraphLayoutProfile FKismetAILayout::SelectProfile(const FLayoutGraph& LG)
{
	for (UEdGraphNode* Node : LG.Nodes)
	{
		if (Node)
		{
			return SelectProfile(Node->GetGraph());
		}
	}
	return EGraphLayoutProfile::K2Exec;
}

bool FKismetAILayout::ProfileHasSpine(EGraphLayoutProfile Profile)
{
	return Profile == EGraphLayoutProfile::K2Exec
	    || Profile == EGraphLayoutProfile::AnimPose;
}

bool FKismetAILayout::IsSpinePin(const UEdGraphPin* Pin, EGraphLayoutProfile Profile)
{
	if (!Pin) return false;
	switch (Profile)
	{
	case EGraphLayoutProfile::K2Exec:
		return Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
	case EGraphLayoutProfile::AnimPose:
		return UAnimationGraphSchema::IsPosePin(Pin->PinType);
	default:
		return false;
	}
}

bool FKismetAILayout::IsSpineNode(const UEdGraphNode* Node, EGraphLayoutProfile Profile)
{
	if (!Node || !ProfileHasSpine(Profile)) return false;
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (IsSpinePin(Pin, Profile))
		{
			return true;
		}
	}
	return false;
}

bool FKismetAILayout::IsCommentNode(const UEdGraphNode* Node)
{
	FString ClassName = Node->GetClass()->GetName();
	return ClassName.Contains(TEXT("Comment"));
}

int32 FKismetAILayout::CountVisiblePins(const UEdGraphNode* Node, EEdGraphPinDirection Direction)
{
	int32 Count = 0;
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && !Pin->bHidden && Pin->Direction == Direction)
		{
			Count++;
		}
	}
	return Count;
}

FKismetAILayout::FRect FKismetAILayout::NodeRect(const UEdGraphNode* Node)
{
	const FNodeSize Size = EstimateNodeSize(Node);
	return { Node->NodePosX, Node->NodePosY,
	         Node->NodePosX + Size.Width, Node->NodePosY + Size.Height };
}

TArray<UEdGraphNode*> FKismetAILayout::SortNodesByName(const TSet<UEdGraphNode*>& Nodes)
{
	TArray<UEdGraphNode*> Sorted;
	Sorted.Reserve(Nodes.Num());
	for (UEdGraphNode* Node : Nodes)
	{
		if (Node)
		{
			Sorted.Add(Node);
		}
	}
	Sorted.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
	{
		return A.GetName() < B.GetName();
	});
	return Sorted;
}

// ─────────────────────────────────────────────────────────────────────────────
// EstimateNodeSize
// ─────────────────────────────────────────────────────────────────────────────

FNodeSize FKismetAILayout::EstimateNodeSize(const UEdGraphNode* Node)
{
	if (!Node) return { 220, 74 };

	// Comment nodes store their own size
	if (IsCommentNode(Node))
	{
		int32 W = FMath::Max(Node->NodeWidth, 100);
		int32 H = FMath::Max(Node->NodeHeight, 40);
		return { W, H };
	}

	FString ClassName = Node->GetClass()->GetName();

	// Knot/reroute nodes are tiny
	if (ClassName.Contains(TEXT("Knot")))
	{
		return { 16, 16 };
	}

	// Count visible pins
	int32 InputPins = CountVisiblePins(Node, EGPD_Input);
	int32 OutputPins = CountVisiblePins(Node, EGPD_Output);
	int32 PinRows = FMath::Max(InputPins, OutputPins);
	PinRows = FMath::Max(PinRows, 1); // At least one row

	// Height: title + pins + padding
	constexpr int32 TitleHeight = 26;
	constexpr int32 PinRowHeight = 24;
	constexpr int32 BottomPadding = 10;
	int32 Height = TitleHeight + PinRows * PinRowHeight + BottomPadding;

	// Width: base depends on node class
	int32 Width = 220; // default

	if (ClassName.Contains(TEXT("Event")))
	{
		Width = 250;
	}
	else if (ClassName.Contains(TEXT("IfThenElse")))
	{
		Width = 200;
	}
	else if (ClassName.Contains(TEXT("MacroInstance")))
	{
		Width = 240;
	}
	else if (ClassName.Contains(TEXT("DynamicCast")))
	{
		Width = 260;
	}

	// Adjust width for long titles
	FString Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
	int32 TitleLen = Title.Len();
	if (TitleLen > 10)
	{
		Width = FMath::Max(Width, 160 + (TitleLen - 10) * 8);
	}
	Width = FMath::Min(Width, 400);

	return { Width, Height };
}

// ─────────────────────────────────────────────────────────────────────────────
// ExpandToSubgraph
// ─────────────────────────────────────────────────────────────────────────────

TSet<UEdGraphNode*> FKismetAILayout::ExpandToSubgraph(const TSet<UEdGraphNode*>& SeedNodes)
{
	TSet<UEdGraphNode*> Visited;
	TArray<UEdGraphNode*> Queue;

	for (UEdGraphNode* Node : SeedNodes)
	{
		if (Node && !IsCommentNode(Node))
		{
			Visited.Add(Node);
			Queue.Add(Node);
		}
	}

	while (Queue.Num() > 0)
	{
		UEdGraphNode* Current = Queue[0];
		Queue.RemoveAt(0);

		for (const UEdGraphPin* Pin : Current->Pins)
		{
			if (!Pin) continue;
			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!LinkedPin) continue;
				UEdGraphNode* Neighbor = LinkedPin->GetOwningNode();
				if (Neighbor && !IsCommentNode(Neighbor) && !Visited.Contains(Neighbor))
				{
					Visited.Add(Neighbor);
					Queue.Add(Neighbor);
				}
			}
		}
	}

	return Visited;
}

// ─────────────────────────────────────────────────────────────────────────────
// BuildLayoutGraph
// ─────────────────────────────────────────────────────────────────────────────

FKismetAILayout::FLayoutGraph FKismetAILayout::BuildLayoutGraph(const TSet<UEdGraphNode*>& SubgraphNodes)
{
	FLayoutGraph LG;
	LG.Nodes = SortNodesByName(SubgraphNodes);

	for (UEdGraphNode* Node : LG.Nodes)
	{
		LG.ForwardEdges.FindOrAdd(Node);
		LG.BackwardEdges.FindOrAdd(Node);
		LG.NodeSizes.Add(Node, EstimateNodeSize(Node));
	}

	// Build edges: for each output pin linked to an input pin within the subgraph,
	// add a forward edge from the output's owner to the input's owner.
	// Prioritize exec connections for edge direction.
	TSet<TPair<UEdGraphNode*, UEdGraphNode*>> AddedEdges;

	for (UEdGraphNode* Node : LG.Nodes)
	{
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output) continue;

			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!LinkedPin) continue;
				UEdGraphNode* Target = LinkedPin->GetOwningNode();
				if (!Target || Target == Node || !SubgraphNodes.Contains(Target)) continue;

				TPair<UEdGraphNode*, UEdGraphNode*> EdgeKey(Node, Target);
				if (AddedEdges.Contains(EdgeKey)) continue;
				AddedEdges.Add(EdgeKey);

				LG.ForwardEdges.FindOrAdd(Node).Add(Target);
				LG.BackwardEdges.FindOrAdd(Target).Add(Node);
			}
		}
	}

	return LG;
}

// ─────────────────────────────────────────────────────────────────────────────
// AssignLayers (longest-path from sources)
// ─────────────────────────────────────────────────────────────────────────────

void FKismetAILayout::AssignLayers(FLayoutGraph& LG)
{
	// Compute in-degree for Kahn's topological sort
	TMap<UEdGraphNode*, int32> InDegree;
	for (UEdGraphNode* Node : LG.Nodes)
	{
		InDegree.Add(Node, LG.BackwardEdges.FindOrAdd(Node).Num());
	}

	// Initialize queue with source nodes (in-degree 0), sorted by name
	TArray<UEdGraphNode*> Queue;
	for (UEdGraphNode* Node : LG.Nodes)
	{
		if (InDegree[Node] == 0)
		{
			Queue.Add(Node);
		}
	}
	Queue.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
	{
		return A.GetName() < B.GetName();
	});

	TArray<UEdGraphNode*> TopoOrder;
	TopoOrder.Reserve(LG.Nodes.Num());

	while (Queue.Num() > 0)
	{
		UEdGraphNode* Node = Queue[0];
		Queue.RemoveAt(0);
		TopoOrder.Add(Node);

		// Sort successors for determinism before processing
		TArray<UEdGraphNode*>& Successors = LG.ForwardEdges.FindOrAdd(Node);
		TArray<UEdGraphNode*> SortedSuccessors = Successors;
		SortedSuccessors.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
		{
			return A.GetName() < B.GetName();
		});

		for (UEdGraphNode* Succ : SortedSuccessors)
		{
			InDegree[Succ]--;
			if (InDegree[Succ] == 0)
			{
				// Insert maintaining sorted order
				int32 InsertIdx = 0;
				for (; InsertIdx < Queue.Num(); InsertIdx++)
				{
					if (Succ->GetName() < Queue[InsertIdx]->GetName())
					{
						break;
					}
				}
				Queue.Insert(Succ, InsertIdx);
			}
		}
	}

	// Handle cycles: any node not in TopoOrder gets appended (sorted by name)
	if (TopoOrder.Num() < LG.Nodes.Num())
	{
		TSet<UEdGraphNode*> InTopo(TopoOrder);
		for (UEdGraphNode* Node : LG.Nodes)
		{
			if (!InTopo.Contains(Node))
			{
				TopoOrder.Add(Node);
			}
		}
	}

	// Longest-path layer assignment
	for (UEdGraphNode* Node : TopoOrder)
	{
		int32 Layer = 0;
		for (UEdGraphNode* Pred : LG.BackwardEdges.FindOrAdd(Node))
		{
			if (LG.LayerAssignment.Contains(Pred))
			{
				Layer = FMath::Max(Layer, LG.LayerAssignment[Pred] + 1);
			}
		}
		LG.LayerAssignment.Add(Node, Layer);
	}

	// Pull pure (off-spine) nodes back toward their consumers, in reverse
	// topological order. Skipped entirely for spine-less profiles (MaterialData):
	// there every node is "pure", so pulling them all back would collapse ranks.
	const EGraphLayoutProfile Profile = SelectProfile(LG);
	for (int32 i = TopoOrder.Num() - 1; ProfileHasSpine(Profile) && i >= 0; i--)
	{
		UEdGraphNode* Node = TopoOrder[i];
		if (IsSpineNode(Node, Profile)) continue;

		int32 MinConsumerLayer = INT32_MAX;
		for (UEdGraphNode* Succ : LG.ForwardEdges.FindOrAdd(Node))
		{
			if (LG.LayerAssignment.Contains(Succ))
			{
				MinConsumerLayer = FMath::Min(MinConsumerLayer, LG.LayerAssignment[Succ]);
			}
		}

		if (MinConsumerLayer != INT32_MAX && MinConsumerLayer > 0)
		{
			LG.LayerAssignment[Node] = MinConsumerLayer - 1;
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// OrderWithinLayers (barycenter heuristic, 2 passes)
// ─────────────────────────────────────────────────────────────────────────────

void FKismetAILayout::OrderWithinLayers(FLayoutGraph& LG)
{
	// Group nodes by layer
	int32 MaxLayer = 0;
	TMap<int32, TArray<UEdGraphNode*>> LayerGroups;
	for (UEdGraphNode* Node : LG.Nodes)
	{
		int32 Layer = LG.LayerAssignment.FindRef(Node);
		LayerGroups.FindOrAdd(Layer).Add(Node);
		MaxLayer = FMath::Max(MaxLayer, Layer);
	}

	// Initialize order within each layer by sorting by name
	for (auto& Pair : LayerGroups)
	{
		Pair.Value.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
		{
			return A.GetName() < B.GetName();
		});
		for (int32 i = 0; i < Pair.Value.Num(); i++)
		{
			LG.OrderInLayer.Add(Pair.Value[i], i);
		}
	}

	// Helper: compute barycenter of a node relative to its neighbors in an adjacent layer
	auto ComputeBarycenter = [&](UEdGraphNode* Node, const TMap<UEdGraphNode*, TArray<UEdGraphNode*>>& Edges) -> int32
	{
		const TArray<UEdGraphNode*>* Neighbors = Edges.Find(Node);
		if (!Neighbors || Neighbors->Num() == 0)
		{
			return LG.OrderInLayer.FindRef(Node) * 1000; // Keep current position
		}

		int32 Sum = 0;
		int32 Count = 0;
		for (UEdGraphNode* Neighbor : *Neighbors)
		{
			if (LG.OrderInLayer.Contains(Neighbor))
			{
				Sum += LG.OrderInLayer[Neighbor] * 1000; // Scale up for integer precision
				Count++;
			}
		}
		return (Count > 0) ? (Sum / Count) : (LG.OrderInLayer.FindRef(Node) * 1000);
	};

	// Tie-breaker: when sibling nodes share a predecessor (same barycenter), order
	// them by the predecessor's OUTPUT-PIN order so multi-output drivers fan out
	// top-to-bottom in pin order — Sequence's Then 0/1/2…, Branch's True/False, a
	// Switch's cases. Returns the index of the driving output pin among the
	// predecessor's output pins (lower = higher up); INT32_MAX when none.
	auto SourcePinOrder = [&](UEdGraphNode* Node) -> int32
	{
		int32 Best = INT32_MAX;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input) continue;
			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (!Linked) continue;
				UEdGraphNode* Pred = Linked->GetOwningNode();
				if (!Pred) continue;
				int32 Idx = 0;
				for (UEdGraphPin* PP : Pred->Pins)
				{
					if (!PP || PP->Direction != EGPD_Output) continue;
					if (PP == Linked) { Best = FMath::Min(Best, Idx); break; }
					++Idx;
				}
			}
		}
		return Best;
	};

	// Within-layer order key: barycenter first, then driving-pin order (so a
	// Sequence/Branch/Switch fans out in pin order), then name for determinism.
	struct FOrderKey { int32 BC; int32 Pin; FString Name; UEdGraphNode* Node; };
	auto OrderKeyLess = [](const FOrderKey& A, const FOrderKey& B)
	{
		if (A.BC != B.BC)   return A.BC  < B.BC;
		if (A.Pin != B.Pin) return A.Pin < B.Pin;
		return A.Name < B.Name;
	};

	// Forward pass: layer 1..MaxLayer, using backward edges (neighbors in previous layer)
	for (int32 Layer = 1; Layer <= MaxLayer; Layer++)
	{
		TArray<UEdGraphNode*>* Group = LayerGroups.Find(Layer);
		if (!Group || Group->Num() <= 1) continue;

		// Compute order keys: (barycenter, source-pin order, name).
		TArray<FOrderKey> Keys;
		for (UEdGraphNode* Node : *Group)
		{
			Keys.Add({ ComputeBarycenter(Node, LG.BackwardEdges), SourcePinOrder(Node), Node->GetName(), Node });
		}

		Keys.Sort(OrderKeyLess);

		// Update order
		Group->Empty();
		for (int32 i = 0; i < Keys.Num(); i++)
		{
			Group->Add(Keys[i].Node);
			LG.OrderInLayer[Keys[i].Node] = i;
		}
	}

	// Backward pass: layer (MaxLayer-1)..0, using forward edges (neighbors in next layer)
	for (int32 Layer = MaxLayer - 1; Layer >= 0; Layer--)
	{
		TArray<UEdGraphNode*>* Group = LayerGroups.Find(Layer);
		if (!Group || Group->Num() <= 1) continue;

		TArray<FOrderKey> Keys;
		for (UEdGraphNode* Node : *Group)
		{
			Keys.Add({ ComputeBarycenter(Node, LG.ForwardEdges), SourcePinOrder(Node), Node->GetName(), Node });
		}

		Keys.Sort(OrderKeyLess);

		Group->Empty();
		for (int32 i = 0; i < Keys.Num(); i++)
		{
			Group->Add(Keys[i].Node);
			LG.OrderInLayer[Keys[i].Node] = i;
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// PinOffsetY
// ─────────────────────────────────────────────────────────────────────────────

float FKismetAILayout::PinOffsetY(const UEdGraphNode* Node, const UEdGraphPin* Pin)
{
	// Constants must match EstimateNodeSize exactly.
	constexpr float TitleHeight  = 26.f;
	constexpr float PinRowHeight = 24.f;

	const EEdGraphPinDirection Dir = Pin->Direction;

	// Count the pin's 0-based index among visible pins in the same direction.
	int32 RowIndex = 0;
	for (const UEdGraphPin* P : Node->Pins)
	{
		if (!P || P->bHidden || P->Direction != Dir) continue;
		if (P == Pin) break;
		RowIndex++;
	}

	return TitleHeight + (RowIndex + 0.5f) * PinRowHeight;
}

// ─────────────────────────────────────────────────────────────────────────────
// BuildSolverInputs
// ─────────────────────────────────────────────────────────────────────────────

void FKismetAILayout::BuildSolverInputs(const FLayoutGraph& LG, FSolverInputs& Out)
{
	// Apply solver tuning. RankSpacing / NodeSpacingY already default to the
	// adapter's Horizontal/VerticalSpacing; only the Y-pass mode is opt-in here.
	{
		FLayeredConfig Cfg;
		Cfg.RankSpacing     = static_cast<float>(HorizontalSpacing);
		Cfg.NodeSpacingY    = static_cast<float>(VerticalSpacing);
		Cfg.bFourDirBalance = bUseFourDirBalance;
		Out.Solver.SetConfig(Cfg);
	}

	// Map each subgraph node to a core vertex id (in LG.Nodes order, which is
	// name-sorted — satisfies determinism).
	Out.NodeToId.Reserve(LG.Nodes.Num());

	for (UEdGraphNode* Node : LG.Nodes)
	{
		const FNodeSize& Size = LG.NodeSizes.FindChecked(Node);
		const int32 Id = Out.Solver.AddVertex(static_cast<float>(Size.Width),
		                                      static_cast<float>(Size.Height));
		Out.NodeToId.Add(Node, Id);
	}

	// Seed rank and order from the Sugiyama phases already run by the caller.
	for (UEdGraphNode* Node : LG.Nodes)
	{
		const int32 Id    = Out.NodeToId.FindChecked(Node);
		const int32 Rank  = LG.LayerAssignment.FindChecked(Node);
		const int32 Order = LG.OrderInLayer.FindChecked(Node);
		Out.Solver.SetSeedRank(Id, Rank);
		Out.Solver.SetSeedOrder(Id, Order);
	}

	// Emit one directed edge per connected output→input pin pair within the
	// subgraph. Iterate LG.Nodes (name-sorted) and each Node->Pins in array
	// order for determinism. One edge per physical pin pair — NOT the
	// deduped LG.ForwardEdges which collapses multi-pin connections.
	// Record EdgePins in the SAME loop so edge index ↔ pin pair stays aligned.
	const EGraphLayoutProfile Profile = SelectProfile(LG);
	TSet<UEdGraphNode*> SubgraphSet(LG.Nodes);
	for (UEdGraphNode* Node : LG.Nodes)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output) continue;

			const float FromY = PinOffsetY(Node, Pin);
			// Spine edges (exec for K2, pose for anim) get the solver's
			// straighten-preference; material graphs have no spine pins.
			const bool bExec  = IsSpinePin(Pin, Profile);

			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!LinkedPin) continue;
				UEdGraphNode* Target = LinkedPin->GetOwningNode();
				if (!Target || Target == Node || !SubgraphSet.Contains(Target)) continue;

				const float ToY = PinOffsetY(Target, LinkedPin);

				const int32 FromId = Out.NodeToId.FindChecked(Node);
				const int32 ToId   = Out.NodeToId.FindChecked(Target);
				Out.Solver.AddEdge(FromId, ToId, FromY, ToY, bExec);
				// Record the physical pin pair at the same edge index.
				Out.EdgePins.Add(TPair<UEdGraphPin*, UEdGraphPin*>(Pin, LinkedPin));
			}
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// AssignCoordinates
// ─────────────────────────────────────────────────────────────────────────────

// Round to the nearest multiple of Grid, half away from zero. Deterministic.
static int32 SnapToGrid(int32 Value, int32 Grid)
{
	if (Grid <= 1) return Value;
	const int32 Half = Grid / 2;
	return (Value >= 0)
		? ((Value + Half) / Grid) * Grid
		: -(((-Value + Half) / Grid) * Grid);
}

void FKismetAILayout::AssignCoordinates(FLayoutGraph& LG)
{
	// Build the solver inputs (vertices, seeds, edges + pin map).
	FSolverInputs SI;
	BuildSolverInputs(LG, SI);

	SI.Solver.Solve();

	// Write back Y from the solver; keep X = Layer * HorizontalSpacing.
	for (UEdGraphNode* Node : LG.Nodes)
	{
		const int32 Id    = SI.NodeToId.FindChecked(Node);
		const int32 Layer = LG.LayerAssignment.FindChecked(Node);
		Node->NodePosX = Layer * HorizontalSpacing;
		Node->NodePosY = static_cast<int32>(SI.Solver.GetPosition(Id).Y);
	}

	// Post-pass: place pure (off-spine) nodes below the spine corridor.
	//
	// Problem: when a pure node sits between spine-connected nodes, the spine wire
	// runs horizontally while data wires cross it diagonally, creating ugly
	// X-shaped crossings.
	//
	// Solution: shift all pure nodes as a group below the spine corridor, preserving
	// their relative Y positions from the main layout pass (so multiple pure nodes
	// at the same layer are properly spaced and don't overlap each other).
	//
	// Spine-less profiles (MaterialData) have no corridor — every node is data —
	// so the pass is skipped (ProfileHasSpine guards it). It also self-skips when
	// the graph happens to contain no spine nodes (SpineNodesBottomY stays unset).
	const EGraphLayoutProfile Profile = SelectProfile(LG);

	int32 SpineNodesBottomY = INT32_MIN;
	int32 PureNodesTopY = INT32_MAX;
	bool bHasPureNodes = false;

	for (UEdGraphNode* Node : LG.Nodes)
	{
		if (IsSpineNode(Node, Profile))
		{
			FNodeSize Size = LG.NodeSizes.FindRef(Node);
			int32 Bottom = Node->NodePosY + Size.Height;
			SpineNodesBottomY = FMath::Max(SpineNodesBottomY, Bottom);
		}
		else
		{
			PureNodesTopY = FMath::Min(PureNodesTopY, Node->NodePosY);
			bHasPureNodes = true;
		}
	}

	if (bUsePureNodeCorridor && ProfileHasSpine(Profile) && bHasPureNodes && SpineNodesBottomY != INT32_MIN)
	{
		constexpr int32 PureNodeGap = 40;
		int32 TargetTopY = SpineNodesBottomY + PureNodeGap;
		int32 ShiftY = TargetTopY - PureNodesTopY;

		if (ShiftY > 0)
		{
			for (UEdGraphNode* Node : LG.Nodes)
			{
				if (IsSpineNode(Node, Profile)) continue;
				Node->NodePosY += ShiftY;
			}
		}
	}

	// Snap Y to the editor grid so rows line up cleanly. X is already a clean
	// multiple of HorizontalSpacing. Anchoring by the callers later applies
	// the same delta to every node, so even if the anchor offset isn't on-grid
	// the nodes stay aligned to each other.
	for (UEdGraphNode* Node : LG.Nodes)
	{
		Node->NodePosY = SnapToGrid(Node->NodePosY, GridSize);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// ResolveOverlaps
// ─────────────────────────────────────────────────────────────────────────────

void FKismetAILayout::ResolveOverlaps(
	FLayoutGraph& LG,
	const TArray<UEdGraphNode*>& AllGraphNodes)
{
	if (LG.Nodes.Num() == 0) return;

	TSet<UEdGraphNode*> SubgraphSet(LG.Nodes);

	// Compute subgraph bounding box
	int32 SubMinX = INT32_MAX, SubMinY = INT32_MAX;
	int32 SubMaxX = INT32_MIN, SubMaxY = INT32_MIN;
	for (UEdGraphNode* Node : LG.Nodes)
	{
		FNodeSize Size = LG.NodeSizes.FindRef(Node);
		SubMinX = FMath::Min(SubMinX, Node->NodePosX);
		SubMinY = FMath::Min(SubMinY, Node->NodePosY);
		SubMaxX = FMath::Max(SubMaxX, Node->NodePosX + Size.Width);
		SubMaxY = FMath::Max(SubMaxY, Node->NodePosY + Size.Height);
	}

	// Check each non-subgraph node for overlap
	int32 MaxShiftY = 0;
	for (UEdGraphNode* ExtNode : AllGraphNodes)
	{
		if (!ExtNode || SubgraphSet.Contains(ExtNode) || IsCommentNode(ExtNode)) continue;

		FNodeSize ExtSize = EstimateNodeSize(ExtNode);
		int32 ExtMinX = ExtNode->NodePosX;
		int32 ExtMinY = ExtNode->NodePosY;
		int32 ExtMaxX = ExtMinX + ExtSize.Width;
		int32 ExtMaxY = ExtMinY + ExtSize.Height;

		// Check AABB overlap
		bool bOverlapsX = (SubMinX < ExtMaxX) && (SubMaxX > ExtMinX);
		bool bOverlapsY = (SubMinY < ExtMaxY) && (SubMaxY > ExtMinY);

		if (bOverlapsX && bOverlapsY)
		{
			// Compute minimum Y shift to resolve
			int32 ShiftNeeded = ExtMaxY - SubMinY + VerticalSpacing;
			MaxShiftY = FMath::Max(MaxShiftY, ShiftNeeded);
		}
	}

	if (MaxShiftY > 0)
	{
		for (UEdGraphNode* Node : LG.Nodes)
		{
			Node->NodePosY += MaxShiftY;
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// InsertReroutes
// ─────────────────────────────────────────────────────────────────────────────

void FKismetAILayout::InsertReroutes(UEdGraph* Graph, const TSet<UEdGraphNode*>& Subgraph)
{
	if (!Graph || Subgraph.Num() == 0) return;

	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (!Schema)
	{
		UE_LOG(LogKismetAILayout, Warning, TEXT("InsertReroutes: graph has no schema, skipping"));
		return;
	}

	// Knot materialization spawns UK2Node_Knot, which is valid for K2 and anim
	// (K2-derived) graphs but not material graphs (those use a different reroute
	// expression). Skip rather than inject an incompatible node.
	if (SelectProfile(Graph) == EGraphLayoutProfile::MaterialData)
	{
		UE_LOG(LogKismetAILayout, Warning, TEXT("InsertReroutes: material graph, skipping (UK2Node_Knot not applicable)"));
		return;
	}

	// ── Step 1: Collapse existing knots within the subgraph (idempotency) ────
	// Collect knots first to avoid modifying the subgraph while iterating.
	TArray<UK2Node_Knot*> ExistingKnots;
	for (UEdGraphNode* Node : Subgraph)
	{
		if (UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Node))
		{
			ExistingKnots.Add(Knot);
		}
	}

	for (UK2Node_Knot* Knot : ExistingKnots)
	{
		UEdGraphPin* KnotIn  = Knot->GetInputPin();
		UEdGraphPin* KnotOut = Knot->GetOutputPin();
		if (!KnotIn || !KnotOut) continue;

		// Collect source pins (those connected to KnotIn) and target pins
		// (those connected to KnotOut) before modifying anything.
		TArray<UEdGraphPin*> SourcePins;
		for (UEdGraphPin* Linked : KnotIn->LinkedTo)
		{
			if (Linked) SourcePins.Add(Linked);
		}
		TArray<UEdGraphPin*> TargetPins;
		for (UEdGraphPin* Linked : KnotOut->LinkedTo)
		{
			if (Linked) TargetPins.Add(Linked);
		}

		// Reconnect each source directly to each target.
		for (UEdGraphPin* Src : SourcePins)
		{
			for (UEdGraphPin* Tgt : TargetPins)
			{
				Src->GetOwningNode()->Modify();
				Tgt->GetOwningNode()->Modify();
				Src->Modify();
				Tgt->Modify();
				Schema->TryCreateConnection(Src, Tgt);
			}
		}

		// Break all links from the knot and remove it.
		Knot->Modify();
		KnotIn->BreakAllPinLinks();
		KnotOut->BreakAllPinLinks();
		Graph->RemoveNode(Knot);
	}

	// ── Step 2: Re-run layout on the knot-free subgraph ──────────────────────
	// Refresh the active node set (knots are gone).
	TSet<UEdGraphNode*> Sub2;
	for (UEdGraphNode* Node : Subgraph)
	{
		if (Node && Node->GetGraph() == Graph)
		{
			Sub2.Add(Node);
		}
	}
	// Also remove any knot we just deleted.
	for (UK2Node_Knot* Knot : ExistingKnots)
	{
		Sub2.Remove(Knot);
	}
	if (Sub2.Num() == 0) return;

	FLayoutGraph LG2 = BuildLayoutGraph(Sub2);
	AssignLayers(LG2);
	OrderWithinLayers(LG2);
	FSolverInputs SI;
	BuildSolverInputs(LG2, SI);
	SI.Solver.Solve();

	// ── Step 3: Materialize dummy chains as UK2Node_Knot nodes ───────────────
	const int32 NumEdges = SI.EdgePins.Num();
	for (int32 e = 0; e < NumEdges; e++)
	{
		const TArray<int32>& Chain = SI.Solver.GetEdgeChain(e);
		if (Chain.Num() == 0) continue;  // Only span>1 edges

		UEdGraphPin* SourcePin = SI.EdgePins[e].Key;
		UEdGraphPin* TargetPin = SI.EdgePins[e].Value;

		// Guard: pins must still be valid and owned by live nodes.
		if (!SourcePin || !TargetPin) continue;
		UEdGraphNode* SourceNode = SourcePin->GetOwningNodeUnchecked();
		UEdGraphNode* TargetNode = TargetPin->GetOwningNodeUnchecked();
		if (!SourceNode || !TargetNode) continue;

		// Map knot coordinates into the FINAL (anchored) frame. This re-solve
		// is in the raw solver frame, but FormatNodes already anchored the real
		// nodes by a uniform DeltaX and ResolveOverlaps shifted Y. Place each knot
		// on the straight segment between the source and target pins in their final
		// on-screen positions, so it sits on the wire's lane regardless of anchoring
		// or the pure-node post-pass (which the bare re-solve does not reproduce).
		const int32* SourceVtx = SI.NodeToId.Find(SourceNode);
		const int32* TargetVtx = SI.NodeToId.Find(TargetNode);
		if (!SourceVtx || !TargetVtx) continue;
		const float SourceSolverX = SI.Solver.GetPosition(*SourceVtx).X;
		const float TargetSolverX = SI.Solver.GetPosition(*TargetVtx).X;
		const int32 DeltaX = SourceNode->NodePosX - FMath::RoundToInt(SourceSolverX);
		const float SourcePinY = SourceNode->NodePosY + PinOffsetY(SourceNode, SourcePin);
		const float TargetPinY = TargetNode->NodePosY + PinOffsetY(TargetNode, TargetPin);
		const float SpanX = TargetSolverX - SourceSolverX;

		// ── Create and wire the knot chain ───────────────────────────────────
		// Order to avoid a broken wire:
		//   1. Create all knot nodes.
		//   2. Wire Source→Knot[0]→...→Knot[N]→Target using TryCreateConnection.
		//   3. Only THEN break the original Source↔Target direct link.

		TArray<UK2Node_Knot*> NewKnots;
		NewKnots.Reserve(Chain.Num());
		for (int32 DummyId : Chain)
		{
			UK2Node_Knot* Knot = NewObject<UK2Node_Knot>(Graph);
			Knot->CreateNewGuid();
			Knot->AllocateDefaultPins();
			Graph->AddNode(Knot, false, false);

			// X: dummy's rank column shifted into the anchored frame.
			const FVector2D DummyPos = SI.Solver.GetPosition(DummyId);
			Knot->NodePosX = FMath::RoundToInt(DummyPos.X) + DeltaX;
			// Y: interpolate between source and target pin Y by rank fraction, so
			// the knot lands on the wire. Knot is 16 tall; offset by half so its
			// centered pin sits on the line.
			const float Frac = (FMath::Abs(SpanX) > KINDA_SMALL_NUMBER)
				? (DummyPos.X - SourceSolverX) / SpanX : 0.5f;
			const float KnotCenterY = FMath::Lerp(SourcePinY, TargetPinY, Frac);
			Knot->NodePosY = SnapToGrid(FMath::RoundToInt(KnotCenterY) - 8, GridSize);

			NewKnots.Add(Knot);
		}

		// Wire: Source→Knot[0], Knot[k].Out→Knot[k+1].In, Knot[N].Out→Target.
		bool bChainOk = true;

		// Source→first knot
		{
			UEdGraphPin* KnotIn = NewKnots[0]->GetInputPin();
			SourceNode->Modify();
			NewKnots[0]->Modify();
			if (!Schema->TryCreateConnection(SourcePin, KnotIn))
			{
				UE_LOG(LogKismetAILayout, Warning,
					TEXT("InsertReroutes: TryCreateConnection Source->Knot[0] failed for edge %d; leaving original wire intact"), e);
				bChainOk = false;
			}
		}

		// Knot[k].Out→Knot[k+1].In
		for (int32 k = 0; k + 1 < NewKnots.Num() && bChainOk; k++)
		{
			UEdGraphPin* KnotOut = NewKnots[k]->GetOutputPin();
			UEdGraphPin* NextIn  = NewKnots[k + 1]->GetInputPin();
			NewKnots[k]->Modify();
			NewKnots[k + 1]->Modify();
			if (!Schema->TryCreateConnection(KnotOut, NextIn))
			{
				UE_LOG(LogKismetAILayout, Warning,
					TEXT("InsertReroutes: TryCreateConnection Knot[%d]->Knot[%d] failed for edge %d; leaving original wire intact"), k, k + 1, e);
				bChainOk = false;
			}
		}

		// Last knot→Target
		if (bChainOk)
		{
			UEdGraphPin* LastOut = NewKnots.Last()->GetOutputPin();
			NewKnots.Last()->Modify();
			TargetNode->Modify();
			if (!Schema->TryCreateConnection(LastOut, TargetPin))
			{
				UE_LOG(LogKismetAILayout, Warning,
					TEXT("InsertReroutes: TryCreateConnection Knot[N]->Target failed for edge %d; leaving original wire intact"), e);
				bChainOk = false;
			}
		}

		if (bChainOk)
		{
			// Chain is connected: now break the original direct Source↔Target link.
			SourcePin->Modify();
			TargetPin->Modify();
			SourcePin->BreakLinkTo(TargetPin);
		}
		else
		{
			// Chain failed: tear down the partial knot chain to avoid dangling nodes.
			for (UK2Node_Knot* Knot : NewKnots)
			{
				Knot->GetInputPin()->BreakAllPinLinks();
				Knot->GetOutputPin()->BreakAllPinLinks();
				Graph->RemoveNode(Knot);
			}
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// KeywordColorForTitle
// ─────────────────────────────────────────────────────────────────────────────

FLinearColor FKismetAILayout::KeywordColorForTitle(const FString& Title)
{
	const FString L = Title.ToLower();
	auto Has = [&](const TCHAR* K) { return L.Contains(K); };

	if (Has(TEXT("destroy")) || Has(TEXT("death")) || Has(TEXT("die")) || Has(TEXT("dead"))
		|| Has(TEXT("kill")) || Has(TEXT("remove")))
	{
		return FLinearColor(0.40f, 0.16f, 0.18f, 1.0f);
	}
	if (Has(TEXT("damage")) || Has(TEXT("hit")) || Has(TEXT("hurt")) || Has(TEXT("health"))
		|| Has(TEXT("attack")))
	{
		return FLinearColor(0.55f, 0.20f, 0.22f, 1.0f);
	}
	if (Has(TEXT("spawn")) || Has(TEXT("create")) || Has(TEXT("add")) || Has(TEXT("build"))
		|| Has(TEXT("construct")))
	{
		return FLinearColor(0.22f, 0.46f, 0.30f, 1.0f);
	}
	if (Has(TEXT("begin")) || Has(TEXT("init")) || Has(TEXT("start")) || Has(TEXT("setup"))
		|| Has(TEXT("ready")) || Has(TEXT("load")))
	{
		return FLinearColor(0.18f, 0.32f, 0.55f, 1.0f);
	}
	if (Has(TEXT("tick")) || Has(TEXT("update")) || Has(TEXT("frame")) || Has(TEXT("loop")))
	{
		return FLinearColor(0.16f, 0.42f, 0.45f, 1.0f);
	}
	if (Has(TEXT("move")) || Has(TEXT("look")) || Has(TEXT("jump")) || Has(TEXT("click"))
		|| Has(TEXT("touch")))
	{
		return FLinearColor(0.55f, 0.36f, 0.16f, 1.0f);
	}
	if (Has(TEXT("overlap")) || Has(TEXT("collision")))
	{
		return FLinearColor(0.42f, 0.26f, 0.48f, 1.0f);
	}
	if (Has(TEXT("sound")) || Has(TEXT("audio")) || Has(TEXT("play")) || Has(TEXT("music"))
		|| Has(TEXT("sfx")))
	{
		return FLinearColor(0.30f, 0.30f, 0.52f, 1.0f);
	}
	// Neutral grey
	return FLinearColor(0.30f, 0.34f, 0.40f, 1.0f);
}

// ─────────────────────────────────────────────────────────────────────────────
// GroupEventSubtrees
// ─────────────────────────────────────────────────────────────────────────────

void FKismetAILayout::GroupEventSubtrees(UEdGraph* Graph, const TSet<UEdGraphNode*>& Subgraph)
{
	if (!Graph || Subgraph.Num() == 0) return;

	// Step 1: Identify event roots within Subgraph (sorted by name for determinism).
	TArray<UEdGraphNode*> Sorted = SortNodesByName(Subgraph);
	TArray<UEdGraphNode*> Roots;
	for (UEdGraphNode* Node : Sorted)
	{
		if (!Node) continue;

		// Explicit event node types
		if (Cast<UK2Node_Event>(Node) || Cast<UK2Node_CustomEvent>(Node))
		{
			Roots.Add(Node);
			continue;
		}

		// Generic event-root heuristic: has an exec-output pin AND no connected exec-input
		bool bHasExecOut = false;
		bool bHasConnectedExecIn = false;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				if (Pin->Direction == EGPD_Output)
				{
					bHasExecOut = true;
				}
				else if (Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() > 0)
				{
					bHasConnectedExecIn = true;
				}
			}
		}
		if (bHasExecOut && !bHasConnectedExecIn)
		{
			Roots.Add(Node);
		}
	}

	// Collect existing comments for idempotency check
	TArray<UEdGraphNode_Comment*> ExistingComments;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UEdGraphNode_Comment* C = Cast<UEdGraphNode_Comment>(Node))
		{
			ExistingComments.Add(C);
		}
	}

	// Step 2 & onward: for each root, BFS to collect subtree, then create comment
	for (UEdGraphNode* Root : Roots)
	{
		// BFS forward over output-pin LinkedTo (exec AND data) staying within Subgraph
		TSet<UEdGraphNode*> Members;
		TArray<UEdGraphNode*> Queue;
		Members.Add(Root);
		Queue.Add(Root);

		while (Queue.Num() > 0)
		{
			UEdGraphNode* Current = Queue[0];
			Queue.RemoveAt(0);

			for (const UEdGraphPin* Pin : Current->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output) continue;
				for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (!LinkedPin) continue;
					UEdGraphNode* Neighbor = LinkedPin->GetOwningNodeUnchecked();
					if (!Neighbor) continue;
					if (!Subgraph.Contains(Neighbor)) continue;
					if (Members.Contains(Neighbor)) continue;
					Members.Add(Neighbor);
					Queue.Add(Neighbor);
				}
			}
		}

		// Skip single-member subtrees
		if (Members.Num() < 2) continue;

		// Idempotency — skip if every member's center is already inside an existing comment
		bool bAlreadyCovered = false;
		for (const UEdGraphNode_Comment* ExComment : ExistingComments)
		{
			const int32 CMinX = ExComment->NodePosX;
			const int32 CMinY = ExComment->NodePosY;
			const int32 CMaxX = CMinX + FMath::Max(ExComment->NodeWidth, 100);
			const int32 CMaxY = CMinY + FMath::Max(ExComment->NodeHeight, 40);

			bool bAllInside = true;
			for (const UEdGraphNode* Member : Members)
			{
				const FRect R = NodeRect(Member);
				const int32 MX = (R.MinX + R.MaxX) / 2;
				const int32 MY = (R.MinY + R.MaxY) / 2;
				if (MX < CMinX || MX > CMaxX || MY < CMinY || MY > CMaxY)
				{
					bAllInside = false;
					break;
				}
			}
			if (bAllInside)
			{
				bAlreadyCovered = true;
				break;
			}
		}
		if (bAlreadyCovered) continue;

		// Step 5: compute bounding box
		int32 MinX = INT32_MAX, MinY = INT32_MAX, MaxX = INT32_MIN, MaxY = INT32_MIN;
		for (const UEdGraphNode* Member : Members)
		{
			const FRect R = NodeRect(Member);
			MinX = FMath::Min(MinX, R.MinX);
			MinY = FMath::Min(MinY, R.MinY);
			MaxX = FMath::Max(MaxX, R.MaxX);
			MaxY = FMath::Max(MaxY, R.MaxY);
		}

		// Build title from root node
		const FString Title = Root->GetNodeTitle(ENodeTitleType::ListView).ToString();

		// Create the comment node (purely additive)
		UEdGraphNode_Comment* C = NewObject<UEdGraphNode_Comment>(Graph);
		Graph->AddNode(C, false, false);
		C->CreateNewGuid();
		C->NodePosX = MinX - CommentPad;
		C->NodePosY = MinY - CommentTitlePad;
		C->NodeWidth  = (MaxX - MinX) + 2 * CommentPad;
		C->NodeHeight = (MaxY - MinY) + CommentPad + CommentTitlePad;
		C->NodeComment = Title;
		C->CommentColor = KeywordColorForTitle(Title);
		C->bCommentBubbleVisible = false;

		// Populate the under-comment cache so editor group-move drags the members
		// with the box (GetNodesUnderComment is only refreshed by Slate interaction
		// headlessly — mirror the reflow loop / PlaceNewNodes pattern).
		C->ClearNodesUnderComment();
		for (UEdGraphNode* Member : Members)
		{
			C->AddNodeUnderComment(Member);
		}

		// Add the newly created comment to ExistingComments so subsequent roots
		// can detect they are already covered (idempotency within a single call).
		ExistingComments.Add(C);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// FormatNodes (explicit full reformat — layout_graph tool)
// ─────────────────────────────────────────────────────────────────────────────

void FKismetAILayout::FormatNodes(UEdGraph* Graph, const TSet<UEdGraphNode*>& SeedNodes, bool bReroute, bool bGroup)
{
	if (!Graph) return;

	// Empty seeds means "format the whole graph".
	TSet<UEdGraphNode*> CleanSeeds;
	if (SeedNodes.Num() == 0)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && !IsCommentNode(Node))
			{
				CleanSeeds.Add(Node);
			}
		}
	}
	else
	{
		for (UEdGraphNode* Node : SeedNodes)
		{
			if (Node && !IsCommentNode(Node))
			{
				CleanSeeds.Add(Node);
			}
		}
	}
	if (CleanSeeds.Num() == 0) return;

	// Expand to full connected subgraphs
	TSet<UEdGraphNode*> SubgraphNodes = ExpandToSubgraph(CleanSeeds);

	// Capture comment membership BEFORE moving anything: a node belongs to a
	// comment when its center sits inside the comment box. The editor-side
	// cache (GetNodesUnderComment) is only refreshed by Slate interaction, so
	// geometry is the source of truth headlessly.
	TArray<UEdGraphNode_Comment*> Comments;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node))
		{
			Comments.Add(Comment);
		}
	}
	TMap<UEdGraphNode_Comment*, TArray<UEdGraphNode*>> CommentMembers;
	for (UEdGraphNode_Comment* Comment : Comments)
	{
		const int32 CMinX = Comment->NodePosX;
		const int32 CMinY = Comment->NodePosY;
		const int32 CMaxX = CMinX + FMath::Max(Comment->NodeWidth, 100);
		const int32 CMaxY = CMinY + FMath::Max(Comment->NodeHeight, 40);
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || IsCommentNode(Node)) continue;
			const FRect R = NodeRect(Node);
			const int32 CX = (R.MinX + R.MaxX) / 2;
			const int32 CY = (R.MinY + R.MaxY) / 2;
			if (CX >= CMinX && CX <= CMaxX && CY >= CMinY && CY <= CMaxY)
			{
				CommentMembers.FindOrAdd(Comment).Add(Node);
			}
		}
	}

	// Partition into connected components (there may be multiple disconnected subgraphs)
	TArray<TSet<UEdGraphNode*>> Components;
	TSet<UEdGraphNode*> Assigned;
	for (UEdGraphNode* Node : SortNodesByName(SubgraphNodes))
	{
		if (Assigned.Contains(Node)) continue;

		TSet<UEdGraphNode*> Seed;
		Seed.Add(Node);
		TSet<UEdGraphNode*> Component = ExpandToSubgraph(Seed);

		TSet<UEdGraphNode*> FilteredComponent;
		for (UEdGraphNode* N : Component)
		{
			if (SubgraphNodes.Contains(N))
			{
				FilteredComponent.Add(N);
				Assigned.Add(N);
			}
		}
		if (FilteredComponent.Num() > 0)
		{
			Components.Add(MoveTemp(FilteredComponent));
		}
	}

	TArray<UEdGraphNode*> AllGraphNodes;
	Graph->GetNodesOfClass<UEdGraphNode>(AllGraphNodes);

	for (const TSet<UEdGraphNode*>& Component : Components)
	{
		// Anchor each component at its own pre-format top-left so the graph
		// stays in the region the user expects it.
		int32 OrigMinX = INT32_MAX, OrigMinY = INT32_MAX;
		for (UEdGraphNode* Node : Component)
		{
			OrigMinX = FMath::Min(OrigMinX, Node->NodePosX);
			OrigMinY = FMath::Min(OrigMinY, Node->NodePosY);
		}

		FLayoutGraph LG = BuildLayoutGraph(Component);
		AssignLayers(LG);
		OrderWithinLayers(LG);
		AssignCoordinates(LG);

		int32 NewMinX = INT32_MAX, NewMinY = INT32_MAX;
		for (UEdGraphNode* Node : LG.Nodes)
		{
			NewMinX = FMath::Min(NewMinX, Node->NodePosX);
			NewMinY = FMath::Min(NewMinY, Node->NodePosY);
		}
		const int32 DeltaX = SnapToGrid(OrigMinX - NewMinX, GridSize);
		const int32 DeltaY = SnapToGrid(OrigMinY - NewMinY, GridSize);
		for (UEdGraphNode* Node : LG.Nodes)
		{
			Node->NodePosX += DeltaX;
			Node->NodePosY += DeltaY;
		}

		ResolveOverlaps(LG, AllGraphNodes);
	}

	// Opt-in: insert reroute knots for long edges (only when caller requests).
	if (bReroute)
	{
		InsertReroutes(Graph, SubgraphNodes);
	}

	// Reflow each pre-existing comment box around its members and refresh the
	// engine cache so editor group-move behavior matches what is on screen.
	// This runs BEFORE GroupEventSubtrees so that existing comment boxes have
	// up-to-date positions when the idempotency check runs.
	for (UEdGraphNode_Comment* Comment : Comments)
	{
		const TArray<UEdGraphNode*>* Members = CommentMembers.Find(Comment);
		if (!Members || Members->Num() == 0) continue;

		int32 MinX = INT32_MAX, MinY = INT32_MAX, MaxX = INT32_MIN, MaxY = INT32_MIN;
		for (UEdGraphNode* Node : *Members)
		{
			const FRect R = NodeRect(Node);
			MinX = FMath::Min(MinX, R.MinX);
			MinY = FMath::Min(MinY, R.MinY);
			MaxX = FMath::Max(MaxX, R.MaxX);
			MaxY = FMath::Max(MaxY, R.MaxY);
		}

		Comment->Modify();
		Comment->NodePosX = MinX - CommentPad;
		Comment->NodePosY = MinY - CommentTitlePad;
		Comment->NodeWidth = (MaxX - MinX) + 2 * CommentPad;
		Comment->NodeHeight = (MaxY - MinY) + CommentTitlePad + CommentPad;
		Comment->ClearNodesUnderComment();
		for (UEdGraphNode* Node : *Members)
		{
			Comment->AddNodeUnderComment(Node);
		}
	}

	// Opt-in: wrap each event subtree in a keyword-colored comment box.
	// Runs AFTER the reflow loop so that pre-existing comment boxes are at
	// their current post-layout positions when the idempotency check fires.
	if (bGroup)
	{
		GroupEventSubtrees(Graph, SubgraphNodes);
	}

	KISMETAI_DEV_LOG(LogKismetAILayout, Log, TEXT("Format applied to %d nodes across %d component(s)"),
		SubgraphNodes.Num(), Components.Num());
}

// ─────────────────────────────────────────────────────────────────────────────
// PlaceNewNodes (containment placement — implicit path for all mutating tools)
// ─────────────────────────────────────────────────────────────────────────────

void FKismetAILayout::PlaceNewNodes(UEdGraph* Graph, const TSet<UEdGraphNode*>& NewNodesIn,
	TSet<UEdGraphNode*>* OutIslandNodes, bool bForceInternalLayout)
{
	if (!Graph || NewNodesIn.Num() == 0) return;

	TSet<UEdGraphNode*> NewNodes;
	TArray<UEdGraphNode_Comment*> NewComments; // comments in the same import batch
	for (UEdGraphNode* Node : NewNodesIn)
	{
		if (!Node) continue;
		if (UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node))
		{
			NewComments.Add(Comment);
		}
		else
		{
			NewNodes.Add(Node);
		}
	}
	if (NewNodes.Num() == 0) return;

	// Everything that already existed is immovable: nodes and comment boxes
	// are obstacles, never layout subjects. Comments imported in this batch
	// are neither — they follow the cluster that holds their member nodes.
	TArray<UEdGraphNode*> ExistingNodes;
	TArray<UEdGraphNode_Comment*> Comments;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node || NewNodesIn.Contains(Node)) continue;
		if (UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node))
		{
			Comments.Add(Comment);
		}
		else
		{
			ExistingNodes.Add(Node);
		}
	}

	auto CommentRect = [](const UEdGraphNode_Comment* Comment) -> FRect
	{
		return { Comment->NodePosX, Comment->NodePosY,
		         Comment->NodePosX + FMath::Max(Comment->NodeWidth, 100),
		         Comment->NodePosY + FMath::Max(Comment->NodeHeight, 40) };
	};
	auto CenterInRect = [](const UEdGraphNode* Node, const FRect& R) -> bool
	{
		const FNodeSize Size = EstimateNodeSize(Node);
		const int32 CX = Node->NodePosX + Size.Width / 2;
		const int32 CY = Node->NodePosY + Size.Height / 2;
		return CX >= R.MinX && CX <= R.MaxX && CY >= R.MinY && CY <= R.MaxY;
	};

	// Partition the new nodes into clusters connected through new nodes only.
	TArray<TSet<UEdGraphNode*>> Clusters;
	{
		TSet<UEdGraphNode*> Assigned;
		for (UEdGraphNode* Node : SortNodesByName(NewNodes))
		{
			if (Assigned.Contains(Node)) continue;
			TSet<UEdGraphNode*> Cluster;
			TArray<UEdGraphNode*> Queue;
			Cluster.Add(Node);
			Queue.Add(Node);
			while (Queue.Num() > 0)
			{
				UEdGraphNode* Cur = Queue.Pop();
				for (UEdGraphPin* Pin : Cur->Pins)
				{
					if (!Pin) continue;
					for (UEdGraphPin* Linked : Pin->LinkedTo)
					{
						UEdGraphNode* Nb = Linked ? Linked->GetOwningNode() : nullptr;
						if (Nb && NewNodes.Contains(Nb) && !Cluster.Contains(Nb))
						{
							Cluster.Add(Nb);
							Queue.Add(Nb);
						}
					}
				}
			}
			Assigned.Append(Cluster);
			Clusters.Add(MoveTemp(Cluster));
		}
	}

	// Membership of imported comments, captured against pre-translation
	// positions so each comment can follow its members' cluster.
	TMap<UEdGraphNode_Comment*, TArray<UEdGraphNode*>> NewCommentMembers;
	for (UEdGraphNode_Comment* Comment : NewComments)
	{
		const FRect CR{ Comment->NodePosX, Comment->NodePosY,
			Comment->NodePosX + FMath::Max(Comment->NodeWidth, 100),
			Comment->NodePosY + FMath::Max(Comment->NodeHeight, 40) };
		for (UEdGraphNode* Node : NewNodes)
		{
			const FNodeSize Size = EstimateNodeSize(Node);
			const int32 CX = Node->NodePosX + Size.Width / 2;
			const int32 CY = Node->NodePosY + Size.Height / 2;
			if (CX >= CR.MinX && CX <= CR.MaxX && CY >= CR.MinY && CY <= CR.MaxY)
			{
				NewCommentMembers.FindOrAdd(Comment).Add(Node);
			}
		}
	}
	TMap<UEdGraphNode*, FIntPoint> AppliedDeltas;

	TArray<FRect> PlacedRects; // clusters placed earlier in this call

	for (const TSet<UEdGraphNode*>& Cluster : Clusters)
	{
		TArray<UEdGraphNode*> ClusterArr = SortNodesByName(Cluster);

		// Internal arrangement: keep authored relative positions when usable
		// (write_blueprint T3D often places deliberately); run the hierarchical
		// layout only when new nodes overlap each other (the "everything
		// spawned at 0,0" case).
		bool bInternalOverlap = false;
		for (int32 i = 0; i < ClusterArr.Num() && !bInternalOverlap; i++)
		{
			for (int32 j = i + 1; j < ClusterArr.Num(); j++)
			{
				if (NodeRect(ClusterArr[i]).Intersects(NodeRect(ClusterArr[j])))
				{
					bInternalOverlap = true;
					break;
				}
			}
		}
		if (bForceInternalLayout || bInternalOverlap)
		{
			FLayoutGraph LG = BuildLayoutGraph(Cluster);
			AssignLayers(LG);
			OrderWithinLayers(LG);
			AssignCoordinates(LG);
		}

		// Anchor: the best link to an existing node decides where the cluster
		// goes. Exec beats pose beats data; the wire direction decides the side.
		//
		// Pose links (anim graphs) flow left->right into the Result sink just like
		// exec, so a new node feeding another node's pose INPUT (the link is an
		// OUTPUT on the new node) sits to its LEFT; a node consuming an existing
		// node's pose output sits to its RIGHT. A pure data node feeding a value
		// pin uses the generic data-pin fallback (last resort) so it lands left of
		// its consumer without disturbing existing K2 placement.
		UEdGraphNode* AnchorExisting = nullptr;
		UEdGraphNode* AnchorNew = nullptr;
		bool bPlaceRightOfAnchor = true;
		int32 BestScore = INT32_MAX;
		FString BestTie;
		for (UEdGraphNode* Node : ClusterArr)
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin) continue;
				const bool bExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
				const bool bPose = !bExec && UAnimationGraphSchema::IsPosePin(Pin->PinType);
				for (UEdGraphPin* Linked : Pin->LinkedTo)
				{
					UEdGraphNode* Existing = Linked ? Linked->GetOwningNode() : nullptr;
					if (!Existing || NewNodes.Contains(Existing) || IsCommentNode(Existing)) continue;

					int32 Score;
					bool bRight;
					if (bExec && Pin->Direction == EGPD_Input)      { Score = 0; bRight = true;  } // existing execs into new
					else if (bExec)                                  { Score = 1; bRight = false; } // new execs into existing
					else if (bPose && Pin->Direction == EGPD_Input)  { Score = 2; bRight = true;  } // existing pose feeds new
					else if (bPose)                                  { Score = 3; bRight = false; } // new feeds existing pose input
					else if (Pin->Direction == EGPD_Input)           { Score = 4; bRight = true;  } // existing data-feeds new
					else                                             { Score = 5; bRight = false; } // new data-feeds existing

					const FString Tie = Existing->GetName() + TEXT(".") + Pin->PinName.ToString();
					if (Score < BestScore || (Score == BestScore && Tie < BestTie))
					{
						BestScore = Score;
						BestTie = Tie;
						AnchorExisting = Existing;
						AnchorNew = Node;
						bPlaceRightOfAnchor = bRight;
					}
				}
			}
		}

		// A cluster with no link to a pre-existing node is an island: it lands off
		// to the right of the graph's content with no real anchor. Report its nodes
		// so the caller can pull them next to their peer when they are first wired.
		if (!AnchorExisting && OutIslandNodes)
		{
			for (UEdGraphNode* Node : ClusterArr)
			{
				OutIslandNodes->Add(Node);
			}
		}

		// Desired translation for the cluster.
		int32 DeltaX = 0, DeltaY = 0;
		if (AnchorExisting)
		{
			const FNodeSize ESize = EstimateNodeSize(AnchorExisting);
			const FNodeSize NSize = EstimateNodeSize(AnchorNew);
			const int32 TargetX = bPlaceRightOfAnchor
				? AnchorExisting->NodePosX + ESize.Width + PlacementGapX
				: AnchorExisting->NodePosX - NSize.Width - PlacementGapX;
			const int32 TargetY = AnchorExisting->NodePosY;
			DeltaX = TargetX - AnchorNew->NodePosX;
			DeltaY = TargetY - AnchorNew->NodePosY;
		}
		else
		{
			// Island: to the right of the graph's content, top-aligned.
			int32 ExistMaxX = INT32_MIN, ExistMinY = INT32_MAX;
			for (UEdGraphNode* Existing : ExistingNodes)
			{
				const FRect R = NodeRect(Existing);
				ExistMaxX = FMath::Max(ExistMaxX, R.MaxX);
				ExistMinY = FMath::Min(ExistMinY, R.MinY);
			}
			for (const FRect& R : PlacedRects)
			{
				ExistMaxX = FMath::Max(ExistMaxX, R.MaxX);
				ExistMinY = FMath::Min(ExistMinY, R.MinY);
			}
			if (ExistMaxX != INT32_MIN)
			{
				int32 ClusterMinX = INT32_MAX, ClusterMinY = INT32_MAX;
				for (UEdGraphNode* Node : ClusterArr)
				{
					ClusterMinX = FMath::Min(ClusterMinX, Node->NodePosX);
					ClusterMinY = FMath::Min(ClusterMinY, Node->NodePosY);
				}
				DeltaX = (ExistMaxX + SubgraphMargin) - ClusterMinX;
				DeltaY = ExistMinY - ClusterMinY;
			}
		}
		DeltaX = SnapToGrid(DeltaX, GridSize);
		DeltaY = SnapToGrid(DeltaY, GridSize);

		// Obstacles: every existing node, every cluster placed earlier in this
		// call, and every comment box that does not contain the anchor — a new
		// node must not land inside an unrelated comment frame.
		UEdGraphNode_Comment* AnchorComment = nullptr;
		TArray<FRect> Obstacles;
		for (UEdGraphNode* Existing : ExistingNodes)
		{
			Obstacles.Add(NodeRect(Existing));
		}
		Obstacles.Append(PlacedRects);
		for (UEdGraphNode_Comment* Comment : Comments)
		{
			const FRect CR = CommentRect(Comment);
			if (AnchorExisting && CenterInRect(AnchorExisting, CR))
			{
				if (!AnchorComment)
				{
					AnchorComment = Comment;
				}
				continue; // the anchor's own frame is an allowed region
			}
			Obstacles.Add(CR);
		}

		auto ClusterBounds = [&](int32 DX, int32 DY) -> FRect
		{
			FRect B{ INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN };
			for (UEdGraphNode* Node : ClusterArr)
			{
				const FRect R = NodeRect(Node);
				B.MinX = FMath::Min(B.MinX, R.MinX + DX);
				B.MinY = FMath::Min(B.MinY, R.MinY + DY);
				B.MaxX = FMath::Max(B.MaxX, R.MaxX + DX);
				B.MaxY = FMath::Max(B.MaxY, R.MaxY + DY);
			}
			return B;
		};
		auto Collides = [&](const FRect& R) -> bool
		{
			// Small breathing margin so "just clears" doesn't read as touching.
			const FRect Inflated{ R.MinX - GridSize, R.MinY - GridSize, R.MaxX + GridSize, R.MaxY + GridSize };
			for (const FRect& O : Obstacles)
			{
				if (Inflated.Intersects(O)) return true;
			}
			return false;
		};

		// Free-space search: sweep vertically first (keeps the X lane that
		// reads as "next step in the chain"), then try further columns,
		// finally drop below everything.
		const int32 StepY = VerticalSpacing + GridSize;
		int32 FinalDX = DeltaX, FinalDY = DeltaY;
		bool bPlaced = false;
		for (int32 Col = 0; Col < 4 && !bPlaced; Col++)
		{
			const int32 DX = DeltaX + Col * HorizontalSpacing;
			for (int32 Attempt = 0; Attempt <= 20 && !bPlaced; Attempt++)
			{
				const int32 K = (Attempt + 1) / 2;
				const int32 Off = (Attempt == 0) ? 0 : ((Attempt % 2 == 1) ? K * StepY : -K * StepY);
				const int32 DY = DeltaY + Off;
				if (!Collides(ClusterBounds(DX, DY)))
				{
					FinalDX = DX;
					FinalDY = DY;
					bPlaced = true;
				}
			}
		}
		if (!bPlaced)
		{
			int32 MaxBottom = INT32_MIN;
			for (const FRect& O : Obstacles)
			{
				MaxBottom = FMath::Max(MaxBottom, O.MaxY);
			}
			const FRect AtLane = ClusterBounds(DeltaX, 0);
			FinalDX = DeltaX;
			FinalDY = SnapToGrid(MaxBottom + SubgraphMargin - AtLane.MinY, GridSize);
		}

		for (UEdGraphNode* Node : ClusterArr)
		{
			Node->NodePosX += FinalDX;
			Node->NodePosY += FinalDY;
			AppliedDeltas.Add(Node, FIntPoint(FinalDX, FinalDY));
		}
		const FRect Final = ClusterBounds(0, 0);
		PlacedRects.Add(Final);

		// If the anchor lives inside a comment, adopt the new nodes into the
		// frame so it keeps telling the truth: grow the box to cover them and
		// update the engine-side membership cache.
		if (AnchorComment)
		{
			const FRect CR = CommentRect(AnchorComment);
			const int32 NewMinX = FMath::Min(CR.MinX, Final.MinX - CommentPad);
			const int32 NewMinY = FMath::Min(CR.MinY, Final.MinY - CommentPad);
			const int32 NewMaxX = FMath::Max(CR.MaxX, Final.MaxX + CommentPad);
			const int32 NewMaxY = FMath::Max(CR.MaxY, Final.MaxY + CommentPad);

			AnchorComment->Modify();
			AnchorComment->NodePosX = NewMinX;
			AnchorComment->NodePosY = NewMinY;
			AnchorComment->NodeWidth = NewMaxX - NewMinX;
			AnchorComment->NodeHeight = NewMaxY - NewMinY;
			for (UEdGraphNode* Node : ClusterArr)
			{
				AnchorComment->AddNodeUnderComment(Node);
			}
		}
	}

	// Imported comment boxes follow the cluster holding their members and the
	// membership cache is initialized so editor group-move works immediately.
	for (UEdGraphNode_Comment* Comment : NewComments)
	{
		const TArray<UEdGraphNode*>* Members = NewCommentMembers.Find(Comment);
		if (Members && Members->Num() > 0)
		{
			if (const FIntPoint* Delta = AppliedDeltas.Find((*Members)[0]))
			{
				Comment->NodePosX += Delta->X;
				Comment->NodePosY += Delta->Y;
			}
			Comment->ClearNodesUnderComment();
			for (UEdGraphNode* Member : *Members)
			{
				Comment->AddNodeUnderComment(Member);
			}
		}
	}

	KISMETAI_DEV_LOG(LogKismetAILayout, Log, TEXT("Placed %d new node(s) in %d cluster(s); existing nodes untouched"),
		NewNodes.Num(), Clusters.Num());
}
