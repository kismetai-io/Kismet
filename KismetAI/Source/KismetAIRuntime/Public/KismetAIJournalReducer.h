// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// The one session-journal reader.
//
// A session journal is append-only JSONL under Saved/KismetAI/sessions. This
// file is the only code that decides what a journal means: every production
// path that turns one back into a conversation — FKismetAISessionStorage::
// LoadSession / LoadMessages, RunTask's resume, the panel's replay entries —
// goes through ParseJournalBytes and Reduce below. Previously, production
// resume used a second, hand-written reader inside LoadMessages, so tests of
// this reducer said nothing about what a resumed user actually got.
//
// ── Format version ───────────────────────────────────────────────────────────
// `session_start.v` is the journal format version. It changes only for an
// incompatible change; a new record type is additive (older readers ignore an
// unknown `type`), so the projection records do not need a bump.
//   absent header  legacy (pre-header) journal; read as version 1.
//   v <= 1         supported.
//   v  > 1         or not a number: UnsupportedVersion. The entries this build
//                  understands are still reduced for inspection, but CanResume()
//                  is false and RunTask refuses to append to the file. Nothing is
//                  migrated or rewritten, ever: not at startup, not on load.
// Per-record schemas: tool_call / turn_note records (`schema`) newer than
// KismetAIToolCallJournal::SchemaVersion are not interpreted; the call they
// would have answered is projected as OUTCOME_UNRECORDED, with a diagnostic.
//
// ── Supported variants ───────────────────────────────────────────────────────
//   user_message           string content, optional `images`, optional
//                          `synthetic` (older truncation nudge; still a
//                          user turn, reported as a legacy synthetic message).
//   assistant_message      verbatim provider block array — thinking (with
//                          signature), redacted_thinking, text, tool_use and any
//                          block or field this build does not know, all kept.
//                          Optional stamp (run_id / turn_id) and
//                          stamp (request_id / attempt_id / message_id).
//   tool_result_message    legacy batch form (`results`, no `record`) and the
//                          Per-call `tool_call` / `turn_note` records.
//   synthetic_message runtime-written model-visible message with
//                          `kind` and provenance; projected as a user turn.
//   context_compaction     schema 2 with `projection`: replayed onto
//                          FSessionSnapshot::Context (KismetAIContextProjection.h);
//                          schema 1 and failed attempts: metadata only.
//   session_start, session_meta, token_stats, error, history_repair,
//   journal_seal, journal_recovery, model_attempt (a model attempt
//   that did not become the turn's message, kept for display) and unknown
//   types:                 not conversation; ignored by the projection.
//   mutation_intent, mutation_reconciliation: not conversation;
//                          indexed by call_id into UncertainCalls /
//                          Reconciliations, and a reconciliation's note is
//                          appended to its call's OUTCOME_UNRECORDED result.
//
// ── Damage ───────────────────────────────────────────────────────────────────
// Nothing here repairs the file; the original bytes stay where they are and a
// diagnostic names the 1-based line, byte offset and length of each problem.
//   final record cut short (no trailing newline)        Recovered
//   a cut-short record that AppendEntry later sealed
//     (a `journal_seal` record right after it)          Recovered
//   byte-identical duplicate of an earlier record
//     (same `uuid`): the later copy is dropped          Recovered
//   tool_call record with an unknown schema             Recovered (call unrecorded)
//   malformed or untyped record anywhere else           Damaged (later records still load)
//   same `uuid`, different content (both kept)          Damaged
// A damaged journal still resumes — refusing would strand the user — but the
// snapshot says so, RunTask journals a `journal_recovery` entry and emits a
// `journal_recovery` event, and every call whose outcome cannot be read is an
// explicit OUTCOME_UNRECORDED error result, never a success.
//
// ── Recovery projection vs. request projection ──────────────────────────────
// FSessionSnapshot::Messages is the recovery projection: provider-neutral, in
// journal order, per-call records folded (FResumeProjector), pairing repaired
// without removing a genuine user message (KismetAIHistoryRepair), thinking and
// provider-specific blocks intact. ProjectResumeRequest derives the history a
// resumed request starts from (prior-turn thinking removed) as new objects; the
// provider encoders adapt further downstream.
//
// ── Effective context ───────────────────────────────────────────────
// FSessionSnapshot::Context is Messages with the journal's projection records
// replayed in order, by the same KismetAIContextProjection::Apply the live loop
// used to make them. Each record names the length and structural anchor of the
// projection it was decided on; a record that does not match (or an older or
// unreadable one) is skipped with a diagnostic and Context keeps the full
// history from there — the conservative fallback. RunTask then rebudgets the
// restored context for the current model before its first request.
//
// ── Intentional differences from the retired LoadMessages reader ───────────
//   - A later turn that reuses an earlier turn's tool_use id with DIFFERENT
//     content is kept and renamed by the pairing repair. The old reader dropped
//     it and its results: a completed edit vanished from resumed history.
//   - An unanswered tool batch (legacy interrupted turn) is answered with
//     OUTCOME_UNRECORDED by the loader itself; RunTask did the same repair right
//     after the old loader, so the outgoing request is unchanged.
//   - An assistant entry with empty content, or a tool_result_message without
//     `results`, yields no message. The old reader emitted an empty assistant
//     message and a role-only user message, both invalid requests.
//   - Thinking is kept in the snapshot and removed only by ProjectResumeRequest.
//     Same outgoing request as before; the old reader discarded it on load.
//   - Malformed lines are classified (above) instead of being skipped silently.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "KismetAIHistoryRepair.h"
#include "KismetAIAgentTypes.h"
#include "KismetAIMutationIntent.h"

namespace KismetAIJournalReducer
{
	// session_start.v this build writes and the newest it reads.
	inline constexpr int32 JournalFormatVersion = 1;

	// Entry types this module owns.
	inline constexpr const TCHAR* SealEntryType = TEXT("journal_seal");
	inline constexpr const TCHAR* RecoveryEntryType = TEXT("journal_recovery");
}

enum class EJournalIntegrity : uint8
{
	// Every line parsed and nothing needed recovering.
	Intact,
	// Explainable damage, handled with explicit uncertainty (see the table above).
	Recovered,
	// Unexplained damage in the middle of the file; later records still load.
	Damaged,
	// Written by a newer, incompatible build. Not resumable.
	UnsupportedVersion,
	// No journal file for this session.
	Missing,
	// The file exists but could not be read. Not resumable.
	Unreadable,
};

enum class EJournalIssue : uint8
{
	TruncatedFinalRecord,
	SealedTruncatedRecord,
	MalformedRecord,
	UntypedRecord,
	DuplicateRecord,
	ConflictingDuplicateRecord,
	UnsupportedRecordSchema,
	UnsupportedJournalVersion,
	UnreadableFile,
	// Raised only if the user-message preservation check fails (a bug).
	UserMessageLost,
	// A context_compaction projection record that does not apply to the
	// projection at its position (or cannot be read): it is skipped and the
	// context keeps the full history from there (Recovered).
	ProjectionNotReproduced,
};

struct KISMETAIRUNTIME_API FJournalDiagnostic
{
	EJournalIssue Issue = EJournalIssue::MalformedRecord;
	// 1-based line; 0 when the issue is not tied to one line.
	int32 Line = 0;
	// Byte offset of the line's first byte and its length (newline excluded);
	// -1 when unknown (entries reduced without their source bytes).
	int64 ByteOffset = -1;
	int64 ByteLength = -1;
	FString Detail;
	// The start of the offending line, for a human; the bytes stay in the file.
	FString Excerpt;

	static const TCHAR* IssueToString(EJournalIssue Issue);
	TSharedPtr<FJsonObject> ToJson() const;
};

// One decoded journal line.
struct FJournalLine
{
	TSharedPtr<FJsonObject> Entry;
	int32 Line = 0;
	int64 ByteOffset = -1;
	int64 ByteLength = -1;
};

struct KISMETAIRUNTIME_API FParsedJournal
{
	// Every line that decoded to a JSON object, in file order.
	TArray<FJournalLine> Lines;
	// Lines that did not decode, classified.
	TArray<FJournalDiagnostic> Diagnostics;
	int32 MalformedLineCount = 0;

	TArray<TSharedPtr<FJsonObject>> Entries() const;
};

// What replaying the journal's projection records did.
struct KISMETAIRUNTIME_API FContextReplayReport
{
	// Schema-2 context_compaction records applied, and skipped as not reproducible.
	int32 AppliedRecords = 0;
	int32 NotReproducedRecords = 0;
	// Committed (schema 1) records: no summary was persisted, so they
	// cannot be replayed; the context keeps the full history they compacted.
	int32 LegacyCompactionRecords = 0;
	// Usable window / model of the last applied record (0 / empty when none).
	int32 EffectiveLimit = 0;
	FString Model;
	// Synthetic messages replayed (synthetic_message and legacy flagged user_message).
	int32 SyntheticMessages = 0;

	FString ToString() const;
	TSharedPtr<FJsonObject> ToJson() const;
};

// A model-visible message the runtime wrote, with its provenance.
struct FSyntheticMessageInfo
{
	// The projected message object (in Messages and, unless compacted away, in Context).
	TSharedPtr<FJsonValue> Message;
	FString Kind;
	FString Uuid;
	// An older `user_message` with `synthetic: true`.
	bool bLegacy = false;
};

struct KISMETAIRUNTIME_API FSessionSnapshot
{
	// The recovery projection (see header). Provider-neutral; thinking kept.
	// The full canonical transcript: compaction never shortens it.
	TArray<FAgentMessage> Messages;

	// The effective context: Messages with every reproducible
	// context_compaction projection record applied at its journal position, in
	// order, then pairing-repaired. Equal to Messages when the journal has no
	// such records (legacy sessions included). This is what a resumed request
	// starts from (ProjectResumeRequest(Context)).
	TArray<FAgentMessage> Context;
	FContextReplayReport ContextReplay;
	// Bodies the replayed microcompaction cleared, by tool_use_id (the evidence
	// a later summary reads).
	TMap<FString, FString> ClearedToolResultBodies;
	// Runtime-written model-visible messages, in journal order.
	TArray<FSyntheticMessageInfo> SyntheticMessages;
	// Damage already announced to the model by a journal_damage_notice.
	TSet<FString> NoticedDamageKeys;
	// Key under which a diagnostic is announced to the model; empty for issues
	// that cannot have lost conversation content.
	static FString DamageNoticeKey(const FJournalDiagnostic& Diagnostic);

	// From the LAST session_meta. It is re-appended on every terminal path, so
	// an earlier one describes a run that has since been superseded.
	FString Status;
	FString StopReason;
	FString Model;

	// First user message, with any <references> block stripped — the session's
	// display title.
	FString Title;

	int32 EntryCount = 0;
	// Lines that did not decode (truncated, sealed or malformed).
	int32 SkippedEntryCount = 0;

	// session_start.v; 0 when the journal has no header (legacy).
	int32 JournalVersion = 0;
	EJournalIntegrity Integrity = EJournalIntegrity::Intact;
	TArray<FJournalDiagnostic> Diagnostics;

	// What the pairing repair changed to make Messages provider-valid.
	KismetAIHistoryRepair::FRepairReport Repair;
	// Stamped calls with no readable record (projected OUTCOME_UNRECORDED).
	int32 UnrecordedCalls = 0;

	// Accepted indexed terminal tool records, unaffected by context compaction.
	// Each retains its original run_id; consumers must not attribute historical
	// changes to a new run. Includes failed/not-started calls, so inspect status.
	TArray<TSharedPtr<FJsonObject>> CommittedCalls;

	// (KismetAIMutationIntent.h): the unrecorded calls that are mutations,
	// each with its mutation_intent and first mutation_reconciliation when the
	// journal has them. A call with a reconciliation also carries that
	// reconciliation's note in its projected OUTCOME_UNRECORDED result.
	TArray<FKismetAIUncertainCall> UncertainCalls;
	// Every readable mutation_reconciliation, first per call, in journal order:
	// the evidence the resumed run's replay guard is built from.
	TArray<FKismetAIReconciliation> Reconciliations;
	// user_message entries reduced (after duplicate removal), and whether each
	// one is still a genuine user message in Messages.
	int32 UserMessageEntries = 0;
	bool bUserMessagesPreserved = true;

	// False for UnsupportedVersion and Unreadable: appending to such a file is
	// not safe. Missing is resumable (a fresh journal is started).
	bool CanResume() const;
	static const TCHAR* IntegrityToString(EJournalIntegrity Integrity);
	// {integrity, journal_version, skipped, unrecorded_calls, diagnostics[]}.
	TSharedPtr<FJsonObject> IntegrityToJson() const;
};

namespace KismetAIJournalReducer
{
	// The authoritative decoder: UTF-8 JSONL bytes -> entries + classified
	// diagnostics with line numbers and byte offsets.
	KISMETAIRUNTIME_API FParsedJournal ParseJournalBytes(const TArray<uint8>& Bytes);

	// Same decoder over text (converted to UTF-8). OutSkipped counts lines that
	// did not decode.
	KISMETAIRUNTIME_API TArray<TSharedPtr<FJsonObject>> ParseEntries(const FString& Jsonl, int32& OutSkipped);

	// Reduce a decoded journal to its recovery projection and integrity report.
	KISMETAIRUNTIME_API FSessionSnapshot Reduce(const FParsedJournal& Journal);

	// Reduce bare entries (no source bytes: diagnostics carry no offsets).
	KISMETAIRUNTIME_API FSessionSnapshot Reduce(const TArray<TSharedPtr<FJsonObject>>& Entries);

	// Convenience: decode then reduce.
	KISMETAIRUNTIME_API FSessionSnapshot ReduceJsonl(const FString& Jsonl);
	KISMETAIRUNTIME_API FSessionSnapshot ReduceJournalBytes(const TArray<uint8>& Bytes);

	// The history a resumed request starts from: the recovery projection with
	// prior-turn thinking / redacted_thinking blocks removed (an assistant
	// message left empty is dropped). Thinking signatures are bound to the turn
	// and model that produced them and are never needed once a new user message
	// follows. Messages that change are copied; the input is not mutated.
	KISMETAIRUNTIME_API TArray<FAgentMessage> ProjectResumeRequest(const TArray<FAgentMessage>& Messages);

	// Compatibility adapter for callers that still expose the persisted JSON shape.
	KISMETAIRUNTIME_API TArray<TSharedPtr<FJsonValue>> ProjectResumeRequest(const TArray<TSharedPtr<FJsonValue>>& Messages);
}
