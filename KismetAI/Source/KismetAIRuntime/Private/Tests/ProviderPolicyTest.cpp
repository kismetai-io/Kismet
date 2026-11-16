// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
#include "Fakes/ScriptedAgentSeams.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderPolicy_OverflowDecreases,
	"KismetAI.ProviderPolicy.OverflowStrictlyDecreases", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProviderPolicy_OverflowDecreases::RunTest(const FString& Parameters)
{
	using namespace KismetAITestFakes;
	for (const int32 ReportedWindow : {9000, 11000, 20000})
	{
		FKismetAIAgentConfig Config;
		Config.ProviderId = TEXT("anthropic");
		Config.Model = TEXT("claude-sonnet-4-6");
		Config.MaxTokens = 5000;
		Config.MaxRetries = 1;
		Config.RetryBaseDelayMs = Config.RetryMaxDelayMs = Config.RetryJitterFactor = 0;
		const FModelCallRequest Request({}, {}, TEXT("fixed prompt"), Config);
		int32 Calls = 0;
		const FModelCallOutcome Result = FKismetAICurlModelClient::RunRetryLoop(Request, {},
			[&](const FModelCallRequest& Attempt)
			{
				++Calls;
				TestEqual(TEXT("provider fixed"), Attempt.Config.ProviderId, Request.Config.ProviderId);
				TestEqual(TEXT("model fixed"), Attempt.Config.Model, Request.Config.Model);
				TestEqual(TEXT("prompt fixed"), Attempt.SystemPrompt, Request.SystemPrompt);
				if (Calls > 1)
				{
					TestTrue(TEXT("retry output strictly decreases"), Attempt.Config.MaxTokens < Config.MaxTokens);
					return MakeTextTurn(TEXT("done"));
				}
				FModelCallOutcome Out;
				Out.Result = EModelCallResult::MaxTokensOverflow;
				Out.ContextLimit = ReportedWindow;
				Out.InputTokens = 5000;
				Out.RequestCount = 1;
				Out.UsageReports.Add(MakeUsage(5000, 0));
				return Out;
			}, [] { return false; });
		TestEqual(TEXT("only actual decrease retried"), Calls, ReportedWindow == 9000 ? 2 : 1);
		TestEqual(TEXT("original request unchanged"), Request.Config.MaxTokens, 5000);
		TestTrue(TEXT("failed attempt usage retained"), Result.UsageReports.Num() >= 1);
		if (ReportedWindow != 9000)
		{
			TestEqual(TEXT("refusal retains attempt count"), Result.AttemptCount, 1);
			TestEqual(TEXT("refusal retains request count"), Result.RequestCount, 1);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderPolicy_InvalidProviderTransport,
	"KismetAI.ProviderPolicy.InvalidProviderBeforeTransport", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProviderPolicy_InvalidProviderTransport::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Config;
	Config.ProviderId = TEXT("misspelled-provider");
	Config.ApiBaseUrl = TEXT("http://127.0.0.1:1");
	Config.MaxRetries = 0;
	FKismetAICurlModelClient Client(Config);
	const auto Result = Client.Call(FModelCallRequest({}, {}, TEXT("no dispatch"), Config), {});
	TestEqual(TEXT("invalid selection refused before curl"), Result.ErrorType, FString(TEXT("unsupported_provider")));
	TestEqual(TEXT("no request dispatched"), Result.RequestCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderPolicy_InvalidRequestPolicy,
	"KismetAI.ProviderPolicy.InvalidRequestPolicyBeforeAttempt", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProviderPolicy_InvalidRequestPolicy::RunTest(const FString& Parameters)
{
	using namespace KismetAITestFakes;
	for (int32 Case = 0; Case < 6; ++Case)
	{
		FKismetAIAgentConfig Config;
		Config.ProviderId = TEXT("anthropic");
		Config.Model = TEXT("unknown-policy-model");
		Config.MaxRetries = 0;
		Config.ThinkingMode = EThinkingMode::Disabled;
		Config.bProviderEnvironmentResolved = true;
		if (Case == 0) Config.MaxTokens = 0;
		if (Case == 1) Config.AdaptiveOutputTokensOverride = TEXT("1024junk");
		if (Case == 2) Config.ThinkingMode = EThinkingMode::Enabled;
		if (Case == 3) Config.OperatingContextLimit = -1;
		if (Case == 4) Config.OperatingContextLimit = 1000;
		if (Case == 5) { Config.OperatingContextLimit = 1000; Config.MaxTokens = 100; }
		int32 Attempts = 0;
		const auto Result = FKismetAICurlModelClient::RunRetryLoop(FModelCallRequest({}, {}, Case == 5 ? FString::ChrN(8000, TCHAR('x')) : FString(TEXT("fixed")), Config), {},
			[&](const FModelCallRequest&) { ++Attempts; return MakeTextTurn(TEXT("must not dispatch")); }, [] { return false; });
		TestEqual(TEXT("unsupported policy refuses before attempt"), Attempts, 0);
		TestEqual(TEXT("invalid policy produces clear error"), Result.ErrorType, FString(TEXT("invalid_model_policy")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderPolicy_OperatingInputsFrozen,
	"KismetAI.ProviderPolicy.OperatingInputsFrozen", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProviderPolicy_OperatingInputsFrozen::RunTest(const FString& Parameters)
{
	using namespace KismetAITestFakes;
	FKismetAIAgentConfig Config;
	Config.OperatingContextLimit = 32768;
	Config.ProviderId = TEXT("anthropic");
	Config.Model = TEXT("claude-sonnet-4-6");
	Config.MaxTokens = 5000;
	Config.MaxRetries = 1;
	Config.RetryBaseDelayMs = Config.RetryMaxDelayMs = Config.RetryJitterFactor = 0;
	const FModelCallRequest Request({}, {}, TEXT("fixed"), Config);
	Config.OperatingContextLimit = 65536;
	Config.ProviderId = TEXT("openai");
	int32 Attempts = 0;
	FKismetAICurlModelClient::RunRetryLoop(Request, {}, [&](const FModelCallRequest& Attempt)
	{
		TestEqual(TEXT("retry retains admitted operating context"), Attempt.Config.OperatingContextLimit, 32768);
		TestEqual(TEXT("retry retains admitted provider"), Attempt.Config.ProviderId, FString(TEXT("anthropic")));
		if (++Attempts == 1) { FModelCallOutcome Out; Out.Result=EModelCallResult::MaxTokensOverflow; Out.ContextLimit=9000; Out.InputTokens=5000; return Out; }
		return MakeTextTurn(TEXT("done"));
	}, [] { return false; });
	TestEqual(TEXT("retry actually exercised"), Attempts, 2);
	TestEqual(TEXT("unrelated next config unchanged"), Config.OperatingContextLimit, 65536);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderPolicy_UnknownAdaptive,
	"KismetAI.ProviderPolicy.UnknownAdaptiveRemainsUsable", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProviderPolicy_UnknownAdaptive::RunTest(const FString& Parameters)
{
	using namespace KismetAITestFakes;
	FKismetAIAgentConfig Config;
	Config.Model = TEXT("unknown-policy-model");
	Config.ProviderId = TEXT("openrouter");
	Config.OperatingContextLimit = 32768;
	Config.MaxRetries = 0;
	Config.bProviderEnvironmentResolved = true;
	Config.AdaptiveOutputTokensOverride = TEXT("512");
	int32 Calls = 0;
	const auto Result = FKismetAICurlModelClient::RunRetryLoop(FModelCallRequest({}, {}, TEXT("fixed"), Config), {},
		[&](const FModelCallRequest& Request)
		{
			++Calls;
			TestEqual(TEXT("next-turn escape hatch does not change initial output"), Request.Config.MaxTokens, 4096);
			return MakeTextTurn(TEXT("done"));
		}, [] { return false; });
	TestEqual(TEXT("unknown default adaptive accepted"), Calls, 1);
	TestTrue(TEXT("request succeeds without invented optional capabilities"), Result.Result == EModelCallResult::Success);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderPolicy_ExplicitBudgetThinkingContract,
	"KismetAI.ProviderPolicy.ExplicitBudgetThinkingContract", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProviderPolicy_ExplicitBudgetThinkingContract::RunTest(const FString& Parameters)
{
	using namespace KismetAITestFakes;
	for (const bool bOpenAI : {true, false})
	{
		FKismetAIAgentConfig Config;
		Config.ProviderId = bOpenAI ? TEXT("openai") : TEXT("anthropic");
		Config.Model = bOpenAI ? TEXT("gpt-6.1-sol") : TEXT("claude-haiku-4-5-20251001");
		Config.ThinkingMode = EThinkingMode::Enabled;
		Config.ThinkingBudgetTokens = bOpenAI ? 10000 : 0;
		Config.MaxRetries = 0;
		Config.bProviderEnvironmentResolved = true;
		int32 Attempts = 0;
		const auto Result = FKismetAICurlModelClient::RunRetryLoop(FModelCallRequest({}, {}, TEXT("fixed"), Config), {},
			[&](const FModelCallRequest&) { ++Attempts; return MakeTextTurn(TEXT("must not dispatch")); }, [] { return false; });
		TestEqual(bOpenAI ? TEXT("OpenAI cannot silently ignore explicit budget thinking") : TEXT("Anthropic cannot send zero thinking budget"), Attempts, 0);
		TestEqual(TEXT("explicit invalid budget thinking produces clear policy refusal"), Result.ErrorType, FString(TEXT("invalid_model_policy")));
	}
	return true;
}
