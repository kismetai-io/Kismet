// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Misc/AutomationTest.h"
#include "KismetAIValidation.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

constexpr auto ManageAssetTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// Tests
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageAsset_NullArgs, "KismetAI.Validation.ManageAsset_NullArgs", ManageAssetTestFlags)
bool FManageAsset_NullArgs::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> NullArgs;
	FValidationResult R = FKismetAIValidation::ValidateManageAsset(NullArgs);
	TestFalse(TEXT("Null args is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageAsset_MissingAction, "KismetAI.Validation.ManageAsset_MissingAction", ManageAssetTestFlags)
bool FManageAsset_MissingAction::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/BP_Test"));
	FValidationResult R = FKismetAIValidation::ValidateManageAsset(Args);
	TestFalse(TEXT("Missing action is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageAsset_InvalidAction, "KismetAI.Validation.ManageAsset_InvalidAction", ManageAssetTestFlags)
bool FManageAsset_InvalidAction::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("action"), TEXT("destroy"));
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/BP_Test"));
	FValidationResult R = FKismetAIValidation::ValidateManageAsset(Args);
	TestFalse(TEXT("Invalid action is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageAsset_MissingAssetPath, "KismetAI.Validation.ManageAsset_MissingAssetPath", ManageAssetTestFlags)
bool FManageAsset_MissingAssetPath::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("action"), TEXT("delete"));
	FValidationResult R = FKismetAIValidation::ValidateManageAsset(Args);
	TestFalse(TEXT("Missing asset_path is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageAsset_InvalidAssetPath, "KismetAI.Validation.ManageAsset_InvalidAssetPath", ManageAssetTestFlags)
bool FManageAsset_InvalidAssetPath::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("action"), TEXT("save"));
	Args->SetStringField(TEXT("asset_path"), TEXT("/Engine/BP_Test"));
	FValidationResult R = FKismetAIValidation::ValidateManageAsset(Args);
	TestFalse(TEXT("Non /Game/ path is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageAsset_DuplicateMissingNewName, "KismetAI.Validation.ManageAsset_DuplicateMissingNewName", ManageAssetTestFlags)
bool FManageAsset_DuplicateMissingNewName::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("action"), TEXT("duplicate"));
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/BP_Test"));
	FValidationResult R = FKismetAIValidation::ValidateManageAsset(Args);
	TestFalse(TEXT("Duplicate without new_name is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageAsset_RenameMissingNewName, "KismetAI.Validation.ManageAsset_RenameMissingNewName", ManageAssetTestFlags)
bool FManageAsset_RenameMissingNewName::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("action"), TEXT("rename"));
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/BP_Test"));
	FValidationResult R = FKismetAIValidation::ValidateManageAsset(Args);
	TestFalse(TEXT("Rename without new_name is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageAsset_MoveMissingNewPath, "KismetAI.Validation.ManageAsset_MoveMissingNewPath", ManageAssetTestFlags)
bool FManageAsset_MoveMissingNewPath::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("action"), TEXT("move"));
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/BP_Test"));
	FValidationResult R = FKismetAIValidation::ValidateManageAsset(Args);
	TestFalse(TEXT("Move without new_path is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageAsset_InvalidNewName, "KismetAI.Validation.ManageAsset_InvalidNewName", ManageAssetTestFlags)
bool FManageAsset_InvalidNewName::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("action"), TEXT("rename"));
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/BP_Test"));
	Args->SetStringField(TEXT("new_name"), TEXT("My New Blueprint"));
	FValidationResult R = FKismetAIValidation::ValidateManageAsset(Args);
	TestFalse(TEXT("new_name with spaces is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageAsset_InvalidNewPath, "KismetAI.Validation.ManageAsset_InvalidNewPath", ManageAssetTestFlags)
bool FManageAsset_InvalidNewPath::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("action"), TEXT("move"));
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/BP_Test"));
	Args->SetStringField(TEXT("new_path"), TEXT("/Engine/Stuff"));
	FValidationResult R = FKismetAIValidation::ValidateManageAsset(Args);
	TestFalse(TEXT("new_path not starting with /Game/ is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageAsset_ValidDelete, "KismetAI.Validation.ManageAsset_ValidDelete", ManageAssetTestFlags)
bool FManageAsset_ValidDelete::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("action"), TEXT("delete"));
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/BP_Unused"));
	FValidationResult R = FKismetAIValidation::ValidateManageAsset(Args);
	TestTrue(TEXT("Valid delete passes"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageAsset_ValidSave, "KismetAI.Validation.ManageAsset_ValidSave", ManageAssetTestFlags)
bool FManageAsset_ValidSave::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("action"), TEXT("save"));
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/BP_Modified"));
	FValidationResult R = FKismetAIValidation::ValidateManageAsset(Args);
	TestTrue(TEXT("Valid save passes"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageAsset_ValidDuplicate, "KismetAI.Validation.ManageAsset_ValidDuplicate", ManageAssetTestFlags)
bool FManageAsset_ValidDuplicate::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("action"), TEXT("duplicate"));
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/BP_Enemy"));
	Args->SetStringField(TEXT("new_name"), TEXT("BP_EnemyRanged"));
	Args->SetStringField(TEXT("new_path"), TEXT("/Game/Characters"));
	FValidationResult R = FKismetAIValidation::ValidateManageAsset(Args);
	TestTrue(TEXT("Valid duplicate passes"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageAsset_ValidMove, "KismetAI.Validation.ManageAsset_ValidMove", ManageAssetTestFlags)
bool FManageAsset_ValidMove::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("action"), TEXT("move"));
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/BP_Temp"));
	Args->SetStringField(TEXT("new_path"), TEXT("/Game/Characters"));
	FValidationResult R = FKismetAIValidation::ValidateManageAsset(Args);
	TestTrue(TEXT("Valid move passes"), R.bIsValid);
	return true;
}
