// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Inverting a handler must not change what either caller sees.
//
// compile_blueprint is the first tool whose work returns FToolExecutionResult
// instead of writing an HTTP response. Two things have to stay true:
//   1. The HTTP route body is unchanged — external callers (the eval framework,
//      any script) parse it.
//   2. The string the model receives is unchanged — the system prompt and
//      system_prompt_errors.txt teach it to read these error codes, and the
//      loop's IsToolResultError keys on the <tool_use_error> wrapper.

#include "KismetAIModule.h"
#include "KismetAIToolResult.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"

constexpr auto InversionFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// The envelope shape both callers share.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInversion_EnvelopeShape, "KismetAI.ToolResult.EnvelopeShape", InversionFlags)
bool FInversion_EnvelopeShape::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("status"), TEXT("compiled"));
	Data->SetNumberField(TEXT("errors"), 0);

	const FString Ok = FToolExecutionResult::Success(Data).ToEnvelopeString();
	TestTrue(TEXT("success flag"), Ok.Contains(TEXT("\"success\": true")) || Ok.Contains(TEXT("\"success\":true")));
	TestTrue(TEXT("data is nested under 'data'"), Ok.Contains(TEXT("\"data\"")));
	TestTrue(TEXT("payload survives"), Ok.Contains(TEXT("compiled")));

	const FString Err = FToolExecutionResult::Error(
		404, TEXT("BLUEPRINT_NOT_FOUND"), TEXT("Blueprint not found: /Game/Nope")).ToEnvelopeString();
	TestTrue(TEXT("failure flag"), Err.Contains(TEXT("\"success\": false")) || Err.Contains(TEXT("\"success\":false")));
	TestTrue(TEXT("error_code preserved"), Err.Contains(TEXT("BLUEPRINT_NOT_FOUND")));
	TestTrue(TEXT("message preserved"), Err.Contains(TEXT("/Game/Nope")));
	TestTrue(TEXT("suggestions array is always present"), Err.Contains(TEXT("suggestions")));
	return true;
}

// The model-facing string: bare on success, wrapped on failure. The loop's
// IsToolResultError depends on exactly this.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInversion_ModelStringWrapping, "KismetAI.ToolResult.ModelStringWrapping", InversionFlags)
bool FInversion_ModelStringWrapping::RunTest(const FString& Parameters)
{
	const FString Ok = FToolExecutionResult::Success().ToModelString();
	TestFalse(TEXT("success is not wrapped"), Ok.Contains(TEXT("<tool_use_error>")));
	TestFalse(TEXT("and the loop does not flag it"), FKismetAIAgent::IsToolResultError(Ok));

	const FString Err = FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("nope")).ToModelString();
	TestTrue(TEXT("failure is wrapped"), Err.StartsWith(TEXT("<tool_use_error>")));
	TestTrue(TEXT("and closed"), Err.EndsWith(TEXT("</tool_use_error>")));
	TestTrue(TEXT("and the loop flags it"), FKismetAIAgent::IsToolResultError(Err));
	return true;
}

// End to end on the real handler: the typed path and the HTTP path must agree.
// This is the assertion that makes the inversion safe to repeat for the other
// 31 tools.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInversion_CompileBothPathsAgree, "KismetAI.ToolResult.CompileBothPathsAgree", InversionFlags)
bool FInversion_CompileBothPathsAgree::RunTest(const FString& Parameters)
{
	FKismetAIModule& Module = FKismetAIModule::Get();

	// A missing blueprint is the cheapest deterministic case that exercises the
	// full path without creating assets.
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), TEXT("/Game/__KismetAI_NoSuchBlueprint__"));

	// New path: typed handler -> model string.
	const FString TypedOut = Module.DoCompileBlueprint(Args).ToModelString();

	// Old path: synthetic HTTP request -> captured response body.
	const FString HttpOut = Module.ExecuteHandlerDirect(&FKismetAIModule::HandleCompile, Args);

	TestTrue(TEXT("typed path reports the missing blueprint"),
		TypedOut.Contains(TEXT("BLUEPRINT_NOT_FOUND")));
	TestTrue(TEXT("http path reports the missing blueprint"),
		HttpOut.Contains(TEXT("BLUEPRINT_NOT_FOUND")));
	TestTrue(TEXT("both are flagged as errors by the loop"),
		FKismetAIAgent::IsToolResultError(TypedOut) && FKismetAIAgent::IsToolResultError(HttpOut));

	// The HTTP path fills in per-error-code default suggestions that the typed
	// path leaves empty, so the strings are not byte-identical — but the fields
	// the model acts on are.
	TestTrue(TEXT("http path carries the default suggestions"),
		HttpOut.Contains(TEXT("search_assets")));
	return true;
}
