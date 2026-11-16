// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// tool_use / tool_result pairing repair that can never delete the user's words.
//
// Why: providers reject a request in which an assistant tool_use has no
// tool_result in the very next message, a tool_result answers no tool_use, or a
// tool_use id repeats. The old repair (ValidateToolPairing) made such a history
// valid by DELETING messages: an assistant batch with missing results was
// removed together with "the next user message". On resume RunTask appended the
// new instruction before that repair ran, so after an interrupted batch the
// "next user message" was the instruction itself — "Stop editing; only explain"
// stayed in the JSONL and never reached the model.
//
// ── The invariant ────────────────────────────────────────────────────────────
// Every message is classified explicitly (EMessageKind). A user message with no
// tool_result block is a GenuineUser message — typed input, a queued/steered
// instruction, an attachment, or an orchestrator nudge — and the repair never
// removes, reorders, merges or edits one: the output contains the same objects,
// in the same order. RepairToolPairing checks this after every pass and reports
// a violation as an error rather than trusting the construction.
//
// Only ToolResultEnvelope messages (user role, at least one tool_result block)
// and AssistantToolBatch messages are ever rewritten, and always into new
// objects: the input's JSON objects are never mutated.
//
// ── Missing results are reported, not invented ───────────────────────────────
// A tool_use with no recorded result keeps its place in history and is answered
// with OUTCOME_UNRECORDED (is_error): it may or may not have run. If no envelope
// follows the batch at all, one is inserted directly after it (before any
// genuine user message). Nothing is ever turned into a success, a rollback or
// permission to replay.
//
// ── Duplicate tool_use ids ───────────────────────────────────────────────────
//  - A later assistant turn whose content is identical to an earlier kept turn
//    is a re-persisted copy (double-written journal line): it and the results in
//    its own adjacent envelope are dropped. Genuine user messages after it stay.
//  - A later turn that reuses an id with different content is a real turn (some
//    OpenAI-compatible hosts number calls per response): the colliding ids are
//    renamed with KismetAIToolCallJournal::MakeDuplicateIdAlias in that turn and
//    in its own adjacent envelope, so each result stays with the turn it
//    followed.
//  - Several calls of ONE turn sharing an id: a result recorded under that id
//    cannot be attributed, so every one of those calls gets OUTCOME_AMBIGUOUS
//    (is_error) quoting what was recorded, and the second and later calls are
//    renamed. Stamped journals avoid the ambiguity: FResumeProjector
//    pairs those records by call_index before this repair sees them.
//
// ── What this is not ─────────────────────────────────────────────────────────
// A projection over in-memory history. It never touches the journal file, which
// stays the canonical record. It does not decide what to compact or trim
// (TrimConversation does; now it holds the same invariant, keeping every
// genuine user message from the span it summarizes verbatim and checking that
// before it commits); it only makes whatever remains valid without removing
// user input.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "KismetAIToolCallJournal.h"

namespace KismetAIHistoryRepair
{
	inline constexpr const TCHAR* OutcomeAmbiguousCode = KismetAIToolCallJournal::OutcomeAmbiguousCode;

	enum class EMessageKind : uint8
	{
		// System messages, malformed entries, assistant text-only turns.
		Other,
		// role "user" with no tool_result block: protected, never rewritten.
		GenuineUser,
		// role "user" with at least one tool_result block.
		ToolResultEnvelope,
		// role "assistant" with at least one tool_use block that has an id.
		AssistantToolBatch,
	};

	KISMETAIRUNTIME_API EMessageKind Classify(const TSharedPtr<FJsonValue>& Message);

	inline bool IsGenuineUserMessage(const TSharedPtr<FJsonValue>& Message)
	{
		return Classify(Message) == EMessageKind::GenuineUser;
	}

	struct KISMETAIRUNTIME_API FRepairReport
	{
		// OUTCOME_UNRECORDED results added for calls with no recorded result.
		int32 UnrecordedResults = 0;
		// Envelopes inserted because none followed a batch.
		int32 InsertedEnvelopes = 0;
		// OUTCOME_AMBIGUOUS results for calls sharing an id within one turn.
		int32 AmbiguousResults = 0;
		// tool_use ids renamed to a _dup alias.
		int32 RenamedToolUseIds = 0;
		// Re-persisted identical assistant turns dropped.
		int32 DroppedDuplicateTurns = 0;
		// tool_result blocks that answered no tool_use right before them.
		int32 DroppedOrphanResults = 0;
		// Second and later results for the same (unique) call; the first stands.
		int32 DroppedExtraResults = 0;
		// Genuine user messages in the input (all of them are in the output).
		int32 GenuineUserMessages = 0;
		// False only if the preservation invariant was violated (a bug).
		bool bGenuineUserMessagesPreserved = true;

		bool HasChanges() const;
		FString ToString() const;
		TSharedPtr<FJsonObject> ToJson() const;
	};

	// Make Messages provider-valid without removing a genuine user message.
	// Idempotent: a second call on its own output changes nothing.
	KISMETAIRUNTIME_API FRepairReport RepairToolPairing(TArray<TSharedPtr<FJsonValue>>& Messages);
}
