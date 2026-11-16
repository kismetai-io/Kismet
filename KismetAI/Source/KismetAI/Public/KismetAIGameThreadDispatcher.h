// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// The game-thread dispatcher.
//
// Every tool call made from a worker thread (the panel's agent run, an HTTP
// agent run) reaches the editor through here: the work is queued for the game
// thread and the worker waits for it. Previously, that wait was an untimed
// FEvent wait over a lambda that captured the worker's stack (Work, Admit and
// Result by reference) and raw module `this`; nothing could end the wait except
// the game thread running the callback. A Stop that arrived while the callback
// was queued therefore could not release the worker — and when the game thread
// was the one waiting for the worker (ShutdownModule's drain),
// both sat until the timeout and the worker was left parked on borrowed state.
//
// Ownership
// ---------
// One FJob per dispatched call, held by shared reference by exactly three
// parties: the waiting worker, the queued game-thread callback, and the
// cancellation listener registered on the call's token. Each can outlive the
// others; none borrows from another's stack. The job owns:
//   - the body (the module's guarded entry -> admission -> handler sequence,
//     which owns its own copies of the work and admission functions),
//   - the "cancelled before start" result to report if it never starts,
//   - the outcome, and one manual-reset completion event (returned to the
//     pool in the job's destructor, i.e. after the last owner lets go).
// The dispatcher's shared state (closed flag, scheduler, in-flight list,
// counters) is a separate shared object, so a job never needs the dispatcher —
// or the module — to finish.
//
// State machine (all transitions under the job's lock)
// ---------------------------------------------------
//   Queued  -> Running    the callback starts on the game thread
//   Queued  -> Abandoned  the call's token is cancelled, or the dispatcher is
//                         closed, before the callback starts
//   Running -> Completed  the body returned
// Completed and Abandoned are terminal; the completion event is triggered on
// that one transition, so it fires exactly once per job whatever mix of
// success, refusal, cancellation, duplicate Stop and teardown happens. The
// worker waits once, with no timeout and no polling: a terminal transition is
// guaranteed because a queued job is abandoned by cancellation/Close and a
// running job runs to completion on the game thread.
//
// A Running job is never abandoned. That is deliberate: the handler is a native
// Unreal operation that cannot be preempted, so a waiter released early could
// only report a guess ("uncertain") while the edit lands anyway. Instead the
// worker keeps waiting (the game thread is busy with exactly that work, so the
// wait is bounded by it) and reports the real outcome with
// bCancelRequestedWhileRunning. It is also what keeps the admission function's
// borrowed policy pointer valid: Admit only runs inside a Running body, and the
// executor that owns the default policy is still inside Invoke then.
//
// The body runs on the game thread only after Queued -> Running succeeds, so
// once Close() has returned (on the game thread) no queued body will ever touch
// the module again; late callbacks find the job Abandoned and drop it.

#pragma once

#include "CoreMinimal.h"
#include "KismetAICancellation.h"
#include "KismetAIToolResult.h"

// What one dispatched call reports.
struct FKismetAIDispatchOutcome
{
	FToolExecutionResult Result;

	// The handler did not run and never will: abandoned while queued, refused
	// at callback entry because the token was cancelled, or refused because
	// the dispatcher was closed. Result is the call's CancelledBeforeStart.
	bool bStoppedBeforeStart = false;

	// Set only by the module's editor-state guard, before policy admission or
	// handler entry. Workers may defer this refusal; tool errors cannot opt in.
	bool bEditorBusyBeforeAdmission = false;

	// The token was cancelled after admission while the handler ran. Result is
	// the handler's real result; nothing was preempted or undone.
	bool bCancelRequestedWhileRunning = false;
};

class FKismetAIGameThreadDispatcher
{
public:
	// Posts a callback to the game thread. The default is
	// AsyncTask(ENamedThreads::GameThread, ...).
	using FScheduler = TFunction<void(TFunction<void()>)>;

	// The game-thread side of one call. Must check the token itself at entry
	// (the module's body does: entry check, editor guard, Admit, handler).
	using FBody = TFunction<FKismetAIDispatchOutcome()>;

	FKismetAIGameThreadDispatcher();
	~FKismetAIGameThreadDispatcher();

	// Run Body on the game thread and return its outcome. From a worker thread
	// this queues and waits as described above; on the game thread it runs
	// Body inline (waiting there for the game thread would deadlock).
	//
	// Token cancelled before the callback starts -> returns at once with
	// { CancelledBeforeStart, bStoppedBeforeStart }, and Body never runs.
	FKismetAIDispatchOutcome Dispatch(FBody Body, const FKismetAICancellationToken& Token,
		const FToolExecutionResult& CancelledBeforeStart);

	// Teardown (game thread): refuse every later Dispatch, abandon every queued
	// job and wake its worker. Running jobs (only possible if Close is reached
	// from inside a handler that pumps the game thread) finish normally.
	// Idempotent.
	void Close();
	bool IsClosed() const;

	// Tests only: hold callbacks instead of posting them, to force an
	// interleaving deterministically. nullptr restores the default.
	void SetSchedulerForTests(FScheduler InScheduler);

	struct FStats
	{
		int32 Dispatched = 0;  // worker-thread dispatches that created a job
		int32 Started = 0;     // Queued -> Running
		int32 Completed = 0;   // Running -> Completed (completion signals)
		int32 Abandoned = 0;   // Queued -> Abandoned (completion signals)
		int32 InFlight = 0;    // jobs a worker is still waiting on
	};
	FStats GetStatsForTests() const;

private:
	struct FJob;
	struct FShared;
	TSharedRef<FShared, ESPMode::ThreadSafe> Shared;
};
