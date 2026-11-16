// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "KismetAIMutationOutcome.h"

class UObject;
class UBlueprint;
class FCompilerResultsLog;

// Shared result/save recording for editor domains. UObject access is game-thread-only.
namespace KismetAIEditorMutation
{
	FKismetAIMutationOutcome BeginMutationOutcome(bool bPackageWasDirtyBefore);
	void RecordCompileOutcome(FKismetAIMutationOutcome& Outcome, const UBlueprint* Blueprint, const FCompilerResultsLog& Results);
	void AutoSaveAfterMutation(UObject* Asset, FKismetAIMutationOutcome& Outcome);
	void AttachMutationOutcome(const TSharedPtr<FJsonObject>& Data, const FKismetAIMutationOutcome& Outcome);
}
