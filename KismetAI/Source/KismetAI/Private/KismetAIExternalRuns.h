// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// External agent runs as addressable resources.
//
// Previously, POST /api/v1/agent/run ran the whole agent loop inline on the
// game thread inside the HTTP request, collected every event in a local array
// and sent them as one text/event-stream body when the run was over. A client
// saw nothing until the end, could not ask what the run was doing, could not
// stop it, and after a client timeout could not tell which edits had been
// applied: retrying started a second run.
//
// Now a run started over HTTP is a record in FKismetAIExternalRunRegistry,
// keyed by the session controller's run id (which is also the journal
// run id and the run_id in every event). The run executes on a controller
// worker; this record is what the HTTP routes read:
//
//   - an ordered event log with monotonic sequence numbers (cursors), bounded
//     in count and bytes; once the oldest events are dropped a cursor before
//     them is reported as expired, never silently skipped;
//   - progress counters, the current phase and the last event;
//   - exactly one terminal outcome, set when the run's finalization has run
//     (the lease is released by then), carrying the terminal event and the
//     Mutation-outcome ledger;
//   - cancel bookkeeping (when, and in which phase it was caught).
//
// The record is plain Core/Json state behind a mutex: the agent's step
// callback appends from the worker thread, HTTP handlers read on the game
// thread. Nothing here touches a UObject.

#pragma once

#include "CoreMinimal.h"
#include "KismetAISessionController.h"
#include "Dom/JsonObject.h"
#include "HAL/CriticalSection.h"

namespace KismetAIExternalRuns
{
	// The SSE `event:` name for an agent step type. Unchanged from the
	// older body so existing parsers keep working: done/error/plan_ready
	// and text_* keep their own name, everything else is "step". The run's
	// own first event is "run".
	FString SseEventNameFor(const FString& Type);

	// One SSE frame: `id: <seq>\nevent: <name>\ndata: <json>\n\n`.
	FString FormatSseFrame(int64 Seq, const FString& Name, const FString& Data);

	// Stable fingerprint of what a submit asked for, so a repeated
	// client_request_id can be told apart from a reused one.
	FString FingerprintRequest(const FString& Input, const FString& SessionId, const FString& Blueprint, int32 MaxSteps);
}

struct FKismetAIRunEvent
{
	int64 Seq = 0;
	FString Type;   // the agent's step type (tool_call, done, ...) or "run"
	FString Name;   // SSE event name
	FString Data;   // JSON object text
	FDateTime At;
};

struct FKismetAIRunEventPage
{
	TArray<FKismetAIRunEvent> Events;
	// The requested cursor is older than the oldest retained event: events
	// after it were dropped. Events is empty when set.
	bool bExpired = false;
	int64 FirstRetainedSeq = 0;   // 0 when nothing is retained
	int64 LastSeq = 0;            // last event ever appended (0: none)
	int64 NextAfter = 0;          // pass as `after` to continue
	bool bFinished = false;       // the run has its terminal outcome
	bool bMore = false;           // a limit cut the page short
};

class FKismetAIExternalRunRecord
{
public:
	struct FLimits
	{
		int32 MaxEvents = 20000;
		int64 MaxBytes = 4 * 1024 * 1024;
	};

	enum class ECancelAck : uint8
	{
		Requested,          // first cancel; the run was still going
		AlreadyRequested,   // an earlier cancel is still being honoured
		AlreadyFinished,    // the run already has its terminal outcome
	};

	struct FCancelResult
	{
		ECancelAck Ack = ECancelAck::Requested;
		FString PhaseAtRequest;
	};

	FKismetAIExternalRunRecord(const FString& InRunId, const FString& InClientRequestId,
		const FString& InFingerprint, const FString& InSessionId, FLimits InLimits);

	// Thread-safe. Called from the agent's step callback (worker thread).
	void Append(const FString& Type, const FString& JsonData);

	// The controller run this record follows, for cancel and worker state.
	// Dropped when the run finishes.
	void SetControllerRun(const FKismetAIRunPtr& InRun);
	FKismetAIRunPtr GetControllerRun() const;

	void SetFinalText(const FString& Text);
	void SetSessionId(const FString& InSessionId);

	// Record a cancel request. Does not cancel anything itself.
	FCancelResult NoteCancel();

	// The run's single terminal outcome. The first call wins; later calls
	// are ignored. FallbackStatus is used only when the agent emitted neither
	// `done` nor a run-ending `error` (for example a worker refused at
	// shutdown: "not_started").
	void Finish(const FString& FallbackStatus);

	FKismetAIRunEventPage Read(int64 After, int32 Limit) const;

	bool IsFinished() const;
	FDateTime GetFinishedAt() const;
	FDateTime GetCreatedAt() const { return CreatedAt; }
	const FString& GetRunId() const { return RunId; }
	const FString& GetClientRequestId() const { return ClientRequestId; }
	const FString& GetFingerprint() const { return Fingerprint; }
	FString GetPhase() const;

	// Status resource (GET /api/v1/agent/runs/status). Worker carries the
	// controller's view when the run is still known to it.
	TSharedRef<FJsonObject> BuildStatusJson(const TOptional<FKismetAIRunStatus>& Worker) const;

private:
	void AppendLocked(const FString& Type, const FString& JsonData);
	void TrimLocked();
	FString PhaseLocked() const;

	const FString RunId;
	const FString ClientRequestId;
	const FString Fingerprint;
	const FLimits Limits;
	const FDateTime CreatedAt;

	mutable FCriticalSection Mutex;
	FString SessionId;
	FKismetAIRunPtr ControllerRun;

	TArray<FKismetAIRunEvent> Events;   // ascending Seq, contiguous
	int64 RetainedBytes = 0;
	int64 NextSeq = 1;
	int64 DroppedEvents = 0;

	// Progress.
	int32 ModelAttempts = 0;
	int32 ModelRequests = 0;
	int32 ToolCalls = 0;
	int32 ToolResults = 0;
	int32 ToolErrors = 0;
	int32 Questions = 0;
	FString CurrentTool;
	FString PhaseHint = TEXT("starting");
	FString LastEventType;
	FDateTime LastEventAt;

	// Cancel.
	bool bCancelRequested = false;
	FDateTime CancelRequestedAt;
	FString CancelPhase;

	// Terminal.
	bool bTerminalEventSeen = false;
	FString TerminalEvent;      // done | error
	FString TerminalStatus;     // done.status, or "error"
	TSharedPtr<FJsonObject> TerminalData;
	TSharedPtr<FJsonObject> MutationOutcomes;
	FString FinalText;
	bool bFinished = false;
	FDateTime FinishedAt;
	FString OutcomeStatus;
};

using FKismetAIExternalRunRef = TSharedRef<FKismetAIExternalRunRecord, ESPMode::ThreadSafe>;
using FKismetAIExternalRunPtr = TSharedPtr<FKismetAIExternalRunRecord, ESPMode::ThreadSafe>;

// Every external run the editor still remembers. Active runs are never
// evicted; finished ones are kept for FinishedTtl and at most MaxFinishedRuns
// (oldest finished first), so a client can poll the outcome after a
// disconnect without the editor growing without bound.
class FKismetAIExternalRunRegistry
{
public:
	struct FLimits
	{
		int32 MaxFinishedRuns = 16;
		FTimespan FinishedTtl = FTimespan::FromHours(1.0);
	};

	FKismetAIExternalRunRegistry() = default;
	explicit FKismetAIExternalRunRegistry(FLimits InLimits) : Limits(InLimits) {}

	void Add(const FKismetAIExternalRunRef& Record);
	FKismetAIExternalRunPtr Find(const FString& RunId) const;
	FKismetAIExternalRunPtr FindByClientRequestId(const FString& ClientRequestId) const;

	// Evict finished runs past the limits. Now is injectable for tests.
	void Prune(const FDateTime& Now);
	void Prune() { Prune(FDateTime::UtcNow()); }
	int32 Num() const;

private:
	FLimits Limits;
	mutable FCriticalSection Mutex;
	TArray<FKismetAIExternalRunRef> Records;   // insertion order
};
