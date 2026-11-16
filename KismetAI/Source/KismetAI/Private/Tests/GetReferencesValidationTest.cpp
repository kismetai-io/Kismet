// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Misc/AutomationTest.h"
#include "KismetAIValidation.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

constexpr auto GetRefsTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// Tests
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetRefs_NullArgs, "KismetAI.Validation.GetReferences_NullArgs", GetRefsTestFlags)
bool FGetRefs_NullArgs::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> NullArgs;
	FValidationResult R = FKismetAIValidation::ValidateGetReferences(NullArgs);
	TestFalse(TEXT("Null args is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetRefs_MissingAssetPath, "KismetAI.Validation.GetReferences_MissingAssetPath", GetRefsTestFlags)
bool FGetRefs_MissingAssetPath::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	FValidationResult R = FKismetAIValidation::ValidateGetReferences(Args);
	TestFalse(TEXT("Missing asset_path is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetRefs_EmptyAssetPath, "KismetAI.Validation.GetReferences_EmptyAssetPath", GetRefsTestFlags)
bool FGetRefs_EmptyAssetPath::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT(""));
	FValidationResult R = FKismetAIValidation::ValidateGetReferences(Args);
	TestFalse(TEXT("Empty asset_path is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetRefs_InvalidPath, "KismetAI.Validation.GetReferences_InvalidPath", GetRefsTestFlags)
bool FGetRefs_InvalidPath::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Engine/Content/SomeAsset"));
	FValidationResult R = FKismetAIValidation::ValidateGetReferences(Args);
	TestFalse(TEXT("Non /Game/ path is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetRefs_ValidMinimal, "KismetAI.Validation.GetReferences_ValidMinimal", GetRefsTestFlags)
bool FGetRefs_ValidMinimal::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/Blueprints/BP_Player"));
	FValidationResult R = FKismetAIValidation::ValidateGetReferences(Args);
	TestTrue(TEXT("Valid minimal args passes"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetRefs_InvalidDirection, "KismetAI.Validation.GetReferences_InvalidDirection", GetRefsTestFlags)
bool FGetRefs_InvalidDirection::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/BP_Test"));
	Args->SetStringField(TEXT("direction"), TEXT("sideways"));
	FValidationResult R = FKismetAIValidation::ValidateGetReferences(Args);
	TestFalse(TEXT("Invalid direction is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetRefs_ValidDirectionBoth, "KismetAI.Validation.GetReferences_ValidDirectionBoth", GetRefsTestFlags)
bool FGetRefs_ValidDirectionBoth::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/BP_Test"));
	Args->SetStringField(TEXT("direction"), TEXT("both"));
	FValidationResult R = FKismetAIValidation::ValidateGetReferences(Args);
	TestTrue(TEXT("Direction 'both' is valid"), R.bIsValid);
	return true;
}

// 0 means "use default" — OpenAI/Codex models fill optional ints with 0.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetRefs_LimitZeroAccepted, "KismetAI.Validation.GetReferences_LimitZeroAccepted", GetRefsTestFlags)
bool FGetRefs_LimitZeroAccepted::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/BP_Test"));
	Args->SetNumberField(TEXT("limit"), 0);
	FValidationResult R = FKismetAIValidation::ValidateGetReferences(Args);
	TestTrue(TEXT("Limit 0 is accepted (treated as default)"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetRefs_NegativeLimitRejected, "KismetAI.Validation.GetReferences_NegativeLimitRejected", GetRefsTestFlags)
bool FGetRefs_NegativeLimitRejected::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/BP_Test"));
	Args->SetNumberField(TEXT("limit"), -1);
	FValidationResult R = FKismetAIValidation::ValidateGetReferences(Args);
	TestFalse(TEXT("Negative limit is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetRefs_LimitTooHigh, "KismetAI.Validation.GetReferences_LimitTooHigh", GetRefsTestFlags)
bool FGetRefs_LimitTooHigh::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/BP_Test"));
	Args->SetNumberField(TEXT("limit"), 999);
	FValidationResult R = FKismetAIValidation::ValidateGetReferences(Args);
	TestFalse(TEXT("Limit 999 is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetRefs_FullValid, "KismetAI.Validation.GetReferences_FullValid", GetRefsTestFlags)
bool FGetRefs_FullValid::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/Characters/BP_Enemy"));
	Args->SetStringField(TEXT("direction"), TEXT("dependencies"));
	Args->SetBoolField(TEXT("recursive"), true);
	Args->SetBoolField(TEXT("filter_engine"), false);
	Args->SetNumberField(TEXT("limit"), 100);
	FValidationResult R = FKismetAIValidation::ValidateGetReferences(Args);
	TestTrue(TEXT("Full valid args passes"), R.bIsValid);
	return true;
}
