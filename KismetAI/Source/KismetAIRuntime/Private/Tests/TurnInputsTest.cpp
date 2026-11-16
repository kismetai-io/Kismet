// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Fakes/ScriptedAgentSeams.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformMisc.h"

namespace KismetAITurnInputsTest
{
	using namespace KismetAITestFakes;
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FKismetAIAgentConfig Config()
	{
		FKismetAIAgentConfig C;
		C.Model = TEXT("claude-sonnet-4-6");
		C.ThinkingMode = EThinkingMode::Disabled;
		C.MaxSteps = 4;
		C.MaxRetries = 0;
		C.MaxTokens = 4096;
		return C;
	}

	TSharedPtr<FJsonObject> SearchArgs()
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("query"), TEXT("turn-input-fixture"));
		return Args;
	}

	class FCallerRecorder final : public IKismetAIToolInvoker
	{
	public:
		FScriptedToolInvoker Inner;
		TArray<FKismetAICallerContext> Callers;
		virtual FKismetAIInvokeResult Invoke(const FKismetAIToolInvocation& Invocation) override
		{
			Callers.Add(Invocation.Caller);
			return Inner.Invoke(Invocation);
		}
		virtual TSet<FString> GetDispatchableToolNames() const override { return Inner.GetDispatchableToolNames(); }
		virtual FString ResolveHandlerName(const FString& Name, const FString& Mode) const override { return Inner.ResolveHandlerName(Name, Mode); }
	};

	class FPendingModel final : public IKismetAIModelClient
	{
	public:
		FScriptedModelClient Inner;
		TFunction<void()> DuringFirstCall;
		explicit FPendingModel(TArray<FModelCallOutcome> Script) : Inner(MoveTemp(Script)) {}
		virtual FModelCallOutcome Call(const FModelCallRequest& Request, const FModelCallbacks& Callbacks) override
		{
			if (Inner.NumCalls() == 0 && DuringFirstCall) { DuringFirstCall(); }
			return Inner.Call(Request, Callbacks);
		}
		virtual FModelCallOutcome CallWithRetry(const FModelCallRequest& Request, const FModelCallbacks& Callbacks) override { return Call(Request, Callbacks); }
		virtual void Cancel() override { Inner.Cancel(); }
		virtual FString GetProviderName() const override { return Inner.GetProviderName(); }
		virtual int32 CacheTtlSeconds() const override { return Inner.CacheTtlSeconds(); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTurnInputs_TruncationDoesNotLeak,
	"KismetAI.TurnInputs.TruncationBudgetDoesNotLeakToNextRun", KismetAITurnInputsTest::Flags)
bool FTurnInputs_TruncationDoesNotLeak::RunTest(const FString& Parameters)
{
	using namespace KismetAITurnInputsTest;
	FKismetAIAgentConfig C = Config();
	C.bAdaptiveMaxTokens = false;
	FScriptedModelClient Model({ MakeTextTurn(TEXT("partial"), TEXT("max_tokens")), MakeTextTurn(TEXT("done")), MakeTextTurn(TEXT("next")) });
	FScriptedToolInvoker Invoker;
	FKismetAIAgent Agent(Invoker, Model, C);
	Agent.RunTask(TEXT("first"));
	const int32 NextRunRequest = Model.NumCalls();
	Agent.RunTask(TEXT("second"));
	if (!TestEqual(TEXT("first run continued once after truncation"), NextRunRequest, 2)
		|| !TestEqual(TEXT("second run issued one request"), Model.NumCalls(), 3)) { return false; }
	TestEqual(TEXT("continuation has larger budget"), Model.GetRequests()[1].MaxTokens, 8192);
	TestEqual(TEXT("next run starts at configured budget"), Model.GetRequests()[NextRunRequest].MaxTokens, C.MaxTokens);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTurnInputs_AdaptiveDoesNotLeak,
	"KismetAI.TurnInputs.AdaptiveBudgetDoesNotLeakToNextRun", KismetAITurnInputsTest::Flags)
bool FTurnInputs_AdaptiveDoesNotLeak::RunTest(const FString& Parameters)
{
	using namespace KismetAITurnInputsTest;
	FKismetAIAgentConfig C = Config();
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), TEXT("/Game/TurnInputFixture"));
	FScriptedModelClient Model({ MakeToolTurn(TEXT("read"), TEXT("read_blueprint"), Args), MakeTextTurn(TEXT("done")), MakeTextTurn(TEXT("next")) });
	FScriptedToolInvoker Invoker;
	FKismetAIAgent Agent(Invoker, Model, C);
	Agent.RunTask(TEXT("first"));
	const int32 NextRunRequest = Model.NumCalls();
	Agent.RunTask(TEXT("second"));
	if (!TestEqual(TEXT("first run executed a tool roundtrip"), NextRunRequest, 2)
		|| !TestEqual(TEXT("second run issued one request"), Model.NumCalls(), 3)) { return false; }
	TestEqual(TEXT("read-followup uses adaptive high budget"), Model.GetRequests()[1].MaxTokens, C.MaxTokensHigh);
	TestEqual(TEXT("next run restores configured budget"), Model.GetRequests()[NextRunRequest].MaxTokens, C.MaxTokens);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTurnInputs_CallerIsFrozen,
	"KismetAI.TurnInputs.PendingCallKeepsCallerUntilNextRun", KismetAITurnInputsTest::Flags)
bool FTurnInputs_CallerIsFrozen::RunTest(const FString& Parameters)
{
	using namespace KismetAITurnInputsTest;
	FPendingModel Model({ MakeToolTurn(TEXT("search1"), TEXT("search_assets"), SearchArgs()), MakeTextTurn(TEXT("done")),
		MakeToolTurn(TEXT("search2"), TEXT("search_assets"), SearchArgs()), MakeTextTurn(TEXT("done")) });
	FCallerRecorder Invoker;
	FKismetAIAgent Agent(Invoker, Model, Config());
	Agent.SetCallerContext(FKismetAICallerContext::Verified(EKismetAIEntryPoint::HttpAgentRun, TEXT("original")));
	// A settings update lands after the request entered the model client and
	// before it returns a tool call. No sleeps or scheduler race is needed.
	Model.DuringFirstCall = [&Agent]()
	{
		Agent.SetCallerContext(FKismetAICallerContext::Verified(EKismetAIEntryPoint::Panel, TEXT("next-run")));
	};
	Agent.RunTask(TEXT("first"));
	Agent.RunTask(TEXT("second"));
	if (!TestEqual(TEXT("one real executor invocation in each run"), Invoker.Callers.Num(), 2)) { return false; }
	TestEqual(TEXT("pending response retains original principal"), Invoker.Callers[0].Principal, FString(TEXT("original")));
	TestTrue(TEXT("pending response retains authenticated entry point"), Invoker.Callers[0].EntryPoint == EKismetAIEntryPoint::HttpAgentRun);
	TestEqual(TEXT("next run captures updated principal"), Invoker.Callers[1].Principal, FString(TEXT("next-run")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTurnInputs_AdaptiveEnvironmentIsFrozen,
	"KismetAI.TurnInputs.AdaptiveEnvironmentChangesAtNextRun", KismetAITurnInputsTest::Flags)
bool FTurnInputs_AdaptiveEnvironmentIsFrozen::RunTest(const FString& Parameters)
{
	using namespace KismetAITurnInputsTest;
	const FString Previous = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMETAI_MAX_OUTPUT_TOKENS"));
	ON_SCOPE_EXIT { FPlatformMisc::SetEnvironmentVar(TEXT("KISMETAI_MAX_OUTPUT_TOKENS"), *Previous); };
	FPlatformMisc::SetEnvironmentVar(TEXT("KISMETAI_MAX_OUTPUT_TOKENS"), TEXT("6000"));
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), TEXT("/Game/TurnInputFixture"));
	FPendingModel Model({ MakeToolTurn(TEXT("read1"), TEXT("read_blueprint"), Args), MakeTextTurn(TEXT("done")),
		MakeToolTurn(TEXT("read2"), TEXT("read_blueprint"), Args), MakeTextTurn(TEXT("done")) });
	FScriptedToolInvoker Invoker;
	FKismetAIAgent Agent(Invoker, Model, Config());
	Model.DuringFirstCall = []()
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("KISMETAI_MAX_OUTPUT_TOKENS"), TEXT("7000"));
	};
	Agent.RunTask(TEXT("first"));
	const int32 NextRunRequest = Model.Inner.NumCalls();
	Agent.RunTask(TEXT("second"));
	if (!TestEqual(TEXT("first run made a tool roundtrip"), NextRunRequest, 2)
		|| !TestEqual(TEXT("each run made two requests"), Model.Inner.NumCalls(), 4)) { return false; }
	TestEqual(TEXT("pending run retains captured adaptive override"), Model.Inner.GetRequests()[1].MaxTokens, 6000);
	TestEqual(TEXT("next run captures changed adaptive override"), Model.Inner.GetRequests()[3].MaxTokens, 7000);
	return true;
}
