// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

// Unified accessor for Config/ files. In release builds these are compiled
// into the binary (see Scripts/embed_config.py + Build.cs). In development
// builds the files are read from Config/ on disk — LoadConfigFile() falls
// through to FFileHelper::LoadFileToString when no embedded copy exists.
namespace KismetAIEmbeddedConfig
{
	// True when prompts/UI were baked into the binary at build time.
	KISMETAIRUNTIME_API bool IsEmbedded();

	// Look up a file by name (e.g. "system_prompt.txt") and return its UTF-8
	// contents as FString. Returns false when no embedded copy exists.
	KISMETAIRUNTIME_API bool TryLoadEmbedded(const TCHAR* FileName, FString& OutContent);

	// Convenience wrapper: embedded first, then FFileHelper on DiskPath.
	// Returns true if either source produced content.
	KISMETAIRUNTIME_API bool LoadConfigFile(const TCHAR* FileName, const FString& DiskPath, FString& OutContent);
}
