// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIGameThreadDispatcher.h"
#include "Containers/Ticker.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"

struct FKismetAIGameThreadDispatcher::FShared
{
	mutable FCriticalSection Mutex;
	bool bClosed = false;
	FScheduler SchedulerOverride;
	TArray<TSharedRef<FJob, ESPMode::ThreadSafe>> InFlight;
	FStats Stats;
};

struct FKismetAIGameThreadDispatcher::FJob
{
	enum class EState : uint8 { Queued, Running, Completed, Abandoned };

	FJob(FBody InBody, const FToolExecutionResult& InCancelledBeforeStart, TSharedRef<FShared, ESPMode::ThreadSafe> InShared)
		: Body(MoveTemp(InBody))
		, CancelledBeforeStart(InCancelledBeforeStart)
		, Shared(MoveTemp(InShared))
		, DoneEvent(FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/true))
	{
	}

	~FJob()
	{
		FPlatformProcess::ReturnSynchEventToPool(DoneEvent);
	}

	// Queued -> Abandoned. From the cancellation listener (any thread) or
	// Close (game thread). A no-op once the job has started or finished.
	bool TryAbandon()
	{
		FBody Dropped;
		{
			FScopeLock Lock(&Mutex);
			if (State != EState::Queued)
			{
				return false;
			}
			State = EState::Abandoned;
			Outcome.Result = CancelledBeforeStart;
			Outcome.bStoppedBeforeStart = true;
			Dropped = MoveTemp(Body);
			Body = nullptr;
		}
		{
			FScopeLock Lock(&Shared->Mutex);
			++Shared->Stats.Abandoned;
		}
		DoneEvent->Trigger();
		return true;   // Dropped (the module's body and its captures) is released here
	}

	// The game-thread callback: Queued -> Running -> Completed, or nothing.
	void RunOnGameThread()
	{
		check(IsInGameThread());
		FBody ToRun;
		{
			FScopeLock Lock(&Mutex);
			if (State != EState::Queued)
			{
				return;   // abandoned: the worker has already been answered
			}
			State = EState::Running;
			ToRun = MoveTemp(Body);
			Body = nullptr;
		}
		{
			FScopeLock Lock(&Shared->Mutex);
			++Shared->Stats.Started;
		}

		FKismetAIDispatchOutcome Result = ToRun ? ToRun() : FKismetAIDispatchOutcome();
		ToRun = nullptr;

		{
			FScopeLock Lock(&Mutex);
			Outcome = MoveTemp(Result);
			State = EState::Completed;
		}
		{
			FScopeLock Lock(&Shared->Mutex);
			++Shared->Stats.Completed;
		}
		DoneEvent->Trigger();
	}

	FKismetAIDispatchOutcome TakeOutcome()
	{
		FScopeLock Lock(&Mutex);
		check(State == EState::Completed || State == EState::Abandoned);
		return Outcome;
	}

	FCriticalSection Mutex;
	EState State = EState::Queued;
	FBody Body;
	const FToolExecutionResult CancelledBeforeStart;
	FKismetAIDispatchOutcome Outcome;
	TSharedRef<FShared, ESPMode::ThreadSafe> Shared;
	FEvent* DoneEvent;
};

FKismetAIGameThreadDispatcher::FKismetAIGameThreadDispatcher()
	: Shared(MakeShared<FShared, ESPMode::ThreadSafe>())
{
}

FKismetAIGameThreadDispatcher::~FKismetAIGameThreadDispatcher()
{
	Close();
}

FKismetAIDispatchOutcome FKismetAIGameThreadDispatcher::Dispatch(FBody Body, const FKismetAICancellationToken& Token,
	const FToolExecutionResult& CancelledBeforeStart)
{
	auto StoppedBeforeStart = [&CancelledBeforeStart]()
	{
		FKismetAIDispatchOutcome Out;
		Out.Result = CancelledBeforeStart;
		Out.bStoppedBeforeStart = true;
		return Out;
	};

	if (IsInGameThread())
	{
		// Inline: there is no queue to abandon. Body checks the token at entry.
		if (IsClosed())
		{
			return StoppedBeforeStart();
		}
		return Body ? Body() : FKismetAIDispatchOutcome();
	}

	TSharedRef<FJob, ESPMode::ThreadSafe> Job = MakeShared<FJob, ESPMode::ThreadSafe>(MoveTemp(Body), CancelledBeforeStart, Shared);

	FScheduler Scheduler;
	{
		FScopeLock Lock(&Shared->Mutex);
		if (Shared->bClosed)
		{
			// Admission is closed (teardown). Same Mutex as Close, so a job is
			// either refused here or visible to Close — never neither.
			return StoppedBeforeStart();
		}
		Shared->InFlight.Add(Job);
		++Shared->Stats.Dispatched;
		Scheduler = Shared->SchedulerOverride;
	}

	// Cancellation wakes the waiter by abandoning the queued job. The listener
	// owns a job reference: it may still be running on the cancelling thread
	// after this worker has unsubscribed and returned.
	const uint64 ListenerId = Token.Subscribe([Job]() { Job->TryAbandon(); });

	// Already cancelled (Subscribe ran the listener) — nothing to post.
	bool bQueued = false;
	{
		FScopeLock Lock(&Job->Mutex);
		bQueued = Job->State == FJob::EState::Queued;
	}
	if (bQueued)
	{
		TFunction<void()> Callback = [Job]() { Job->RunOnGameThread(); };
		if (Scheduler)
		{
			Scheduler(MoveTemp(Callback));
		}
		else
		{
			// TaskGraph would install the worker's inherited context on the game
			// thread, including an absent/stale FAppTime. Run at the normal frame
			// ticker instead so renderer descendants inherit this frame's time.
			// AddTicker is thread-safe; an abandoned job remains a harmless no-op.
			FTSTicker::GetCoreTicker().AddTicker(TEXT("KismetAI.EditorDispatch"), 0.0f,
				[Callback = MoveTemp(Callback)](float) mutable
				{
					Callback();
					return false;
				});
		}
	}

	// One wait, no timeout, no polling: the job reaches a terminal state either
	// on the game thread (Completed) or through cancellation / Close
	// (Abandoned), and the event is triggered exactly once on that transition.
	Job->DoneEvent->Wait();

	Token.Unsubscribe(ListenerId);
	{
		FScopeLock Lock(&Shared->Mutex);
		Shared->InFlight.Remove(Job);
	}
	return Job->TakeOutcome();
}

void FKismetAIGameThreadDispatcher::Close()
{
	TArray<TSharedRef<FJob, ESPMode::ThreadSafe>> ToAbandon;
	{
		FScopeLock Lock(&Shared->Mutex);
		Shared->bClosed = true;
		ToAbandon = Shared->InFlight;
	}
	for (const TSharedRef<FJob, ESPMode::ThreadSafe>& Job : ToAbandon)
	{
		Job->TryAbandon();
	}
}

bool FKismetAIGameThreadDispatcher::IsClosed() const
{
	FScopeLock Lock(&Shared->Mutex);
	return Shared->bClosed;
}

void FKismetAIGameThreadDispatcher::SetSchedulerForTests(FScheduler InScheduler)
{
	FScopeLock Lock(&Shared->Mutex);
	Shared->SchedulerOverride = MoveTemp(InScheduler);
}

FKismetAIGameThreadDispatcher::FStats FKismetAIGameThreadDispatcher::GetStatsForTests() const
{
	FScopeLock Lock(&Shared->Mutex);
	FStats Out = Shared->Stats;
	Out.InFlight = Shared->InFlight.Num();
	return Out;
}
