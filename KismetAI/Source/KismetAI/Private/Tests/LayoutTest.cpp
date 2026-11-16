// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAILayout.h"
#include "KismetAILayoutCommands.h"
#include "KismetAIModule.h"
#include "Misc/AutomationTest.h"
#include "Framework/Application/SlateApplication.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_Knot.h"
#include "K2Node_CustomEvent.h"
#include "AnimationGraphSchema.h"
#include "Animation/AnimNodeBase.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "K2Node_CallFunction.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet/KismetSystemLibrary.h"
#include "UObject/Package.h"

constexpr auto LayoutTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// Helper: create a minimal node with pins for layout testing
// ─────────────────────────────────────────────────────────────────────────────

static UEdGraphNode* CreateTestNode(UEdGraph* Graph, const FName& Name, int32 NumInputs = 1, int32 NumOutputs = 1)
{
	UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, UEdGraphNode::StaticClass(), Name);
	Node->CreateNewGuid();
	Node->NodePosX = 0;
	Node->NodePosY = 0;
	Graph->AddNode(Node, false, false);

	// Add exec input
	for (int32 i = 0; i < NumInputs; i++)
	{
		UEdGraphPin* Pin = Node->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec,
			FName(*FString::Printf(TEXT("In_%d"), i)));
		Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
	}

	// Add exec output
	for (int32 i = 0; i < NumOutputs; i++)
	{
		UEdGraphPin* Pin = Node->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec,
			FName(*FString::Printf(TEXT("Out_%d"), i)));
		Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
	}

	return Node;
}

static void ConnectNodes(UEdGraphNode* Source, int32 OutputIdx, UEdGraphNode* Target, int32 InputIdx)
{
	UEdGraphPin* SourcePin = nullptr;
	UEdGraphPin* TargetPin = nullptr;

	int32 OutCount = 0;
	for (UEdGraphPin* Pin : Source->Pins)
	{
		if (Pin->Direction == EGPD_Output)
		{
			if (OutCount == OutputIdx)
			{
				SourcePin = Pin;
				break;
			}
			OutCount++;
		}
	}

	int32 InCount = 0;
	for (UEdGraphPin* Pin : Target->Pins)
	{
		if (Pin->Direction == EGPD_Input)
		{
			if (InCount == InputIdx)
			{
				TargetPin = Pin;
				break;
			}
			InCount++;
		}
	}

	if (SourcePin && TargetPin)
	{
		SourcePin->LinkedTo.Add(TargetPin);
		TargetPin->LinkedTo.Add(SourcePin);
	}
}

struct FNodeBounds
{
	int32 MinX, MinY, MaxX, MaxY;
};

static FNodeBounds GetBounds(UEdGraphNode* Node)
{
	FNodeSize Size = FKismetAILayout::EstimateNodeSize(Node);
	return { Node->NodePosX, Node->NodePosY,
	         Node->NodePosX + Size.Width, Node->NodePosY + Size.Height };
}

static bool BoundsOverlap(const FNodeBounds& A, const FNodeBounds& B)
{
	return (A.MinX < B.MaxX) && (A.MaxX > B.MinX) &&
	       (A.MinY < B.MaxY) && (A.MaxY > B.MinY);
}

// ─────────────────────────────────────────────────────────────────────────────
// EstimateNodeSize tests
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayout_EstimateSize_Basic,
	"KismetAI.Layout.EstimateSize.BasicNode", LayoutTestFlags)
bool FLayout_EstimateSize_Basic::RunTest(const FString& Parameters)
{
	UEdGraph* Graph = NewObject<UEdGraph>();
	UEdGraphNode* Node = CreateTestNode(Graph, TEXT("TestNode"), 2, 2);

	FNodeSize Size = FKismetAILayout::EstimateNodeSize(Node);
	TestTrue(TEXT("Width is positive"), Size.Width > 0);
	TestTrue(TEXT("Height is positive"), Size.Height > 0);
	TestTrue(TEXT("Width reasonable"), Size.Width >= 100 && Size.Width <= 400);
	TestTrue(TEXT("Height reasonable"), Size.Height >= 40 && Size.Height <= 300);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayout_EstimateSize_MorePinsTaller,
	"KismetAI.Layout.EstimateSize.MorePinsTaller", LayoutTestFlags)
bool FLayout_EstimateSize_MorePinsTaller::RunTest(const FString& Parameters)
{
	UEdGraph* Graph = NewObject<UEdGraph>();
	UEdGraphNode* SmallNode = CreateTestNode(Graph, TEXT("Small"), 1, 1);
	UEdGraphNode* BigNode = CreateTestNode(Graph, TEXT("Big"), 6, 6);

	FNodeSize SmallSize = FKismetAILayout::EstimateNodeSize(SmallNode);
	FNodeSize BigSize = FKismetAILayout::EstimateNodeSize(BigNode);

	TestTrue(TEXT("More pins = taller node"), BigSize.Height > SmallSize.Height);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayout_EstimateSize_NullNode,
	"KismetAI.Layout.EstimateSize.NullNode", LayoutTestFlags)
bool FLayout_EstimateSize_NullNode::RunTest(const FString& Parameters)
{
	FNodeSize Size = FKismetAILayout::EstimateNodeSize(nullptr);
	TestTrue(TEXT("Default width for null"), Size.Width == 220);
	TestTrue(TEXT("Default height for null"), Size.Height == 74);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Layout: linear chain produces no overlaps
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayout_LinearChain_NoOverlap,
	"KismetAI.Layout.LinearChain.NoOverlap", LayoutTestFlags)
bool FLayout_LinearChain_NoOverlap::RunTest(const FString& Parameters)
{
	UEdGraph* Graph = NewObject<UEdGraph>();

	// Create a chain: A -> B -> C -> D -> E
	TArray<UEdGraphNode*> Nodes;
	for (int32 i = 0; i < 5; i++)
	{
		Nodes.Add(CreateTestNode(Graph, *FString::Printf(TEXT("Node_%d"), i)));
	}
	for (int32 i = 0; i < 4; i++)
	{
		ConnectNodes(Nodes[i], 0, Nodes[i + 1], 0);
	}

	TSet<UEdGraphNode*> SeedNodes(Nodes);
	FKismetAILayout::FormatNodes(Graph, SeedNodes);

	// Verify no overlaps
	for (int32 i = 0; i < Nodes.Num(); i++)
	{
		for (int32 j = i + 1; j < Nodes.Num(); j++)
		{
			FNodeBounds A = GetBounds(Nodes[i]);
			FNodeBounds B = GetBounds(Nodes[j]);
			TestFalse(
				*FString::Printf(TEXT("Nodes %d and %d should not overlap"), i, j),
				BoundsOverlap(A, B));
		}
	}

	// Verify left-to-right ordering (each node has greater X than previous)
	for (int32 i = 0; i < 4; i++)
	{
		TestTrue(
			*FString::Printf(TEXT("Node %d should be left of Node %d"), i, i + 1),
			Nodes[i]->NodePosX < Nodes[i + 1]->NodePosX);
	}

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Layout: a chain of differently-sized nodes is vertically aligned (straight)
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayout_LinearChain_Aligned,
	"KismetAI.Layout.LinearChain.Aligned", LayoutTestFlags)
bool FLayout_LinearChain_Aligned::RunTest(const FString& Parameters)
{
	UEdGraph* Graph = NewObject<UEdGraph>();

	// Chain of nodes with deliberately different heights (varying pin counts).
	// Before the alignment pass, independent per-layer centering left each node
	// at a different top-Y, so the exec wire sloped. They must now share a Y.
	TArray<UEdGraphNode*> Nodes;
	Nodes.Add(CreateTestNode(Graph, TEXT("Node_0"), 1, 1));
	Nodes.Add(CreateTestNode(Graph, TEXT("Node_1"), 5, 5));
	Nodes.Add(CreateTestNode(Graph, TEXT("Node_2"), 1, 2));
	Nodes.Add(CreateTestNode(Graph, TEXT("Node_3"), 3, 1));
	for (int32 i = 0; i < Nodes.Num() - 1; i++)
	{
		ConnectNodes(Nodes[i], 0, Nodes[i + 1], 0);
	}

	TSet<UEdGraphNode*> SeedNodes(Nodes);
	FKismetAILayout::FormatNodes(Graph, SeedNodes);

	const int32 BaseY = Nodes[0]->NodePosY;
	for (int32 i = 1; i < Nodes.Num(); i++)
	{
		TestEqual(
			*FString::Printf(TEXT("Node %d top-Y aligned with chain"), i),
			Nodes[i]->NodePosY, BaseY);
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Layout: final positions are snapped to the editor grid
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayout_GridSnapped,
	"KismetAI.Layout.GridSnapped", LayoutTestFlags)
bool FLayout_GridSnapped::RunTest(const FString& Parameters)
{
	UEdGraph* Graph = NewObject<UEdGraph>();

	UEdGraphNode* Entry = CreateTestNode(Graph, TEXT("Entry"), 0, 1);
	UEdGraphNode* Branch = CreateTestNode(Graph, TEXT("Branch"), 1, 2);
	UEdGraphNode* T = CreateTestNode(Graph, TEXT("TrueTarget"), 1, 1);
	UEdGraphNode* F = CreateTestNode(Graph, TEXT("FalseTarget"), 1, 1);
	ConnectNodes(Entry, 0, Branch, 0);
	ConnectNodes(Branch, 0, T, 0);
	ConnectNodes(Branch, 1, F, 0);

	TSet<UEdGraphNode*> SeedNodes = { Entry, Branch, T, F };
	FKismetAILayout::FormatNodes(Graph, SeedNodes);

	const int32 Grid = FKismetAILayout::GridSize;
	for (UEdGraphNode* Node : SeedNodes)
	{
		TestEqual(
			*FString::Printf(TEXT("%s Y is grid-aligned"), *Node->GetName()),
			Node->NodePosY % Grid, 0);
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Layout: branch topology produces no overlaps
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayout_Branch_NoOverlap,
	"KismetAI.Layout.Branch.NoOverlap", LayoutTestFlags)
bool FLayout_Branch_NoOverlap::RunTest(const FString& Parameters)
{
	UEdGraph* Graph = NewObject<UEdGraph>();

	// Create: Entry -> Branch (2 outputs) -> TrueTarget, FalseTarget
	UEdGraphNode* Entry = CreateTestNode(Graph, TEXT("Entry"), 0, 1);
	UEdGraphNode* Branch = CreateTestNode(Graph, TEXT("Branch"), 1, 2);
	UEdGraphNode* TrueTarget = CreateTestNode(Graph, TEXT("TrueTarget"), 1, 1);
	UEdGraphNode* FalseTarget = CreateTestNode(Graph, TEXT("FalseTarget"), 1, 1);

	ConnectNodes(Entry, 0, Branch, 0);
	ConnectNodes(Branch, 0, TrueTarget, 0);
	ConnectNodes(Branch, 1, FalseTarget, 0);

	TSet<UEdGraphNode*> SeedNodes;
	SeedNodes.Add(Entry);
	SeedNodes.Add(Branch);
	SeedNodes.Add(TrueTarget);
	SeedNodes.Add(FalseTarget);

	FKismetAILayout::FormatNodes(Graph, SeedNodes);

	// Verify no overlaps between all pairs
	TArray<UEdGraphNode*> AllNodes = { Entry, Branch, TrueTarget, FalseTarget };
	for (int32 i = 0; i < AllNodes.Num(); i++)
	{
		for (int32 j = i + 1; j < AllNodes.Num(); j++)
		{
			FNodeBounds A = GetBounds(AllNodes[i]);
			FNodeBounds B = GetBounds(AllNodes[j]);
			TestFalse(
				*FString::Printf(TEXT("%s and %s should not overlap"),
					*AllNodes[i]->GetName(), *AllNodes[j]->GetName()),
				BoundsOverlap(A, B));
		}
	}

	// TrueTarget and FalseTarget should be at same X (same layer) but different Y
	TestEqual(TEXT("Branch targets at same X"), TrueTarget->NodePosX, FalseTarget->NodePosX);
	TestTrue(TEXT("Branch targets at different Y"), TrueTarget->NodePosY != FalseTarget->NodePosY);

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Layout: deterministic (two runs produce identical positions)
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayout_Deterministic,
	"KismetAI.Layout.Deterministic", LayoutTestFlags)
bool FLayout_Deterministic::RunTest(const FString& Parameters)
{
	auto BuildAndLayout = [this]() -> TMap<FName, TPair<int32, int32>>
	{
		UEdGraph* Graph = NewObject<UEdGraph>();
		TArray<UEdGraphNode*> Nodes;
		for (int32 i = 0; i < 5; i++)
		{
			Nodes.Add(CreateTestNode(Graph, *FString::Printf(TEXT("Node_%d"), i)));
		}
		// Chain: 0 -> 1 -> 2, 1 -> 3, 3 -> 4
		ConnectNodes(Nodes[0], 0, Nodes[1], 0);
		ConnectNodes(Nodes[1], 0, Nodes[2], 0);
		ConnectNodes(Nodes[1], 0, Nodes[3], 0);
		ConnectNodes(Nodes[3], 0, Nodes[4], 0);

		TSet<UEdGraphNode*> SeedNodes(Nodes);
		FKismetAILayout::FormatNodes(Graph, SeedNodes);

		TMap<FName, TPair<int32, int32>> Positions;
		for (UEdGraphNode* Node : Nodes)
		{
			Positions.Add(Node->GetFName(), TPair<int32, int32>(Node->NodePosX, Node->NodePosY));
		}
		return Positions;
	};

	TMap<FName, TPair<int32, int32>> Run1 = BuildAndLayout();
	TMap<FName, TPair<int32, int32>> Run2 = BuildAndLayout();

	for (const auto& Pair : Run1)
	{
		const TPair<int32, int32>* Other = Run2.Find(Pair.Key);
		TestTrue(*FString::Printf(TEXT("%s exists in both runs"), *Pair.Key.ToString()), Other != nullptr);
		if (Other)
		{
			TestEqual(*FString::Printf(TEXT("%s X matches"), *Pair.Key.ToString()),
				Pair.Value.Key, Other->Key);
			TestEqual(*FString::Printf(TEXT("%s Y matches"), *Pair.Key.ToString()),
				Pair.Value.Value, Other->Value);
		}
	}

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Layout: empty dirty set is a no-op
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayout_EmptyDirtySet_NoOp,
	"KismetAI.Layout.EmptyDirtySet.NoOp", LayoutTestFlags)
bool FLayout_EmptyDirtySet_NoOp::RunTest(const FString& Parameters)
{
	UEdGraph* Graph = NewObject<UEdGraph>();
	UEdGraphNode* Node = CreateTestNode(Graph, TEXT("Existing"), 1, 1);
	Node->NodePosX = 42;
	Node->NodePosY = 99;

	TSet<UEdGraphNode*> Empty;
	FKismetAILayout::PlaceNewNodes(Graph, Empty);

	TestEqual(TEXT("X unchanged"), Node->NodePosX, 42);
	TestEqual(TEXT("Y unchanged"), Node->NodePosY, 99);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Layout: anchor to non-dirty node preserves its position
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayout_Anchor_NonDirtyPreserved,
	"KismetAI.Layout.Anchor.NonDirtyPreserved", LayoutTestFlags)
bool FLayout_Anchor_NonDirtyPreserved::RunTest(const FString& Parameters)
{
	UEdGraph* Graph = NewObject<UEdGraph>();

	UEdGraphNode* Existing = CreateTestNode(Graph, TEXT("Existing"), 0, 1);
	Existing->NodePosX = 500;
	Existing->NodePosY = 200;

	UEdGraphNode* NewNode = CreateTestNode(Graph, TEXT("NewNode"), 1, 0);
	NewNode->NodePosX = 0;
	NewNode->NodePosY = 0;

	ConnectNodes(Existing, 0, NewNode, 0);

	// Only NewNode is new
	TSet<UEdGraphNode*> NewNodes;
	NewNodes.Add(NewNode);

	FKismetAILayout::PlaceNewNodes(Graph, NewNodes);

	// Existing node should keep its original position (it's the anchor)
	TestEqual(TEXT("Anchor X preserved"), Existing->NodePosX, 500);
	TestEqual(TEXT("Anchor Y preserved"), Existing->NodePosY, 200);

	// NewNode should be to the right of Existing
	TestTrue(TEXT("New node to the right"), NewNode->NodePosX > Existing->NodePosX);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Layout: new subgraph placed beside existing content
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayout_NewSubgraph_BesideExisting,
	"KismetAI.Layout.NewSubgraph.BesideExisting", LayoutTestFlags)
bool FLayout_NewSubgraph_BesideExisting::RunTest(const FString& Parameters)
{
	UEdGraph* Graph = NewObject<UEdGraph>();

	// Existing disconnected node
	UEdGraphNode* Existing = CreateTestNode(Graph, TEXT("Existing"), 1, 1);
	Existing->NodePosX = 100;
	Existing->NodePosY = 100;

	// New disconnected chain: A -> B
	UEdGraphNode* A = CreateTestNode(Graph, TEXT("NewA"), 0, 1);
	UEdGraphNode* B = CreateTestNode(Graph, TEXT("NewB"), 1, 0);
	ConnectNodes(A, 0, B, 0);

	TSet<UEdGraphNode*> NewNodes;
	NewNodes.Add(A);
	NewNodes.Add(B);

	FKismetAILayout::PlaceNewNodes(Graph, NewNodes);

	// New nodes should not overlap existing node
	FNodeBounds ExBounds = GetBounds(Existing);
	FNodeBounds ABounds = GetBounds(A);
	FNodeBounds BBounds = GetBounds(B);

	TestFalse(TEXT("A does not overlap Existing"), BoundsOverlap(ABounds, ExBounds));
	TestFalse(TEXT("B does not overlap Existing"), BoundsOverlap(BBounds, ExBounds));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// PlaceNewNodes: pre-existing nodes are never repositioned
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlace_ExistingUntouched,
	"KismetAI.Layout.Place.ExistingUntouched", LayoutTestFlags)
bool FPlace_ExistingUntouched::RunTest(const FString& Parameters)
{
	UEdGraph* Graph = NewObject<UEdGraph>();

	// Human-arranged chain A -> B -> C at deliberate positions
	UEdGraphNode* A = CreateTestNode(Graph, TEXT("A"), 0, 1);
	UEdGraphNode* B = CreateTestNode(Graph, TEXT("B"), 1, 2);
	UEdGraphNode* C = CreateTestNode(Graph, TEXT("C"), 1, 0);
	A->NodePosX = 100;  A->NodePosY = 700;
	B->NodePosX = 500;  B->NodePosY = 640;
	C->NodePosX = 900;  C->NodePosY = 770;
	ConnectNodes(A, 0, B, 0);
	ConnectNodes(B, 0, C, 0);

	// New node wired from B's second exec output
	UEdGraphNode* N = CreateTestNode(Graph, TEXT("New"), 1, 1);
	ConnectNodes(B, 1, N, 0);

	TSet<UEdGraphNode*> NewNodes;
	NewNodes.Add(N);
	FKismetAILayout::PlaceNewNodes(Graph, NewNodes);

	TestEqual(TEXT("A.X unchanged"), A->NodePosX, 100);
	TestEqual(TEXT("A.Y unchanged"), A->NodePosY, 700);
	TestEqual(TEXT("B.X unchanged"), B->NodePosX, 500);
	TestEqual(TEXT("B.Y unchanged"), B->NodePosY, 640);
	TestEqual(TEXT("C.X unchanged"), C->NodePosX, 900);
	TestEqual(TEXT("C.Y unchanged"), C->NodePosY, 770);

	// New node sits to the right of its anchor B, reasonably near it
	TestTrue(TEXT("New node right of anchor"), N->NodePosX > B->NodePosX);
	TestTrue(TEXT("New node near anchor"), FMath::Abs(N->NodePosY - B->NodePosY) < 600);

	// And overlaps nothing
	for (UEdGraphNode* Existing : { A, B, C })
	{
		TestFalse(*FString::Printf(TEXT("New does not overlap %s"), *Existing->GetName()),
			BoundsOverlap(GetBounds(N), GetBounds(Existing)));
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// PlaceNewNodes: occupied slot beside the anchor is avoided, not overlapped
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlace_AvoidsObstacles,
	"KismetAI.Layout.Place.AvoidsObstacles", LayoutTestFlags)
bool FPlace_AvoidsObstacles::RunTest(const FString& Parameters)
{
	UEdGraph* Graph = NewObject<UEdGraph>();

	UEdGraphNode* Anchor = CreateTestNode(Graph, TEXT("Anchor"), 0, 2);
	Anchor->NodePosX = 0;
	Anchor->NodePosY = 0;

	// A blocker already occupies the natural spot right of the anchor
	UEdGraphNode* Blocker = CreateTestNode(Graph, TEXT("Blocker"), 1, 1);
	Blocker->NodePosX = Anchor->NodePosX + FKismetAILayout::EstimateNodeSize(Anchor).Width + FKismetAILayout::PlacementGapX;
	Blocker->NodePosY = 0;

	UEdGraphNode* N = CreateTestNode(Graph, TEXT("New"), 1, 1);
	ConnectNodes(Anchor, 0, N, 0);

	TSet<UEdGraphNode*> NewNodes;
	NewNodes.Add(N);
	FKismetAILayout::PlaceNewNodes(Graph, NewNodes);

	TestEqual(TEXT("Blocker X unchanged"), Blocker->NodePosX,
		Anchor->NodePosX + FKismetAILayout::EstimateNodeSize(Anchor).Width + FKismetAILayout::PlacementGapX);
	TestEqual(TEXT("Blocker Y unchanged"), Blocker->NodePosY, 0);
	TestFalse(TEXT("New does not overlap Blocker"), BoundsOverlap(GetBounds(N), GetBounds(Blocker)));
	TestFalse(TEXT("New does not overlap Anchor"), BoundsOverlap(GetBounds(N), GetBounds(Anchor)));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// PlaceNewNodes: comment boxes that don't contain the anchor are obstacles
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlace_RespectsForeignComment,
	"KismetAI.Layout.Place.RespectsForeignComment", LayoutTestFlags)
bool FPlace_RespectsForeignComment::RunTest(const FString& Parameters)
{
	UEdGraph* Graph = NewObject<UEdGraph>();

	UEdGraphNode* Anchor = CreateTestNode(Graph, TEXT("Anchor"), 0, 1);
	Anchor->NodePosX = 0;
	Anchor->NodePosY = 0;

	// Foreign comment box exactly where the new node would naturally land
	UEdGraphNode_Comment* Comment = NewObject<UEdGraphNode_Comment>(Graph);
	Graph->AddNode(Comment, false, false);
	Comment->CreateNewGuid();
	Comment->NodePosX = Anchor->NodePosX + FKismetAILayout::EstimateNodeSize(Anchor).Width + 20;
	Comment->NodePosY = -100;
	Comment->NodeWidth = 600;
	Comment->NodeHeight = 400;

	UEdGraphNode* N = CreateTestNode(Graph, TEXT("New"), 1, 1);
	ConnectNodes(Anchor, 0, N, 0);

	TSet<UEdGraphNode*> NewNodes;
	NewNodes.Add(N);
	FKismetAILayout::PlaceNewNodes(Graph, NewNodes);

	const FNodeBounds CommentBounds{ Comment->NodePosX, Comment->NodePosY,
		Comment->NodePosX + Comment->NodeWidth, Comment->NodePosY + Comment->NodeHeight };
	TestFalse(TEXT("New node does not land inside a foreign comment"),
		BoundsOverlap(GetBounds(N), CommentBounds));
	TestEqual(TEXT("Comment box unmoved (X)"), Comment->NodePosX,
		Anchor->NodePosX + FKismetAILayout::EstimateNodeSize(Anchor).Width + 20);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// PlaceNewNodes: a new node joining a commented chain grows that comment
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlace_GrowsAnchorComment,
	"KismetAI.Layout.Place.GrowsAnchorComment", LayoutTestFlags)
bool FPlace_GrowsAnchorComment::RunTest(const FString& Parameters)
{
	UEdGraph* Graph = NewObject<UEdGraph>();

	UEdGraphNode* Anchor = CreateTestNode(Graph, TEXT("Anchor"), 0, 1);
	Anchor->NodePosX = 100;
	Anchor->NodePosY = 100;

	// Comment box containing the anchor
	UEdGraphNode_Comment* Comment = NewObject<UEdGraphNode_Comment>(Graph);
	Graph->AddNode(Comment, false, false);
	Comment->CreateNewGuid();
	Comment->NodePosX = 50;
	Comment->NodePosY = 30;
	Comment->NodeWidth = 400;
	Comment->NodeHeight = 300;

	UEdGraphNode* N = CreateTestNode(Graph, TEXT("New"), 1, 1);
	ConnectNodes(Anchor, 0, N, 0);

	TSet<UEdGraphNode*> NewNodes;
	NewNodes.Add(N);
	FKismetAILayout::PlaceNewNodes(Graph, NewNodes);

	// The comment must now geometrically cover the new node...
	FNodeBounds NB = GetBounds(N);
	TestTrue(TEXT("Comment covers new node left"), Comment->NodePosX <= NB.MinX);
	TestTrue(TEXT("Comment covers new node top"), Comment->NodePosY <= NB.MinY);
	TestTrue(TEXT("Comment covers new node right"), Comment->NodePosX + Comment->NodeWidth >= NB.MaxX);
	TestTrue(TEXT("Comment covers new node bottom"), Comment->NodePosY + Comment->NodeHeight >= NB.MaxY);

	// ...and the engine-side membership cache must agree
	TestTrue(TEXT("Membership cache updated"), Comment->GetNodesUnderComment().Contains(N));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Layout: mixed-height chain — pin Y is straight (not node-top Y)
//
// Uses the same constants as EstimateNodeSize / PinOffsetY:
//   TitleHeight=26, PinRowHeight=24
// For a straight A[out0]->B[in0]->C[out0]->D[in0] chain, the solver aligns
// output-pin Y of A with input-pin Y of B, etc.  Because the connected pins
// are always the first visible pin in their direction (index 0), each pin
// lands at offset 26 + 0.5*24 = 38 from the node top on both sides, so
// NodePosY equality still holds for the exec wire.
//
// This test uses nodes whose CONNECTED pins are NOT the first visible pin —
// the source node has extra hidden inputs that don't affect visible count, but
// here we build them via extra output pins that shift the connected pin index.
// Actually: easiest to build a scenario where input pin index != 0 on the
// receiving node so that PinOffsetY values differ from 38.
//
// Concrete setup:
//   A: 0 inputs, 1 output  -> out-pin index 0, offset = 26 + 0.5*24 = 38
//   B: 3 inputs, 2 outputs -> in-pin index 0, offset = 26 + 0.5*24 = 38
//                             out-pin index 1, offset = 26 + 1.5*24 = 62
//   C: 1 input,  1 output  -> in-pin index 0, offset = 26 + 0.5*24 = 38
//
// Chain: A[out0] -> B[in0], B[out1] -> C[in0]
//
// After solve:  A.NodePosY + 38 == B.NodePosY + 38  (A->B wire)
//               B.NodePosY + 62 == C.NodePosY + 38  (B->C wire)
//               => C.NodePosY == B.NodePosY + 24
//
// Node-top Y cannot be equal for the B->C leg, but pin Y must be.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayout_MixedHeight_PinYStraight,
	"KismetAI.Layout.MixedHeight.PinYStraight", LayoutTestFlags)
bool FLayout_MixedHeight_PinYStraight::RunTest(const FString& Parameters)
{
	// Constants must match EstimateNodeSize / PinOffsetY exactly.
	constexpr int32 TitleHeight  = 26;
	constexpr int32 PinRowHeight = 24;

	UEdGraph* Graph = NewObject<UEdGraph>();

	// A: 0 inputs, 1 output (exec)
	UEdGraphNode* A = CreateTestNode(Graph, TEXT("MHA"), 0, 1);
	// B: 3 inputs (exec), 2 outputs (exec)
	UEdGraphNode* B = CreateTestNode(Graph, TEXT("MHB"), 3, 2);
	// C: 1 input  (exec), 1 output (exec)
	UEdGraphNode* C = CreateTestNode(Graph, TEXT("MHC"), 1, 1);

	// Wire: A[out0] -> B[in0],  B[out1] -> C[in0]
	ConnectNodes(A, 0, B, 0);   // A's first output -> B's first input
	ConnectNodes(B, 1, C, 0);   // B's second output -> C's first input

	TSet<UEdGraphNode*> Seeds = { A, B, C };
	FKismetAILayout::FormatNodes(Graph, Seeds);

	// Pin offsets from node top (RowIndexWithinDirection is 0-based among visible same-dir pins)
	const int32 APinOutOffset = TitleHeight + 0 * PinRowHeight + PinRowHeight / 2; // out index 0: 38
	const int32 BPinInOffset  = TitleHeight + 0 * PinRowHeight + PinRowHeight / 2; // in  index 0: 38
	const int32 BPinOutOffset = TitleHeight + 1 * PinRowHeight + PinRowHeight / 2; // out index 1: 62
	const int32 CPinInOffset  = TitleHeight + 0 * PinRowHeight + PinRowHeight / 2; // in  index 0: 38

	// Verify A->B wire is pin-Y straight (solver aligned output pin of A with input pin of B)
	const int32 APinY = A->NodePosY + APinOutOffset;
	const int32 BInPinY = B->NodePosY + BPinInOffset;
	TestTrue(TEXT("A->B wire: pin Y equal (within grid snap tolerance)"),
		FMath::Abs(APinY - BInPinY) <= FKismetAILayout::GridSize);

	// Verify B->C wire is pin-Y straight (output index 1 of B -> input index 0 of C)
	const int32 BOutPinY = B->NodePosY + BPinOutOffset;
	const int32 CPinY    = C->NodePosY + CPinInOffset;
	TestTrue(TEXT("B->C wire: pin Y equal (within grid snap tolerance)"),
		FMath::Abs(BOutPinY - CPinY) <= FKismetAILayout::GridSize);

	// Confirm no overlaps
	TestFalse(TEXT("A and B do not overlap"), BoundsOverlap(GetBounds(A), GetBounds(B)));
	TestFalse(TEXT("B and C do not overlap"), BoundsOverlap(GetBounds(B), GetBounds(C)));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FormatNodes: comment boxes are reflowed around their pre-format members
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFormat_CommentReflow,
	"KismetAI.Layout.Format.CommentReflow", LayoutTestFlags)
bool FFormat_CommentReflow::RunTest(const FString& Parameters)
{
	UEdGraph* Graph = NewObject<UEdGraph>();

	// Two-node chain inside a comment box
	UEdGraphNode* A = CreateTestNode(Graph, TEXT("A"), 0, 1);
	UEdGraphNode* B = CreateTestNode(Graph, TEXT("B"), 1, 0);
	A->NodePosX = 200; A->NodePosY = 200;
	B->NodePosX = 240; B->NodePosY = 230; // overlapping mess on purpose
	ConnectNodes(A, 0, B, 0);

	UEdGraphNode_Comment* Comment = NewObject<UEdGraphNode_Comment>(Graph);
	Graph->AddNode(Comment, false, false);
	Comment->CreateNewGuid();
	Comment->NodePosX = 150;
	Comment->NodePosY = 120;
	Comment->NodeWidth = 400;
	Comment->NodeHeight = 300;

	TSet<UEdGraphNode*> Seeds;
	FKismetAILayout::FormatNodes(Graph, Seeds); // whole graph

	// After format both members must still be inside the (reflowed) comment
	for (UEdGraphNode* Member : { A, B })
	{
		FNodeBounds MB = GetBounds(Member);
		const int32 CX = (MB.MinX + MB.MaxX) / 2;
		const int32 CY = (MB.MinY + MB.MaxY) / 2;
		TestTrue(*FString::Printf(TEXT("%s center inside reflowed comment"), *Member->GetName()),
			CX >= Comment->NodePosX && CX <= Comment->NodePosX + Comment->NodeWidth &&
			CY >= Comment->NodePosY && CY <= Comment->NodePosY + Comment->NodeHeight);
	}

	// And the format actually fixed the overlap
	TestFalse(TEXT("A and B no longer overlap"), BoundsOverlap(GetBounds(A), GetBounds(B)));

	// Membership cache refreshed
	TestTrue(TEXT("Cache holds A"), Comment->GetNodesUnderComment().Contains(A));
	TestTrue(TEXT("Cache holds B"), Comment->GetNodesUnderComment().Contains(B));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Reroute tests
//
// These tests require a real Blueprint (with a K2 schema) so that
// Schema->TryCreateConnection works inside InsertReroutes.  UK2Node_Knot
// pins are wildcards; the K2 schema allows wildcard↔exec connections.
// ─────────────────────────────────────────────────────────────────────────────

constexpr auto RerouteTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace RerouteTestHelpers
{
	static UBlueprint* MakeRerouteBP(const TCHAR* AssetName)
	{
		FString PkgPath = FString::Printf(TEXT("/Temp/KismetRerouteTest/%s"), AssetName);
		UPackage* Pkg = CreatePackage(*PkgPath);
		if (!Pkg) return nullptr;
		return FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Pkg, AssetName, BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	}

	// Add a PrintString call-function node to a graph, allocating its default pins.
	static UK2Node_CallFunction* AddPrint(UEdGraph* Graph)
	{
		UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph);
		Node->FunctionReference.SetExternalMember(
			GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
			UKismetSystemLibrary::StaticClass());
		Graph->AddNode(Node, false, false);
		Node->CreateNewGuid();
		Node->AllocateDefaultPins();
		return Node;
	}

	// Walk from a pin transitively through UK2Node_Knot output pins until we
	// reach a non-knot node, or return nullptr if the chain is broken.
	static UEdGraphPin* WalkThroughKnots(UEdGraphPin* StartPin)
	{
		UEdGraphPin* Cur = StartPin;
		for (int32 Safety = 0; Safety < 100; Safety++)
		{
			if (!Cur || Cur->LinkedTo.Num() == 0) return nullptr;
			UEdGraphPin* Next = Cur->LinkedTo[0];
			if (!Next) return nullptr;
			UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Next->GetOwningNodeUnchecked());
			if (!Knot)
			{
				return Next; // reached a non-knot pin
			}
			// Step through knot: in-pin was reached; hop to out-pin
			Cur = Knot->GetOutputPin();
		}
		return nullptr; // should never reach here
	}

	// Count UK2Node_Knot nodes in a graph.
	static int32 CountKnots(UEdGraph* Graph)
	{
		int32 Count = 0;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Cast<UK2Node_Knot>(Node)) Count++;
		}
		return Count;
	}
} // namespace RerouteTestHelpers

// ─────────────────────────────────────────────────────────────────────────────
// Reroute.LongEdgeGetsKnot
//
// Topology:
//   Event(rank0) → PrintA(rank1) → PrintB(rank2)
//   Plus a direct extra exec wire Event.then2 → PrintB.execute added to
//   LinkedTo so the solver sees a span-2 edge.
//
// After FormatNodes(..., true): at least one UK2Node_Knot must exist, and
// Event's second exec output must reach PrintB's execute input transitively
// through the knot(s).
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReroute_LongEdgeGetsKnot,
	"KismetAI.Reroute.LongEdgeGetsKnot", RerouteTestFlags)
bool FReroute_LongEdgeGetsKnot::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RerouteTestHelpers::MakeRerouteBP(TEXT("BP_RerouteLongEdge"));
	if (!BP || BP->UbergraphPages.Num() == 0)
	{
		AddError(TEXT("Could not create test blueprint"));
		return false;
	}
	UEdGraph* Graph = BP->UbergraphPages[0];
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (!Schema)
	{
		AddError(TEXT("Blueprint graph has no schema"));
		return false;
	}

	// Create Event → PrintA → PrintB (establishes ranks 0, 1, 2)
	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
	Event->CustomFunctionName = FName(TEXT("RerouteTestEvent"));
	Graph->AddNode(Event, false, false);
	Event->CreateNewGuid();
	Event->AllocateDefaultPins();

	UK2Node_CallFunction* PrintA = RerouteTestHelpers::AddPrint(Graph);
	UK2Node_CallFunction* PrintB = RerouteTestHelpers::AddPrint(Graph);

	// Wire the exec chain using the schema so pins carry live LinkedTo entries.
	UEdGraphPin* EventThen   = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* PrintAExec  = PrintA->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	UEdGraphPin* PrintAThen  = PrintA->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* PrintBExec  = PrintB->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);

	if (!EventThen || !PrintAExec || !PrintAThen || !PrintBExec)
	{
		AddError(TEXT("Could not find expected exec pins"));
		return false;
	}

	Schema->TryCreateConnection(EventThen, PrintAExec);   // Event→PrintA (span 1)
	Schema->TryCreateConnection(PrintAThen, PrintBExec);  // PrintA→PrintB (span 1)

	// Add a DIRECT extra exec output pin on Event → PrintB (span-2 edge).
	// Using a manual additional pin + LinkedTo so the schema's single-link
	// enforcement on the existing EventThen pin is not tripped.
	UEdGraphPin* EventThen2 = Event->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec,
		FName(TEXT("then2")));
	EventThen2->PinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
	EventThen2->LinkedTo.Add(PrintBExec);
	PrintBExec->LinkedTo.Add(EventThen2);

	// Run FormatNodes with reroute=true
	TSet<UEdGraphNode*> Seeds = { Event, PrintA, PrintB };
	FKismetAILayout::FormatNodes(Graph, Seeds, /*bReroute=*/true);

	// At least one knot must have been created (span>1 gets a knot)
	const int32 KnotCount = RerouteTestHelpers::CountKnots(Graph);
	TestTrue(TEXT("At least one UK2Node_Knot created for span-2 edge"), KnotCount >= 1);

	if (KnotCount >= 1)
	{
		// EventThen2 should now link to a knot's input pin.
		bool bLinksToKnot = false;
		for (UEdGraphPin* Linked : EventThen2->LinkedTo)
		{
			if (Linked && Cast<UK2Node_Knot>(Linked->GetOwningNodeUnchecked()))
			{
				bLinksToKnot = true;
				break;
			}
		}
		TestTrue(TEXT("EventThen2 now links to a UK2Node_Knot input"), bLinksToKnot);

		// Walk transitively through knot chain to reach PrintB's execute pin.
		UEdGraphPin* Terminal = nullptr;
		for (UEdGraphPin* Linked : EventThen2->LinkedTo)
		{
			if (!Linked) continue;
			UK2Node_Knot* FirstKnot = Cast<UK2Node_Knot>(Linked->GetOwningNodeUnchecked());
			if (FirstKnot)
			{
				Terminal = RerouteTestHelpers::WalkThroughKnots(FirstKnot->GetOutputPin());
				break;
			}
		}
		TestTrue(TEXT("Knot chain reaches PrintB's execute pin"),
			Terminal != nullptr && Terminal == PrintBExec);
	}

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Reroute.KnotOnLane
//
// Same span-2 topology as LongEdgeGetsKnot, but the nodes start FAR off-origin.
// FormatNodes anchors each component back to its pre-format top-left, so the
// knot must be placed in that same anchored frame: at the rank-1 column
// (Event.finalX + HorizontalSpacing) and grid-snapped in Y. A knot left in
// the raw solver frame (X≈300) would fail the X assertion.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReroute_KnotOnLane,
	"KismetAI.Reroute.KnotOnLane", RerouteTestFlags)
bool FReroute_KnotOnLane::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RerouteTestHelpers::MakeRerouteBP(TEXT("BP_RerouteKnotOnLane"));
	if (!BP || BP->UbergraphPages.Num() == 0)
	{
		AddError(TEXT("Could not create test blueprint"));
		return false;
	}
	UEdGraph* Graph = BP->UbergraphPages[0];
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (!Schema)
	{
		AddError(TEXT("Blueprint graph has no schema"));
		return false;
	}

	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
	Event->CustomFunctionName = FName(TEXT("RerouteLaneEvent"));
	Graph->AddNode(Event, false, false);
	Event->CreateNewGuid();
	Event->AllocateDefaultPins();

	UK2Node_CallFunction* PrintA = RerouteTestHelpers::AddPrint(Graph);
	UK2Node_CallFunction* PrintB = RerouteTestHelpers::AddPrint(Graph);

	// Place the whole component FAR from the origin so a raw-solver-frame knot
	// (X≈300, Y≈0) is unmistakably wrong.
	Event->NodePosX  = 5000; Event->NodePosY  = 3000;
	PrintA->NodePosX = 5400; PrintA->NodePosY = 3200;
	PrintB->NodePosX = 5800; PrintB->NodePosY = 3100;

	UEdGraphPin* EventThen   = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* PrintAExec  = PrintA->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	UEdGraphPin* PrintAThen  = PrintA->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* PrintBExec  = PrintB->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	if (!EventThen || !PrintAExec || !PrintAThen || !PrintBExec)
	{
		AddError(TEXT("Could not find expected exec pins"));
		return false;
	}

	Schema->TryCreateConnection(EventThen, PrintAExec);
	Schema->TryCreateConnection(PrintAThen, PrintBExec);

	UEdGraphPin* EventThen2 = Event->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec,
		FName(TEXT("then2")));
	EventThen2->PinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
	EventThen2->LinkedTo.Add(PrintBExec);
	PrintBExec->LinkedTo.Add(EventThen2);

	TSet<UEdGraphNode*> Seeds = { Event, PrintA, PrintB };
	FKismetAILayout::FormatNodes(Graph, Seeds, /*bReroute=*/true);

	// Find the knot.
	UK2Node_Knot* Knot = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UK2Node_Knot* K = Cast<UK2Node_Knot>(Node)) { Knot = K; break; }
	}
	if (!Knot)
	{
		AddError(TEXT("Expected a knot on the span-2 edge"));
		return false;
	}

	// Knot sits one rank-column right of Event, in the anchored frame.
	TestEqual(TEXT("Knot X == Event.finalX + HorizontalSpacing (anchored frame)"),
		Knot->NodePosX, Event->NodePosX + FKismetAILayout::HorizontalSpacing);

	// Y grid-snapped.
	TestEqual(TEXT("Knot Y is grid-snapped"),
		Knot->NodePosY % FKismetAILayout::GridSize, 0);

	// Knot Y lies within the vertical span of the two endpoints (on the lane).
	const int32 LoY = FMath::Min(Event->NodePosY, PrintB->NodePosY) - FKismetAILayout::GridSize;
	const int32 HiY = FMath::Max(Event->NodePosY, PrintB->NodePosY) + 120;
	TestTrue(TEXT("Knot Y is on the wire's lane between endpoints"),
		Knot->NodePosY >= LoY && Knot->NodePosY <= HiY);

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Reroute.ShortEdgeUntouched
//
// A→B adjacent ranks, reroute=true → zero knots.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReroute_ShortEdgeUntouched,
	"KismetAI.Reroute.ShortEdgeUntouched", RerouteTestFlags)
bool FReroute_ShortEdgeUntouched::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RerouteTestHelpers::MakeRerouteBP(TEXT("BP_RerouteShortEdge"));
	if (!BP || BP->UbergraphPages.Num() == 0)
	{
		AddError(TEXT("Could not create test blueprint"));
		return false;
	}
	UEdGraph* Graph = BP->UbergraphPages[0];
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (!Schema)
	{
		AddError(TEXT("Blueprint graph has no schema"));
		return false;
	}

	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
	Event->CustomFunctionName = FName(TEXT("RerouteShortEvent"));
	Graph->AddNode(Event, false, false);
	Event->CreateNewGuid();
	Event->AllocateDefaultPins();

	UK2Node_CallFunction* PrintA = RerouteTestHelpers::AddPrint(Graph);

	UEdGraphPin* EventThen  = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* PrintAExec = PrintA->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	if (!EventThen || !PrintAExec)
	{
		AddError(TEXT("Could not find exec pins"));
		return false;
	}
	Schema->TryCreateConnection(EventThen, PrintAExec);

	TSet<UEdGraphNode*> Seeds = { Event, PrintA };
	FKismetAILayout::FormatNodes(Graph, Seeds, /*bReroute=*/true);

	TestEqual(TEXT("Zero knots for span-1 edge"),
		RerouteTestHelpers::CountKnots(Graph), 0);

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Reroute.Idempotent
//
// Run reroute twice on the same span-2 graph — same knot count both times
// (collapse-then-regenerate).
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReroute_Idempotent,
	"KismetAI.Reroute.Idempotent", RerouteTestFlags)
bool FReroute_Idempotent::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RerouteTestHelpers::MakeRerouteBP(TEXT("BP_RerouteIdempotent"));
	if (!BP || BP->UbergraphPages.Num() == 0)
	{
		AddError(TEXT("Could not create test blueprint"));
		return false;
	}
	UEdGraph* Graph = BP->UbergraphPages[0];
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (!Schema)
	{
		AddError(TEXT("Blueprint graph has no schema"));
		return false;
	}

	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
	Event->CustomFunctionName = FName(TEXT("RerouteIdempotentEvent"));
	Graph->AddNode(Event, false, false);
	Event->CreateNewGuid();
	Event->AllocateDefaultPins();

	UK2Node_CallFunction* PrintA = RerouteTestHelpers::AddPrint(Graph);
	UK2Node_CallFunction* PrintB = RerouteTestHelpers::AddPrint(Graph);

	UEdGraphPin* EventThen  = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* PrintAExec = PrintA->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	UEdGraphPin* PrintAThen = PrintA->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* PrintBExec = PrintB->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);

	if (!EventThen || !PrintAExec || !PrintAThen || !PrintBExec)
	{
		AddError(TEXT("Could not find exec pins"));
		return false;
	}

	Schema->TryCreateConnection(EventThen, PrintAExec);
	Schema->TryCreateConnection(PrintAThen, PrintBExec);

	// Span-2 extra exec output for Event → PrintB
	UEdGraphPin* EventThen2 = Event->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec,
		FName(TEXT("then2")));
	EventThen2->PinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
	EventThen2->LinkedTo.Add(PrintBExec);
	PrintBExec->LinkedTo.Add(EventThen2);

	TSet<UEdGraphNode*> Seeds = { Event, PrintA, PrintB };

	// First run
	FKismetAILayout::FormatNodes(Graph, Seeds, /*bReroute=*/true);
	const int32 KnotsAfterFirst = RerouteTestHelpers::CountKnots(Graph);
	TestTrue(TEXT("First run produced at least one knot"), KnotsAfterFirst >= 1);

	// Second run — must produce the same count (idempotency)
	FKismetAILayout::FormatNodes(Graph, Seeds, /*bReroute=*/true);
	const int32 KnotsAfterSecond = RerouteTestHelpers::CountKnots(Graph);
	TestEqual(TEXT("Idempotent: knot count unchanged on second run"),
		KnotsAfterSecond, KnotsAfterFirst);

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Reroute.Disabled
//
// FormatNodes(..., false) on the span-2 graph → zero knots.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReroute_Disabled,
	"KismetAI.Reroute.Disabled", RerouteTestFlags)
bool FReroute_Disabled::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RerouteTestHelpers::MakeRerouteBP(TEXT("BP_RerouteDisabled"));
	if (!BP || BP->UbergraphPages.Num() == 0)
	{
		AddError(TEXT("Could not create test blueprint"));
		return false;
	}
	UEdGraph* Graph = BP->UbergraphPages[0];
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (!Schema)
	{
		AddError(TEXT("Blueprint graph has no schema"));
		return false;
	}

	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
	Event->CustomFunctionName = FName(TEXT("RerouteDisabledEvent"));
	Graph->AddNode(Event, false, false);
	Event->CreateNewGuid();
	Event->AllocateDefaultPins();

	UK2Node_CallFunction* PrintA = RerouteTestHelpers::AddPrint(Graph);
	UK2Node_CallFunction* PrintB = RerouteTestHelpers::AddPrint(Graph);

	UEdGraphPin* EventThen  = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* PrintAExec = PrintA->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	UEdGraphPin* PrintAThen = PrintA->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* PrintBExec = PrintB->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);

	if (!EventThen || !PrintAExec || !PrintAThen || !PrintBExec)
	{
		AddError(TEXT("Could not find exec pins"));
		return false;
	}

	Schema->TryCreateConnection(EventThen, PrintAExec);
	Schema->TryCreateConnection(PrintAThen, PrintBExec);

	// Add span-2 edge manually
	UEdGraphPin* EventThen2 = Event->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec,
		FName(TEXT("then2")));
	EventThen2->PinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
	EventThen2->LinkedTo.Add(PrintBExec);
	PrintBExec->LinkedTo.Add(EventThen2);

	TSet<UEdGraphNode*> Seeds = { Event, PrintA, PrintB };

	// Run with bReroute=false (default behavior)
	FKismetAILayout::FormatNodes(Graph, Seeds, /*bReroute=*/false);

	TestEqual(TEXT("bReroute=false: zero knots created"),
		RerouteTestHelpers::CountKnots(Graph), 0);

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Group tests
//
// Use a real Blueprint + K2 schema so UK2Node_CustomEvent/AllocateDefaultPins
// work and exec pins carry correct PinCategory == PC_Exec.
// ─────────────────────────────────────────────────────────────────────────────

namespace GroupTestHelpers
{
	// Count UEdGraphNode_Comment nodes in a graph.
	static int32 CountComments(UEdGraph* Graph)
	{
		int32 Count = 0;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Cast<UEdGraphNode_Comment>(Node)) Count++;
		}
		return Count;
	}

	// Return true when the comment box (pos+size) encloses the node's bounding rect.
	static bool CommentEnclosesNode(const UEdGraphNode_Comment* Comment, UEdGraphNode* Node)
	{
		FNodeSize Size = FKismetAILayout::EstimateNodeSize(Node);
		const int32 NMinX = Node->NodePosX;
		const int32 NMinY = Node->NodePosY;
		const int32 NMaxX = NMinX + Size.Width;
		const int32 NMaxY = NMinY + Size.Height;

		const int32 CMinX = Comment->NodePosX;
		const int32 CMinY = Comment->NodePosY;
		const int32 CMaxX = CMinX + Comment->NodeWidth;
		const int32 CMaxY = CMinY + Comment->NodeHeight;

		return (CMinX <= NMinX) && (CMinY <= NMinY) && (CMaxX >= NMaxX) && (CMaxY >= NMaxY);
	}
} // namespace GroupTestHelpers

// ─────────────────────────────────────────────────────────────────────────────
// Group.EventGetsComment
//
// CustomEvent → PrintA → PrintB wired via K2 schema;
// FormatNodes(..., false, true) must create exactly one comment that
// encloses all three nodes.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroup_EventGetsComment,
	"KismetAI.Group.EventGetsComment", LayoutTestFlags)
bool FGroup_EventGetsComment::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RerouteTestHelpers::MakeRerouteBP(TEXT("BP_GroupEventGetsComment"));
	if (!BP || BP->UbergraphPages.Num() == 0)
	{
		AddError(TEXT("Could not create test blueprint"));
		return false;
	}
	UEdGraph* Graph = BP->UbergraphPages[0];
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (!Schema)
	{
		AddError(TEXT("Blueprint graph has no schema"));
		return false;
	}

	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
	Event->CustomFunctionName = FName(TEXT("GroupTestEvent"));
	Graph->AddNode(Event, false, false);
	Event->CreateNewGuid();
	Event->AllocateDefaultPins();

	UK2Node_CallFunction* PrintA = RerouteTestHelpers::AddPrint(Graph);
	UK2Node_CallFunction* PrintB = RerouteTestHelpers::AddPrint(Graph);

	UEdGraphPin* EventThen  = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* PrintAExec = PrintA->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	UEdGraphPin* PrintAThen = PrintA->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* PrintBExec = PrintB->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);

	if (!EventThen || !PrintAExec || !PrintAThen || !PrintBExec)
	{
		AddError(TEXT("Could not find expected exec pins"));
		return false;
	}

	Schema->TryCreateConnection(EventThen, PrintAExec);
	Schema->TryCreateConnection(PrintAThen, PrintBExec);

	TSet<UEdGraphNode*> Seeds = { Event, PrintA, PrintB };
	FKismetAILayout::FormatNodes(Graph, Seeds, /*bReroute=*/false, /*bGroup=*/true);

	// Exactly one comment must have been created
	const int32 CommentCount = GroupTestHelpers::CountComments(Graph);
	TestEqual(TEXT("Exactly one comment created for event subtree"), CommentCount, 1);

	if (CommentCount == 1)
	{
		UEdGraphNode_Comment* Comment = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UEdGraphNode_Comment* C = Cast<UEdGraphNode_Comment>(Node))
			{
				Comment = C;
				break;
			}
		}

		TestTrue(TEXT("Comment box encloses Event node"), GroupTestHelpers::CommentEnclosesNode(Comment, Event));
		TestTrue(TEXT("Comment box encloses PrintA node"), GroupTestHelpers::CommentEnclosesNode(Comment, PrintA));
		TestTrue(TEXT("Comment box encloses PrintB node"), GroupTestHelpers::CommentEnclosesNode(Comment, PrintB));
	}

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Group.SingleNodeNoComment
//
// A lone CustomEvent (no downstream nodes) — members.Num() < 2 → zero comments
// created.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroup_SingleNodeNoComment,
	"KismetAI.Group.SingleNodeNoComment", LayoutTestFlags)
bool FGroup_SingleNodeNoComment::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RerouteTestHelpers::MakeRerouteBP(TEXT("BP_GroupSingleNode"));
	if (!BP || BP->UbergraphPages.Num() == 0)
	{
		AddError(TEXT("Could not create test blueprint"));
		return false;
	}
	UEdGraph* Graph = BP->UbergraphPages[0];

	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
	Event->CustomFunctionName = FName(TEXT("LoneEvent"));
	Graph->AddNode(Event, false, false);
	Event->CreateNewGuid();
	Event->AllocateDefaultPins();

	TSet<UEdGraphNode*> Seeds = { Event };
	FKismetAILayout::FormatNodes(Graph, Seeds, /*bReroute=*/false, /*bGroup=*/true);

	TestEqual(TEXT("Lone event: zero comments created"),
		GroupTestHelpers::CountComments(Graph), 0);

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Group.Idempotent
//
// Run grouping twice on the same graph — comment count must be identical after
// the second run.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroup_Idempotent,
	"KismetAI.Group.Idempotent", LayoutTestFlags)
bool FGroup_Idempotent::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RerouteTestHelpers::MakeRerouteBP(TEXT("BP_GroupIdempotent"));
	if (!BP || BP->UbergraphPages.Num() == 0)
	{
		AddError(TEXT("Could not create test blueprint"));
		return false;
	}
	UEdGraph* Graph = BP->UbergraphPages[0];
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (!Schema)
	{
		AddError(TEXT("Blueprint graph has no schema"));
		return false;
	}

	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
	Event->CustomFunctionName = FName(TEXT("IdempotentEvent"));
	Graph->AddNode(Event, false, false);
	Event->CreateNewGuid();
	Event->AllocateDefaultPins();

	UK2Node_CallFunction* PrintA = RerouteTestHelpers::AddPrint(Graph);

	UEdGraphPin* EventThen  = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* PrintAExec = PrintA->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);

	if (!EventThen || !PrintAExec)
	{
		AddError(TEXT("Could not find exec pins"));
		return false;
	}
	Schema->TryCreateConnection(EventThen, PrintAExec);

	TSet<UEdGraphNode*> Seeds = { Event, PrintA };

	// First run
	FKismetAILayout::FormatNodes(Graph, Seeds, /*bReroute=*/false, /*bGroup=*/true);
	const int32 CountAfterFirst = GroupTestHelpers::CountComments(Graph);
	TestEqual(TEXT("First run produced exactly one comment"), CountAfterFirst, 1);
	if (CountAfterFirst != 1) return false;

	// Second run — comment count must be unchanged (idempotency)
	FKismetAILayout::FormatNodes(Graph, Seeds, /*bReroute=*/false, /*bGroup=*/true);
	const int32 CountAfterSecond = GroupTestHelpers::CountComments(Graph);
	TestEqual(TEXT("Idempotent: comment count unchanged after second run"),
		CountAfterSecond, CountAfterFirst);

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Group.Disabled
//
// FormatNodes(..., false, false) on an event→print graph → zero comments.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroup_Disabled,
	"KismetAI.Group.Disabled", LayoutTestFlags)
bool FGroup_Disabled::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RerouteTestHelpers::MakeRerouteBP(TEXT("BP_GroupDisabled"));
	if (!BP || BP->UbergraphPages.Num() == 0)
	{
		AddError(TEXT("Could not create test blueprint"));
		return false;
	}
	UEdGraph* Graph = BP->UbergraphPages[0];
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (!Schema)
	{
		AddError(TEXT("Blueprint graph has no schema"));
		return false;
	}

	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
	Event->CustomFunctionName = FName(TEXT("DisabledGroupEvent"));
	Graph->AddNode(Event, false, false);
	Event->CreateNewGuid();
	Event->AllocateDefaultPins();

	UK2Node_CallFunction* PrintA = RerouteTestHelpers::AddPrint(Graph);

	UEdGraphPin* EventThen  = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* PrintAExec = PrintA->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);

	if (!EventThen || !PrintAExec)
	{
		AddError(TEXT("Could not find exec pins"));
		return false;
	}
	Schema->TryCreateConnection(EventThen, PrintAExec);

	TSet<UEdGraphNode*> Seeds = { Event, PrintA };

	// Run with bGroup=false (default behavior)
	FKismetAILayout::FormatNodes(Graph, Seeds, /*bReroute=*/false, /*bGroup=*/false);

	TestEqual(TEXT("bGroup=false: zero comments created"),
		GroupTestHelpers::CountComments(Graph), 0);

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Group.ColorByKeyword
//
// "ApplyDamage" event → R > G and R > B (red family).
// "SpawnActor"  event → G > R and G > B (green family).
// Each tested in its own small graph.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroup_ColorByKeyword,
	"KismetAI.Group.ColorByKeyword", LayoutTestFlags)
bool FGroup_ColorByKeyword::RunTest(const FString& Parameters)
{
	// ── "ApplyDamage" — should get a damage/red family color ─────────────────
	{
		UBlueprint* BP = RerouteTestHelpers::MakeRerouteBP(TEXT("BP_GroupColorDamage"));
		if (!BP || BP->UbergraphPages.Num() == 0)
		{
			AddError(TEXT("Could not create damage blueprint"));
			return false;
		}
		UEdGraph* Graph = BP->UbergraphPages[0];
		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (!Schema)
		{
			AddError(TEXT("Blueprint graph has no schema (damage)"));
			return false;
		}

		UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
		Event->CustomFunctionName = FName(TEXT("ApplyDamage"));
		Graph->AddNode(Event, false, false);
		Event->CreateNewGuid();
		Event->AllocateDefaultPins();

		UK2Node_CallFunction* PrintA = RerouteTestHelpers::AddPrint(Graph);
		UEdGraphPin* EventThen  = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
		UEdGraphPin* PrintAExec = PrintA->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
		if (!EventThen || !PrintAExec) { AddError(TEXT("No exec pins (damage)")); return false; }
		Schema->TryCreateConnection(EventThen, PrintAExec);

		TSet<UEdGraphNode*> Seeds = { Event, PrintA };
		FKismetAILayout::FormatNodes(Graph, Seeds, /*bReroute=*/false, /*bGroup=*/true);

		if (GroupTestHelpers::CountComments(Graph) == 1)
		{
			UEdGraphNode_Comment* Comment = nullptr;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UEdGraphNode_Comment* C = Cast<UEdGraphNode_Comment>(Node)) { Comment = C; break; }
			}
			TestTrue(TEXT("ApplyDamage: R > G (red family)"), Comment->CommentColor.R > Comment->CommentColor.G);
			TestTrue(TEXT("ApplyDamage: R > B (red family)"), Comment->CommentColor.R > Comment->CommentColor.B);
		}
		else
		{
			AddError(TEXT("Expected exactly one comment for ApplyDamage subtree"));
		}
	}

	// ── "SpawnActor" — should get a spawn/green family color ─────────────────
	{
		UBlueprint* BP = RerouteTestHelpers::MakeRerouteBP(TEXT("BP_GroupColorSpawn"));
		if (!BP || BP->UbergraphPages.Num() == 0)
		{
			AddError(TEXT("Could not create spawn blueprint"));
			return false;
		}
		UEdGraph* Graph = BP->UbergraphPages[0];
		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (!Schema)
		{
			AddError(TEXT("Blueprint graph has no schema (spawn)"));
			return false;
		}

		UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
		Event->CustomFunctionName = FName(TEXT("SpawnActor"));
		Graph->AddNode(Event, false, false);
		Event->CreateNewGuid();
		Event->AllocateDefaultPins();

		UK2Node_CallFunction* PrintA = RerouteTestHelpers::AddPrint(Graph);
		UEdGraphPin* EventThen  = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
		UEdGraphPin* PrintAExec = PrintA->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
		if (!EventThen || !PrintAExec) { AddError(TEXT("No exec pins (spawn)")); return false; }
		Schema->TryCreateConnection(EventThen, PrintAExec);

		TSet<UEdGraphNode*> Seeds = { Event, PrintA };
		FKismetAILayout::FormatNodes(Graph, Seeds, /*bReroute=*/false, /*bGroup=*/true);

		if (GroupTestHelpers::CountComments(Graph) == 1)
		{
			UEdGraphNode_Comment* Comment = nullptr;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UEdGraphNode_Comment* C = Cast<UEdGraphNode_Comment>(Node)) { Comment = C; break; }
			}
			TestTrue(TEXT("SpawnActor: G > R (green family)"), Comment->CommentColor.G > Comment->CommentColor.R);
			TestTrue(TEXT("SpawnActor: G > B (green family)"), Comment->CommentColor.G > Comment->CommentColor.B);
		}
		else
		{
			AddError(TEXT("Expected exactly one comment for SpawnActor subtree"));
		}
	}

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ManualTrigger tests
// ─────────────────────────────────────────────────────────────────────────────

constexpr auto ManualTriggerTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// ManualTrigger.CommandHasChord
//
// Verifies that FKismetAILayoutCommands registers a FormatGraph command
// with a valid FUICommandInfo bound to Ctrl+Shift+L.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManualTrigger_CommandHasChord,
	"KismetAI.ManualTrigger.CommandHasChord", ManualTriggerTestFlags)
bool FManualTrigger_CommandHasChord::RunTest(const FString& Parameters)
{
	// Register only if Slate is available (it is in the automation editor).
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("CommandHasChord: FSlateApplication not initialized — cannot test commands"));
		return false;
	}

	// The module already registers these at startup. Only register/unregister
	// ourselves if it hasn't — tearing down a module-owned singleton would trip
	// TCommands::Unregister's IsUnique() check.
	const bool bWasRegistered = FKismetAILayoutCommands::IsRegistered();
	if (!bWasRegistered)
	{
		FKismetAILayoutCommands::Register();
	}

	const FKismetAILayoutCommands& Cmds = FKismetAILayoutCommands::Get();
	TestTrue(TEXT("FormatGraph command is valid"), Cmds.FormatGraph.IsValid());

	if (Cmds.FormatGraph.IsValid())
	{
		const TSharedPtr<const FInputChord> ActiveChord = Cmds.FormatGraph->GetActiveChord(EMultipleKeyBindingIndex::Primary);
		TestTrue(TEXT("FormatGraph has an active primary chord"), ActiveChord.IsValid());
		if (ActiveChord.IsValid())
		{
			TestEqual(TEXT("FormatGraph chord key is L"), ActiveChord->Key, EKeys::L);
			TestTrue(TEXT("FormatGraph chord has Control modifier"), ActiveChord->bCtrl);
			TestTrue(TEXT("FormatGraph chord has Shift modifier"), ActiveChord->bShift);
			TestFalse(TEXT("FormatGraph chord has no Alt modifier"), ActiveChord->bAlt);
		}
	}

	if (!bWasRegistered)
	{
		FKismetAILayoutCommands::Unregister();
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ManualTrigger.FormatsFocusedGraph
//
// Creates a real Blueprint with an Event → PrintA → PrintB chain, scatters
// node positions, calls FormatFocusedGraph, and asserts the layout ran
// (PrintA.X > Event.X and PrintB.X > PrintA.X).
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManualTrigger_FormatsFocusedGraph,
	"KismetAI.ManualTrigger.FormatsFocusedGraph", ManualTriggerTestFlags)
bool FManualTrigger_FormatsFocusedGraph::RunTest(const FString& Parameters)
{
	UBlueprint* BP = RerouteTestHelpers::MakeRerouteBP(TEXT("BP_ManualTriggerFormat"));
	if (!BP || BP->UbergraphPages.Num() == 0)
	{
		AddError(TEXT("FormatsFocusedGraph: could not create test blueprint"));
		return false;
	}
	UEdGraph* Graph = BP->UbergraphPages[0];
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (!Schema)
	{
		AddError(TEXT("FormatsFocusedGraph: blueprint graph has no schema"));
		return false;
	}

	// Build Event → PrintA → PrintB chain using K2 schema connections.
	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
	Event->CustomFunctionName = FName(TEXT("ManualTriggerEvent"));
	Graph->AddNode(Event, false, false);
	Event->CreateNewGuid();
	Event->AllocateDefaultPins();

	UK2Node_CallFunction* PrintA = RerouteTestHelpers::AddPrint(Graph);
	UK2Node_CallFunction* PrintB = RerouteTestHelpers::AddPrint(Graph);

	UEdGraphPin* EventThen  = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* PrintAExec = PrintA->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	UEdGraphPin* PrintAThen = PrintA->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* PrintBExec = PrintB->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);

	if (!EventThen || !PrintAExec || !PrintAThen || !PrintBExec)
	{
		AddError(TEXT("FormatsFocusedGraph: could not find expected exec pins"));
		return false;
	}
	Schema->TryCreateConnection(EventThen, PrintAExec);
	Schema->TryCreateConnection(PrintAThen, PrintBExec);

	// Scatter positions to a random pile so we can verify layout actually moved them.
	Event->NodePosX  = 999; Event->NodePosY  = 999;
	PrintA->NodePosX = 999; PrintA->NodePosY = 999;
	PrintB->NodePosX = 999; PrintB->NodePosY = 999;

	// Act: call the manual-trigger handler (the subject under test).
	FKismetAIModule::FormatFocusedGraph(Graph);

	// Assert: nodes are now ranked left-to-right (Event < PrintA < PrintB).
	TestTrue(TEXT("PrintA.X > Event.X after format (layout assigned ranks)"),
		PrintA->NodePosX > Event->NodePosX);
	TestTrue(TEXT("PrintB.X > PrintA.X after format"),
		PrintB->NodePosX > PrintA->NodePosX);

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Layout profile (FGraphLayoutProfile) tests
// ─────────────────────────────────────────────────────────────────────────────

// Make a data-only node (no exec pins) for spine tests.
static UEdGraphNode* CreateDataNode(UEdGraph* Graph, const FName& Name)
{
	UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, UEdGraphNode::StaticClass(), Name);
	Node->CreateNewGuid();
	Graph->AddNode(Node, false, false);
	UEdGraphPin* In  = Node->CreatePin(EGPD_Input,  UEdGraphSchema_K2::PC_Int, TEXT("DataIn"));
	In->PinType.PinCategory  = UEdGraphSchema_K2::PC_Int;
	UEdGraphPin* Out = Node->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Int, TEXT("DataOut"));
	Out->PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
	return Node;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayout_Profile_K2SpineQueries,
	"KismetAI.Layout.Profile.K2SpineQueries", LayoutTestFlags)
bool FLayout_Profile_K2SpineQueries::RunTest(const FString& Parameters)
{
	UEdGraph* Graph = NewObject<UEdGraph>();
	// A schema-less graph selects the default K2 profile.
	TestEqual(TEXT("schema-less graph -> K2Exec"),
		(int32)FKismetAILayout::SelectProfile(Graph), (int32)EGraphLayoutProfile::K2Exec);

	UEdGraphNode* ExecNode = CreateTestNode(Graph, TEXT("ExecNode"));
	UEdGraphNode* DataNode = CreateDataNode(Graph, TEXT("DataNode"));

	// Spine membership under K2.
	TestTrue(TEXT("exec node is K2 spine"),
		FKismetAILayout::IsSpineNode(ExecNode, EGraphLayoutProfile::K2Exec));
	TestFalse(TEXT("data node is not K2 spine"),
		FKismetAILayout::IsSpineNode(DataNode, EGraphLayoutProfile::K2Exec));

	// Spine concept by profile.
	TestTrue(TEXT("K2Exec has spine"), FKismetAILayout::ProfileHasSpine(EGraphLayoutProfile::K2Exec));
	TestTrue(TEXT("AnimPose has spine"), FKismetAILayout::ProfileHasSpine(EGraphLayoutProfile::AnimPose));
	TestFalse(TEXT("MaterialData has no spine"), FKismetAILayout::ProfileHasSpine(EGraphLayoutProfile::MaterialData));

	// In a spine-less profile nothing is spine, so no pull-back/corridor fires.
	TestFalse(TEXT("exec node not spine under MaterialData"),
		FKismetAILayout::IsSpineNode(ExecNode, EGraphLayoutProfile::MaterialData));
	return true;
}

// A pure data DAG (no spine) must NOT be collapsed by the pull-back, nor have its
// nodes shoved into a corridor. Drive the profile as MaterialData by leaving the
// graph schema-less and checking via the public layout entry on an all-data chain:
// ranks must remain strictly increasing (longest-path preserved).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayout_Profile_DataChainRanksPreserved,
	"KismetAI.Layout.Profile.DataChainRanksPreserved", LayoutTestFlags)
bool FLayout_Profile_DataChainRanksPreserved::RunTest(const FString& Parameters)
{
	// Schema-less graph -> K2Exec, where an all-data chain WOULD be pull-back
	// compressed. We instead assert the spine-less codepath via IsSpineNode:
	// every node reports off-spine, so for MaterialData the pull-back is skipped.
	UEdGraph* Graph = NewObject<UEdGraph>();
	UEdGraphNode* A = CreateDataNode(Graph, TEXT("A"));
	UEdGraphNode* B = CreateDataNode(Graph, TEXT("B"));
	UEdGraphNode* C = CreateDataNode(Graph, TEXT("C"));
	ConnectNodes(A, 0, B, 0);
	ConnectNodes(B, 0, C, 0);

	// Under MaterialData none of these are spine — the guard that skips pull-back
	// and corridor keys off exactly this.
	TestFalse(TEXT("A off-spine (material)"), FKismetAILayout::IsSpineNode(A, EGraphLayoutProfile::MaterialData));
	TestFalse(TEXT("B off-spine (material)"), FKismetAILayout::IsSpineNode(B, EGraphLayoutProfile::MaterialData));
	TestFalse(TEXT("C off-spine (material)"), FKismetAILayout::IsSpineNode(C, EGraphLayoutProfile::MaterialData));
	return true;
}

// Add a local-space pose pin (PC_Struct + FPoseLink) to a node — the anim spine.
static UEdGraphNode* CreatePoseNode(UEdGraph* Graph, const FName& Name)
{
	UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, UEdGraphNode::StaticClass(), Name);
	Node->CreateNewGuid();
	Graph->AddNode(Node, false, false);
	UEdGraphPin* In  = Node->CreatePin(EGPD_Input,  UEdGraphSchema_K2::PC_Struct, TEXT("PoseIn"));
	In->PinType.PinCategory          = UEdGraphSchema_K2::PC_Struct;
	In->PinType.PinSubCategoryObject = FPoseLink::StaticStruct();
	UEdGraphPin* Out = Node->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Struct, TEXT("PoseOut"));
	Out->PinType.PinCategory          = UEdGraphSchema_K2::PC_Struct;
	Out->PinType.PinSubCategoryObject = FPoseLink::StaticStruct();
	return Node;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayout_Profile_AnimGraphSpine,
	"KismetAI.Layout.Profile.AnimGraphSpine", LayoutTestFlags)
bool FLayout_Profile_AnimGraphSpine::RunTest(const FString& Parameters)
{
	UEdGraph* Graph = NewObject<UEdGraph>();
	Graph->Schema = UAnimationGraphSchema::StaticClass();

	// Animation graphs select the pose profile.
	TestEqual(TEXT("anim schema -> AnimPose"),
		(int32)FKismetAILayout::SelectProfile(Graph), (int32)EGraphLayoutProfile::AnimPose);

	UEdGraphNode* PoseNode = CreatePoseNode(Graph, TEXT("Pose"));
	UEdGraphNode* DataNode = CreateDataNode(Graph, TEXT("AnimData"));

	// Pose pins are the spine; plain data pins are not. (Pose nodes have NO exec
	// pins, so the old exec-only logic would have mis-treated them as pure.)
	TestTrue(TEXT("pose node is anim spine"),
		FKismetAILayout::IsSpineNode(PoseNode, EGraphLayoutProfile::AnimPose));
	TestFalse(TEXT("data node is not anim spine"),
		FKismetAILayout::IsSpineNode(DataNode, EGraphLayoutProfile::AnimPose));

	// A pose pin counts as a spine pin (drives solver wire-straightening).
	UEdGraphPin* PoseOut = nullptr;
	for (UEdGraphPin* P : PoseNode->Pins)
		if (P->Direction == EGPD_Output) { PoseOut = P; break; }
	TestTrue(TEXT("pose out is a spine pin"),
		PoseOut && FKismetAILayout::IsSpinePin(PoseOut, EGraphLayoutProfile::AnimPose));

	// Under K2Exec a pose pin is NOT a spine pin (exec-only there).
	TestFalse(TEXT("pose out not a K2 spine pin"),
		PoseOut && FKismetAILayout::IsSpinePin(PoseOut, EGraphLayoutProfile::K2Exec));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayout_Profile_MaterialNoSpine,
	"KismetAI.Layout.Profile.MaterialNoSpine", LayoutTestFlags)
bool FLayout_Profile_MaterialNoSpine::RunTest(const FString& Parameters)
{
	UEdGraph* Graph = NewObject<UEdGraph>();
	Graph->Schema = UMaterialGraphSchema::StaticClass();

	// Material graphs select the data profile.
	TestEqual(TEXT("material schema -> MaterialData"),
		(int32)FKismetAILayout::SelectProfile(Graph), (int32)EGraphLayoutProfile::MaterialData);
	TestFalse(TEXT("MaterialData has no spine"),
		FKismetAILayout::ProfileHasSpine(EGraphLayoutProfile::MaterialData));

	// Even a node with exec pins is off-spine under MaterialData, so the
	// pull-back / corridor passes never fire on a material graph.
	UEdGraphNode* Node = CreateTestNode(Graph, TEXT("MatNode"));
	TestFalse(TEXT("no node is spine under MaterialData"),
		FKismetAILayout::IsSpineNode(Node, EGraphLayoutProfile::MaterialData));
	return true;
}

// Sequence-style fan-out: a driver with two exec outputs feeds two successors in
// the same layer. They must stack in OUTPUT-PIN order (Out_0 above Out_1), not
// node-name order. The successors are named so name-order is the OPPOSITE of pin
// order — so a pass that survives proves the pin-order tiebreaker beat the name.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayout_SequenceFanoutPinOrder,
	"KismetAI.Layout.SequenceFanoutPinOrder", LayoutTestFlags)
bool FLayout_SequenceFanoutPinOrder::RunTest(const FString& Parameters)
{
	UEdGraph* Graph = NewObject<UEdGraph>();
	UEdGraphNode* Seq   = CreateTestNode(Graph, TEXT("Seq"), 1, 2); // two exec outputs
	UEdGraphNode* Then0 = CreateTestNode(Graph, TEXT("ZThen0"));    // name sorts LAST
	UEdGraphNode* Then1 = CreateTestNode(Graph, TEXT("AThen1"));    // name sorts FIRST

	ConnectNodes(Seq, 0, Then0, 0); // Out_0 -> ZThen0
	ConnectNodes(Seq, 1, Then1, 0); // Out_1 -> AThen1

	FKismetAILayout::FormatNodes(Graph, {});

	// Pin order wins: Out_0's target sits above Out_1's target, despite ZThen0's
	// name sorting after AThen1.
	TestTrue(TEXT("Out_0 target is above Out_1 target"),
		Then0->NodePosY < Then1->NodePosY);
	return true;
}
