// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Misc/AutomationTest.h"
#include "KismetAIModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/TextBlock.h"
#include "Components/Button.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"

constexpr auto WidgetEditTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	static TSharedPtr<FJsonObject> RunEditWidget(UWidgetBlueprint* WBP, const TArray<TSharedPtr<FJsonValue>>& Ops)
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("widget"), WBP->GetPathName());
		Body->SetArrayField(TEXT("operations"), Ops);

		FString Resp = FKismetAIModule::Get().ExecuteHandlerDirect(&FKismetAIModule::HandleEditWidget, Body);
		Resp.RemoveFromStart(TEXT("<tool_use_error>"));
		Resp.RemoveFromEnd(TEXT("</tool_use_error>"));

		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp);
		FJsonSerializer::Deserialize(Reader, Parsed);
		return Parsed;
	}

	static UWidgetBlueprint* MakeEditTestWBP(const TCHAR* PackagePath, const TCHAR* AssetName)
	{
		UPackage* Pkg = CreatePackage(PackagePath);
		if (!Pkg) return nullptr;
		return Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			UUserWidget::StaticClass(), Pkg, AssetName, BPTYPE_Normal,
			UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
	}

	static TSharedPtr<FJsonValue> Op(TFunction<void(TSharedPtr<FJsonObject>&)> Build)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		Build(O);
		return MakeShared<FJsonValueObject>(O);
	}

	static TSharedPtr<FJsonValue> Vec2(double X, double Y)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(X));
		Arr.Add(MakeShared<FJsonValueNumber>(Y));
		return MakeShared<FJsonValueArray>(Arr);
	}

	// Find a node by "name" in a serialized widget tree node (depth-first).
	static TSharedPtr<FJsonObject> FindNode(const TSharedPtr<FJsonObject>& Node, const FString& Name)
	{
		if (!Node.IsValid()) return nullptr;
		FString NodeName;
		if (Node->TryGetStringField(TEXT("name"), NodeName) && NodeName == Name) return Node;
		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		if (Node->TryGetArrayField(TEXT("children"), Children))
		{
			for (const TSharedPtr<FJsonValue>& C : *Children)
			{
				if (TSharedPtr<FJsonObject> F = FindNode(C->AsObject(), Name)) return F;
			}
		}
		return nullptr;
	}

	static TSharedPtr<FJsonObject> RootOf(const TSharedPtr<FJsonObject>& Env)
	{
		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (!Env.IsValid() || !Env->TryGetObjectField(TEXT("data"), Data) || !Data) return nullptr;
		const TSharedPtr<FJsonObject>* Tree = nullptr;
		if (!(*Data)->TryGetObjectField(TEXT("tree"), Tree) || !Tree) return nullptr;
		const TSharedPtr<FJsonObject>* Root = nullptr;
		if (!(*Tree)->TryGetObjectField(TEXT("root"), Root) || !Root) return nullptr;
		return *Root;
	}
}

// (a) Build root + children via one batch; verify the WidgetTree directly and the readback tree.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetEdit_AddTree, "KismetAI.Widgets.EditAddTree", WidgetEditTestFlags)
bool FWidgetEdit_AddTree::RunTest(const FString& Parameters)
{
	UWidgetBlueprint* WBP = MakeEditTestWBP(TEXT("/Temp/KismetWidgetEditAdd"), TEXT("WBP_EditAdd"));
	if (!WBP || !WBP->WidgetTree) { AddError(TEXT("could not create WBP")); return false; }

	TArray<TSharedPtr<FJsonValue>> Ops;
	Ops.Add(Op([](TSharedPtr<FJsonObject>& O){ O->SetStringField(TEXT("action"), TEXT("add_widget")); O->SetStringField(TEXT("class"), TEXT("CanvasPanel")); O->SetStringField(TEXT("name"), TEXT("RootCanvas")); }));
	Ops.Add(Op([](TSharedPtr<FJsonObject>& O){ O->SetStringField(TEXT("action"), TEXT("add_widget")); O->SetStringField(TEXT("class"), TEXT("TextBlock")); O->SetStringField(TEXT("name"), TEXT("Title")); O->SetStringField(TEXT("parent"), TEXT("RootCanvas")); O->SetBoolField(TEXT("is_variable"), true); }));
	Ops.Add(Op([](TSharedPtr<FJsonObject>& O){ O->SetStringField(TEXT("action"), TEXT("add_widget")); O->SetStringField(TEXT("class"), TEXT("VerticalBox")); O->SetStringField(TEXT("name"), TEXT("Stack")); O->SetStringField(TEXT("parent"), TEXT("RootCanvas")); }));
	Ops.Add(Op([](TSharedPtr<FJsonObject>& O){ O->SetStringField(TEXT("action"), TEXT("add_widget")); O->SetStringField(TEXT("class"), TEXT("Button")); O->SetStringField(TEXT("name"), TEXT("OkButton")); O->SetStringField(TEXT("parent"), TEXT("Stack")); }));

	TSharedPtr<FJsonObject> Env = RunEditWidget(WBP, Ops);
	TestTrue(TEXT("success"), Env.IsValid() && Env->GetBoolField(TEXT("success")));
	if (!Env.IsValid()) return false;
	const TSharedPtr<FJsonObject>* Data = nullptr;
	Env->TryGetObjectField(TEXT("data"), Data);
	if (Data)
	{
		TestEqual(TEXT("4 ops succeeded"), (int32)(*Data)->GetNumberField(TEXT("operations_succeeded")), 4);
		TestEqual(TEXT("0 ops failed"), (int32)(*Data)->GetNumberField(TEXT("operations_failed")), 0);
		TestTrue(TEXT("compiled"), (*Data)->GetBoolField(TEXT("compiled")));
	}

	// Verify the live WidgetTree.
	UWidget* Root = WBP->WidgetTree->RootWidget;
	TestTrue(TEXT("root is CanvasPanel"), Root && Root->IsA<UCanvasPanel>());
	UWidget* Title = WBP->WidgetTree->FindWidget(FName("Title"));
	TestTrue(TEXT("Title is TextBlock + variable"), Title && Title->IsA<UTextBlock>() && Title->bIsVariable);
	UWidget* Btn = WBP->WidgetTree->FindWidget(FName("OkButton"));
	TestTrue(TEXT("Button under VerticalBox"), Btn && Btn->GetParent() && Btn->GetParent()->IsA<UVerticalBox>());

	// Verify the readback tree.
	TSharedPtr<FJsonObject> RootNode = RootOf(Env);
	TestTrue(TEXT("readback root present"), RootNode.IsValid());
	if (RootNode.IsValid())
	{
		TestEqual(TEXT("readback root class"), RootNode->GetStringField(TEXT("class")), FString(TEXT("CanvasPanel")));
		TestTrue(TEXT("readback finds Title"), FindNode(RootNode, TEXT("Title")).IsValid());
		TSharedPtr<FJsonObject> BtnNode = FindNode(RootNode, TEXT("OkButton"));
		TestTrue(TEXT("readback finds Button"), BtnNode.IsValid());
	}
	return true;
}

// (b) set_slot_property canvas position/size, verify via Cast<UCanvasPanelSlot>.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetEdit_SlotProps, "KismetAI.Widgets.EditSlotProps", WidgetEditTestFlags)
bool FWidgetEdit_SlotProps::RunTest(const FString& Parameters)
{
	UWidgetBlueprint* WBP = MakeEditTestWBP(TEXT("/Temp/KismetWidgetEditSlot"), TEXT("WBP_EditSlot"));
	if (!WBP || !WBP->WidgetTree) { AddError(TEXT("could not create WBP")); return false; }
	UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), FName("RootCanvas"));
	WBP->WidgetTree->RootWidget = Root;
	UTextBlock* Title = WBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName("Title"));
	Root->AddChild(Title);

	TArray<TSharedPtr<FJsonValue>> Ops;
	Ops.Add(Op([](TSharedPtr<FJsonObject>& O){
		O->SetStringField(TEXT("action"), TEXT("set_slot_property"));
		O->SetStringField(TEXT("name"), TEXT("Title"));
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetField(TEXT("position"), Vec2(120.0, 60.0));
		P->SetField(TEXT("size"), Vec2(240.0, 40.0));
		O->SetObjectField(TEXT("properties"), P);
	}));

	TSharedPtr<FJsonObject> Env = RunEditWidget(WBP, Ops);
	TestTrue(TEXT("success"), Env.IsValid() && Env->GetBoolField(TEXT("success")));

	UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Title->Slot);
	TestTrue(TEXT("slot is canvas slot"), Slot != nullptr);
	if (Slot)
	{
		TestEqual(TEXT("position X"), Slot->GetPosition().X, 120.0);
		TestEqual(TEXT("position Y"), Slot->GetPosition().Y, 60.0);
		TestEqual(TEXT("size X"), Slot->GetSize().X, 240.0);
		TestEqual(TEXT("size Y"), Slot->GetSize().Y, 40.0);
	}
	return true;
}

// (c) set_widget_property on a TextBlock Text, verify GetText().
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetEdit_WidgetProp, "KismetAI.Widgets.EditWidgetProp", WidgetEditTestFlags)
bool FWidgetEdit_WidgetProp::RunTest(const FString& Parameters)
{
	UWidgetBlueprint* WBP = MakeEditTestWBP(TEXT("/Temp/KismetWidgetEditProp"), TEXT("WBP_EditProp"));
	if (!WBP || !WBP->WidgetTree) { AddError(TEXT("could not create WBP")); return false; }
	UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), FName("RootCanvas"));
	WBP->WidgetTree->RootWidget = Root;
	UTextBlock* Title = WBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName("Title"));
	Root->AddChild(Title);

	TArray<TSharedPtr<FJsonValue>> Ops;
	Ops.Add(Op([](TSharedPtr<FJsonObject>& O){
		O->SetStringField(TEXT("action"), TEXT("set_widget_property"));
		O->SetStringField(TEXT("name"), TEXT("Title"));
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("Text"), TEXT("Hello World"));
		O->SetObjectField(TEXT("properties"), P);
	}));

	TSharedPtr<FJsonObject> Env = RunEditWidget(WBP, Ops);
	TestTrue(TEXT("success"), Env.IsValid() && Env->GetBoolField(TEXT("success")));
	TestEqual(TEXT("Text applied"), Title->GetText().ToString(), FString(TEXT("Hello World")));
	return true;
}

// (d) reparent the TextBlock into the VerticalBox; verify parent + slot class changed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetEdit_Reparent, "KismetAI.Widgets.EditReparent", WidgetEditTestFlags)
bool FWidgetEdit_Reparent::RunTest(const FString& Parameters)
{
	UWidgetBlueprint* WBP = MakeEditTestWBP(TEXT("/Temp/KismetWidgetEditReparent"), TEXT("WBP_EditReparent"));
	if (!WBP || !WBP->WidgetTree) { AddError(TEXT("could not create WBP")); return false; }
	UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), FName("RootCanvas"));
	WBP->WidgetTree->RootWidget = Root;
	UTextBlock* Title = WBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName("Title"));
	Root->AddChild(Title);
	UVerticalBox* VBox = WBP->WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), FName("Stack"));
	Root->AddChild(VBox);

	TestTrue(TEXT("Title starts in canvas slot"), Cast<UCanvasPanelSlot>(Title->Slot) != nullptr);

	TArray<TSharedPtr<FJsonValue>> Ops;
	Ops.Add(Op([](TSharedPtr<FJsonObject>& O){ O->SetStringField(TEXT("action"), TEXT("reparent_widget")); O->SetStringField(TEXT("name"), TEXT("Title")); O->SetStringField(TEXT("new_parent"), TEXT("Stack")); }));

	TSharedPtr<FJsonObject> Env = RunEditWidget(WBP, Ops);
	TestTrue(TEXT("success"), Env.IsValid() && Env->GetBoolField(TEXT("success")));
	TestTrue(TEXT("Title now under VerticalBox"), Title->GetParent() == VBox);
	TestTrue(TEXT("slot class is VerticalBoxSlot"), Cast<UVerticalBoxSlot>(Title->Slot) != nullptr);
	return true;
}

// (e) rename a bIsVariable widget referenced by a variable-get node; compile green + reference follows.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetEdit_RenameWithRef, "KismetAI.Widgets.EditRenameWithRef", WidgetEditTestFlags)
bool FWidgetEdit_RenameWithRef::RunTest(const FString& Parameters)
{
	UWidgetBlueprint* WBP = MakeEditTestWBP(TEXT("/Temp/KismetWidgetEditRename"), TEXT("WBP_EditRename"));
	if (!WBP || !WBP->WidgetTree || WBP->UbergraphPages.Num() == 0) { AddError(TEXT("could not create WBP")); return false; }
	UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), FName("RootCanvas"));
	WBP->WidgetTree->RootWidget = Root;
	UTextBlock* Title = WBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName("Title"));
	Title->bIsVariable = true;
	Root->AddChild(Title);
	// Compile once so the widget variable exists on the generated class for the getter to bind.
	FKismetEditorUtilities::CompileBlueprint(WBP);

	// Add a variable-get node referencing "Title" in the event graph.
	UEdGraph* Graph = WBP->UbergraphPages[0];
	UK2Node_VariableGet* Getter = NewObject<UK2Node_VariableGet>(Graph);
	Getter->VariableReference.SetSelfMember(FName("Title"));
	Graph->AddNode(Getter, false, false);
	Getter->CreateNewGuid();
	Getter->PostPlacedNewNode();
	Getter->AllocateDefaultPins();

	TArray<TSharedPtr<FJsonValue>> Ops;
	Ops.Add(Op([](TSharedPtr<FJsonObject>& O){ O->SetStringField(TEXT("action"), TEXT("rename_widget")); O->SetStringField(TEXT("name"), TEXT("Title")); O->SetStringField(TEXT("new_name"), TEXT("HeaderText")); }));

	TSharedPtr<FJsonObject> Env = RunEditWidget(WBP, Ops);
	TestTrue(TEXT("success"), Env.IsValid() && Env->GetBoolField(TEXT("success")));
	const TSharedPtr<FJsonObject>* Data = nullptr;
	if (Env.IsValid() && Env->TryGetObjectField(TEXT("data"), Data) && Data)
	{
		TestEqual(TEXT("1 op succeeded"), (int32)(*Data)->GetNumberField(TEXT("operations_succeeded")), 1);
		TestTrue(TEXT("compiled green"), (*Data)->GetBoolField(TEXT("compiled")));
	}

	TestTrue(TEXT("widget renamed in tree"), WBP->WidgetTree->FindWidget(FName("HeaderText")) != nullptr);
	TestTrue(TEXT("old name gone"), WBP->WidgetTree->FindWidget(FName("Title")) == nullptr);
	// The getter node's variable reference followed the rename.
	TestEqual(TEXT("getter ref updated"), Getter->VariableReference.GetMemberName(), FName("HeaderText"));
	return true;
}

// (f) remove_widget reports graph_references for a referenced widget and deletes it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetEdit_RemoveReportsRefs, "KismetAI.Widgets.EditRemoveReportsRefs", WidgetEditTestFlags)
bool FWidgetEdit_RemoveReportsRefs::RunTest(const FString& Parameters)
{
	UWidgetBlueprint* WBP = MakeEditTestWBP(TEXT("/Temp/KismetWidgetEditRemove"), TEXT("WBP_EditRemove"));
	if (!WBP || !WBP->WidgetTree || WBP->UbergraphPages.Num() == 0) { AddError(TEXT("could not create WBP")); return false; }
	UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), FName("RootCanvas"));
	WBP->WidgetTree->RootWidget = Root;
	UTextBlock* Title = WBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName("Title"));
	Title->bIsVariable = true;
	Root->AddChild(Title);
	FKismetEditorUtilities::CompileBlueprint(WBP);

	UEdGraph* Graph = WBP->UbergraphPages[0];
	UK2Node_VariableGet* Getter = NewObject<UK2Node_VariableGet>(Graph);
	Getter->VariableReference.SetSelfMember(FName("Title"));
	Graph->AddNode(Getter, false, false);
	Getter->CreateNewGuid();
	Getter->PostPlacedNewNode();
	Getter->AllocateDefaultPins();

	TArray<TSharedPtr<FJsonValue>> Ops;
	Ops.Add(Op([](TSharedPtr<FJsonObject>& O){ O->SetStringField(TEXT("action"), TEXT("remove_widget")); O->SetStringField(TEXT("name"), TEXT("Title")); }));

	TSharedPtr<FJsonObject> Env = RunEditWidget(WBP, Ops);
	TestTrue(TEXT("success"), Env.IsValid() && Env->GetBoolField(TEXT("success")));

	const TSharedPtr<FJsonObject>* Data = nullptr;
	Env->TryGetObjectField(TEXT("data"), Data);
	if (Data)
	{
		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		(*Data)->TryGetArrayField(TEXT("results"), Results);
		if (Results && Results->Num() == 1)
		{
			TSharedPtr<FJsonObject> R = (*Results)[0]->AsObject();
			const TArray<TSharedPtr<FJsonValue>>* Refs = nullptr;
			TestTrue(TEXT("graph_references reported"), R->TryGetArrayField(TEXT("graph_references"), Refs) && Refs && Refs->Num() > 0);
		}
		else { AddError(TEXT("expected one result")); }
	}
	TestTrue(TEXT("widget deleted"), WBP->WidgetTree->FindWidget(FName("Title")) == nullptr);
	return true;
}

// (g) replace TextBlock with Button keeps tree position (and name when compatible).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetEdit_Replace, "KismetAI.Widgets.EditReplace", WidgetEditTestFlags)
bool FWidgetEdit_Replace::RunTest(const FString& Parameters)
{
	UWidgetBlueprint* WBP = MakeEditTestWBP(TEXT("/Temp/KismetWidgetEditReplace"), TEXT("WBP_EditReplace"));
	if (!WBP || !WBP->WidgetTree) { AddError(TEXT("could not create WBP")); return false; }

	// Build through edit_widget so the widgets are registered the same way real usage registers
	// them (replace_widget routes through the engine's variable-guid bookkeeping).
	{
		TArray<TSharedPtr<FJsonValue>> Setup;
		Setup.Add(Op([](TSharedPtr<FJsonObject>& O){ O->SetStringField(TEXT("action"), TEXT("add_widget")); O->SetStringField(TEXT("class"), TEXT("CanvasPanel")); O->SetStringField(TEXT("name"), TEXT("RootCanvas")); }));
		Setup.Add(Op([](TSharedPtr<FJsonObject>& O){ O->SetStringField(TEXT("action"), TEXT("add_widget")); O->SetStringField(TEXT("class"), TEXT("TextBlock")); O->SetStringField(TEXT("name"), TEXT("Title")); O->SetStringField(TEXT("parent"), TEXT("RootCanvas")); }));
		RunEditWidget(WBP, Setup);
	}
	UCanvasPanel* Root = Cast<UCanvasPanel>(WBP->WidgetTree->RootWidget);
	if (!Root) { AddError(TEXT("setup did not create root canvas")); return false; }

	TArray<TSharedPtr<FJsonValue>> Ops;
	Ops.Add(Op([](TSharedPtr<FJsonObject>& O){ O->SetStringField(TEXT("action"), TEXT("replace_widget")); O->SetStringField(TEXT("name"), TEXT("Title")); O->SetStringField(TEXT("class"), TEXT("Button")); }));

	TSharedPtr<FJsonObject> Env = RunEditWidget(WBP, Ops);
	TestTrue(TEXT("success"), Env.IsValid() && Env->GetBoolField(TEXT("success")));

	// A single child remains under the root, now a Button.
	TestEqual(TEXT("root still has one child"), Root->GetChildrenCount(), 1);
	UWidget* Child = Root->GetChildAt(0);
	TestTrue(TEXT("child is now a Button"), Child && Child->IsA<UButton>());
	return true;
}

// (h) per-op failure isolation: a bad op among good ops — counts correct, good ops applied.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetEdit_FailureIsolation, "KismetAI.Widgets.EditFailureIsolation", WidgetEditTestFlags)
bool FWidgetEdit_FailureIsolation::RunTest(const FString& Parameters)
{
	UWidgetBlueprint* WBP = MakeEditTestWBP(TEXT("/Temp/KismetWidgetEditIsolation"), TEXT("WBP_EditIsolation"));
	if (!WBP || !WBP->WidgetTree) { AddError(TEXT("could not create WBP")); return false; }

	TArray<TSharedPtr<FJsonValue>> Ops;
	Ops.Add(Op([](TSharedPtr<FJsonObject>& O){ O->SetStringField(TEXT("action"), TEXT("add_widget")); O->SetStringField(TEXT("class"), TEXT("CanvasPanel")); O->SetStringField(TEXT("name"), TEXT("RootCanvas")); }));
	Ops.Add(Op([](TSharedPtr<FJsonObject>& O){ O->SetStringField(TEXT("action"), TEXT("add_widget")); O->SetStringField(TEXT("class"), TEXT("NotARealWidgetXyz")); O->SetStringField(TEXT("parent"), TEXT("RootCanvas")); }));
	Ops.Add(Op([](TSharedPtr<FJsonObject>& O){ O->SetStringField(TEXT("action"), TEXT("add_widget")); O->SetStringField(TEXT("class"), TEXT("Button")); O->SetStringField(TEXT("name"), TEXT("GoodButton")); O->SetStringField(TEXT("parent"), TEXT("RootCanvas")); }));

	TSharedPtr<FJsonObject> Env = RunEditWidget(WBP, Ops);
	TestTrue(TEXT("success envelope"), Env.IsValid() && Env->GetBoolField(TEXT("success")));
	const TSharedPtr<FJsonObject>* Data = nullptr;
	Env->TryGetObjectField(TEXT("data"), Data);
	if (Data)
	{
		TestEqual(TEXT("2 succeeded"), (int32)(*Data)->GetNumberField(TEXT("operations_succeeded")), 2);
		TestEqual(TEXT("1 failed"), (int32)(*Data)->GetNumberField(TEXT("operations_failed")), 1);
	}
	// Good ops applied despite the bad one in the middle.
	TestTrue(TEXT("root created"), WBP->WidgetTree->RootWidget && WBP->WidgetTree->RootWidget->IsA<UCanvasPanel>());
	TestTrue(TEXT("good button created"), WBP->WidgetTree->FindWidget(FName("GoodButton")) != nullptr);
	return true;
}

// (i) validation rejections: missing widget field, empty operations.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetEdit_ValidationRejections, "KismetAI.Widgets.EditValidationRejections", WidgetEditTestFlags)
bool FWidgetEdit_ValidationRejections::RunTest(const FString& Parameters)
{
	// Missing 'widget'.
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetArrayField(TEXT("operations"), {});
		FString Resp = FKismetAIModule::Get().ExecuteHandlerDirect(&FKismetAIModule::HandleEditWidget, Body);
		TestTrue(TEXT("missing widget rejected"), Resp.Contains(TEXT("<tool_use_error>")) || Resp.Contains(TEXT("\"success\":false")) || Resp.Contains(TEXT("MISSING_FIELD")));
	}
	// Empty operations.
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("widget"), TEXT("/Game/UI/WBP_Nope"));
		Body->SetArrayField(TEXT("operations"), {});
		FString Resp = FKismetAIModule::Get().ExecuteHandlerDirect(&FKismetAIModule::HandleEditWidget, Body);
		TestTrue(TEXT("empty operations rejected"), Resp.Contains(TEXT("<tool_use_error>")) || Resp.Contains(TEXT("\"success\":false")) || Resp.Contains(TEXT("MISSING_FIELD")));
	}
	return true;
}
