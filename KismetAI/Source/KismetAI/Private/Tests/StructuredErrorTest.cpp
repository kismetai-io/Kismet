// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIValidation.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

constexpr auto ErrorTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// Test: Validation error JSON has correct structure
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructuredError_Format, "KismetAI.Error.StructuredFormat", ErrorTestFlags)
bool FStructuredError_Format::RunTest(const FString& Parameters)
{
	FValidationResult VR;
	VR.AddError(TEXT("blueprint"), TEXT("required field missing"));
	FString Json = VR.ToJsonString();

	// Parse back to verify structure
	TSharedPtr<FJsonObject> Parsed;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	TestTrue(TEXT("Error JSON is valid"), FJsonSerializer::Deserialize(Reader, Parsed));
	TestTrue(TEXT("Parsed is valid"), Parsed.IsValid());

	// Check top-level fields
	TestFalse(TEXT("success is false"), Parsed->GetBoolField(TEXT("success")));

	// Check error object
	const TSharedPtr<FJsonObject>* ErrorObj;
	TestTrue(TEXT("Has error object"), Parsed->TryGetObjectField(TEXT("error"), ErrorObj));
	TestTrue(TEXT("Has error_code"), (*ErrorObj)->HasField(TEXT("error_code")));
	TestTrue(TEXT("Has message"), (*ErrorObj)->HasField(TEXT("message")));
	TestTrue(TEXT("Has suggestions"), (*ErrorObj)->HasField(TEXT("suggestions")));

	// error_code is VALIDATION_ERROR
	TestEqual(TEXT("error_code is VALIDATION_ERROR"),
		(*ErrorObj)->GetStringField(TEXT("error_code")), TEXT("VALIDATION_ERROR"));

	// suggestions is an array
	const TArray<TSharedPtr<FJsonValue>>* SuggestionsArr;
	TestTrue(TEXT("suggestions is array"), (*ErrorObj)->TryGetArrayField(TEXT("suggestions"), SuggestionsArr));
	TestTrue(TEXT("suggestions is non-empty"), SuggestionsArr->Num() > 0);

	return true;
}

// Test: Multiple validation errors are all captured
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructuredError_MultipleErrors, "KismetAI.Error.MultipleErrors", ErrorTestFlags)
bool FStructuredError_MultipleErrors::RunTest(const FString& Parameters)
{
	FValidationResult VR;
	VR.AddError(TEXT("blueprint"), TEXT("required"));
	VR.AddError(TEXT("operations"), TEXT("must be array"));
	VR.AddError(TEXT("operations[0].action"), TEXT("missing"));

	TestEqual(TEXT("Three errors captured"), VR.Errors.Num(), 3);
	TestFalse(TEXT("Result is invalid"), VR.bIsValid);

	FString Json = VR.ToJsonString();
	TestTrue(TEXT("First error in JSON"), Json.Contains(TEXT("blueprint")));
	TestTrue(TEXT("Second error in JSON"), Json.Contains(TEXT("operations")));
	return true;
}

// Test: Error wrapping uses <tool_use_error> XML tags
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructuredError_XMLWrapping, "KismetAI.Error.XMLWrapping", ErrorTestFlags)
bool FStructuredError_XMLWrapping::RunTest(const FString& Parameters)
{
	FValidationResult VR;
	VR.AddError(TEXT("blueprint"), TEXT("required"));
	FString Json = VR.ToJsonString();
	FString Wrapped = FString::Printf(TEXT("<tool_use_error>%s</tool_use_error>"), *Json);

	TestTrue(TEXT("Starts with <tool_use_error>"), Wrapped.StartsWith(TEXT("<tool_use_error>")));
	TestTrue(TEXT("Ends with </tool_use_error>"), Wrapped.EndsWith(TEXT("</tool_use_error>")));
	TestTrue(TEXT("Contains JSON inside"), Wrapped.Contains(TEXT("VALIDATION_ERROR")));
	return true;
}

// Test: is_error detection logic
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStructuredError_IsErrorDetection, "KismetAI.Error.IsErrorDetection", ErrorTestFlags)
bool FStructuredError_IsErrorDetection::RunTest(const FString& Parameters)
{
	// Normal result — should NOT be is_error
	FString NormalResult = TEXT("{\"success\":true,\"data\":{}}");
	TestFalse(TEXT("Normal result is not error"),
		NormalResult.Contains(TEXT("<tool_use_error>")) || NormalResult.Contains(TEXT("\"success\":false")));

	// Error result — should be is_error
	FString ErrorResult = TEXT("<tool_use_error>{\"success\":false,\"error\":{}}</tool_use_error>");
	TestTrue(TEXT("Error result detected by XML tag"),
		ErrorResult.Contains(TEXT("<tool_use_error>")));

	// Handler error — should be is_error
	FString HandlerError = TEXT("{\"success\":false,\"error\":{\"error_code\":\"BLUEPRINT_NOT_FOUND\"}}");
	TestTrue(TEXT("Handler error detected by success:false"),
		HandlerError.Contains(TEXT("\"success\":false")));

	return true;
}
