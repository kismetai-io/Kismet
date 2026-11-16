// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// The one owner of agent-run lifecycle.
//
// Previously, the panel launch, the HTTP /agent/run handler and module
// shutdown each held their own piece of a run: a "running" bool and a panel
// agent slot the module mutated through references, an HTTP agent slot behind
// its own mutex, and a shutdown that aborted every agent but waited only for
// the first, with a timeout after which the module was destroyed under any
// worker still holding it. Admission, cancel, answers and drain now go through
// this type, and only through its operations.
//
// Lease (admission rule)
// ----------------------
// One run holds the editor's execution lease at a time, whatever started it.
// The lease covers the whole turn: model calls, tool calls, ask_user and
// approval waits, and the panel's game-thread finalization (which may hand the
// lease straight to the next queued message). A second run is never admitted
// alongside it:
//   - TryAdmit returns Busy. The panel queues its message and starts it when
//     the lease is released; /agent/run answers 409 agent_busy and changes
//     nothing.
//   - After Shutdown begins, TryAdmit returns Closed.
// The module has one current session id and one diff state; two runs at once
// corrupted both, and their tool calls could interleave on the same asset.
//
// Run state machine (one FKismetAIRun, all fields under the controller lock)
// --------------------------------------------------------------------------
//   Admitted --StartWorker--> Running --body returns--> Exited
//   Admitted --StartWorker refused (closed)-----------------------> Exited
// orthogonal to that:
//   lease:  held from TryAdmit until Release (or Shutdown)
//   cancel: requested once by Cancel/CancelActive/Shutdown; idempotent
// A worker leaves Running only after its body returned AND everything the
// body captured (the agent, its invoker, its model client) was released, so
// "Exited" means the worker holds nothing. Release may come before Exited:
// the panel's finalization is posted from inside the body.
//
// Controller state: Open -> Closing (Shutdown: admission closed, cancelling,
// draining) -> Closed. Never reopens.
//
// Ownership
// ---------
// Everything a worker touches is shared, never borrowed from the module: the
// run record and the controller's shared state (this file), the agent and its
// invoker, which holds the game-thread dispatcher and the tool policy by
// shared reference (KismetAIModuleToolInvoker.h). So a worker that outlives a
// timed-out Shutdown, and the module object after it, still touches only
// memory it co-owns. Code is kept mapped by SupportsDynamicReloading() ==
// false on the module.
//
// Threading: every operation is safe from any thread. TryAdmit, StartWorker,
// Release and Shutdown are called on the game thread in production. Panel and
// HTTP runs both run on StartWorker workers.

#pragma once

#include "CoreMinimal.h"
#include "KismetAIAgent.h"
#include "KismetAIMessageQueue.h"

class FKismetAIGameThreadDispatcher;

enum class EKismetAIRunOrigin : uint8
{
	Panel,
	HttpAgentRun,
};

const TCHAR* LexToString(EKismetAIRunOrigin Origin);

enum class EKismetAIRunState : uint8
{
	Admitted,   // holds the lease, no worker yet
	Running,    // a worker is inside the body
	Exited,     // the worker returned and released what it held (or never started)
};

const TCHAR* LexToString(EKismetAIRunState State);

enum class EKismetAIAdmissionRefusal : uint8
{
	None,
	Busy,     // another run holds the lease
	Closed,   // Shutdown has begun
};

// A snapshot of one run, for callers and tests. The HTTP run status (
// KismetAIExternalRuns.h) reports it as `worker`.
struct FKismetAIRunStatus
{
	FString RunId;
	EKismetAIRunOrigin Origin = EKismetAIRunOrigin::Panel;
	EKismetAIRunState State = EKismetAIRunState::Admitted;
	bool bLeaseHeld = false;
	bool bCancelRequested = false;
};

class FKismetAIRun;
using FKismetAIRunRef = TSharedRef<FKismetAIRun, ESPMode::ThreadSafe>;
using FKismetAIRunPtr = TSharedPtr<FKismetAIRun, ESPMode::ThreadSafe>;

// One admitted run. Opaque outside the controller; hold it to Release it.
class FKismetAIRun
{
public:
	~FKismetAIRun();
	const FString& GetRunId() const { return RunId; }
	EKismetAIRunOrigin GetOrigin() const { return Origin; }

private:
	friend class FKismetAISessionController;
	FKismetAIRun(EKismetAIRunOrigin InOrigin, TSharedPtr<FKismetAIAgent, ESPMode::ThreadSafe> InAgent);

	const FString RunId;
	const EKismetAIRunOrigin Origin;

	// Guarded by the controller lock.
	TSharedPtr<FKismetAIAgent, ESPMode::ThreadSafe> Agent;   // reset when the worker exits
	EKismetAIRunState State = EKismetAIRunState::Admitted;
	bool bLeaseHeld = true;
	bool bCancelRequested = false;

	// Manual reset; triggered once, on the transition to Exited.
	FEvent* ExitedEvent = nullptr;
};

struct FKismetAIAdmission
{
	FKismetAIRunPtr Run;
	EKismetAIAdmissionRefusal Refusal = EKismetAIAdmissionRefusal::None;
	// When Busy: who holds the lease.
	FString ActiveRunId;
	EKismetAIRunOrigin ActiveOrigin = EKismetAIRunOrigin::Panel;

	bool IsAdmitted() const { return Run.IsValid(); }
};

class FKismetAISessionController
{
public:
	FKismetAISessionController();
	~FKismetAISessionController();

	// --- admission and lease ---------------------------------------------

	// Take the lease for a run driven by Agent. Refused while another run
	// holds it or once Shutdown has begun; nothing is changed on refusal.
	FKismetAIAdmission TryAdmit(EKismetAIRunOrigin Origin, TSharedRef<FKismetAIAgent, ESPMode::ThreadSafe> Agent);

	// Run Body on a pool thread as this run's worker. Refused (returns false,
	// Body is dropped unrun, the run goes to Exited) after Shutdown began or if
	// the run already has or had a worker.
	bool StartWorker(const FKismetAIRunRef& Run, TFunction<void()> Body);

	// Give the lease back. Idempotent. Does not wait for the worker.
	void Release(const FKismetAIRunRef& Run);

	// True while some run holds the lease.
	bool IsRunning() const;
	TOptional<FKismetAIRunStatus> GetActiveRun() const;
	TOptional<FKismetAIRunStatus> GetRunStatus(const FKismetAIRunRef& Run) const;
	// Workers still inside a body, leased or not.
	int32 NumLiveWorkers() const;
	// Block until Run's worker has exited (or never had one and never will),
	// up to TimeoutMs. True when it has. Never call it on the game thread for a
	// run whose worker may be waiting on a game-thread callback that is not
	// cancelled.
	bool WaitForWorkerExit(const FKismetAIRunRef& Run, uint32 TimeoutMs) const;

	// --- cancel and answers -----------------------------------------------

	// Stop the lease holder, whatever started it. Never blocks. Returns false
	// when no run holds the lease.
	bool CancelActive();
	bool Cancel(const FKismetAIRunRef& Run);

	// Deliver an ask_user/approval answer to the lease holder only. A run that
	// was cancelled, has exited or no longer holds the lease never receives
	// one, and the agent refuses an id it is not waiting on.
	bool SubmitAnswer(const FString& QuestionId, const FString& AnswersJson,
		EKismetAIEntryPoint AnsweredVia = EKismetAIEntryPoint::Unspecified);

	// --- teardown ---------------------------------------------------------

	struct FDrainReport
	{
		int32 Cancelled = 0;            // runs Shutdown cancelled
		int32 Drained = 0;              // workers that exited before the deadline
		TArray<FString> UndrainedRunIds; // workers still running at the deadline
		bool bAlreadyClosed = false;
		bool AllDrained() const { return UndrainedRunIds.Num() == 0; }
	};

	// Module teardown, on the game thread, in this order:
	//   1. close admission (TryAdmit -> Closed, StartWorker refused);
	//   2. close the dispatcher, abandoning queued editor callbacks and waking
	//      their workers;
	//   3. cancel every run (model call killed, ask_user/approval woken, token
	//      cancelled so later dispatches are refused);
	//   4. wait for every worker, leased or not, up to TimeoutMs in total.
	// It never needs the game thread to run anything while it waits. On
	// timeout it returns the undrained runs; they keep shared ownership of
	// everything they reach (see Ownership above). Idempotent.
	FDrainReport Shutdown(FKismetAIGameThreadDispatcher* Dispatcher, uint32 TimeoutMs);
	bool IsClosed() const;

	// --- session identity -------------------------------------------------

	const FString& GetSessionId() const { return SessionId; }
	void SetSessionId(const FString& InSessionId) { SessionId = InSessionId; }

	// --- queue and steering -----------------------------------------------

	FKismetAIMessageQueue& Queue() { return MessageQueue; }
	const FKismetAIMessageQueue& Queue() const { return MessageQueue; }

private:
	struct FShared;
	TSharedRef<FShared, ESPMode::ThreadSafe> Shared;

	bool BeginWorker(const FKismetAIRunRef& Run);
	static void FinishWorker(const TSharedRef<FShared, ESPMode::ThreadSafe>& InShared, const FKismetAIRunRef& Run);

	// Game thread only (panel/UI state).
	FString SessionId;
	FKismetAIMessageQueue MessageQueue;
};
