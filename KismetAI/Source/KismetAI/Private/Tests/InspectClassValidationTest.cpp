// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Misc/AutomationTest.h"
#include "KismetAIValidation.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

constexpr auto InspectClassTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// Tests
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectClass_NullArgs, "KismetAI.Validation.InspectClass_NullArgs", InspectClassTestFlags)
bool FInspectClass_NullArgs::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> NullArgs;
	FValidationResult R = FKismetAIValidation::ValidateInspectClass(NullArgs);
	TestFalse(TEXT("Null args is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectClass_MissingClassName, "KismetAI.Validation.InspectClass_MissingClassName", InspectClassTestFlags)
bool FInspectClass_MissingClassName::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	FValidationResult R = FKismetAIValidation::ValidateInspectClass(Args);
	TestFalse(TEXT("Missing class_name is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectClass_EmptyClassName, "KismetAI.Validation.InspectClass_EmptyClassName", InspectClassTestFlags)
bool FInspectClass_EmptyClassName::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("class_name"), TEXT(""));
	FValidationResult R = FKismetAIValidation::ValidateInspectClass(Args);
	TestFalse(TEXT("Empty class_name is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectClass_ValidMinimal, "KismetAI.Validation.InspectClass_ValidMinimal", InspectClassTestFlags)
bool FInspectClass_ValidMinimal::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("class_name"), TEXT("Actor"));
	FValidationResult R = FKismetAIValidation::ValidateInspectClass(Args);
	TestTrue(TEXT("Valid minimal args passes"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectClass_InvalidFilter, "KismetAI.Validation.InspectClass_InvalidFilter", InspectClassTestFlags)
bool FInspectClass_InvalidFilter::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("class_name"), TEXT("Actor"));
	Args->SetStringField(TEXT("filter"), TEXT("variables"));
	FValidationResult R = FKismetAIValidation::ValidateInspectClass(Args);
	TestFalse(TEXT("Invalid filter is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectClass_ValidFilterProperties, "KismetAI.Validation.InspectClass_ValidFilterProperties", InspectClassTestFlags)
bool FInspectClass_ValidFilterProperties::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("class_name"), TEXT("Character"));
	Args->SetStringField(TEXT("filter"), TEXT("properties"));
	FValidationResult R = FKismetAIValidation::ValidateInspectClass(Args);
	TestTrue(TEXT("Filter 'properties' is valid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectClass_ValidFilterFunctions, "KismetAI.Validation.InspectClass_ValidFilterFunctions", InspectClassTestFlags)
bool FInspectClass_ValidFilterFunctions::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("class_name"), TEXT("Character"));
	Args->SetStringField(TEXT("filter"), TEXT("functions"));
	FValidationResult R = FKismetAIValidation::ValidateInspectClass(Args);
	TestTrue(TEXT("Filter 'functions' is valid"), R.bIsValid);
	return true;
}

// 0 means "use default" — OpenAI/Codex models fill optional ints with 0.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectClass_LimitZeroAccepted, "KismetAI.Validation.InspectClass_LimitZeroAccepted", InspectClassTestFlags)
bool FInspectClass_LimitZeroAccepted::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("class_name"), TEXT("Actor"));
	Args->SetNumberField(TEXT("limit"), 0);
	FValidationResult R = FKismetAIValidation::ValidateInspectClass(Args);
	TestTrue(TEXT("Limit 0 is accepted (treated as default)"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectClass_NegativeLimitRejected, "KismetAI.Validation.InspectClass_NegativeLimitRejected", InspectClassTestFlags)
bool FInspectClass_NegativeLimitRejected::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("class_name"), TEXT("Actor"));
	Args->SetNumberField(TEXT("limit"), -1);
	FValidationResult R = FKismetAIValidation::ValidateInspectClass(Args);
	TestFalse(TEXT("Negative limit is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectClass_LimitTooHigh, "KismetAI.Validation.InspectClass_LimitTooHigh", InspectClassTestFlags)
bool FInspectClass_LimitTooHigh::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("class_name"), TEXT("Actor"));
	Args->SetNumberField(TEXT("limit"), 200);
	FValidationResult R = FKismetAIValidation::ValidateInspectClass(Args);
	TestFalse(TEXT("Limit 200 is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectClass_FullValid, "KismetAI.Validation.InspectClass_FullValid", InspectClassTestFlags)
bool FInspectClass_FullValid::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("class_name"), TEXT("/Script/Engine.Character"));
	Args->SetStringField(TEXT("filter"), TEXT("all"));
	Args->SetBoolField(TEXT("include_inherited"), true);
	Args->SetStringField(TEXT("category"), TEXT("Movement"));
	Args->SetStringField(TEXT("query"), TEXT("jump"));
	Args->SetNumberField(TEXT("limit"), 50);
	FValidationResult R = FKismetAIValidation::ValidateInspectClass(Args);
	TestTrue(TEXT("Full valid args passes"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectClass_BlueprintPath, "KismetAI.Validation.InspectClass_BlueprintPath", InspectClassTestFlags)
bool FInspectClass_BlueprintPath::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("class_name"), TEXT("/Game/BP_Enemy"));
	FValidationResult R = FKismetAIValidation::ValidateInspectClass(Args);
	TestTrue(TEXT("Blueprint path as class_name is valid"), R.bIsValid);
	return true;
}
