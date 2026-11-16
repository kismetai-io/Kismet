// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// The cancellation token tree.
//
// The property that matters is subtree propagation: cancelling a run must reach
// work already in flight, without the canceller having to find and poke every
// in-flight thing. Everything else here guards an edge that would let work
// escape a cancellation it was born into.

#include "KismetAICancellation.h"
#include "KismetAIToolExecutor.h"
#include "KismetAIToolInvoker.h"
#include "Misc/AutomationTest.h"
#include "Async/Async.h"

constexpr auto CancelFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCancel_ParentCancelsChildren, "KismetAI.Cancellation.ParentCancelsChildren", CancelFlags)
bool FCancel_ParentCancelsChildren::RunTest(const FString& Parameters)
{
	FKismetAICancellationToken Root;
	FKismetAICancellationToken Turn = Root.CreateChild();
	FKismetAICancellationToken Call = Turn.CreateChild();

	TestFalse(TEXT("nothing cancelled initially"),
		Root.IsCancelled() || Turn.IsCancelled() || Call.IsCancelled());

	Root.Cancel();

	TestTrue(TEXT("root cancelled"), Root.IsCancelled());
	TestTrue(TEXT("turn cancelled with it"), Turn.IsCancelled());
	TestTrue(TEXT("and the call two levels down"), Call.IsCancelled());
	return true;
}

// Cancelling one turn must not end the run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCancel_ChildDoesNotCancelParent, "KismetAI.Cancellation.ChildDoesNotCancelParent", CancelFlags)
bool FCancel_ChildDoesNotCancelParent::RunTest(const FString& Parameters)
{
	FKismetAICancellationToken Root;
	FKismetAICancellationToken A = Root.CreateChild();
	FKismetAICancellationToken B = Root.CreateChild();

	A.Cancel();

	TestTrue(TEXT("the cancelled child is cancelled"), A.IsCancelled());
	TestFalse(TEXT("the parent is not"), Root.IsCancelled());
	TestFalse(TEXT("nor is its sibling"), B.IsCancelled());
	return true;
}

// A child created after cancellation must not escape it — otherwise a turn
// starting in the gap between abort and teardown would run anyway.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCancel_ChildOfCancelledStartsCancelled, "KismetAI.Cancellation.ChildOfCancelledStartsCancelled", CancelFlags)
bool FCancel_ChildOfCancelledStartsCancelled::RunTest(const FString& Parameters)
{
	FKismetAICancellationToken Root;
	Root.Cancel();

	FKismetAICancellationToken Late = Root.CreateChild();
	TestTrue(TEXT("born cancelled"), Late.IsCancelled());

	FKismetAICancellationToken Later = Late.CreateChild();
	TestTrue(TEXT("and so is its child"), Later.IsCancelled());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCancel_IsIdempotent, "KismetAI.Cancellation.IsIdempotent", CancelFlags)
bool FCancel_IsIdempotent::RunTest(const FString& Parameters)
{
	FKismetAICancellationToken Root;
	FKismetAICancellationToken Child = Root.CreateChild();
	Root.Cancel();
	Root.Cancel();
	Root.Cancel();
	TestTrue(TEXT("still cancelled, no crash"), Root.IsCancelled() && Child.IsCancelled());
	return true;
}

// Abort arrives on the game thread while the worker is mid-turn, so cancelling
// from another thread has to be safe.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCancel_CrossThread, "KismetAI.Cancellation.CrossThread", CancelFlags)
bool FCancel_CrossThread::RunTest(const FString& Parameters)
{
	FKismetAICancellationToken Root;
	TArray<FKismetAICancellationToken> Children;
	for (int32 i = 0; i < 32; ++i)
	{
		Children.Add(Root.CreateChild());
	}

	auto Future = Async(EAsyncExecution::Thread, [&Root]() { Root.Cancel(); });
	Future.Wait();

	for (const FKismetAICancellationToken& C : Children)
	{
		TestTrue(TEXT("every child saw the cancellation"), C.IsCancelled());
	}
	return true;
}

namespace
{
	class FCountingInvoker final : public IKismetAIToolInvoker
	{
	public:
		virtual FKismetAIInvokeResult Invoke(const FKismetAIToolInvocation& Invocation) override
		{
			if (const TOptional<FToolExecutionResult> Refused = Invocation.RunAdmission())
			{
				return FKismetAIInvokeResult::FromTyped(Refused.GetValue());
			}
			++Calls;
			return FKismetAIInvokeResult::FromModelString(TEXT("{\"success\":true}"));
		}
		virtual TSet<FString> GetDispatchableToolNames() const override { return {}; }
		virtual FString ResolveHandlerName(const FString&, const FString&) const override { return {}; }
		int32 Calls = 0;
	};
}

// The executor must not dispatch a cancelled call — and must still return a
// tool_result, or the tool_use it answers is left orphaned and the session
// cannot be resumed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCancel_ExecutorSkipsButStillAnswers, "KismetAI.Cancellation.ExecutorSkipsButStillAnswers", CancelFlags)
bool FCancel_ExecutorSkipsButStillAnswers::RunTest(const FString& Parameters)
{
	FCountingInvoker Invoker;
	FKismetAIToolExecutor Executor(Invoker);

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), TEXT("/Game/BP"));

	FToolCallContext Ctx;
	Ctx.Caller = FKismetAICallerContext::InProcess();
	Ctx.ToolName = TEXT("read_blueprint");
	Ctx.Arguments = Args;
	Ctx.RequestBody = Args;
	Ctx.CancelToken.Cancel();

	const FToolCallOutcome Out = Executor.Execute(Ctx);

	TestEqual(TEXT("nothing was dispatched"), Invoker.Calls, 0);
	TestTrue(TEXT("marked cancelled"), Out.bWasCancelled);
	TestTrue(TEXT("reported as an error to the model"), Out.bIsError);
	TestTrue(TEXT("with a CANCELLED code"), Out.ResultForModel.Contains(TEXT("CANCELLED")));
	TestFalse(TEXT("but a result IS produced, so pairing survives"), Out.ResultForModel.IsEmpty());
	return true;
}

// A live token dispatches normally — the check must not be a blanket refusal.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCancel_LiveTokenDispatches, "KismetAI.Cancellation.LiveTokenDispatches", CancelFlags)
bool FCancel_LiveTokenDispatches::RunTest(const FString& Parameters)
{
	FCountingInvoker Invoker;
	FKismetAIToolExecutor Executor(Invoker);

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), TEXT("/Game/BP"));

	FToolCallContext Ctx;
	Ctx.Caller = FKismetAICallerContext::InProcess();
	Ctx.ToolName = TEXT("read_blueprint");
	Ctx.Arguments = Args;
	Ctx.RequestBody = Args;

	const FToolCallOutcome Out = Executor.Execute(Ctx);
	TestEqual(TEXT("dispatched"), Invoker.Calls, 1);
	TestFalse(TEXT("not marked cancelled"), Out.bWasCancelled);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Cancellation listeners. The game-thread dispatcher wakes a waiting
// worker through these instead of polling IsCancelled.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCancel_ListenerFiresOnce, "KismetAI.Cancellation.ListenerFiresOnce", CancelFlags)
bool FCancel_ListenerFiresOnce::RunTest(const FString& Parameters)
{
	FKismetAICancellationToken Run;
	FKismetAICancellationToken Call = Run.CreateChild();
	TSharedRef<int32, ESPMode::ThreadSafe> Fired = MakeShared<int32, ESPMode::ThreadSafe>(0);
	const uint64 Id = Call.Subscribe([Fired]() { ++(*Fired); });
	TestTrue(TEXT("registered while live"), Id != 0);
	TestEqual(TEXT("one listener registered"), Call.NumListenersForTests(), 1);

	// Stop pressed twice, on the parent: the call's listener fires once.
	Run.Cancel();
	Run.Cancel();
	Call.Cancel();
	TestEqual(TEXT("fired exactly once"), *Fired, 1);
	TestEqual(TEXT("listeners released after firing"), Call.NumListenersForTests(), 0);

	Call.Unsubscribe(Id);   // late unsubscribe is harmless
	TestEqual(TEXT("still once"), *Fired, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCancel_ListenerOnCancelledTokenRunsNow, "KismetAI.Cancellation.ListenerOnCancelledTokenRunsNow", CancelFlags)
bool FCancel_ListenerOnCancelledTokenRunsNow::RunTest(const FString& Parameters)
{
	FKismetAICancellationToken Token;
	Token.Cancel();
	int32 Fired = 0;
	const uint64 Id = Token.Subscribe([&Fired]() { ++Fired; });
	TestEqual(TEXT("ran before Subscribe returned"), Fired, 1);
	TestEqual(TEXT("nothing to unsubscribe"), Id, (uint64)0);
	TestEqual(TEXT("nothing retained"), Token.NumListenersForTests(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCancel_UnsubscribedListenerDoesNotFire, "KismetAI.Cancellation.UnsubscribedListenerDoesNotFire", CancelFlags)
bool FCancel_UnsubscribedListenerDoesNotFire::RunTest(const FString& Parameters)
{
	FKismetAICancellationToken Token;
	int32 Fired = 0;
	const uint64 Id = Token.Subscribe([&Fired]() { ++Fired; });
	Token.Unsubscribe(Id);
	TestEqual(TEXT("unsubscribed"), Token.NumListenersForTests(), 0);
	Token.Cancel();
	TestEqual(TEXT("did not fire"), Fired, 0);
	return true;
}

// A listener may itself take locks or cancel other tokens: it runs outside the
// token's lock, so cancelling the very token it is registered on from inside
// it (re-entrancy) neither deadlocks nor fires twice.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCancel_ListenerReentrancy, "KismetAI.Cancellation.ListenerReentrancy", CancelFlags)
bool FCancel_ListenerReentrancy::RunTest(const FString& Parameters)
{
	FKismetAICancellationToken Token;
	FKismetAICancellationToken Other;
	int32 Fired = 0;
	Token.Subscribe([&Fired, Token, Other]() mutable
	{
		++Fired;
		Token.Cancel();
		Other.Cancel();
	});
	Token.Cancel();
	TestEqual(TEXT("fired once"), Fired, 1);
	TestTrue(TEXT("listener could cancel another token"), Other.IsCancelled());
	return true;
}

// An invoker that reports Stop arrived while the handler ran: the executor
// returns the handler's real result and does not call it cancelled.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCancel_ExecutorReportsCancelWhileRunning, "KismetAI.Cancellation.ExecutorReportsCancelWhileRunning", CancelFlags)
bool FCancel_ExecutorReportsCancelWhileRunning::RunTest(const FString& Parameters)
{
	class FRunningInvoker final : public IKismetAIToolInvoker
	{
	public:
		virtual FKismetAIInvokeResult Invoke(const FKismetAIToolInvocation& Invocation) override
		{
			SeenToken = Invocation.CancelToken;
			SeenCancelledResult = Invocation.CancelledBeforeStart;
			if (const TOptional<FToolExecutionResult> Refused = Invocation.RunAdmission())
			{
				return FKismetAIInvokeResult::FromTyped(Refused.GetValue());
			}
			FKismetAICancellationToken Stop = Invocation.CancelToken;
			Stop.Cancel();   // Stop lands mid-handler
			FKismetAIInvokeResult R = FKismetAIInvokeResult::FromTyped(FToolExecutionResult::Success());
			R.bCancelRequestedWhileRunning = true;
			return R;
		}
		virtual TSet<FString> GetDispatchableToolNames() const override { return { TEXT("compile_blueprint") }; }
		virtual FString ResolveHandlerName(const FString&, const FString&) const override { return TEXT("Fake"); }
		FKismetAICancellationToken SeenToken;
		FToolExecutionResult SeenCancelledResult;
	};

	FRunningInvoker Invoker;
	FKismetAIToolExecutor Executor(Invoker);
	FToolCallContext Ctx;
	Ctx.ToolName = TEXT("compile_blueprint");
	Ctx.Caller = FKismetAICallerContext::InProcess();
	Ctx.Arguments = MakeShared<FJsonObject>();
	Ctx.Arguments->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_X"));
	Ctx.RequestBody = Ctx.Arguments;
	const FToolCallOutcome Out = Executor.Execute(Ctx);

	TestTrue(TEXT("the invocation carried the call's own token"), Invoker.SeenToken.IsCancelled() && Ctx.CancelToken.IsCancelled());
	TestEqual(TEXT("the invocation carried the not-started result"), Invoker.SeenCancelledResult.ErrorCode, FString(TEXT("CANCELLED")));
	TestTrue(TEXT("the handler's success is reported"), Out.Typed.bSuccess);
	TestFalse(TEXT("not reported as cancelled"), Out.bWasCancelled);
	TestTrue(TEXT("Stop during the handler is reported"), Out.bCancelRequestedWhileRunning);
	return true;
}

// However the invoker reports a call stopped before its handler started, the
// model and the journal read one canonical not-started result.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCancel_ExecutorCanonicalNotStarted, "KismetAI.Cancellation.ExecutorCanonicalNotStarted", CancelFlags)
bool FCancel_ExecutorCanonicalNotStarted::RunTest(const FString& Parameters)
{
	class FAbandoningInvoker final : public IKismetAIToolInvoker
	{
	public:
		virtual FKismetAIInvokeResult Invoke(const FKismetAIToolInvocation& Invocation) override
		{
			FKismetAIInvokeResult R = FKismetAIInvokeResult::FromTyped(Invocation.CancelledBeforeStart);
			R.bStoppedBeforeStart = true;
			return R;
		}
		virtual TSet<FString> GetDispatchableToolNames() const override { return { TEXT("compile_blueprint") }; }
		virtual FString ResolveHandlerName(const FString&, const FString&) const override { return TEXT("Fake"); }
	};

	FAbandoningInvoker Invoker;
	FKismetAIToolExecutor Executor(Invoker);
	FToolCallContext Ctx;
	Ctx.ToolName = TEXT("compile_blueprint");
	Ctx.Caller = FKismetAICallerContext::InProcess();
	Ctx.Arguments = MakeShared<FJsonObject>();
	Ctx.Arguments->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_X"));
	Ctx.RequestBody = Ctx.Arguments;
	const FToolCallOutcome Out = Executor.Execute(Ctx);

	TestTrue(TEXT("cancelled"), Out.bWasCancelled);
	TestFalse(TEXT("not also running"), Out.bCancelRequestedWhileRunning);
	TestEqual(TEXT("typed code"), Out.Typed.ErrorCode, FString(TEXT("CANCELLED")));
	TestEqual(TEXT("canonical model string"), Out.ResultForModel,
		FKismetAIToolExecutor::CancelledBeforeStartModelString(TEXT("compile_blueprint")));
	return true;
}
