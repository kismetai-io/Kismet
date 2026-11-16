// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAISessionController.h"
#include "KismetAIAgent.h"
#include "KismetAIGameThreadDispatcher.h"
#include "Async/Async.h"
#include "HAL/CriticalSection.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"

DEFINE_LOG_CATEGORY_STATIC(LogKismetAISession, Log, All);

const TCHAR* LexToString(EKismetAIRunOrigin Origin)
{
	switch (Origin)
	{
	case EKismetAIRunOrigin::Panel: return TEXT("panel");
	case EKismetAIRunOrigin::HttpAgentRun: return TEXT("agent_run");
	}
	return TEXT("unknown");
}

const TCHAR* LexToString(EKismetAIRunState State)
{
	switch (State)
	{
	case EKismetAIRunState::Admitted: return TEXT("admitted");
	case EKismetAIRunState::Running: return TEXT("running");
	case EKismetAIRunState::Exited: return TEXT("exited");
	}
	return TEXT("unknown");
}

// What workers and the controller share. A worker holds this by reference, so
// it outlives the controller (and the module) if a worker does.
struct FKismetAISessionController::FShared
{
	enum class EState : uint8 { Open, Closing, Closed };

	mutable FCriticalSection Mutex;
	EState State = EState::Open;
	FKismetAIRunPtr LeaseHolder;
	// Every run with a worker inside its body, leased or not.
	TArray<FKismetAIRunRef> LiveWorkers;
};

FKismetAIRun::FKismetAIRun(EKismetAIRunOrigin InOrigin, TSharedPtr<FKismetAIAgent, ESPMode::ThreadSafe> InAgent)
	: RunId(FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower))
	, Origin(InOrigin)
	, Agent(MoveTemp(InAgent))
	, ExitedEvent(FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/true))
{
}

FKismetAIRun::~FKismetAIRun()
{
	FPlatformProcess::ReturnSynchEventToPool(ExitedEvent);
}

namespace
{
	FKismetAIRunStatus MakeStatus(const FKismetAIRun& Run, EKismetAIRunState State, bool bLeaseHeld, bool bCancelRequested)
	{
		FKismetAIRunStatus Status;
		Status.RunId = Run.GetRunId();
		Status.Origin = Run.GetOrigin();
		Status.State = State;
		Status.bLeaseHeld = bLeaseHeld;
		Status.bCancelRequested = bCancelRequested;
		return Status;
	}
}

FKismetAISessionController::FKismetAISessionController()
	: Shared(MakeShared<FShared, ESPMode::ThreadSafe>())
{
}

FKismetAISessionController::~FKismetAISessionController()
{
	// Normal teardown went through Shutdown. If it did not (a test, or a
	// module that never started), still refuse later admission: a worker
	// holding Shared must not see an open controller that no longer exists.
	FScopeLock Lock(&Shared->Mutex);
	Shared->State = FShared::EState::Closed;
	if (Shared->LeaseHolder.IsValid())
	{
		Shared->LeaseHolder->bLeaseHeld = false;
		Shared->LeaseHolder.Reset();
	}
}

FKismetAIAdmission FKismetAISessionController::TryAdmit(EKismetAIRunOrigin Origin,
	TSharedRef<FKismetAIAgent, ESPMode::ThreadSafe> Agent)
{
	FKismetAIAdmission Out;
	FScopeLock Lock(&Shared->Mutex);
	if (Shared->State != FShared::EState::Open)
	{
		Out.Refusal = EKismetAIAdmissionRefusal::Closed;
		return Out;
	}
	if (Shared->LeaseHolder.IsValid())
	{
		Out.Refusal = EKismetAIAdmissionRefusal::Busy;
		Out.ActiveRunId = Shared->LeaseHolder->RunId;
		Out.ActiveOrigin = Shared->LeaseHolder->Origin;
		return Out;
	}
	Agent->PrepareRunInputs();
	FKismetAIRunRef Run = MakeShareable(new FKismetAIRun(Origin, Agent));
	Shared->LeaseHolder = Run;
	Out.Run = Run;
	return Out;
}

bool FKismetAISessionController::BeginWorker(const FKismetAIRunRef& Run)
{
	FScopeLock Lock(&Shared->Mutex);
	if (Run->State != EKismetAIRunState::Admitted)
	{
		return false;   // one worker per run
	}
	if (Shared->State != FShared::EState::Open)
	{
		// Admission closed between TryAdmit and here: the body never runs.
		Run->State = EKismetAIRunState::Exited;
		Run->Agent.Reset();
		Run->ExitedEvent->Trigger();
		return false;
	}
	Run->State = EKismetAIRunState::Running;
	Shared->LiveWorkers.Add(Run);
	return true;
}

void FKismetAISessionController::FinishWorker(const TSharedRef<FShared, ESPMode::ThreadSafe>& InShared, const FKismetAIRunRef& Run)
{
	// Drop the run's agent reference outside the lock: if it is the last one,
	// the agent (its invoker, its model client) is destroyed here, on the
	// worker, before the run is declared exited.
	TSharedPtr<FKismetAIAgent, ESPMode::ThreadSafe> Dropped;
	{
		FScopeLock Lock(&InShared->Mutex);
		Dropped = MoveTemp(Run->Agent);
		Run->Agent.Reset();
	}
	Dropped.Reset();

	FScopeLock Lock(&InShared->Mutex);
	Run->State = EKismetAIRunState::Exited;
	InShared->LiveWorkers.RemoveAll([&Run](const FKismetAIRunRef& R) { return R == Run; });
	Run->ExitedEvent->Trigger();
}

bool FKismetAISessionController::StartWorker(const FKismetAIRunRef& Run, TFunction<void()> Body)
{
	if (!BeginWorker(Run))
	{
		return false;
	}
	// The worker owns Shared, Run and Body. Body is destroyed (releasing what it
	// captured) before FinishWorker declares the run exited.
	Async(EAsyncExecution::ThreadPool, [InShared = Shared, Run, Body = MoveTemp(Body)]() mutable
	{
		Body();
		Body = nullptr;
		FinishWorker(InShared, Run);
	});
	return true;
}

void FKismetAISessionController::Release(const FKismetAIRunRef& Run)
{
	FScopeLock Lock(&Shared->Mutex);
	Run->bLeaseHeld = false;
	if (Shared->LeaseHolder == Run)
	{
		Shared->LeaseHolder.Reset();
	}
}

bool FKismetAISessionController::IsRunning() const
{
	FScopeLock Lock(&Shared->Mutex);
	return Shared->LeaseHolder.IsValid();
}

TOptional<FKismetAIRunStatus> FKismetAISessionController::GetActiveRun() const
{
	FScopeLock Lock(&Shared->Mutex);
	if (!Shared->LeaseHolder.IsValid())
	{
		return {};
	}
	const FKismetAIRun& Run = *Shared->LeaseHolder;
	return MakeStatus(Run, Run.State, Run.bLeaseHeld, Run.bCancelRequested);
}

TOptional<FKismetAIRunStatus> FKismetAISessionController::GetRunStatus(const FKismetAIRunRef& Run) const
{
	FScopeLock Lock(&Shared->Mutex);
	return MakeStatus(*Run, Run->State, Run->bLeaseHeld, Run->bCancelRequested);
}

int32 FKismetAISessionController::NumLiveWorkers() const
{
	FScopeLock Lock(&Shared->Mutex);
	return Shared->LiveWorkers.Num();
}

bool FKismetAISessionController::WaitForWorkerExit(const FKismetAIRunRef& Run, uint32 TimeoutMs) const
{
	return Run->ExitedEvent->Wait(TimeoutMs);
}

bool FKismetAISessionController::Cancel(const FKismetAIRunRef& Run)
{
	TSharedPtr<FKismetAIAgent, ESPMode::ThreadSafe> Agent;
	{
		FScopeLock Lock(&Shared->Mutex);
		if (Run->State == EKismetAIRunState::Exited)
		{
			return false;
		}
		Run->bCancelRequested = true;
		Agent = Run->Agent;
	}
	// Outside the lock: Abort cancels the run token, whose listeners (a queued
	// dispatch job) run synchronously here, kills the model call and wakes an
	// ask_user wait. The agent records the Stop even if its worker has not
	// reached RunTask yet.
	if (Agent.IsValid())
	{
		Agent->Abort();
	}
	return true;
}

bool FKismetAISessionController::CancelActive()
{
	FKismetAIRunPtr Active;
	{
		FScopeLock Lock(&Shared->Mutex);
		Active = Shared->LeaseHolder;
	}
	return Active.IsValid() && Cancel(Active.ToSharedRef());
}

bool FKismetAISessionController::SubmitAnswer(const FString& QuestionId, const FString& AnswersJson,
	EKismetAIEntryPoint AnsweredVia)
{
	TSharedPtr<FKismetAIAgent, ESPMode::ThreadSafe> Agent;
	{
		FScopeLock Lock(&Shared->Mutex);
		const FKismetAIRunPtr& Active = Shared->LeaseHolder;
		if (!Active.IsValid() || Active->bCancelRequested || Active->State != EKismetAIRunState::Running)
		{
			return false;
		}
		Agent = Active->Agent;
	}
	return Agent.IsValid() && Agent->SubmitAnswer(QuestionId, AnswersJson, AnsweredVia);
}

bool FKismetAISessionController::IsClosed() const
{
	FScopeLock Lock(&Shared->Mutex);
	return Shared->State != FShared::EState::Open;
}

FKismetAISessionController::FDrainReport FKismetAISessionController::Shutdown(
	FKismetAIGameThreadDispatcher* Dispatcher, uint32 TimeoutMs)
{
	FDrainReport Report;

	// 1. Close admission and take the set of runs to settle, noting which had
	// a worker at this moment (those are the ones to drain).
	TArray<FKismetAIRunRef> Runs;
	TSet<FKismetAIRun*> HadWorker;
	{
		FScopeLock Lock(&Shared->Mutex);
		if (Shared->State != FShared::EState::Open)
		{
			Report.bAlreadyClosed = true;
		}
		Shared->State = FShared::EState::Closing;
		Runs = Shared->LiveWorkers;
		if (Shared->LeaseHolder.IsValid())
		{
			Runs.AddUnique(Shared->LeaseHolder.ToSharedRef());
		}
		for (const FKismetAIRunRef& Run : Runs)
		{
			if (Run->State == EKismetAIRunState::Running)
			{
				HadWorker.Add(&Run.Get());
			}
		}
	}

	// 2. Abandon queued editor callbacks and refuse later ones. A
	// worker parked on one is woken with a not-started result.
	if (Dispatcher)
	{
		Dispatcher->Close();
	}

	// 3. Cancel every run.
	for (const FKismetAIRunRef& Run : Runs)
	{
		if (Cancel(Run))
		{
			++Report.Cancelled;
		}
	}

	// 4. Drain every worker against one deadline. Nothing here needs the game
	// thread: queued callbacks were abandoned, the model call was killed and
	// the answer wait was woken.
	const double Deadline = FPlatformTime::Seconds() + TimeoutMs / 1000.0;
	for (const FKismetAIRunRef& Run : Runs)
	{
		{
			FScopeLock Lock(&Shared->Mutex);
			if (Run->State == EKismetAIRunState::Admitted)
			{
				// Admitted, never started: it never will be.
				Run->State = EKismetAIRunState::Exited;
				Run->Agent.Reset();
				Run->ExitedEvent->Trigger();
			}
		}
		if (!HadWorker.Contains(&Run.Get()))
		{
			continue;
		}
		const double Remaining = FMath::Max(0.0, Deadline - FPlatformTime::Seconds());
		if (Run->ExitedEvent->Wait(static_cast<uint32>(Remaining * 1000.0)))
		{
			++Report.Drained;
		}
		else
		{
			Report.UndrainedRunIds.Add(Run->RunId);
		}
	}

	{
		FScopeLock Lock(&Shared->Mutex);
		Shared->State = FShared::EState::Closed;
		if (Shared->LeaseHolder.IsValid())
		{
			Shared->LeaseHolder->bLeaseHeld = false;
			Shared->LeaseHolder.Reset();
		}
	}

	if (!Report.AllDrained())
	{
		UE_LOG(LogKismetAISession, Error,
			TEXT("%d agent worker(s) did not exit within %ums of cancellation (runs: %s). They keep shared ownership of the agent, the dispatcher and the tool policy; no module state is referenced."),
			Report.UndrainedRunIds.Num(), TimeoutMs, *FString::Join(Report.UndrainedRunIds, TEXT(", ")));
	}
	return Report;
}
