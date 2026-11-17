// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

DECLARE_LOG_CATEGORY_EXTERN(LogKismetAIUpdater, Log, All);

/** Result of CheckAsync. Always populated even on "no update" so the UI can show
 *  current/latest version info. On failure, ErrorMessage is set and other fields
 *  are best-effort. */
struct FKismetAIUpdateInfo
{
	FString CurrentVersion;
	FString LatestVersion;
	FString ReleaseNotes;
	FString AssetURL;
	int64   AssetSizeBytes = 0;
	bool    bHasUpdate     = false;
	bool    bHasMatchingAsset = false; // false when latest > current but no asset for engine+platform
	FString ErrorMessage;              // empty on success
};

/**
 * Self-contained plugin updater. Pulls a manifest.json from a hardcoded URL,
 * downloads the matching asset zip, extracts it, and applies it via rename-aside
 * (works on currently-loaded DLL/dylib/.so). Restart of the editor is required
 * to pick up the new binary.
 *
 * No external dependencies — uses FHttpModule (already linked) and FZipArchiveReader
 * (FileUtilities module). All public methods are static and safe to call from any
 * thread; results are delivered via callbacks invoked on the game thread.
 */
class FKismetAIUpdater
{
public:
	/** GET the manifest, compute hasUpdate, find platform+engine asset. OnDone always
	 *  called on game thread. */
	static void CheckAsync(TFunction<void(FKismetAIUpdateInfo)> OnDone);

	/** Download the zip from AssetURL, extract it, and apply over the live plugin
	 *  install. OnDone(true, "") on success; OnDone(false, error) on failure.
	 *  Game thread. Re-entry is rejected (returns immediate failure). */
	static void InstallAsync(const FString& AssetURL, TFunction<void(bool, FString)> OnDone);

	/** False in a Fab build (KISMETAI_SELF_UPDATE=0): the Epic Games Launcher
	 *  owns updates there, so CheckAsync and InstallAsync refuse without any
	 *  network request and the launch-time check is skipped. */
	static bool IsSelfUpdateEnabled();

	/** User-facing reason CheckAsync/InstallAsync give when self-update is off. */
	static FString GetSelfUpdateDisabledMessage();

	/** Restart the editor preserving the current project. Asks for unsaved-work
	 *  confirmation via UnrealEdMisc. Game thread only. */
	static void RestartEditor();

	/** Sweep Intermediate/Update/staged and any leftover .old files under the
	 *  plugin dir. Safe to call from StartupModule — by the time we run, the new
	 *  DLL is already loaded so the renamed-aside copies are no longer needed. */
	static void CleanupStaleFiles();

	/** Plugin install root, e.g. <Project>/Plugins/KismetAI/. */
	static FString GetPluginDir();

	/** "Win64" / "Mac" / "Linux" — matches package.ps1 -TargetPlatforms naming. */
	static FString GetCurrentPlatformName();

	/** "5.6" — running editor's Major.Minor, matches package.ps1 zip naming. */
	static FString GetCurrentEngineVersion();

	/** Test-only surface. Drives the rename-aside apply against arbitrary
	 *  staged + target dirs. Production callers use InstallAsync. */
	static bool ApplyStagedTreeForTest(const FString& StagedRoot, const FString& TargetDir, FString& OutErr);
};
