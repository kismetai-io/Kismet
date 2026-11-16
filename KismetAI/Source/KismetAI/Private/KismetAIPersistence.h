// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Editor-side save boundary for agent mutations.
//
// Every tool that persists an agent edit goes through SaveAfterMutation /
// SaveExplicitly, which return what actually happened instead of logging a
// warning and returning void. The save call itself is behind an overridable
// function so tests can inject a failure without a read-only file, and the
// eval-mode gate can be forced so "not_requested" is testable without the
// environment variable.
//
// Game thread only: these touch UPackage.

#pragma once

#include "CoreMinimal.h"
#include "KismetAIMutationOutcome.h"

class UObject;
class UPackage;

namespace KismetAIPersistence
{
	// Writes Package (with Asset as its primary object) to Filename. Returns
	// false and sets OutError on failure. The default wraps UPackage::Save.
	using FSaveFunction = TFunction<bool(UPackage* Package, UObject* Asset, const FString& Filename, FString& OutError)>;

	// KISMETAI_EVAL_ALLOW=1|true (or a test override): mutations stay in memory.
	bool IsEvalNoSaveMode();

	// Whether the asset's package has unsaved changes. Capture this BEFORE the
	// tool mutates anything; it drives the pre-existing-changes disclosure.
	bool IsPackageDirty(const UObject* Asset);

	// Fills the save fields of InOutOutcome (status, reason/error, package,
	// filename, policy) for an autosave after an agent mutation. Callers set
	// bAppliedInMemory, compile status and bPackageWasDirtyBefore. When nothing
	// was applied no save is attempted.
	void SaveAfterMutation(UObject* Asset, FKismetAIMutationOutcome& InOutOutcome);

	// The same save, requested explicitly (manage_asset action=save). Not
	// gated by eval mode: the caller asked for it. Saves even a clean package.
	void SaveExplicitly(UObject* Asset, FKismetAIMutationOutcome& InOutOutcome);

	// Test seams. Scoped so a failing test cannot leak an override into the
	// next one.
	struct FScopedSaveFunctionOverride
	{
		explicit FScopedSaveFunctionOverride(FSaveFunction InFunction);
		~FScopedSaveFunctionOverride();
		FScopedSaveFunctionOverride(const FScopedSaveFunctionOverride&) = delete;
		FScopedSaveFunctionOverride& operator=(const FScopedSaveFunctionOverride&) = delete;
	private:
		FSaveFunction Previous;
		bool bHadPrevious = false;
	};

	struct FScopedEvalModeOverride
	{
		explicit FScopedEvalModeOverride(bool bEvalMode);
		~FScopedEvalModeOverride();
		FScopedEvalModeOverride(const FScopedEvalModeOverride&) = delete;
		FScopedEvalModeOverride& operator=(const FScopedEvalModeOverride&) = delete;
	private:
		TOptional<bool> Previous;
	};
}
