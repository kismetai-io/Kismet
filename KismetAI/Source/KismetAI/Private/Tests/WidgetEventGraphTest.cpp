// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

// Phase 0 of UMG support: Widget Blueprint event graphs are ordinary K2 graphs,
// so the existing graph tools (describe_graph / insert_node / edit_graph /
// compile) must work on a UWidgetBlueprint without any widget-specific code.
// These tests pin that guarantee.

#include "Misc/AutomationTest.h"
#include "KismetAIModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/KismetEditorUtilities.h"

constexpr auto WidgetGraphTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	static TSharedPtr<FJsonObject> RunWidgetGraphHandler(
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

	static UWidgetBlueprint* MakeTestWidgetBlueprint(const TCHAR* PackagePath, const TCHAR* AssetName)
	{
		UPackage* Pkg = CreatePackage(PackagePath);
		if (!Pkg) return nullptr;
		return Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			UUserWidget::StaticClass(), Pkg, AssetName, BPTYPE_Normal,
			UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
	}

	static UK2Node_CustomEvent* AddCustomEvent(UEdGraph* Graph, const TCHAR* EventName)
	{
		UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
		Event->CustomFunctionName = FName(EventName);
		Graph->AddNode(Event, false, false);
		Event->CreateNewGuid();
		Event->AllocateDefaultPins();
		return Event;
	}
}

// describe_graph, insert_node (with auto-wiring), and compile all run unchanged
// against a Widget Blueprint's event graph.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetGraph_DescribeInsertCompile, "KismetAI.Widgets.EventGraph_DescribeInsertCompile", WidgetGraphTestFlags)
bool FWidgetGraph_DescribeInsertCompile::RunTest(const FString& Parameters)
{
	UWidgetBlueprint* WBP = MakeTestWidgetBlueprint(TEXT("/Temp/KismetWidgetGraphTest"), TEXT("WBP_KismetGraphTest"));
	if (!WBP || WBP->UbergraphPages.Num() == 0)
	{
		AddError(TEXT("could not create test widget blueprint"));
		return false;
	}
	const FString Path = WBP->GetPathName();
	UEdGraph* Graph = WBP->UbergraphPages[0];
	UK2Node_CustomEvent* Event = AddCustomEvent(Graph, TEXT("KismetWidgetTestEvent"));

	// describe_graph resolves the widget blueprint and lists the event node.
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("blueprint"), Path);
		TSharedPtr<FJsonObject> Env = RunWidgetGraphHandler(&FKismetAIModule::HandleDescribeGraph, Body);
		TestTrue(TEXT("describe_graph succeeded on widget blueprint"), Env.IsValid() && Env->GetBoolField(TEXT("success")));
	}

	// insert_node spawns Print String and auto-wires it to the event.
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("blueprint"), Path);
		Body->SetStringField(TEXT("node"), TEXT("Print String"));
		TSharedPtr<FJsonObject> From = MakeShared<FJsonObject>();
		From->SetStringField(TEXT("node"), Event->GetName());
		From->SetStringField(TEXT("pin"), TEXT("then"));
		Body->SetObjectField(TEXT("connect_from"), From);

		TSharedPtr<FJsonObject> Env = RunWidgetGraphHandler(&FKismetAIModule::HandleInsertNode, Body);
		TestTrue(TEXT("insert_node succeeded on widget blueprint"), Env.IsValid() && Env->GetBoolField(TEXT("success")));
	}

	// The event's then pin is actually wired to the new node.
	{
		UEdGraphPin* Then = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
		TestTrue(TEXT("event then pin exists"), Then != nullptr);
		if (Then)
		{
			TestEqual(TEXT("event wired to inserted node"), Then->LinkedTo.Num(), 1);
		}
	}

	// compile_blueprint runs the widget blueprint compiler without errors.
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("blueprint"), Path);
		TSharedPtr<FJsonObject> Env = RunWidgetGraphHandler(&FKismetAIModule::HandleCompile, Body);
		TestTrue(TEXT("compile succeeded on widget blueprint"), Env.IsValid() && Env->GetBoolField(TEXT("success")));

		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (Env.IsValid() && Env->TryGetObjectField(TEXT("data"), Data))
		{
			const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
			if ((*Data)->TryGetArrayField(TEXT("errors"), Errors))
			{
				TestEqual(TEXT("no compile errors"), Errors->Num(), 0);
			}
		}
	}

	return true;
}

// edit_graph wiring ops work on a widget event graph: connect two nodes and
// set a pin default through the batch endpoint.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetGraph_EditGraphOps, "KismetAI.Widgets.EventGraph_EditGraphOps", WidgetGraphTestFlags)
bool FWidgetGraph_EditGraphOps::RunTest(const FString& Parameters)
{
	UWidgetBlueprint* WBP = MakeTestWidgetBlueprint(TEXT("/Temp/KismetWidgetEditTest"), TEXT("WBP_KismetEditTest"));
	if (!WBP || WBP->UbergraphPages.Num() == 0)
	{
		AddError(TEXT("could not create test widget blueprint"));
		return false;
	}
	UEdGraph* Graph = WBP->UbergraphPages[0];
	UK2Node_CustomEvent* EventA = AddCustomEvent(Graph, TEXT("KismetWidgetEventA"));

	// Spawn a Print String via insert_node (unwired), then wire + set default via edit_graph.
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("blueprint"), WBP->GetPathName());
		Body->SetStringField(TEXT("node"), TEXT("Print String"));
		TSharedPtr<FJsonObject> Env = RunWidgetGraphHandler(&FKismetAIModule::HandleInsertNode, Body);
		TestTrue(TEXT("insert_node (unwired) succeeded"), Env.IsValid() && Env->GetBoolField(TEXT("success")));
	}

	FString PrintName;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->GetName().Contains(TEXT("CallFunction")))
		{
			PrintName = Node->GetName();
			break;
		}
	}
	if (PrintName.IsEmpty())
	{
		AddError(TEXT("inserted PrintString node not found in widget event graph"));
		return false;
	}

	auto Op = [](std::initializer_list<TPair<FString, FString>> Fields)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		for (const auto& F : Fields) O->SetStringField(F.Key, F.Value);
		return MakeShared<FJsonValueObject>(O);
	};
	TArray<TSharedPtr<FJsonValue>> Ops;
	Ops.Add(Op({ {TEXT("action"), TEXT("connect")}, {TEXT("source_node"), EventA->GetName()}, {TEXT("source_pin"), TEXT("then")}, {TEXT("target_node"), PrintName}, {TEXT("target_pin"), TEXT("execute")} }));
	Ops.Add(Op({ {TEXT("action"), TEXT("set_pin_default")}, {TEXT("node"), PrintName}, {TEXT("pin"), TEXT("InString")}, {TEXT("value"), TEXT("FromWidget")} }));

	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("blueprint"), WBP->GetPathName());
	Body->SetArrayField(TEXT("operations"), Ops);
	TSharedPtr<FJsonObject> Env = RunWidgetGraphHandler(&FKismetAIModule::HandleEditGraph, Body);
	TestTrue(TEXT("edit_graph succeeded on widget blueprint"), Env.IsValid() && Env->GetBoolField(TEXT("success")));

	const TSharedPtr<FJsonObject>* Data = nullptr;
	if (Env.IsValid() && Env->TryGetObjectField(TEXT("data"), Data))
	{
		TestEqual(TEXT("both ops succeeded"), (int32)(*Data)->GetNumberField(TEXT("operations_succeeded")), 2);
	}

	UEdGraphPin* Then = EventA->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	TestTrue(TEXT("event wired to print"), Then && Then->LinkedTo.Num() == 1);

	return true;
}
