// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Attempt-aware streamed events.
//
// A model request (one CallWithRetry) can make several transport attempts. Each
// attempt may stream text and thinking before it fails, and the next attempt
// streams through the same callbacks. Previously, the agent kept one
// text_start..text_end segment for the whole request, so the panel showed a
// failed attempt's "use A" and the retry's "use B" as one answer while the
// journal kept only "use B".
//
// ── Event protocol (additive; older consumers ignore the new fields/types) ──
//   attempt_start  run_id, turn_id, request_id, attempt_id, message_id,
//                  attempt (0-based within the request), replaces (the
//                  attempt_id this one retries; absent on the first)
//   text_start / text_delta / text_end / thinking_delta / thinking
//                  unchanged fields plus attempt_id and message_id. A text
//                  segment never spans two attempts: text_end closes it when
//                  the attempt ends, whatever the outcome.
//   attempt_end    identity as attempt_start, plus
//                    status      completed | interrupted | failed | aborted
//                    will_retry  true only with interrupted
//                    error_code, message, http_status   (not completed)
//                    text, thinking   what was streamed (not completed)
//                  completed: this attempt's content is the turn's message
//                  (its assistant_message record carries the same attempt_id
//                  and message_id). Anything else: what it streamed is not
//                  committed history and must not be shown as an answer.
//   Every streamed text/thinking payload is provisional until its attempt_end.
//
// ── Journal ──────────────────────────────────────────────────────────────────
//   model_attempt (schema 1): one record per attempt that did not complete,
//   written when it ends, with the attempt_end fields. Not conversation: the
//   reducer ignores it, so resume never sees failed text; the panel's replay
//   renders it as the live run did. A committed assistant_message is stamped
//   with request_id / attempt_id / message_id.
//
// All payloads go through the JSON serializer. Nothing is escaped by hand.

#pragma once

#include "CoreMinimal.h"
#include "KismetAIAgent.h"
#include "KismetAIModelClient.h"
#include "Dom/JsonObject.h"

namespace KismetAIStreamEvents
{
	inline constexpr const TCHAR* AttemptEntryType = TEXT("model_attempt");
	inline constexpr int32 AttemptEntrySchema = 1;

	enum class EAttemptStatus : uint8
	{
		Completed,    // the attempt's response is the turn's message
		Interrupted,  // failed, and the request makes another attempt
		Failed,       // failed, and it ends the request
		Aborted,      // the user stopped the run while it was in flight
	};

	const TCHAR* StatusToString(EAttemptStatus Status);

	// Condensed serializer output: one line, every control character escaped.
	FString ToJson(const TSharedPtr<FJsonObject>& Obj);
}

// Owns the streamed events of ONE model request and the attempts inside it.
// Lives on the agent's worker thread, like the callbacks that feed it.
class FKismetAIModelRequestStream
{
public:
	using FJournalAppend = TFunction<bool(const TSharedPtr<FJsonObject>& Entry)>;

	FKismetAIModelRequestStream(const FOnAgentStep& InStepCallback, FJournalAppend InJournalAppend,
		const FString& InRunId, const FString& InTurnId, int32 InRequestIndex);

	~FKismetAIModelRequestStream();
	FKismetAIModelRequestStream(const FKismetAIModelRequestStream&) = delete;
	FKismetAIModelRequestStream& operator=(const FKismetAIModelRequestStream&) = delete;

	// Retained callbacks become inert when the request closes or is destroyed.
	FModelCallbacks MakeCallbacks();

	// Opens the attempt (idempotent for the one already open). The caller opens
	// attempt 0 before the call so every request has a start, whatever client.
	void OpenAttempt(int32 AttemptIndex);

	void OnTextDelta(const FString& Text);
	void OnThinkingDelta(const FString& Text);
	void OnThinkingComplete(const FString& Text);

	// The retry loop gave up on an attempt and will make another one.
	void OnAttemptRetrying(int32 AttemptIndex, const FModelCallOutcome& Failed);

	// Close the attempt that ended the request, once the caller has judged the
	// outcome. Revokes callbacks even when no attempt is open.
	void CloseFinal(KismetAIStreamEvents::EAttemptStatus Status, const FModelCallOutcome& Outcome);

	// Identity of the committed message, for its assistant_message record.
	void StampCommit(const TSharedPtr<FJsonObject>& Entry) const;

	const FString& GetRequestId() const { return RequestId; }
	const FString& GetMessageId() const { return MessageId; }

private:
	struct FCallbackLifetime;
	TSharedRef<FCallbackLifetime, ESPMode::ThreadSafe> CallbackLifetime;
	void EnsureOpen();
	void CloseAttempt(KismetAIStreamEvents::EAttemptStatus Status, const FModelCallOutcome* Outcome, bool bWillRetry);
	void OnAttemptCompleted(const FModelCallOutcome& Outcome);
	TSharedPtr<FJsonObject> MakeIdentity(const TCHAR* Type) const;
	TSharedPtr<FJsonObject> MakeContentEvent(const TCHAR* Type) const;
	void Emit(const TCHAR* Type, const TSharedPtr<FJsonObject>& Payload) const;

	FOnAgentStep StepCallback;
	FJournalAppend JournalAppend;
	FString RunId;
	FString TurnId;
	FString RequestId;
	FString MessageId;

	bool bFinalClosed = false;
	bool bOpen = false;
	int32 AttemptIndex = INDEX_NONE;
	FString AttemptId;
	FString PreviousAttemptId;
	FString CompletedAttemptId;
	double AttemptStartedAt = 0;
	TSharedPtr<FJsonObject> AttemptDiagnostics;
	bool bTextStarted = false;
	FString AttemptText;
	FString AttemptThinking;       // completed thinking blocks of this attempt
	FString PendingThinkingBlock;  // deltas of the block still streaming
};
