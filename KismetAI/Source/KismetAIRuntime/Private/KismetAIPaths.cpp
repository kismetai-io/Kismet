// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIPaths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"

namespace KismetAIPaths
{

FString GetConfigDir()
{
	const FString Override = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMET_CONFIG_DIR"));
	if (!Override.IsEmpty()) return FPaths::ConvertRelativePathToFull(Override);
	const FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
	const FString NewDir = Home / TEXT(".kismet");
	const FString LegacyDir = Home / TEXT(".kismetai");

	// Prefer the current name; fall back to the legacy one only when it is the
	// one that actually exists, so an existing install keeps its keys.
	IFileManager& FileMgr = IFileManager::Get();
	if (FileMgr.DirectoryExists(*NewDir))
	{
		return NewDir;
	}
	if (FileMgr.DirectoryExists(*LegacyDir))
	{
		return LegacyDir;
	}
	return NewDir;
}

}
