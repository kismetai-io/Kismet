// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Misc/AutomationTest.h"
#include "KismetAIValidation.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

constexpr auto ReadSourceTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// Validation Tests
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadSource_NullArgs, "KismetAI.Validation.ReadSource_NullArgs", ReadSourceTestFlags)
bool FReadSource_NullArgs::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> NullArgs;
	FValidationResult R = FKismetAIValidation::ValidateReadSource(NullArgs);
	TestFalse(TEXT("Null args is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadSource_InvalidMode, "KismetAI.Validation.ReadSource_InvalidMode", ReadSourceTestFlags)
bool FReadSource_InvalidMode::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("mode"), TEXT("invalid"));
	FValidationResult R = FKismetAIValidation::ValidateReadSource(Args);
	TestFalse(TEXT("Invalid mode is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadSource_FindMissingPattern, "KismetAI.Validation.ReadSource_FindMissingPattern", ReadSourceTestFlags)
bool FReadSource_FindMissingPattern::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("mode"), TEXT("find"));
	FValidationResult R = FKismetAIValidation::ValidateReadSource(Args);
	TestFalse(TEXT("Find without pattern is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadSource_FindEngineShortPattern, "KismetAI.Validation.ReadSource_FindEngineShortPattern", ReadSourceTestFlags)
bool FReadSource_FindEngineShortPattern::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("mode"), TEXT("find"));
	Args->SetStringField(TEXT("pattern"), TEXT("*.h"));
	Args->SetStringField(TEXT("scope"), TEXT("engine"));
	FValidationResult R = FKismetAIValidation::ValidateReadSource(Args);
	TestTrue(TEXT("Engine header glob is admitted under traversal budgets"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadSource_FindValidProject, "KismetAI.Validation.ReadSource_FindValidProject", ReadSourceTestFlags)
bool FReadSource_FindValidProject::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("mode"), TEXT("find"));
	Args->SetStringField(TEXT("pattern"), TEXT("*.cpp"));
	FValidationResult R = FKismetAIValidation::ValidateReadSource(Args);
	TestTrue(TEXT("Find with project scope passes"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadSource_FindValidEngine, "KismetAI.Validation.ReadSource_FindValidEngine", ReadSourceTestFlags)
bool FReadSource_FindValidEngine::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("mode"), TEXT("find"));
	Args->SetStringField(TEXT("pattern"), TEXT("Actor.h"));
	Args->SetStringField(TEXT("scope"), TEXT("engine"));
	FValidationResult R = FKismetAIValidation::ValidateReadSource(Args);
	TestTrue(TEXT("Find with engine scope and long pattern passes"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadSource_OutlineMissingFile, "KismetAI.Validation.ReadSource_OutlineMissingFile", ReadSourceTestFlags)
bool FReadSource_OutlineMissingFile::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("mode"), TEXT("outline"));
	FValidationResult R = FKismetAIValidation::ValidateReadSource(Args);
	TestFalse(TEXT("Outline without file is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadSource_RawMissingFile, "KismetAI.Validation.ReadSource_RawMissingFile", ReadSourceTestFlags)
bool FReadSource_RawMissingFile::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("mode"), TEXT("raw"));
	FValidationResult R = FKismetAIValidation::ValidateReadSource(Args);
	TestFalse(TEXT("Raw without file is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadSource_PathTraversal, "KismetAI.Validation.ReadSource_PathTraversal", ReadSourceTestFlags)
bool FReadSource_PathTraversal::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("mode"), TEXT("raw"));
	Args->SetStringField(TEXT("file"), TEXT("../../etc/passwd"));
	FValidationResult R = FKismetAIValidation::ValidateReadSource(Args);
	TestFalse(TEXT("Path traversal is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadSource_ValidOutline, "KismetAI.Validation.ReadSource_ValidOutline", ReadSourceTestFlags)
bool FReadSource_ValidOutline::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("mode"), TEXT("outline"));
	Args->SetStringField(TEXT("file"), TEXT("/some/path/Actor.h"));
	Args->SetStringField(TEXT("query"), TEXT("Location"));
	FValidationResult R = FKismetAIValidation::ValidateReadSource(Args);
	TestTrue(TEXT("Valid outline args pass"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadSource_ValidRaw, "KismetAI.Validation.ReadSource_ValidRaw", ReadSourceTestFlags)
bool FReadSource_ValidRaw::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("mode"), TEXT("raw"));
	Args->SetStringField(TEXT("file"), TEXT("/some/path/Actor.cpp"));
	Args->SetNumberField(TEXT("offset"), 100);
	Args->SetNumberField(TEXT("limit"), 200);
	FValidationResult R = FKismetAIValidation::ValidateReadSource(Args);
	TestTrue(TEXT("Valid raw args pass"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadSource_InvalidScope, "KismetAI.Validation.ReadSource_InvalidScope", ReadSourceTestFlags)
bool FReadSource_InvalidScope::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("mode"), TEXT("find"));
	Args->SetStringField(TEXT("pattern"), TEXT("Actor.h"));
	Args->SetStringField(TEXT("scope"), TEXT("system"));
	FValidationResult R = FKismetAIValidation::ValidateReadSource(Args);
	TestFalse(TEXT("Invalid scope is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadSource_LimitTooHigh, "KismetAI.Validation.ReadSource_LimitTooHigh", ReadSourceTestFlags)
bool FReadSource_LimitTooHigh::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("mode"), TEXT("raw"));
	Args->SetStringField(TEXT("file"), TEXT("/some/path/Actor.cpp"));
	Args->SetNumberField(TEXT("limit"), 1000);
	FValidationResult R = FKismetAIValidation::ValidateReadSource(Args);
	TestFalse(TEXT("Limit over 500 is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadSource_NegativeOffset, "KismetAI.Validation.ReadSource_NegativeOffset", ReadSourceTestFlags)
bool FReadSource_NegativeOffset::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("mode"), TEXT("raw"));
	Args->SetStringField(TEXT("file"), TEXT("/some/path/Actor.cpp"));
	Args->SetNumberField(TEXT("offset"), -5);
	FValidationResult R = FKismetAIValidation::ValidateReadSource(Args);
	TestFalse(TEXT("Negative offset is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadSource_DefaultMode, "KismetAI.Validation.ReadSource_DefaultMode", ReadSourceTestFlags)
bool FReadSource_DefaultMode::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("file"), TEXT("/some/path/Actor.h"));
	FValidationResult R = FKismetAIValidation::ValidateReadSource(Args);
	TestTrue(TEXT("Default mode (outline) with file passes"), R.bIsValid);
	return true;
}
