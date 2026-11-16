// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIAgent.h"
#include "Misc/AutomationTest.h"

constexpr auto UFETestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// Test: NetworkUnreachable produces an internet/connection-oriented message.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUFE_NetworkUnreachable_MentionsConnection, "KismetAI.UserFacingError.NetworkUnreachable_MentionsConnection", UFETestFlags)
bool FUFE_NetworkUnreachable_MentionsConnection::RunTest(const FString& Parameters)
{
	FModelCallOutcome Outcome;
	Outcome.Result = EModelCallResult::NetworkUnreachable;
	Outcome.HttpStatusCode = 0;
	Outcome.ErrorType = TEXT("");
	Outcome.ErrorMessage = TEXT("curl exit 6");

	FUserFacingError UFE = FKismetAIAgent::BuildUserFacingError(Outcome);
	TestTrue(TEXT("Message mentions internet"), UFE.Message.ToLower().Contains(TEXT("internet")));
	TestTrue(TEXT("At least one suggestion"), UFE.Suggestions.Num() > 0);
	return true;
}

// Test: overloaded_error ErrorType triggers the dedicated overload copy.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUFE_OverloadedErrorType_OverloadCopy, "KismetAI.UserFacingError.OverloadedErrorType_OverloadCopy", UFETestFlags)
bool FUFE_OverloadedErrorType_OverloadCopy::RunTest(const FString& Parameters)
{
	FModelCallOutcome Outcome;
	Outcome.Result = EModelCallResult::RetryableError;
	Outcome.HttpStatusCode = 529;
	Outcome.ErrorType = TEXT("overloaded_error");
	Outcome.ErrorMessage = TEXT("Overloaded");

	FUserFacingError UFE = FKismetAIAgent::BuildUserFacingError(Outcome);
	TestTrue(TEXT("Message contains 'overloaded'"), UFE.Message.ToLower().Contains(TEXT("overloaded")));
	TestTrue(TEXT("Message contains 'try again'"), UFE.Message.ToLower().Contains(TEXT("try again")));
	return true;
}

// Test: 529 status without overloaded_error type still triggers overload copy (AS3).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUFE_Http529NoErrorType_OverloadCopy, "KismetAI.UserFacingError.Http529NoErrorType_OverloadCopy", UFETestFlags)
bool FUFE_Http529NoErrorType_OverloadCopy::RunTest(const FString& Parameters)
{
	FModelCallOutcome Outcome;
	Outcome.Result = EModelCallResult::RetryableError;
	Outcome.HttpStatusCode = 529;
	Outcome.ErrorType = TEXT("");  // upstream CDN may strip the type
	Outcome.ErrorMessage = TEXT("");

	FUserFacingError UFE = FKismetAIAgent::BuildUserFacingError(Outcome);
	TestTrue(TEXT("Message contains 'overloaded'"), UFE.Message.ToLower().Contains(TEXT("overloaded")));
	TestTrue(TEXT("Message contains 'try again'"), UFE.Message.ToLower().Contains(TEXT("try again")));
	return true;
}

// Test: FatalError preserves the original error message verbatim.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUFE_FatalErrorPreservesOriginal, "KismetAI.UserFacingError.FatalErrorPreservesOriginal", UFETestFlags)
bool FUFE_FatalErrorPreservesOriginal::RunTest(const FString& Parameters)
{
	FModelCallOutcome Outcome;
	Outcome.Result = EModelCallResult::FatalError;
	Outcome.HttpStatusCode = 401;
	Outcome.ErrorType = TEXT("authentication_error");
	Outcome.ErrorMessage = TEXT("Authentication failed");

	FUserFacingError UFE = FKismetAIAgent::BuildUserFacingError(Outcome);
	TestEqual(TEXT("Message preserved verbatim"), UFE.Message, FString(TEXT("Authentication failed")));
	return true;
}

// Test: PromptTooLong preserves the original error message verbatim.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUFE_PromptTooLongPreservesOriginal, "KismetAI.UserFacingError.PromptTooLongPreservesOriginal", UFETestFlags)
bool FUFE_PromptTooLongPreservesOriginal::RunTest(const FString& Parameters)
{
	FModelCallOutcome Outcome;
	Outcome.Result = EModelCallResult::PromptTooLong;
	Outcome.HttpStatusCode = 400;
	Outcome.ErrorType = TEXT("invalid_request_error");
	Outcome.ErrorMessage = TEXT("prompt is too long: 200001 > 200000");

	FUserFacingError UFE = FKismetAIAgent::BuildUserFacingError(Outcome);
	TestEqual(TEXT("Message preserved verbatim"), UFE.Message, Outcome.ErrorMessage);
	return true;
}
