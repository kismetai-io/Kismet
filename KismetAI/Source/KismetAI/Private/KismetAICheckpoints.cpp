// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAICheckpoints.h"
#include "KismetAIAssetState.h"
#include "PackageTools.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "HAL/FileManager.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/LinkerLoad.h"
#include "UObject/ObjectVersion.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogKismetAICheckpoints, Log, All);

namespace
{
	const TCHAR* ManifestFormat = TEXT("kismet_checkpoint");
	constexpr int32 ManifestVersion = 1;
	const TCHAR* ManifestFile = TEXT("manifest.json");
	const TCHAR* CheckpointFile = TEXT("checkpoint.uasset");
	const TCHAR* ResultFile = TEXT("result.uasset");

	// Game-thread-only, like every caller.
	FKismetAICheckpointStore::EFault GFault = FKismetAICheckpointStore::EFault::None;

	class FErrorCapture : public FOutputDevice
	{
	public:
		TArray<FString> Lines;
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type, const FName&) override
		{
			if (V && *V) { Lines.Add(FString(V)); }
		}
	};

	FString EngineVersionString()
	{
		return FEngineVersion::Current().ToString(EVersionComponent::Changelist);
	}

	int32 PackageFileVersion()
	{
		return GPackageFileUEVersion.ToValue();
	}

	FString SafeSegment(const FString& In)
	{
		FString Out = FPaths::MakeValidFileName(In, TEXT('_'));
		Out.ReplaceInline(TEXT("/"), TEXT("_"));
		Out.ReplaceInline(TEXT("\\"), TEXT("_"));
		return Out.IsEmpty() ? FString(TEXT("_")) : Out;
	}

	// The package written as a copy: the editor's autosave flags, so the live
	// package keeps its loaded path and its dirty flag (SavePackage2.cpp,
	// IsUpdatingLoadedPath / IsKeepDirty).
	bool SaveCopy(UPackage* Package, const FString& Filename, FString& OutError)
	{
		UObject* Asset = Package ? Package->FindAssetInPackage() : nullptr;
		if (!Asset)
		{
			OutError = TEXT("the package holds no asset");
			return false;
		}
		// A Blueprint whose class is out of date would be saved with stale class
		// defaults that a normal load re-derives by compiling. Compile first, so
		// the copy loads the same way however it is loaded (a compile changes no
		// authored state).
		if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
		{
			if (Blueprint->Status == BS_Dirty || Blueprint->Status == BS_Unknown)
			{
				FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
			}
		}
		FErrorCapture Errors;
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Standalone;
		Args.SaveFlags = SAVE_FromAutosave | SAVE_KeepDirty | SAVE_NoError;
		Args.bWarnOfLongFilename = false;
		Args.Error = &Errors;
		const FSavePackageResultStruct Result = UPackage::Save(Package, Asset, *Filename, Args);
		if (!Result.IsSuccessful())
		{
			OutError = Errors.Lines.Num() > 0 ? FString::Join(Errors.Lines, TEXT("; ")) : FString(TEXT("UPackage::Save failed"));
			return false;
		}
		return true;
	}

	bool WriteBytes(const TArray<uint8>& Bytes, const FString& Filename)
	{
		return FFileHelper::SaveArrayToFile(Bytes, *Filename);
	}

	TSharedPtr<FJsonObject> ObjectField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Out = nullptr;
		return Obj.IsValid() && Obj->TryGetObjectField(Field, Out) && Out ? *Out : nullptr;
	}

	bool ReadFingerprint(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, FKismetAIAssetFingerprint& Out)
	{
		return FKismetAIAssetFingerprint::FromJson(ObjectField(Obj, Field), Out);
	}

	FString CurrentTime()
	{
		return FDateTime::UtcNow().ToIso8601();
	}

	// The state a saved copy holds is the state that LOADING it produces, which
	// can differ from the live state it was saved from in what loading derives
	// (a Blueprint compiled on load regenerates its class defaults). Restores
	// are verified load-to-load against this, and any difference from the live
	// state is kept as a disclosure.
	FKismetAIAssetFingerprint DescribeSavedCopy(const FString& File, const FString& PackagePath,
		const FKismetAIAssetFingerprint& Live, const TSharedPtr<FJsonObject>& Into)
	{
		const FKismetAIAssetFingerprint Loaded = KismetAIAssetState::CaptureFile(File, PackagePath);
		if (Loaded.HasStateEvidence())
		{
			Into->SetObjectField(TEXT("file_fingerprint"), Loaded.ToJson());
			TArray<TSharedPtr<FJsonValue>> Differences;
			if (!Loaded.SameState(Live))
			{
				for (const FString& Line : FKismetAIAssetFingerprint::DescribeDifferences(Live, Loaded))
				{
					Differences.Add(MakeShared<FJsonValueString>(Line));
				}
			}
			Into->SetArrayField(TEXT("load_differences"), Differences);
		}
		return Loaded;
	}
}

// ─────────────────────────────────────────────────────────────────────────────

const TCHAR* FKismetAIRestoreReport::StatusToString(EKismetAIRestoreStatus Status)
{
	switch (Status)
	{
	case EKismetAIRestoreStatus::Restored:               return TEXT("restored");
	case EKismetAIRestoreStatus::Conflict:               return TEXT("conflict");
	case EKismetAIRestoreStatus::CheckpointUnavailable:  return TEXT("checkpoint_unavailable");
	case EKismetAIRestoreStatus::CheckpointCorrupt:      return TEXT("checkpoint_corrupt");
	case EKismetAIRestoreStatus::CheckpointIncompatible: return TEXT("checkpoint_incompatible");
	case EKismetAIRestoreStatus::CheckpointUnverifiable: return TEXT("checkpoint_unverifiable");
	case EKismetAIRestoreStatus::Unsupported:            return TEXT("unsupported");
	case EKismetAIRestoreStatus::RestoreFailed:          return TEXT("restore_failed");
	case EKismetAIRestoreStatus::Unresolved:             return TEXT("unresolved");
	}
	return TEXT("unresolved");
}

TSharedPtr<FJsonObject> FKismetAIRestoreReport::ToJson() const
{
	TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetStringField(TEXT("asset"), AssetPath);
	J->SetStringField(TEXT("session_id"), SessionId);
	J->SetStringField(TEXT("run_id"), RunId);
	J->SetStringField(TEXT("status"), StatusToString(Status));
	J->SetBoolField(TEXT("restored"), Succeeded());
	J->SetStringField(TEXT("memory_state"), MemoryState);
	J->SetStringField(TEXT("disk_state"), DiskState);
	J->SetStringField(TEXT("explanation"), Explanation);
	TArray<TSharedPtr<FJsonValue>> Ev;
	for (const FString& E : Evidence) { Ev.Add(MakeShared<FJsonValueString>(E)); }
	J->SetArrayField(TEXT("evidence"), Ev);
	J->SetBoolField(TEXT("includes_previously_unsaved_edits"), bIncludesPreviouslyUnsavedEdits);
	return J;
}

// ─────────────────────────────────────────────────────────────────────────────

FKismetAICheckpointStore::FScopedFault::FScopedFault(EFault InFault) { GFault = InFault; }
FKismetAICheckpointStore::FScopedFault::~FScopedFault() { GFault = EFault::None; }

FKismetAICheckpointStore::FKismetAICheckpointStore(const FString& InRoot)
	: Root(InRoot.IsEmpty() ? FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("KismetAI") / TEXT("checkpoints")) : InRoot)
{
}

FString FKismetAICheckpointStore::AssetKey(const FString& PackagePath)
{
	const FString Lower = PackagePath.ToLower();
	FTCHARToUTF8 Utf8(*Lower);
	FSHA1 Sha;
	Sha.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	Sha.Final();
	uint8 Hash[FSHA1::DigestSize];
	Sha.GetHash(Hash);
	return SafeSegment(FPackageName::GetShortName(PackagePath)).Left(48) + TEXT("_") + BytesToHex(Hash, 4).ToLower();
}

FString FKismetAICheckpointStore::AssetDirectory(const FString& SessionId, const FString& RunId, const FString& PackagePath) const
{
	return Root / SafeSegment(SessionId) / SafeSegment(RunId) / AssetKey(PackagePath);
}

TSharedPtr<FJsonObject> FKismetAICheckpointStore::ReadManifest(const FString& Dir, FString& OutError) const
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *(Dir / ManifestFile)))
	{
		OutError = TEXT("missing");
		return nullptr;
	}
	TSharedPtr<FJsonObject> Manifest;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Manifest) || !Manifest.IsValid())
	{
		OutError = TEXT("the manifest is not valid JSON");
		return nullptr;
	}
	FString Format;
	int32 Version = 0;
	if (!Manifest->TryGetStringField(TEXT("format"), Format) || Format != ManifestFormat
		|| !Manifest->TryGetNumberField(TEXT("version"), Version) || Version != ManifestVersion)
	{
		OutError = TEXT("the manifest is not a kismet_checkpoint/1 manifest");
		return nullptr;
	}
	return Manifest;
}

bool FKismetAICheckpointStore::WriteManifest(const FString& Dir, const TSharedPtr<FJsonObject>& Manifest) const
{
	Manifest->SetStringField(TEXT("updated_at"), CurrentTime());
	FString Text;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(Manifest.ToSharedRef(), Writer);
	IFileManager::Get().MakeDirectory(*Dir, true);
	// Write then move, so a crash leaves the old manifest or the new one.
	const FString Temp = Dir / TEXT("manifest.json.tmp");
	if (!FFileHelper::SaveStringToFile(Text, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return false;
	}
	return IFileManager::Get().Move(*(Dir / ManifestFile), *Temp, /*bReplace=*/true);
}

void FKismetAICheckpointStore::AddHistory(const TSharedPtr<FJsonObject>& Manifest, const FString& Event, const FString& Detail) const
{
	TArray<TSharedPtr<FJsonValue>> History;
	const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
	if (Manifest->TryGetArrayField(TEXT("history"), Existing))
	{
		History = *Existing;
	}
	TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("at"), CurrentTime());
	Item->SetStringField(TEXT("event"), Event);
	if (!Detail.IsEmpty())
	{
		Item->SetStringField(TEXT("detail"), Detail);
	}
	History.Add(MakeShared<FJsonValueObject>(Item));
	Manifest->SetArrayField(TEXT("history"), History);
}

bool FKismetAICheckpointStore::CreateCheckpoint(const FString& Dir, const FString& SessionId, const FString& RunId,
	const FString& PackagePath, const FKismetAIAssetFingerprint& Current)
{
	TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();
	Manifest->SetStringField(TEXT("format"), ManifestFormat);
	Manifest->SetNumberField(TEXT("version"), ManifestVersion);
	Manifest->SetStringField(TEXT("session_id"), SessionId);
	Manifest->SetStringField(TEXT("run_id"), RunId);
	Manifest->SetStringField(TEXT("asset"), PackagePath);
	Manifest->SetStringField(TEXT("asset_class"), Current.AssetClass);
	Manifest->SetStringField(TEXT("created_at"), CurrentTime());
	Manifest->SetStringField(TEXT("engine_version"), EngineVersionString());
	Manifest->SetNumberField(TEXT("package_file_version"), PackageFileVersion());
	Manifest->SetStringField(TEXT("status"), TEXT("pending"));
	Manifest->SetBoolField(TEXT("foreign_change"), false);
	Manifest->SetObjectField(TEXT("last_agent_fingerprint"), Current.ToJson());

	FString Unrestorable;
	UPackage* Package = FindPackage(nullptr, *PackagePath);
	if (!Current.bCaptured)
	{
		Unrestorable = Current.UnsupportedReason.IsEmpty() ? FString(TEXT("the asset's state could not be captured")) : Current.UnsupportedReason;
	}
	else if (!Current.bExists)
	{
		Unrestorable = TEXT("the asset did not exist before the run (the run created it); there is no earlier state to restore");
	}
	else if (!Current.bSupported)
	{
		Unrestorable = Current.UnsupportedReason;
	}
	else if (PackagePath.StartsWith(TEXT("/Temp/")) || KismetAIAssetState::PackageFilename(PackagePath).IsEmpty())
	{
		Unrestorable = TEXT("the package has no file on disk");
	}
	else if (!Package)
	{
		Unrestorable = TEXT("the package is not loaded");
	}

	if (Unrestorable.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*Dir, true);
		FString Error;
		const FString File = Dir / CheckpointFile;
		FString Sha1;
		int64 Size = -1;
		if (!SaveCopy(Package, File, Error) || !KismetAIAssetState::HashFile(File, Sha1, Size))
		{
			Unrestorable = FString::Printf(TEXT("the checkpoint could not be written: %s"), *Error);
			IFileManager::Get().Delete(*File, false, true, true);
		}
		else
		{
			TSharedPtr<FJsonObject> Checkpoint = MakeShared<FJsonObject>();
			Checkpoint->SetStringField(TEXT("file"), CheckpointFile);
			Checkpoint->SetStringField(TEXT("sha1"), Sha1);
			Checkpoint->SetNumberField(TEXT("size"), (double)Size);
			Checkpoint->SetObjectField(TEXT("fingerprint"), Current.ToJson());
			// Verified at creation: the copy loads, and what it loads is recorded.
			if (!DescribeSavedCopy(File, PackagePath, Current, Checkpoint).HasStateEvidence())
			{
				Unrestorable = TEXT("the checkpoint was written but could not be loaded back for verification");
				IFileManager::Get().Delete(*File, false, true, true);
			}
			else
			{
				Manifest->SetObjectField(TEXT("checkpoint"), Checkpoint);
				Manifest->SetBoolField(TEXT("package_was_dirty"), Current.bPackageDirty);
			}
		}
	}

	Manifest->SetBoolField(TEXT("restorable"), Unrestorable.IsEmpty());
	if (!Unrestorable.IsEmpty())
	{
		Manifest->SetStringField(TEXT("unrestorable_reason"), Unrestorable);
	}
	AddHistory(Manifest, TEXT("created"), Unrestorable);
	if (!WriteManifest(Dir, Manifest))
	{
		UE_LOG(LogKismetAICheckpoints, Warning, TEXT("Could not write the checkpoint manifest for %s in %s"), *PackagePath, *Dir);
		return false;
	}
	UE_LOG(LogKismetAICheckpoints, Log, TEXT("Checkpoint for %s (run %s): %s"), *PackagePath, *RunId,
		Unrestorable.IsEmpty() ? TEXT("written") : *Unrestorable);
	return true;
}

void FKismetAICheckpointStore::BeginMutation(const FString& SessionId, const FString& RunId, const FString& PackagePath,
	const FKismetAIAssetFingerprint& Current)
{
	if (SessionId.IsEmpty() || RunId.IsEmpty() || PackagePath.IsEmpty())
	{
		return;
	}
	const FString Dir = AssetDirectory(SessionId, RunId, PackagePath);
	FString Error;
	TSharedPtr<FJsonObject> Manifest = ReadManifest(Dir, Error);
	if (!Manifest.IsValid())
	{
		if (Error == TEXT("missing"))
		{
			CreateCheckpoint(Dir, SessionId, RunId, PackagePath, Current);
		}
		return;
	}

	FString Status;
	Manifest->TryGetStringField(TEXT("status"), Status);
	FKismetAIAssetFingerprint Last;
	if (Status != TEXT("pending") || !ReadFingerprint(Manifest, TEXT("last_agent_fingerprint"), Last))
	{
		return;
	}
	// Between two of the run's calls the asset should be exactly as the run
	// left it. Anything else was done by someone else, and a restore to the
	// pre-run state would throw it away.
	if (Last.HasStateEvidence() && !Last.SameState(Current))
	{
		TArray<TSharedPtr<FJsonValue>> Evidence;
		const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
		if (Manifest->TryGetArrayField(TEXT("foreign_evidence"), Existing))
		{
			Evidence = *Existing;
		}
		for (const FString& Line : FKismetAIAssetFingerprint::DescribeDifferences(Last, Current))
		{
			Evidence.Add(MakeShared<FJsonValueString>(Line));
		}
		Manifest->SetBoolField(TEXT("foreign_change"), true);
		Manifest->SetArrayField(TEXT("foreign_evidence"), Evidence);
		AddHistory(Manifest, TEXT("foreign_change"), TEXT("the asset changed between two of the run's calls"));
		WriteManifest(Dir, Manifest);
		UE_LOG(LogKismetAICheckpoints, Warning, TEXT("%s changed outside run %s; its checkpoint will not be restored over that change"),
			*PackagePath, *RunId);
	}
}

void FKismetAICheckpointStore::EndMutation(const FString& SessionId, const FString& RunId, const FString& PackagePath,
	const FKismetAIAssetFingerprint& Current)
{
	const FString Dir = AssetDirectory(SessionId, RunId, PackagePath);
	FString Error;
	TSharedPtr<FJsonObject> Manifest = ReadManifest(Dir, Error);
	FString Status;
	if (!Manifest.IsValid() || !Manifest->TryGetStringField(TEXT("status"), Status) || Status != TEXT("pending"))
	{
		return;
	}
	Manifest->SetObjectField(TEXT("last_agent_fingerprint"), Current.ToJson());
	WriteManifest(Dir, Manifest);
}

FKismetAIRestoreReport FKismetAICheckpointStore::Restore(const FString& SessionId, const FString& RunId,
	const FString& PackagePath, ETarget Target)
{
	FKismetAIRestoreReport Report;
	Report.SessionId = SessionId;
	Report.RunId = RunId;
	Report.AssetPath = PackagePath;
	Report.MemoryState = TEXT("unchanged");
	Report.DiskState = TEXT("unchanged");
	const bool bBeforeRun = Target == ETarget::BeforeRun;

	const FString Dir = AssetDirectory(SessionId, RunId, PackagePath);
	FString ManifestError;
	TSharedPtr<FJsonObject> Manifest = ReadManifest(Dir, ManifestError);
	auto Finish = [&](EKismetAIRestoreStatus Status, const FString& Explanation) -> FKismetAIRestoreReport
	{
		Report.Status = Status;
		Report.Explanation = Explanation;
		UE_LOG(LogKismetAICheckpoints, Log, TEXT("Restore %s (%s) of %s: %s - %s%s%s"), bBeforeRun ? TEXT("before-run") : TEXT("run-result"),
			*RunId, *PackagePath, FKismetAIRestoreReport::StatusToString(Status), *Explanation,
			Report.Evidence.Num() > 0 ? TEXT(" Evidence: ") : TEXT(""), *FString::Join(Report.Evidence, TEXT("; ")));
		if (Manifest.IsValid())
		{
			AddHistory(Manifest, bBeforeRun ? TEXT("restore") : TEXT("reapply"),
				FString::Printf(TEXT("%s: %s"), FKismetAIRestoreReport::StatusToString(Status), *Explanation));
			if (Status == EKismetAIRestoreStatus::RestoreFailed || Status == EKismetAIRestoreStatus::Unresolved)
			{
				Manifest->SetStringField(TEXT("status"), Status == EKismetAIRestoreStatus::Unresolved ? TEXT("unresolved") : TEXT("restore_failed"));
			}
			WriteManifest(Dir, Manifest);
		}
		return Report;
	};

	// 1. The manifest.
	if (!Manifest.IsValid())
	{
		if (ManifestError == TEXT("missing"))
		{
			return Finish(EKismetAIRestoreStatus::CheckpointUnavailable,
				TEXT("No checkpoint was recorded for this asset in this run (it was deleted, or never written). Nothing was restored."));
		}
		return Finish(EKismetAIRestoreStatus::CheckpointCorrupt,
			FString::Printf(TEXT("The checkpoint manifest cannot be read (%s). Nothing was restored."), *ManifestError));
	}
	FString EngineVersion;
	int32 FileVersion = 0;
	Manifest->TryGetStringField(TEXT("engine_version"), EngineVersion);
	Manifest->TryGetNumberField(TEXT("package_file_version"), FileVersion);
	if (EngineVersion != EngineVersionString() || FileVersion != PackageFileVersion())
	{
		return Finish(EKismetAIRestoreStatus::CheckpointIncompatible, FString::Printf(
			TEXT("The checkpoint was written by engine %s (package version %d); this editor is %s (%d). Nothing was restored."),
			*EngineVersion, FileVersion, *EngineVersionString(), PackageFileVersion()));
	}
	FString Status;
	Manifest->TryGetStringField(TEXT("status"), Status);
	if (Status == TEXT("accepted"))
	{
		return Finish(EKismetAIRestoreStatus::CheckpointUnavailable, TEXT("The run's changes were accepted and the checkpoint deleted. Nothing was restored."));
	}
	bool bRestorable = false;
	Manifest->TryGetBoolField(TEXT("restorable"), bRestorable);
	if (!bRestorable)
	{
		FString Reason;
		Manifest->TryGetStringField(TEXT("unrestorable_reason"), Reason);
		return Finish(EKismetAIRestoreStatus::Unsupported, FString::Printf(TEXT("This asset has no restorable checkpoint: %s. Nothing was restored."), *Reason));
	}
	if (!bBeforeRun && Status != TEXT("restored"))
	{
		return Finish(EKismetAIRestoreStatus::Conflict, TEXT("Reapply needs a completed restore of this run first. Nothing was changed."));
	}

	// 2. The file to put back, and the state it must hold.
	const TSharedPtr<FJsonObject> Source = ObjectField(Manifest, bBeforeRun ? TEXT("checkpoint") : TEXT("result"));
	FKismetAIAssetFingerprint TargetState;      // live state the copy was saved from
	FKismetAIAssetFingerprint TargetFileState;  // what loading the copy produces
	FString ExpectedSha1;
	if (!Source.IsValid() || !ReadFingerprint(Source, TEXT("fingerprint"), TargetState)
		|| !ReadFingerprint(Source, TEXT("file_fingerprint"), TargetFileState) || !Source->TryGetStringField(TEXT("sha1"), ExpectedSha1))
	{
		return Finish(bBeforeRun ? EKismetAIRestoreStatus::CheckpointCorrupt : EKismetAIRestoreStatus::CheckpointUnavailable,
			bBeforeRun ? TEXT("The manifest does not describe the checkpoint file. Nothing was restored.")
				: TEXT("No copy of the run's result was kept. Nothing was changed."));
	}
	const FString SourceFile = Dir / (bBeforeRun ? CheckpointFile : ResultFile);
	TArray<uint8> SourceBytes;
	if (!FFileHelper::LoadFileToArray(SourceBytes, *SourceFile, FILEREAD_Silent))
	{
		return Finish(EKismetAIRestoreStatus::CheckpointUnavailable,
			FString::Printf(TEXT("The checkpoint file %s is missing. Nothing was restored."), *SourceFile));
	}
	{
		FString Sha1;
		int64 Size = -1;
		KismetAIAssetState::HashFile(SourceFile, Sha1, Size);
		if (Sha1 != ExpectedSha1)
		{
			return Finish(EKismetAIRestoreStatus::CheckpointCorrupt,
				FString::Printf(TEXT("The checkpoint file does not match its recorded SHA-1 (expected %s, found %s). Nothing was restored."), *ExpectedSha1, *Sha1));
		}
	}

	// 3. The asset must be exactly as the run left it (or, for Reapply, as the
	// restore left it). Otherwise putting the old state back discards work
	// nobody asked to discard.
	bool bForeign = false;
	Manifest->TryGetBoolField(TEXT("foreign_change"), bForeign);
	if (bBeforeRun && bForeign)
	{
		const TArray<TSharedPtr<FJsonValue>>* Evidence = nullptr;
		if (Manifest->TryGetArrayField(TEXT("foreign_evidence"), Evidence))
		{
			for (const TSharedPtr<FJsonValue>& V : *Evidence) { Report.Evidence.Add(V->AsString()); }
		}
		return Finish(EKismetAIRestoreStatus::Conflict,
			TEXT("The asset was changed outside the run while the run was editing it. Restoring the checkpoint would discard that change, so nothing was restored. Undo the run's edits by hand or with Edit > Undo."));
	}
	FKismetAIAssetFingerprint Expected;
	if (bBeforeRun)
	{
		ReadFingerprint(Manifest, TEXT("last_agent_fingerprint"), Expected);
	}
	else
	{
		// After a restore the asset was reloaded from the checkpoint copy.
		ReadFingerprint(ObjectField(Manifest, TEXT("checkpoint")), TEXT("file_fingerprint"), Expected);
	}
	const FKismetAIAssetFingerprint Current = KismetAIAssetState::Capture(PackagePath);
	if ((Current.SameState(TargetState) || Current.SameState(TargetFileState)) && !Current.SameState(Expected))
	{
		// Already there: e.g. the run's edits were never saved and the editor
		// restarted, so the asset loaded in its pre-run state. Proven, not
		// assumed: the file must describe the same state.
		const FString CurrentFile = KismetAIAssetState::PackageFilename(PackagePath);
		const FKismetAIAssetFingerprint OnDisk = CurrentFile.IsEmpty() ? FKismetAIAssetFingerprint() : KismetAIAssetState::CaptureFile(CurrentFile, PackagePath);
		if (OnDisk.SameState(TargetFileState))
		{
			Report.MemoryState = bBeforeRun ? TEXT("checkpoint (already)") : TEXT("run result (already)");
			Report.DiskState = Report.MemoryState;
			Manifest->SetStringField(TEXT("status"), bBeforeRun ? TEXT("restored") : TEXT("pending"));
			return Finish(EKismetAIRestoreStatus::Restored,
				TEXT("The asset already equals this state in memory and on disk (the run's unsaved edits were lost, or undone). Nothing was changed."));
		}
	}
	if (!Expected.SameState(Current))
	{
		Report.Evidence = FKismetAIAssetFingerprint::DescribeDifferences(Expected, Current);
		return Finish(EKismetAIRestoreStatus::Conflict, bBeforeRun
			? TEXT("The asset changed after the run's last edit (by you, another tool, or a later run). Restoring the checkpoint would discard that work, so nothing was restored.")
			: TEXT("The asset changed after the restore. Reapplying would discard that work, so nothing was changed."));
	}

	// 4. The file really holds the state it was taken from.
	const FKismetAIAssetFingerprint InFile = KismetAIAssetState::CaptureFile(SourceFile, PackagePath);
	if (!InFile.HasStateEvidence())
	{
		return Finish(EKismetAIRestoreStatus::CheckpointUnverifiable,
			FString::Printf(TEXT("The checkpoint file could not be loaded for verification (%s). Nothing was restored."), *InFile.UnsupportedReason));
	}
	if (!InFile.SameState(TargetFileState))
	{
		Report.Evidence = FKismetAIAssetFingerprint::DescribeDifferences(TargetFileState, InFile);
		return Finish(EKismetAIRestoreStatus::CheckpointUnverifiable,
			TEXT("The checkpoint file does not describe the state recorded when it was taken. Nothing was restored."));
	}

	UPackage* Package = FindPackage(nullptr, *PackagePath);
	const FString PackageFile = KismetAIAssetState::PackageFilename(PackagePath);
	if (!Package || PackageFile.IsEmpty())
	{
		return Finish(EKismetAIRestoreStatus::Unsupported, TEXT("The package is not loaded or has no file. Nothing was restored."));
	}

	// Keep the run's result, so Reapply can put it back.
	if (bBeforeRun)
	{
		FString Error, Sha1;
		int64 Size = -1;
		const FString File = Dir / ResultFile;
		if (SaveCopy(Package, File, Error) && KismetAIAssetState::HashFile(File, Sha1, Size))
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("file"), ResultFile);
			Result->SetStringField(TEXT("sha1"), Sha1);
			Result->SetNumberField(TEXT("size"), (double)Size);
			Result->SetObjectField(TEXT("fingerprint"), Current.ToJson());
			if (DescribeSavedCopy(File, PackagePath, Current, Result).HasStateEvidence())
			{
				Manifest->SetObjectField(TEXT("result"), Result);
			}
			else
			{
				Manifest->RemoveField(TEXT("result"));
				AddHistory(Manifest, TEXT("result_not_kept"), TEXT("the copy of the run's result could not be loaded back"));
			}
		}
		else
		{
			Manifest->RemoveField(TEXT("result"));
			AddHistory(Manifest, TEXT("result_not_kept"), Error);
		}
	}

	// 5. Replace the file and make memory follow it. The loader is detached
	// first (as source control revert does), so the live objects never read
	// from a file that changed beneath them.
	TArray<uint8> PreviousBytes;
	const bool bHadFile = FFileHelper::LoadFileToArray(PreviousBytes, *PackageFile, FILEREAD_Silent);
	Package->FullyLoad();
	ResetLoaders(Package);

	auto PutPreviousFileBack = [&]() -> bool
	{
		if (GFault == EFault::ReloadAndRollbackFail)
		{
			return false;
		}
		return bHadFile ? WriteBytes(PreviousBytes, PackageFile) : IFileManager::Get().Delete(*PackageFile, false, true, true);
	};

	const bool bWritten = GFault != EFault::WriteFails && WriteBytes(SourceBytes, PackageFile);
	if (!bWritten)
	{
		const bool bBack = PutPreviousFileBack();
		Report.MemoryState = TEXT("unchanged (the run's result)");
		Report.DiskState = bBack ? TEXT("unchanged") : TEXT("unknown");
		return Finish(bBack ? EKismetAIRestoreStatus::RestoreFailed : EKismetAIRestoreStatus::Unresolved,
			FString::Printf(TEXT("The package file %s could not be written (read-only, locked or disk full?). The editor still holds the run's result."), *PackageFile));
	}
	Report.DiskState = bBeforeRun ? TEXT("checkpoint") : TEXT("run result");

	FText ReloadError;
	bool bReloaded = false;
	if (GFault != EFault::ReloadFails && GFault != EFault::ReloadAndRollbackFail)
	{
		UPackageTools::ReloadPackages({ Package }, ReloadError, EReloadPackagesInteractionMode::AssumePositive);
		bReloaded = ReloadError.IsEmpty();
	}
	else
	{
		ReloadError = FText::FromString(TEXT("reload refused (test fault)"));
	}
	if (!bReloaded)
	{
		const bool bBack = PutPreviousFileBack();
		Report.MemoryState = TEXT("the run's result (reload did not happen)");
		if (bBack)
		{
			Report.DiskState = TEXT("unchanged (the previous file was put back)");
			return Finish(EKismetAIRestoreStatus::RestoreFailed, FString::Printf(
				TEXT("The package could not be reloaded from the checkpoint (%s). The previous file was put back, so memory and disk both still hold what they held before."),
				*ReloadError.ToString()));
		}
		Report.DiskState = bBeforeRun ? TEXT("checkpoint") : TEXT("run result");
		return Finish(EKismetAIRestoreStatus::Unresolved, FString::Printf(
			TEXT("The package could not be reloaded (%s) and the previous file could not be put back. Memory holds the run's result, the file on disk holds the %s: they differ. Do not save this asset until you have decided which one to keep."),
			*ReloadError.ToString(), bBeforeRun ? TEXT("checkpoint") : TEXT("run result")));
	}

	// 6. Verify: the reloaded asset and the file must describe the target state.
	const FKismetAIAssetFingerprint After = KismetAIAssetState::Capture(PackagePath);
	Report.MemoryState = TEXT("reloaded from disk");
	if (!After.SameState(TargetFileState) || After.FileSha1 != ExpectedSha1)
	{
		Report.Evidence = FKismetAIAssetFingerprint::DescribeDifferences(TargetFileState, After);
		if (After.FileSha1 != ExpectedSha1)
		{
			Report.Evidence.Add(TEXT("the package file does not have the checkpoint's SHA-1"));
		}
		return Finish(EKismetAIRestoreStatus::Unresolved,
			TEXT("The package was reloaded from the restored file, but what loaded does not match the checkpoint. Memory and disk agree with each other; neither is proven to be the pre-run state."));
	}

	Report.MemoryState = bBeforeRun ? TEXT("checkpoint") : TEXT("run result");
	// What loading derived differently from the live state the copy was saved
	// from (e.g. class defaults regenerated by a compile on load) is disclosed.
	const TArray<TSharedPtr<FJsonValue>>* LoadDifferences = nullptr;
	if (Source->TryGetArrayField(TEXT("load_differences"), LoadDifferences))
	{
		for (const TSharedPtr<FJsonValue>& V : *LoadDifferences)
		{
			Report.Evidence.Add(TEXT("re-derived on load: ") + V->AsString());
		}
	}
	bool bWasDirty = false;
	Manifest->TryGetBoolField(TEXT("package_was_dirty"), bWasDirty);
	Report.bIncludesPreviouslyUnsavedEdits = bBeforeRun && bWasDirty;
	if (bBeforeRun)
	{
		Manifest->SetStringField(TEXT("status"), TEXT("restored"));
	}
	else
	{
		Manifest->SetStringField(TEXT("status"), TEXT("pending"));
		Manifest->SetObjectField(TEXT("last_agent_fingerprint"), After.ToJson());
	}
	return Finish(EKismetAIRestoreStatus::Restored, bBeforeRun
		? (Report.bIncludesPreviouslyUnsavedEdits
			? TEXT("Restored to the state before the run, verified in memory and on disk. That state included edits that were unsaved when the run started; they are now saved.")
			: TEXT("Restored to the state before the run, verified in memory and on disk."))
		: TEXT("The run's result was put back, verified in memory and on disk."));
}

TArray<FKismetAICheckpointInfo> FKismetAICheckpointStore::ListRun(const FString& SessionId, const FString& RunId) const
{
	TArray<FKismetAICheckpointInfo> Out;
	const FString RunDir = Root / SafeSegment(SessionId) / SafeSegment(RunId);
	if (SessionId.IsEmpty() || RunId.IsEmpty())
	{
		return Out;
	}
	TArray<FString> AssetDirs;
	IFileManager::Get().FindFiles(AssetDirs, *(RunDir / TEXT("*")), false, true);
	for (const FString& Name : AssetDirs)
	{
		const FString Dir = RunDir / Name;
		FString Error;
		const TSharedPtr<FJsonObject> Manifest = ReadManifest(Dir, Error);
		FKismetAICheckpointInfo Info;
		Info.SessionId = SessionId;
		Info.RunId = RunId;
		Info.Directory = Dir;
		if (!Manifest.IsValid())
		{
			Info.Status = TEXT("corrupt");
			Out.Add(MoveTemp(Info));
			continue;
		}
		Manifest->TryGetStringField(TEXT("session_id"), Info.SessionId);
		Manifest->TryGetStringField(TEXT("run_id"), Info.RunId);
		Manifest->TryGetStringField(TEXT("asset"), Info.AssetPath);
		Manifest->TryGetStringField(TEXT("status"), Info.Status);
		Manifest->TryGetBoolField(TEXT("restorable"), Info.bRestorable);
		FKismetAIAssetFingerprint Last, Before;
		Info.bChangedByRun = ReadFingerprint(Manifest, TEXT("last_agent_fingerprint"), Last)
			&& ReadFingerprint(ObjectField(Manifest, TEXT("checkpoint")), TEXT("fingerprint"), Before)
			&& !Last.SameState(Before);
		FString Created;
		Manifest->TryGetStringField(TEXT("created_at"), Created);
		FDateTime::ParseIso8601(*Created, Info.CreatedAt);
		Out.Add(MoveTemp(Info));
	}
	return Out;
}

TArray<FKismetAICheckpointInfo> FKismetAICheckpointStore::ListPending() const
{
	TArray<FKismetAICheckpointInfo> Out;
	TArray<FString> Sessions;
	IFileManager::Get().FindFiles(Sessions, *(Root / TEXT("*")), false, true);
	for (const FString& Session : Sessions)
	{
		TArray<FString> Runs;
		IFileManager::Get().FindFiles(Runs, *(Root / Session / TEXT("*")), false, true);
		for (const FString& Run : Runs)
		{
			for (FKismetAICheckpointInfo& Info : ListRun(Session, Run))
			{
				if (Info.Status == TEXT("pending") || Info.Status == TEXT("restore_failed") || Info.Status == TEXT("unresolved"))
				{
					Out.Add(MoveTemp(Info));
				}
			}
		}
	}
	Out.Sort([](const FKismetAICheckpointInfo& A, const FKismetAICheckpointInfo& B) { return A.CreatedAt > B.CreatedAt; });
	return Out;
}

FString FKismetAICheckpointStore::FindLatestPendingRun(const FString& SessionId) const
{
	for (const FKismetAICheckpointInfo& Info : ListPending())
	{
		if (Info.SessionId == SessionId)
		{
			return Info.RunId;
		}
	}
	return FString();
}

void FKismetAICheckpointStore::Accept(const FString& SessionId, const FString& RunId)
{
	for (const FKismetAICheckpointInfo& Info : ListRun(SessionId, RunId))
	{
		FString Error;
		TSharedPtr<FJsonObject> Manifest = ReadManifest(Info.Directory, Error);
		IFileManager::Get().Delete(*(Info.Directory / CheckpointFile), false, true, true);
		IFileManager::Get().Delete(*(Info.Directory / ResultFile), false, true, true);
		if (Manifest.IsValid())
		{
			Manifest->SetStringField(TEXT("status"), TEXT("accepted"));
			AddHistory(Manifest, TEXT("accepted"), FString());
			WriteManifest(Info.Directory, Manifest);
		}
	}
}

int32 FKismetAICheckpointStore::Prune(FTimespan MaxAge)
{
	int32 Removed = 0;
	const FDateTime Now = FDateTime::UtcNow();
	TArray<FString> Sessions;
	IFileManager::Get().FindFiles(Sessions, *(Root / TEXT("*")), false, true);
	for (const FString& Session : Sessions)
	{
		TArray<FString> Runs;
		IFileManager::Get().FindFiles(Runs, *(Root / Session / TEXT("*")), false, true);
		for (const FString& Run : Runs)
		{
			const TArray<FKismetAICheckpointInfo> Infos = ListRun(Session, Run);
			bool bAllResolved = Infos.Num() > 0;
			bool bAllOld = true;
			for (const FKismetAICheckpointInfo& Info : Infos)
			{
				bAllResolved &= Info.Status == TEXT("accepted") || Info.Status == TEXT("restored");
				bAllOld &= Info.CreatedAt.GetTicks() > 0 && Now - Info.CreatedAt > MaxAge;
			}
			if (bAllResolved || (Infos.Num() > 0 && bAllOld))
			{
				IFileManager::Get().DeleteDirectory(*(Root / Session / Run), false, true);
				++Removed;
			}
		}
		TArray<FString> Left;
		IFileManager::Get().FindFiles(Left, *(Root / Session / TEXT("*")), true, true);
		if (Left.Num() == 0)
		{
			IFileManager::Get().DeleteDirectory(*(Root / Session), false, true);
		}
	}
	return Removed;
}
