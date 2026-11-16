// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Recorded context-projection decisions and synthetic model-visible messages.
//
// Why: the model does not see the journal, it sees a projection of it. Before
// Two things changed that projection in memory only — compaction (the
// summary, the dropped span, the tool results microcompaction cleared) and the
// R6 premature-end-turn nudge — so a reopened session sent a different, larger
// request than the one the model last accepted, and a switch to a smaller
// model sent a request that could not fit. This header is the one definition
// of those changes, used by BOTH sides:
//   - the live loop (FKismetAIAgent) builds a decision as an FProjectionRecord,
//     applies it with Apply() and journals the same record;
//   - the reducer (KismetAIJournalReducer::Reduce) reads the record back and
//     applies it with the same Apply() to the same messages.
// The journal stays append-only and keeps every source event: a record adds a
// projection step on top of the canonical transcript, never replaces it.
//
// ── context_compaction, schema 2 ────────────────────────────────────────────
//   {type:"context_compaction", schema:2, uuid, timestamp, phase, run_id,
//    result:     outcome (status, failure_code, counts, ...),
//    projection: FProjectionRecord::ToJson()          (committed records only)
//    summary:    {model, provider, compact_prompt_sha1, transcript_messages,
//                 input_tokens, output_tokens, stop_reason}      (op summarize)
//    estimate:   {source:"estimated", method, tokens_before, tokens_after,
//                 effective_limit, context_window, output_reserve,
//                 overflow_threshold, last_reported_input_tokens}
//    config:     {model, provider}}
// Only committed decisions carry `projection`, so a failed attempt and a crash
// while the summary was being generated (no line at all) both leave the
// projection exactly as it was. Schema-1 lines carry no summary text
// and cannot be replayed: the reducer counts them and keeps the full history.
//
// ── synthetic_message, schema 1 ─────────────────────────────────────────────
//   {type:"synthetic_message", schema:1, uuid, timestamp, origin:"orchestrator",
//    kind, role:"user", content:<text>, run_id?, turn_id?, covers?[]}
// A model-visible message the runtime wrote, not the user. It replays as
// MakeUserTextMessage(content) at its journal position. Kinds below; a legacy
// `user_message` with `synthetic:true` (the older truncation nudge) is read
// as kind LegacySynthetic. The panel does not render these (the live run never
// showed them). Structurally they are still user turns, so the usual
// guarantees (never removed by pairing repair, kept verbatim by compaction)
// apply to them unchanged.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace KismetAIContextProjection
{
	inline constexpr const TCHAR* CompactionEntryType = TEXT("context_compaction");
	// schema of a context_compaction line that carries a replayable `projection`.
	inline constexpr int32 CompactionRecordSchema = 2;

	inline constexpr const TCHAR* SyntheticEntryType = TEXT("synthetic_message");
	inline constexpr int32 SyntheticRecordSchema = 1;

	namespace SyntheticKind
	{
		// Output hit max_tokens before any tool call; asks the model to finish.
		inline constexpr const TCHAR* OutputTruncationNudge = TEXT("output_truncation_nudge");
		// R6: a text-only turn described an action it did not take.
		inline constexpr const TCHAR* PrematureEndTurnNudge = TEXT("premature_end_turn_nudge");
		// Resume found records that could not be read; tells the model history may be missing.
		inline constexpr const TCHAR* JournalDamageNotice = TEXT("journal_damage_notice");
		// An older `user_message` flagged `synthetic`.
		inline constexpr const TCHAR* LegacySynthetic = TEXT("legacy_synthetic_user_message");
	}

	// ── Shared message construction ──────────────────────────────────────────

	// {role:"user", content:<Text>} — the shape of every runtime-written message.
	KISMETAIRUNTIME_API TSharedPtr<FJsonValue> MakeUserTextMessage(const FString& Text);

	// The text microcompaction leaves in place of a tool_result body.
	KISMETAIRUNTIME_API FString MakeClearedMarker(int32 OriginalChars);
	KISMETAIRUNTIME_API bool IsClearedMarker(const FString& Body);

	// A tool_result body as text, whatever shape the provider gave it.
	KISMETAIRUNTIME_API FString ToolResultBodyText(const TSharedPtr<FJsonObject>& Block);

	// Replace the body of every tool_result whose tool_use_id is listed with the
	// cleared marker. Unambiguously paired mutation calls also retain bounded,
	// explicitly unverified reported outcome fields; these are context hints,
	// never journal/ledger authority. Copy-on-write: changed blocks go into new message objects,
	// so other holders of the input objects keep the full bodies. Already-cleared
	// bodies are left alone. OutClearedBodies (first body per id wins) receives
	// the original text. Returns how many bodies were cleared.
	KISMETAIRUNTIME_API int32 ClearToolResults(TArray<TSharedPtr<FJsonValue>>& Messages, const TSet<FString>& ToolUseIds,
		TMap<FString, FString>* OutClearedBodies);

	// The structural identity of a projection: per message its role and the
	// tool_use / tool_result ids it carries (text, thinking and cleared bodies
	// do not count), hashed. A record applies only to a projection with the same
	// anchor and length as the one the live loop decided on.
	KISMETAIRUNTIME_API FString ComputeAnchor(const TArray<TSharedPtr<FJsonValue>>& Messages);

	// Messages[0, Head) + Summary + every genuine user message (in order, same
	// objects) of Messages[Head, Num - Tail) + Messages[Num - Tail, Num).
	KISMETAIRUNTIME_API TArray<TSharedPtr<FJsonValue>> BuildSummarizedProjection(const TArray<TSharedPtr<FJsonValue>>& Messages,
		int32 HeadCount, int32 TailCount, const TSharedPtr<FJsonValue>& SummaryMessage, int32* OutRetainedUserMessages = nullptr);

	// ── Records ───────────────────────────────────────────────────────────────

	enum class EProjectionOp : uint8
	{
		// Clear the listed tool-result bodies; no message is removed.
		Microcompact,
		// Clear the listed bodies, then replace Messages[Head, Num - Tail) by the
		// summary message followed by the genuine user messages of that span.
		Summarize,
	};

	struct KISMETAIRUNTIME_API FProjectionRecord
	{
		EProjectionOp Op = EProjectionOp::Microcompact;
		// Length and anchor of the projection the decision was made on.
		int32 MessagesBefore = 0;
		FString Anchor;
		int32 HeadMessages = 1;
		int32 TailMessages = 0;
		int32 RetainedUserMessages = 0;
		TArray<FString> ClearedToolUseIds;
		// The exact model-visible text of the summary message (op Summarize).
		FString SummaryMessageText;

		static const TCHAR* OpToString(EProjectionOp Op);
		TSharedPtr<FJsonObject> ToJson() const;
		// False (with a reason) for a record this build cannot apply.
		static bool FromJson(const TSharedPtr<FJsonObject>& Obj, FProjectionRecord& Out, FString& OutError);
	};

	enum class EApplyResult : uint8
	{
		Applied,
		// The projection is not the one the decision was made on; nothing changed.
		AnchorMismatch,
		// The record is inconsistent with the projection (bounds); nothing changed.
		Invalid,
	};

	// Apply a record, atomically: Messages changes only when the result is
	// Applied. The anchor and length are checked before anything is changed.
	KISMETAIRUNTIME_API EApplyResult Apply(const FProjectionRecord& Record, TArray<TSharedPtr<FJsonValue>>& Messages,
		TMap<FString, FString>* OutClearedBodies, FString* OutDetail = nullptr);

	// A synthetic_message journal entry (see header).
	KISMETAIRUNTIME_API TSharedPtr<FJsonObject> BuildSyntheticEntry(const FString& Kind, const FString& Text,
		const FString& RunId, const FString& TurnId);
}
