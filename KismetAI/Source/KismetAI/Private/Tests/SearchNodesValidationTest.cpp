// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Misc/AutomationTest.h"
#include "KismetAIValidation.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

constexpr auto SearchNodesTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// Tests
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchNodes_NullArgs, "KismetAI.Validation.SearchNodes_NullArgs", SearchNodesTestFlags)
bool FSearchNodes_NullArgs::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> NullArgs;
	FValidationResult R = FKismetAIValidation::ValidateSearchNodes(NullArgs);
	TestFalse(TEXT("Null args is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchNodes_MissingQuery, "KismetAI.Validation.SearchNodes_MissingQuery", SearchNodesTestFlags)
bool FSearchNodes_MissingQuery::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	FValidationResult R = FKismetAIValidation::ValidateSearchNodes(Args);
	TestFalse(TEXT("Missing query is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchNodes_EmptyQuery, "KismetAI.Validation.SearchNodes_EmptyQuery", SearchNodesTestFlags)
bool FSearchNodes_EmptyQuery::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("query"), TEXT(""));
	FValidationResult R = FKismetAIValidation::ValidateSearchNodes(Args);
	TestFalse(TEXT("Empty query is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchNodes_ValidMinimal, "KismetAI.Validation.SearchNodes_ValidMinimal", SearchNodesTestFlags)
bool FSearchNodes_ValidMinimal::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("query"), TEXT("print string"));
	FValidationResult R = FKismetAIValidation::ValidateSearchNodes(Args);
	TestTrue(TEXT("Valid minimal args passes"), R.bIsValid);
	return true;
}

// 0 means "use default" — OpenAI/Codex models fill optional ints with 0.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchNodes_LimitZeroAccepted, "KismetAI.Validation.SearchNodes_LimitZeroAccepted", SearchNodesTestFlags)
bool FSearchNodes_LimitZeroAccepted::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("query"), TEXT("test"));
	Args->SetNumberField(TEXT("limit"), 0);
	FValidationResult R = FKismetAIValidation::ValidateSearchNodes(Args);
	TestTrue(TEXT("Limit 0 is accepted (treated as default)"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchNodes_NegativeLimitRejected, "KismetAI.Validation.SearchNodes_NegativeLimitRejected", SearchNodesTestFlags)
bool FSearchNodes_NegativeLimitRejected::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("query"), TEXT("test"));
	Args->SetNumberField(TEXT("limit"), -1);
	FValidationResult R = FKismetAIValidation::ValidateSearchNodes(Args);
	TestFalse(TEXT("Negative limit is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchNodes_LimitTooHigh, "KismetAI.Validation.SearchNodes_LimitTooHigh", SearchNodesTestFlags)
bool FSearchNodes_LimitTooHigh::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("query"), TEXT("test"));
	Args->SetNumberField(TEXT("limit"), 500);
	FValidationResult R = FKismetAIValidation::ValidateSearchNodes(Args);
	TestFalse(TEXT("Limit 500 is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchNodes_FullValid, "KismetAI.Validation.SearchNodes_FullValid", SearchNodesTestFlags)
bool FSearchNodes_FullValid::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("query"), TEXT("line trace"));
	Args->SetStringField(TEXT("category"), TEXT("Physics"));
	Args->SetStringField(TEXT("class"), TEXT("KismetSystemLibrary"));
	Args->SetBoolField(TEXT("include_pins"), true);
	Args->SetNumberField(TEXT("limit"), 50);
	FValidationResult R = FKismetAIValidation::ValidateSearchNodes(Args);
	TestTrue(TEXT("Full valid args passes"), R.bIsValid);
	return true;
}
