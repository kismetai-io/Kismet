// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// The historical "illegal StaticLoadObject while saving" report.
//
// The engine refuses object lookup on the game thread while a package is being
// serialized: StaticLoadObject/FindPackage reach StaticFindObjectFast, which
// logs Fatal "Illegal call to StaticFindObjectFast() while serializing object
// data!" (UObjectGlobals.cpp). The flag is per game thread, so only KismetAI
// code that runs ON the game thread INSIDE a save can trip it.
//
// How such code gets inside a save: SavePackage reports progress through
// FScopedSlowTask -> GWarn->RequestUpdateUI -> ProgressReported. In the editor
// (FFeedbackContextEditor) that flushes rendering commands and ticks Slate,
// and the render sync pumps the game thread's task queue
// (FFrameEndSync::Sync / FRenderCommandFence::Wait -> ProcessTasksUntilIdle,
// visible in the retained UE5.7 crash stacks). Every
// AsyncTask(ENamedThreads::GameThread, ...) KismetAI has queued runs there.
// The built-in autosave shows exactly that progress dialog.
//
// These tests stand a pumping feedback context in for the editor's progress UI
// (the automation editor runs -unattended with no progress window) and save a
// disposable package with a KismetAI game-thread callback already queued.

#include "KismetAIModule.h"
#include "KismetAIModuleToolInvoker.h"
#include "KismetAIMutationIntent.h"
#include "KismetAIToolExecutor.h"
#include "ScopedTestGrant.h"
#include "DisposableContent.h"
#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "BlueprintActionDatabase.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/FeedbackContext.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	constexpr auto ReentrancyFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	bool IsSavingNow()
	{
#if UE_VERSION_OLDER_THAN(5, 8, 0)
		return GIsSavingPackage;
#else
		return UE::IsSavingPackage();
#endif
	}

	// The editor's progress UI, reduced to the part that matters: reporting
	// progress pumps the game thread's task queue.
	class FPumpingProgressContext : public FFeedbackContext
	{
	public:
		int32 PumpsWhileSaving = 0;

		virtual void ProgressReported(const float TotalProgressInterp, FText DisplayMessage) override
		{
			// The editor pumps on every report. Pumping only inside the saving
			// flag's scope makes the hazardous interleaving deterministic: the
			// queued callback cannot slip out at an earlier report.
			if (IsSavingNow())
			{
				FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
				++PumpsWhileSaving;
			}
			if (PumpsWhileSaving < 2)
			{
				// RequestUpdateUI throttles to five updates a second; wait out the
				// throttle so the next progress frame inside the save reports too.
				FPlatformProcess::SleepNoStats(0.21f);
			}
		}

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Previous)
			{
				Previous->Serialize(V, Verbosity, Category);
			}
		}

		FFeedbackContext* Previous = nullptr;
	};

	class FScopedPumpingProgress
	{
	public:
		FScopedPumpingProgress()
		{
			Context.Previous = GWarn;
			GWarn = &Context;
		}
		~FScopedPumpingProgress()
		{
			GWarn = Context.Previous;
		}
		FPumpingProgressContext Context;
	};

	// A Blueprint in a temporary mount: nothing reaches the project's Content.
	struct FDisposableBlueprint
	{
		FString Mount;
		FString Directory;
		FString PackageName;
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = nullptr;

		FDisposableBlueprint()
		{
			const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Mount = TEXT("/Game/__KismetP116_") + Id + TEXT("/");
			Directory = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("KismetP116_") + Id) + TEXT("/");
			IFileManager::Get().MakeDirectory(*Directory, true);
			FPackageName::RegisterMountPoint(Mount, Directory);
			PackageName = Mount + TEXT("BP_SaveReentrancy");
			Package = CreatePackage(*PackageName);
			Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package,
				TEXT("BP_SaveReentrancy"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		}

		~FDisposableBlueprint()
		{
			// Retire it the way CheckpointRecoveryTest does: spawners leave the
			// action database and the classes go with the Blueprint, so a later
			// insert_node or package reload never meets a half-disposed class.
			if (Blueprint)
			{
				FBlueprintActionDatabase::Get().ClearAssetActions(Blueprint);
				for (UClass* Class : { Blueprint->GeneratedClass.Get(), Blueprint->SkeletonGeneratedClass.Get() })
				{
					if (Class)
					{
						Class->ClearFlags(RF_Public | RF_Standalone);
						Class->MarkAsGarbage();
					}
				}
				Blueprint->ClearFlags(RF_Public | RF_Standalone);
				Blueprint->MarkAsGarbage();
			}
			if (Package)
			{
				Package->SetDirtyFlag(false);
			}
			KismetAIDisposableContent::ForgetUndoHistory();
			FPackageName::UnRegisterMountPoint(Mount, Directory);
			IFileManager::Get().DeleteDirectory(*Directory, false, true);
		}

		bool Save() const
		{
			const FString File = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Standalone;
			Args.SaveFlags = SAVE_NoError;
			Args.bSlowTask = true;
			return UPackage::SavePackage(Package, Blueprint, *File, Args);
		}
	};

	// Wait (game thread, without pumping) until a worker has queued its job.
	bool WaitForQueuedJob(const FKismetAIGameThreadDispatcher& Dispatcher, int32 Expected)
	{
		const double Deadline = FPlatformTime::Seconds() + 10.0;
		while (Dispatcher.GetStatsForTests().Dispatched < Expected)
		{
			if (FPlatformTime::Seconds() > Deadline)
			{
				return false;
			}
			FPlatformProcess::SleepNoStats(0.001f);
		}
		return true;
	}

	template <typename T>
	bool PumpUntilReady(TFuture<T>& Future, double Seconds)
	{
		const double Deadline = FPlatformTime::Seconds() + Seconds;
		while (!Future.IsReady() && FPlatformTime::Seconds() < Deadline)
		{
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FPlatformProcess::SleepNoStats(0.001f);
		}
		return Future.IsReady();
	}
}

// The mutation journal captures asset state through the invoker's
// game-thread hop (KismetAIAssetState::Capture -> FindPackage). Queued before a
// save and pumped inside it, that capture used to run under the saving flag:
// the exact "Illegal call ... while serializing object data" Fatal. It must
// now wait for the save to end and then capture normally.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSaveReentrancy_QueuedCaptureWaitsForSave,
	"KismetAI.EditorSafety.SaveReentrancy.QueuedCaptureWaitsForSave", ReentrancyFlags)
bool FSaveReentrancy_QueuedCaptureWaitsForSave::RunTest(const FString& Parameters)
{
	struct FCaptureState
	{
		FDisposableBlueprint Fixture;
		TSharedRef<FKismetAIGameThreadDispatcher, ESPMode::ThreadSafe> Dispatcher = MakeShared<FKismetAIGameThreadDispatcher, ESPMode::ThreadSafe>();
		TFuture<TArray<FKismetAIAssetFingerprint>> Capture;
		double Deadline = FPlatformTime::Seconds() + 20.0;
		~FCaptureState() { Dispatcher->Close(); }
	};
	class FVerifyCapture final : public IAutomationLatentCommand
	{
	public:
		FVerifyCapture(FAutomationTestBase* InTest, TSharedRef<FCaptureState> InState) : Test(InTest), State(InState) {}
		virtual bool Update() override
		{
			if (!State->Capture.IsReady() && FPlatformTime::Seconds() < State->Deadline) return false;
			if (!Test->TestTrue(TEXT("capture finishes after the save"), State->Capture.IsReady()))
			{
				State->Dispatcher->Close();
				return true;
			}
			const TArray<FKismetAIAssetFingerprint> States = State->Capture.Get();
			if (Test->TestEqual(TEXT("one fingerprint"), States.Num(), 1))
			{
				Test->TestTrue(TEXT("the capture ran (late, not skipped)"), States[0].bCaptured);
				Test->TestTrue(TEXT("and saw the saved asset"), States[0].bExists && States[0].bFileExists);
				Test->TestFalse(TEXT("after the save, not during it: the package is clean"), States[0].bPackageDirty);
			}
			return true;
		}
	private:
		FAutomationTestBase* Test;
		TSharedRef<FCaptureState> State;
	};
	auto State = MakeShared<FCaptureState>();
	if (!TestNotNull(TEXT("disposable Blueprint created"), State->Fixture.Blueprint)) return false;
	State->Fixture.Package->MarkPackageDirty();
	auto Invoker = MakeShared<FKismetAIModuleToolInvoker>(FKismetAIModule::Get(), State->Dispatcher);
	const TArray<FString> Paths = { State->Fixture.PackageName };
	State->Capture = Async(EAsyncExecution::ThreadPool, [Invoker, Paths]() { return Invoker->CaptureAssetState(Paths); });
	if (!TestTrue(TEXT("capture queued for the game thread"), WaitForQueuedJob(*State->Dispatcher, 1))) return false;
	int32 PumpsWhileSaving = 0;
	bool bSaved = false;
	{
		FScopedPumpingProgress Progress;
		FPlatformProcess::SleepNoStats(0.21f);
		bSaved = State->Fixture.Save();
		PumpsWhileSaving = Progress.Context.PumpsWhileSaving;
	}
	TestTrue(TEXT("disposable package saved"), bSaved);
	TestTrue(TEXT("the save pumped the game-thread queue while the saving flag was set"), PumpsWhileSaving > 0);
	TestEqual(TEXT("frame dispatch cannot enter object capture inside a reentrant save"), State->Dispatcher->GetStatsForTests().Started, 0);
	TestFalse(TEXT("capture remains queued until a normal frame after saving"), State->Capture.IsReady());
	ADD_LATENT_AUTOMATION_COMMAND(FVerifyCapture(this, State));
	return true;
}

// The legacy bounce (ExecuteHandlerDirect: AsyncTask + FEvent), which is the
// path a tool call took when the original report was filed. Queued before the
// save and pumped inside it, it must refuse with EDITOR_BUSY (the M1 guard)
// instead of entering a handler that loads the Blueprint.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSaveReentrancy_QueuedLegacyToolIsRefused,
	"KismetAI.EditorSafety.SaveReentrancy.QueuedLegacyToolIsRefused", ReentrancyFlags)
bool FSaveReentrancy_QueuedLegacyToolIsRefused::RunTest(const FString& Parameters)
{
	FDisposableBlueprint Fixture;
	if (!TestNotNull(TEXT("disposable Blueprint created"), Fixture.Blueprint)) return false;
	Fixture.Package->MarkPackageDirty();

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), Fixture.Blueprint->GetPathName());
	std::atomic<bool> bStarted{ false };
	TFuture<FString> Result = Async(EAsyncExecution::ThreadPool, [Args, &bStarted]()
	{
		bStarted = true;
		return FKismetAIModule::Get().ExecuteHandlerDirect(&FKismetAIModule::HandleCompile, Args);
	});
	while (!bStarted)
	{
		FPlatformProcess::SleepNoStats(0.001f);
	}
	// The worker posts its AsyncTask right after starting; give it a moment.
	FPlatformProcess::SleepNoStats(0.05f);

	int32 PumpsWhileSaving = 0;
	bool bSaved = false;
	{
		FScopedPumpingProgress Progress;
		FPlatformProcess::SleepNoStats(0.21f);
		bSaved = Fixture.Save();
		PumpsWhileSaving = Progress.Context.PumpsWhileSaving;
	}
	TestTrue(TEXT("disposable package saved"), bSaved);
	TestTrue(TEXT("the save pumped the game-thread queue while the saving flag was set"), PumpsWhileSaving > 0);
	if (!TestTrue(TEXT("legacy call returns"), PumpUntilReady(Result, 20.0))) return false;
	const FString Text = Result.Get();
	TestTrue(TEXT("the call inside the save was refused before the handler"), Text.Contains(TEXT("EDITOR_BUSY")));
	return true;
}

// Run finalization (the panel's and /agent/run's end-of-run game-thread task)
// loads the Blueprint for the diff and fingerprints the run's packages. Posted
// as an AsyncTask, it can be pumped inside a save like any other; it must wait
// for the save and then run, not be dropped.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSaveReentrancy_QueuedFinalizationWaitsForSave,
	"KismetAI.EditorSafety.SaveReentrancy.QueuedFinalizationWaitsForSave", ReentrancyFlags)
bool FSaveReentrancy_QueuedFinalizationWaitsForSave::RunTest(const FString& Parameters)
{
	FDisposableBlueprint Fixture;
	if (!TestNotNull(TEXT("disposable Blueprint created"), Fixture.Blueprint)) return false;
	Fixture.Package->MarkPackageDirty();

	FKismetAIModule& Module = FKismetAIModule::Get();
	TSharedRef<int32> Runs = MakeShared<int32>(0);
	TSharedRef<bool> bRanWhileSaving = MakeShared<bool>(false);
	TSharedRef<bool> bOfferedWhileSaving = MakeShared<bool>(false);
	const FString PackageName = Fixture.PackageName;
	AsyncTask(ENamedThreads::GameThread, [&Module, Runs, bRanWhileSaving, bOfferedWhileSaving, PackageName]()
	{
		*bOfferedWhileSaving = IsSavingNow();
		Module.RunWhenObjectAccessAllowed([Runs, bRanWhileSaving, PackageName]()
		{
			*bRanWhileSaving |= IsSavingNow();
			// What finalization does first: find the run's package.
			FindPackage(nullptr, *PackageName);
			++*Runs;
		});
	});

	bool bSaved = false;
	{
		FScopedPumpingProgress Progress;
		FPlatformProcess::SleepNoStats(0.21f);
		bSaved = Fixture.Save();
	}
	TestTrue(TEXT("disposable package saved"), bSaved);
	TestTrue(TEXT("the finalization task was pumped inside the save"), *bOfferedWhileSaving);
	TestEqual(TEXT("and did not run there"), *Runs, 0);

	FTSTicker::GetCoreTicker().Tick(0.0f);
	TestEqual(TEXT("it runs on the next tick after the save"), *Runs, 1);
	TestFalse(TEXT("never under the saving flag"), *bRanWhileSaving);
	FTSTicker::GetCoreTicker().Tick(0.0f);
	TestEqual(TEXT("exactly once"), *Runs, 1);
	return true;
}

namespace
{
	struct FHeldBusyDispatch
	{
		struct FQueue
		{
			FCriticalSection Mutex;
			TArray<TFunction<void()>> Callbacks;
		};

		TSharedRef<FKismetAIGameThreadDispatcher, ESPMode::ThreadSafe> Dispatcher = MakeShared<FKismetAIGameThreadDispatcher, ESPMode::ThreadSafe>();
		TSharedRef<FQueue, ESPMode::ThreadSafe> Queue = MakeShared<FQueue, ESPMode::ThreadSafe>();

		FHeldBusyDispatch()
		{
			Dispatcher->SetSchedulerForTests([State = Queue](TFunction<void()> Callback)
			{
				FScopeLock Lock(&State->Mutex);
				State->Callbacks.Add(MoveTemp(Callback));
			});
		}

		~FHeldBusyDispatch()
		{
			Dispatcher->Close();
			Release();
		}

		bool HasQueued()
		{
			FScopeLock Lock(&Queue->Mutex);
			return !Queue->Callbacks.IsEmpty();
		}

		void Release()
		{
			TArray<TFunction<void()>> Ready;
			{
				FScopeLock Lock(&Queue->Mutex);
				Ready = MoveTemp(Queue->Callbacks);
				Queue->Callbacks.Reset();
			}
			for (TFunction<void()>& Callback : Ready) Callback();
		}

		void ReleaseWhileBusy()
		{
			const bool bPrevious = GIsSlowTask;
			GIsSlowTask = true;
			ON_SCOPE_EXIT { GIsSlowTask = bPrevious; };
			Release();
		}

		template <typename T>
		bool AwaitQueued(TFuture<T>& Worker)
		{
			const double Deadline = FPlatformTime::Seconds() + 5.0;
			while (!HasQueued() && !Worker.IsReady() && FPlatformTime::Seconds() < Deadline)
			{
				FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			}
			return HasQueued();
		}

		template <typename T>
		bool Finish(TFuture<T>& Worker, double Seconds = 5.0)
		{
			const double Deadline = FPlatformTime::Seconds() + Seconds;
			while (!Worker.IsReady() && FPlatformTime::Seconds() < Deadline)
			{
				Release();
				FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			}
			return Worker.IsReady();
		}
	};

	FKismetAIToolInvocation BusyReadInvocation(const FDisposableBlueprint& Fixture,
		const TSharedRef<int32, ESPMode::ThreadSafe>& Admissions)
	{
		FKismetAIToolInvocation Call;
		Call.ToolName = TEXT("read_blueprint");
		Call.Mode = TEXT("describe");
		Call.RequestBody = MakeShared<FJsonObject>();
		Call.RequestBody->SetStringField(TEXT("blueprint"), Fixture.PackageName);
		Call.Admit = [Admissions]() -> TOptional<FToolExecutionResult>
		{
			++*Admissions;
			return {};
		};
		return Call;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSaveReentrancy_BusyWorkerWaitsOnce,
	"KismetAI.EditorSafety.SaveReentrancy.BusyWorkerWaitsOnce", ReentrancyFlags)
bool FSaveReentrancy_BusyWorkerWaitsOnce::RunTest(const FString& Parameters)
{
	FDisposableBlueprint Fixture;
	FHeldBusyDispatch Held;
	const auto Admissions = MakeShared<int32, ESPMode::ThreadSafe>(0);
	const FKismetAIToolInvocation Call = BusyReadInvocation(Fixture, Admissions);
	const auto Invoker = MakeShared<FKismetAIModuleToolInvoker>(FKismetAIModule::Get(), Held.Dispatcher);
	TFuture<FKismetAIInvokeResult> Worker = Async(EAsyncExecution::ThreadPool, [Invoker, Call]() { return Invoker->Invoke(Call); });
	ON_SCOPE_EXIT { Held.Dispatcher->Close(); Held.Release(); Worker.Wait(); };
	if (!TestTrue(TEXT("first callback queued"), Held.AwaitQueued(Worker))) return false;
	Held.ReleaseWhileBusy();
	TestEqual(TEXT("busy refusal consumes no admission"), *Admissions, 0);
	if (!TestTrue(TEXT("worker waits and queues another admission attempt"), Held.AwaitQueued(Worker))) return false;
	Held.Release();
	if (!TestTrue(TEXT("worker finishes after editor is safe"), Held.Finish(Worker))) return false;
	const FKismetAIInvokeResult Result = Worker.Get();
	TestTrue(TEXT("real Blueprint read succeeds"), Result.Typed.IsSet() && Result.Typed->bSuccess);
	TestEqual(TEXT("one admission despite multiple guard attempts"), *Admissions, 1);
	TestEqual(TEXT("one busy attempt and one admitted attempt"), Held.Dispatcher->GetStatsForTests().Completed, 2);
	Held.Release();
	TestEqual(TEXT("late pumping does not repeat admission"), *Admissions, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSaveReentrancy_BusyWorkerCancelAndClose,
	"KismetAI.EditorSafety.SaveReentrancy.BusyWorkerCancelAndClose", ReentrancyFlags)
bool FSaveReentrancy_BusyWorkerCancelAndClose::RunTest(const FString& Parameters)
{
	for (const bool bClose : { false, true })
	{
		FDisposableBlueprint Fixture;
		FHeldBusyDispatch Held;
		const auto Admissions = MakeShared<int32, ESPMode::ThreadSafe>(0);
		FKismetAIToolInvocation Call = BusyReadInvocation(Fixture, Admissions);
		const auto Invoker = MakeShared<FKismetAIModuleToolInvoker>(FKismetAIModule::Get(), Held.Dispatcher);
		TFuture<FKismetAIInvokeResult> Worker = Async(EAsyncExecution::ThreadPool, [Invoker, Call]() { return Invoker->Invoke(Call); });
		ON_SCOPE_EXIT { Held.Dispatcher->Close(); Held.Release(); Worker.Wait(); };
		if (!TestTrue(TEXT("initial callback queued"), Held.AwaitQueued(Worker))) return false;
		Held.ReleaseWhileBusy();
		if (!TestTrue(TEXT("retry callback queued"), Held.AwaitQueued(Worker))) return false;
		if (bClose) Held.Dispatcher->Close();
		else Call.CancelToken.Cancel();
		TestTrue(TEXT("cancel or close releases worker without callback"), Worker.WaitFor(FTimespan::FromSeconds(2.0)));
		if (!Worker.IsReady()) return false;
		const FKismetAIInvokeResult Result = Worker.Get();
		TestTrue(TEXT("invoker reports cancellation before start"), Result.bStoppedBeforeStart && Result.Typed.IsSet() && Result.Typed->ErrorCode == TEXT("CANCELLED"));
		Held.ReleaseWhileBusy();
		TestEqual(TEXT("cancellation has priority over busy and no late admission"), *Admissions, 0);

		FHeldBusyDispatch ExecutorHeld;
		const auto ExecutorInvoker = MakeShared<FKismetAIModuleToolInvoker>(FKismetAIModule::Get(), ExecutorHeld.Dispatcher);
		FToolCallContext Context;
		Context.ToolName = TEXT("read_blueprint");
		Context.Caller = FKismetAICallerContext::InProcess();
		Context.RequestBody = Call.RequestBody;
		Context.Arguments = Context.RequestBody;
		TFuture<FToolCallOutcome> ExecutorWorker = Async(EAsyncExecution::ThreadPool, [ExecutorInvoker, Context]()
		{
			FKismetAIToolExecutor Executor(*ExecutorInvoker);
			return Executor.Execute(Context);
		});
		ON_SCOPE_EXIT { ExecutorHeld.Dispatcher->Close(); ExecutorHeld.Release(); ExecutorWorker.Wait(); };
		if (!TestTrue(TEXT("executor call queued"), ExecutorHeld.AwaitQueued(ExecutorWorker))) return false;
		ExecutorHeld.ReleaseWhileBusy();
		if (!TestTrue(TEXT("executor retry queued"), ExecutorHeld.AwaitQueued(ExecutorWorker))) return false;
		if (bClose) ExecutorHeld.Dispatcher->Close();
		else Context.CancelToken.Cancel();
		if (!TestTrue(TEXT("executor cancellation completes without callback"), ExecutorWorker.WaitFor(FTimespan::FromSeconds(2.0)))) return false;
		const FToolCallOutcome ExecutorResult = ExecutorWorker.Get();
		TestTrue(TEXT("executor supplies typed nonexecution evidence"), ExecutorResult.bWasCancelled
			&& ExecutorResult.Typed.bHandlerNotStarted && ExecutorResult.Typed.ErrorCode == TEXT("CANCELLED"));
		ExecutorHeld.ReleaseWhileBusy();
		TestEqual(TEXT("late executor callback never starts"), ExecutorHeld.Dispatcher->GetStatsForTests().Started, 1);
		TestEqual(TEXT("executor queued retry abandoned once"), ExecutorHeld.Dispatcher->GetStatsForTests().Abandoned, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSaveReentrancy_BusyWorkerRechecksAuthority,
	"KismetAI.EditorSafety.SaveReentrancy.BusyWorkerRechecksAuthority", ReentrancyFlags)
bool FSaveReentrancy_BusyWorkerRechecksAuthority::RunTest(const FString& Parameters)
{
	FDisposableBlueprint Fixture;
	Fixture.Package->SetDirtyFlag(false);
	const FString LoadedPath = Fixture.Package->GetLoadedPath().GetDebugName();
	FHeldBusyDispatch Held;
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Mcp }, EKismetAIAuthorityOp::Edit, { *Fixture.Mount });
	const auto Invoker = MakeShared<FKismetAIModuleToolInvoker>(FKismetAIModule::Get(), Held.Dispatcher);
	FToolCallContext Call;
	Call.ToolName = TEXT("compile_blueprint");
	Call.Caller = FKismetAICallerContext::Verified(EKismetAIEntryPoint::Mcp, TEXT("busy-authority-test"));
	Call.RequestBody = MakeShared<FJsonObject>();
	Call.RequestBody->SetStringField(TEXT("blueprint"), Fixture.PackageName);
	Call.Arguments = Call.RequestBody;
	TFuture<FToolCallOutcome> Worker = Async(EAsyncExecution::ThreadPool, [Invoker, Call]()
	{
		FKismetAIToolExecutor Executor(*Invoker);
		return Executor.Execute(Call);
	});
	ON_SCOPE_EXIT { Held.Dispatcher->Close(); Held.Release(); Worker.Wait(); };
	if (!TestTrue(TEXT("authorized call queued"), Held.AwaitQueued(Worker))) return false;
	Held.ReleaseWhileBusy();
	if (!TestTrue(TEXT("busy call remains pending"), Held.AwaitQueued(Worker))) return false;
	Grant.Revoke();
	Held.Release();
	if (!TestTrue(TEXT("revoked call finishes"), Held.Finish(Worker))) return false;
	const FToolCallOutcome Result = Worker.Get();
	TestEqual(TEXT("authority rechecked after wait"), Result.Typed.ErrorCode, FString(TEXT("AUTHORITY_DENIED")));
	TestTrue(TEXT("handler did not start"), Result.Typed.bHandlerNotStarted);
	TestFalse(TEXT("no compile dirties the package"), Fixture.Package->IsDirty());
	TestEqual(TEXT("loaded path unchanged"), Fixture.Package->GetLoadedPath().GetDebugName(), LoadedPath);
	TestFalse(TEXT("no package saved"), FPackageName::DoesPackageExist(Fixture.PackageName));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSaveReentrancy_BusyWorkerRechecksRevision,
	"KismetAI.EditorSafety.SaveReentrancy.BusyWorkerRechecksRevision", ReentrancyFlags)
bool FSaveReentrancy_BusyWorkerRechecksRevision::RunTest(const FString& Parameters)
{
	FDisposableBlueprint Fixture;
	FHeldBusyDispatch Held;
	const auto Admissions = MakeShared<int32, ESPMode::ThreadSafe>(0);
	FKismetAIToolInvocation Call = BusyReadInvocation(Fixture, Admissions);
	const auto Invoker = MakeShared<FKismetAIModuleToolInvoker>(FKismetAIModule::Get(), Held.Dispatcher);
	const FKismetAIInvokeResult Read = Invoker->Invoke(Call);
	FString Revision;
	if (Read.Typed.IsSet() && Read.Typed->Data) Read.Typed->Data->TryGetStringField(TEXT("asset_revision"), Revision);
	if (!TestFalse(TEXT("initial read has revision"), Revision.IsEmpty())) return false;
	*Admissions = 0;
	Call.ToolName = TEXT("compile_blueprint");
	Call.Mode.Reset();
	Call.RequestBody->SetStringField(TEXT("expected_revision"), Revision);
	TFuture<FKismetAIInvokeResult> Worker = Async(EAsyncExecution::ThreadPool, [Invoker, Call]() { return Invoker->Invoke(Call); });
	ON_SCOPE_EXIT { Held.Dispatcher->Close(); Held.Release(); Worker.Wait(); };
	if (!TestTrue(TEXT("compile queued"), Held.AwaitQueued(Worker))) return false;
	Held.ReleaseWhileBusy();
	if (!TestTrue(TEXT("compile waits for safe admission"), Held.AwaitQueued(Worker))) return false;
	FEdGraphPinType Type;
	Type.PinCategory = TEXT("bool");
	FBlueprintEditorUtils::AddMemberVariable(Fixture.Blueprint, TEXT("ChangedWhileWaiting"), Type);
	Held.Release();
	if (!TestTrue(TEXT("stale call finishes"), Held.Finish(Worker))) return false;
	const FKismetAIInvokeResult Result = Worker.Get();
	TestTrue(TEXT("new revision refuses the stale compile"), Result.Typed.IsSet() && Result.Typed->ErrorCode == TEXT("ASSET_REVISION_CONFLICT"));
	TestEqual(TEXT("only final safe attempt reaches admission"), *Admissions, 1);
	TestTrue(TEXT("human variable retained"), Fixture.Blueprint->NewVariables.ContainsByPredicate([](const FBPVariableDescription& Var) { return Var.VarName == TEXT("ChangedWhileWaiting"); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSaveReentrancy_BusyWorkerTimeout,
	"KismetAI.EditorSafety.SaveReentrancy.BusyWorkerTimeout", ReentrancyFlags)
bool FSaveReentrancy_BusyWorkerTimeout::RunTest(const FString& Parameters)
{
	FDisposableBlueprint Fixture;
	FHeldBusyDispatch Held;
	const auto Admissions = MakeShared<int32, ESPMode::ThreadSafe>(0);
	const FKismetAIToolInvocation Call = BusyReadInvocation(Fixture, Admissions);
	const auto Invoker = MakeShared<FKismetAIModuleToolInvoker>(FKismetAIModule::Get(), Held.Dispatcher);
	TFuture<FKismetAIInvokeResult> Worker = Async(EAsyncExecution::ThreadPool, [Invoker, Call]() { return Invoker->Invoke(Call); });
	ON_SCOPE_EXIT { Held.Dispatcher->Close(); Held.Release(); Worker.Wait(); };
	if (!TestTrue(TEXT("first callback queued"), Held.AwaitQueued(Worker))) return false;
	const double Start = FPlatformTime::Seconds();
	while (!Worker.IsReady() && FPlatformTime::Seconds() - Start < 7.0)
	{
		Held.ReleaseWhileBusy();
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	}
	const double Elapsed = FPlatformTime::Seconds() - Start;
	if (!TestTrue(TEXT("pumped busy editor reaches bounded refusal"), Worker.IsReady())) return false;
	const FKismetAIInvokeResult Result = Worker.Get();
	TestTrue(TEXT("busy wait allows the specified five-second budget"), Elapsed >= 4.9);
	TestTrue(TEXT("busy wait terminates within pumping tolerance"), Elapsed < 6.5);
	TestTrue(TEXT("timeout reports busy before handler"), Result.Typed.IsSet() && Result.Typed->ErrorCode == TEXT("EDITOR_BUSY") && Result.Typed->bHandlerNotStarted);
	TestEqual(TEXT("persistent busy never reaches admission"), *Admissions, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSaveReentrancy_BusyRetryExpiresBeforeLateCallback,
	"KismetAI.EditorSafety.SaveReentrancy.BusyRetryExpiresBeforeLateCallback", ReentrancyFlags)
bool FSaveReentrancy_BusyRetryExpiresBeforeLateCallback::RunTest(const FString& Parameters)
{
	FDisposableBlueprint Fixture;
	FHeldBusyDispatch Held;
	const auto Admissions = MakeShared<int32, ESPMode::ThreadSafe>(0);
	const FKismetAIToolInvocation Call = BusyReadInvocation(Fixture, Admissions);
	const auto Invoker = MakeShared<FKismetAIModuleToolInvoker>(FKismetAIModule::Get(), Held.Dispatcher);
	TFuture<FKismetAIInvokeResult> Worker = Async(EAsyncExecution::ThreadPool, [Invoker, Call]() { return Invoker->Invoke(Call); });
	ON_SCOPE_EXIT { Held.Dispatcher->Close(); Held.Release(); Worker.Wait(); };
	if (!TestTrue(TEXT("first callback queued"), Held.AwaitQueued(Worker))) return false;
	Held.ReleaseWhileBusy();
	if (!TestTrue(TEXT("retry queued"), Held.AwaitQueued(Worker))) return false;
	const double Deadline = FPlatformTime::Seconds() + 5.1;
	while (FPlatformTime::Seconds() < Deadline)
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	}
	Held.Release();
	if (!TestTrue(TEXT("expired queued attempt returns"), Held.Finish(Worker))) return false;
	const FKismetAIInvokeResult Result = Worker.Get();
	TestTrue(TEXT("late callback reports not-started timeout"), Result.Typed.IsSet() && Result.Typed->ErrorCode == TEXT("EDITOR_BUSY") && Result.Typed->bHandlerNotStarted);
	TestEqual(TEXT("safe editor cannot admit an expired retry"), *Admissions, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSaveReentrancy_InlineAndNonGuardBusyNeverWait,
	"KismetAI.EditorSafety.SaveReentrancy.InlineAndNonGuardBusyNeverWait", ReentrancyFlags)
bool FSaveReentrancy_InlineAndNonGuardBusyNeverWait::RunTest(const FString& Parameters)
{
	FDisposableBlueprint Fixture;
	FHeldBusyDispatch Held;
	const auto Admissions = MakeShared<int32, ESPMode::ThreadSafe>(0);
	FKismetAIToolInvocation Call = BusyReadInvocation(Fixture, Admissions);
	const auto Invoker = MakeShared<FKismetAIModuleToolInvoker>(FKismetAIModule::Get(), Held.Dispatcher);
	const double Start = FPlatformTime::Seconds();
	{
		const bool bPrevious = GIsSlowTask;
		GIsSlowTask = true;
		ON_SCOPE_EXIT { GIsSlowTask = bPrevious; };
		const FKismetAIInvokeResult Result = Invoker->Invoke(Call);
		TestTrue(TEXT("inline busy returns refusal"), Result.Typed.IsSet() && Result.Typed->ErrorCode == TEXT("EDITOR_BUSY"));
	}
	TestTrue(TEXT("inline call never waits for its own game thread"), FPlatformTime::Seconds() - Start < 0.5);
	TestEqual(TEXT("inline busy consumes no admission"), *Admissions, 0);
	Call.Admit = [Admissions]() -> TOptional<FToolExecutionResult>
	{
		++*Admissions;
		return FToolExecutionResult::Error(503, TEXT("EDITOR_BUSY"), TEXT("Admission produced this code, not the guard."));
	};
	TFuture<FKismetAIInvokeResult> Worker = Async(EAsyncExecution::ThreadPool, [Invoker, Call]() { return Invoker->Invoke(Call); });
	ON_SCOPE_EXIT { Held.Dispatcher->Close(); Held.Release(); Worker.Wait(); };
	if (!TestTrue(TEXT("non-guard refusal completes"), Held.Finish(Worker))) return false;
	TestEqual(TEXT("admission-produced busy is not retried"), *Admissions, 1);
	TestEqual(TEXT("one worker dispatch only"), Held.Dispatcher->GetStatsForTests().Dispatched, 1);
	const FKismetAIInvokeResult Result = Worker.Get();
	TestTrue(TEXT("original refusal preserved"), Result.Typed.IsSet() && Result.Typed->ErrorCode == TEXT("EDITOR_BUSY"));
	int32 HandlerCalls = 0;
	auto Body = FKismetAIModule::MakeAdmittedWorkBody(FKismetAIModule::Get(), [&HandlerCalls]()
	{
		++HandlerCalls;
		return FToolExecutionResult::Error(503, TEXT("EDITOR_BUSY"), TEXT("Handler-produced busy."));
	}, {}, FKismetAICancellationToken(), FToolExecutionResult::Error(499, TEXT("CANCELLED"), TEXT("Cancelled")));
	const FKismetAIDispatchOutcome HandlerOutcome = Body();
	TestEqual(TEXT("handler returned its own error once"), HandlerCalls, 1);
	TestFalse(TEXT("handler error is not pre-handler evidence"), HandlerOutcome.Result.bHandlerNotStarted);
	return true;
}
