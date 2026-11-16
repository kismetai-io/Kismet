// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Typed outcome of a mutating tool call.
//
// A mutating tool used to report one thing — "success" — while three
// independent things happened: the asset changed in memory, the Blueprint was
// (or was not) recompiled, and the package was (or was not) written to disk.
// The save helpers returned void, so a read-only file or a source-control lock
// left an edit that only existed in memory under a success result, and a graph
// that failed to compile could already be on disk before anyone saw the error.
//
// This type keeps the four states apart. Handlers attach it to their success
// payload as `data.outcome`; the outer envelope keeps its shape (`success`
// still means "the tool ran and its operations were applied"), so existing
// consumers that read `success`, `compile_success` or `compiled` see what they
// saw before. Everything a reader needs to tell "changed but unsaved" or
// "saved but does not compile" from a clean edit is in the outcome.
//
// JSON contract (data.outcome) — see ToJson():
//   applied_in_memory                  bool   the tool changed the live asset
//   compile_status                     "succeeded" | "failed" | "not_run"
//   compile_error_count                int    present when compile_status != not_run
//   save_status                        "saved" | "failed" | "not_requested"
//   save_skip_reason                   string present when save_status == not_requested:
//                                             "eval_mode" | "no_file_backing" |
//                                             "package_not_dirty" | "nothing_applied" |
//                                             "not_autosaved"
//   save_error                         string present when save_status == failed
//   package                            string long package name, when known
//   package_file                       string target file, when a save was attempted
//   package_was_dirty_before           bool   the package already had unsaved changes
//                                             before this tool touched it
//   save_included_preexisting_changes  bool   saved == true AND dirty before: the file
//                                             now also holds edits the agent did not make
//   save_policy                        string the policy that decided whether to save
//   recovery_status                    "not_tracked" | "restored" | "partially_restored" |
//                                             "restore_failed" (atomic batch rollback)
//   retry_safe                         bool   present when recovery_status != not_tracked:
//                                             true only when the asset is verified back at its
//                                             pre-batch state, so a corrected batch may be resent
//   summary                            string one line for humans and the model
//
// "saved" means UPackage::Save reported success for the target file. It is
// not a crash-safety claim — no fsync or reload is implied.
//
// Lives in the runtime module: pure JSON and strings, no editor types.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

enum class EKismetAICompileStatus : uint8
{
	// The tool did not compile (layout, asset edits, read-only batches).
	NotRun,
	Succeeded,
	Failed,
};

enum class EKismetAISaveStatus : uint8
{
	// No save was attempted. SaveSkipReason says why.
	NotRequested,
	// UPackage::Save reported success for PackageFilename.
	Saved,
	// A save was attempted and did not complete. The change is in memory only.
	Failed,
};

// What happened to the pre-mutation state when an atomic batch failed.
// Only atomic batches (edit_blueprint / edit_material with atomic=true) that
// hit a failing operation attempt a restore; every other mutation is
// NotTracked (editor Undo and source control are the user's recovery paths).
enum class EKismetAIRecoveryStatus : uint8
{
	NotTracked,
	// The batch's own transaction record was applied and a semantic snapshot
	// of the asset equals the pre-batch snapshot. Nothing the batch did remains.
	Restored,
	// The record was applied but the asset still differs from the pre-batch
	// snapshot: part of the batch may remain. State is uncertain.
	PartiallyRestored,
	// No restore could run; every operation that succeeded is still applied.
	// The batch's transaction is kept in the undo buffer so a human can undo it.
	RestoreFailed,
};

namespace KismetAISaveSkipReason
{
	// KISMETAI_EVAL_ALLOW: eval runs keep edits in memory so the harness's
	// reload-based reset works and committed fixtures stay clean.
	inline constexpr const TCHAR* EvalMode = TEXT("eval_mode");
	// /Temp/ or otherwise not a long package name: nothing on disk to write.
	inline constexpr const TCHAR* NoFileBacking = TEXT("no_file_backing");
	// The package had no unsaved changes when the save point was reached.
	inline constexpr const TCHAR* PackageNotDirty = TEXT("package_not_dirty");
	// The tool applied nothing (all operations failed, rolled back, or read-only).
	inline constexpr const TCHAR* NothingApplied = TEXT("nothing_applied");
	// The tool does not autosave by design (e.g. material edits).
	inline constexpr const TCHAR* NotAutosaved = TEXT("not_autosaved");
	// An atomic batch failed and its rollback could not be verified. Not saved,
	// so the file on disk keeps its pre-batch content.
	inline constexpr const TCHAR* RestoreIncomplete = TEXT("restore_incomplete");
}

namespace KismetAISavePolicy
{
	// The documented product policy for tools that autosave: once a mutation is
	// applied, the package is saved whether or not the Blueprint compiles.
	// Agent work is multi-step and intermediate graphs routinely fail to compile
	// (a node is placed before it is wired). Holding them back would leave the
	// editor's only copy of the work in memory, and the package stays dirty so
	// the user's next Ctrl+S writes the same state anyway. What makes this safe
	// to keep is that compile_status travels with save_status, so an
	// uncompilable graph on disk is always reported as such.
	inline constexpr const TCHAR* AutosaveEvenIfCompileFailed = TEXT("autosave_after_mutation_even_if_compile_failed");
	// An explicit save the caller asked for (manage_asset action=save).
	inline constexpr const TCHAR* ExplicitSave = TEXT("explicit_save");
	// The tool never saves; the user saves (or discards) the change.
	inline constexpr const TCHAR* NoAutosave = TEXT("no_autosave");
}

struct KISMETAIRUNTIME_API FKismetAIMutationOutcome
{
	bool bAppliedInMemory = false;

	EKismetAICompileStatus CompileStatus = EKismetAICompileStatus::NotRun;
	int32 CompileErrorCount = 0;

	EKismetAISaveStatus SaveStatus = EKismetAISaveStatus::NotRequested;
	FString SaveSkipReason;
	FString SaveError;
	FString SavePolicy;

	FString PackageName;
	FString PackageFilename;
	bool bPackageWasDirtyBefore = false;

	EKismetAIRecoveryStatus RecoveryStatus = EKismetAIRecoveryStatus::NotTracked;

	// An atomic rollback was attempted and did not verifiably restore the asset.
	bool IsRestoreIncomplete() const
	{
		return RecoveryStatus == EKismetAIRecoveryStatus::PartiallyRestored
			|| RecoveryStatus == EKismetAIRecoveryStatus::RestoreFailed;
	}

	// Whether resending a corrected batch is safe: true unless a rollback left
	// the asset in an unverified state (re-read it first, never replay).
	bool IsRetrySafe() const { return !IsRestoreIncomplete(); }

	// The file now holds edits made before the agent touched the package.
	bool SavedIncludesPreexistingChanges() const
	{
		return SaveStatus == EKismetAISaveStatus::Saved && bPackageWasDirtyBefore;
	}

	// A state the user should be told about: changed but unsaved, not
	// compiling, or a failed batch whose rollback could not be verified.
	bool NeedsAttention() const
	{
		return SaveStatus == EKismetAISaveStatus::Failed
			|| CompileStatus == EKismetAICompileStatus::Failed
			|| IsRestoreIncomplete();
	}

	static const TCHAR* CompileStatusToString(EKismetAICompileStatus Status);
	static const TCHAR* SaveStatusToString(EKismetAISaveStatus Status);
	static const TCHAR* RecoveryStatusToString(EKismetAIRecoveryStatus Status);
	static bool CompileStatusFromString(const FString& In, EKismetAICompileStatus& Out);
	static bool SaveStatusFromString(const FString& In, EKismetAISaveStatus& Out);
	static bool RecoveryStatusFromString(const FString& In, EKismetAIRecoveryStatus& Out);

	// One line that states every dimension; never says "saved" unless it was,
	// and leads with an incomplete rollback when there was one.
	FString BuildSummary() const;

	TSharedPtr<FJsonObject> ToJson() const;
	static bool FromJson(const TSharedPtr<FJsonObject>& Json, FKismetAIMutationOutcome& Out);

	// Find `data.outcome` in a tool result as the model receives it (bare or
	// wrapped in <tool_use_error>). False when the result carries no outcome.
	static bool TryExtractFromToolResult(const FString& ToolResult, FKismetAIMutationOutcome& Out);

private:
	// Applied / compile / save, without the recovery headline.
	FString BuildStateSummary() const;
};

// Committed operation evidence, keyed by package with separate compile/save
// freshness. Resume retains attributed unresolved history; MutationCount counts
// only this run. Failed operations require matching successful operation proof.
class KISMETAIRUNTIME_API FKismetAIRunOutcomeLedger
{
public:
	void Record(const FString& ToolName, const FKismetAIMutationOutcome& Outcome);

	bool IsEmpty() const { return Packages.Num() == 0 && UnknownOperations.IsEmpty() && FailedOperations.IsEmpty(); }
	void RecordCommittedCall(const TSharedPtr<FJsonObject>& Entry);
	void RecordUnknown(const FString& Description) { UnknownOperations.AddUnique(Description); }
	void RetainPriorUnresolved();
	TArray<FString> GetUnverifiedPackages(bool bCurrentRunOnly = false) const;
	bool HasUnresolvedEvidence() const;
	TSharedPtr<FJsonObject> BuildCompletionSummary(const FString& TerminalOutcome) const;

	TArray<FString> GetUnsavedPackages() const;
	TArray<FString> GetCompileFailedPackages() const;
	TArray<FString> GetPackagesSavedWithPreexistingChanges() const;
	// Packages whose latest atomic rollback was partial or failed.
	TArray<FString> GetUnrestoredPackages() const;

	bool NeedsAttention() const;

	// The `mutation_outcomes` step event payload (type field included).
	TSharedPtr<FJsonObject> BuildEventJson() const;

private:
	struct FPackageState
	{
		FKismetAIMutationOutcome Last;
		FString LastTool;
		bool bEverSavedWithPreexistingChanges = false;
		bool bEverRestoreIncomplete = false;
		int32 MutationCount = 0;
		bool bCompileRequired = false;
		bool bPriorEvidence = false;
		FString EvidenceRunId;
		FString PriorEvidenceRunId;
	};
	TArray<FString> UnknownOperations;
	TMap<FString, FString> FailedOperations;
	TMap<FString, FPackageState> Packages;
};
