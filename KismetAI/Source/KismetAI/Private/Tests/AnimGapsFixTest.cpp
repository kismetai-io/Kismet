// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

// Automation tests for the three "anim gaps" fixes:
//   1. Pure K2 nodes are legal in an anim graph (variable getters, pure functions, knots);
//      impure (exec) nodes are rejected with an exec-rule schema error.
//   2. Transition rule graphs are addressable by endpoints ("SM/From->To", "#N", "@blend"),
//      and manage_state_machine {"action":"get"} returns those addresses; describe prints them.
//   3. Pose-aware placement: a node feeding another node's pose input lands to its LEFT and
//      near it; a pure getter feeding a value pin lands left of its consumer.
//
// These drive the real HTTP handlers via ExecuteHandlerDirect against anim BPs placed in named
// /Temp packages so FindBlueprint (StaticLoadObject) resolves them.

#include "Misc/AutomationTest.h"
#include "KismetAIModule.h"
#include "KismetAIUtils.h"
#include "KismetAILayout.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "BlueprintActionDatabase.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintNodeBinder.h"
#include "K2Node_VariableGet.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "AnimationStateMachineGraph.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_TwoWayBlend.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimStateTransitionNode.h"
#include "AnimationTransitionGraph.h"

namespace AnimGapsTestHelpers
{
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	struct FFixture
	{
		UAnimBlueprint* AnimBP = nullptr;
		UEdGraph* AnimGraph = nullptr;
		FString BPPath;
		bool bValid = false;
	};

	static FFixture BuildFixture(const TCHAR* PkgName)
	{
		FFixture F;
		UPackage* Pkg = CreatePackage(PkgName);
		if (!Pkg) return F;
		const FName BPName = MakeUniqueObjectName(Pkg, UAnimBlueprint::StaticClass(), TEXT("ABP_KismetGapsTest"));
		UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			UAnimInstance::StaticClass(), Pkg, BPName, BPTYPE_Normal,
			UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass()));
		if (!AnimBP) return F;
		F.AnimBP = AnimBP;
		F.AnimGraph = KismetAIUtils::FindAnimGraph(AnimBP);
		if (!F.AnimGraph) return F;
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

	static FString EnvToString(const TSharedPtr<FJsonObject>& Env)
	{
		FString S;
		if (Env.IsValid())
		{
			TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&S);
			FJsonSerializer::Serialize(Env.ToSharedRef(), W);
		}
		return S;
	}

	static UEdGraphNode* SpawnAnimNodeByClass(UEdGraph* Graph, UClass* NodeClass)
	{
		UBlueprintNodeSpawner* Found = nullptr;
		{
			FBlueprintActionDatabase& DB = FBlueprintActionDatabase::Get();
			const FBlueprintActionDatabase::FActionRegistry& All = DB.GetAllActions();
			for (const auto& Pair : All)
			{
				for (UBlueprintNodeSpawner* Spawner : Pair.Value)
				{
					if (Spawner && Spawner->NodeClass == NodeClass) { Found = Spawner; break; }
				}
				if (Found) break;
			}
		}
		if (!Found) return nullptr;
		IBlueprintNodeBinder::FBindingSet Empty;
		return Found->Invoke(Graph, Empty, FVector2D::ZeroVector);
	}

	static UEdGraphPin* FirstPoseInput(UEdGraphNode* Node)
	{
		for (UEdGraphPin* P : Node->Pins)
			if (P && P->Direction == EGPD_Input && UAnimationGraphSchema::IsPosePin(P->PinType)) return P;
		return nullptr;
	}
	static UEdGraphPin* FirstPoseOutput(UEdGraphNode* Node)
	{
		for (UEdGraphPin* P : Node->Pins)
			if (P && P->Direction == EGPD_Output && UAnimationGraphSchema::IsPosePin(P->PinType)) return P;
		return nullptr;
	}

	// Add a bool member variable to the Blueprint (the real-world repro precondition).
	static bool AddBoolVar(UBlueprint* BP, const FString& VarName)
	{
		FEdGraphPinType BoolType;
		BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		return FBlueprintEditorUtils::AddMemberVariable(BP, FName(*VarName), BoolType, TEXT("false"));
	}

	// One-op manage_state_machine call returning results[0].
	static TSharedPtr<FJsonObject> RunSMOp(const FString& BPPath, const FString& SM, TSharedPtr<FJsonObject> OpObj, double* OutSucceeded = nullptr)
	{
		TArray<TSharedPtr<FJsonValue>> Ops; Ops.Add(MakeShared<FJsonValueObject>(OpObj));
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("blueprint"), BPPath);
		if (!SM.IsEmpty()) Body->SetStringField(TEXT("state_machine"), SM);
		Body->SetArrayField(TEXT("operations"), Ops);
		TSharedPtr<FJsonObject> Resp = RunHandler(&FKismetAIModule::HandleManageStateMachine, Body);
		const TSharedPtr<FJsonObject>* Data = GetData(Resp);
		if (!Data) return nullptr;
		if (OutSucceeded) (*Data)->TryGetNumberField(TEXT("operations_succeeded"), *OutSucceeded);
		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		if ((*Data)->TryGetArrayField(TEXT("results"), Results) && Results->Num() > 0)
			return (*Results)[0]->AsObject();
		return nullptr;
	}

	// Build a 3-state SM (Idle, Walk, Run) with Idle entry. Returns the SM address.
	static FString Build3StateSM(const FString& BPPath)
	{
		{
			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("action"), TEXT("add_state_machine"));
			O->SetStringField(TEXT("name"), TEXT("Locomotion"));
			RunSMOp(BPPath, FString(), O);
		}
		const FString SM = TEXT("AnimGraph/Locomotion");
		for (const TCHAR* Name : { TEXT("Idle"), TEXT("Walk"), TEXT("Run") })
		{
			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("action"), TEXT("add_state"));
			O->SetStringField(TEXT("name"), Name);
			RunSMOp(BPPath, SM, O);
		}
		{
			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("action"), TEXT("set_entry_state"));
			O->SetStringField(TEXT("state"), TEXT("Idle"));
			RunSMOp(BPPath, SM, O);
		}
		return SM;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Fix 1 (a): T3D import of a pure VariableGet + Knot into an anim graph is ACCEPTED;
//            an impure node (Branch) is rejected with the exec-rule message.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimGaps_PureK2T3DAccepted, "KismetAI.Anim.Gaps.PureK2T3DAccepted", AnimGapsTestHelpers::Flags)
bool FAnimGaps_PureK2T3DAccepted::RunTest(const FString& Parameters)
{
	using namespace AnimGapsTestHelpers;
	FFixture F = BuildFixture(TEXT("/Temp/KismetGapsPureT3D"));
	TestTrue(TEXT("fixture built"), F.bValid);
	if (!F.bValid) return false;

	// Add a bool member variable so the getter references something real.
	TestTrue(TEXT("added bool var"), AddBoolVar(F.AnimBP, TEXT("bShouldJump")));
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(F.AnimBP);

	const int32 NodesBefore = F.AnimGraph->Nodes.Num();

	// A pure VariableGet + a Knot — both legal in an anim graph (no exec pins).
	const FString PureT3D = TEXT(
		"Begin Object Class=/Script/BlueprintGraph.K2Node_VariableGet Name=\"K2Node_VariableGet_0\"\n"
		"   VariableReference=(MemberName=\"bShouldJump\",bSelfContext=True)\n"
		"   NodeGuid=GUID_GET\n"
		"CustomProperties Pin (PinId=LINK_VALUE,PinName=\"bShouldJump\",Direction=\"EGPD_Output\",PinType.PinCategory=\"bool\")\n"
		"End Object\n"
		"Begin Object Class=/Script/BlueprintGraph.K2Node_Knot Name=\"K2Node_Knot_0\"\n"
		"   NodeGuid=GUID_KNOT\n"
		"CustomProperties Pin (PinId=LINK_KIN,PinName=\"InputPin\",PinType.PinCategory=\"wildcard\")\n"
		"CustomProperties Pin (PinId=LINK_KOUT,PinName=\"OutputPin\",Direction=\"EGPD_Output\",PinType.PinCategory=\"wildcard\")\n"
		"End Object\n");

	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("blueprint"), F.BPPath);
	Body->SetStringField(TEXT("graph"), TEXT("AnimGraph"));
	Body->SetStringField(TEXT("t3d"), PureT3D);
	TSharedPtr<FJsonObject> Resp = RunHandler(&FKismetAIModule::HandleInjectT3D, Body);
	const TSharedPtr<FJsonObject>* Data = GetData(Resp);
	TestTrue(TEXT("pure-node inject returned data (accepted)"), Data != nullptr);

	// The getter must now be present in the anim graph.
	bool bGetPresent = false;
	for (UEdGraphNode* N : F.AnimGraph->Nodes)
		if (Cast<UK2Node_VariableGet>(N)) bGetPresent = true;
	TestTrue(TEXT("pure VariableGet was added to the anim graph"), bGetPresent);
	TestTrue(TEXT("node count grew"), F.AnimGraph->Nodes.Num() > NodesBefore);

	// Compiles without errors.
	{
		FCompilerResultsLog Log;
		FKismetEditorUtilities::CompileBlueprint(F.AnimBP, EBlueprintCompileOptions::None, &Log);
		TestTrue(TEXT("compiles after pure node accepted"), F.AnimBP->Status != BS_Error);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimGaps_ImpureK2T3DRejected, "KismetAI.Anim.Gaps.ImpureK2T3DRejected", AnimGapsTestHelpers::Flags)
bool FAnimGaps_ImpureK2T3DRejected::RunTest(const FString& Parameters)
{
	using namespace AnimGapsTestHelpers;
	FFixture F = BuildFixture(TEXT("/Temp/KismetGapsImpureT3D"));
	TestTrue(TEXT("fixture built"), F.bValid);
	if (!F.bValid) return false;

	const int32 NodesBefore = F.AnimGraph->Nodes.Num();

	// A Branch (K2Node_IfThenElse) has exec pins → illegal in an anim graph.
	const FString ImpureT3D = TEXT(
		"Begin Object Class=/Script/BlueprintGraph.K2Node_IfThenElse Name=\"K2Node_IfThenElse_0\"\n"
		"   NodeGuid=GUID_BRANCH\n"
		"CustomProperties Pin (PinId=LINK_EXEC,PinName=\"execute\",PinType.PinCategory=\"exec\")\n"
		"CustomProperties Pin (PinId=LINK_THEN,PinName=\"then\",Direction=\"EGPD_Output\",PinType.PinCategory=\"exec\")\n"
		"CustomProperties Pin (PinId=LINK_ELSE,PinName=\"else\",Direction=\"EGPD_Output\",PinType.PinCategory=\"exec\")\n"
		"CustomProperties Pin (PinId=LINK_COND,PinName=\"Condition\",PinType.PinCategory=\"bool\")\n"
		"End Object\n");

	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("blueprint"), F.BPPath);
	Body->SetStringField(TEXT("graph"), TEXT("AnimGraph"));
	Body->SetStringField(TEXT("t3d"), ImpureT3D);
	TSharedPtr<FJsonObject> Resp = RunHandler(&FKismetAIModule::HandleInjectT3D, Body);

	bool bBranchPresent = false;
	for (UEdGraphNode* N : F.AnimGraph->Nodes)
		if (N && N->GetClass()->GetName().Contains(TEXT("IfThenElse"))) bBranchPresent = true;
	TestFalse(TEXT("impure Branch was NOT added to the anim graph"), bBranchPresent);
	TestEqual(TEXT("node count unchanged"), F.AnimGraph->Nodes.Num(), NodesBefore);

	// The error must reference the exec/impure rule (SCHEMA_MISMATCH).
	const FString RespStr = EnvToString(Resp);
	TestTrue(TEXT("response cites SCHEMA_MISMATCH"), RespStr.Contains(TEXT("SCHEMA_MISMATCH")));
	TestTrue(TEXT("response explains the exec/impure rule"),
		RespStr.Contains(TEXT("impure")) || RespStr.Contains(TEXT("exec")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Fix 1 (b): insert_node — a pure function ("NOT Boolean") and a Get-variable into an
//            anim graph succeed; wiring the getter into a blend bool pin compiles clean.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimGaps_InsertPureIntoAnim, "KismetAI.Anim.Gaps.InsertPureIntoAnim", AnimGapsTestHelpers::Flags)
bool FAnimGaps_InsertPureIntoAnim::RunTest(const FString& Parameters)
{
	using namespace AnimGapsTestHelpers;
	FFixture F = BuildFixture(TEXT("/Temp/KismetGapsInsertPure"));
	TestTrue(TEXT("fixture built"), F.bValid);
	if (!F.bValid) return false;

	TestTrue(TEXT("added bool var"), AddBoolVar(F.AnimBP, TEXT("bShouldJump")));
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(F.AnimBP);

	auto Insert = [&](const FString& NodeName) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("blueprint"), F.BPPath);
		Body->SetStringField(TEXT("graph"), TEXT("AnimGraph"));
		Body->SetStringField(TEXT("node"), NodeName);
		return RunHandler(&FKismetAIModule::HandleInsertNode, Body);
	};

	// (1) Pure function NOT Boolean — legal in the anim graph now.
	TSharedPtr<FJsonObject> NotResp = Insert(TEXT("NOT Boolean"));
	const TSharedPtr<FJsonObject>* NotData = GetData(NotResp);
	TestTrue(TEXT("'NOT Boolean' insert returned data (pure node accepted)"), NotData != nullptr);

	// (2) Get the bool variable via the "Get <var>" fallback.
	TSharedPtr<FJsonObject> GetResp = Insert(TEXT("Get bShouldJump"));
	const TSharedPtr<FJsonObject>* GetData_ = GetData(GetResp);
	TestTrue(TEXT("'Get bShouldJump' insert returned data"), GetData_ != nullptr);
	FString GetNodeName;
	if (GetData_) (*GetData_)->TryGetStringField(TEXT("node_name"), GetNodeName);
	TestTrue(TEXT("getter node named"), !GetNodeName.IsEmpty());

	// Find the spawned getter and a TwoWayBlend; wire getter.bool -> blend bool pin via edit.
	UK2Node_VariableGet* Getter = nullptr;
	for (UEdGraphNode* N : F.AnimGraph->Nodes) if ((Getter = Cast<UK2Node_VariableGet>(N)) != nullptr) break;
	TestNotNull(TEXT("getter present in anim graph"), Getter);

	UAnimGraphNode_TwoWayBlend* Blend = Cast<UAnimGraphNode_TwoWayBlend>(
		SpawnAnimNodeByClass(F.AnimGraph, UAnimGraphNode_TwoWayBlend::StaticClass()));
	TestNotNull(TEXT("blend spawned"), Blend);
	if (!Getter || !Blend) return false;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(F.AnimBP);

	// Find the blend's bool input pin (bActiveValue / Active Value).
	UEdGraphPin* BoolPin = nullptr;
	for (UEdGraphPin* P : Blend->Pins)
	{
		if (P && P->Direction == EGPD_Input && P->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean
			&& !UAnimationGraphSchema::IsPosePin(P->PinType))
		{
			BoolPin = P; break;
		}
	}
	// Some engine versions hide bActiveValue as a non-pin property by default; expose it if needed.
	if (!BoolPin)
	{
		TSharedPtr<FJsonObject> Vis = MakeShared<FJsonObject>();
		Vis->SetStringField(TEXT("action"), TEXT("set_pin_visibility"));
		Vis->SetStringField(TEXT("node"), Blend->GetName());
		Vis->SetStringField(TEXT("property"), TEXT("bActiveValue"));
		Vis->SetBoolField(TEXT("visible"), true);
		TArray<TSharedPtr<FJsonValue>> Ops; Ops.Add(MakeShared<FJsonValueObject>(Vis));
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("blueprint"), F.BPPath);
		Body->SetStringField(TEXT("graph"), TEXT("AnimGraph"));
		Body->SetArrayField(TEXT("operations"), Ops);
		RunHandler(&FKismetAIModule::HandleEditGraph, Body);
		for (UEdGraphPin* P : Blend->Pins)
		{
			if (P && P->Direction == EGPD_Input && P->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean
				&& !UAnimationGraphSchema::IsPosePin(P->PinType))
			{
				BoolPin = P; break;
			}
		}
	}

	if (BoolPin)
	{
		UEdGraphPin* GetterOut = nullptr;
		for (UEdGraphPin* P : Getter->Pins) if (P && P->Direction == EGPD_Output) { GetterOut = P; break; }
		TestNotNull(TEXT("getter output pin"), GetterOut);
		TSharedPtr<FJsonObject> Conn = MakeShared<FJsonObject>();
		Conn->SetStringField(TEXT("action"), TEXT("connect"));
		Conn->SetStringField(TEXT("source_node"), Getter->GetName());
		Conn->SetStringField(TEXT("source_pin"), GetterOut ? GetterOut->PinName.ToString() : TEXT("bShouldJump"));
		Conn->SetStringField(TEXT("target_node"), Blend->GetName());
		Conn->SetStringField(TEXT("target_pin"), BoolPin->PinName.ToString());
		TArray<TSharedPtr<FJsonValue>> Ops; Ops.Add(MakeShared<FJsonValueObject>(Conn));
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("blueprint"), F.BPPath);
		Body->SetStringField(TEXT("graph"), TEXT("AnimGraph"));
		Body->SetArrayField(TEXT("operations"), Ops);
		RunHandler(&FKismetAIModule::HandleEditGraph, Body);
		TestEqual(TEXT("blend bool pin now driven by the getter"), BoolPin->LinkedTo.Num(), 1);
	}

	// Compile clean.
	{
		FCompilerResultsLog Log;
		FKismetEditorUtilities::CompileBlueprint(F.AnimBP, EBlueprintCompileOptions::None, &Log);
		TestTrue(TEXT("compiles after wiring getter into blend"), F.AnimBP->Status != BS_Error);
	}
	return true;
}

// insert_node of an impure node into an anim graph fails with the exec-rule message.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimGaps_InsertImpureRejected, "KismetAI.Anim.Gaps.InsertImpureRejected", AnimGapsTestHelpers::Flags)
bool FAnimGaps_InsertImpureRejected::RunTest(const FString& Parameters)
{
	using namespace AnimGapsTestHelpers;
	FFixture F = BuildFixture(TEXT("/Temp/KismetGapsInsertImpure"));
	TestTrue(TEXT("fixture built"), F.bValid);
	if (!F.bValid) return false;

	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("blueprint"), F.BPPath);
	Body->SetStringField(TEXT("graph"), TEXT("AnimGraph"));
	Body->SetStringField(TEXT("node"), TEXT("Branch")); // impure exec node
	TSharedPtr<FJsonObject> Resp = RunHandler(&FKismetAIModule::HandleInsertNode, Body);

	// It must not have spawned a Branch in the anim graph.
	bool bBranch = false;
	for (UEdGraphNode* N : F.AnimGraph->Nodes)
		if (N && N->GetClass()->GetName().Contains(TEXT("IfThenElse"))) bBranch = true;
	TestFalse(TEXT("Branch not spawned into anim graph"), bBranch);

	// Error explains the impure/exec rule, NOT "only anim nodes".
	const FString RespStr = EnvToString(Resp);
	TestFalse(TEXT("error does not claim only anim nodes legal"), RespStr.Contains(TEXT("only UAnimGraphNode")));
	TestTrue(TEXT("error explains pure-vs-impure"),
		RespStr.Contains(TEXT("impure")) || RespStr.Contains(TEXT("exec")) || RespStr.Contains(TEXT("pure")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Fix 2: endpoint transition addressing + get op + describe output + opaque rename.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimGaps_EndpointAddressing, "KismetAI.Anim.Gaps.EndpointAddressing", AnimGapsTestHelpers::Flags)
bool FAnimGaps_EndpointAddressing::RunTest(const FString& Parameters)
{
	using namespace AnimGapsTestHelpers;
	FFixture F = BuildFixture(TEXT("/Temp/KismetGapsEndpoint"));
	TestTrue(TEXT("fixture built"), F.bValid);
	if (!F.bValid) return false;

	const FString SM = Build3StateSM(F.BPPath);

	// One Idle->Walk transition.
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("add_transition"));
		O->SetStringField(TEXT("from"), TEXT("Idle"));
		O->SetStringField(TEXT("to"), TEXT("Walk"));
		double S = 0; TSharedPtr<FJsonObject> R = RunSMOp(F.BPPath, SM, O, &S);
		TestEqual(TEXT("add Idle->Walk succeeded"), (int32)S, 1);
		// add_transition returns the endpoint-form rule_graph_address.
		if (R)
		{
			FString Addr; R->TryGetStringField(TEXT("rule_graph_address"), Addr);
			TestEqual(TEXT("rule_graph_address is endpoint form"), Addr, FString(TEXT("AnimGraph/Locomotion/Idle->Walk")));
		}
	}

	// Endpoint addressing resolves.
	UEdGraph* RuleG = KismetAIUtils::FindGraph(F.AnimBP, TEXT("AnimGraph/Locomotion/Idle->Walk"));
	TestNotNull(TEXT("endpoint path resolves to the rule graph"), RuleG);
	// Bare endpoint form resolves too.
	UEdGraph* RuleBare = KismetAIUtils::FindGraph(F.AnimBP, TEXT("Idle->Walk"));
	TestEqual(TEXT("bare endpoint form resolves to same graph"), RuleBare, RuleG);
	// Space-tolerant.
	UEdGraph* RuleSpaced = KismetAIUtils::FindGraph(F.AnimBP, TEXT("AnimGraph/Locomotion/Idle -> Walk"));
	TestEqual(TEXT("spaces around -> tolerated"), RuleSpaced, RuleG);

	// Rename the rule graph to an opaque engine-style name; endpoint addressing still resolves.
	if (RuleG)
	{
		FBlueprintEditorUtils::RenameGraph(RuleG, TEXT("AnimationTransitionGraph_7"));
		UEdGraph* StillResolves = KismetAIUtils::FindGraph(F.AnimBP, TEXT("AnimGraph/Locomotion/Idle->Walk"));
		TestEqual(TEXT("endpoint addressing survives opaque rename"), StillResolves, RuleG);
	}

	// Duplicate Idle->Walk pair → #2 addressing.
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("add_transition"));
		O->SetStringField(TEXT("from"), TEXT("Idle"));
		O->SetStringField(TEXT("to"), TEXT("Walk"));
		double S = 0; RunSMOp(F.BPPath, SM, O, &S);
		TestEqual(TEXT("second Idle->Walk added"), (int32)S, 1);
	}
	UEdGraph* Dup1 = KismetAIUtils::FindGraph(F.AnimBP, TEXT("AnimGraph/Locomotion/Idle->Walk#1"));
	UEdGraph* Dup2 = KismetAIUtils::FindGraph(F.AnimBP, TEXT("AnimGraph/Locomotion/Idle->Walk#2"));
	TestNotNull(TEXT("Idle->Walk#1 resolves"), Dup1);
	TestNotNull(TEXT("Idle->Walk#2 resolves"), Dup2);
	TestTrue(TEXT("#1 and #2 are distinct graphs"), Dup1 != Dup2);
	TestEqual(TEXT("#1 is the first (renamed) graph"), Dup1, RuleG);
	// Ambiguous (no #N) → no resolution, candidates reported.
	{
		TArray<FString> Cand;
		UEdGraph* Ambiguous = KismetAIUtils::FindGraph(F.AnimBP, TEXT("AnimGraph/Locomotion/Idle->Walk"), &Cand);
		TestNull(TEXT("ambiguous endpoint without #N does not resolve"), Ambiguous);
		TestTrue(TEXT("ambiguity reports #N candidates"), Cand.Num() >= 2);
	}

	// get op returns matching endpoint addresses.
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("get"));
		TSharedPtr<FJsonObject> R = RunSMOp(F.BPPath, SM, O);
		TestTrue(TEXT("get op returned a result"), R.IsValid());
		if (R)
		{
			FString Entry; R->TryGetStringField(TEXT("entry_state"), Entry);
			TestEqual(TEXT("get reports entry_state Idle"), Entry, FString(TEXT("Idle")));
			const TArray<TSharedPtr<FJsonValue>>* Trans = nullptr;
			TestTrue(TEXT("get returns transitions array"), R->TryGetArrayField(TEXT("transitions"), Trans));
			bool bHasEndpointAddr = false;
			if (Trans)
			{
				for (const TSharedPtr<FJsonValue>& TV : *Trans)
				{
					const TSharedPtr<FJsonObject> TO = TV->AsObject();
					FString RG; if (TO.IsValid()) TO->TryGetStringField(TEXT("rule_graph"), RG);
					if (RG.Contains(TEXT("Idle->Walk"))) bHasEndpointAddr = true;
				}
			}
			TestTrue(TEXT("get transition rule_graph uses endpoint form"), bHasEndpointAddr);
		}
	}

	// describe output contains the endpoint address string.
	{
		UEdGraph* SMG = KismetAIUtils::FindGraph(F.AnimBP, SM);
		FString Desc;
		FKismetAIModule::Get().DescribeAnimGraph(F.AnimBP, SMG, Desc);
		TestTrue(TEXT("describe prints the endpoint rule address"), Desc.Contains(TEXT("AnimGraph/Locomotion/Idle->Walk")));
	}

	return true;
}

// Custom blend graph via @blend addressing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimGaps_CustomBlendAddress, "KismetAI.Anim.Gaps.CustomBlendAddress", AnimGapsTestHelpers::Flags)
bool FAnimGaps_CustomBlendAddress::RunTest(const FString& Parameters)
{
	using namespace AnimGapsTestHelpers;
	FFixture F = BuildFixture(TEXT("/Temp/KismetGapsBlend"));
	TestTrue(TEXT("fixture built"), F.bValid);
	if (!F.bValid) return false;

	const FString SM = Build3StateSM(F.BPPath);
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("action"), TEXT("add_transition"));
		O->SetStringField(TEXT("from"), TEXT("Idle"));
		O->SetStringField(TEXT("to"), TEXT("Walk"));
		RunSMOp(F.BPPath, SM, O);
	}

	// Find the transition and enable a custom blend graph (LogicType Custom + create graph).
	UEdGraph* SMG = KismetAIUtils::FindGraph(F.AnimBP, SM);
	UAnimationStateMachineGraph* SMTyped = Cast<UAnimationStateMachineGraph>(SMG);
	UAnimStateTransitionNode* Trans = nullptr;
	if (SMTyped)
	{
		for (UEdGraphNode* N : SMTyped->Nodes)
			if ((Trans = Cast<UAnimStateTransitionNode>(N)) != nullptr) break;
	}
	TestNotNull(TEXT("transition node found"), Trans);
	if (!Trans) return false;

	// Switching LogicType to Custom creates the CustomTransitionGraph; the editor drives this
	// through PostEditChangeProperty (CreateCustomTransitionGraph itself is protected). Fire that
	// path with a synthetic property-changed event so this works on 5.6 and 5.7 alike.
	Trans->LogicType = ETransitionLogicType::TLT_Custom;
	if (FProperty* LogicProp = FindFProperty<FProperty>(UAnimStateTransitionNode::StaticClass(), TEXT("LogicType")))
	{
		FPropertyChangedEvent Evt(LogicProp);
		Trans->PostEditChangeProperty(Evt);
	}
	TestNotNull(TEXT("custom transition graph created"), Trans->CustomTransitionGraph.Get());

	// @blend resolves to the custom transition graph; without @blend resolves to the rule graph.
	UEdGraph* BlendG = KismetAIUtils::FindGraph(F.AnimBP, TEXT("AnimGraph/Locomotion/Idle->Walk@blend"));
	TestEqual(TEXT("@blend resolves to the CustomTransitionGraph"), BlendG, Trans->CustomTransitionGraph.Get());
	UEdGraph* RuleG = KismetAIUtils::FindGraph(F.AnimBP, TEXT("AnimGraph/Locomotion/Idle->Walk"));
	TestEqual(TEXT("no @blend resolves to the rule BoundGraph"), RuleG, Trans->BoundGraph.Get());
	TestTrue(TEXT("rule and blend graphs are distinct"), BlendG != RuleG);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Fix 3: pose-aware placement. A node feeding an existing node's pose input lands to
//        its LEFT and within a sane distance (not at origin / far away).
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimGaps_PosePlacement, "KismetAI.Anim.Gaps.PosePlacement", AnimGapsTestHelpers::Flags)
bool FAnimGaps_PosePlacement::RunTest(const FString& Parameters)
{
	using namespace AnimGapsTestHelpers;
	FFixture F = BuildFixture(TEXT("/Temp/KismetGapsPosePlace"));
	TestTrue(TEXT("fixture built"), F.bValid);
	if (!F.bValid) return false;

	// Existing consumer: a TwoWayBlend placed at a known location, with a free pose input.
	UAnimGraphNode_TwoWayBlend* Blend = Cast<UAnimGraphNode_TwoWayBlend>(
		SpawnAnimNodeByClass(F.AnimGraph, UAnimGraphNode_TwoWayBlend::StaticClass()));
	TestNotNull(TEXT("consumer blend spawned"), Blend);
	if (!Blend) return false;
	Blend->NodePosX = 1000;
	Blend->NodePosY = 200;

	// New node: a SequencePlayer whose pose OUTPUT feeds the blend's pose INPUT.
	UAnimGraphNode_SequencePlayer* Player = Cast<UAnimGraphNode_SequencePlayer>(
		SpawnAnimNodeByClass(F.AnimGraph, UAnimGraphNode_SequencePlayer::StaticClass()));
	TestNotNull(TEXT("new player spawned"), Player);
	if (!Player) return false;
	Player->NodePosX = 0;
	Player->NodePosY = 0;

	UEdGraphPin* POut = FirstPoseOutput(Player);
	UEdGraphPin* BIn = FirstPoseInput(Blend);
	TestNotNull(TEXT("player pose out"), POut);
	TestNotNull(TEXT("blend pose in"), BIn);
	if (!POut || !BIn) return false;
	TestTrue(TEXT("wired player -> blend pose"), F.AnimGraph->GetSchema()->TryCreateConnection(POut, BIn));

	// Place ONLY the new player; the blend is an immovable obstacle/anchor.
	TSet<UEdGraphNode*> NewNodes; NewNodes.Add(Player);
	FKismetAILayout::PlaceNewNodes(F.AnimGraph, NewNodes);

	// Pose flows left->right into the sink, so the producer sits to the LEFT of the consumer.
	TestTrue(TEXT("player placed LEFT of the blend it feeds"), Player->NodePosX < Blend->NodePosX);
	// And reasonably near it (not stranded at origin or across the canvas).
	const int32 Dist = FMath::Abs(Blend->NodePosX - Player->NodePosX);
	TestTrue(TEXT("player within a sane distance of the blend (<600)"), Dist < 600);
	TestTrue(TEXT("player not left at origin"), Player->NodePosX != 0);
	// Roughly row-aligned with its consumer.
	TestTrue(TEXT("player roughly row-aligned with the blend"), FMath::Abs(Player->NodePosY - Blend->NodePosY) < 300);
	return true;
}

// A pure getter feeding a value pin lands left of its consumer too (data-pin fallback anchor).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimGaps_DataPinPlacement, "KismetAI.Anim.Gaps.DataPinPlacement", AnimGapsTestHelpers::Flags)
bool FAnimGaps_DataPinPlacement::RunTest(const FString& Parameters)
{
	using namespace AnimGapsTestHelpers;
	FFixture F = BuildFixture(TEXT("/Temp/KismetGapsDataPlace"));
	TestTrue(TEXT("fixture built"), F.bValid);
	if (!F.bValid) return false;

	TestTrue(TEXT("added bool var"), AddBoolVar(F.AnimBP, TEXT("bShouldJump")));
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(F.AnimBP);

	// Existing consumer blend at a known spot, with an exposed bool input pin.
	UAnimGraphNode_TwoWayBlend* Blend = Cast<UAnimGraphNode_TwoWayBlend>(
		SpawnAnimNodeByClass(F.AnimGraph, UAnimGraphNode_TwoWayBlend::StaticClass()));
	TestNotNull(TEXT("consumer blend spawned"), Blend);
	if (!Blend) return false;
	Blend->NodePosX = 1200;
	Blend->NodePosY = 100;

	UEdGraphPin* BoolPin = nullptr;
	for (UEdGraphPin* P : Blend->Pins)
		if (P && P->Direction == EGPD_Input && P->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean
			&& !UAnimationGraphSchema::IsPosePin(P->PinType)) { BoolPin = P; break; }
	if (!BoolPin)
	{
		// Some versions hide bActiveValue; can't reliably test the value wire — skip gracefully.
		AddInfo(TEXT("blend bool pin not exposed by default in this engine version; data-pin placement asserted only when a value pin exists"));
		return true;
	}

	// New getter; wire its bool output into the blend's bool input.
	UK2Node_VariableGet* Getter = NewObject<UK2Node_VariableGet>(F.AnimGraph);
	F.AnimGraph->AddNode(Getter, false, false);
	Getter->VariableReference.SetSelfMember(FName(TEXT("bShouldJump")));
	Getter->CreateNewGuid();
	Getter->PostPlacedNewNode();
	Getter->AllocateDefaultPins();
	Getter->NodePosX = 0;
	Getter->NodePosY = 0;

	UEdGraphPin* GOut = nullptr;
	for (UEdGraphPin* P : Getter->Pins) if (P && P->Direction == EGPD_Output) { GOut = P; break; }
	TestNotNull(TEXT("getter output pin"), GOut);
	if (!GOut) return false;
	TestTrue(TEXT("wired getter -> blend bool pin"), F.AnimGraph->GetSchema()->TryCreateConnection(GOut, BoolPin));

	TSet<UEdGraphNode*> NewNodes; NewNodes.Add(Getter);
	FKismetAILayout::PlaceNewNodes(F.AnimGraph, NewNodes);

	TestTrue(TEXT("getter placed LEFT of its consumer"), Getter->NodePosX < Blend->NodePosX);
	TestTrue(TEXT("getter within sane distance of consumer (<600)"), FMath::Abs(Blend->NodePosX - Getter->NodePosX) < 600);
	TestTrue(TEXT("getter not left at origin"), Getter->NodePosX != 0);
	return true;
}
