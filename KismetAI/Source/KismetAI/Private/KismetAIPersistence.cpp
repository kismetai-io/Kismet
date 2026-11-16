// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIPersistence.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "Misc/OutputDevice.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogKismetAIPersistence, Log, All);

namespace KismetAIPersistence
{
	namespace
	{
		// Game-thread-only state, like every caller of this file.
		TOptional<FSaveFunction> GSaveFunctionOverride;
		TOptional<bool> GEvalModeOverride;

		// Collects what SavePackage writes to its error device so the reason
		// reaches the tool result instead of only the log.
		class FSaveErrorCapture : public FOutputDevice
		{
		public:
			TArray<FString> Lines;
			virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
			{
				if (V && *V)
				{
					Lines.Add(FString(V));
				}
			}
		};

		const TCHAR* SaveResultToString(ESavePackageResult Result)
		{
			switch (Result)
			{
			case ESavePackageResult::Success:           return TEXT("Success");
			case ESavePackageResult::Canceled:          return TEXT("Canceled");
			case ESavePackageResult::ValidatorError:    return TEXT("ValidatorError");
			case ESavePackageResult::ValidatorSuppress: return TEXT("ValidatorSuppress");
			case ESavePackageResult::Timeout:           return TEXT("Timeout");
			case ESavePackageResult::MissingFile:       return TEXT("MissingFile");
			case ESavePackageResult::Error:
			default:                                    return TEXT("Error");
			}
		}

		bool DefaultSave(UPackage* Package, UObject* Asset, const FString& Filename, FString& OutError)
		{
			// A read-only target (source-control lock, file permissions) fails
			// inside SavePackage with only a log line; name it up front.
			IFileManager& FileManager = IFileManager::Get();
			if (FileManager.FileExists(*Filename) && FileManager.IsReadOnly(*Filename))
			{
				OutError = FString::Printf(
					TEXT("target file is read-only (checked out by source control? file permissions?): %s"), *Filename);
				return false;
			}

			FSaveErrorCapture Capture;
			FSavePackageArgs SaveArgs;
			SaveArgs.Error = &Capture;
			const FSavePackageResultStruct Result = UPackage::Save(Package, Asset, *Filename, SaveArgs);
			if (Result.IsSuccessful())
			{
				return true;
			}

			OutError = FString::Printf(TEXT("UPackage::Save returned %s"), SaveResultToString(Result.Result));
			if (Capture.Lines.Num() > 0)
			{
				OutError += TEXT(": ") + FString::Join(Capture.Lines, TEXT("; "));
			}
			if (FileManager.FileExists(*Filename) && FileManager.IsReadOnly(*Filename))
			{
				OutError += TEXT(" (target file is read-only)");
			}
			return false;
		}

		// Shared tail of both save entry points. Assumes the package is known and
		// file-backed.
		void RunSave(UObject* Asset, UPackage* Package, FKismetAIMutationOutcome& InOutOutcome)
		{
			const FString Filename = FPackageName::LongPackageNameToFilename(
				Package->GetName(), FPackageName::GetAssetPackageExtension());
			InOutOutcome.PackageFilename = FPaths::ConvertRelativePathToFull(Filename);

			FString Error;
			const bool bSaved = GSaveFunctionOverride.IsSet()
				? (*GSaveFunctionOverride)(Package, Asset, Filename, Error)
				: DefaultSave(Package, Asset, Filename, Error);

			if (bSaved)
			{
				InOutOutcome.SaveStatus = EKismetAISaveStatus::Saved;
				InOutOutcome.SaveError.Reset();
				return;
			}

			InOutOutcome.SaveStatus = EKismetAISaveStatus::Failed;
			InOutOutcome.SaveError = Error.IsEmpty() ? FString(TEXT("save failed for an unknown reason")) : Error;
			UE_LOG(LogKismetAIPersistence, Warning, TEXT("Save failed for %s: %s"),
				*Package->GetName(), *InOutOutcome.SaveError);
		}

		// Package identity + file backing. False (with the outcome filled) when
		// there is nothing on disk to write.
		UPackage* ResolveFileBackedPackage(UObject* Asset, FKismetAIMutationOutcome& InOutOutcome)
		{
			UPackage* Package = Asset ? Asset->GetOutermost() : nullptr;
			if (!Package)
			{
				InOutOutcome.SaveStatus = EKismetAISaveStatus::NotRequested;
				InOutOutcome.SaveSkipReason = KismetAISaveSkipReason::NoFileBacking;
				return nullptr;
			}
			const FString PackageName = Package->GetName();
			InOutOutcome.PackageName = PackageName;
			if (!FPackageName::IsValidLongPackageName(PackageName) || PackageName.StartsWith(TEXT("/Temp/")))
			{
				InOutOutcome.SaveStatus = EKismetAISaveStatus::NotRequested;
				InOutOutcome.SaveSkipReason = KismetAISaveSkipReason::NoFileBacking;
				return nullptr;
			}
			return Package;
		}
	}

	bool IsEvalNoSaveMode()
	{
		if (GEvalModeOverride.IsSet())
		{
			return GEvalModeOverride.GetValue();
		}
		// Eval-harness runs keep graph edits in memory only: autosaving burns
		// each sample's solution into the fixture .uasset, which pollutes the
		// committed baseline and defeats the framework's reload-based per-sample
		// reset. Same opt-in as the KismetAIEval module's :8324 listener gate.
		static const bool bEvalMode = []()
		{
			const FString OptIn = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMETAI_EVAL_ALLOW"));
			return OptIn == TEXT("1") || OptIn.ToLower() == TEXT("true");
		}();
		return bEvalMode;
	}

	bool IsPackageDirty(const UObject* Asset)
	{
		const UPackage* Package = Asset ? Asset->GetOutermost() : nullptr;
		return Package && Package->IsDirty();
	}

	void SaveAfterMutation(UObject* Asset, FKismetAIMutationOutcome& InOutOutcome)
	{
		ensure(IsInGameThread());
		InOutOutcome.SavePolicy = KismetAISavePolicy::AutosaveEvenIfCompileFailed;

		UPackage* Package = ResolveFileBackedPackage(Asset, InOutOutcome);
		if (!Package)
		{
			return;
		}
		if (!InOutOutcome.bAppliedInMemory)
		{
			InOutOutcome.SaveStatus = EKismetAISaveStatus::NotRequested;
			InOutOutcome.SaveSkipReason = KismetAISaveSkipReason::NothingApplied;
			return;
		}
		if (IsEvalNoSaveMode())
		{
			InOutOutcome.SaveStatus = EKismetAISaveStatus::NotRequested;
			InOutOutcome.SaveSkipReason = KismetAISaveSkipReason::EvalMode;
			return;
		}
		if (!Package->IsDirty())
		{
			InOutOutcome.SaveStatus = EKismetAISaveStatus::NotRequested;
			InOutOutcome.SaveSkipReason = KismetAISaveSkipReason::PackageNotDirty;
			return;
		}
		RunSave(Asset, Package, InOutOutcome);
	}

	void SaveExplicitly(UObject* Asset, FKismetAIMutationOutcome& InOutOutcome)
	{
		ensure(IsInGameThread());
		InOutOutcome.SavePolicy = KismetAISavePolicy::ExplicitSave;

		UPackage* Package = ResolveFileBackedPackage(Asset, InOutOutcome);
		if (!Package)
		{
			return;
		}
		RunSave(Asset, Package, InOutOutcome);
	}

	FScopedSaveFunctionOverride::FScopedSaveFunctionOverride(FSaveFunction InFunction)
	{
		bHadPrevious = GSaveFunctionOverride.IsSet();
		if (bHadPrevious)
		{
			Previous = GSaveFunctionOverride.GetValue();
		}
		GSaveFunctionOverride = MoveTemp(InFunction);
	}

	FScopedSaveFunctionOverride::~FScopedSaveFunctionOverride()
	{
		if (bHadPrevious)
		{
			GSaveFunctionOverride = MoveTemp(Previous);
		}
		else
		{
			GSaveFunctionOverride.Reset();
		}
	}

	FScopedEvalModeOverride::FScopedEvalModeOverride(bool bEvalMode)
	{
		Previous = GEvalModeOverride;
		GEvalModeOverride = bEvalMode;
	}

	FScopedEvalModeOverride::~FScopedEvalModeOverride()
	{
		GEvalModeOverride = Previous;
	}
}
