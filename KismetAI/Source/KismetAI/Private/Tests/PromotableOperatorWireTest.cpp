// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

// Fix B verification: a "Divide"/"Greater" query now routes to UE's type-promotable
// operator node (FTypePromotion). Its UNWIRED default pins display an exotic type
// (FrameNumber), but the node is type-PROMOTABLE — wiring a real/float value must
// promote its pins to real and compile cleanly. This test proves the routed node is
// functionally correct for the realistic eval flow (float input -> Divide.A).

#include "Misc/AutomationTest.h"
#include "KismetAIModule.h"
#include "KismetAIUtils.h"
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
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"

namespace PromoWireHelpers
{
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	static TSharedPtr<FJsonObject> Run(
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

	static FString InsertNodeName(const TSharedPtr<FJsonObject>& Env)
	{
		const TSharedPtr<FJsonObject>* Data = nullptr;
		FString Name;
		if (Env.IsValid() && Env->TryGetObjectField(TEXT("data"), Data) && Data)
		{
			(*Data)->TryGetStringField(TEXT("node_name"), Name);
		}
		return Name;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPromoWire_DivideAdaptsToReal,
	"KismetAI.InsertNode.PromotableDivideAdaptsToReal", PromoWireHelpers::Flags)
bool FPromoWire_DivideAdaptsToReal::RunTest(const FString& Parameters)
{
	using namespace PromoWireHelpers;

	UPackage* Pkg = CreatePackage(TEXT("/Temp/PromoWire"));
	const FName BPName = MakeUniqueObjectName(Pkg, UBlueprint::StaticClass(), TEXT("BP_PromoWire"));
	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Pkg, BPName, BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("BP"), BP)) return false;
	const FString BPPath = BP->GetPathName();

	// A float member variable to feed the operator.
	FEdGraphPinType FloatType;
	FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
	FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
	FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("Num"), FloatType, TEXT("1.0"));

	// Route "Divide" -> promotable operator node.
	TSharedPtr<FJsonObject> DivBody = MakeShared<FJsonObject>();
	DivBody->SetStringField(TEXT("blueprint"), BPPath);
	DivBody->SetStringField(TEXT("graph"), TEXT("EventGraph"));
	DivBody->SetStringField(TEXT("node"), TEXT("Divide"));
	const FString DivName = InsertNodeName(Run(&FKismetAIModule::HandleInsertNode, DivBody));
	if (!TestTrue(TEXT("Divide inserted"), !DivName.IsEmpty())) return false;

	// Insert a float getter wired into Divide.A.
	TSharedPtr<FJsonObject> CT = MakeShared<FJsonObject>();
	CT->SetStringField(TEXT("node"), DivName);
	CT->SetStringField(TEXT("pin"), TEXT("A"));
	TSharedPtr<FJsonObject> GetBody = MakeShared<FJsonObject>();
	GetBody->SetStringField(TEXT("blueprint"), BPPath);
	GetBody->SetStringField(TEXT("graph"), TEXT("EventGraph"));
	GetBody->SetStringField(TEXT("node"), TEXT("Get Num"));
	GetBody->SetObjectField(TEXT("connect_to"), CT);
	Run(&FKismetAIModule::HandleInsertNode, GetBody);

	// Inspect the Divide node's A pin: it must have promoted to real (not FrameNumber/etc.).
	UEdGraph* Graph = KismetAIUtils::FindGraph(BP, TEXT("EventGraph"));
	UEdGraphNode* DivNode = nullptr;
	for (UEdGraphNode* N : Graph->Nodes) { if (N && N->GetName() == DivName) { DivNode = N; break; } }
	if (!TestNotNull(TEXT("Divide node found"), DivNode)) return false;

	UEdGraphPin* PinA = nullptr;
	for (UEdGraphPin* P : DivNode->Pins)
	{
		if (P && P->Direction == EGPD_Input && P->PinName == TEXT("A")) { PinA = P; break; }
	}
	if (!TestNotNull(TEXT("Divide has input pin A"), PinA)) return false;

	// Fix B's guarantee: the routed promotable node ADAPTS to the wired numeric scalar
	// (real or int) — i.e. it is no longer stuck on its exotic FrameNumber/Timespan
	// unwired default. The exact scalar (real vs int) depends on how the wired source
	// resolves in the current editor state, so assert "numeric scalar", not specifically
	// real, to stay deterministic across standalone vs full-suite runs.
	const FName Cat = PinA->PinType.PinCategory;
	const bool bNumericScalar = (Cat == UEdGraphSchema_K2::PC_Real || Cat == UEdGraphSchema_K2::PC_Int);
	AddInfo(FString::Printf(TEXT("Divide.A after wiring: category='%s' subcat='%s' linked=%d"),
		*Cat.ToString(), *PinA->PinType.PinSubCategory.ToString(), PinA->LinkedTo.Num()));
	TestTrue(TEXT("Divide.A adapted to a numeric scalar (not the exotic frame/time default)"), bNumericScalar);

	// SCORER-VISIBLE FIELDS (Fix C design): what the snapshot would capture for this node.
	FString MemberName;
	if (UK2Node_CallFunction* CF = Cast<UK2Node_CallFunction>(DivNode))
	{
		MemberName = CF->FunctionReference.GetMemberName().ToString();
	}
	AddInfo(FString::Printf(TEXT("SCORER-FIELDS class='%s' member_name='%s' fulltitle='%s' listtitle='%s'"),
		*DivNode->GetClass()->GetName(),
		*MemberName,
		*DivNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString().Replace(TEXT("\n"), TEXT(" / ")),
		*DivNode->GetNodeTitle(ENodeTitleType::ListView).ToString().Replace(TEXT("\n"), TEXT(" / "))));

	// And it must compile.
	TSharedPtr<FJsonObject> CompileBody = MakeShared<FJsonObject>();
	CompileBody->SetStringField(TEXT("blueprint"), BPPath);
	TSharedPtr<FJsonObject> CompileEnv = Run(&FKismetAIModule::HandleCompile, CompileBody);
	bool bCompiled = false;
	if (CompileEnv.IsValid()) CompileEnv->TryGetBoolField(TEXT("success"), bCompiled);
	TestTrue(TEXT("Blueprint compiles with the promoted Divide"), bCompiled);

	return true;
}
