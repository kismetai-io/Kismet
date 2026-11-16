// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Misc/AutomationTest.h"
#include "KismetAIModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "EdGraphNode_Comment.h"
#include "KismetAILayout.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"

constexpr auto EditGraphTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	static TSharedPtr<FJsonObject> RunEditGraphHandler(
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

	static UBlueprint* MakeTestBlueprint(const TCHAR* PackagePath, const TCHAR* AssetName)
	{
		UPackage* Pkg = CreatePackage(PackagePath);
		if (!Pkg) return nullptr;
		return FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Pkg, AssetName, BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	}

	static UK2Node_CallFunction* AddPrintStringNode(UEdGraph* Graph)
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

	static const TSharedPtr<FJsonObject>* GetData(const TSharedPtr<FJsonObject>& Env)
	{
		static TSharedPtr<FJsonObject> Null;
		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (Env.IsValid() && Env->TryGetObjectField(TEXT("data"), Data))
		{
			return Data;
		}
		return nullptr;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// set_pin_default must validate values through the schema: malformed struct
// literals and non-numeric floats are rejected at edit time (INVALID_DEFAULT)
// instead of surfacing as compile errors much later.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditGraph_SetPinDefaultValidation, "KismetAI.EditGraph.SetPinDefaultValidation", EditGraphTestFlags)
bool FEditGraph_SetPinDefaultValidation::RunTest(const FString& Parameters)
{
	UBlueprint* BP = MakeTestBlueprint(TEXT("/Temp/KismetSetPinDefaultTest"), TEXT("BP_KismetSetPinDefaultTest"));
	if (!BP || BP->UbergraphPages.Num() == 0)
	{
		AddError(TEXT("could not create test blueprint"));
		return false;
	}
	UEdGraph* Graph = BP->UbergraphPages[0];
	UK2Node_CallFunction* Print = AddPrintStringNode(Graph);
	if (!Print)
	{
		AddError(TEXT("could not spawn PrintString node"));
		return false;
	}

	UEdGraphPin* ColorPin = Print->FindPin(TEXT("TextColor"), EGPD_Input);
	UEdGraphPin* DurationPin = Print->FindPin(TEXT("Duration"), EGPD_Input);
	if (!ColorPin || !DurationPin)
	{
		AddError(TEXT("PrintString pins not found"));
		return false;
	}
	const FString OriginalColor = ColorPin->DefaultValue;
	const FString ValidColor = TEXT("(R=0.000000,G=1.000000,B=0.000000,A=1.000000)");

	auto MakeOp = [&](const FString& Pin, const FString& Value)
	{
		TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
		Op->SetStringField(TEXT("action"), TEXT("set_pin_default"));
		Op->SetStringField(TEXT("node"), Print->GetName());
		Op->SetStringField(TEXT("pin"), Pin);
		Op->SetStringField(TEXT("value"), Value);
		return MakeShared<FJsonValueObject>(Op);
	};

	TArray<TSharedPtr<FJsonValue>> Ops;
	Ops.Add(MakeOp(TEXT("TextColor"), TEXT("not-a-color")));   // malformed struct -> reject
	Ops.Add(MakeOp(TEXT("TextColor"), ValidColor));            // valid struct -> accept
	Ops.Add(MakeOp(TEXT("Duration"), TEXT("abc")));            // non-numeric float -> reject

	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("blueprint"), BP->GetPathName());
	Body->SetArrayField(TEXT("operations"), Ops);

	TSharedPtr<FJsonObject> Env = RunEditGraphHandler(&FKismetAIModule::HandleEditGraph, Body);
	const TSharedPtr<FJsonObject>* Data = GetData(Env);
	if (!Data)
	{
		AddError(TEXT("edit_graph returned no data envelope"));
		return false;
	}

	TestEqual(TEXT("one op succeeded"), (int32)(*Data)->GetNumberField(TEXT("operations_succeeded")), 1);
	TestEqual(TEXT("two ops failed"), (int32)(*Data)->GetNumberField(TEXT("operations_failed")), 2);

	const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
	if ((*Data)->TryGetArrayField(TEXT("results"), Results) && Results->Num() == 3)
	{
		TSharedPtr<FJsonObject> R0 = (*Results)[0]->AsObject();
		TestFalse(TEXT("malformed struct rejected"), R0->GetBoolField(TEXT("success")));
		TestEqual(TEXT("malformed struct error code"), R0->GetStringField(TEXT("error_code")), FString(TEXT("INVALID_DEFAULT")));

		TSharedPtr<FJsonObject> R1 = (*Results)[1]->AsObject();
		TestTrue(TEXT("valid struct accepted"), R1->GetBoolField(TEXT("success")));
		TestEqual(TEXT("new_value reported"), R1->GetStringField(TEXT("new_value")), ValidColor);

		TSharedPtr<FJsonObject> R2 = (*Results)[2]->AsObject();
		TestFalse(TEXT("non-numeric float rejected"), R2->GetBoolField(TEXT("success")));
		TestEqual(TEXT("float error code"), R2->GetStringField(TEXT("error_code")), FString(TEXT("INVALID_DEFAULT")));
	}
	else
	{
		AddError(TEXT("expected 3 result entries"));
	}

	// The rejected writes must not have touched the pins; the valid one must stick.
	TestEqual(TEXT("color pin holds the valid value"), ColorPin->DefaultValue, ValidColor);
	TestNotEqual(TEXT("color pin is not the malformed value"), ColorPin->DefaultValue, FString(TEXT("not-a-color")));
	TestNotEqual(TEXT("duration pin is not the malformed value"), DurationPin->DefaultValue, FString(TEXT("abc")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// compile_blueprint must flag exec subgraphs that are wired internally but
// have no exec path from any entry point — the Blueprint compiler is silent
// about them.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompile_UnreachableExecDetection, "KismetAI.Compile.UnreachableExecDetection", EditGraphTestFlags)
bool FCompile_UnreachableExecDetection::RunTest(const FString& Parameters)
{
	UBlueprint* BP = MakeTestBlueprint(TEXT("/Temp/KismetUnreachableTest"), TEXT("BP_KismetUnreachableTest"));
	if (!BP || BP->UbergraphPages.Num() == 0)
	{
		AddError(TEXT("could not create test blueprint"));
		return false;
	}
	UEdGraph* Graph = BP->UbergraphPages[0];
	const UEdGraphSchema* Schema = Graph->GetSchema();

	// Two PrintString nodes wired to each other but to no entry point.
	UK2Node_CallFunction* NodeA = AddPrintStringNode(Graph);
	UK2Node_CallFunction* NodeB = AddPrintStringNode(Graph);
	UEdGraphPin* AThen = NodeA->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* BExec = NodeB->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	if (!AThen || !BExec || !Schema->TryCreateConnection(AThen, BExec))
	{
		AddError(TEXT("could not wire test chain"));
		return false;
	}

	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("blueprint"), BP->GetPathName());

	TSharedPtr<FJsonObject> Env = RunEditGraphHandler(&FKismetAIModule::HandleCompile, Body);
	const TSharedPtr<FJsonObject>* Data = GetData(Env);
	if (!Data)
	{
		AddError(TEXT("compile returned no data envelope"));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
	TestTrue(TEXT("unreachable warnings present"), (*Data)->TryGetArrayField(TEXT("unreachable_node_warnings"), Warnings));
	if (Warnings)
	{
		bool bMentionsHead = false;
		for (const TSharedPtr<FJsonValue>& W : *Warnings)
		{
			const FString Text = W->AsString();
			if (Text.Contains(NodeA->GetName()) && Text.Contains(TEXT("1 downstream")))
			{
				bMentionsHead = true;
			}
		}
		TestTrue(TEXT("warning names the chain head and downstream count"), bMentionsHead);
	}

	// Wire a custom event into the chain head; the warning must disappear.
	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
	Event->CustomFunctionName = FName(TEXT("KismetAITestEvent"));
	Graph->AddNode(Event, false, false);
	Event->CreateNewGuid();
	Event->AllocateDefaultPins();

	UEdGraphPin* EventThen = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* AExec = NodeA->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	if (!EventThen || !AExec || !Schema->TryCreateConnection(EventThen, AExec))
	{
		AddError(TEXT("could not wire event into chain"));
		return false;
	}

	Env = RunEditGraphHandler(&FKismetAIModule::HandleCompile, Body);
	Data = GetData(Env);
	if (!Data)
	{
		AddError(TEXT("second compile returned no data envelope"));
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* WarningsAfter = nullptr;
	TestFalse(TEXT("no unreachable warnings once wired to an event"),
		(*Data)->TryGetArrayField(TEXT("unreachable_node_warnings"), WarningsAfter));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// edit_blueprint must never reposition nodes: it only rewires. This is the
// regression test for the "agent touches all the nodes" layout vandalism.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditGraph_PositionPreservation, "KismetAI.EditGraph.PositionPreservation", EditGraphTestFlags)
bool FEditGraph_PositionPreservation::RunTest(const FString& Parameters)
{
	UBlueprint* BP = MakeTestBlueprint(TEXT("/Temp/KismetPosPreserveTest"), TEXT("BP_KismetPosPreserveTest"));
	if (!BP || BP->UbergraphPages.Num() == 0)
	{
		AddError(TEXT("could not create test blueprint"));
		return false;
	}
	UEdGraph* Graph = BP->UbergraphPages[0];
	const UEdGraphSchema* Schema = Graph->GetSchema();

	// Hand-arranged chain: Event -> PrintA -> PrintB, plus a loose PrintC.
	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
	Event->CustomFunctionName = FName(TEXT("KismetPosTestEvent"));
	Graph->AddNode(Event, false, false);
	Event->CreateNewGuid();
	Event->AllocateDefaultPins();
	Event->NodePosX = 0;    Event->NodePosY = 0;

	UK2Node_CallFunction* PrintA = AddPrintStringNode(Graph);
	UK2Node_CallFunction* PrintB = AddPrintStringNode(Graph);
	UK2Node_CallFunction* PrintC = AddPrintStringNode(Graph);
	PrintA->NodePosX = 400;  PrintA->NodePosY = 16;
	PrintB->NodePosX = 800;  PrintB->NodePosY = -48;
	PrintC->NodePosX = 1200; PrintC->NodePosY = 304;

	Schema->TryCreateConnection(Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), PrintA->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input));
	Schema->TryCreateConnection(PrintA->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), PrintB->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input));

	TMap<FString, TPair<int32, int32>> Before;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node) Before.Add(Node->GetName(), TPair<int32, int32>(Node->NodePosX, Node->NodePosY));
	}

	// Rewire through edit_graph: disconnect A->B, connect A->C, set a default.
	auto Op = [](std::initializer_list<TPair<FString, FString>> Fields)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		for (const auto& F : Fields) O->SetStringField(F.Key, F.Value);
		return MakeShared<FJsonValueObject>(O);
	};
	TArray<TSharedPtr<FJsonValue>> Ops;
	Ops.Add(Op({ {TEXT("action"), TEXT("disconnect")}, {TEXT("source_node"), PrintA->GetName()}, {TEXT("source_pin"), TEXT("then")}, {TEXT("target_node"), PrintB->GetName()}, {TEXT("target_pin"), TEXT("execute")} }));
	Ops.Add(Op({ {TEXT("action"), TEXT("connect")}, {TEXT("source_node"), PrintA->GetName()}, {TEXT("source_pin"), TEXT("then")}, {TEXT("target_node"), PrintC->GetName()}, {TEXT("target_pin"), TEXT("execute")} }));
	Ops.Add(Op({ {TEXT("action"), TEXT("set_pin_default")}, {TEXT("node"), PrintB->GetName()}, {TEXT("pin"), TEXT("InString")}, {TEXT("value"), TEXT("Rewired")} }));

	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("blueprint"), BP->GetPathName());
	Body->SetArrayField(TEXT("operations"), Ops);
	TSharedPtr<FJsonObject> Env = RunEditGraphHandler(&FKismetAIModule::HandleEditGraph, Body);
	TestTrue(TEXT("edit_graph succeeded"), Env.IsValid() && Env->GetBoolField(TEXT("success")));

	int32 Moved = 0;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		const TPair<int32, int32>* Orig = Before.Find(Node->GetName());
		if (Orig && (Node->NodePosX != Orig->Key || Node->NodePosY != Orig->Value))
		{
			Moved++;
			AddError(FString::Printf(TEXT("node %s moved: (%d,%d) -> (%d,%d)"),
				*Node->GetName(), Orig->Key, Orig->Value, Node->NodePosX, Node->NodePosY));
		}
	}
	TestEqual(TEXT("zero nodes repositioned by edit_graph"), Moved, 0);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// insert_node places only the new node: near its anchor, overlapping nothing,
// with every pre-existing node exactly where it was.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInsertNode_Placement, "KismetAI.InsertNode.Placement", EditGraphTestFlags)
bool FInsertNode_Placement::RunTest(const FString& Parameters)
{
	UBlueprint* BP = MakeTestBlueprint(TEXT("/Temp/KismetInsertPlaceTest"), TEXT("BP_KismetInsertPlaceTest"));
	if (!BP || BP->UbergraphPages.Num() == 0)
	{
		AddError(TEXT("could not create test blueprint"));
		return false;
	}
	UEdGraph* Graph = BP->UbergraphPages[0];
	const UEdGraphSchema* Schema = Graph->GetSchema();

	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
	Event->CustomFunctionName = FName(TEXT("KismetInsertTestEvent"));
	Graph->AddNode(Event, false, false);
	Event->CreateNewGuid();
	Event->AllocateDefaultPins();
	Event->NodePosX = 0; Event->NodePosY = 0;

	UK2Node_CallFunction* PrintA = AddPrintStringNode(Graph);
	PrintA->NodePosX = 400; PrintA->NodePosY = 0;
	Schema->TryCreateConnection(Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), PrintA->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input));

	TMap<FString, TPair<int32, int32>> Before;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node) Before.Add(Node->GetName(), TPair<int32, int32>(Node->NodePosX, Node->NodePosY));
	}

	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("blueprint"), BP->GetPathName());
	Body->SetStringField(TEXT("node"), TEXT("Print String"));
	TSharedPtr<FJsonObject> From = MakeShared<FJsonObject>();
	From->SetStringField(TEXT("node"), PrintA->GetName());
	From->SetStringField(TEXT("pin"), TEXT("then"));
	Body->SetObjectField(TEXT("connect_from"), From);

	TSharedPtr<FJsonObject> Env = RunEditGraphHandler(&FKismetAIModule::HandleInsertNode, Body);
	TestTrue(TEXT("insert_node succeeded"), Env.IsValid() && Env->GetBoolField(TEXT("success")));

	UEdGraphNode* NewNode = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		const TPair<int32, int32>* Orig = Before.Find(Node->GetName());
		if (!Orig)
		{
			NewNode = Node;
			continue;
		}
		TestEqual(FString::Printf(TEXT("%s X unchanged"), *Node->GetName()), Node->NodePosX, Orig->Key);
		TestEqual(FString::Printf(TEXT("%s Y unchanged"), *Node->GetName()), Node->NodePosY, Orig->Value);
	}
	if (!NewNode)
	{
		AddError(TEXT("inserted node not found in graph"));
		return false;
	}

	// Near the anchor, on its right, overlapping nothing
	TestTrue(TEXT("new node right of anchor"), NewNode->NodePosX > PrintA->NodePosX);
	TestTrue(TEXT("new node near anchor"), FMath::Abs(NewNode->NodePosY - PrintA->NodePosY) < 600);
	const FNodeSize NewSize = FKismetAILayout::EstimateNodeSize(NewNode);
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node || Node == NewNode || Node->IsA<UEdGraphNode_Comment>()) continue;
		const FNodeSize Size = FKismetAILayout::EstimateNodeSize(Node);
		const bool bOverlap =
			NewNode->NodePosX < Node->NodePosX + Size.Width &&
			NewNode->NodePosX + NewSize.Width > Node->NodePosX &&
			NewNode->NodePosY < Node->NodePosY + Size.Height &&
			NewNode->NodePosY + NewSize.Height > Node->NodePosY;
		TestFalse(FString::Printf(TEXT("new node does not overlap %s"), *Node->GetName()), bOverlap);
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// layout_graph endpoint: explicit reformat works and reports moved nodes
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutGraph_Handler, "KismetAI.LayoutGraph.Handler", EditGraphTestFlags)
bool FLayoutGraph_Handler::RunTest(const FString& Parameters)
{
	UBlueprint* BP = MakeTestBlueprint(TEXT("/Temp/KismetLayoutToolTest"), TEXT("BP_KismetLayoutToolTest"));
	if (!BP || BP->UbergraphPages.Num() == 0)
	{
		AddError(TEXT("could not create test blueprint"));
		return false;
	}
	UEdGraph* Graph = BP->UbergraphPages[0];
	const UEdGraphSchema* Schema = Graph->GetSchema();

	// A deliberately messy chain: all nodes piled at the same spot.
	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
	Event->CustomFunctionName = FName(TEXT("KismetLayoutToolEvent"));
	Graph->AddNode(Event, false, false);
	Event->CreateNewGuid();
	Event->AllocateDefaultPins();
	Event->NodePosX = 0; Event->NodePosY = 0;

	UK2Node_CallFunction* PrintA = AddPrintStringNode(Graph);
	UK2Node_CallFunction* PrintB = AddPrintStringNode(Graph);
	PrintA->NodePosX = 0; PrintA->NodePosY = 0;
	PrintB->NodePosX = 0; PrintB->NodePosY = 0;
	Schema->TryCreateConnection(Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), PrintA->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input));
	Schema->TryCreateConnection(PrintA->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), PrintB->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input));

	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("blueprint"), BP->GetPathName());
	TSharedPtr<FJsonObject> Env = RunEditGraphHandler(&FKismetAIModule::HandleLayoutGraph, Body);
	TestTrue(TEXT("layout_graph succeeded"), Env.IsValid() && Env->GetBoolField(TEXT("success")));

	const TSharedPtr<FJsonObject>* Data = GetData(Env);
	if (Data)
	{
		TestTrue(TEXT("some nodes moved"), (*Data)->GetNumberField(TEXT("nodes_moved")) > 0);
	}

	// The pile is now a left-to-right chain with no overlaps
	TestTrue(TEXT("A right of Event"), PrintA->NodePosX > Event->NodePosX);
	TestTrue(TEXT("B right of A"), PrintB->NodePosX > PrintA->NodePosX);
	return true;
}
