// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIAgent.h"
#include "Misc/AutomationTest.h"

// Note: There is no Classifier_Success200 test. ClassifyCallOutcome is only
// called from CallAnthropicOnce on the error/non-success path. The success
// path bypasses classification entirely (Outcome.Result is set directly to
// EModelCallResult::Success). Testing the classifier with a 200 response
// code would not reflect any real call site.

constexpr auto ClassifierTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// 529 (overloaded) is a retryable transient overload condition
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassifier_529Retryable, "KismetAI.Classifier.529Retryable", ClassifierTestFlags)
bool FClassifier_529Retryable::RunTest(const FString& Parameters)
{
	EModelCallResult Result = FKismetAIAgent::ClassifyCallOutcome(
		/*ExitCode=*/0, /*ResponseCode=*/529,
		/*ErrorType=*/TEXT("overloaded_error"),
		/*ErrorMessage=*/TEXT("Overloaded"),
		/*bAborted=*/false);
	TestTrue(TEXT("529 classifies as RetryableError"),
		Result == EModelCallResult::RetryableError);
	return true;
}

// 500-class server errors are retryable
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassifier_500Retryable, "KismetAI.Classifier.500Retryable", ClassifierTestFlags)
bool FClassifier_500Retryable::RunTest(const FString& Parameters)
{
	EModelCallResult Result = FKismetAIAgent::ClassifyCallOutcome(
		0, 500, TEXT("api_error"), TEXT("Internal server error"), false);
	TestTrue(TEXT("500 classifies as RetryableError"),
		Result == EModelCallResult::RetryableError);
	return true;
}

// 429 (rate limit) is retryable
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassifier_429Retryable, "KismetAI.Classifier.429Retryable", ClassifierTestFlags)
bool FClassifier_429Retryable::RunTest(const FString& Parameters)
{
	EModelCallResult Result = FKismetAIAgent::ClassifyCallOutcome(
		0, 429, TEXT("rate_limit_error"), TEXT("Rate limit"), false);
	TestTrue(TEXT("429 classifies as RetryableError"),
		Result == EModelCallResult::RetryableError);
	return true;
}

// 400 with invalid_request_error and an unrelated message is fatal
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassifier_400Fatal, "KismetAI.Classifier.400Fatal", ClassifierTestFlags)
bool FClassifier_400Fatal::RunTest(const FString& Parameters)
{
	EModelCallResult Result = FKismetAIAgent::ClassifyCallOutcome(
		0, 400, TEXT("invalid_request_error"), TEXT("bad json"), false);
	TestTrue(TEXT("400 with bad json classifies as FatalError"),
		Result == EModelCallResult::FatalError);
	return true;
}

// "prompt is too long" message routes to PromptTooLong (regardless of HTTP code path)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassifier_PromptTooLong, "KismetAI.Classifier.PromptTooLong", ClassifierTestFlags)
bool FClassifier_PromptTooLong::RunTest(const FString& Parameters)
{
	EModelCallResult Result = FKismetAIAgent::ClassifyCallOutcome(
		0, 400, TEXT("invalid_request_error"),
		TEXT("prompt is too long: 200001 tokens > 200000 maximum"),
		false);
	TestTrue(TEXT("Prompt too long routes to PromptTooLong"),
		Result == EModelCallResult::PromptTooLong);
	return true;
}

// "max_tokens exceed context limit" routes to MaxTokensOverflow
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassifier_MaxTokensOverflow, "KismetAI.Classifier.MaxTokensOverflow", ClassifierTestFlags)
bool FClassifier_MaxTokensOverflow::RunTest(const FString& Parameters)
{
	EModelCallResult Result = FKismetAIAgent::ClassifyCallOutcome(
		0, 400, TEXT("invalid_request_error"),
		TEXT("input length and `max_tokens` exceed context limit: 195000 + 8192 > 200000"),
		false);
	TestTrue(TEXT("Max tokens overflow routes to MaxTokensOverflow"),
		Result == EModelCallResult::MaxTokensOverflow);
	return true;
}

// curl exited non-zero with no HTTP response — connection failure, retryable
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassifier_ExitNonzeroNoResponseRetryable, "KismetAI.Classifier.ExitNonzeroNoResponseRetryable", ClassifierTestFlags)
bool FClassifier_ExitNonzeroNoResponseRetryable::RunTest(const FString& Parameters)
{
	EModelCallResult Result = FKismetAIAgent::ClassifyCallOutcome(
		/*ExitCode=*/35, /*ResponseCode=*/0,
		/*ErrorType=*/TEXT("connection_error"),
		/*ErrorMessage=*/TEXT("Could not connect to Anthropic API"),
		/*bAborted=*/false);
	TestTrue(TEXT("ExitCode != 0 and ResponseCode == 0 classifies as RetryableError"),
		Result == EModelCallResult::RetryableError);
	return true;
}

// Aborted call is fatal regardless of other inputs
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassifier_AbortedFatal, "KismetAI.Classifier.AbortedFatal", ClassifierTestFlags)
bool FClassifier_AbortedFatal::RunTest(const FString& Parameters)
{
	// Even if everything else looks retryable, abort wins
	EModelCallResult Result = FKismetAIAgent::ClassifyCallOutcome(
		0, 529, TEXT("overloaded_error"), TEXT("Overloaded"), /*bAborted=*/true);
	TestTrue(TEXT("Aborted classifies as FatalError"),
		Result == EModelCallResult::FatalError);
	return true;
}

// curl exit 6 (CURLE_COULDNT_RESOLVE_HOST) — DNS failure, fast-fail
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassifier_Exit6NetworkUnreachable, "KismetAI.Classifier.Exit6NetworkUnreachable", ClassifierTestFlags)
bool FClassifier_Exit6NetworkUnreachable::RunTest(const FString& Parameters)
{
	EModelCallResult Result = FKismetAIAgent::ClassifyCallOutcome(
		/*ExitCode=*/6, /*ResponseCode=*/0,
		/*ErrorType=*/TEXT("connection_error"),
		/*ErrorMessage=*/TEXT("Could not resolve host"),
		/*bAborted=*/false);
	TestTrue(TEXT("ExitCode 6 with no response classifies as NetworkUnreachable"),
		Result == EModelCallResult::NetworkUnreachable);
	return true;
}

// curl exit 7 (CURLE_COULDNT_CONNECT) — TCP connect failure, fast-fail
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassifier_Exit7NetworkUnreachable, "KismetAI.Classifier.Exit7NetworkUnreachable", ClassifierTestFlags)
bool FClassifier_Exit7NetworkUnreachable::RunTest(const FString& Parameters)
{
	EModelCallResult Result = FKismetAIAgent::ClassifyCallOutcome(
		/*ExitCode=*/7, /*ResponseCode=*/0,
		/*ErrorType=*/TEXT("connection_error"),
		/*ErrorMessage=*/TEXT("Failed to connect"),
		/*bAborted=*/false);
	TestTrue(TEXT("ExitCode 7 with no response classifies as NetworkUnreachable"),
		Result == EModelCallResult::NetworkUnreachable);
	return true;
}

// Other non-zero curl exits (e.g. 28 timeout) remain RetryableError, NOT NetworkUnreachable
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassifier_OtherExitNonzeroStillRetryable, "KismetAI.Classifier.OtherExitNonzeroStillRetryable", ClassifierTestFlags)
bool FClassifier_OtherExitNonzeroStillRetryable::RunTest(const FString& Parameters)
{
	// curl exit 28 = CURLE_OPERATION_TIMEDOUT — transient, retryable, but not NetworkUnreachable
	EModelCallResult Result = FKismetAIAgent::ClassifyCallOutcome(
		/*ExitCode=*/28, /*ResponseCode=*/0,
		/*ErrorType=*/TEXT("connection_error"),
		/*ErrorMessage=*/TEXT("Operation timed out"),
		/*bAborted=*/false);
	TestTrue(TEXT("ExitCode 28 still classifies as RetryableError, not NetworkUnreachable"),
		Result == EModelCallResult::RetryableError);
	return true;
}
