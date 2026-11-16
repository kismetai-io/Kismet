// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Per-call tool outcome records in the session journal.
//
// Why: the loop used to journal the assistant's tool_use batch, run every call
// keeping the results in a local array, and write one tool_result_message after
// the whole batch. A Stop between call A and call B returned from the loop
// before that write, so A's real outcome (an asset already changed) never
// reached the journal and a resumed session had no record that A ran.
//
// Now each call's terminal outcome is appended as soon as the call returns,
// before the next call starts, and calls a cancellation prevents from starting
// are recorded as not executed. A failed append stops the batch: no further
// call (and so no further mutation) runs without a durable record.
//
// ── Identities ────────────────────────────────────────────────────────────────
//   run_id   a GUID minted per RunTask invocation (one user request).
//   turn_id  "<run_id>/t<N>", N = the zero-based model turn within that run.
//   call_id  "<turn_id>/c<I>", I = the call's zero-based position in the
//            assistant message's tool_use blocks.
// All three are strings written into the journal; nothing recomputes them on
// reload, so they survive a restart. The provider's tool_use id is kept
// verbatim (`tool_use_id`, and inside the result block) — it is the pairing key
// providers require; call_id is ours and never sent to a provider.
//
// ── Journal shape (schema "tool_call/1") ─────────────────────────────────────
// 1. The batch's assistant_message is stamped before any call runs:
//      {"type":"assistant_message", ..., "content":[...tool_use...],
//       "run_id":R, "turn_id":T, "tool_call_count":N}
// 2. One line per call, in call order, appended when that call is terminal:
//      {"type":"tool_result_message", "record":"tool_call", "schema":1,
//       "uuid":..., "timestamp":..., "run_id":R, "turn_id":T, "call_id":C,
//       "call_index":I, "call_count":N, "tool_use_id":ID, "tool_name":NAME,
//       "is_mutation":bool,
//       "status":"succeeded"|"failed"|"short_circuited"|"not_started",
//       "attempted":bool,              // handed to the executor; side effects possible
//       "not_started_reason":"cancelled",          // only when status == not_started
//       "mutation_outcome":{...},      // FKismetAIMutationOutcome::ToJson, when the result carried one
//       "results":[ {"type":"tool_result","tool_use_id":ID,"content":"...","is_error":bool} ]}
//    `type` and `results` deliberately match the legacy batch entry so the
//    panel's replay (ui.html) and any reader that knew tool_result_message keep
//    working; `record` is what marks the per-call form.
// 3. Optionally, one guidance line after the last call of a completed batch:
//      {"type":"tool_result_message", "record":"turn_note", "run_id":R,
//       "turn_id":T, "results":[{"type":"text","text":"[orchestrator] ..."}]}
//
// ── Reload ───────────────────────────────────────────────────────────────────
// FResumeProjector folds every tool_call / turn_note record that shares the
// stamped assistant message's turn_id into ONE user message, ordered by the
// assistant's tool_use blocks, so the provider sees exactly one tool_result per
// tool_use. A stamped call with no record (crash, or a failed append) gets an
// explicit OUTCOME_UNRECORDED error result: it never becomes a success, a
// rollback or permission to replay. Legacy journals (no turn_id) take the old
// path unchanged.
//
// ── What this does NOT guarantee ─────────────────────────────────────────────
// There is a window between a tool's side effect and its record reaching disk
// (see the comment at the append site in KismetAIAgent.cpp). A crash there
// leaves the call unrecorded; reload reports it as OUTCOME_UNRECORDED. No
// exactly-once or idempotent execution is claimed; reconciling asset state
// against the journal is separate.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace KismetAIToolCallJournal
{
	// entry["record"] values.
	inline constexpr const TCHAR* RecordToolCall = TEXT("tool_call");
	inline constexpr const TCHAR* RecordTurnNote = TEXT("turn_note");
	inline constexpr int32 SchemaVersion = 1;

	// Error codes of the synthetic results this journal writes or projects.
	inline constexpr const TCHAR* NotExecutedCode = TEXT("NOT_EXECUTED");
	inline constexpr const TCHAR* OutcomeUnrecordedCode = TEXT("OUTCOME_UNRECORDED");
	// Several calls of one turn share a tool_use id and a recorded result
	// cannot be attributed to one of them.
	inline constexpr const TCHAR* OutcomeAmbiguousCode = TEXT("OUTCOME_AMBIGUOUS");

	enum class ECallStatus : uint8
	{
		// Attempted; the result carried no error marker.
		Succeeded,
		// Attempted; the result is an error. Side effects are still possible
		// (a partially applied batch says so in its mutation_outcome).
		Failed,
		// A loop-health guard answered instead of the tool. Nothing ran.
		ShortCircuited,
		// Never handed to the tool. Nothing ran.
		NotStarted,
	};

	KISMETAIRUNTIME_API const TCHAR* StatusToString(ECallStatus Status);

	KISMETAIRUNTIME_API FString MakeRunId();
	KISMETAIRUNTIME_API FString MakeTurnId(const FString& RunId, int32 TurnIndex);
	KISMETAIRUNTIME_API FString MakeCallId(const FString& TurnId, int32 CallIndex);

	// Model-call identities, nested under the turn like call ids.
	//   request  "<turn>/r<n>"     one CallWithRetry (a turn makes a second one
	//                              only after compacting on prompt_too_long)
	//   attempt  "<request>/a<k>"  one transport attempt inside that request
	//   message  "<turn>/m"        the assistant message the turn commits; every
	//                              attempt of every request in the turn is a
	//                              candidate for it, and at most one becomes it
	KISMETAIRUNTIME_API FString MakeRequestId(const FString& TurnId, int32 RequestIndex);
	KISMETAIRUNTIME_API FString MakeAttemptId(const FString& RequestId, int32 AttemptIndex);
	KISMETAIRUNTIME_API FString MakeMessageId(const FString& TurnId);

	struct KISMETAIRUNTIME_API FCallRecord
	{
		FString RunId;
		FString TurnId;
		int32 CallIndex = 0;
		int32 CallCount = 0;
		FString ToolUseId;
		FString ToolName;
		bool bIsMutation = false;
		ECallStatus Status = ECallStatus::NotStarted;
		// Present when the tool result carried an outcome.
		TSharedPtr<FJsonObject> MutationOutcome;
		// The exact tool_result block the model sees.
		TSharedPtr<FJsonObject> ResultBlock;
		// Stop arrived while this call's handler was running. Status is
		// still the handler's real outcome (succeeded / failed), never
		// not_started; written as "cancel_requested_while_running": true.
		bool bCancelRequestedWhileRunning = false;

		bool WasAttempted() const
		{
			return Status == ECallStatus::Succeeded || Status == ECallStatus::Failed;
		}
	};

	// Add run_id / turn_id / tool_call_count to the batch's assistant_message entry.
	KISMETAIRUNTIME_API void StampToolBatch(const TSharedPtr<FJsonObject>& AssistantEntry,
		const FString& RunId, const FString& TurnId, int32 CallCount);

	KISMETAIRUNTIME_API TSharedPtr<FJsonObject> BuildCallEntry(const FCallRecord& Record);
	KISMETAIRUNTIME_API TSharedPtr<FJsonObject> BuildNoteEntry(const FString& RunId, const FString& TurnId,
		const TArray<TSharedPtr<FJsonValue>>& Blocks);

	// tool_result for a call that never started because the run was cancelled.
	KISMETAIRUNTIME_API TSharedPtr<FJsonObject> MakeNotStartedResultBlock(const FString& ToolUseId, const FString& ToolName);

	// tool_result projected on reload for a stamped call with no record. A
	// non-empty ReconciliationNote is appended to the message: what the
	// asset looked like compared with the call's recorded before-state.
	KISMETAIRUNTIME_API TSharedPtr<FJsonObject> MakeUnrecordedResultBlock(const FString& ToolUseId, const FString& ToolName,
		const FString& ReconciliationNote = FString());

	// tool_result for a call whose id is shared with other calls of the same turn
	// when no recorded result can be attributed to it. RecordedEvidence,
	// when non-empty, is the text of results recorded under the shared id; it is
	// quoted as unattributed evidence, never presented as this call's outcome.
	KISMETAIRUNTIME_API TSharedPtr<FJsonObject> MakeAmbiguousResultBlock(const FString& ToolUseId, const FString& ToolName,
		const FString& SharedId, int32 CallsSharingId, const TArray<FString>& RecordedEvidence);

	// A tool_result block's content as text (string content verbatim, block-array
	// content as condensed JSON).
	KISMETAIRUNTIME_API FString ResultContentText(const TSharedPtr<FJsonObject>& ResultBlock);

	// The deterministic replacement id for a tool_use whose id is already taken:
	// "<Id>_dup<N>" with the smallest N >= 1 not in Taken. Stays inside the
	// [A-Za-z0-9_-] alphabet providers accept for tool ids when Id does.
	KISMETAIRUNTIME_API FString MakeDuplicateIdAlias(const FString& Id, const TSet<FString>& Taken);

	// The executor's own pre-dispatch cancellation result (FKismetAIToolExecutor
	// step 3): the call was cancelled before it reached the tool.
	KISMETAIRUNTIME_API bool IsExecutorCancelledResult(const FString& Result);

	// Folds per-call records back into provider-shaped history on reload. The
	// journal reducer (KismetAIJournalReducer::Reduce) drives it; now that
	// is the only reader, used by LoadSession / LoadMessages and RunTask's resume:
	//   - BeforeMessageEntry() before any other entry that produces a message;
	//   - OnAssistantAppended() after appending an assistant message;
	//   - TryConsumeToolResultEntry() for each tool_result_message — false means
	//     "not part of the open batch", and the caller handles it as before;
	//   - Finish() after the last entry.
	//
	// When one assistant message carries the same tool_use id more than
	// once, records are matched by call_index (exact), the second and later
	// occurrences are renamed with MakeDuplicateIdAlias in both the projected
	// assistant message and its result, and a record that cannot be matched by
	// index is never handed to one of the colliding calls.
	class KISMETAIRUNTIME_API FResumeProjector
	{
	public:
		void BeforeMessageEntry(TArray<TSharedPtr<FJsonValue>>& Messages);
		void OnAssistantAppended(const TSharedPtr<FJsonObject>& Entry, TArray<TSharedPtr<FJsonValue>>& Messages);
		bool TryConsumeToolResultEntry(const TSharedPtr<FJsonObject>& Entry, TArray<TSharedPtr<FJsonValue>>& Messages);
		void Finish(TArray<TSharedPtr<FJsonValue>>& Messages);

		// Stamped calls that had no record at all on this load.
		int32 GetUnrecordedCallCount() const { return UnrecordedCalls; }

		// First indexed terminal record accepted for each stamped call, in journal
		// order. Identity and input come from the assistant's committed batch;
		// outcome fields come from that call's accepted record, never model prose.
		const TArray<TSharedPtr<FJsonObject>>& GetCommittedCalls() const { return CommittedCalls; }

		// Each unrecorded call, in journal order, with its arguments.
		struct FUnrecordedCall
		{
			FString TurnId;
			int32 CallIndex = 0;
			FString ToolUseId;     // as projected (alias when renamed)
			FString ToolName;
			TSharedPtr<FJsonObject> Input;
		};
		const TArray<FUnrecordedCall>& GetUnrecordedCalls() const { return UnrecordedCallList; }

		// Reconciliation notes by call_id, appended to the projected
		// OUTCOME_UNRECORDED result of that call. Set before the first entry.
		void SetReconciliationNotes(TMap<FString, FString> InNotes) { ReconciliationNotes = MoveTemp(InNotes); }

	private:
		void Close(TArray<TSharedPtr<FJsonValue>>& Messages);

		bool bOpen = false;
		FString OpenTurnId;
		int32 AssistantIndex = INDEX_NONE;                       // the projected assistant message in Messages
		TArray<TPair<FString, FString>> ExpectedCalls;          // (tool_use_id, tool_name) in order
		TArray<TSharedPtr<FJsonObject>> ExpectedInputs;         // the tool_use input, same order
		TMap<FString, TSharedPtr<FJsonValue>> Received;         // first record per tool_use_id wins
		TMap<int32, TSharedPtr<FJsonValue>> ReceivedByIndex;    // by the record's call_index, when it names that call
		TArray<TSharedPtr<FJsonValue>> Notes;
		TArray<TSharedPtr<FJsonObject>> CommittedCalls;
		int32 UnrecordedCalls = 0;
		TArray<FUnrecordedCall> UnrecordedCallList;
		TMap<FString, FString> ReconciliationNotes;
	};
}
