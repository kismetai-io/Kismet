// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIUpdater.h"
#include "KismetAILog.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "FileUtilities/ZipArchiveReader.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UnrealEdMisc.h"

DEFINE_LOG_CATEGORY(LogKismetAIUpdater);

// -----------------------------------------------------------------------------
// Manifest source
//
// KISMETAI_MANIFEST_URL and KISMETAI_RELEASE_CHANNEL are injected by
// KismetAI.Build.cs (ConfigureUpdaterEndpoint). Shipping/Test always
// resolve to prod; other configs honor KISMETAI_CHANNEL=dev.
//
// The updater fetches a manifest from that URL. Schema:
//   {
//     "latest": {
//       "version": "0.1.7",
//       "notes":   "free-form markdown shown to the user before install",
//       "assets": [
//         {
//           "engine":   "5.6",      // matches FEngineVersion Major.Minor
//           "platform": "Mac",      // one of Win64 | Mac | Linux
//           "url":      "https://...KismetAI-0.1.7-UE5.6-Mac-release.zip",
//           "size":     12345678    // optional, informational
//         },
//         { ... }
//       ]
//     }
//   }
//
// CheckAsync picks the asset whose engine == running editor's Major.Minor AND
// platform == FPlatformProperties::IniPlatformName-derived name. If no match,
// bHasUpdate is true but bHasMatchingAsset is false.
// -----------------------------------------------------------------------------
#ifndef KISMETAI_MANIFEST_URL
#error "KISMETAI_MANIFEST_URL must be defined by KismetAI.Build.cs"
#endif
#ifndef KISMETAI_SELF_UPDATE
#error "KISMETAI_SELF_UPDATE must be defined by KismetAI.Build.cs"
#endif

bool FKismetAIUpdater::IsSelfUpdateEnabled()
{
	return KISMETAI_SELF_UPDATE != 0;
}

FString FKismetAIUpdater::GetSelfUpdateDisabledMessage()
{
	return TEXT("This copy of Kismet was installed from Fab. Update it from the Epic Games Launcher.");
}

namespace
{
	// Re-entry guard for InstallAsync. One install at a time. The lock only
	// covers the in-progress flip; subsequent writes to bInstallInProgress in
	// the network/thread-pool completion path are unlocked. That's safe in
	// practice because all CheckForUpdate/InstallUpdate entry points are
	// invoked from the JS bridge on the game thread, so there is no concurrent
	// caller of InstallAsync to race against.
	static FCriticalSection InstallLock;
	static bool bInstallInProgress = false;

	// Strip a leading 'v' from semver tags. "v0.1.7" -> "0.1.7".
	FString NormalizeVersion(const FString& In)
	{
		FString S = In;
		S.TrimStartAndEndInline();
		if (S.StartsWith(TEXT("v"), ESearchCase::IgnoreCase)) S.RightChopInline(1);
		return S;
	}

	// Three-component numeric semver compare. Missing components are treated as 0.
	// Returns -1 if A<B, 0 if equal, +1 if A>B. Pre-release suffixes ignored.
	int32 CompareSemver(const FString& A, const FString& B)
	{
		auto SplitNums = [](const FString& V) -> TArray<int32>
		{
			TArray<int32> Out;
			FString Core = V;
			int32 DashIdx;
			if (Core.FindChar('-', DashIdx)) Core.LeftInline(DashIdx);
			TArray<FString> Parts;
			Core.ParseIntoArray(Parts, TEXT("."), true);
			for (const FString& P : Parts) Out.Add(FCString::Atoi(*P));
			while (Out.Num() < 3) Out.Add(0);
			return Out;
		};
		TArray<int32> Av = SplitNums(NormalizeVersion(A));
		TArray<int32> Bv = SplitNums(NormalizeVersion(B));
		for (int32 i = 0; i < 3; ++i)
		{
			if (Av[i] < Bv[i]) return -1;
			if (Av[i] > Bv[i]) return  1;
		}
		return 0;
	}

	// Resolve target absolute path from a staged file's relative path. The release
	// zip layout wraps everything under a top-level "KismetAI/" folder (see
	// package.ps1). We strip that wrapper so the contents land directly in the
	// existing plugin dir.
	FString StripKismetAIPrefix(const FString& Rel)
	{
		FString R = Rel;
		R.ReplaceInline(TEXT("\\"), TEXT("/"));
		const FString Prefix(TEXT("KismetAI/"));
		if (R.StartsWith(Prefix, ESearchCase::IgnoreCase)) R.RightChopInline(Prefix.Len());
		return R;
	}

	// Reject paths that try to escape the plugin dir (zip-slip / symlink trickery).
	bool IsSafeRelativePath(const FString& Rel)
	{
		if (Rel.IsEmpty()) return false;
		if (Rel.Contains(TEXT(".."))) return false;
		if (Rel.StartsWith(TEXT("/")) || Rel.Contains(TEXT(":"))) return false;
		return true;
	}

	void UpdaterRunOnGameThread(TFunction<void()> Fn)
	{
		AsyncTask(ENamedThreads::GameThread, MoveTemp(Fn));
	}
}

FString FKismetAIUpdater::GetPluginDir()
{
	TSharedPtr<IPlugin> P = IPluginManager::Get().FindPlugin(TEXT("KismetAI"));
	if (!P.IsValid()) return FString();
	return FPaths::ConvertRelativePathToFull(P->GetBaseDir());
}

FString FKismetAIUpdater::GetCurrentPlatformName()
{
#if PLATFORM_WINDOWS
	return TEXT("Win64");
#elif PLATFORM_MAC
	return TEXT("Mac");
#elif PLATFORM_LINUX
	return TEXT("Linux");
#else
	return FString();
#endif
}

FString FKismetAIUpdater::GetCurrentEngineVersion()
{
	const FEngineVersion& V = FEngineVersion::Current();
	return FString::Printf(TEXT("%u.%u"), V.GetMajor(), V.GetMinor());
}

// -----------------------------------------------------------------------------
// CheckAsync — GET manifest.json, parse, find matching asset.
// -----------------------------------------------------------------------------
void FKismetAIUpdater::CheckAsync(TFunction<void(FKismetAIUpdateInfo)> OnDone)
{
	FKismetAIUpdateInfo Seed;
	TSharedPtr<IPlugin> P = IPluginManager::Get().FindPlugin(TEXT("KismetAI"));
	Seed.CurrentVersion = P.IsValid() ? P->GetDescriptor().VersionName : FString();

	if (!IsSelfUpdateEnabled())
	{
		Seed.ErrorMessage = GetSelfUpdateDisabledMessage();
		OnDone(MoveTemp(Seed));
		return;
	}

	TSharedRef<IHttpRequest> Req = FHttpModule::Get().CreateRequest();
	Req->SetVerb(TEXT("GET"));
	Req->SetURL(KISMETAI_MANIFEST_URL);
	Req->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Req->SetHeader(TEXT("Cache-Control"), TEXT("no-cache"));

	Req->OnProcessRequestComplete().BindLambda(
		[Seed, OnDone](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk) mutable
		{
			FKismetAIUpdateInfo Info = Seed;

			if (!bOk || !Resp.IsValid())
			{
				Info.ErrorMessage = TEXT("Network error fetching update manifest.");
				UpdaterRunOnGameThread([Info, OnDone]() { OnDone(Info); });
				return;
			}
			const int32 Code = Resp->GetResponseCode();
			if (Code < 200 || Code >= 300)
			{
				Info.ErrorMessage = FString::Printf(TEXT("Manifest HTTP %d."), Code);
				UpdaterRunOnGameThread([Info, OnDone]() { OnDone(Info); });
				return;
			}

			const FString Body = Resp->GetContentAsString();
			TSharedPtr<FJsonObject> Root;
			TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Body);
			if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid())
			{
				Info.ErrorMessage = TEXT("Manifest JSON parse failed.");
				UpdaterRunOnGameThread([Info, OnDone]() { OnDone(Info); });
				return;
			}
			const TSharedPtr<FJsonObject>* Latest = nullptr;
			if (!Root->TryGetObjectField(TEXT("latest"), Latest) || !Latest)
			{
				Info.ErrorMessage = TEXT("Manifest missing 'latest' field.");
				UpdaterRunOnGameThread([Info, OnDone]() { OnDone(Info); });
				return;
			}
			(*Latest)->TryGetStringField(TEXT("version"), Info.LatestVersion);
			(*Latest)->TryGetStringField(TEXT("notes"),   Info.ReleaseNotes);

			if (Info.LatestVersion.IsEmpty())
			{
				Info.ErrorMessage = TEXT("Manifest 'latest.version' missing.");
				UpdaterRunOnGameThread([Info, OnDone]() { OnDone(Info); });
				return;
			}

			Info.bHasUpdate = CompareSemver(Info.LatestVersion, Info.CurrentVersion) > 0;

			if (Info.bHasUpdate)
			{
				const FString WantPlatform = FKismetAIUpdater::GetCurrentPlatformName();
				const FString WantEngine   = FKismetAIUpdater::GetCurrentEngineVersion();

				const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
				if ((*Latest)->TryGetArrayField(TEXT("assets"), Assets) && Assets)
				{
					for (const TSharedPtr<FJsonValue>& V : *Assets)
					{
						const TSharedPtr<FJsonObject>* AObj = nullptr;
						if (!V->TryGetObject(AObj) || !AObj) continue;
						FString Eng, Plat, Url;
						(*AObj)->TryGetStringField(TEXT("engine"),   Eng);
						(*AObj)->TryGetStringField(TEXT("platform"), Plat);
						(*AObj)->TryGetStringField(TEXT("url"),      Url);
						if (Eng == WantEngine && Plat == WantPlatform && !Url.IsEmpty())
						{
							Info.AssetURL = Url;
							int64 Sz = 0;
							if ((*AObj)->TryGetNumberField(TEXT("size"), Sz))
								Info.AssetSizeBytes = Sz;
							Info.bHasMatchingAsset = true;
							break;
						}
					}
				}
			}

			UpdaterRunOnGameThread([Info, OnDone]() { OnDone(Info); });
		});

	if (!Req->ProcessRequest())
	{
		FKismetAIUpdateInfo Info = Seed;
		Info.ErrorMessage = TEXT("Failed to start update check request.");
		OnDone(Info);
	}
}

// -----------------------------------------------------------------------------
// InstallAsync — download, extract, apply.
// -----------------------------------------------------------------------------
namespace
{
	bool ExtractZipToDir(const FString& ZipPath, const FString& OutDir, FString& OutErr)
	{
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		IFileHandle* Handle = PF.OpenRead(*ZipPath);
		if (!Handle)
		{
			OutErr = FString::Printf(TEXT("Cannot open downloaded zip: %s"), *ZipPath);
			return false;
		}

		// FZipArchiveReader takes ownership of the handle.
		FZipArchiveReader Zip(Handle);
		if (!Zip.IsValid())
		{
			OutErr = TEXT("Downloaded zip is invalid or corrupt.");
			return false;
		}

		IFileManager::Get().MakeDirectory(*OutDir, /*Tree=*/true);

		const TArray<FString> Names = Zip.GetFileNames();
		for (const FString& Name : Names)
		{
			// Some zip producers emit explicit directory entries with trailing '/'.
			if (Name.IsEmpty() || Name.EndsWith(TEXT("/"))) continue;

			if (!IsSafeRelativePath(Name))
			{
				OutErr = FString::Printf(TEXT("Refusing unsafe path in zip: %s"), *Name);
				return false;
			}

			TArray<uint8> Bytes;
			if (!Zip.TryReadFile(Name, Bytes))
			{
				OutErr = FString::Printf(TEXT("Read failed for entry: %s"), *Name);
				return false;
			}

			const FString OutPath = OutDir / Name;
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutPath), /*Tree=*/true);
			if (!FFileHelper::SaveArrayToFile(Bytes, *OutPath))
			{
				OutErr = FString::Printf(TEXT("Write failed: %s"), *OutPath);
				return false;
			}
		}
		return true;
	}

	// Walk a directory recursively and call Visitor(absolute path, isDirectory).
	bool ListRecursive(const FString& Dir, TArray<FString>& OutFiles)
	{
		class FCollector : public IPlatformFile::FDirectoryVisitor
		{
		public:
			TArray<FString>* Files;
			virtual bool Visit(const TCHAR* FilenameOrDirectory, bool bIsDir) override
			{
				if (!bIsDir) Files->Add(FilenameOrDirectory);
				return true;
			}
		};
		FCollector V;
		V.Files = &OutFiles;
		return FPlatformFileManager::Get().GetPlatformFile().IterateDirectoryRecursively(*Dir, V);
	}

	// Move existing TargetAbs aside as TargetAbs + ".old", then move StagedAbs ->
	// TargetAbs. Both moves use replace=true so a leftover .old from a prior failed
	// attempt is overwritten cleanly. Returns false on filesystem failure.
	bool ApplyOneFile(const FString& StagedAbs, const FString& TargetAbs, FString& OutErr)
	{
		IFileManager& FM = IFileManager::Get();
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(TargetAbs), /*Tree=*/true);

		const FString OldAbs = TargetAbs + TEXT(".old");
		const bool bHadOld = FM.FileExists(*TargetAbs);
		if (bHadOld)
		{
			// IFileManager::Move signature: Move(Dst, Src, bReplace, bEvenIfReadOnly, ...).
			if (!FM.Move(*OldAbs, *TargetAbs, /*Replace=*/true, /*EvenIfReadOnly=*/true))
			{
				OutErr = FString::Printf(TEXT("Could not rename live file aside: %s"), *TargetAbs);
				return false;
			}
		}
		if (!FM.Move(*TargetAbs, *StagedAbs, /*Replace=*/true, /*EvenIfReadOnly=*/true))
		{
			OutErr = FString::Printf(TEXT("Could not move staged file into place: %s -> %s"),
				*StagedAbs, *TargetAbs);
			// This file has not reached Applied yet, so the caller can only roll
			// back earlier swaps. Restore the current file before returning.
			if (bHadOld && !FM.Move(*TargetAbs, *OldAbs, /*Replace=*/true, /*EvenIfReadOnly=*/true))
			{
				OutErr += FString::Printf(TEXT(" Automatic restoration failed; check recovery backup path: %s"), *OldAbs);
			}
			return false;
		}
		return true;
	}

	// Tracks what was swapped so we can roll back if a later move fails.
	struct FAppliedSwap
	{
		FString TargetAbs;     // live path after apply (originally the new file)
		bool    bHadOldCopy;   // true if we renamed an existing target -> .old
	};

	// Roll back: for each successful swap (in reverse), delete the new file we
	// placed and rename .old back to the live name. Best-effort — log but don't
	// fail-loop, since we're already in the error path.
	void RollbackSwaps(const TArray<FAppliedSwap>& Done)
	{
		IFileManager& FM = IFileManager::Get();
		for (int32 i = Done.Num() - 1; i >= 0; --i)
		{
			const FAppliedSwap& S = Done[i];
			const FString OldAbs = S.TargetAbs + TEXT(".old");
			// Remove the just-installed file (the rename "in" we did).
			FM.Delete(*S.TargetAbs, /*RequireExists=*/false, /*EvenReadOnly=*/true);
			// If we had pushed an existing copy aside, restore it.
			if (S.bHadOldCopy && FM.FileExists(*OldAbs))
			{
				FM.Move(*S.TargetAbs, *OldAbs, /*Replace=*/true, /*EvenIfReadOnly=*/true);
			}
		}
	}

	bool ApplyStagedTree(const FString& StagedRoot, const FString& PluginDir, FString& OutErr)
	{
		TArray<FString> Files;
		if (!ListRecursive(StagedRoot, Files))
		{
			OutErr = TEXT("Could not enumerate staged files.");
			return false;
		}

		// Apply non-binary files first; binaries last so a crash mid-apply leaves
		// the live DLL intact and the editor still usable.
		TArray<FString> EarlyFiles, BinaryFiles;
		for (const FString& F : Files)
		{
			(F.Contains(TEXT("/Binaries/")) ? BinaryFiles : EarlyFiles).Add(F);
		}

		IFileManager& FM = IFileManager::Get();
		TArray<FAppliedSwap> Applied;

		auto ApplyList = [&](const TArray<FString>& List) -> bool
		{
			for (const FString& StagedAbs : List)
			{
				FString Rel = StagedAbs;
				FPaths::MakePathRelativeTo(Rel, *(StagedRoot + TEXT("/")));
				const FString RelStripped = StripKismetAIPrefix(Rel);
				if (!IsSafeRelativePath(RelStripped))
				{
					OutErr = FString::Printf(TEXT("Refusing unsafe staged path: %s"), *Rel);
					return false;
				}
				const FString TargetAbs = FPaths::ConvertRelativePathToFull(PluginDir / RelStripped);

				// Belt-and-braces: ensure target is inside the plugin dir.
				const FString PluginDirFull = FPaths::ConvertRelativePathToFull(PluginDir) + TEXT("/");
				if (!TargetAbs.StartsWith(PluginDirFull))
				{
					OutErr = FString::Printf(TEXT("Refusing to write outside plugin dir: %s"), *TargetAbs);
					return false;
				}

				const bool bHadOld = FM.FileExists(*TargetAbs);
				if (!ApplyOneFile(StagedAbs, TargetAbs, OutErr)) return false;
				Applied.Add({TargetAbs, bHadOld});
			}
			return true;
		};

		if (!ApplyList(EarlyFiles) || !ApplyList(BinaryFiles))
		{
			KISMETAI_LOG(LogKismetAIUpdater, Warning,
				TEXT("Apply failed mid-stream — rolling back %d swap(s)."), Applied.Num());
			RollbackSwaps(Applied);
			return false;
		}
		return true;
	}
}

void FKismetAIUpdater::InstallAsync(const FString& AssetURL, TFunction<void(bool, FString)> OnDone)
{
	if (!IsSelfUpdateEnabled())
	{
		OnDone(false, GetSelfUpdateDisabledMessage());
		return;
	}

	{
		FScopeLock Lock(&InstallLock);
		if (bInstallInProgress)
		{
			OnDone(false, TEXT("An install is already in progress."));
			return;
		}
		bInstallInProgress = true;
	}

	const FString PluginDir = GetPluginDir();
	if (PluginDir.IsEmpty())
	{
		bInstallInProgress = false;
		OnDone(false, TEXT("Plugin install dir not found."));
		return;
	}

	const FString WorkDir   = PluginDir / TEXT("Intermediate") / TEXT("Update");
	const FString ZipPath   = WorkDir / TEXT("pending.zip");
	const FString StagedDir = WorkDir / TEXT("staged");

	// Wipe any prior staged content before starting fresh.
	IFileManager::Get().DeleteDirectory(*StagedDir, /*RequireExists=*/false, /*Tree=*/true);
	IFileManager::Get().Delete(*ZipPath, /*RequireExists=*/false, /*EvenReadOnly=*/true);
	IFileManager::Get().MakeDirectory(*WorkDir, /*Tree=*/true);

	KISMETAI_LOG(LogKismetAIUpdater, Log, TEXT("Installing update from %s"), *AssetURL);

	TSharedRef<IHttpRequest> Req = FHttpModule::Get().CreateRequest();
	Req->SetVerb(TEXT("GET"));
	Req->SetURL(AssetURL);
	Req->SetTimeout(300.0f);

	Req->OnProcessRequestComplete().BindLambda(
		[OnDone, ZipPath, StagedDir, PluginDir](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
		{
			auto Finish = [OnDone](bool bSuccess, FString Err)
			{
				bInstallInProgress = false;
				UpdaterRunOnGameThread([OnDone, bSuccess, Err]() { OnDone(bSuccess, Err); });
			};

			if (!bOk || !Resp.IsValid())
			{
				Finish(false, TEXT("Network error downloading update."));
				return;
			}
			const int32 Code = Resp->GetResponseCode();
			if (Code < 200 || Code >= 300)
			{
				Finish(false, FString::Printf(TEXT("Download HTTP %d."), Code));
				return;
			}

			const TArray<uint8>& Bytes = Resp->GetContent();
			if (Bytes.Num() == 0)
			{
				Finish(false, TEXT("Empty download response."));
				return;
			}
			if (!FFileHelper::SaveArrayToFile(Bytes, *ZipPath))
			{
				Finish(false, FString::Printf(TEXT("Could not write %s"), *ZipPath));
				return;
			}

			// Extract + apply on a worker thread (file IO can be slow), result back on game thread.
			Async(EAsyncExecution::ThreadPool, [Finish, ZipPath, StagedDir, PluginDir]()
			{
				FString Err;
				if (!ExtractZipToDir(ZipPath, StagedDir, Err))
				{
					Finish(false, Err);
					return;
				}
				if (!ApplyStagedTree(StagedDir, PluginDir, Err))
				{
					Finish(false, Err);
					return;
				}

				// Clean up the staged tree and zip — .old files stay until next launch.
				IFileManager::Get().DeleteDirectory(*StagedDir, false, /*Tree=*/true);
				IFileManager::Get().Delete(*ZipPath, false, /*EvenReadOnly=*/true);

				KISMETAI_LOG(LogKismetAIUpdater, Log, TEXT("Update installed. Restart required."));
				Finish(true, FString());
			});
		});

	if (!Req->ProcessRequest())
	{
		bInstallInProgress = false;
		OnDone(false, TEXT("Failed to start download request."));
	}
}

void FKismetAIUpdater::RestartEditor()
{
	FUnrealEdMisc::Get().RestartEditor(/*bWarn=*/true);
}

bool FKismetAIUpdater::ApplyStagedTreeForTest(const FString& StagedRoot, const FString& TargetDir, FString& OutErr)
{
	return ApplyStagedTree(StagedRoot, TargetDir, OutErr);
}

void FKismetAIUpdater::CleanupStaleFiles()
{
	const FString PluginDir = GetPluginDir();
	if (PluginDir.IsEmpty()) return;

	const FString StagedDir = PluginDir / TEXT("Intermediate") / TEXT("Update") / TEXT("staged");
	IFileManager::Get().DeleteDirectory(*StagedDir, /*RequireExists=*/false, /*Tree=*/true);

	const FString ZipPath = PluginDir / TEXT("Intermediate") / TEXT("Update") / TEXT("pending.zip");
	IFileManager::Get().Delete(*ZipPath, /*RequireExists=*/false, /*EvenReadOnly=*/true);

	TArray<FString> AllFiles;
	if (!ListRecursive(PluginDir, AllFiles)) return;
	int32 Removed = 0, Preserved = 0;
	IFileManager& FM = IFileManager::Get();
	for (const FString& F : AllFiles)
	{
		if (!F.EndsWith(TEXT(".old"))) continue;

		// Critical safety: if the live counterpart is missing, this .old is the
		// only surviving copy of the file (e.g. from a partially-applied install
		// that crashed before completion). DO NOT delete it — the user can rename
		// it back manually to recover.
		const FString LivePath = F.LeftChop(4); // strip ".old"
		if (!FM.FileExists(*LivePath))
		{
			KISMETAI_LOG(LogKismetAIUpdater, Warning,
				TEXT("Preserving %s — live file missing, this .old may be the only copy."), *F);
			++Preserved;
			continue;
		}

		if (FM.Delete(*F, /*RequireExists=*/false, /*EvenReadOnly=*/true)) ++Removed;
	}
	if (Removed > 0 || Preserved > 0)
	{
		KISMETAI_DEV_LOG(LogKismetAIUpdater, Log,
			TEXT("Cleanup: removed %d post-update .old files, preserved %d (live missing)."),
			Removed, Preserved);
	}
}
