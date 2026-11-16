// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIEditorMutation.h"
#include "KismetAIPersistence.h"
#include "Engine/Blueprint.h"
#include "Kismet2/CompilerResultsLog.h"

namespace KismetAIEditorMutation
{
// Mutation outcome helpers. A mutating handler records what it applied,
// how the compile went and what the save did, and attaches all of it to its
// success payload as `outcome` (see KismetAIMutationOutcome.h for the contract).
// The save itself goes through KismetAIPersistence, which reports failure
// instead of logging and returning void.
//
// Save policy (KismetAISavePolicy::AutosaveEvenIfCompileFailed): once a change
// is applied, the package is saved whether or not the Blueprint compiles. The
// outcome carries compile_status next to save_status, so a saved-but-broken
// graph is never reported as a clean edit. Eval mode (KISMETAI_EVAL_ALLOW)
// still never saves and now says so (save_status=not_requested, eval_mode).
FKismetAIMutationOutcome BeginMutationOutcome(bool bPackageWasDirtyBefore)
{
	FKismetAIMutationOutcome Outcome;
	Outcome.bPackageWasDirtyBefore = bPackageWasDirtyBefore;
	return Outcome;
}

// Record a compile that actually ran. Reads Blueprint->Status, so call it after
// CompileBlueprint and before anything else can change the status.
void RecordCompileOutcome(FKismetAIMutationOutcome& Outcome, const UBlueprint* Blueprint, const FCompilerResultsLog& Results)
{
	const bool bFailed = !Blueprint || Blueprint->Status == BS_Error;
	Outcome.CompileStatus = bFailed ? EKismetAICompileStatus::Failed : EKismetAICompileStatus::Succeeded;
	Outcome.CompileErrorCount = Results.NumErrors;
	if (bFailed && Outcome.CompileErrorCount == 0)
	{
		Outcome.CompileErrorCount = 1; // BS_Error with no logged message still failed
	}
}

// Persist an agent mutation to disk (unless eval mode / no file / nothing
// applied) and record the result. Non-interactive: never prompts for save or
// source-control checkout.
void AutoSaveAfterMutation(UObject* Asset, FKismetAIMutationOutcome& Outcome)
{
	KismetAIPersistence::SaveAfterMutation(Asset, Outcome);
}

// Attach the outcome as the first field of the payload so result truncation
// (which keeps the head) never drops it.
void AttachMutationOutcome(const TSharedPtr<FJsonObject>& Data, const FKismetAIMutationOutcome& Outcome)
{
	if (Data.IsValid())
	{
		Data->SetObjectField(TEXT("outcome"), Outcome.ToJson());
	}
}
}
