// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

// Automation tests for manage_state_machine (Phase 3): build a full locomotion state machine through
// the real HTTP handler (add_state_machine -> add_state -> set_entry_state -> add_transition ->
// rename_state -> delete_state), verify bound-graph addresses resolve via FindGraph, the describe
// output reflects the states/transitions, and the Blueprint compiles clean. Plus negative cases
// (duplicate state, transition between nonexistent states, entry->conduit behavior).
//
// These drive the handler via ExecuteHandlerDirect against a UAnimBlueprint in a named /Temp package
// so FindBlueprint (StaticLoadObject) resolves it.

#include "Misc/AutomationTest.h"
#include "KismetAIModule.h"
#include "KismetAIUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "AnimationStateMachineGraph.h"
#include "AnimStateConduitNode.h"
#include "AnimStateEntryNode.h"

namespace AnimSMTestHelpers
{
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	struct FFixture
	{
		UAnimBlueprint* AnimBP = nullptr;
		FString BPPath;
		bool bValid = false;
	};

	static FFixture BuildFixture(const TCHAR* PkgName)
	{
		FFixture F;
		UPackage* Pkg = CreatePackage(PkgName);
		if (!Pkg) return F;
		const FName BPName = MakeUniqueObjectName(Pkg, UAnimBlueprint::StaticClass(), TEXT("ABP_KismetSMTest"));
		UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			UAnimInstance::StaticClass(), Pkg, BPName, BPTYPE_Normal,
			UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass()));
		if (!AnimBP) return F;
		F.AnimBP = AnimBP;
		if (!KismetAIUtils::FindAnimGraph(AnimBP)) return F;
		F.BPPath = AnimBP->GetPathName();
		F.bValid = true;
		return F;
	}

	static TSharedPtr<FJsonObject> RunHandler(
		bool (FKismetAIModule::*Handler)(const FHttpServerRequest&, const FHttpResultCallback&),
		TSharedPtr<FJsonObject> Body)
	{
		FString Resp = FKismetAIModule::Get().ExecuteHandlerDirect(Handler, Body);
		Resp.RemoveFromStart(TEXT("<tool_use_error>"));
		Resp.RemoveFromEnd(TEXT("</tool_use_error>"));
		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp);
		FJsonSerializer::Deserialize(Reader, Parsed);
		return Parsed;
	}

	static const TSharedPtr<FJsonObject>* GetData(const TSharedPtr<FJsonObject>& Env)
	{
		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (Env.IsValid() && Env->TryGetObjectField(TEXT("data"), Data)) return Data;
		return nullptr;
	}

	// Build a single-op JSON body for the SM handler.
	static TSharedPtr<FJsonObject> MakeBody(const FString& BPPath, const FString& SMAddress, TArray<TSharedPtr<FJsonValue>> Ops)
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("blueprint"), BPPath);
		if (!SMAddress.IsEmpty()) Body->SetStringField(TEXT("state_machine"), SMAddress);
		Body->SetArrayField(TEXT("operations"), Ops);
		return Body;
	}

	static TSharedPtr<FJsonValue> Op(TSharedPtr<FJsonObject> O) { return MakeShared<FJsonValueObject>(O); }

	// Run one op, return the first result object (results[0]).
	static TSharedPtr<FJsonObject> RunOp(const FString& BPPath, const FString& SMAddress, TSharedPtr<FJsonObject> OpObj, double* OutSucceeded = nullptr)
	{
		TArray<TSharedPtr<FJsonValue>> Ops; Ops.Add(Op(OpObj));
		TSharedPtr<FJsonObject> Resp = RunHandler(&FKismetAIModule::HandleManageStateMachine, MakeBody(BPPath, SMAddress, Ops));
		const TSharedPtr<FJsonObject>* Data = GetData(Resp);
		if (!Data) return nullptr;
		if (OutSucceeded) (*Data)->TryGetNumberField(TEXT("operations_succeeded"), *OutSucceeded);
		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		if ((*Data)->TryGetArrayField(TEXT("results"), Results) && Results->Num() > 0)
			return (*Results)[0]->AsObject();
		return nullptr;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Happy path: full 3-state locomotion SM, rename, delete, address resolution,
// describe, and clean compile.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimSM_HappyPath, "KismetAI.Anim.SM.HappyPath", AnimSMTestHelpers::Flags)
bool FAnimSM_HappyPath::RunTest(const FString& Parameters)
{
	using namespace AnimSMTestHelpers;
	FFixture F = BuildFixture(TEXT("/Temp/KismetAnimSMHappy"));
	TestTrue(TEXT("fixture built"), F.bValid);
	if (!F.bValid) return false;

	// 1) Create the state machine.
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("add_state_machine"));
		O->SetStringField(TEXT("name"), TEXT("Locomotion"));
		double S = 0; TSharedPtr<FJsonObject> R = RunOp(F.BPPath, FString(), O, &S);
		TestEqual(TEXT("add_state_machine succeeded"), (int32)S, 1);
		TestTrue(TEXT("add_state_machine returned result"), R.IsValid());
		if (R) { FString Addr; R->TryGetStringField(TEXT("graph_address"), Addr); TestEqual(TEXT("SM address"), Addr, FString(TEXT("AnimGraph/Locomotion"))); }
	}

	const FString SM = TEXT("AnimGraph/Locomotion");

	// 2) Add three states.
	for (const TCHAR* Name : { TEXT("Idle"), TEXT("Walk"), TEXT("Run") })
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("add_state"));
		O->SetStringField(TEXT("name"), Name);
		double S = 0; TSharedPtr<FJsonObject> R = RunOp(F.BPPath, SM, O, &S);
		TestEqual(FString::Printf(TEXT("add_state %s succeeded"), Name), (int32)S, 1);
		if (R)
		{
			FString Addr; R->TryGetStringField(TEXT("graph_address"), Addr);
			const FString Expected = FString::Printf(TEXT("AnimGraph/Locomotion/%s"), Name);
			TestEqual(FString::Printf(TEXT("state %s address"), Name), Addr, Expected);
			// The returned bound-graph address must resolve via FindGraph.
			UEdGraph* G = KismetAIUtils::FindGraph(F.AnimBP, Addr);
			TestNotNull(FString::Printf(TEXT("state %s bound graph resolves"), Name), G);
		}
	}

	// 3) Set entry state to Idle.
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("set_entry_state"));
		O->SetStringField(TEXT("state"), TEXT("Idle"));
		double S = 0; RunOp(F.BPPath, SM, O, &S);
		TestEqual(TEXT("set_entry_state succeeded"), (int32)S, 1);
	}

	// 4) Add transitions: Idle->Walk (crossfade+priority), Walk->Run (automatic_rule), Run->Idle.
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("add_transition"));
		O->SetStringField(TEXT("from"), TEXT("Idle"));
		O->SetStringField(TEXT("to"), TEXT("Walk"));
		O->SetNumberField(TEXT("crossfade_duration"), 0.3);
		O->SetNumberField(TEXT("priority_order"), 2);
		double S = 0; TSharedPtr<FJsonObject> R = RunOp(F.BPPath, SM, O, &S);
		TestEqual(TEXT("add_transition Idle->Walk succeeded"), (int32)S, 1);
		if (R)
		{
			FString Addr; R->TryGetStringField(TEXT("graph_address"), Addr);
			TestTrue(TEXT("transition rule graph address returned"), !Addr.IsEmpty());
			UEdGraph* G = KismetAIUtils::FindGraph(F.AnimBP, Addr);
			TestNotNull(TEXT("transition rule graph resolves"), G);
		}
	}
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("add_transition"));
		O->SetStringField(TEXT("from"), TEXT("Walk"));
		O->SetStringField(TEXT("to"), TEXT("Run"));
		O->SetBoolField(TEXT("automatic_rule"), true);
		double S = 0; RunOp(F.BPPath, SM, O, &S);
		TestEqual(TEXT("add_transition Walk->Run succeeded"), (int32)S, 1);
	}
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("add_transition"));
		O->SetStringField(TEXT("from"), TEXT("Run"));
		O->SetStringField(TEXT("to"), TEXT("Idle"));
		double S = 0; RunOp(F.BPPath, SM, O, &S);
		TestEqual(TEXT("add_transition Run->Idle succeeded"), (int32)S, 1);
	}

	// 5) Rename Walk -> Walking; the new bound-graph address must resolve, the old must not.
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("rename_state"));
		O->SetStringField(TEXT("state"), TEXT("Walk"));
		O->SetStringField(TEXT("new_name"), TEXT("Walking"));
		double S = 0; TSharedPtr<FJsonObject> R = RunOp(F.BPPath, SM, O, &S);
		TestEqual(TEXT("rename_state succeeded"), (int32)S, 1);

		UEdGraph* NewG = KismetAIUtils::FindGraph(F.AnimBP, TEXT("AnimGraph/Locomotion/Walking"));
		TestNotNull(TEXT("renamed state bound graph resolves at new address"), NewG);
		UEdGraph* OldG = KismetAIUtils::FindGraph(F.AnimBP, TEXT("AnimGraph/Locomotion/Walk"));
		TestNull(TEXT("old state address no longer resolves"), OldG);
	}

	// 6) Delete Run; its transitions (Walking->Run created as Walk->Run, and Run->Idle) must be removed.
	//    Note Walk->Run endpoints update with the rename, so it is now Walking->Run.
	UEdGraph* SMG = KismetAIUtils::FindGraph(F.AnimBP, SM);
	TestNotNull(TEXT("SM graph resolves"), SMG);
	int32 TransitionsBefore = 0;
	if (UAnimationStateMachineGraph* SMTyped = Cast<UAnimationStateMachineGraph>(SMG))
	{
		for (UEdGraphNode* N : SMTyped->Nodes) if (N && N->GetClass()->GetName().Contains(TEXT("Transition"))) TransitionsBefore++;
	}
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("delete_state"));
		O->SetStringField(TEXT("state"), TEXT("Run"));
		double S = 0; TSharedPtr<FJsonObject> R = RunOp(F.BPPath, SM, O, &S);
		TestEqual(TEXT("delete_state succeeded"), (int32)S, 1);
		if (R)
		{
			double Removed = 0; R->TryGetNumberField(TEXT("removed_transitions"), Removed);
			TestEqual(TEXT("delete_state removed the 2 transitions touching Run"), (int32)Removed, 2);
		}
	}
	// After delete: only the Idle->Walking transition should remain; no transition references Run.
	if (UAnimationStateMachineGraph* SMTyped = Cast<UAnimationStateMachineGraph>(SMG))
	{
		int32 TransitionsAfter = 0;
		bool bAnyRefRun = false;
		for (UEdGraphNode* N : SMTyped->Nodes)
		{
			if (N && N->GetClass()->GetName().Contains(TEXT("Transition")))
			{
				TransitionsAfter++;
			}
		}
		TestEqual(TEXT("only Idle->Walking transition remains"), TransitionsAfter, 1);
		TestEqual(TEXT("two transitions were removed by delete_state"), TransitionsBefore - TransitionsAfter, 2);
		(void)bAnyRefRun;
		// The Run state graph address must no longer resolve.
		TestNull(TEXT("deleted Run address no longer resolves"), KismetAIUtils::FindGraph(F.AnimBP, TEXT("AnimGraph/Locomotion/Run")));
	}

	// 7) describe output reflects the SM (states + the surviving transition).
	{
		FString Desc;
		const bool bHandled = FKismetAIModule::Get().DescribeAnimGraph(F.AnimBP, SMG, Desc);
		TestTrue(TEXT("DescribeAnimGraph handles the SM graph"), bHandled);
		TestTrue(TEXT("describe mentions Idle"), Desc.Contains(TEXT("Idle")));
		TestTrue(TEXT("describe mentions Walking"), Desc.Contains(TEXT("Walking")));
		TestFalse(TEXT("describe no longer mentions Run state"), Desc.Contains(TEXT("- Run:")));
		TestTrue(TEXT("describe shows Idle -> Walking transition"), Desc.Contains(TEXT("Idle -> Walking")));
	}

	// 8) Compile clean (no ERRORS; warnings acceptable). Empty states + an entry + one transition
	//    with a default (false) rule must compile without errors.
	{
		FCompilerResultsLog Log;
		FKismetEditorUtilities::CompileBlueprint(F.AnimBP, EBlueprintCompileOptions::None, &Log);
		TestTrue(TEXT("Blueprint compiles without errors"), F.AnimBP->Status != BS_Error);
		TestEqual(TEXT("no compile errors"), Log.NumErrors, 0);
	}

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Negative: duplicate state name; transition between nonexistent states.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimSM_Negative, "KismetAI.Anim.SM.Negative", AnimSMTestHelpers::Flags)
bool FAnimSM_Negative::RunTest(const FString& Parameters)
{
	using namespace AnimSMTestHelpers;
	FFixture F = BuildFixture(TEXT("/Temp/KismetAnimSMNeg"));
	TestTrue(TEXT("fixture built"), F.bValid);
	if (!F.bValid) return false;

	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("add_state_machine"));
		O->SetStringField(TEXT("name"), TEXT("SM"));
		double S = 0; RunOp(F.BPPath, FString(), O, &S);
		TestEqual(TEXT("add_state_machine succeeded"), (int32)S, 1);
	}
	const FString SM = TEXT("AnimGraph/SM");

	// First Idle succeeds.
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("add_state"));
		O->SetStringField(TEXT("name"), TEXT("Idle"));
		double S = 0; RunOp(F.BPPath, SM, O, &S);
		TestEqual(TEXT("first add Idle succeeds"), (int32)S, 1);
	}
	// Duplicate Idle fails.
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("add_state"));
		O->SetStringField(TEXT("name"), TEXT("Idle"));
		double S = 0; TSharedPtr<FJsonObject> R = RunOp(F.BPPath, SM, O, &S);
		TestEqual(TEXT("duplicate add Idle does not succeed"), (int32)S, 0);
		if (R)
		{
			FString Code; R->TryGetStringField(TEXT("error_code"), Code);
			TestEqual(TEXT("duplicate state error code"), Code, FString(TEXT("STATE_EXISTS")));
		}
	}
	// Transition between nonexistent states fails.
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("add_transition"));
		O->SetStringField(TEXT("from"), TEXT("Nope"));
		O->SetStringField(TEXT("to"), TEXT("AlsoNope"));
		double S = 0; TSharedPtr<FJsonObject> R = RunOp(F.BPPath, SM, O, &S);
		TestEqual(TEXT("transition between nonexistent states fails"), (int32)S, 0);
		if (R)
		{
			FString Code; R->TryGetStringField(TEXT("error_code"), Code);
			TestEqual(TEXT("nonexistent endpoint error code"), Code, FString(TEXT("STATE_NOT_FOUND")));
		}
	}

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Entry -> conduit: document what the schema permits. We add a conduit, set it as the
// entry state, and assert the outcome (the SM schema allows entry to point at a conduit).
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimSM_EntryConduit, "KismetAI.Anim.SM.EntryConduit", AnimSMTestHelpers::Flags)
bool FAnimSM_EntryConduit::RunTest(const FString& Parameters)
{
	using namespace AnimSMTestHelpers;
	FFixture F = BuildFixture(TEXT("/Temp/KismetAnimSMConduit"));
	TestTrue(TEXT("fixture built"), F.bValid);
	if (!F.bValid) return false;

	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("add_state_machine"));
		O->SetStringField(TEXT("name"), TEXT("SM"));
		double S = 0; RunOp(F.BPPath, FString(), O, &S);
		TestEqual(TEXT("add_state_machine succeeded"), (int32)S, 1);
	}
	const FString SM = TEXT("AnimGraph/SM");

	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("add_conduit"));
		O->SetStringField(TEXT("name"), TEXT("Branch"));
		double S = 0; RunOp(F.BPPath, SM, O, &S);
		TestEqual(TEXT("add_conduit succeeded"), (int32)S, 1);
	}

	// Attempt to set the conduit as the entry state. Record the outcome (the schema permits this:
	// the entry node can point at a conduit as a routing branch point).
	bool bEntrySucceeded = false;
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("set_entry_state"));
		O->SetStringField(TEXT("state"), TEXT("Branch"));
		double S = 0; RunOp(F.BPPath, SM, O, &S);
		bEntrySucceeded = ((int32)S == 1);
	}

	// Verify the actual graph wiring: the entry node's output should link to the conduit when the
	// op reported success. We assert handler success and entry linkage are consistent.
	UEdGraph* SMG = KismetAIUtils::FindGraph(F.AnimBP, SM);
	TestNotNull(TEXT("SM graph resolves"), SMG);
	if (UAnimationStateMachineGraph* SMTyped = Cast<UAnimationStateMachineGraph>(SMG))
	{
		int32 EntryLinks = -1;
		if (SMTyped->EntryNode)
		{
			if (UEdGraphPin* Out = KismetAIUtils::GetStateEntryOutputPin(SMTyped->EntryNode))
			{
				EntryLinks = Out->LinkedTo.Num();
			}
		}
		// Documented behavior: set_entry_state to a conduit succeeds and produces a single entry link.
		TestTrue(TEXT("entry->conduit reported success"), bEntrySucceeded);
		TestEqual(TEXT("entry node links to exactly one node (the conduit)"), EntryLinks, 1);
	}

	return true;
}
