// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

// Phase 4 of UMG support: create_asset(asset_type="WidgetBlueprint") creates a
// Widget Blueprint via UWidgetBlueprintFactory and seeds a root panel. The
// handler does not save to disk (same as the Blueprint branch), so creating
// under /Game/ in these tests leaves no files behind.

#include "Misc/AutomationTest.h"
#include "KismetAIModule.h"
#include "KismetAIValidation.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/VerticalBox.h"

constexpr auto CreateWidgetBPTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	static TSharedPtr<FJsonObject> RunCreateAsset(TSharedPtr<FJsonObject> Body)
	{
		FString Resp = FKismetAIModule::Get().ExecuteHandlerDirect(&FKismetAIModule::HandleCreateAsset, Body);
		Resp.RemoveFromStart(TEXT("<tool_use_error>"));
		Resp.RemoveFromEnd(TEXT("</tool_use_error>"));
		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp);
		FJsonSerializer::Deserialize(Reader, Parsed);
		return Parsed;
	}

	static TSharedPtr<FJsonObject> MakeBody(const TCHAR* Name)
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("asset_type"), TEXT("WidgetBlueprint"));
		Body->SetStringField(TEXT("name"), Name);
		Body->SetStringField(TEXT("path"), TEXT("/Game/KismetAITransientTests"));
		return Body;
	}
}

// Validation accepts the WidgetBlueprint asset type.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateAsset_ValidWidgetBlueprint, "KismetAI.Validation.CreateAsset_ValidWidgetBlueprint", CreateWidgetBPTestFlags)
bool FCreateAsset_ValidWidgetBlueprint::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Args = MakeBody(TEXT("WBP_ValidationProbe"));
	FValidationResult R = FKismetAIValidation::ValidateCreateAsset(Args);
	TestTrue(TEXT("WidgetBlueprint asset_type accepted"), R.bIsValid);
	return true;
}

// Default creation: UserWidget parent, CanvasPanel root, compiles clean.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateWidgetBP_Defaults, "KismetAI.Widgets.CreateAsset_Defaults", CreateWidgetBPTestFlags)
bool FCreateWidgetBP_Defaults::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Env = RunCreateAsset(MakeBody(TEXT("WBP_KismetCreateDefault")));
	if (!Env.IsValid() || !Env->GetBoolField(TEXT("success")))
	{
		AddError(TEXT("create_asset WidgetBlueprint failed"));
		return false;
	}
	const TSharedPtr<FJsonObject>* Data = nullptr;
	if (!Env->TryGetObjectField(TEXT("data"), Data))
	{
		AddError(TEXT("no data envelope"));
		return false;
	}
	TestEqual(TEXT("asset_type"), (*Data)->GetStringField(TEXT("asset_type")), FString(TEXT("WidgetBlueprint")));
	TestEqual(TEXT("parent_class"), (*Data)->GetStringField(TEXT("parent_class")), FString(TEXT("UserWidget")));
	TestEqual(TEXT("root seeded with canvas"), (*Data)->GetStringField(TEXT("root_widget")), FString(TEXT("CanvasPanel")));

	const FString AssetPath = (*Data)->GetStringField(TEXT("asset_path"));
	UWidgetBlueprint* WBP = FindObject<UWidgetBlueprint>(nullptr, *AssetPath);
	TestNotNull(TEXT("created asset resolvable"), WBP);
	if (WBP)
	{
		TestTrue(TEXT("parent is UserWidget"), WBP->ParentClass == UUserWidget::StaticClass());
		TestNotNull(TEXT("widget tree exists"), WBP->WidgetTree.Get());
		if (WBP->WidgetTree)
		{
			TestNotNull(TEXT("root widget exists"), WBP->WidgetTree->RootWidget.Get());
			TestTrue(TEXT("root is a CanvasPanel"), WBP->WidgetTree->RootWidget && WBP->WidgetTree->RootWidget->IsA<UCanvasPanel>());
		}
	}
	return true;
}

// root_widget overrides: "none" leaves the tree empty; a panel class name is honored.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateWidgetBP_RootOverrides, "KismetAI.Widgets.CreateAsset_RootOverrides", CreateWidgetBPTestFlags)
bool FCreateWidgetBP_RootOverrides::RunTest(const FString& Parameters)
{
	{
		TSharedPtr<FJsonObject> Body = MakeBody(TEXT("WBP_KismetCreateNoRoot"));
		Body->SetStringField(TEXT("root_widget"), TEXT("none"));
		TSharedPtr<FJsonObject> Env = RunCreateAsset(Body);
		TestTrue(TEXT("create with root none succeeded"), Env.IsValid() && Env->GetBoolField(TEXT("success")));
		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (Env.IsValid() && Env->TryGetObjectField(TEXT("data"), Data))
		{
			TestEqual(TEXT("root reported none"), (*Data)->GetStringField(TEXT("root_widget")), FString(TEXT("none")));
			UWidgetBlueprint* WBP = FindObject<UWidgetBlueprint>(nullptr, *(*Data)->GetStringField(TEXT("asset_path")));
			TestTrue(TEXT("tree has no root"), WBP && WBP->WidgetTree && !WBP->WidgetTree->RootWidget);
		}
	}
	{
		TSharedPtr<FJsonObject> Body = MakeBody(TEXT("WBP_KismetCreateVBoxRoot"));
		Body->SetStringField(TEXT("root_widget"), TEXT("VerticalBox"));
		TSharedPtr<FJsonObject> Env = RunCreateAsset(Body);
		TestTrue(TEXT("create with vbox root succeeded"), Env.IsValid() && Env->GetBoolField(TEXT("success")));
		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (Env.IsValid() && Env->TryGetObjectField(TEXT("data"), Data))
		{
			UWidgetBlueprint* WBP = FindObject<UWidgetBlueprint>(nullptr, *(*Data)->GetStringField(TEXT("asset_path")));
			TestTrue(TEXT("root is a VerticalBox"), WBP && WBP->WidgetTree && WBP->WidgetTree->RootWidget && WBP->WidgetTree->RootWidget->IsA<UVerticalBox>());
		}
	}
	return true;
}

// Bad parents and bad roots fail with the specific error codes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateWidgetBP_BadInputs, "KismetAI.Widgets.CreateAsset_BadInputs", CreateWidgetBPTestFlags)
bool FCreateWidgetBP_BadInputs::RunTest(const FString& Parameters)
{
	auto ErrorCodeOf = [](const TSharedPtr<FJsonObject>& Env) -> FString
	{
		const TSharedPtr<FJsonObject>* Err = nullptr;
		if (Env.IsValid() && Env->TryGetObjectField(TEXT("error"), Err))
		{
			return (*Err)->GetStringField(TEXT("error_code"));
		}
		return FString();
	};

	{
		TSharedPtr<FJsonObject> Body = MakeBody(TEXT("WBP_KismetCreateBadParent"));
		Body->SetStringField(TEXT("parent_class"), TEXT("Actor"));
		TSharedPtr<FJsonObject> Env = RunCreateAsset(Body);
		TestTrue(TEXT("non-UserWidget parent rejected"), Env.IsValid() && !Env->GetBoolField(TEXT("success")));
		TestEqual(TEXT("parent error code"), ErrorCodeOf(Env), FString(TEXT("PARENT_CLASS_NOT_FOUND")));
	}
	{
		TSharedPtr<FJsonObject> Body = MakeBody(TEXT("WBP_KismetCreateBadRoot"));
		Body->SetStringField(TEXT("root_widget"), TEXT("TextBlock"));   // not a panel
		TSharedPtr<FJsonObject> Env = RunCreateAsset(Body);
		TestTrue(TEXT("non-panel root rejected"), Env.IsValid() && !Env->GetBoolField(TEXT("success")));
		TestEqual(TEXT("root error code"), ErrorCodeOf(Env), FString(TEXT("INVALID_ROOT_WIDGET")));
	}
	return true;
}
