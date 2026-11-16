// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Fakes/FixtureTransport.h"
#include "Fakes/ScriptedAgentSeams.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

using namespace KismetAIFixtureTransport;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTurnRequest_RetrySnapshot,
	"KismetAI.TurnRequest.RetrySnapshot", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FTurnRequest_RetrySnapshot::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Config;
	Config.Model = TEXT("claude-sonnet-4-6");
	Config.ApiBaseUrl = TEXT("http://127.0.0.1:18001");
	Config.MaxTokens = 16000;
	Config.MaxRetries = 1;
	Config.RetryBaseDelayMs = 0;
	Config.RetryMaxDelayMs = 0;
	Config.RetryJitterFactor = 0;
	Config.ThinkingMode = EThinkingMode::Disabled;
	TArray<FAgentMessage> Messages{FAgentMessage::MakeText(EAgentRole::User, TEXT("original message"))};
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));
	Schema->SetStringField(TEXT("description"), TEXT("original schema"));
	TSharedPtr<FJsonObject> Tool = MakeShared<FJsonObject>();
	Tool->SetStringField(TEXT("name"), TEXT("read_blueprint"));
	Tool->SetStringField(TEXT("description"), TEXT("read"));
	Tool->SetObjectField(TEXT("input_schema"), Schema);
	TArray<TSharedPtr<FJsonValue>> Tools{MakeShared<FJsonValueObject>(Tool)};
	FString Prompt = TEXT("original prompt");
	const FModelCallRequest Request{Messages, Tools, Prompt, Config};
	FScriptedAttempt First;
	First.HttpStatus = 400;
	First.Bytes = FAnthropicStreamBuilder().Raw(TEXT("{\"error\":{\"type\":\"invalid_request_error\",\"message\":\"input length and `max_tokens` exceed context limit: 190000 + 16000 > 200000\"}}")).Bytes();
	First.OnDelivered = [&]()
	{
		Config.Model = TEXT("claude-haiku-4-5-20251001");
		Config.ApiBaseUrl = TEXT("http://127.0.0.1:18002");
		Schema->SetStringField(TEXT("description"), TEXT("changed schema"));
		Messages[0].Parts[0].Text = TEXT("changed message");
		Prompt = TEXT("changed prompt");
	};
	FScriptedAttempt Success;
	Success.Bytes = FAnthropicStreamBuilder().MessageStart().BlockStart(TEXT("text")).Text(TEXT("done")).BlockStop().MessageEnd(TEXT("end_turn")).Bytes();
	FFixtureTransportModelClient Client({First, Success});
	AddExpectedError(TEXT("LLM API returned status 400"), EAutomationExpectedErrorFlags::Contains, 2);
	FModelCallbacks Callbacks;
	Callbacks.OnAttemptRetrying = [&](int32, const FModelCallOutcome& Failed)
	{
		TestEqual(TEXT("retry event identifies failed attempt"), Failed.AttemptCount, 1);
		TestEqual(TEXT("retry event retains failed output cap"), Failed.EffectiveMaxTokens, 16000);
	};
	const FModelCallOutcome Outcome = Client.CallWithRetry(Request, Callbacks);
	TestTrue(TEXT("negotiation retries successfully"), Outcome.Result == EModelCallResult::Success);
	TestEqual(TEXT("negotiated output cap is explicit"), Outcome.EffectiveMaxTokens, 9000);
	TestEqual(TEXT("attempt count is explicit"), Outcome.AttemptCount, 2);
	TestEqual(TEXT("persistent output preference is unchanged"), Config.MaxTokens, 16000);
	if (!TestEqual(TEXT("two attempts"), Client.CapturedBodies.Num(), 2)) return false;
	TestEqual(TEXT("retry retains model"), Client.CapturedConfigs[1].Model, FString(TEXT("claude-sonnet-4-6")));
	TestEqual(TEXT("retry retains endpoint"), Client.CapturedConfigs[1].ApiBaseUrl, FString(TEXT("http://127.0.0.1:18001")));
	TestEqual(TEXT("retry uses negotiated budget"), Client.CapturedConfigs[1].MaxTokens, 9000);
	TestTrue(TEXT("retry retains nested schema"), Client.CapturedBodies[1].Contains(TEXT("original schema")));
	TestFalse(TEXT("retry excludes changed schema"), Client.CapturedBodies[1].Contains(TEXT("changed schema")));
	TestTrue(TEXT("retry retains prompt"), Client.CapturedBodies[1].Contains(TEXT("original prompt")));
	TestTrue(TEXT("retry retains message"), Client.CapturedBodies[1].Contains(TEXT("original message")));
	FKismetAIAgentConfig OtherConfig;
	OtherConfig.MaxTokens = 16000;
	const FModelCallRequest OtherRequest{Messages, Tools, Prompt, OtherConfig};
	FFixtureTransportModelClient OtherClient({Success});
	OtherClient.CallWithRetry(OtherRequest, {});
	TestEqual(TEXT("other session retains configured budget"), OtherClient.CapturedConfigs[0].MaxTokens, 16000);
	const FModelCallRequest NextRequest{Messages, Tools, Prompt, Config};
	FFixtureTransportModelClient NextClient({Success});
	NextClient.CallWithRetry(NextRequest, {});
	TestEqual(TEXT("next call retains configured budget"), NextClient.CapturedConfigs[0].MaxTokens, 16000);
	TestEqual(TEXT("next call picks changed model"), NextClient.CapturedConfigs[0].Model, Config.Model);
	FFixtureTransportModelClient CancelledClient({First, Success});
	FModelCallbacks CancelCallbacks;
	CancelCallbacks.OnAttemptRetrying = [&](int32, const FModelCallOutcome&) { CancelledClient.Cancel(); };
	const FModelCallOutcome Cancelled = CancelledClient.CallWithRetry(Request, CancelCallbacks);
	TestEqual(TEXT("cancel prevents negotiated retry"), CancelledClient.AttemptsUsed(), 1);
	TestEqual(TEXT("cancel preserves failed attempt count"), Cancelled.AttemptCount, 1);
	TestEqual(TEXT("cancel preserves last attempted cap"), Cancelled.EffectiveMaxTokens, 16000);
	TestEqual(TEXT("cancel reports live stop"), Cancelled.ErrorMessage, FString(TEXT("Aborted")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTurnRequest_EnvironmentSnapshot,
	"KismetAI.TurnRequest.EnvironmentSnapshot", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FTurnRequest_EnvironmentSnapshot::RunTest(const FString& Parameters)
{
	const FString PreviousUrl = FPlatformMisc::GetEnvironmentVariable(TEXT("OPENAI_BASE_URL"));
	const FString PreviousOrg = FPlatformMisc::GetEnvironmentVariable(TEXT("OPENAI_ORG_ID"));
	const FString PreviousProject = FPlatformMisc::GetEnvironmentVariable(TEXT("OPENAI_PROJECT_ID"));
	ON_SCOPE_EXIT
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("OPENAI_BASE_URL"), *PreviousUrl);
		FPlatformMisc::SetEnvironmentVar(TEXT("OPENAI_ORG_ID"), *PreviousOrg);
		FPlatformMisc::SetEnvironmentVar(TEXT("OPENAI_PROJECT_ID"), *PreviousProject);
	};
	FPlatformMisc::SetEnvironmentVar(TEXT("OPENAI_BASE_URL"), TEXT("http://127.0.0.1:18101"));
	FPlatformMisc::SetEnvironmentVar(TEXT("OPENAI_ORG_ID"), TEXT("original-org"));
	FPlatformMisc::SetEnvironmentVar(TEXT("OPENAI_PROJECT_ID"), TEXT("original-project"));
	FKismetAIAgentConfig Config;
	Config.Model = TEXT("gpt-4.1");
	Config.ProviderId = TEXT("openai");
	Config.MaxRetries = 1;
	Config.RetryBaseDelayMs = 0;
	Config.RetryMaxDelayMs = 0;
	TArray<FAgentMessage> Messages{FAgentMessage::MakeText(EAgentRole::User, TEXT("hello"))};
	TArray<TSharedPtr<FJsonValue>> Tools;
	FString Prompt;
	const FModelCallRequest Request{Messages, Tools, Prompt, Config};
	FScriptedAttempt First;
	First.HttpStatus = 503;
	First.Bytes = FAnthropicStreamBuilder().Raw(TEXT("{\"error\":{\"type\":\"server_error\",\"message\":\"retry\"}}")).Bytes();
	First.OnDelivered = []()
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("OPENAI_BASE_URL"), TEXT("http://127.0.0.1:18102"));
		FPlatformMisc::SetEnvironmentVar(TEXT("OPENAI_ORG_ID"), TEXT("changed-org"));
		FPlatformMisc::SetEnvironmentVar(TEXT("OPENAI_PROJECT_ID"), TEXT("changed-project"));
	};
	FScriptedAttempt Success;
	Success.Bytes = FAnthropicStreamBuilder().Raw(TEXT("event: response.completed\ndata: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_test\",\"status\":\"completed\",\"usage\":{\"input_tokens\":1,\"output_tokens\":1}}}\n\n")).Bytes();
	FFixtureTransportModelClient Client({First, Success});
	AddExpectedError(TEXT("LLM API returned status 503"), EAutomationExpectedErrorFlags::Contains, 1);
	const FModelCallOutcome Outcome = Client.CallWithRetry(Request, {});
	TestTrue(TEXT("OpenAI retry completed"), Outcome.Result == EModelCallResult::Success);
	if (!TestEqual(TEXT("two OpenAI attempts"), Client.CapturedEndpoints.Num(), 2)) return false;
	TestEqual(TEXT("retry pins environment endpoint"), Client.CapturedEndpoints[1], FString(TEXT("http://127.0.0.1:18101/v1/responses")));
	TestTrue(TEXT("retry pins organization"), Client.CapturedHeaders[1].Contains(TEXT("OpenAI-Organization: original-org")));
	TestTrue(TEXT("retry pins project"), Client.CapturedHeaders[1].Contains(TEXT("OpenAI-Project: original-project")));
	const FModelCallRequest NextRequest{Messages, Tools, Prompt, Config};
	FFixtureTransportModelClient NextClient({Success});
	NextClient.CallWithRetry(NextRequest, {});
	TestEqual(TEXT("new request reads changed environment"), NextClient.CapturedEndpoints[0], FString(TEXT("http://127.0.0.1:18102/v1/responses")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTurnRequest_Loopback,
	"KismetAI.TurnRequest.Loopback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FTurnRequest_Loopback::RunTest(const FString& Parameters)
{
	FString Url = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMETAI_TEST_TURN_PROVIDER_URL"));
	if (Url.IsEmpty())
	{
		AddInfo(TEXT("Loopback probe not selected; set KISMETAI_TEST_TURN_PROVIDER_URL to run actual curl."));
		return true;
	}
	Url.RemoveFromEnd(TEXT("/"));
	const FString Prefix = TEXT("http://127.0.0.1:");
	const FString Port = Url.Mid(Prefix.Len());
	if (!Url.StartsWith(Prefix) || !Port.IsNumeric() || FCString::Atoi(*Port) < 1 || FCString::Atoi(*Port) > 65535)
	{
		AddError(TEXT("Turn fixture requires an explicit loopback port"));
		return false;
	}
	FKismetAIAgentConfig Config;
	Config.Model = TEXT("claude-sonnet-4-6");
	Config.ApiBaseUrl = Url;
	Config.ApiKey = TEXT("unused-loopback-test-key");
	Config.MaxTokens = 16000;
	Config.MaxRetries = 1;
	Config.RetryBaseDelayMs = 0;
	Config.RetryMaxDelayMs = 0;
	Config.ThinkingMode = EThinkingMode::Disabled;
	TArray<FAgentMessage> Messages{FAgentMessage::MakeText(EAgentRole::User, TEXT("original-loopback"))};
	TArray<TSharedPtr<FJsonValue>> Tools;
	FString Prompt = TEXT("original-loopback-prompt");
	const FModelCallRequest Request{Messages, Tools, Prompt, Config};
	FKismetAICurlModelClient Client(Config);
	FModelCallbacks Callbacks;
	Callbacks.OnAttemptRetrying = [&](int32, const FModelCallOutcome&)
	{
		Config.Model = TEXT("claude-haiku-4-5-20251001");
		Messages[0].Parts[0].Text = TEXT("changed-loopback");
		Prompt = TEXT("changed-loopback-prompt");
	};
	AddExpectedError(TEXT("LLM API returned status 400"), EAutomationExpectedErrorFlags::Contains, 1);
	const FModelCallOutcome Outcome = Client.CallWithRetry(Request, Callbacks);
	TestTrue(TEXT("actual curl retry passes server-side immutable body assertions"), Outcome.Result == EModelCallResult::Success);
	TestEqual(TEXT("actual curl makes two attempts"), Outcome.RequestCount, 2);
	TestEqual(TEXT("actual curl reports effective cap"), Outcome.EffectiveMaxTokens, 9000);
	TestEqual(TEXT("actual curl reports attempts"), Outcome.AttemptCount, 2);
	TestEqual(TEXT("actual curl does not mutate persistent limit"), Config.MaxTokens, 16000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTurnRequest_OverlappingAgents,
	"KismetAI.TurnRequest.OverlappingAgents", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FTurnRequest_OverlappingAgents::RunTest(const FString& Parameters)
{
	using namespace KismetAITestFakes;
	FKismetAIAgentConfig Config;
	Config.ApiKey = TEXT("unused-fixture-key");
	Config.Model = TEXT("claude-sonnet-4-6");
	Config.MaxTokens = 16000;
	Config.MaxSteps = 2;
	Config.MaxRetries = 1;
	Config.RetryBaseDelayMs = 0;
	Config.RetryMaxDelayMs = 0;
	Config.ThinkingMode = EThinkingMode::Disabled;
	Config.bAdaptiveMaxTokens = false;
	FScriptedAttempt Success;
	Success.Bytes = FAnthropicStreamBuilder().MessageStart().BlockStart(TEXT("text")).Text(TEXT("done")).BlockStop().MessageEnd(TEXT("end_turn")).Bytes();
	FFixtureTransportModelClient ModelB({Success});
	FScriptedToolInvoker ToolsB;
	FEventRecorder EventsB;
	FKismetAIAgent AgentB(ToolsB, ModelB, Config);
	FString ResultB;
	FScriptedAttempt First;
	First.HttpStatus = 400;
	First.Bytes = FAnthropicStreamBuilder().Raw(TEXT("{\"error\":{\"type\":\"invalid_request_error\",\"message\":\"input length and `max_tokens` exceed context limit: 190000 + 16000 > 200000\"}}")).Bytes();
	First.OnDelivered = [&]()
	{
		// B runs while A's first request is still awaiting classification.
		ResultB = AgentB.RunTask(TEXT("Answer for session B"), EventsB.MakeCallback());
	};
	FFixtureTransportModelClient ModelA({First, Success, Success});
	FScriptedToolInvoker ToolsA;
	FEventRecorder EventsA;
	FKismetAIAgent AgentA(ToolsA, ModelA, Config);
	AddExpectedError(TEXT("LLM API returned status 400"), EAutomationExpectedErrorFlags::Contains, 1);
	const FString ResultA = AgentA.RunTask(TEXT("Answer for session A"), EventsA.MakeCallback());
	TestEqual(TEXT("A completes negotiated request"), ResultA, FString(TEXT("done")));
	TestEqual(TEXT("overlapping B completes"), ResultB, FString(TEXT("done")));
	AgentA.RunTask(TEXT("Next user turn in session A"), EventsA.MakeCallback());
	if (!TestEqual(TEXT("A attempts across both user turns"), ModelA.CapturedConfigs.Num(), 3)) return false;
	if (!TestEqual(TEXT("B independently sends one request"), ModelB.CapturedConfigs.Num(), 1)) return false;
	TestEqual(TEXT("A starts at configured cap"), ModelA.CapturedConfigs[0].MaxTokens, 16000);
	TestEqual(TEXT("A retries at negotiated cap"), ModelA.CapturedConfigs[1].MaxTokens, 9000);
	TestEqual(TEXT("B keeps its configured cap while A is pending"), ModelB.CapturedConfigs[0].MaxTokens, 16000);
	TestEqual(TEXT("same A agent next user turn resets to configured cap"), ModelA.CapturedConfigs[2].MaxTokens, 16000);
	TestNotEqual(TEXT("sessions retain separate identities"), AgentA.GetSessionId(), AgentB.GetSessionId());
	return true;
}
