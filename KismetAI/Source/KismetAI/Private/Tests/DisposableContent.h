// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Disposable /Game fixtures that leave nothing behind in the project's
// Content folder.
//
// A test that edits a file-backed fixture outside eval mode autosaves it, so
// "the fixture was only in memory" is not a cleanup plan. Two shapes:
//
// - FScopedDisposableMount: a /Game/<unique>/ mount point backed by a temporary
//   directory outside the project. Package names stay long /Game names (scoped
//   authority, checkpoints and asset fingerprints treat them as project
//   assets), but every save lands in the temporary directory, which is deleted
//   with the mount.
// - DeleteFixtureFolder / PruneEmptyFixtureFolders: for fixtures that must live
//   under a fixed /Game folder, delete that folder's files and every directory
//   left empty above it, up to Content. Only folders whose name marks them as
//   test-owned ("KismetAITest", "__Kismet") are ever touched.
// - ForgetUndoHistory: call after marking a fixture garbage, before the next
//   garbage collection, so the undo buffer no longer holds it.

#pragma once

#include "CoreMinimal.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

namespace KismetAIDisposableContent
{
	// Empty the editor's undo buffer. A fixture edited inside a transaction
	// (every Blueprint tool opens one) stays referenced by the buffer after it is
	// marked garbage. On UE 5.6 the next collection then crashes intermittently
	// in the reachability batcher (SIGSEGV at 0xc in
	// TReferenceBatcher::DrainValidated); no plugin code is on that stack.
	// Editor asset deletion (ObjectTools) resets the buffer for the same reason.
	inline void ForgetUndoHistory()
	{
		if (GEditor && !GIsTransacting)
		{
			GEditor->ResetTransaction(FText::FromString(TEXT("KismetAI test fixture disposed")));
		}
	}

	inline bool IsTestOwnedFolder(const FString& LongFolder)
	{
		return LongFolder.StartsWith(TEXT("/Game/"))
			&& (LongFolder.Contains(TEXT("KismetAITest")) || LongFolder.Contains(TEXT("/__Kismet")));
	}

	// The on-disk directory of a /Game folder, or empty if it cannot be mapped.
	inline FString FolderOnDisk(const FString& LongFolder)
	{
		FString Probe;
		if (!FPackageName::TryConvertLongPackageNameToFilename(LongFolder / TEXT("Probe"), Probe))
		{
			return FString();
		}
		return FPaths::ConvertRelativePathToFull(FPaths::GetPath(Probe));
	}

	// Remove LongFolder's directory if it is empty, then each empty parent up to
	// (not including) the project's Content directory.
	inline void PruneEmptyFixtureFolders(const FString& LongFolder)
	{
		if (!IsTestOwnedFolder(LongFolder))
		{
			return;
		}
		const FString Content = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
		FString Dir = FolderOnDisk(LongFolder);
		IFileManager& FM = IFileManager::Get();
		while (!Dir.IsEmpty() && FPaths::IsUnderDirectory(Dir, Content) && !FPaths::IsSamePath(Dir, Content))
		{
			if (!FM.DirectoryExists(*Dir))
			{
				Dir = FPaths::GetPath(Dir);
				continue;
			}
			bool bEmpty = true;
			FM.IterateDirectory(*Dir, [&bEmpty](const TCHAR*, bool) { bEmpty = false; return false; });
			if (!bEmpty || !FM.DeleteDirectory(*Dir, false, false))
			{
				return;
			}
			Dir = FPaths::GetPath(Dir);
		}
	}

	// Delete a test-owned /Game folder from disk (files and subfolders), then
	// prune the empty directories above it.
	inline void DeleteFixtureFolder(const FString& LongFolder)
	{
		if (!IsTestOwnedFolder(LongFolder))
		{
			return;
		}
		const FString Dir = FolderOnDisk(LongFolder);
		if (!Dir.IsEmpty())
		{
			IFileManager::Get().DeleteDirectory(*Dir, false, true);
		}
		PruneEmptyFixtureFolders(LongFolder);
	}

	// A /Game/__KismetAITest_<Stem>_<id>/ mount backed by a temporary directory.
	struct FScopedDisposableMount
	{
		FString Mount;      // "/Game/__KismetAITest_<Stem>_<id>/"
		FString Directory;  // absolute temporary directory, with trailing slash

		explicit FScopedDisposableMount(const TCHAR* Stem)
		{
			const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(10);
			Mount = FString::Printf(TEXT("/Game/__KismetAITest_%s_%s/"), Stem, *Id);
			Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformProcess::UserTempDir(),
				TEXT("KismetAITestContent"), FString::Printf(TEXT("%s_%s"), Stem, *Id))) + TEXT("/");
			IFileManager::Get().MakeDirectory(*Directory, true);
			FPackageName::RegisterMountPoint(Mount, Directory);
		}

		~FScopedDisposableMount()
		{
			FPackageName::UnRegisterMountPoint(Mount, Directory);
			IFileManager::Get().DeleteDirectory(*Directory, false, true);
		}

		// A long package name inside the mount: "/Game/__KismetAITest_.../<Name>".
		FString PackageName(const TCHAR* Name) const { return Mount + Name; }
		// The mount as an authority scope ("/Game/__KismetAITest_...").
		FString Scope() const { return Mount.LeftChop(1); }

		FScopedDisposableMount(const FScopedDisposableMount&) = delete;
		FScopedDisposableMount& operator=(const FScopedDisposableMount&) = delete;
	};
}
