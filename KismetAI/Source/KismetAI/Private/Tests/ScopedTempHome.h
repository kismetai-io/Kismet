// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

// Scoped redirect of KISMET_CONFIG_DIR to a fresh temp dir under ProjectIntermediate/.
// Lets tests that touch ~/.kismet/* (KeyStore, Auth) work in isolation
// without clobbering the developer's real config files.
//
// FKismetAIModule::GetConfigDir() reads KISMET_CONFIG_DIR on every call, so changing
// the env var is enough — no other invalidation needed.
struct FScopedTempHome
{
	FString OldConfigDir;
	FString TempHome;

	explicit FScopedTempHome(const TCHAR* TestName)
	{
		OldConfigDir = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMET_CONFIG_DIR"));
		TempHome = FPaths::ProjectIntermediateDir() /
		           FString::Printf(TEXT("%s_%s"), TestName, *FGuid::NewGuid().ToString(EGuidFormats::Short));
		IFileManager::Get().MakeDirectory(*TempHome, /*Tree=*/true);
		FPlatformMisc::SetEnvironmentVar(TEXT("KISMET_CONFIG_DIR"), *(TempHome / TEXT(".kismet")));
	}

	~FScopedTempHome()
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("KISMET_CONFIG_DIR"), *OldConfigDir);
		IFileManager::Get().DeleteDirectory(*TempHome, /*RequireExists=*/false, /*Tree=*/true);
	}
};
