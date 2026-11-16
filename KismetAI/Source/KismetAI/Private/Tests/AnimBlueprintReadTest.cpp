// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

// Automation tests for Animation Blueprint graph addressing and read (describe/overview) helpers.
// These build a transient UAnimBlueprint with a state machine (two states + a transition)
// programmatically and exercise FindGraph path resolution plus DescribeAnimGraph / AppendAnimOverview.

#include "KismetAIModule.h"
#include "KismetAIUtils.h"
#include "Misc/AutomationTest.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "AnimationGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationStateMachineSchema.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimStateEntryNode.h"

namespace AnimTestHelpers
{
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	// A built anim BP fixture: the blueprint, its anim graph, the state machine node + graph, and two
	// states with a transition between them.
	struct FAnimFixture
	{
		UAnimBlueprint* AnimBP = nullptr;
		UEdGraph* AnimGraph = nullptr;
		UAnimGraphNode_StateMachine* SMNode = nullptr;
		UAnimationStateMachineGraph* SMGraph = nullptr;
		UAnimStateNode* StateA = nullptr;
		UAnimStateNode* StateB = nullptr;
		UAnimStateTransitionNode* Transition = nullptr;
		bool bValid = false;
	};

	static FAnimFixture BuildFixture()
	{
		FAnimFixture F;

		// Create a transient Animation Blueprint (UAnimInstance parent, no skeleton — works headless).
		// A unique name avoids the CreateBlueprint duplicate-name assert when the suite runs the fixture
		// more than once in the same process.
		const FName BPName = MakeUniqueObjectName(GetTransientPackage(), UAnimBlueprint::StaticClass(), TEXT("ABP_KismetAITest"));
		UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			UAnimInstance::StaticClass(),
			GetTransientPackage(),
			BPName,
			BPTYPE_Normal,
			UAnimBlueprint::StaticClass(),
			UAnimBlueprintGeneratedClass::StaticClass()));
		if (!AnimBP)
		{
			return F;
		}
		F.AnimBP = AnimBP;

		// The anim graph is auto-created in FunctionGraphs.
		UEdGraph* AnimGraph = KismetAIUtils::FindAnimGraph(AnimBP);
		if (!AnimGraph)
		{
			return F;
		}
		F.AnimGraph = AnimGraph;

		// Place a state machine node in the anim graph.
		UAnimGraphNode_StateMachine* SMNode = NewObject<UAnimGraphNode_StateMachine>(AnimGraph);
		AnimGraph->AddNode(SMNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		SMNode->CreateNewGuid();
		SMNode->PostPlacedNewNode();   // creates EditorStateMachineGraph + EntryNode, adds to SubGraphs
		SMNode->AllocateDefaultPins();
		F.SMNode = SMNode;

		UAnimationStateMachineGraph* SMGraph = SMNode->EditorStateMachineGraph;
		if (!SMGraph)
		{
			return F;
		}
		F.SMGraph = SMGraph;

		// Spawn two states. SpawnNodeFromTemplate -> PerformAction creates each state's bound graph
		// (UAnimationStateGraph) + its UAnimGraphNode_StateResult.
		UAnimStateNode* StateA = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateNode>(
			SMGraph, NewObject<UAnimStateNode>(SMGraph), FVector2f(100.f, 100.f), false);
		UAnimStateNode* StateB = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateNode>(
			SMGraph, NewObject<UAnimStateNode>(SMGraph), FVector2f(400.f, 100.f), false);
		if (!StateA || !StateB)
		{
			return F;
		}
		F.StateA = StateA;
		F.StateB = StateB;

		// Wire entry -> StateA so the initial state is well-defined.
		if (SMGraph->EntryNode)
		{
			if (UEdGraphPin* EntryPin = KismetAIUtils::GetStateEntryOutputPin(SMGraph->EntryNode))
			{
				if (UEdGraphPin* StatePin = StateA->GetInputPin())
				{
					EntryPin->MakeLinkTo(StatePin);
				}
			}
		}

		// Create a transition StateA -> StateB.
		UAnimStateTransitionNode* Trans = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateTransitionNode>(
			SMGraph, NewObject<UAnimStateTransitionNode>(SMGraph), FVector2f(250.f, 100.f), false);
		if (!Trans)
		{
			return F;
		}
		Trans->CreateConnections(StateA, StateB);
		Trans->CrossfadeDuration = 0.25f;
		Trans->PriorityOrder = 3;
		F.Transition = Trans;

		F.bValid = true;
		return F;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// FindAnimGraph + default-graph semantics
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimDefaultGraphIsAnimGraph, "KismetAI.Anim.FindGraph.DefaultIsAnimGraph", AnimTestHelpers::Flags)
bool FAnimDefaultGraphIsAnimGraph::RunTest(const FString& Parameters)
{
	AnimTestHelpers::FAnimFixture F = AnimTestHelpers::BuildFixture();
	TestTrue(TEXT("Anim fixture built"), F.bValid);
	if (!F.bValid) return false;

	UEdGraph* AnimGraph = KismetAIUtils::FindAnimGraph(F.AnimBP);
	TestNotNull(TEXT("FindAnimGraph returns the anim graph"), AnimGraph);
	TestTrue(TEXT("Anim graph is a UAnimationGraph"), Cast<UAnimationGraph>(AnimGraph) != nullptr);

	// Empty graph name on an anim BP defaults to the AnimGraph (not UbergraphPages[0]).
	UEdGraph* Default = KismetAIUtils::FindGraph(F.AnimBP, TEXT(""));
	TestEqual(TEXT("Empty graph name resolves to AnimGraph for anim BP"), Default, AnimGraph);

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Nested path addressing + recursive bare-name resolution
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimNestedPathResolution, "KismetAI.Anim.FindGraph.NestedPaths", AnimTestHelpers::Flags)
bool FAnimNestedPathResolution::RunTest(const FString& Parameters)
{
	AnimTestHelpers::FAnimFixture F = AnimTestHelpers::BuildFixture();
	TestTrue(TEXT("Anim fixture built"), F.bValid);
	if (!F.bValid) return false;

	const FString SMName = F.SMGraph->GetName();
	const FString StateAName = F.StateA->GetBoundGraph() ? F.StateA->GetBoundGraph()->GetName() : FString();

	// Path "AnimGraph/<StateMachine>".
	{
		const FString Path = FString::Printf(TEXT("AnimGraph/%s"), *SMName);
		UEdGraph* G = KismetAIUtils::FindGraph(F.AnimBP, Path);
		TestNotNull(TEXT("Path to state machine graph resolves"), G);
		TestEqual(TEXT("Resolved graph is the SM graph"), G, (UEdGraph*)F.SMGraph);
	}

	// Path "AnimGraph/<StateMachine>/<StateBoundGraph>".
	{
		const FString Path = FString::Printf(TEXT("AnimGraph/%s/%s"), *SMName, *StateAName);
		UEdGraph* G = KismetAIUtils::FindGraph(F.AnimBP, Path);
		TestNotNull(TEXT("Path to state bound graph resolves"), G);
		TestEqual(TEXT("Resolved graph is StateA bound graph"), G, F.StateA->GetBoundGraph());
	}

	// Bare unique nested name resolves recursively (the state machine graph name is unique).
	{
		UEdGraph* G = KismetAIUtils::FindGraph(F.AnimBP, SMName);
		TestEqual(TEXT("Bare SM graph name resolves recursively"), G, (UEdGraph*)F.SMGraph);
	}

	// Nonexistent path returns null.
	{
		UEdGraph* G = KismetAIUtils::FindGraph(F.AnimBP, TEXT("AnimGraph/NoSuchMachine"));
		TestNull(TEXT("Nonexistent nested path returns null"), G);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimCollectGraphsRecursive, "KismetAI.Anim.FindGraph.CollectRecursive", AnimTestHelpers::Flags)
bool FAnimCollectGraphsRecursive::RunTest(const FString& Parameters)
{
	AnimTestHelpers::FAnimFixture F = AnimTestHelpers::BuildFixture();
	TestTrue(TEXT("Anim fixture built"), F.bValid);
	if (!F.bValid) return false;

	TMap<UEdGraph*, FString> All;
	KismetAIUtils::CollectGraphsRecursive(F.AnimGraph, FString(), All);

	// Must include the anim graph, the SM graph, and both states' bound graphs.
	TestTrue(TEXT("Collected anim graph"), All.Contains(F.AnimGraph));
	TestTrue(TEXT("Collected SM graph"), All.Contains(F.SMGraph));
	TestTrue(TEXT("Collected StateA bound graph"), F.StateA->GetBoundGraph() && All.Contains(F.StateA->GetBoundGraph()));
	TestTrue(TEXT("Collected StateB bound graph"), F.StateB->GetBoundGraph() && All.Contains(F.StateB->GetBoundGraph()));

	// The qualified path for the SM graph starts under the anim graph.
	if (const FString* SMPath = All.Find(F.SMGraph))
	{
		TestTrue(TEXT("SM path is nested under AnimGraph"), SMPath->StartsWith(F.AnimGraph->GetName() + TEXT("/")));
	}

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// DescribeAnimGraph — state machine + pose tree
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimDescribeStateMachine, "KismetAI.Anim.Describe.StateMachine", AnimTestHelpers::Flags)
bool FAnimDescribeStateMachine::RunTest(const FString& Parameters)
{
	AnimTestHelpers::FAnimFixture F = AnimTestHelpers::BuildFixture();
	TestTrue(TEXT("Anim fixture built"), F.bValid);
	if (!F.bValid) return false;

	FKismetAIModule& Module = FKismetAIModule::Get();
	FString Desc;
	const bool bHandled = Module.DescribeAnimGraph(F.AnimBP, F.SMGraph, Desc);
	TestTrue(TEXT("DescribeAnimGraph handles a state machine graph"), bHandled);

	TestTrue(TEXT("Mentions state machine"), Desc.Contains(TEXT("state machine")));
	TestTrue(TEXT("Lists Entry ->"), Desc.Contains(TEXT("Entry ->")));
	TestTrue(TEXT("Lists States section"), Desc.Contains(TEXT("States (")));
	TestTrue(TEXT("Lists Transitions section"), Desc.Contains(TEXT("Transitions (")));

	const FString StateAName = F.StateA->GetStateName();
	const FString StateBName = F.StateB->GetStateName();
	TestTrue(TEXT("Mentions StateA name"), Desc.Contains(StateAName));
	TestTrue(TEXT("Mentions StateB name"), Desc.Contains(StateBName));
	// Transition From -> To with the crossfade we set.
	TestTrue(TEXT("Shows transition arrow"), Desc.Contains(StateAName + TEXT(" -> ") + StateBName));
	TestTrue(TEXT("Shows crossfade duration"), Desc.Contains(TEXT("crossfade=0.25s")));
	TestTrue(TEXT("Shows priority"), Desc.Contains(TEXT("priority=3")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimDescribePoseTree, "KismetAI.Anim.Describe.PoseTree", AnimTestHelpers::Flags)
bool FAnimDescribePoseTree::RunTest(const FString& Parameters)
{
	AnimTestHelpers::FAnimFixture F = AnimTestHelpers::BuildFixture();
	TestTrue(TEXT("Anim fixture built"), F.bValid);
	if (!F.bValid) return false;

	// The main anim graph is a pose graph with a Root sink node.
	FKismetAIModule& Module = FKismetAIModule::Get();
	FString Desc;
	const bool bHandled = Module.DescribeAnimGraph(F.AnimBP, F.AnimGraph, Desc);
	TestTrue(TEXT("DescribeAnimGraph handles the anim graph"), bHandled);
	TestTrue(TEXT("Pose tree header present"), Desc.Contains(TEXT("Pose tree")) || Desc.Contains(TEXT("no result")));

	// A regular function graph should NOT be treated as an anim graph.
	UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
		F.AnimBP, TEXT("PlainFn"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph(F.AnimBP, FuncGraph, /*bIsUserCreated=*/true, static_cast<UFunction*>(nullptr));
	FString FnDesc;
	const bool bFnHandled = Module.DescribeAnimGraph(F.AnimBP, FuncGraph, FnDesc);
	TestFalse(TEXT("Plain function graph is not handled by DescribeAnimGraph"), bFnHandled);

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// AppendAnimOverview
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimOverview, "KismetAI.Anim.Overview.Hierarchy", AnimTestHelpers::Flags)
bool FAnimOverview::RunTest(const FString& Parameters)
{
	AnimTestHelpers::FAnimFixture F = AnimTestHelpers::BuildFixture();
	TestTrue(TEXT("Anim fixture built"), F.bValid);
	if (!F.bValid) return false;

	FKismetAIModule& Module = FKismetAIModule::Get();
	FString Desc;
	Module.AppendAnimOverview(F.AnimBP, Desc);

	TestTrue(TEXT("Overview has Animation section"), Desc.Contains(TEXT("### Animation")));
	TestTrue(TEXT("Overview reports target skeleton (None template)"), Desc.Contains(TEXT("Target Skeleton")));
	TestTrue(TEXT("Overview reports the anim graph"), Desc.Contains(TEXT("Anim Graph")));
	TestTrue(TEXT("Overview reports the state machine"), Desc.Contains(TEXT("State Machine:")));
	TestTrue(TEXT("Overview lists states"), Desc.Contains(F.StateA->GetStateName()));

	// AppendAnimOverview is a no-op for ordinary Blueprints.
	const FName PlainName = MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), TEXT("BP_PlainOverviewTest"));
	UBlueprint* PlainBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), GetTransientPackage(), PlainName,
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	FString PlainDesc;
	Module.AppendAnimOverview(PlainBP, PlainDesc);
	TestTrue(TEXT("No anim section for ordinary BP"), PlainDesc.IsEmpty());

	return true;
}
