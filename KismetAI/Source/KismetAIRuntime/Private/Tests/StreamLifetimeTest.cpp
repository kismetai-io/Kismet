// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIStreamEvents.h"
#include "Misc/AutomationTest.h"
#include "Fakes/FixtureTransport.h"
#include "Fakes/ScriptedAgentSeams.h"
#include "KismetAISessionStorage.h"
#include "KismetAIMutationOutcome.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Async/Async.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include <atomic>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamCallbacksClosedTest, "KismetAI.StreamLifetime.ClosedCallbacks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FStreamCallbacksClosedTest::RunTest(const FString& Parameters)
{
	int32 Events = 0;
	int32 JournalWrites = 0;
	FOnAgentStep Callback;
	Callback.BindLambda([&Events](const FString&, const FString&) { ++Events; });
	FKismetAIModelRequestStream Stream(Callback,
		[&JournalWrites](const TSharedPtr<FJsonObject>&) { ++JournalWrites; return true; },
		TEXT("run"), TEXT("turn"), 0);
	const FModelCallbacks Retained = Stream.MakeCallbacks();
	Stream.OpenAttempt(0);
	Retained.OnTextDelta(TEXT("before close"));
	FModelCallOutcome Outcome;
	Stream.CloseFinal(KismetAIStreamEvents::EAttemptStatus::Aborted, Outcome);
	const int32 ClosedEvents = Events;
	const int32 ClosedWrites = JournalWrites;
	Retained.OnTextDelta(TEXT("late text"));
	Retained.OnThinkingDelta(TEXT("late thinking"));
	Retained.OnThinkingComplete(TEXT("late block"));
	Retained.OnAttemptStart(1);
	Retained.OnAttemptRetrying(1, Outcome);
	TestEqual(TEXT("Closed request emits no late events"), Events, ClosedEvents);
	TestEqual(TEXT("Closed request writes no late journal records"), JournalWrites, ClosedWrites);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamCallbacksDestroyedTest, "KismetAI.StreamLifetime.DestroyedCallbacks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FStreamCallbacksDestroyedTest::RunTest(const FString& Parameters)
{
	int32 Events = 0;
	int32 JournalWrites = 0;
	FOnAgentStep Callback;
	Callback.BindLambda([&Events](const FString&, const FString&) { ++Events; });
	FModelCallbacks Retained;
	{
		FKismetAIModelRequestStream Stream(Callback,
			[&JournalWrites](const TSharedPtr<FJsonObject>&) { ++JournalWrites; return true; },
			TEXT("run"), TEXT("turn"), 0);
		Retained = Stream.MakeCallbacks();
		Stream.OpenAttempt(0);
	}
	const int32 DestroyedEvents = Events;
	FModelCallOutcome Outcome;
	Retained.OnTextDelta(TEXT("late text"));
	Retained.OnThinkingDelta(TEXT("late thinking"));
	Retained.OnThinkingComplete(TEXT("late block"));
	Retained.OnAttemptStart(1);
	Retained.OnAttemptRetrying(1, Outcome);
	TestEqual(TEXT("Destroyed request emits no late events"), Events, DestroyedEvents);
	TestEqual(TEXT("Destroyed request writes no journal records"), JournalWrites, 0);
	return true;
}

namespace
{
	class FRetainingTransportClient final : public IKismetAIModelClient
	{
	public:
		explicit FRetainingTransportClient(TArray<KismetAIFixtureTransport::FScriptedAttempt> Attempts)
			: Inner(MoveTemp(Attempts)) {}
		FModelCallbacks Retained;
		KismetAIFixtureTransport::FFixtureTransportModelClient Inner;
		virtual FModelCallOutcome Call(const FModelCallRequest& Request, const FModelCallbacks& Callbacks) override
		{
			Retained = Callbacks;
			return Inner.Call(Request, Callbacks);
		}
		virtual FModelCallOutcome CallWithRetry(const FModelCallRequest& Request, const FModelCallbacks& Callbacks) override
		{
			Retained = Callbacks;
			return Inner.CallWithRetry(Request, Callbacks);
		}
		virtual void Cancel() override { Inner.Cancel(); }
		virtual FString GetProviderName() const override { return Inner.GetProviderName(); }
		virtual int32 CacheTtlSeconds() const override { return Inner.CacheTtlSeconds(); }
	};

	FKismetAIAgentConfig StreamLifetimeConfig()
	{
		FKismetAIAgentConfig Config;
		Config.ApiKey = TEXT("unused-local-fixture-key");
		Config.Model = TEXT("claude-sonnet-4-6");
		Config.MaxSteps = 1;
		Config.MaxRetries = 0;
		Config.ThinkingMode = EThinkingMode::Disabled;
		return Config;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamCallbacksRunTaskTest, "KismetAI.StreamLifetime.RunTaskRetainedCallbacks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FStreamCallbacksRunTaskTest::RunTest(const FString& Parameters)
{
	using namespace KismetAITestFakes;
	using namespace KismetAIFixtureTransport;
	FAnthropicStreamBuilder Wire;
	Wire.MessageStart().BlockStart(TEXT("text")).Text(TEXT("The answer is 42.")).BlockStop().MessageEnd(TEXT("end_turn"));
	FScriptedAttempt Attempt;
	Attempt.Bytes = Wire.Bytes();
	FRetainingTransportClient Client({ Attempt });
	FScriptedToolInvoker Invoker;
	FEventRecorder Events;
	FString SessionId;
	{
		FKismetAIAgent Agent(Invoker, Client, StreamLifetimeConfig());
		TestEqual(TEXT("Real loop accepted production decoder response"),
			Agent.RunTask(TEXT("What is the answer?"), Events.MakeCallback()), FString(TEXT("The answer is 42.")));
		SessionId = Agent.GetSessionId();
	}
	FString Before;
	TestTrue(TEXT("Journal exists"), FFileHelper::LoadFileToString(Before, *FKismetAISessionStorage::GetSessionFilePath(SessionId)));
	const int32 EventCount = Events.Events.Num();
	Client.Retained.OnTextDelta(TEXT("late text"));
	Client.Retained.OnThinkingDelta(TEXT("late thinking"));
	Client.Retained.OnThinkingComplete(TEXT("late block"));
	Client.Retained.OnAttemptStart(9);
	Client.Retained.OnAttemptRetrying(9, FModelCallOutcome());
	FString After;
	TestTrue(TEXT("Journal still readable"), FFileHelper::LoadFileToString(After, *FKismetAISessionStorage::GetSessionFilePath(SessionId)));
	TestEqual(TEXT("Late callbacks cannot append journal records"), After, Before);
	TestEqual(TEXT("Late callbacks cannot emit events"), Events.Events.Num(), EventCount);
	TestEqual(TEXT("Exactly one terminal result"), Events.Count(TEXT("done")) + Events.Count(TEXT("error")), 1);
	TestEqual(TEXT("No tool execution"), Events.Count(TEXT("tool_call")), 0);
	FKismetAISessionStorage::DeleteSession(SessionId);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamLifecycleLocalCurlTest, "KismetAI.StreamLifetime.LocalCurlResumeSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FStreamLifecycleLocalCurlTest::RunTest(const FString& Parameters)
{
	using namespace KismetAITestFakes;
	const FString Url = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMETAI_TEST_LIFECYCLE_PROVIDER_URL"));
	if (Url.IsEmpty())
	{
		AddInfo(TEXT("Local curl smoke NOT RUN: requires KISMETAI_TEST_LIFECYCLE_PROVIDER_URL and the two-response local fixture server."));
		return true;
	}
	const FString Prefix = TEXT("http://127.0.0.1:");
	const FString PortText = Url.Mid(Prefix.Len());
	bool bDigits = !PortText.IsEmpty() && PortText.Len() <= 5;
	for (TCHAR C : PortText) { bDigits &= C >= TEXT('0') && C <= TEXT('9'); }
	if (!TestTrue(TEXT("Smoke endpoint is strictly local with a valid port"),
		Url.StartsWith(Prefix) && bDigits && FCString::Atoi(*PortText) > 0 && FCString::Atoi(*PortText) <= 65535)) return false;

	FKismetAIAgentConfig Config = StreamLifetimeConfig();
	Config.ProviderId = TEXT("anthropic");
	Config.bProviderEnvironmentResolved = true;
	Config.ProviderBaseUrls.Add(TEXT("anthropic"), Url);
	Config.ProviderKeys.Add(TEXT("anthropic"), TEXT("unused-local-fixture-key"));
	FKismetAIMutationOutcome Outcome;
	Outcome.bAppliedInMemory = true;
	Outcome.CompileStatus = EKismetAICompileStatus::Succeeded;
	Outcome.SaveStatus = EKismetAISaveStatus::Saved;
	Outcome.PackageName = TEXT("/Game/LifecycleFixture");
	auto Data = MakeShared<FJsonObject>();
	Data->SetObjectField(TEXT("outcome"), Outcome.ToJson());
	auto Envelope = MakeShared<FJsonObject>();
	Envelope->SetBoolField(TEXT("success"), true);
	Envelope->SetObjectField(TEXT("data"), Data);
	FScriptedToolInvoker Invoker;
	Invoker.SetResult(TEXT("edit_blueprint"), KismetAIFixtureTransport::Condensed(Envelope));
	FEventRecorder First;
	FString SessionId;
	{
		FKismetAICurlModelClient Client(Config);
		FKismetAIAgent Agent(Invoker, Client, Config);
		FOnAgentStep Callback;
		Callback.BindLambda([&](const FString& Type, const FString& Json)
		{
			First.Events.Emplace(Type, Json);
			if (Type == TEXT("tool_result")) { Agent.Abort(); }
		});
		Agent.RunTask(TEXT("Apply the local fixture edit."), Callback);
		SessionId = Agent.GetSessionId();
	}
	const FString* FirstDone = First.FirstJsonOfType(TEXT("done"));
	TestEqual(TEXT("One mutation executed"), Invoker.CountCalls(TEXT("edit_blueprint")), 1);
	TestEqual(TEXT("One partial terminal"), First.Count(TEXT("done")) + First.Count(TEXT("error")), 1);
	TestTrue(TEXT("Partial run aborted"), FirstDone && FirstDone->Contains(TEXT("aborted")));
	TestEqual(TEXT("Committed mutation evidence emitted"), First.Count(TEXT("mutation_outcomes")), 1);
	FEventRecorder Resumed;
	{
		FKismetAICurlModelClient FreshClient(Config);
		FKismetAIAgent FreshAgent(Invoker, FreshClient, Config);
		FreshAgent.RunTask(TEXT("Report the recorded result."), Resumed.MakeCallback(), FString(), SessionId);
		TestEqual(TEXT("Resume preserves session"), FreshAgent.GetSessionId(), SessionId);
	}
	const FString* Done = Resumed.FirstJsonOfType(TEXT("done"));
	TestEqual(TEXT("One resumed terminal"), Resumed.Count(TEXT("done")) + Resumed.Count(TEXT("error")), 1);
	TestTrue(TEXT("Fresh production client resumes to completion"), Done && Done->Contains(TEXT("completed")));
	TestEqual(TEXT("Resume does not repeat mutation"), Invoker.CountCalls(TEXT("edit_blueprint")), 1);
	FKismetAISessionStorage::DeleteSession(SessionId);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamCallbackTeardownRaceTest, "KismetAI.StreamLifetime.ConcurrentTeardownDrainsCallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FStreamCallbackTeardownRaceTest::RunTest(const FString& Parameters)
{
	FEvent* Entered = FPlatformProcess::GetSynchEventFromPool(true);
	FEvent* Release = FPlatformProcess::GetSynchEventFromPool(true);
	FEvent* Closing = FPlatformProcess::GetSynchEventFromPool(true);
	FEvent* Closed = FPlatformProcess::GetSynchEventFromPool(true);
	std::atomic<int32> Events{0};
	FOnAgentStep Callback;
	Callback.BindLambda([&](const FString& Type, const FString&)
	{
		++Events;
		if (Type == TEXT("text_delta"))
		{
			Entered->Trigger();
			Release->Wait();
		}
	});
	auto* Stream = new FKismetAIModelRequestStream(Callback,
		[](const TSharedPtr<FJsonObject>&) { return true; }, TEXT("run"), TEXT("turn"), 0);
	const FModelCallbacks Retained = Stream->MakeCallbacks();
	Stream->OpenAttempt(0);
	auto Delivery = Async(EAsyncExecution::Thread, [&]() { Retained.OnTextDelta(TEXT("in flight")); });
	const bool bEntered = Entered->Wait(5000);
	TestTrue(TEXT("Callback entered the controlled barrier"), bEntered);
	auto Teardown = Async(EAsyncExecution::Thread, [&]()
	{
		Closing->Trigger();
		delete Stream;
		Closed->Trigger();
	});
	TestTrue(TEXT("Concurrent teardown started"), Closing->Wait(5000));
	TestFalse(TEXT("Teardown cannot finish while callback is active"), Closed->Wait(0));
	Release->Trigger();
	Delivery.Wait();
	Teardown.Wait();
	TestTrue(TEXT("Teardown finishes after callback drains"), Closed->Wait(0));
	const int32 ClosedCount = Events.load();
	Retained.OnTextDelta(TEXT("late"));
	Retained.OnAttemptStart(2);
	TestEqual(TEXT("No callbacks after concurrent teardown"), Events.load(), ClosedCount);
	FPlatformProcess::ReturnSynchEventToPool(Entered);
	FPlatformProcess::ReturnSynchEventToPool(Release);
	FPlatformProcess::ReturnSynchEventToPool(Closing);
	FPlatformProcess::ReturnSynchEventToPool(Closed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamCallbackAbortTest, "KismetAI.StreamLifetime.RunTaskCallbackAbort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FStreamCallbackAbortTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("LLM API returned status 200"), EAutomationExpectedErrorFlags::Contains, 1);
	using namespace KismetAITestFakes;
	using namespace KismetAIFixtureTransport;
	FAnthropicStreamBuilder Wire;
	Wire.MessageStart().BlockStart(TEXT("text")).Text(TEXT("A provisional answer.")).BlockStop().MessageEnd(TEXT("end_turn"));
	FScriptedAttempt Attempt;
	Attempt.Bytes = Wire.Bytes();
	FRetainingTransportClient Client({ Attempt });
	FScriptedToolInvoker Invoker;
	FEventRecorder Events;
	FKismetAIAgent Agent(Invoker, Client, StreamLifetimeConfig());
	FOnAgentStep Callback;
	Callback.BindLambda([&](const FString& Type, const FString& Json)
	{
		Events.Events.Emplace(Type, Json);
		if (Type == TEXT("text_delta")) { Agent.Abort(); }
	});
	Agent.RunTask(TEXT("Answer the question."), Callback);
	TestEqual(TEXT("Reentrant Abort produces one terminal"), Events.Count(TEXT("done")) + Events.Count(TEXT("error")), 1);
	const FString* Done = Events.FirstJsonOfType(TEXT("done"));
	TestTrue(TEXT("Reentrant Abort remains cancelled"), Done && Done->Contains(TEXT("aborted")));
	TestEqual(TEXT("No tools after cancellation"), Events.Count(TEXT("tool_call")), 0);
	FKismetAISessionStorage::DeleteSession(Agent.GetSessionId());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamReentrantCloseTest, "KismetAI.StreamLifetime.ReentrantCloseStopsCurrentCallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FStreamReentrantCloseTest::RunTest(const FString& Parameters)
{
	KismetAITestFakes::FEventRecorder Events;
	FKismetAIModelRequestStream* Active = nullptr;
	FOnAgentStep Callback;
	Callback.BindLambda([&](const FString& Type, const FString& Json)
	{
		Events.Events.Emplace(Type, Json);
		if (Type == TEXT("text_start") || Type == TEXT("attempt_end"))
		{
			Active->CloseFinal(KismetAIStreamEvents::EAttemptStatus::Aborted, FModelCallOutcome());
		}
	});
	FKismetAIModelRequestStream Stream(Callback, [](const TSharedPtr<FJsonObject>&) { return true; },
		TEXT("run"), TEXT("turn"), 0);
	Active = &Stream;
	Stream.OpenAttempt(0);
	Stream.MakeCallbacks().OnTextDelta(TEXT("must not arrive after close"));
	TestEqual(TEXT("Reentrant close ends attempt exactly once"), Events.Count(TEXT("attempt_end")), 1);
	TestEqual(TEXT("Current callback stops after reentrant close"), Events.Count(TEXT("text_delta")), 0);
	return true;
}
