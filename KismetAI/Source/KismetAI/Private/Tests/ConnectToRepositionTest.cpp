// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

// Automation tests for the "place near wiring" gap fix:
//   1. insert_node gains connect_to — wire the NEW node's first compatible output
//      into an existing node's input at insert time, so the node lands adjacent to
//      the node it feeds (a real anchor) instead of stranded across the canvas.
//   2. Reposition-on-first-wire — an agent-created node inserted with NO anchor is
//      parked as an island; the first edit_blueprint connect that wires it pulls it
//      next to its peer. One-shot, and pre-existing / user nodes NEVER move.
//
// These drive the real HTTP handlers via ExecuteHandlerDirect against BPs placed in
// named /Temp packages so FindBlueprint (StaticLoadObject) resolves them.

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
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "AnimationGraphSchema.h"
#include "AnimGraphNode_TwoWayBlend.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "BlueprintActionDatabase.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintNodeBinder.h"

namespace ConnectToTestHelpers
{
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

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

	static UBlueprint* MakeActorBP(const TCHAR* PkgName, FString& OutPath)
	{
		UPackage* Pkg = CreatePackage(PkgName);
		if (!Pkg) return nullptr;
		const FName BPName = MakeUniqueObjectName(Pkg, UBlueprint::StaticClass(), TEXT("BP_ConnectTo"));
		UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Pkg, BPName, BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (BP) OutPath = BP->GetPathName();
		return BP;
	}

	static UAnimBlueprint* MakeAnimBP(const TCHAR* PkgName, UEdGraph*& OutAnimGraph, FString& OutPath)
	{
		UPackage* Pkg = CreatePackage(PkgName);
		if (!Pkg) return nullptr;
		const FName BPName = MakeUniqueObjectName(Pkg, UAnimBlueprint::StaticClass(), TEXT("ABP_ConnectTo"));
		UAnimBlueprint* BP = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			UAnimInstance::StaticClass(), Pkg, BPName, BPTYPE_Normal,
			UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass()));
		if (!BP) return nullptr;
		OutAnimGraph = KismetAIUtils::FindAnimGraph(BP);
		OutPath = BP->GetPathName();
		return BP;
	}

	static UEdGraph* EventGraph(UBlueprint* BP)
	{
		return KismetAIUtils::FindGraph(BP, TEXT("EventGraph"));
	}

	static UK2Node_CallFunction* AddPrintString(UEdGraph* Graph)
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

	static UEdGraphNode* SpawnAnimNodeByClass(UEdGraph* Graph, UClass* NodeClass)
	{
		UBlueprintNodeSpawner* Found = nullptr;
		FBlueprintActionDatabase& DB = FBlueprintActionDatabase::Get();
		for (const auto& Pair : DB.GetAllActions())
		{
			for (UBlueprintNodeSpawner* Spawner : Pair.Value)
			{
				if (Spawner && Spawner->NodeClass == NodeClass) { Found = Spawner; break; }
			}
			if (Found) break;
		}
		if (!Found) return nullptr;
		IBlueprintNodeBinder::FBindingSet Empty;
		return Found->Invoke(Graph, Empty, FVector2D::ZeroVector);
	}

	static bool AddBoolVar(UBlueprint* BP, const FString& VarName)
	{
		FEdGraphPinType BoolType;
		BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		return FBlueprintEditorUtils::AddMemberVariable(BP, FName(*VarName), BoolType, TEXT("false"));
	}

	static TSharedPtr<FJsonObject> InsertNode(const FString& BPPath, const FString& GraphName,
		const FString& NodeName, TSharedPtr<FJsonObject> ConnectTo = nullptr, TSharedPtr<FJsonObject> ConnectFrom = nullptr)
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("blueprint"), BPPath);
		Body->SetStringField(TEXT("graph"), GraphName);
		Body->SetStringField(TEXT("node"), NodeName);
		if (ConnectTo.IsValid()) Body->SetObjectField(TEXT("connect_to"), ConnectTo);
		if (ConnectFrom.IsValid()) Body->SetObjectField(TEXT("connect_from"), ConnectFrom);
		return RunHandler(&FKismetAIModule::HandleInsertNode, Body);
	}

	static void Connect(const FString& BPPath, const FString& GraphName,
		const FString& SrcNode, const FString& SrcPin, const FString& TgtNode, const FString& TgtPin)
	{
		TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
		Op->SetStringField(TEXT("action"), TEXT("connect"));
		Op->SetStringField(TEXT("source_node"), SrcNode);
		Op->SetStringField(TEXT("source_pin"), SrcPin);
		Op->SetStringField(TEXT("target_node"), TgtNode);
		Op->SetStringField(TEXT("target_pin"), TgtPin);
		TArray<TSharedPtr<FJsonValue>> Ops; Ops.Add(MakeShared<FJsonValueObject>(Op));
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("blueprint"), BPPath);
		Body->SetStringField(TEXT("graph"), GraphName);
		Body->SetArrayField(TEXT("operations"), Ops);
		RunHandler(&FKismetAIModule::HandleEditGraph, Body);
	}

	static UEdGraphNode* FindNodeByName(UEdGraph* Graph, const FString& Name)
	{
		for (UEdGraphNode* N : Graph->Nodes)
			if (N && N->GetName() == Name) return N;
		return nullptr;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// (a) insert_node connect_to wires getter output -> blend bool pin and the node
//     lands LEFT of the target within a sane distance.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConnectTo_AnimGetterLandsLeft, "KismetAI.ConnectTo.AnimGetterLandsLeft", ConnectToTestHelpers::Flags)
bool FConnectTo_AnimGetterLandsLeft::RunTest(const FString& Parameters)
{
	using namespace ConnectToTestHelpers;
	UEdGraph* AnimGraph = nullptr; FString BPPath;
	UAnimBlueprint* BP = MakeAnimBP(TEXT("/Temp/KismetConnectToAnimA"), AnimGraph, BPPath);
	TestNotNull(TEXT("anim BP built"), BP);
	if (!BP || !AnimGraph) return false;

	TestTrue(TEXT("added bool var"), AddBoolVar(BP, TEXT("bIsInAir")));
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	// Existing consumer blend at a known far-right spot, with an exposed bool pin.
	UAnimGraphNode_TwoWayBlend* Blend = Cast<UAnimGraphNode_TwoWayBlend>(
		SpawnAnimNodeByClass(AnimGraph, UAnimGraphNode_TwoWayBlend::StaticClass()));
	TestNotNull(TEXT("blend spawned"), Blend);
	if (!Blend) return false;
	Blend->NodePosX = 1400;
	Blend->NodePosY = 150;

	// Expose the blend's bool Active Value pin if it isn't a pin by default.
	auto FindBoolPin = [&]() -> UEdGraphPin*
	{
		for (UEdGraphPin* P : Blend->Pins)
			if (P && P->Direction == EGPD_Input && P->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean
				&& !UAnimationGraphSchema::IsPosePin(P->PinType)) return P;
		return nullptr;
	};
	UEdGraphPin* BoolPin = FindBoolPin();
	if (!BoolPin)
	{
		TSharedPtr<FJsonObject> Vis = MakeShared<FJsonObject>();
		Vis->SetStringField(TEXT("action"), TEXT("set_pin_visibility"));
		Vis->SetStringField(TEXT("node"), Blend->GetName());
		Vis->SetStringField(TEXT("property"), TEXT("bActiveValue"));
		Vis->SetBoolField(TEXT("visible"), true);
		TArray<TSharedPtr<FJsonValue>> Ops; Ops.Add(MakeShared<FJsonValueObject>(Vis));
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("blueprint"), BPPath);
		Body->SetStringField(TEXT("graph"), TEXT("AnimGraph"));
		Body->SetArrayField(TEXT("operations"), Ops);
		RunHandler(&FKismetAIModule::HandleEditGraph, Body);
		BoolPin = FindBoolPin();
	}
	if (!BoolPin)
	{
		AddInfo(TEXT("blend bool pin not exposed by default in this engine version; skipping connect_to anim assertion"));
		return true;
	}

	// insert_node "Get bIsInAir" with connect_to feeding the blend's bool pin.
	TSharedPtr<FJsonObject> CT = MakeShared<FJsonObject>();
	CT->SetStringField(TEXT("node"), Blend->GetName());
	CT->SetStringField(TEXT("pin"), BoolPin->PinName.ToString());
	TSharedPtr<FJsonObject> Resp = InsertNode(BPPath, TEXT("AnimGraph"), TEXT("Get bIsInAir"), CT);
	const TSharedPtr<FJsonObject>* Data = GetData(Resp);
	TestTrue(TEXT("insert returned data"), Data != nullptr);
	if (!Data) return false;

	// connect_to_result reports a successful wire.
	FString WireRes; (*Data)->TryGetStringField(TEXT("connect_to_result"), WireRes);
	TestTrue(TEXT("connect_to wired the getter into the blend"), WireRes.Contains(TEXT("wired")));

	// Bool pin now driven by the getter.
	TestEqual(TEXT("blend bool pin has one driver"), BoolPin->LinkedTo.Num(), 1);

	// Getter landed LEFT of the blend, within a sane distance, not stranded far-right.
	FString GetterName; (*Data)->TryGetStringField(TEXT("node_name"), GetterName);
	UEdGraphNode* Getter = FindNodeByName(AnimGraph, GetterName);
	TestNotNull(TEXT("getter present"), Getter);
	if (Getter)
	{
		TestTrue(TEXT("getter LEFT of blend"), Getter->NodePosX < Blend->NodePosX);
		TestTrue(TEXT("getter within sane distance (<600)"),
			FMath::Abs(Blend->NodePosX - Getter->NodePosX) < 600);
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// (d) connect_to with exec nodes in a K2 graph — the new node's exec output feeds
//     an existing node's exec input (regression: normal BP flow works).
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConnectTo_K2ExecFlow, "KismetAI.ConnectTo.K2ExecFlow", ConnectToTestHelpers::Flags)
bool FConnectTo_K2ExecFlow::RunTest(const FString& Parameters)
{
	using namespace ConnectToTestHelpers;
	FString BPPath;
	UBlueprint* BP = MakeActorBP(TEXT("/Temp/KismetConnectToK2Exec"), BPPath);
	TestNotNull(TEXT("BP built"), BP);
	if (!BP) return false;
	UEdGraph* Graph = EventGraph(BP);
	TestNotNull(TEXT("event graph"), Graph);
	if (!Graph) return false;

	// Existing downstream PrintString at a far spot, with a free exec input.
	UK2Node_CallFunction* Downstream = AddPrintString(Graph);
	Downstream->NodePosX = 1600;
	Downstream->NodePosY = 300;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	UEdGraphPin* DownExecIn = Downstream->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	TestNotNull(TEXT("downstream exec input"), DownExecIn);

	// insert a new PrintString that feeds the downstream node's exec input (pin auto-picked).
	TSharedPtr<FJsonObject> CT = MakeShared<FJsonObject>();
	CT->SetStringField(TEXT("node"), Downstream->GetName());
	TSharedPtr<FJsonObject> Resp = InsertNode(BPPath, TEXT("EventGraph"), TEXT("Print String"), CT);
	const TSharedPtr<FJsonObject>* Data = GetData(Resp);
	TestTrue(TEXT("insert returned data"), Data != nullptr);
	if (!Data) return false;

	FString WireRes; (*Data)->TryGetStringField(TEXT("connect_to_result"), WireRes);
	TestTrue(TEXT("connect_to wired exec -> exec"), WireRes.Contains(TEXT("wired")));

	// Downstream exec input is now driven.
	TestEqual(TEXT("downstream exec input has one driver"), DownExecIn ? DownExecIn->LinkedTo.Num() : -1, 1);

	// New node landed LEFT of the downstream node (exec flows left->right).
	FString NewName; (*Data)->TryGetStringField(TEXT("node_name"), NewName);
	UEdGraphNode* NewNode = FindNodeByName(Graph, NewName);
	TestNotNull(TEXT("new node present"), NewNode);
	if (NewNode)
	{
		TestTrue(TEXT("new node LEFT of downstream"), NewNode->NodePosX < Downstream->NodePosX);
		TestTrue(TEXT("new node within sane distance (<700)"),
			FMath::Abs(Downstream->NodePosX - NewNode->NodePosX) < 700);
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// (e) connect_to auto-pick rejects an incompatible-only target with a clear error,
//     and the node is still inserted (unwired).
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConnectTo_IncompatibleRejected, "KismetAI.ConnectTo.IncompatibleRejected", ConnectToTestHelpers::Flags)
bool FConnectTo_IncompatibleRejected::RunTest(const FString& Parameters)
{
	using namespace ConnectToTestHelpers;
	FString BPPath;
	UBlueprint* BP = MakeActorBP(TEXT("/Temp/KismetConnectToReject"), BPPath);
	TestNotNull(TEXT("BP built"), BP);
	if (!BP) return false;
	UEdGraph* Graph = EventGraph(BP);
	if (!Graph) return false;

	TestTrue(TEXT("added bool var"), AddBoolVar(BP, TEXT("bFlag")));
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	// Spawn a target getter whose ONLY input is the variable self/exec — a bool getter
	// has no input a bool getter's output could feed. Use a Get node as the target:
	// a VariableGet has no compatible INPUT for another getter's bool output.
	UK2Node_VariableGet* Target = NewObject<UK2Node_VariableGet>(Graph);
	Graph->AddNode(Target, false, false);
	Target->VariableReference.SetSelfMember(FName(TEXT("bFlag")));
	Target->CreateNewGuid();
	Target->PostPlacedNewNode();
	Target->AllocateDefaultPins();
	Target->NodePosX = 800;
	Target->NodePosY = 200;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	const int32 NodesBefore = Graph->Nodes.Num();

	// insert another getter and try to connect_to the first getter (no compatible input).
	TSharedPtr<FJsonObject> CT = MakeShared<FJsonObject>();
	CT->SetStringField(TEXT("node"), Target->GetName());
	TSharedPtr<FJsonObject> Resp = InsertNode(BPPath, TEXT("EventGraph"), TEXT("Get bFlag"), CT);
	const TSharedPtr<FJsonObject>* Data = GetData(Resp);
	TestTrue(TEXT("insert returned data (node still inserted)"), Data != nullptr);
	if (!Data) return false;

	// Node was still inserted (count grew).
	TestTrue(TEXT("node still inserted despite failed wire"), Graph->Nodes.Num() > NodesBefore);

	// connect_to_result explains it was skipped and tells the agent to wire with edit_blueprint.
	FString WireRes; (*Data)->TryGetStringField(TEXT("connect_to_result"), WireRes);
	TestTrue(TEXT("connect_to reports it could not wire"),
		WireRes.Contains(TEXT("skipped")) || WireRes.Contains(TEXT("no compatible")));
	TestTrue(TEXT("connect_to error points at edit_blueprint"), WireRes.Contains(TEXT("edit_blueprint")));
	TestFalse(TEXT("connect_to did not claim success"), WireRes.Contains(TEXT("wired")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// (b) insert with NO anchor, then edit_blueprint connect → the node gets
//     repositioned near its peer (asserts position changed from spawn and is within
//     distance). Requires an active session + baseline so the node counts as
//     agent-created.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConnectTo_RepositionOnFirstWire, "KismetAI.ConnectTo.RepositionOnFirstWire", ConnectToTestHelpers::Flags)
bool FConnectTo_RepositionOnFirstWire::RunTest(const FString& Parameters)
{
	using namespace ConnectToTestHelpers;
	FString BPPath;
	UBlueprint* BP = MakeActorBP(TEXT("/Temp/KismetRepositionWire"), BPPath);
	TestNotNull(TEXT("BP built"), BP);
	if (!BP) return false;
	UEdGraph* Graph = EventGraph(BP);
	if (!Graph) return false;

	// Existing downstream consumer far to the right.
	UK2Node_CallFunction* Downstream = AddPrintString(Graph);
	Downstream->NodePosX = 1800;
	Downstream->NodePosY = 400;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	UEdGraphPin* DownExecIn = Downstream->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	TestNotNull(TEXT("downstream exec input"), DownExecIn);

	// Establish a session baseline AFTER the downstream node exists — so the
	// downstream node is in baseline (user/pre-session) and any later insert is
	// agent-created. This mirrors the agent-run snapshot taken at run start.
	FKismetAIModule& Mod = FKismetAIModule::Get();
	const FString PrevSession = Mod.GetCurrentSessionId();
	Mod.SetCurrentSessionId(TEXT("connectto-reposition-test"));
	Mod.TakeSessionBaseline(TEXT("connectto-reposition-test"), BPPath, TEXT("EventGraph"));

	// insert a new PrintString with NO connect_from / connect_to → it parks as an island.
	TSharedPtr<FJsonObject> Resp = InsertNode(BPPath, TEXT("EventGraph"), TEXT("Print String"));
	const TSharedPtr<FJsonObject>* Data = GetData(Resp);
	TestTrue(TEXT("insert returned data"), Data != nullptr);
	FString NewName; if (Data) (*Data)->TryGetStringField(TEXT("node_name"), NewName);
	UEdGraphNode* NewNode = FindNodeByName(Graph, NewName);
	TestNotNull(TEXT("new node present"), NewNode);
	if (!NewNode) { Mod.SetCurrentSessionId(PrevSession); return false; }

	const int32 SpawnX = NewNode->NodePosX;
	const int32 SpawnY = NewNode->NodePosY;
	// Island placement parks it to the right of content — far from the downstream node's left side.
	TestTrue(TEXT("island parked away from the downstream input side"), SpawnX > Downstream->NodePosX - 300);

	// Now wire the island's exec output into the downstream node's exec input.
	UEdGraphPin* NewExecOut = NewNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	TestNotNull(TEXT("new node then pin"), NewExecOut);
	if (NewExecOut && DownExecIn)
	{
		Connect(BPPath, TEXT("EventGraph"),
			NewNode->GetName(), NewExecOut->PinName.ToString(),
			Downstream->GetName(), DownExecIn->PinName.ToString());
	}

	// The island should have been repositioned: position changed from spawn, and it
	// now sits LEFT of and near the downstream node.
	TestTrue(TEXT("island moved from its spawn position on first wire"),
		NewNode->NodePosX != SpawnX || NewNode->NodePosY != SpawnY);
	TestTrue(TEXT("repositioned LEFT of downstream"), NewNode->NodePosX < Downstream->NodePosX);
	TestTrue(TEXT("repositioned within sane distance (<700)"),
		FMath::Abs(Downstream->NodePosX - NewNode->NodePosX) < 700);

	Mod.SetCurrentSessionId(PrevSession);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// (c) a pre-existing node (created outside the tracked set, directly via NewObject
//     and present in the session baseline) connected the same way does NOT move.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConnectTo_PreExistingNeverMoves, "KismetAI.ConnectTo.PreExistingNeverMoves", ConnectToTestHelpers::Flags)
bool FConnectTo_PreExistingNeverMoves::RunTest(const FString& Parameters)
{
	using namespace ConnectToTestHelpers;
	FString BPPath;
	UBlueprint* BP = MakeActorBP(TEXT("/Temp/KismetPreExistingNoMove"), BPPath);
	TestNotNull(TEXT("BP built"), BP);
	if (!BP) return false;
	UEdGraph* Graph = EventGraph(BP);
	if (!Graph) return false;

	// Two user nodes at deliberate, far-apart positions.
	UK2Node_CallFunction* UserSource = AddPrintString(Graph);
	UserSource->NodePosX = 100;
	UserSource->NodePosY = 100;
	UK2Node_CallFunction* UserTarget = AddPrintString(Graph);
	UserTarget->NodePosX = 2000;
	UserTarget->NodePosY = 900;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	// Snapshot the baseline AFTER both user nodes exist — both are pre-session.
	FKismetAIModule& Mod = FKismetAIModule::Get();
	const FString PrevSession = Mod.GetCurrentSessionId();
	Mod.SetCurrentSessionId(TEXT("connectto-preexisting-test"));
	Mod.TakeSessionBaseline(TEXT("connectto-preexisting-test"), BPPath, TEXT("EventGraph"));

	const int32 SrcX = UserSource->NodePosX, SrcY = UserSource->NodePosY;
	const int32 TgtX = UserTarget->NodePosX, TgtY = UserTarget->NodePosY;

	UEdGraphPin* SrcThen = UserSource->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* TgtExec = UserTarget->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	TestNotNull(TEXT("source then pin"), SrcThen);
	TestNotNull(TEXT("target exec pin"), TgtExec);
	if (SrcThen && TgtExec)
	{
		Connect(BPPath, TEXT("EventGraph"),
			UserSource->GetName(), SrcThen->PinName.ToString(),
			UserTarget->GetName(), TgtExec->PinName.ToString());
	}

	// Neither user node moved — pre-session nodes are never repositioned.
	TestEqual(TEXT("user source X unchanged"), UserSource->NodePosX, SrcX);
	TestEqual(TEXT("user source Y unchanged"), UserSource->NodePosY, SrcY);
	TestEqual(TEXT("user target X unchanged"), UserTarget->NodePosX, TgtX);
	TestEqual(TEXT("user target Y unchanged"), UserTarget->NodePosY, TgtY);
	// And the connection did succeed.
	TestEqual(TEXT("connection made"), TgtExec ? TgtExec->LinkedTo.Num() : -1, 1);

	Mod.SetCurrentSessionId(PrevSession);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// (f) The HTTP /api/v1/agent/run path arms reposition-on-first-wire. Instead of
//     the manual TakeSessionBaseline seam, this goes through ArmDiffSessionForRun
//     — the exact session/baseline setup HandleAgentRun now performs at run start
//     (the request carries a 'blueprint' hint but no session id, so one is
//     generated). After arming, insert bare → wire → the island repositions,
//     proving the eval/external-driver path is no longer dead.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConnectTo_AgentRunPathArmsReposition, "KismetAI.ConnectTo.AgentRunPathArmsReposition", ConnectToTestHelpers::Flags)
bool FConnectTo_AgentRunPathArmsReposition::RunTest(const FString& Parameters)
{
	using namespace ConnectToTestHelpers;
	FString BPPath;
	UBlueprint* BP = MakeActorBP(TEXT("/Temp/KismetAgentRunArms"), BPPath);
	TestNotNull(TEXT("BP built"), BP);
	if (!BP) return false;
	UEdGraph* Graph = EventGraph(BP);
	if (!Graph) return false;

	// Pre-existing downstream consumer (will be in baseline — pre-session).
	UK2Node_CallFunction* Downstream = AddPrintString(Graph);
	Downstream->NodePosX = 1800;
	Downstream->NodePosY = 400;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	UEdGraphPin* DownExecIn = Downstream->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	TestNotNull(TEXT("downstream exec input"), DownExecIn);

	FKismetAIModule& Mod = FKismetAIModule::Get();
	const FString PrevSession = Mod.GetCurrentSessionId();

	// EXACT HandleAgentRun arming: no requested session id (one is generated), a
	// 'blueprint' hint naming the target asset. This is what the eval framework
	// sends and what the panel path never set for HTTP runs (the original gap).
	const FString RunSession = Mod.ArmDiffSessionForRun(FString(), BPPath);
	TestTrue(TEXT("a session id was generated"), !RunSession.IsEmpty());
	TestEqual(TEXT("module CurrentSessionId is the run session"), Mod.GetCurrentSessionId(), RunSession);

	// insert bare (no connect_from/connect_to) → parks as an island.
	TSharedPtr<FJsonObject> Resp = InsertNode(BPPath, TEXT("EventGraph"), TEXT("Print String"));
	const TSharedPtr<FJsonObject>* Data = GetData(Resp);
	TestTrue(TEXT("insert returned data"), Data != nullptr);
	FString NewName; if (Data) (*Data)->TryGetStringField(TEXT("node_name"), NewName);
	UEdGraphNode* NewNode = FindNodeByName(Graph, NewName);
	TestNotNull(TEXT("new node present"), NewNode);
	if (!NewNode) { Mod.SetCurrentSessionId(PrevSession); return false; }

	const int32 SpawnX = NewNode->NodePosX;
	const int32 SpawnY = NewNode->NodePosY;

	// Wire the island into the downstream node.
	UEdGraphPin* NewExecOut = NewNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	TestNotNull(TEXT("new node then pin"), NewExecOut);
	if (NewExecOut && DownExecIn)
	{
		Connect(BPPath, TEXT("EventGraph"),
			NewNode->GetName(), NewExecOut->PinName.ToString(),
			Downstream->GetName(), DownExecIn->PinName.ToString());
	}

	// Reposition fired — proving the HTTP-run arming works end to end.
	TestTrue(TEXT("island moved from its spawn position on first wire (HTTP-run path)"),
		NewNode->NodePosX != SpawnX || NewNode->NodePosY != SpawnY);
	TestTrue(TEXT("repositioned LEFT of downstream"), NewNode->NodePosX < Downstream->NodePosX);
	TestTrue(TEXT("repositioned within sane distance (<700)"),
		FMath::Abs(Downstream->NodePosX - NewNode->NodePosX) < 700);

	// Downstream (pre-session) never moved.
	TestEqual(TEXT("downstream X unchanged"), Downstream->NodePosX, 1800);
	TestEqual(TEXT("downstream Y unchanged"), Downstream->NodePosY, 400);

	// A second arming on the same session resets the baseline + island marks cleanly:
	// re-arming, then inserting a fresh bare node, must still park-then-reposition.
	const FString RunSession2 = Mod.ArmDiffSessionForRun(RunSession, BPPath);
	TestEqual(TEXT("re-arm with explicit session id reuses it"), RunSession2, RunSession);

	TSharedPtr<FJsonObject> Resp2 = InsertNode(BPPath, TEXT("EventGraph"), TEXT("Print String"));
	const TSharedPtr<FJsonObject>* Data2 = GetData(Resp2);
	FString NewName2; if (Data2) (*Data2)->TryGetStringField(TEXT("node_name"), NewName2);
	UEdGraphNode* NewNode2 = FindNodeByName(Graph, NewName2);
	TestNotNull(TEXT("second new node present"), NewNode2);
	if (NewNode2)
	{
		const int32 Spawn2X = NewNode2->NodePosX, Spawn2Y = NewNode2->NodePosY;
		UEdGraphPin* New2Out = NewNode2->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
		if (New2Out && DownExecIn)
		{
			Connect(BPPath, TEXT("EventGraph"),
				NewNode2->GetName(), New2Out->PinName.ToString(),
				Downstream->GetName(), DownExecIn->PinName.ToString());
		}
		TestTrue(TEXT("second island also repositions after re-arm"),
			NewNode2->NodePosX != Spawn2X || NewNode2->NodePosY != Spawn2Y);
	}

	Mod.SetCurrentSessionId(PrevSession);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// (g) MULTI-NODE CHAIN reposition (the stranded-island gap). Build a 3-node island
//     sub-chain wired only new->new, then wire the chain's END into an existing
//     node. The WHOLE chain must travel to the consumer — not just the one node
//     that touched it. Before the fix, only the end node moved and the two upstream
//     feeders stayed stranded off to the right (long wires); this guards that.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConnectTo_ChainRepositionOnFirstWire, "KismetAI.ConnectTo.ChainRepositionOnFirstWire", ConnectToTestHelpers::Flags)
bool FConnectTo_ChainRepositionOnFirstWire::RunTest(const FString& Parameters)
{
	using namespace ConnectToTestHelpers;
	FString BPPath;
	UBlueprint* BP = MakeActorBP(TEXT("/Temp/KismetChainReposition"), BPPath);
	TestNotNull(TEXT("BP built"), BP);
	if (!BP) return false;
	UEdGraph* Graph = EventGraph(BP);
	if (!Graph) return false;

	// Existing downstream consumer (the eventual anchor), far to the right.
	UK2Node_CallFunction* Downstream = AddPrintString(Graph);
	Downstream->NodePosX = 1400;
	Downstream->NodePosY = 300;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	UEdGraphPin* DownExecIn = Downstream->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	TestNotNull(TEXT("downstream exec input"), DownExecIn);

	// Baseline AFTER the downstream node exists → it is pre-session; later inserts
	// are agent-created islands.
	FKismetAIModule& Mod = FKismetAIModule::Get();
	const FString PrevSession = Mod.GetCurrentSessionId();
	Mod.SetCurrentSessionId(TEXT("chain-reposition-test"));
	Mod.TakeSessionBaseline(TEXT("chain-reposition-test"), BPPath, TEXT("EventGraph"));

	// Insert three bare Print Strings (no connect_from/connect_to) → each parks as an
	// island off to the right.
	auto InsertBare = [&](UEdGraphNode*& Out) -> bool
	{
		const int32 Before = Graph->Nodes.Num();
		TSharedPtr<FJsonObject> Resp = InsertNode(BPPath, TEXT("EventGraph"), TEXT("Print String"));
		const TSharedPtr<FJsonObject>* Data = GetData(Resp);
		if (!Data) return false;
		FString Name; (*Data)->TryGetStringField(TEXT("node_name"), Name);
		Out = FindNodeByName(Graph, Name);
		return Out != nullptr && Graph->Nodes.Num() == Before + 1;
	};

	UEdGraphNode* P1 = nullptr; UEdGraphNode* P2 = nullptr; UEdGraphNode* P3 = nullptr;
	TestTrue(TEXT("inserted P1"), InsertBare(P1));
	TestTrue(TEXT("inserted P2"), InsertBare(P2));
	TestTrue(TEXT("inserted P3"), InsertBare(P3));
	if (!P1 || !P2 || !P3) { Mod.SetCurrentSessionId(PrevSession); return false; }

	const FIntPoint Spawn1(P1->NodePosX, P1->NodePosY);
	const FIntPoint Spawn2(P2->NodePosX, P2->NodePosY);

	// All three parked to the RIGHT of the downstream consumer (the stranded state).
	TestTrue(TEXT("P1 parked right of the consumer"), P1->NodePosX > Downstream->NodePosX);
	TestTrue(TEXT("P2 parked right of the consumer"), P2->NodePosX > Downstream->NodePosX);

	auto ThenOut = [](UEdGraphNode* N) { return N->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output); };
	auto ExecIn  = [](UEdGraphNode* N) { return N->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input); };

	// Wire the chain new->new: P1 -> P2 -> P3. These island-to-island wires must NOT
	// consume the island marks (the fix's skip-guard), so the upstream nodes stay
	// movable until the chain reaches real content.
	Connect(BPPath, TEXT("EventGraph"), P1->GetName(), ThenOut(P1)->PinName.ToString(), P2->GetName(), ExecIn(P2)->PinName.ToString());
	Connect(BPPath, TEXT("EventGraph"), P2->GetName(), ThenOut(P2)->PinName.ToString(), P3->GetName(), ExecIn(P3)->PinName.ToString());

	// Upstream feeders are still parked at spawn (island-to-island wires moved nothing).
	TestEqual(TEXT("P1 still at spawn after new->new wires (X)"), P1->NodePosX, Spawn1.X);
	TestEqual(TEXT("P1 still at spawn after new->new wires (Y)"), P1->NodePosY, Spawn1.Y);
	TestEqual(TEXT("P2 still at spawn after new->new wires (X)"), P2->NodePosX, Spawn2.X);

	// Now wire the chain's END into the existing consumer. This is the anchor wire.
	Connect(BPPath, TEXT("EventGraph"), P3->GetName(), ThenOut(P3)->PinName.ToString(), Downstream->GetName(), DownExecIn->PinName.ToString());

	// THE FIX: the whole chain travels to the consumer. Every node moved off its
	// island spawn spot and now sits LEFT of the consumer (exec flows left->right),
	// instead of only P3 moving while P1/P2 stay stranded on the right.
	TestTrue(TEXT("P1 moved off its island spawn"), P1->NodePosX != Spawn1.X || P1->NodePosY != Spawn1.Y);
	TestTrue(TEXT("P2 moved off its island spawn"), P2->NodePosX != Spawn2.X || P2->NodePosY != Spawn2.Y);
	TestTrue(TEXT("P1 pulled LEFT of the consumer (not stranded right)"), P1->NodePosX < Downstream->NodePosX);
	TestTrue(TEXT("P2 pulled LEFT of the consumer (not stranded right)"), P2->NodePosX < Downstream->NodePosX);
	TestTrue(TEXT("P3 pulled LEFT of the consumer"), P3->NodePosX < Downstream->NodePosX);

	// The consumer (pre-session) never moved.
	TestEqual(TEXT("downstream X unchanged"), Downstream->NodePosX, 1400);
	TestEqual(TEXT("downstream Y unchanged"), Downstream->NodePosY, 300);

	Mod.SetCurrentSessionId(PrevSession);
	return true;
}
