// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIMutationOutcome.h"
#include "KismetAIToolRegistry.h"
#include "KismetAIAuthority.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	TSharedPtr<FJsonValue> SortedEvidenceValue(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid()) return MakeShared<FJsonValueNull>();
		if (Value->Type == EJson::Object)
		{
			auto Sorted = MakeShared<FJsonObject>();
			TArray<FString> Keys;
			for (const auto& Pair : Value->AsObject()->Values) Keys.Add(FString(Pair.Key));
			Keys.Sort();
			for (const FString& Key : Keys) Sorted->SetField(Key, SortedEvidenceValue(Value->AsObject()->TryGetField(Key)));
			return MakeShared<FJsonValueObject>(Sorted);
		}
		if (Value->Type == EJson::Array)
		{
			TArray<TSharedPtr<FJsonValue>> Elements;
			for (const auto& Element : Value->AsArray()) Elements.Add(SortedEvidenceValue(Element));
			return MakeShared<FJsonValueArray>(Elements);
		}
		return Value;
	}

	// Completion repair identity is semantic, unlike the journal replay identity:
	// a fresh precondition token and JSON key ordering do not change the edit.
	FString CompletionOperationArgs(const TSharedPtr<FJsonObject>& Input, const FKismetAIOperation& Operation)
	{
		auto Semantic = MakeShared<FJsonObject>();
		FJsonObject::Duplicate(Input, Semantic);
		Semantic->RemoveField(TEXT("expected_revision"));
		for (const FKismetAIAuthorityTarget& Target : Operation.Targets)
		{
			if (!Target.IsResolved()) continue;
			TArray<FString> Fields;
			Target.Field.ParseIntoArray(Fields, TEXT("+"));
			for (const FString& Field : Fields) Semantic->RemoveField(Field);
			Semantic->SetStringField(Target.Field, Target.PackagePath.ToLower());
		}
		const auto Sorted = SortedEvidenceValue(MakeShared<FJsonValueObject>(Semantic))->AsObject();
		FString Result;
		FJsonSerializer::Serialize(Sorted.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result));
		return Result;
	}
}

const TCHAR* FKismetAIMutationOutcome::CompileStatusToString(EKismetAICompileStatus Status)
{
	switch (Status)
	{
	case EKismetAICompileStatus::Succeeded: return TEXT("succeeded");
	case EKismetAICompileStatus::Failed:    return TEXT("failed");
	case EKismetAICompileStatus::NotRun:
	default:                                return TEXT("not_run");
	}
}

const TCHAR* FKismetAIMutationOutcome::SaveStatusToString(EKismetAISaveStatus Status)
{
	switch (Status)
	{
	case EKismetAISaveStatus::Saved:        return TEXT("saved");
	case EKismetAISaveStatus::Failed:       return TEXT("failed");
	case EKismetAISaveStatus::NotRequested:
	default:                                return TEXT("not_requested");
	}
}

const TCHAR* FKismetAIMutationOutcome::RecoveryStatusToString(EKismetAIRecoveryStatus Status)
{
	switch (Status)
	{
	case EKismetAIRecoveryStatus::Restored:          return TEXT("restored");
	case EKismetAIRecoveryStatus::PartiallyRestored: return TEXT("partially_restored");
	case EKismetAIRecoveryStatus::RestoreFailed:     return TEXT("restore_failed");
	case EKismetAIRecoveryStatus::NotTracked:
	default:                                         return TEXT("not_tracked");
	}
}

bool FKismetAIMutationOutcome::RecoveryStatusFromString(const FString& In, EKismetAIRecoveryStatus& Out)
{
	if (In == TEXT("not_tracked"))        { Out = EKismetAIRecoveryStatus::NotTracked;        return true; }
	if (In == TEXT("restored"))           { Out = EKismetAIRecoveryStatus::Restored;          return true; }
	if (In == TEXT("partially_restored")) { Out = EKismetAIRecoveryStatus::PartiallyRestored; return true; }
	if (In == TEXT("restore_failed"))     { Out = EKismetAIRecoveryStatus::RestoreFailed;     return true; }
	return false;
}

bool FKismetAIMutationOutcome::CompileStatusFromString(const FString& In, EKismetAICompileStatus& Out)
{
	if (In == TEXT("succeeded")) { Out = EKismetAICompileStatus::Succeeded; return true; }
	if (In == TEXT("failed"))    { Out = EKismetAICompileStatus::Failed;    return true; }
	if (In == TEXT("not_run"))   { Out = EKismetAICompileStatus::NotRun;    return true; }
	return false;
}

bool FKismetAIMutationOutcome::SaveStatusFromString(const FString& In, EKismetAISaveStatus& Out)
{
	if (In == TEXT("saved"))         { Out = EKismetAISaveStatus::Saved;        return true; }
	if (In == TEXT("failed"))        { Out = EKismetAISaveStatus::Failed;       return true; }
	if (In == TEXT("not_requested")) { Out = EKismetAISaveStatus::NotRequested; return true; }
	return false;
}

namespace
{
	FString DescribeSkipReason(const FString& Reason)
	{
		if (Reason == KismetAISaveSkipReason::EvalMode)        return TEXT("eval mode keeps edits in memory");
		if (Reason == KismetAISaveSkipReason::NoFileBacking)   return TEXT("the asset has no file on disk");
		if (Reason == KismetAISaveSkipReason::PackageNotDirty) return TEXT("the package had no unsaved changes");
		if (Reason == KismetAISaveSkipReason::NothingApplied)  return TEXT("nothing was applied");
		if (Reason == KismetAISaveSkipReason::NotAutosaved)    return TEXT("this tool does not autosave; save the asset to keep the change");
		if (Reason == KismetAISaveSkipReason::RestoreIncomplete) return TEXT("the rollback was not verified, so the file on disk keeps its pre-batch content");
		return Reason.IsEmpty() ? FString(TEXT("no save requested")) : Reason;
	}
}

FString FKismetAIMutationOutcome::BuildSummary() const
{
	// An atomic rollback is the headline: it decides whether the asset can be
	// trusted at all, so it comes before compile/save.
	FString Recovery;
	switch (RecoveryStatus)
	{
	case EKismetAIRecoveryStatus::Restored:
		return TEXT("Atomic batch failed; every change it made was reverted and verified, the asset is back at its pre-batch state. Not compiled, not saved.");
	case EKismetAIRecoveryStatus::PartiallyRestored:
		Recovery = TEXT("ROLLBACK INCOMPLETE: the atomic batch failed and its revert left differences from the pre-batch state; part of the batch may remain. Do not resend it automatically: re-read the asset first. ");
		break;
	case EKismetAIRecoveryStatus::RestoreFailed:
		Recovery = TEXT("ROLLBACK FAILED: the atomic batch failed and nothing was reverted; the operations that succeeded are still applied (the batch stays in the editor's undo history). Do not resend it automatically: re-read the asset first. ");
		break;
	default:
		break;
	}
	if (!Recovery.IsEmpty())
	{
		return Recovery + BuildStateSummary();
	}
	return BuildStateSummary();
}

FString FKismetAIMutationOutcome::BuildStateSummary() const
{
	if (!bAppliedInMemory && SaveStatus == EKismetAISaveStatus::NotRequested
		&& CompileStatus == EKismetAICompileStatus::NotRun)
	{
		return TEXT("No change was applied.");
	}

	FString Out = bAppliedInMemory ? TEXT("Applied in memory") : TEXT("No change applied by this call");

	switch (CompileStatus)
	{
	case EKismetAICompileStatus::Succeeded:
		Out += TEXT("; compile succeeded");
		break;
	case EKismetAICompileStatus::Failed:
		Out += FString::Printf(TEXT("; compile FAILED (%d error%s)"),
			CompileErrorCount, CompileErrorCount == 1 ? TEXT("") : TEXT("s"));
		break;
	default:
		Out += TEXT("; not compiled");
		break;
	}

	const FString PackageLabel = PackageName.IsEmpty() ? FString(TEXT("the package")) : PackageName;
	switch (SaveStatus)
	{
	case EKismetAISaveStatus::Saved:
		Out += FString::Printf(TEXT("; saved %s to disk."), *PackageLabel);
		if (bPackageWasDirtyBefore)
		{
			Out += TEXT(" The package already had unsaved changes before this edit; the save wrote those too, not only the agent's change.");
		}
		if (CompileStatus == EKismetAICompileStatus::Failed)
		{
			Out += TEXT(" The file on disk now holds a graph that does not compile.");
		}
		break;
	case EKismetAISaveStatus::Failed:
		Out += FString::Printf(TEXT("; NOT saved: %s could not be written (%s). The change exists only in the open editor and is lost if the editor closes without a successful save."),
			*PackageLabel, SaveError.IsEmpty() ? TEXT("unknown reason") : *SaveError);
		break;
	default:
		Out += FString::Printf(TEXT("; not saved (%s)."), *DescribeSkipReason(SaveSkipReason));
		break;
	}
	return Out;
}

TSharedPtr<FJsonObject> FKismetAIMutationOutcome::ToJson() const
{
	TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetBoolField(TEXT("applied_in_memory"), bAppliedInMemory);
	J->SetStringField(TEXT("compile_status"), CompileStatusToString(CompileStatus));
	if (CompileStatus != EKismetAICompileStatus::NotRun)
	{
		J->SetNumberField(TEXT("compile_error_count"), CompileErrorCount);
	}
	J->SetStringField(TEXT("save_status"), SaveStatusToString(SaveStatus));
	if (SaveStatus == EKismetAISaveStatus::NotRequested)
	{
		J->SetStringField(TEXT("save_skip_reason"), SaveSkipReason);
	}
	if (SaveStatus == EKismetAISaveStatus::Failed)
	{
		J->SetStringField(TEXT("save_error"), SaveError);
	}
	if (!PackageName.IsEmpty())
	{
		J->SetStringField(TEXT("package"), PackageName);
	}
	if (!PackageFilename.IsEmpty())
	{
		J->SetStringField(TEXT("package_file"), PackageFilename);
	}
	J->SetBoolField(TEXT("package_was_dirty_before"), bPackageWasDirtyBefore);
	J->SetBoolField(TEXT("save_included_preexisting_changes"), SavedIncludesPreexistingChanges());
	if (!SavePolicy.IsEmpty())
	{
		J->SetStringField(TEXT("save_policy"), SavePolicy);
	}
	J->SetStringField(TEXT("recovery_status"), RecoveryStatusToString(RecoveryStatus));
	if (RecoveryStatus != EKismetAIRecoveryStatus::NotTracked)
	{
		J->SetBoolField(TEXT("retry_safe"), IsRetrySafe());
	}
	J->SetStringField(TEXT("summary"), BuildSummary());
	return J;
}

bool FKismetAIMutationOutcome::FromJson(const TSharedPtr<FJsonObject>& Json, FKismetAIMutationOutcome& Out)
{
	if (!Json.IsValid())
	{
		return false;
	}
	FString CompileStr, SaveStr;
	if (!Json->TryGetStringField(TEXT("compile_status"), CompileStr)
		|| !Json->TryGetStringField(TEXT("save_status"), SaveStr))
	{
		return false;
	}
	FKismetAIMutationOutcome R;
	if (!CompileStatusFromString(CompileStr, R.CompileStatus) || !SaveStatusFromString(SaveStr, R.SaveStatus))
	{
		return false;
	}
	Json->TryGetBoolField(TEXT("applied_in_memory"), R.bAppliedInMemory);
	Json->TryGetNumberField(TEXT("compile_error_count"), R.CompileErrorCount);
	Json->TryGetStringField(TEXT("save_skip_reason"), R.SaveSkipReason);
	Json->TryGetStringField(TEXT("save_error"), R.SaveError);
	Json->TryGetStringField(TEXT("save_policy"), R.SavePolicy);
	Json->TryGetStringField(TEXT("package"), R.PackageName);
	Json->TryGetStringField(TEXT("package_file"), R.PackageFilename);
	Json->TryGetBoolField(TEXT("package_was_dirty_before"), R.bPackageWasDirtyBefore);
	FString RecoveryStr;
	if (Json->TryGetStringField(TEXT("recovery_status"), RecoveryStr))
	{
		// An unknown value from a newer handler is not "safe": treat it as failed.
		if (!RecoveryStatusFromString(RecoveryStr, R.RecoveryStatus))
		{
			R.RecoveryStatus = EKismetAIRecoveryStatus::RestoreFailed;
		}
	}
	Out = MoveTemp(R);
	return true;
}

bool FKismetAIMutationOutcome::TryExtractFromToolResult(const FString& ToolResult, FKismetAIMutationOutcome& Out)
{
	// Cheap reject: most tool results are reads with no outcome.
	if (!ToolResult.Contains(TEXT("\"outcome\"")))
	{
		return false;
	}
	FString Body = ToolResult;
	Body.RemoveFromStart(TEXT("<tool_use_error>"));
	Body.RemoveFromEnd(TEXT("</tool_use_error>"));

	TSharedPtr<FJsonObject> Envelope;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (!FJsonSerializer::Deserialize(Reader, Envelope) || !Envelope.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Data = nullptr;
	const TSharedPtr<FJsonObject>* OutcomeObj = nullptr;
	if (!Envelope->TryGetObjectField(TEXT("data"), Data) || !Data || !(*Data).IsValid()
		|| !(*Data)->TryGetObjectField(TEXT("outcome"), OutcomeObj) || !OutcomeObj)
	{
		return false;
	}
	return FromJson(*OutcomeObj, Out);
}

// ─────────────────────────────────────────────────────────────────────────────
// FKismetAIRunOutcomeLedger
// ─────────────────────────────────────────────────────────────────────────────

void FKismetAIRunOutcomeLedger::Record(const FString& ToolName, const FKismetAIMutationOutcome& Outcome)
{
	if (Outcome.PackageName.IsEmpty()) return;
	const bool bCompile = ToolName == TEXT("compile_blueprint") || ToolName == TEXT("compile_material");
	if (!bCompile && !Outcome.bAppliedInMemory && Outcome.SaveStatus == EKismetAISaveStatus::NotRequested
		&& Outcome.CompileStatus == EKismetAICompileStatus::NotRun && !Outcome.IsRestoreIncomplete()) return;

	FPackageState* Previous = Packages.Find(Outcome.PackageName);
	FKismetAIMutationOutcome Merged = Outcome;
	if (Previous)
	{
		// A new semantic mutation invalidates successful verification. A known
		// failure remains unresolved until a later compile actually succeeds.
		if (Merged.CompileStatus == EKismetAICompileStatus::NotRun
			&& ((!Outcome.bAppliedInMemory && !bCompile) || ToolName == TEXT("layout_graph")
				|| Previous->Last.CompileStatus == EKismetAICompileStatus::Failed))
		{
			Merged.CompileStatus = Previous->Last.CompileStatus;
			Merged.CompileErrorCount = Previous->Last.CompileErrorCount;
		}
		if (Merged.SaveStatus == EKismetAISaveStatus::NotRequested
			&& (!Outcome.bAppliedInMemory || Previous->Last.SaveStatus == EKismetAISaveStatus::Failed))
		{
			Merged.SaveStatus = Previous->Last.SaveStatus;
			Merged.SaveError = Previous->Last.SaveError;
			Merged.SaveSkipReason = Previous->Last.SaveSkipReason;
		}
		Merged.bAppliedInMemory |= Previous->Last.bAppliedInMemory;
	}
	FPackageState& State = Packages.FindOrAdd(Outcome.PackageName);
	State.Last = MoveTemp(Merged);
	State.LastTool = ToolName;
	State.bCompileRequired |= bCompile || (Outcome.bAppliedInMemory && ToolName != TEXT("layout_graph")
		&& (FKismetAIToolRegistry::RevisionTargetField(ToolName) != nullptr
			|| ToolName == TEXT("edit_material") || ToolName == TEXT("create_material_expression")));
	State.bEverSavedWithPreexistingChanges |= Outcome.SavedIncludesPreexistingChanges();
	State.bEverRestoreIncomplete |= Outcome.IsRestoreIncomplete();
	State.MutationCount += Outcome.bAppliedInMemory ? 1 : 0;
}

void FKismetAIRunOutcomeLedger::RecordCommittedCall(const TSharedPtr<FJsonObject>& Entry)
{
	if (!Entry.IsValid()) return;
	FString Tool, Status, RunId, CallId;
	Entry->TryGetStringField(TEXT("tool_name"), Tool);
	Entry->TryGetStringField(TEXT("status"), Status);
	Entry->TryGetStringField(TEXT("run_id"), RunId);
	Entry->TryGetStringField(TEXT("call_id"), CallId);
	if (Status == TEXT("not_started")) return;
	const TSharedPtr<FJsonObject>* Json = nullptr;
	FKismetAIMutationOutcome Outcome;
	const bool bHasOutcome = Entry->TryGetObjectField(TEXT("mutation_outcome"), Json)
		&& FKismetAIMutationOutcome::FromJson(*Json, Outcome) && !Outcome.PackageName.IsEmpty();
	const TSharedPtr<FJsonObject>* InputField = nullptr;
	TSharedPtr<FJsonObject> Input = Entry->TryGetObjectField(TEXT("input"), InputField)
		? *InputField : MakeShared<FJsonObject>();
	const FKismetAIOperation Operation = KismetAIAuthority::DescribeOperation(Tool, Input);
	const bool bMutation = Operation.RequiresAuthority();
	const bool bCompile = Tool == TEXT("compile_blueprint") || Tool == TEXT("compile_material");
	// Structured evidence is useful even when the registry's historical mutation
	// flag is false. Multipurpose inspect actions, however, need no write proof.
	if (!bHasOutcome && !bMutation && !bCompile) return;
	TArray<FString> Targets;
	for (const FKismetAIAuthorityTarget& Target : Operation.Targets)
		Targets.AddUnique(Target.IsResolved() ? Target.PackagePath.ToLower() : Target.Raw);
	if (Targets.IsEmpty() && bHasOutcome) Targets.Add(Outcome.PackageName.ToLower());
	Targets.Sort();
	const FString TargetText = FString::Join(Targets, TEXT(", "));
	FString FailureKey = Tool + TEXT("|") + TargetText;
	// An editing failure is superseded only by the same semantic operation
	// succeeding and applying a change. Compiling/saving the surviving graph
	// cannot establish that a rejected edit was carried out.
	if (!bCompile) FailureKey += TEXT("|") + CompletionOperationArgs(Input, Operation);
	if (Status != TEXT("succeeded"))
	{
		FailedOperations.Add(FailureKey, FString::Printf(TEXT("%s: %s did not complete (%s, run %s)."), *TargetText, *Tool, *CallId, *RunId));
	}
	else if (bHasOutcome && ((bCompile && Outcome.CompileStatus == EKismetAICompileStatus::Succeeded)
		|| (!bCompile && Outcome.bAppliedInMemory)))
	{
		FailedOperations.Remove(FailureKey);
	}
	if (bHasOutcome)
	{
		Record(Tool, Outcome);
		if (FPackageState* State = Packages.Find(Outcome.PackageName))
		{
			State->EvidenceRunId = RunId;
			if (Outcome.bAppliedInMemory && State->Last.SaveStatus == EKismetAISaveStatus::Saved
				&& (!State->bCompileRequired || State->Last.CompileStatus == EKismetAICompileStatus::Succeeded)
				&& !State->bEverRestoreIncomplete)
			{
				State->bPriorEvidence = false;
				State->PriorEvidenceRunId.Empty();
			}
		}
	}
	else if (Status == TEXT("succeeded") || (bMutation && Status == TEXT("failed")))
	{
		bool bHandlerNotStarted = false;
		Entry->TryGetBoolField(TEXT("handler_not_started"), bHandlerNotStarted);
		if (bHandlerNotStarted) return;
		RecordUnknown(FString::Printf(TEXT("%s: %s has no structured change/verification evidence (%s, run %s)."), *TargetText, *Tool, *CallId, *RunId));
	}
}

void FKismetAIRunOutcomeLedger::RetainPriorUnresolved()
{
	for (auto It = Packages.CreateIterator(); It; ++It)
	{
		FPackageState& State = It.Value();
		if (!State.Last.NeedsAttention() && !State.bEverRestoreIncomplete && !State.bEverSavedWithPreexistingChanges
			&& !(State.Last.bAppliedInMemory && State.Last.SaveStatus != EKismetAISaveStatus::Saved)
			&& !(State.bCompileRequired && State.Last.CompileStatus == EKismetAICompileStatus::NotRun))
		{
			It.RemoveCurrent();
			continue;
		}
		State.bPriorEvidence = true;
		State.PriorEvidenceRunId = State.EvidenceRunId;
		State.MutationCount = 0;
	}
}

TArray<FString> FKismetAIRunOutcomeLedger::GetUnverifiedPackages(bool bCurrentRunOnly) const
{
	TArray<FString> Out;
	for (const auto& Pair : Packages)
		if ((!bCurrentRunOnly || Pair.Value.MutationCount > 0)
			&& Pair.Value.bCompileRequired && Pair.Value.Last.CompileStatus == EKismetAICompileStatus::NotRun)
			Out.Add(Pair.Key);
	Out.Sort();
	return Out;
}

bool FKismetAIRunOutcomeLedger::HasUnresolvedEvidence() const
{
	return !GetUnsavedPackages().IsEmpty() || !GetCompileFailedPackages().IsEmpty()
		|| !GetUnverifiedPackages().IsEmpty() || !GetUnrestoredPackages().IsEmpty()
		|| !FailedOperations.IsEmpty() || !UnknownOperations.IsEmpty();
}

TSharedPtr<FJsonObject> FKismetAIRunOutcomeLedger::BuildCompletionSummary(const FString& TerminalOutcome) const
{
	TSharedPtr<FJsonObject> Out = BuildEventJson();
	Out->RemoveField(TEXT("type"));
	FString Status = TerminalOutcome == TEXT("completed") ? TEXT("complete") : TerminalOutcome;
	if (Status == TEXT("complete") && HasUnresolvedEvidence()) Status = TEXT("partial");
	if (!UnknownOperations.IsEmpty() || !GetUnrestoredPackages().IsEmpty()) Status = TEXT("uncertain");
	Out->SetStringField(TEXT("status"), Status);
	Out->SetStringField(TEXT("scope"), TEXT("executed_operations"));
	int32 Changed = 0;
	for (const auto& Pair : Packages) Changed += Pair.Value.MutationCount > 0 ? 1 : 0;
	FString Summary = Changed > 0
		? FString::Printf(TEXT("%d asset%s changed."), Changed, Changed == 1 ? TEXT("") : TEXT("s"))
		: TEXT("No asset changes verified.");
	if (HasUnresolvedEvidence()) Summary += TEXT(" Unresolved checks remain.");
	else if (Changed > 0) Summary += TEXT(" Save and required compile checks passed.");
	Out->SetStringField(TEXT("summary"), Summary);
	return Out;
}

TArray<FString> FKismetAIRunOutcomeLedger::GetUnsavedPackages() const
{
	TArray<FString> Out;
	for (const TPair<FString, FPackageState>& P : Packages)
	{
		if (P.Value.Last.SaveStatus == EKismetAISaveStatus::Failed
			|| (P.Value.Last.bAppliedInMemory && P.Value.Last.SaveStatus != EKismetAISaveStatus::Saved))
		{
			Out.Add(P.Key);
		}
	}
	return Out;
}

TArray<FString> FKismetAIRunOutcomeLedger::GetCompileFailedPackages() const
{
	TArray<FString> Out;
	for (const TPair<FString, FPackageState>& P : Packages)
	{
		if (P.Value.Last.CompileStatus == EKismetAICompileStatus::Failed)
		{
			Out.Add(P.Key);
		}
	}
	return Out;
}

TArray<FString> FKismetAIRunOutcomeLedger::GetPackagesSavedWithPreexistingChanges() const
{
	TArray<FString> Out;
	for (const TPair<FString, FPackageState>& P : Packages)
	{
		if (P.Value.bEverSavedWithPreexistingChanges)
		{
			Out.Add(P.Key);
		}
	}
	return Out;
}

TArray<FString> FKismetAIRunOutcomeLedger::GetUnrestoredPackages() const
{
	TArray<FString> Out;
	for (const TPair<FString, FPackageState>& P : Packages)
	{
		if (P.Value.bEverRestoreIncomplete)
		{
			Out.Add(P.Key);
		}
	}
	return Out;
}

bool FKismetAIRunOutcomeLedger::NeedsAttention() const
{
	if (HasUnresolvedEvidence()) return true;
	for (const TPair<FString, FPackageState>& P : Packages)
	{
		if (P.Value.Last.NeedsAttention() || P.Value.bEverSavedWithPreexistingChanges || P.Value.bEverRestoreIncomplete)
		{
			return true;
		}
	}
	return false;
}

TSharedPtr<FJsonObject> FKismetAIRunOutcomeLedger::BuildEventJson() const
{
	auto ToJsonArray = [](const TArray<FString>& In)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FString& S : In)
		{
			Arr.Add(MakeShared<FJsonValueString>(S));
		}
		return Arr;
	};

	TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetStringField(TEXT("type"), TEXT("mutation_outcomes"));
	J->SetBoolField(TEXT("needs_attention"), NeedsAttention());
	J->SetArrayField(TEXT("unsaved_packages"), ToJsonArray(GetUnsavedPackages()));
	J->SetArrayField(TEXT("compile_failed_packages"), ToJsonArray(GetCompileFailedPackages()));
	J->SetArrayField(TEXT("saved_with_preexisting_changes"), ToJsonArray(GetPackagesSavedWithPreexistingChanges()));
	J->SetArrayField(TEXT("unrestored_packages"), ToJsonArray(GetUnrestoredPackages()));
	J->SetArrayField(TEXT("unverified_packages"), ToJsonArray(GetUnverifiedPackages()));
	TArray<FString> Issues = UnknownOperations;
	for (const auto& Failure : FailedOperations) Issues.Add(Failure.Value);
	Issues.Sort();
	J->SetArrayField(TEXT("unknown_operations"), ToJsonArray(Issues));

	TArray<TSharedPtr<FJsonValue>> PackagesArr;
	TArray<FString> PackageNames;
	Packages.GetKeys(PackageNames);
	PackageNames.Sort();
	for (const FString& PackageName : PackageNames)
	{
		const TPair<FString, FPackageState> P(PackageName, Packages[PackageName]);
		TSharedPtr<FJsonObject> Entry = P.Value.Last.ToJson();
		Entry->SetStringField(TEXT("last_tool"), P.Value.LastTool);
		Entry->SetBoolField(TEXT("prior_evidence"), P.Value.bPriorEvidence);
		Entry->SetStringField(TEXT("evidence_run_id"), P.Value.EvidenceRunId);
		Entry->SetStringField(TEXT("prior_run_id"), P.Value.PriorEvidenceRunId);
		Entry->SetBoolField(TEXT("compile_required"), P.Value.bCompileRequired);
		FString Summary;
		if (P.Value.bPriorEvidence) Summary = P.Value.EvidenceRunId != P.Value.PriorEvidenceRunId
			? TEXT("Includes unresolved earlier-run state; ") : TEXT("Earlier run: ");
		Summary += P.Value.MutationCount > 0 ? TEXT("changed in memory")
			: P.Value.Last.bAppliedInMemory ? TEXT("earlier change") : TEXT("no change applied");
		if (P.Value.Last.CompileStatus == EKismetAICompileStatus::Succeeded) Summary += TEXT("; compile passed");
		else if (P.Value.Last.CompileStatus == EKismetAICompileStatus::Failed) Summary += TEXT("; compile failed");
		else if (P.Value.bCompileRequired) Summary += TEXT("; compile not verified");
		if (P.Value.Last.SaveStatus == EKismetAISaveStatus::Saved) Summary += TEXT("; saved.");
		else if (P.Value.Last.SaveStatus == EKismetAISaveStatus::Failed)
			Summary += TEXT("; not saved: ") + P.Value.Last.SaveError + TEXT(".");
		else if (P.Value.Last.bAppliedInMemory) Summary += TEXT("; not saved (") + DescribeSkipReason(P.Value.Last.SaveSkipReason) + TEXT(").");
		else Summary += TEXT(".");
		if (P.Value.bEverSavedWithPreexistingChanges) Summary += TEXT(" Save included pre-existing unsaved user changes.");
		if (P.Value.bEverRestoreIncomplete) Summary += TEXT(" Recovery incomplete; inspect before retrying.");
		else if (P.Value.Last.RecoveryStatus == EKismetAIRecoveryStatus::Restored) Summary += TEXT(" Latest batch rollback verified.");

		Entry->SetStringField(TEXT("summary"), Summary);
		Entry->SetNumberField(TEXT("mutation_count"), P.Value.MutationCount);
		Entry->SetBoolField(TEXT("ever_saved_with_preexisting_changes"), P.Value.bEverSavedWithPreexistingChanges);
		Entry->SetBoolField(TEXT("ever_restore_incomplete"), P.Value.bEverRestoreIncomplete);
		PackagesArr.Add(MakeShared<FJsonValueObject>(Entry));
	}
	J->SetArrayField(TEXT("packages"), PackagesArr);
	return J;
}
