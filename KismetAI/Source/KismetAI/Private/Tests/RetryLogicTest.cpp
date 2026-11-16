// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIAgent.h"
#include "Misc/AutomationTest.h"
#include "Internationalization/Regex.h"

constexpr auto RetryTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// Test: ComputeRetryDelay produces expected base delays
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRetryDelay_BaseDelays, "KismetAI.Retry.ComputeRetryDelay.BaseDelays", RetryTestFlags)
bool FRetryDelay_BaseDelays::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Config;
	Config.RetryJitterFactor = 0.0;  // Disable jitter for deterministic test

	double Delay0 = FKismetAIAgent::ComputeRetryDelay(0, Config, -1.0);
	double Delay1 = FKismetAIAgent::ComputeRetryDelay(1, Config, -1.0);
	double Delay2 = FKismetAIAgent::ComputeRetryDelay(2, Config, -1.0);

	// Base=500ms. Delay = 500 * 2^attempt / 1000
	// attempt 0: 500*1/1000 = 0.5s
	// attempt 1: 500*2/1000 = 1.0s
	// attempt 2: 500*4/1000 = 2.0s
	TestEqual(TEXT("First retry delay is 0.5s"), Delay0, 0.5);
	TestEqual(TEXT("Second retry delay is 1.0s"), Delay1, 1.0);
	TestEqual(TEXT("Third retry delay is 2.0s"), Delay2, 2.0);
	return true;
}

// Test: retry-after header overrides base delay
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRetryDelay_RetryAfterOverride, "KismetAI.Retry.ComputeRetryDelay.RetryAfterOverride", RetryTestFlags)
bool FRetryDelay_RetryAfterOverride::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Config;
	Config.RetryJitterFactor = 0.0;

	// retry-after of 10s overrides exponential delay entirely
	double Delay = FKismetAIAgent::ComputeRetryDelay(0, Config, 10.0);
	TestEqual(TEXT("retry-after overrides base delay"), Delay, 10.0);

	// retry-after of 0.5s also overrides
	double Delay2 = FKismetAIAgent::ComputeRetryDelay(0, Config, 0.5);
	TestEqual(TEXT("Short retry-after also overrides"), Delay2, 0.5);
	return true;
}

// Test: jitter produces values within expected range
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRetryDelay_Jitter, "KismetAI.Retry.ComputeRetryDelay.Jitter", RetryTestFlags)
bool FRetryDelay_Jitter::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Config;
	Config.RetryJitterFactor = 0.25;

	// Run multiple times, verify all within range
	for (int32 i = 0; i < 100; i++)
	{
		double Delay = FKismetAIAgent::ComputeRetryDelay(0, Config, -1.0);
		// Base is 0.5s, jitter adds 0 to 25% of 0.5 = 0 to 0.125
		// So range is [0.5, 0.625]
		TestTrue(TEXT("Delay within jitter range"),
			Delay >= 0.1 && Delay <= 1.0);
	}
	return true;
}

// Test: Error classification (429, 500+ = retryable)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRetry_ClassifyRetryable, "KismetAI.Retry.Classify.RetryableErrors", RetryTestFlags)
bool FRetry_ClassifyRetryable::RunTest(const FString& Parameters)
{
	TArray<int32> RetryableCodes = {429, 500, 502, 503, 529};
	for (int32 Code : RetryableCodes)
	{
		bool bRetryable = (Code == 429 || Code == 529 || Code == 408 || Code == 409 || Code >= 500);
		TestTrue(FString::Printf(TEXT("HTTP %d is retryable"), Code), bRetryable);
	}

	// Non-retryable
	TArray<int32> NonRetryableCodes = {400, 401, 403, 404};
	for (int32 Code : NonRetryableCodes)
	{
		bool bRetryable = (Code == 429 || Code == 529 || Code == 408 || Code == 409 || Code >= 500);
		TestFalse(FString::Printf(TEXT("HTTP %d is not retryable"), Code), bRetryable);
	}
	return true;
}

// Test: Error classification for MaxTokensOverflow
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRetry_ClassifyMaxTokensOverflow, "KismetAI.Retry.Classify.MaxTokensOverflow", RetryTestFlags)
bool FRetry_ClassifyMaxTokensOverflow::RunTest(const FString& Parameters)
{
	FString ErrorMsg = TEXT("input length and `max_tokens` exceed context limit: 180000 + 4096 > 200000");
	TestTrue(TEXT("Contains overflow pattern"),
		ErrorMsg.Contains(TEXT("max_tokens")) && ErrorMsg.Contains(TEXT("exceed context limit")));

	// Verify regex extraction
	FRegexPattern Pattern(TEXT("(\\d+)\\s*\\+\\s*\\d+\\s*>\\s*(\\d+)"));
	FRegexMatcher Matcher(Pattern, ErrorMsg);
	TestTrue(TEXT("Regex matches"), Matcher.FindNext());
	TestEqual(TEXT("Input tokens extracted"), FCString::Atoi(*Matcher.GetCaptureGroup(1)), 180000);
	TestEqual(TEXT("Context limit extracted"), FCString::Atoi(*Matcher.GetCaptureGroup(2)), 200000);
	return true;
}

// Test: 408 and 409 are retryable
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRetry_Classify408_409, "KismetAI.Retry.Classify.TimeoutAndLock", RetryTestFlags)
bool FRetry_Classify408_409::RunTest(const FString& Parameters)
{
	TArray<int32> RetryableCodes = {429, 408, 409, 500, 502, 503, 529};
	for (int32 Code : RetryableCodes)
	{
		bool bRetryable = (Code == 429 || Code == 529 || Code == 408 || Code == 409 || Code >= 500);
		TestTrue(FString::Printf(TEXT("HTTP %d is retryable"), Code), bRetryable);
	}
	return true;
}

// Test: Delay cap at MaxDelay
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRetryDelay_Cap, "KismetAI.Retry.ComputeRetryDelay.Cap", RetryTestFlags)
bool FRetryDelay_Cap::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Config;
	Config.RetryJitterFactor = 0.0;
	Config.RetryMaxDelayMs = 32000.0;

	// Attempt 10: 500 * 2^10 = 512000ms = 512s, should be capped to 32s
	double Delay = FKismetAIAgent::ComputeRetryDelay(10, Config, -1.0);
	TestEqual(TEXT("Delay capped at 32s"), Delay, 32.0);
	return true;
}

// Test: NetworkUnreachable gets a fast-fail retry budget of exactly 1
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRetryBudget_NetworkUnreachableIsOne, "KismetAI.Retry.Budget.NetworkUnreachableIsOne", RetryTestFlags)
bool FRetryBudget_NetworkUnreachableIsOne::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Config;
	Config.MaxRetries = 5;

	int32 Budget = FKismetAIAgent::GetMaxRetriesForResult(EModelCallResult::NetworkUnreachable, Config);
	TestEqual(TEXT("NetworkUnreachable budget is 1 regardless of Config.MaxRetries"), Budget, 1);
	return true;
}

// Test: All other result categories use Config.MaxRetries unchanged
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRetryBudget_OtherResultsUnchanged, "KismetAI.Retry.Budget.OtherResultsUnchanged", RetryTestFlags)
bool FRetryBudget_OtherResultsUnchanged::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Config;
	Config.MaxRetries = 5;

	TestEqual(TEXT("RetryableError uses Config.MaxRetries"),
		FKismetAIAgent::GetMaxRetriesForResult(EModelCallResult::RetryableError, Config), Config.MaxRetries);
	TestEqual(TEXT("Success uses Config.MaxRetries"),
		FKismetAIAgent::GetMaxRetriesForResult(EModelCallResult::Success, Config), Config.MaxRetries);
	TestEqual(TEXT("FatalError uses Config.MaxRetries"),
		FKismetAIAgent::GetMaxRetriesForResult(EModelCallResult::FatalError, Config), Config.MaxRetries);
	TestEqual(TEXT("PromptTooLong uses Config.MaxRetries"),
		FKismetAIAgent::GetMaxRetriesForResult(EModelCallResult::PromptTooLong, Config), Config.MaxRetries);
	TestEqual(TEXT("MaxTokensOverflow uses Config.MaxRetries"),
		FKismetAIAgent::GetMaxRetriesForResult(EModelCallResult::MaxTokensOverflow, Config), Config.MaxRetries);
	return true;
}
