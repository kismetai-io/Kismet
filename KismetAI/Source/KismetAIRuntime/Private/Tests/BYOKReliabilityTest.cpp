// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Fakes/ScriptedAgentSeams.h"
#include "Fixtures/SSEFixtures.h"
#include "KismetAILLMProvider.h"
#include "KismetAISSEParser.h"
#include "Misc/AutomationTest.h"

using namespace KismetAITestFakes;
constexpr auto BYOKFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	FKismetAIAgentConfig TestConfig(const TCHAR* Model = TEXT("claude-sonnet-4-6"))
	{
		FKismetAIAgentConfig Config;
		Config.Model = Model;
		Config.ThinkingMode = EThinkingMode::Disabled;
		Config.MaxSteps = 3;
		Config.MaxRetries = 0;
		return Config;
	}

	class FProviderScript final : public IKismetAIModelClient
	{
	public:
		FProviderScript(const FModelCallOutcome& Outcome, const TCHAR* Name)
			: Script({Outcome}), ProviderName(Name) {}
		virtual FModelCallOutcome Call(const FModelCallRequest& Request, const FModelCallbacks& Callbacks) override
		{ return Script.Call(Request, Callbacks); }
		virtual FModelCallOutcome CallWithRetry(const FModelCallRequest& Request, const FModelCallbacks& Callbacks) override
		{ return Script.CallWithRetry(Request, Callbacks); }
		virtual void Cancel() override { Script.Cancel(); }
		virtual FString GetProviderName() const override { return ProviderName; }
		virtual int32 CacheTtlSeconds() const override { return 300; }
	private:
		FScriptedModelClient Script;
		FString ProviderName;
	};

	TSharedPtr<FJsonObject> Parse(const TCHAR* Json)
	{
		TSharedPtr<FJsonObject> Result;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Result);
		return Result;
	}

	void Feed(ILLMStreamState& State, const ANSICHAR* Text)
	{
		FKismetAISSEParser Parser;
		Parser.Feed(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
		for (const auto& Event : Parser.DequeueEvents())
		{
			FString Delta;
			if (Event.Data.IsValid()) State.ProcessEvent(Event.EventType, Event.Data, Delta);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBYOK_EmptyTurn, "KismetAI.BYOK.EmptyTurnIsAnError", BYOKFlags)
bool FBYOK_EmptyTurn::RunTest(const FString& Parameters)
{
	FScriptedModelClient Model({MakeTextTurn(TEXT(" \n\t"))});
	FScriptedToolInvoker Tools;
	FEventRecorder Events;
	FKismetAIAgent Agent(Tools, Model, TestConfig());
	Agent.RunTask(TEXT("Answer the question"), Events.MakeCallback());
	TestEqual(TEXT("empty completion emits an actionable error"), Events.Count(TEXT("error")), 1);
	TestEqual(TEXT("empty completion is not success"), Events.Count(TEXT("done")), 0);
	TestEqual(TEXT("no invented tool calls"), Tools.CallLog.Num(), 0);
	TestEqual(TEXT("reported usage is still retained"), Agent.GetTokenStats().TotalInputTokens, 100);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBYOK_MultipleTextBlocks, "KismetAI.BYOK.MultipleTextBlocksSurvive", BYOKFlags)
bool FBYOK_MultipleTextBlocks::RunTest(const FString& Parameters)
{
	FScriptedModelClient Model({MakeResponse({MakeTextBlock(TEXT("First. ")), MakeTextBlock(TEXT("Second."))}, TEXT("end_turn"))});
	FScriptedToolInvoker Tools;
	FEventRecorder Events;
	FKismetAIAgent Agent(Tools, Model, TestConfig());
	TestEqual(TEXT("all visible blocks are returned"), Agent.RunTask(TEXT("Summarize"), Events.MakeCallback()), FString(TEXT("First. Second.")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBYOK_CachedCost, "KismetAI.BYOK.OpenAICachedInputPricedOnce", BYOKFlags)
bool FBYOK_CachedCost::RunTest(const FString& Parameters)
{
	FModelCallOutcome Outcome = MakeResponse({MakeTextBlock(TEXT("Answer."))}, TEXT("end_turn"), 1000, 100);
	Outcome.Response->GetObjectField(TEXT("usage"))->SetNumberField(TEXT("cache_read_input_tokens"), 800);
	FProviderScript Model(Outcome, TEXT("openai"));
	FScriptedToolInvoker Tools;
	FEventRecorder Events;
	FKismetAIAgent Agent(Tools, Model, TestConfig(TEXT("gpt-5-mini")));
	Agent.RunTask(TEXT("Answer"), Events.MakeCallback());
	const FModelCost Rate = FKismetAIAgent::GetModelCost(TEXT("gpt-5-mini"));
	const double Expected = (200 * Rate.InputPerMillion + 800 * Rate.CacheReadPerMillion + 100 * Rate.OutputPerMillion) / 1000000.0;
	TestTrue(TEXT("cached inputs are not charged at both rates"), FMath::IsNearlyEqual(Agent.GetTokenStats().EstimatedCostUSD, Expected, 0.000000001));
	TestEqual(TEXT("provider input counter is preserved"), Agent.GetTokenStats().TotalInputTokens, 1000);
	TestEqual(TEXT("cache counter is preserved"), Agent.GetTokenStats().TotalCacheReadTokens, 800);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBYOK_FailedUsage, "KismetAI.BYOK.FailedResponseRetainsReportedUsage", BYOKFlags)
bool FBYOK_FailedUsage::RunTest(const FString& Parameters)
{
	FModelCallOutcome Outcome = MakeResponse({}, TEXT(""), 300, 17);
	Outcome.Result = EModelCallResult::FatalError;
	Outcome.ErrorType = TEXT("stream_error");
	Outcome.ErrorMessage = TEXT("The provider stopped responding");
	FScriptedModelClient Model({Outcome});
	FScriptedToolInvoker Tools;
	FEventRecorder Events;
	FKismetAIAgent Agent(Tools, Model, TestConfig());
	Agent.RunTask(TEXT("Answer"), Events.MakeCallback());
	TestEqual(TEXT("failed request still reports known input"), Agent.GetTokenStats().TotalInputTokens, 300);
	TestEqual(TEXT("failed request still reports known output"), Agent.GetTokenStats().TotalOutputTokens, 17);
	TestEqual(TEXT("failure remains visible"), Events.Count(TEXT("error")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBYOK_AnthropicStreamError, "KismetAI.BYOK.AnthropicStreamError", BYOKFlags)
bool FBYOK_AnthropicStreamError::RunTest(const FString& Parameters)
{
	TUniquePtr<ILLMProvider> Provider = MakeProviderForModel(TEXT("claude-sonnet-4-6"));
	const auto State = Provider->CreateStreamState();
	FString Delta, Message, Code;
	State->ProcessEvent(TEXT("error"), Parse(TEXT("{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}")), Delta);
	TestTrue(TEXT("HTTP200 error event is a failure"), State->HasStreamError(Message, Code));
	TestEqual(TEXT("provider error retained"), Code, FString(TEXT("overloaded_error")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBYOK_StreamCompletion, "KismetAI.BYOK.StreamCompletionIsExplicit", BYOKFlags)
bool FBYOK_StreamCompletion::RunTest(const FString& Parameters)
{
	for (const TCHAR* Model : {TEXT("claude-sonnet-4-6"), TEXT("gpt-5-mini"), TEXT("MiniMax-M3")})
	{
		TUniquePtr<ILLMProvider> Provider = MakeProviderForModel(Model);
		const auto State = Provider->CreateStreamState();
		bool Complete = true;
		TestTrue(TEXT("unstarted stream declares completion state"), State->BuildResponseJson()->TryGetBoolField(TEXT("stream_complete"), Complete));
		TestFalse(TEXT("empty200 is not a complete stream"), Complete);
		TestFalse(TEXT("absent usage is not fabricated as zero"), State->BuildResponseJson()->HasField(TEXT("usage")));
		if (IsOpenAIModel(Model))
		{
			FString Delta;
			State->ProcessEvent(TEXT("response.completed"), Parse(TEXT("{\"response\":{\"usage\":{\"input_tokens\":7,\"output_tokens\":3}}}")), Delta);
		}
		else Feed(*State, KismetAISSEFixtures::TextOnly());
		Complete = false;
		TestTrue(TEXT("terminal stream declares completion state"), State->BuildResponseJson()->TryGetBoolField(TEXT("stream_complete"), Complete));
		TestTrue(TEXT("terminal event completes stream"), Complete);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBYOK_CancelledBeforeSend, "KismetAI.BYOK.CancelledBeforeSendDoesNotInventRequest", BYOKFlags)
bool FBYOK_CancelledBeforeSend::RunTest(const FString& Parameters)
{
	FKismetAICurlModelClient Model(TEXT("claude-sonnet-4-6"));
	Model.Cancel();
	FScriptedToolInvoker Tools;
	FEventRecorder Events;
	FKismetAIAgent Agent(Tools, Model, TestConfig());
	Agent.RunTask(TEXT("Never send"), Events.MakeCallback());
	TestEqual(TEXT("no provider request was made"), Agent.GetTokenStats().RequestCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBYOK_MissingUsage, "KismetAI.BYOK.UnknownUsageIsVisible", BYOKFlags)
bool FBYOK_MissingUsage::RunTest(const FString& Parameters)
{
	FModelCallOutcome Outcome;
	Outcome.Result = EModelCallResult::FatalError;
	Outcome.ErrorType = TEXT("connection_error");
	Outcome.ErrorMessage = TEXT("Timed out after sending the request");
	Outcome.bUsageIncomplete = true;
	Outcome.RequestCount = 1;
	FScriptedModelClient Model({Outcome});
	FScriptedToolInvoker Tools;
	FEventRecorder Events;
	FKismetAIAgent Agent(Tools, Model, TestConfig());
	Agent.RunTask(TEXT("Answer"), Events.MakeCallback());
	const FString* Stats = Events.FirstJsonOfType(TEXT("token_stats"));
	TestTrue(TEXT("unknown provider usage is visibly partial"), Stats && Stats->Contains(TEXT("\"usage_complete\":false")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBYOK_RetryUsage, "KismetAI.BYOK.RetryUsageAndPartialFlagSurviveRecovery", BYOKFlags)
bool FBYOK_RetryUsage::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), TEXT("/Game/Test"));
	FModelCallOutcome First = MakeToolTurn(TEXT("compile1"), TEXT("compile_blueprint"), Args);
	First.UsageReports = {MakeUsage(100, 20), MakeUsage(200, 30)};
	First.RequestCount = 2;
	First.bUsageIncomplete = true;
	FScriptedModelClient Model({First, MakeTextTurn(TEXT("Compiled."))});
	FScriptedToolInvoker Tools;
	FEventRecorder Events;
	FKismetAIAgent Agent(Tools, Model, TestConfig());
	Agent.RunTask(TEXT("Compile"), Events.MakeCallback());
	TestEqual(TEXT("both attempts and recovered next turn counted"), Agent.GetTokenStats().RequestCount, 3);
	TestEqual(TEXT("all reported inputs preserved"), Agent.GetTokenStats().TotalInputTokens, 400);
	TestEqual(TEXT("all reported outputs preserved"), Agent.GetTokenStats().TotalOutputTokens, 100);
	TestFalse(TEXT("a later complete response cannot hide earlier unknown usage"), Agent.GetTokenStats().bUsageComplete);
	TestEqual(TEXT("normal completion after recovery"), Events.Count(TEXT("done")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBYOK_CompactionUsage, "KismetAI.BYOK.CompactionUsageIsIncluded", BYOKFlags)
bool FBYOK_CompactionUsage::RunTest(const FString& Parameters)
{
	FScriptedModelClient Model({MakeTextTurn(TEXT("Answer.")), MakeTextTurn(TEXT("Summary."))});
	FScriptedToolInvoker Tools;
	FEventRecorder Events;
	FKismetAIAgent Agent(Tools, Model, TestConfig());
	Agent.RunTask(TEXT("Answer"), Events.MakeCallback());
	TArray<TSharedPtr<FJsonValue>> Messages;
	for (int32 Index = 0; Index < 16; ++Index)
	{
		auto Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("role"), Index % 2 ? TEXT("assistant") : TEXT("user"));
		Message->SetStringField(TEXT("content"), TEXT("A conversation message."));
		Messages.Add(MakeShared<FJsonValueObject>(Message));
	}
	Agent.TrimConversation(Messages);
	TestEqual(TEXT("summary actually made a model call"), Model.NumCalls(), 2);
	TestEqual(TEXT("summary input included"), Agent.GetTokenStats().TotalInputTokens, 200);
	TestEqual(TEXT("summary output included"), Agent.GetTokenStats().TotalOutputTokens, 100);
	return true;
}
