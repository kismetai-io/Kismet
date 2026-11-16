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
#include "Components/TextBlock.h"
#include "Components/Button.h"
#include "Kismet2/KismetEditorUtilities.h"

constexpr auto WidgetReadTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	static TSharedPtr<FJsonObject> RunWidgetHandler(
		bool (FKismetAIModule::*Handler)(const FHttpServerRequest&, const FHttpResultCallback&),
		const FString& WidgetPath)
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("widget"), WidgetPath);

		FString Resp = FKismetAIModule::Get().ExecuteHandlerDirect(Handler, Body);
		Resp.RemoveFromStart(TEXT("<tool_use_error>"));
		Resp.RemoveFromEnd(TEXT("</tool_use_error>"));

		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp);
		FJsonSerializer::Deserialize(Reader, Parsed);
		return Parsed;
	}

	static UWidgetBlueprint* MakeWidgetBlueprint(const TCHAR* PackagePath, const TCHAR* AssetName)
	{
		UPackage* Pkg = CreatePackage(PackagePath);
		if (!Pkg) return nullptr;
		return Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			UUserWidget::StaticClass(), Pkg, AssetName, BPTYPE_Normal,
			UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
	}

	// Find a node by "name" in a serialized widget node and its children (depth-first).
	static const TSharedPtr<FJsonObject> FindNodeByName(const TSharedPtr<FJsonObject>& Node, const FString& Name)
	{
		if (!Node.IsValid()) return nullptr;
		FString NodeName;
		if (Node->TryGetStringField(TEXT("name"), NodeName) && NodeName == Name)
		{
			return Node;
		}
		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		if (Node->TryGetArrayField(TEXT("children"), Children))
		{
			for (const TSharedPtr<FJsonValue>& Child : *Children)
			{
				if (TSharedPtr<FJsonObject> Found = FindNodeByName(Child->AsObject(), Name))
				{
					return Found;
				}
			}
		}
		return nullptr;
	}
}

// describe_widget returns the Designer hierarchy: root class, named widgets, slot layout.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetRead_DescribeTree, "KismetAI.Widgets.DescribeTree", WidgetReadTestFlags)
bool FWidgetRead_DescribeTree::RunTest(const FString& Parameters)
{
	UWidgetBlueprint* WBP = MakeWidgetBlueprint(TEXT("/Temp/KismetWidgetReadTest"), TEXT("WBP_KismetReadTest"));
	if (!WBP || !WBP->WidgetTree)
	{
		AddError(TEXT("could not create test widget blueprint"));
		return false;
	}

	// Build: CanvasPanel root -> { TextBlock "Title" (variable), VerticalBox -> Button }
	UWidgetTree* Tree = WBP->WidgetTree;
	UCanvasPanel* Root = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), FName("RootCanvas"));
	Tree->RootWidget = Root;

	UTextBlock* Title = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName("Title"));
	Title->bIsVariable = true;
	Title->SetText(FText::FromString(TEXT("Hello")));
	Root->AddChild(Title);
	if (UCanvasPanelSlot* TitleSlot = Cast<UCanvasPanelSlot>(Title->Slot))
	{
		TitleSlot->SetPosition(FVector2D(100.0f, 50.0f));
		TitleSlot->SetSize(FVector2D(200.0f, 30.0f));
	}

	UVerticalBox* VBox = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), FName("Stack"));
	Root->AddChild(VBox);
	UButton* Btn = Tree->ConstructWidget<UButton>(UButton::StaticClass(), FName("OkButton"));
	VBox->AddChild(Btn);

	TSharedPtr<FJsonObject> Env = RunWidgetHandler(&FKismetAIModule::HandleDescribeWidget, WBP->GetPathName());
	TestTrue(TEXT("describe_widget success"), Env.IsValid() && Env->GetBoolField(TEXT("success")));
	if (!Env.IsValid()) return false;

	const TSharedPtr<FJsonObject>* Data = nullptr;
	if (!Env->TryGetObjectField(TEXT("data"), Data) || !Data)
	{
		AddError(TEXT("missing data"));
		return false;
	}

	const TSharedPtr<FJsonObject>* RootNode = nullptr;
	TestTrue(TEXT("has root"), (*Data)->TryGetObjectField(TEXT("root"), RootNode) && RootNode);
	if (RootNode)
	{
		TestEqual(TEXT("root class CanvasPanel"), (*RootNode)->GetStringField(TEXT("class")), FString(TEXT("CanvasPanel")));

		TSharedPtr<FJsonObject> TitleNode = FindNodeByName(*RootNode, TEXT("Title"));
		TestTrue(TEXT("Title node found"), TitleNode.IsValid());
		if (TitleNode.IsValid())
		{
			TestEqual(TEXT("Title class TextBlock"), TitleNode->GetStringField(TEXT("class")), FString(TEXT("TextBlock")));
			TestTrue(TEXT("Title is_variable"), TitleNode->GetBoolField(TEXT("is_variable")));

			const TSharedPtr<FJsonObject>* Slot = nullptr;
			TestTrue(TEXT("Title has slot"), TitleNode->TryGetObjectField(TEXT("slot"), Slot) && Slot);
			if (Slot)
			{
				TestEqual(TEXT("Title slot class"), (*Slot)->GetStringField(TEXT("class")), FString(TEXT("CanvasPanelSlot")));
				const TArray<TSharedPtr<FJsonValue>>* Pos = nullptr;
				if ((*Slot)->TryGetArrayField(TEXT("position"), Pos) && Pos->Num() == 2)
				{
					TestEqual(TEXT("Title pos X"), (*Pos)[0]->AsNumber(), 100.0);
					TestEqual(TEXT("Title pos Y"), (*Pos)[1]->AsNumber(), 50.0);
				}
				else
				{
					AddError(TEXT("Title slot position missing"));
				}
			}
		}

		// Button nested under the vertical box, with a VerticalBoxSlot.
		TSharedPtr<FJsonObject> BtnNode = FindNodeByName(*RootNode, TEXT("OkButton"));
		TestTrue(TEXT("Button node found"), BtnNode.IsValid());
		if (BtnNode.IsValid())
		{
			TestEqual(TEXT("Button class"), BtnNode->GetStringField(TEXT("class")), FString(TEXT("Button")));
			const TSharedPtr<FJsonObject>* BtnSlot = nullptr;
			if (BtnNode->TryGetObjectField(TEXT("slot"), BtnSlot) && BtnSlot)
			{
				TestEqual(TEXT("Button slot class"), (*BtnSlot)->GetStringField(TEXT("class")), FString(TEXT("VerticalBoxSlot")));
			}
			else
			{
				AddError(TEXT("Button slot missing"));
			}
		}
	}

	return true;
}

// widget_overview reports variables, root class, and counts.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetRead_Overview, "KismetAI.Widgets.Overview", WidgetReadTestFlags)
bool FWidgetRead_Overview::RunTest(const FString& Parameters)
{
	UWidgetBlueprint* WBP = MakeWidgetBlueprint(TEXT("/Temp/KismetWidgetOverviewTest"), TEXT("WBP_KismetOverviewTest"));
	if (!WBP || !WBP->WidgetTree)
	{
		AddError(TEXT("could not create test widget blueprint"));
		return false;
	}

	UWidgetTree* Tree = WBP->WidgetTree;
	UCanvasPanel* Root = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), FName("RootCanvas"));
	Tree->RootWidget = Root;
	UTextBlock* Title = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName("Title"));
	Title->bIsVariable = true;
	Root->AddChild(Title);

	TSharedPtr<FJsonObject> Env = RunWidgetHandler(&FKismetAIModule::HandleWidgetOverview, WBP->GetPathName());
	TestTrue(TEXT("widget_overview success"), Env.IsValid() && Env->GetBoolField(TEXT("success")));
	if (!Env.IsValid()) return false;

	const TSharedPtr<FJsonObject>* Data = nullptr;
	if (!Env->TryGetObjectField(TEXT("data"), Data) || !Data)
	{
		AddError(TEXT("missing data"));
		return false;
	}

	TestEqual(TEXT("root_class CanvasPanel"), (*Data)->GetStringField(TEXT("root_class")), FString(TEXT("CanvasPanel")));

	const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
	TestTrue(TEXT("has variables array"), (*Data)->TryGetArrayField(TEXT("variables"), Variables));
	bool bFoundTitle = false;
	if (Variables)
	{
		for (const TSharedPtr<FJsonValue>& V : *Variables)
		{
			if (TSharedPtr<FJsonObject> VObj = V->AsObject())
			{
				if (VObj->GetStringField(TEXT("name")) == TEXT("Title")) bFoundTitle = true;
			}
		}
	}
	TestTrue(TEXT("variables contains Title"), bFoundTitle);

	// Static arrays always present.
	const TArray<TSharedPtr<FJsonValue>>* Anims = nullptr;
	TestTrue(TEXT("has animations array"), (*Data)->TryGetArrayField(TEXT("animations"), Anims));
	const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
	TestTrue(TEXT("has graphs array"), (*Data)->TryGetArrayField(TEXT("graphs"), Graphs));

	return true;
}

// describe_widget on a non-widget Blueprint fails with WIDGET_BLUEPRINT_NOT_FOUND.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetRead_NotAWidget, "KismetAI.Widgets.NotAWidgetBlueprint", WidgetReadTestFlags)
bool FWidgetRead_NotAWidget::RunTest(const FString& Parameters)
{
	UPackage* Pkg = CreatePackage(TEXT("/Temp/KismetWidgetActorBPTest"));
	UBlueprint* ActorBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Pkg, FName("BP_KismetPlainActor"), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!ActorBP)
	{
		AddError(TEXT("could not create plain actor blueprint"));
		return false;
	}

	TSharedPtr<FJsonObject> Env = RunWidgetHandler(&FKismetAIModule::HandleDescribeWidget, ActorBP->GetPathName());
	TestTrue(TEXT("response parsed"), Env.IsValid());
	if (Env.IsValid())
	{
		bool bSuccess = false;
		Env->TryGetBoolField(TEXT("success"), bSuccess);
		TestFalse(TEXT("non-widget blueprint -> not success"), bSuccess);
	}
	return true;
}

// A bogus path yields a structured not-found error envelope, not a crash.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetRead_NotFound, "KismetAI.Widgets.MissingAsset", WidgetReadTestFlags)
bool FWidgetRead_NotFound::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Env = RunWidgetHandler(&FKismetAIModule::HandleDescribeWidget,
		TEXT("/Game/__DoesNotExist__/WBP_Nope"));
	TestTrue(TEXT("response parsed"), Env.IsValid());
	if (Env.IsValid())
	{
		bool bSuccess = false;
		Env->TryGetBoolField(TEXT("success"), bSuccess);
		TestFalse(TEXT("missing asset -> not success"), bSuccess);
	}
	return true;
}
