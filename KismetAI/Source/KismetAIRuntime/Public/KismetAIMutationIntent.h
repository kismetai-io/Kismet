// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Mutation intent and reconciliation of uncertain writes.
//
// ── The problem ──────────────────────────────────────────────────────────────
// Records each tool call's outcome as soon as the call returns. A crash
// between the tool's side effect and that record leaves the call with no
// outcome; reload projects OUTCOME_UNRECORDED. That is honest but blind: the
// resumed model cannot tell "the node is already there" from "nothing
// happened", and the natural move — send the same insert_node again — puts a
// second copy into the graph.
//
// ── Crash boundaries of one mutating call (agent run) ────────────────────────
//   B0  assistant_message stamped         the call's arguments are durable
//   B1  before-state captured, checkpoint ensured (editor, game thread)
//   B2  mutation_intent appended                  before-state is durable
//   B3  handler runs: apply in memory, compile, autosave (outcome)
//   B4  tool_call record appended                 outcome is durable
//   B5  the result is shown to the model and the UI
// A crash before B2 leaves no intent: the call is reconciled as no_evidence. A
// crash between B2 and B4 is the window this file closes: the intent's
// before-state is compared with the asset as it is now. After B4 the record is
// the truth and nothing is reconciled. "Durable" means the journal append
// returned (an OS write), not fsync: a power loss can still lose B2 or B4, and
// then the call degrades to no_evidence, never to a guess.
//
// ── Journal records (additive types; journal format version unchanged) ─────
//   {"type":"mutation_intent", "schema":1, "uuid", "timestamp",
//    "run_id", "turn_id", "call_id", "call_index", "tool_use_id", "tool_name",
//    "args_canonical":"<the call's arguments, canonical JSON>",
//    "targets":[<asset fingerprint>...],      // what the call names, before it ran
//    "unresolved_targets":["<raw>"...]}       // names the authority parser could not resolve
//   {"type":"mutation_reconciliation", "schema":1, "uuid", "timestamp",
//    "run_id", "turn_id", "call_id", "call_index", "tool_use_id", "tool_name",
//    "args_canonical", "verdict", "assets":[...], "explanation"}
// A reconciliation is written once per uncertain call, on the first resume
// that sees it, and is the evidence every later resume reuses: assets keep
// changing after a crash, and only the first look is evidence about the call.
//
// ── Asset fingerprint ────────────────────────────────────────────────────────
// Computed by the editor (KismetAIAssetState.h): a digest of every non-transient
// property of every object in the asset's package plus each graph node's pins,
// links and defaults, with per-object digests for evidence. No object
// addresses and no property values are stored — only hashes, the asset path
// and object names — so a fingerprint carries no content a user did not
// already put in the journal. Plus the package's dirty flag and the SHA-1 of
// its file on disk.
//
// ── Verdicts ─────────────────────────────────────────────────────────────────
//   not_applied           every target equals its before-state (in memory, and on
//                         disk where the file was recorded): the call has no
//                         visible effect. Its effect may have been lost with the
//                         process (an unsaved edit). The model may redo it; nothing
//                         redoes it automatically.
//   changed_since_intent  a target differs from its before-state. The call may
//                         have applied (fully or partly) or someone else changed
//                         the asset; the difference cannot be attributed. The same
//                         call is refused (RECONCILIATION_REQUIRED) for the rest of
//                         the session; the model must re-read and decide.
//   asset_missing         a target existed before and is gone now.
//   no_evidence           no intent was recorded, a target could not be
//                         fingerprinted, or the asset class is not supported. Also
//                         refused when repeated.
// Nothing here makes execution exactly-once. A verdict says what the asset
// looks like compared with the recorded before-state, nothing more.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

struct KISMETAIRUNTIME_API FKismetAIAssetFingerprint
{
	// Normalized long package path ("/Game/Folder/BP_Name").
	FString AssetPath;
	FString AssetClass;

	// The editor looked at the asset. False means nothing below is evidence
	// (an invoker without asset state, or the capture could not run).
	bool bCaptured = false;

	// The asset exists (loaded or loadable). False for a create target that
	// does not exist yet, or a deleted asset.
	bool bExists = false;

	// False when this build cannot describe the asset's state (no package,
	// unsupported class); UnsupportedReason says why. Digests are empty then.
	bool bSupported = false;
	FString UnsupportedReason;

	// SHA-1 over every object's state, and per-object digests (key: object path
	// inside the package). Equal digests mean equal described state.
	FString Digest;
	TMap<FString, FString> ObjectDigests;

	bool bPackageDirty = false;

	// The package file as it is on disk.
	bool bFileExists = false;
	int64 FileSize = -1;
	FString FileSha1;

	bool HasStateEvidence() const { return bCaptured && bExists && bSupported && !Digest.IsEmpty(); }
	bool SameState(const FKismetAIAssetFingerprint& Other) const;
	bool SameFile(const FKismetAIAssetFingerprint& Other) const;

	// Human-readable differences in described state (objects added, removed,
	// changed), at most MaxLines lines plus one line saying how many were cut.
	static TArray<FString> DescribeDifferences(const FKismetAIAssetFingerprint& Before,
		const FKismetAIAssetFingerprint& After, int32 MaxLines = 12);

	TSharedPtr<FJsonObject> ToJson() const;
	static bool FromJson(const TSharedPtr<FJsonObject>& Json, FKismetAIAssetFingerprint& Out);
};

enum class EKismetAIReconcileVerdict : uint8
{
	NotApplied,
	ChangedSinceIntent,
	AssetMissing,
	NoEvidence,
};

struct KISMETAIRUNTIME_API FKismetAIAssetReconciliation
{
	FString AssetPath;
	EKismetAIReconcileVerdict Verdict = EKismetAIReconcileVerdict::NoEvidence;
	// Loaded state vs. the before-state; unset when there is no evidence.
	TOptional<bool> MemoryMatchesBefore;
	// Package file vs. the before-state's file; unset when either side was not recorded.
	TOptional<bool> DiskMatchesBefore;
	TArray<FString> Differences;
	FString Detail;

	TSharedPtr<FJsonObject> ToJson() const;
	static bool FromJson(const TSharedPtr<FJsonObject>& Json, FKismetAIAssetReconciliation& Out);
};

// One stamped mutating call with no outcome record, as the reader found it.
struct KISMETAIRUNTIME_API FKismetAIUncertainCall
{
	FString RunId;
	FString TurnId;
	FString CallId;
	int32 CallIndex = 0;
	// The id the projection uses (renamed when aliased a duplicate).
	FString ToolUseId;
	FString ToolName;
	FString ArgsCanonical;
	// The call's mutation_intent, when one was journaled.
	TSharedPtr<FJsonObject> Intent;
	// The call's first mutation_reconciliation, when one was journaled.
	TSharedPtr<FJsonObject> Reconciliation;
};

struct KISMETAIRUNTIME_API FKismetAIReconciliation
{
	FString RunId;
	FString TurnId;
	FString CallId;
	int32 CallIndex = 0;
	FString ToolUseId;
	FString ToolName;
	FString ArgsCanonical;
	EKismetAIReconcileVerdict Verdict = EKismetAIReconcileVerdict::NoEvidence;
	TArray<FKismetAIAssetReconciliation> Assets;
	FString Explanation;

	// Whether an identical call must be refused: everything but not_applied.
	bool BlocksReplay() const { return Verdict != EKismetAIReconcileVerdict::NotApplied; }

	// The text appended to the call's OUTCOME_UNRECORDED result for the model.
	FString BuildModelNote() const;

	// The journal record (type mutation_reconciliation).
	TSharedPtr<FJsonObject> ToEntry() const;
	static bool FromEntry(const TSharedPtr<FJsonObject>& Entry, FKismetAIReconciliation& Out);
};

namespace KismetAIMutationIntent
{
	inline constexpr const TCHAR* IntentEntryType = TEXT("mutation_intent");
	inline constexpr const TCHAR* ReconciliationEntryType = TEXT("mutation_reconciliation");
	inline constexpr int32 SchemaVersion = 1;

	// The refusal code for repeating a call whose effect is uncertain.
	inline constexpr const TCHAR* ReconciliationRequiredCode = TEXT("RECONCILIATION_REQUIRED");

	KISMETAIRUNTIME_API const TCHAR* VerdictToString(EKismetAIReconcileVerdict Verdict);
	KISMETAIRUNTIME_API bool VerdictFromString(const FString& In, EKismetAIReconcileVerdict& Out);

	// The identity of a call for the replay guard: tool name + canonical arguments.
	KISMETAIRUNTIME_API FString ReplayKey(const FString& ToolName, const FString& ArgsCanonical);

	// Canonical (condensed) JSON of a call's arguments; "{}" for null.
	KISMETAIRUNTIME_API FString CanonicalArgs(const TSharedPtr<FJsonObject>& Arguments);

	KISMETAIRUNTIME_API TSharedPtr<FJsonObject> BuildIntentEntry(const FString& RunId, const FString& TurnId,
		int32 CallIndex, const FString& ToolUseId, const FString& ToolName, const FString& ArgsCanonical,
		const TArray<FKismetAIAssetFingerprint>& Targets, const TArray<FString>& UnresolvedTargets);

	// The before-state fingerprints of an intent record. False when the record
	// is unreadable or a newer schema.
	KISMETAIRUNTIME_API bool ReadIntentTargets(const TSharedPtr<FJsonObject>& Intent,
		TArray<FKismetAIAssetFingerprint>& OutTargets, TArray<FString>& OutUnresolved);

	// One asset: compare the before-state with the state now.
	KISMETAIRUNTIME_API FKismetAIAssetReconciliation ReconcileAsset(const FKismetAIAssetFingerprint& Before,
		const FKismetAIAssetFingerprint& Now);

	// The whole call. Now is matched to the intent's targets by asset path; a
	// target with no current fingerprint has no evidence. Without an intent the
	// verdict is no_evidence.
	KISMETAIRUNTIME_API FKismetAIReconciliation Reconcile(const FKismetAIUncertainCall& Call,
		const TArray<FKismetAIAssetFingerprint>& Now);
}
