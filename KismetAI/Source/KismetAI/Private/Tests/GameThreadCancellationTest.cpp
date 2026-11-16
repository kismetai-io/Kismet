// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Cancellation at the point editor work begins.
//
// A worker (the panel's agent thread, an HTTP agent run) reaches the editor by
// queueing a callback for the game thread and waiting for it. These tests drive
// the production executor and FKismetAIModuleToolInvoker from a real worker
// thread and use the module's scheduling seam to hold that callback pending
// without blocking the game thread, so every interleaving below is forced by a
// latch rather than by timing:
//
//   - Stop while the callback is queued: the worker must come back without the
//     game thread having to run anything (the shutdown drain
//     shape, where the game thread is the one waiting), the call must be
//     reported as not started, and releasing the callback afterwards must
//     change nothing — no edit, no dirty flag, no save, no admission;
//   - Stop delivered twice: one terminal result, one completion;
//   - Stop during the native operation: the operation is not preempted, its
//     real outcome is reported (applied, not "not executed"), and no further
//     mutation is admitted on that token.

#include "Misc/AutomationTest.h"
#include "KismetAIModule.h"
#include "KismetAIGameThreadDispatcher.h"
#include "KismetAIModuleToolInvoker.h"
#include "KismetAIAgent.h"
#include "KismetAIToolCallJournal.h"
#include "KismetAIToolExecutor.h"
#include "KismetAIToolPolicy.h"
#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/ThreadSafeCounter.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "UObject/Package.h"

namespace KismetAIGameThreadCancelTest
{
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	// How long a test waits for something that must happen promptly. Only an
	// upper bound for a failure; nothing passes by waiting it out.
	constexpr float PromptSeconds = 5.0f;

	UBlueprint* MakeBlueprint(const TCHAR* PackagePath, const TCHAR* AssetName)
	{
		UPackage* Pkg = CreatePackage(PackagePath);
		if (!Pkg) return nullptr;
		UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Pkg, AssetName, BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (BP)
		{
			Pkg->SetDirtyFlag(false);
		}
		return BP;
	}

	UK2Node_CallFunction* AddPrintNode(UBlueprint* BP)
	{
		if (!BP || BP->UbergraphPages.Num() == 0) return nullptr;
		UEdGraph* Graph = BP->UbergraphPages[0];
		UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph);
		Node->FunctionReference.SetExternalMember(
			GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
			UKismetSystemLibrary::StaticClass());
		Graph->AddNode(Node, false, false);
		Node->CreateNewGuid();
		Node->AllocateDefaultPins();
		BP->GetOutermost()->SetDirtyFlag(false);
		return Node;
	}

	TSharedPtr<FJsonObject> SetDurationArgs(UBlueprint* BP, UK2Node_CallFunction* Print, const TCHAR* Value)
	{
		TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
		Op->SetStringField(TEXT("action"), TEXT("set_pin_default"));
		Op->SetStringField(TEXT("node"), Print->GetName());
		Op->SetStringField(TEXT("pin"), TEXT("Duration"));
		Op->SetStringField(TEXT("value"), Value);
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Args->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(Op) });
		return Args;
	}

	FString DurationOf(UK2Node_CallFunction* Print)
	{
		UEdGraphPin* Pin = Print ? Print->FindPin(TEXT("Duration"), EGPD_Input) : nullptr;
		return Pin ? Pin->DefaultValue : FString(TEXT("<no pin>"));
	}

	// Allows everything and counts admissions, so a test can prove a released
	// callback never reached the admission seam (where a single-use grant
	// would be spent). OnAdmission lets a test act at the last instant before
	// the handler runs — the same instant a Stop pumped during the native
	// operation would be observed by nobody.
	class FCountingPolicy final : public IKismetAIToolPolicy
	{
	public:
		virtual FPolicyVerdict Evaluate(const FPolicyContext& Context) const override
		{
			return FPolicyVerdict::Allow();
		}
		virtual FPolicyVerdict EvaluateAdmission(const FPolicyContext& Context) const override
		{
			++AdmissionCalls;
			if (OnAdmission) { OnAdmission(); }
			return FPolicyVerdict::Allow();
		}
		mutable int32 AdmissionCalls = 0;
		TFunction<void()> OnAdmission;
	};

	struct FScopedModulePolicy
	{
		explicit FScopedModulePolicy(const IKismetAIToolPolicy& Policy) { FKismetAIModule::Get().SetToolPolicyOverrideForTests(&Policy); }
		~FScopedModulePolicy() { FKismetAIModule::Get().SetToolPolicyOverrideForTests(nullptr); }
	};

	// The scheduling seam: callbacks the dispatcher queues for the game thread
	// are held here instead of being posted. Queued fires once per held
	// callback, from the worker, so the test knows the call is now waiting on
	// the editor. Release runs them on the game thread (the test's thread).
	class FHeldScheduler
	{
	public:
		FHeldScheduler()
			: QueuedEvent(FPlatformProcess::GetSynchEventFromPool(false))
		{
			FKismetAIModule::Get().SetGameThreadSchedulerForTests([this](TFunction<void()> Callback)
			{
				{
					FScopeLock Lock(&Mutex);
					Held.Add(MoveTemp(Callback));
				}
				QueuedEvent->Trigger();
			});
		}

		~FHeldScheduler()
		{
			FKismetAIModule::Get().SetGameThreadSchedulerForTests(nullptr);
			// Never leave a callback stranded: whatever it does, it does now.
			Release();
			FPlatformProcess::ReturnSynchEventToPool(QueuedEvent);
		}

		bool WaitQueued(float Seconds) { return QueuedEvent->Wait(FTimespan::FromSeconds(Seconds)); }

		int32 Release()
		{
			check(IsInGameThread());
			TArray<TFunction<void()>> ToRun;
			{
				FScopeLock Lock(&Mutex);
				ToRun = MoveTemp(Held);
				Held.Reset();
			}
			for (TFunction<void()>& Callback : ToRun)
			{
				Callback();
			}
			return ToRun.Num();
		}

	private:
		FCriticalSection Mutex;
		TArray<TFunction<void()>> Held;
		FEvent* QueuedEvent;
	};

	// Pump the game thread until a worker's future is ready (or give up).
	template <typename T>
	bool PumpUntilReady(TFuture<T>& Future, float Seconds)
	{
		const double Deadline = FPlatformTime::Seconds() + Seconds;
		while (!Future.IsReady() && FPlatformTime::Seconds() < Deadline)
		{
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FPlatformProcess::SleepNoStats(0);
		}
		return Future.IsReady();
	}

	bool PackageOnDisk(UBlueprint* BP)
	{
		return BP && FPackageName::DoesPackageExist(BP->GetOutermost()->GetName());
	}
}

// Stop while the call is queued for the game thread. The game thread is the
// one waiting here (as it is in ShutdownModule's drain): it does
// not pump, so the worker can only come back if cancellation itself releases
// its wait.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameThreadCancel_QueuedCallIsAbandoned,
	"KismetAI.GameThreadCancel.QueuedCallIsAbandoned", KismetAIGameThreadCancelTest::Flags)
bool FGameThreadCancel_QueuedCallIsAbandoned::RunTest(const FString& Parameters)
{
	using namespace KismetAIGameThreadCancelTest;

	UBlueprint* BP = MakeBlueprint(TEXT("/Temp/KismetP107Queued"), TEXT("BP_KismetP107Queued"));
	UK2Node_CallFunction* Print = AddPrintNode(BP);
	if (!Print)
	{
		AddError(TEXT("fixture setup failed"));
		return false;
	}
	const FString Original = DurationOf(Print);
	const int32 NodesBefore = BP->UbergraphPages[0]->Nodes.Num();

	FCountingPolicy Policy;
	FScopedModulePolicy ScopedPolicy(Policy);
	FHeldScheduler Scheduler;
	const FKismetAIGameThreadDispatcher::FStats Before = FKismetAIModule::Get().GetGameThreadDispatchStatsForTests();

	// The panel adapter: an agent whose caller is the editor user, its tool
	// call made on a worker thread exactly as the panel's run makes it.
	FKismetAIAgentConfig Config;
	TSharedRef<FKismetAIAgent, ESPMode::ThreadSafe> Agent = MakeShared<FKismetAIAgent, ESPMode::ThreadSafe>(
		MakeUnique<FKismetAIModuleToolInvoker>(FKismetAIModule::Get()), Config);
	Agent->SetCallerContext(FKismetAICallerContext::Verified(EKismetAIEntryPoint::Panel, TEXT("editor-user")));
	const TSharedPtr<FJsonObject> Args = SetDurationArgs(BP, Print, TEXT("9.5"));

	TFuture<FString> Worker = Async(EAsyncExecution::Thread, [Agent, Args]()
	{
		return Agent->ExecuteToolForExternalClient(TEXT("edit_blueprint"), Args);
	});

	if (!TestTrue(TEXT("the call reached the game-thread queue"), Scheduler.WaitQueued(PromptSeconds)))
	{
		Agent->Abort();
		PumpUntilReady(Worker, PromptSeconds);
		return false;
	}

	// Stop, twice (a double-click, or panel Stop plus shutdown).
	Agent->Abort();
	Agent->Abort();

	// The game thread does not pump here.
	const bool bReturnedWithoutGameThread = Worker.WaitFor(FTimespan::FromSeconds(PromptSeconds));
	TestTrue(TEXT("Stop releases the worker without the game thread running its callback"), bReturnedWithoutGameThread);
	if (!bReturnedWithoutGameThread)
	{
		// Older shape: the worker is parked on the callback. Let it go so the
		// test can finish, then report what it saw.
		Scheduler.Release();
		PumpUntilReady(Worker, PromptSeconds);
	}
	if (!TestTrue(TEXT("worker finished"), Worker.IsReady()))
	{
		return false;
	}

	const FString Result = Worker.Get();
	TestTrue(TEXT("reported as cancelled before it ran"), KismetAIToolCallJournal::IsExecutorCancelledResult(Result));

	// Now let the abandoned callback run on the game thread.
	Scheduler.Release();
	FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);

	TestEqual(TEXT("pin unchanged"), DurationOf(Print), Original);
	TestEqual(TEXT("no node added or removed"), BP->UbergraphPages[0]->Nodes.Num(), NodesBefore);
	TestFalse(TEXT("package not dirtied"), BP->GetOutermost()->IsDirty());
	TestFalse(TEXT("package not saved"), PackageOnDisk(BP));
	TestEqual(TEXT("the released callback never reached admission"), Policy.AdmissionCalls, 0);

	// Two Stops, one terminal transition, one completion signal; nothing left
	// waiting.
	const FKismetAIGameThreadDispatcher::FStats After = FKismetAIModule::Get().GetGameThreadDispatchStatsForTests();
	TestEqual(TEXT("one job dispatched"), After.Dispatched - Before.Dispatched, 1);
	TestEqual(TEXT("abandoned exactly once"), After.Abandoned - Before.Abandoned, 1);
	TestEqual(TEXT("never started"), After.Started - Before.Started, 0);
	TestEqual(TEXT("never completed"), After.Completed - Before.Completed, 0);
	TestEqual(TEXT("no worker still waiting"), After.InFlight, 0);
	return true;
}

// Stop at the last instant before the handler — from then on the native
// operation is not interruptible. The outcome must say what really happened
// (applied), and the next mutation on the same token must not be admitted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameThreadCancel_CancelDuringWorkReportsApplied,
	"KismetAI.GameThreadCancel.CancelDuringWorkReportsApplied", KismetAIGameThreadCancelTest::Flags)
bool FGameThreadCancel_CancelDuringWorkReportsApplied::RunTest(const FString& Parameters)
{
	using namespace KismetAIGameThreadCancelTest;

	// This test controls admission/cancellation interleaving synchronously.
	FKismetAIModule::Get().SetGameThreadSchedulerForTests([](TFunction<void()> Callback)
	{
		AsyncTask(ENamedThreads::GameThread, MoveTemp(Callback));
	});
	ON_SCOPE_EXIT { FKismetAIModule::Get().SetGameThreadSchedulerForTests(nullptr); };

	UBlueprint* BP = MakeBlueprint(TEXT("/Temp/KismetP107Running"), TEXT("BP_KismetP107Running"));
	UK2Node_CallFunction* Print = AddPrintNode(BP);
	if (!Print)
	{
		AddError(TEXT("fixture setup failed"));
		return false;
	}

	FKismetAICancellationToken Token;
	FCountingPolicy Policy;
	// Stop (twice) at the last instant before the handler: from here the native
	// operation is not interruptible.
	Policy.OnAdmission = [Token]() mutable { Token.Cancel(); Token.Cancel(); };
	FScopedModulePolicy ScopedPolicy(Policy);
	const FKismetAIGameThreadDispatcher::FStats Before = FKismetAIModule::Get().GetGameThreadDispatchStatsForTests();

	FToolCallContext Ctx;
	Ctx.ToolName = TEXT("edit_blueprint");
	Ctx.Caller = FKismetAICallerContext::Verified(EKismetAIEntryPoint::Panel, TEXT("editor-user"));
	Ctx.Arguments = SetDurationArgs(BP, Print, TEXT("6.25"));
	Ctx.RequestBody = Ctx.Arguments;
	Ctx.CancelToken = Token;

	TFuture<FToolCallOutcome> Worker = Async(EAsyncExecution::Thread, [Ctx]()
	{
		FKismetAIModuleToolInvoker Invoker(FKismetAIModule::Get());
		FKismetAIToolExecutor Executor(Invoker);
		return Executor.Execute(Ctx);
	});
	if (!TestTrue(TEXT("worker finished"), PumpUntilReady(Worker, PromptSeconds * 2)))
	{
		return false;
	}
	const FToolCallOutcome Out = Worker.Get();

	TestTrue(TEXT("Stop arrived during the call"), Token.IsCancelled());
	TestTrue(TEXT("the edit was applied"), DurationOf(Print).StartsWith(TEXT("6.25")));
	TestTrue(TEXT("reported as succeeded"), Out.Typed.bSuccess);
	TestFalse(TEXT("not reported as cancelled"), Out.bWasCancelled);
	TestTrue(TEXT("reported as Stop-while-running"), Out.bCancelRequestedWhileRunning);
	{
		const FKismetAIGameThreadDispatcher::FStats After = FKismetAIModule::Get().GetGameThreadDispatchStatsForTests();
		TestEqual(TEXT("started once"), After.Started - Before.Started, 1);
		TestEqual(TEXT("completed once (one completion signal)"), After.Completed - Before.Completed, 1);
		TestEqual(TEXT("a running job is never abandoned"), After.Abandoned - Before.Abandoned, 0);
	}
	TestFalse(TEXT("never described as not executed"), KismetAIToolCallJournal::IsExecutorCancelledResult(Out.ResultForModel));
	TestEqual(TEXT("admitted exactly once"), Policy.AdmissionCalls, 1);

	// The next mutation on the same (now cancelled) token is not admitted.
	FToolCallContext Next = Ctx;
	Next.Arguments = SetDurationArgs(BP, Print, TEXT("1.5"));
	Next.RequestBody = Next.Arguments;
	TFuture<FToolCallOutcome> Worker2 = Async(EAsyncExecution::Thread, [Next]()
	{
		FKismetAIModuleToolInvoker Invoker(FKismetAIModule::Get());
		FKismetAIToolExecutor Executor(Invoker);
		return Executor.Execute(Next);
	});
	if (!TestTrue(TEXT("second worker finished"), PumpUntilReady(Worker2, PromptSeconds)))
	{
		return false;
	}
	const FToolCallOutcome Out2 = Worker2.Get();
	TestTrue(TEXT("next mutation cancelled"), Out2.bWasCancelled);
	TestEqual(TEXT("next mutation code"), Out2.Typed.ErrorCode, FString(TEXT("CANCELLED")));
	TestTrue(TEXT("pin keeps the applied value"), DurationOf(Print).StartsWith(TEXT("6.25")));
	TestEqual(TEXT("no further admission"), Policy.AdmissionCalls, 1);

	BP->GetOutermost()->SetDirtyFlag(false);
	return true;
}

// Teardown: Close abandons every queued job, wakes each worker once, refuses
// later dispatches, and a callback that runs afterwards never enters its body
// (which, in production, is the only part that touches the module).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameThreadCancel_CloseAbandonsQueuedWork,
	"KismetAI.GameThreadCancel.CloseAbandonsQueuedWork", KismetAIGameThreadCancelTest::Flags)
bool FGameThreadCancel_CloseAbandonsQueuedWork::RunTest(const FString& Parameters)
{
	using namespace KismetAIGameThreadCancelTest;

	// A dispatcher of its own: closing the module's would end every later test.
	TSharedRef<FKismetAIGameThreadDispatcher, ESPMode::ThreadSafe> Dispatcher =
		MakeShared<FKismetAIGameThreadDispatcher, ESPMode::ThreadSafe>();

	FCriticalSection HeldMutex;
	TArray<TFunction<void()>> Held;
	FEvent* Queued = FPlatformProcess::GetSynchEventFromPool(false);
	ON_SCOPE_EXIT { FPlatformProcess::ReturnSynchEventToPool(Queued); };
	Dispatcher->SetSchedulerForTests([&HeldMutex, &Held, Queued](TFunction<void()> Callback)
	{
		{
			FScopeLock Lock(&HeldMutex);
			Held.Add(MoveTemp(Callback));
		}
		Queued->Trigger();
	});

	TSharedRef<FThreadSafeCounter, ESPMode::ThreadSafe> BodyRuns = MakeShared<FThreadSafeCounter, ESPMode::ThreadSafe>();
	const FToolExecutionResult Stopped = FToolExecutionResult::Error(409, TEXT("CANCELLED"), TEXT("stopped"));
	auto DispatchOne = [Dispatcher, BodyRuns, Stopped]()
	{
		FKismetAICancellationToken NeverCancelled;
		return Dispatcher->Dispatch([BodyRuns]()
		{
			BodyRuns->Increment();
			FKismetAIDispatchOutcome Out;
			Out.Result = FToolExecutionResult::Success();
			return Out;
		}, NeverCancelled, Stopped);
	};

	TFuture<FKismetAIDispatchOutcome> A = Async(EAsyncExecution::Thread, DispatchOne);
	// An auto-reset event coalesces two triggers before a wait. Acknowledge
	// the first enqueue before starting the second; both remain queued at Close.
	if (!TestTrue(TEXT("first queued"), Queued->Wait(FTimespan::FromSeconds(PromptSeconds))))
	{
		Dispatcher->Close();
		A.Wait();
		return false;
	}
	TFuture<FKismetAIDispatchOutcome> B = Async(EAsyncExecution::Thread, DispatchOne);
	if (!TestTrue(TEXT("second queued"), Queued->Wait(FTimespan::FromSeconds(PromptSeconds))))
	{
		Dispatcher->Close();
		return false;
	}
	TestEqual(TEXT("two workers waiting"), Dispatcher->GetStatsForTests().InFlight, 2);

	// Teardown, twice. The game thread does not pump.
	Dispatcher->Close();
	Dispatcher->Close();
	TestTrue(TEXT("first worker woken by Close"), A.WaitFor(FTimespan::FromSeconds(PromptSeconds)));
	TestTrue(TEXT("second worker woken by Close"), B.WaitFor(FTimespan::FromSeconds(PromptSeconds)));
	if (!A.IsReady() || !B.IsReady())
	{
		return false;
	}
	TestTrue(TEXT("first reported not started"), A.Get().bStoppedBeforeStart && A.Get().Result.ErrorCode == TEXT("CANCELLED"));
	TestTrue(TEXT("second reported not started"), B.Get().bStoppedBeforeStart && B.Get().Result.ErrorCode == TEXT("CANCELLED"));

	// Later dispatches are refused without queueing anything.
	TFuture<FKismetAIDispatchOutcome> C = Async(EAsyncExecution::Thread, DispatchOne);
	TestTrue(TEXT("dispatch after Close returns at once"), C.WaitFor(FTimespan::FromSeconds(PromptSeconds)));
	if (C.IsReady())
	{
		TestTrue(TEXT("refused as not started"), C.Get().bStoppedBeforeStart);
	}

	// The callbacks queued before Close finally run: they must do nothing.
	TArray<TFunction<void()>> ToRun;
	{
		FScopeLock Lock(&HeldMutex);
		ToRun = MoveTemp(Held);
	}
	TestEqual(TEXT("exactly the two pre-Close callbacks were queued"), ToRun.Num(), 2);
	for (TFunction<void()>& Callback : ToRun)
	{
		Callback();
	}
	TestEqual(TEXT("no body ran"), BodyRuns->GetValue(), 0);

	const FKismetAIGameThreadDispatcher::FStats Stats = Dispatcher->GetStatsForTests();
	TestEqual(TEXT("two dispatched"), Stats.Dispatched, 2);
	TestEqual(TEXT("two abandoned, one signal each"), Stats.Abandoned, 2);
	TestEqual(TEXT("none started"), Stats.Started, 0);
	TestEqual(TEXT("none completed"), Stats.Completed, 0);
	TestEqual(TEXT("nothing in flight"), Stats.InFlight, 0);
	return true;
}

// The worker's wait outlives nothing it borrowed: after a queued call is
// abandoned the worker returns and its stack (the executor, the invocation,
// the default policy) is gone; the callback that runs afterwards only holds
// what the job owns. Run under the default AsyncTask scheduler so the late
// callback is a real task-graph task.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameThreadCancel_LateCallbackAfterWorkerReturned,
	"KismetAI.GameThreadCancel.LateCallbackAfterWorkerReturned", KismetAIGameThreadCancelTest::Flags)
bool FGameThreadCancel_LateCallbackAfterWorkerReturned::RunTest(const FString& Parameters)
{
	using namespace KismetAIGameThreadCancelTest;

	UBlueprint* BP = MakeBlueprint(TEXT("/Temp/KismetP107Late"), TEXT("BP_KismetP107Late"));
	UK2Node_CallFunction* Print = AddPrintNode(BP);
	if (!Print)
	{
		AddError(TEXT("fixture setup failed"));
		return false;
	}
	const FString Original = DurationOf(Print);

	// The worker cancels its own call right after it is queued — from the
	// worker thread, while the game thread (this test) is blocked waiting for
	// it and cannot run the AsyncTask.
	TSharedRef<FKismetAICancellationToken, ESPMode::ThreadSafe> Token = MakeShared<FKismetAICancellationToken, ESPMode::ThreadSafe>();
	FEvent* Queued = FPlatformProcess::GetSynchEventFromPool(false);
	ON_SCOPE_EXIT { FPlatformProcess::ReturnSynchEventToPool(Queued); };
	FKismetAIModule::Get().SetGameThreadSchedulerForTests([Queued](TFunction<void()> Callback)
	{
		AsyncTask(ENamedThreads::GameThread, MoveTemp(Callback));
		Queued->Trigger();
	});
	ON_SCOPE_EXIT { FKismetAIModule::Get().SetGameThreadSchedulerForTests(nullptr); };

	const TSharedPtr<FJsonObject> Args = SetDurationArgs(BP, Print, TEXT("8.5"));
	TFuture<FToolCallOutcome> Worker = Async(EAsyncExecution::Thread, [Token, Args, Queued]()
	{
		FKismetAIModuleToolInvoker Invoker(FKismetAIModule::Get());
		FKismetAIToolExecutor Executor(Invoker);   // default policy: owned by this stack frame
		FToolCallContext Ctx;
		Ctx.ToolName = TEXT("edit_blueprint");
		Ctx.Caller = FKismetAICallerContext::InProcess();
		Ctx.Arguments = Args;
		Ctx.RequestBody = Args;
		Ctx.CancelToken = *Token;
		// Cancel from a helper thread once the callback is queued.
		TFuture<void> Stopper = Async(EAsyncExecution::Thread, [Token, Queued]()
		{
			Queued->Wait();
			Token->Cancel();
		});
		FToolCallOutcome Out = Executor.Execute(Ctx);
		Stopper.Wait();
		return Out;
	});

	const bool bReturned = Worker.WaitFor(FTimespan::FromSeconds(PromptSeconds));
	TestTrue(TEXT("worker returned while its callback was still queued"), bReturned);
	if (!bReturned)
	{
		PumpUntilReady(Worker, PromptSeconds);
		return false;
	}
	TestTrue(TEXT("reported as cancelled before it ran"), Worker.Get().bWasCancelled);

	// Now the stale task runs, after the worker's executor and invocation are gone.
	FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	TestEqual(TEXT("pin unchanged"), DurationOf(Print), Original);
	TestFalse(TEXT("package not dirtied"), BP->GetOutermost()->IsDirty());
	return true;
}
