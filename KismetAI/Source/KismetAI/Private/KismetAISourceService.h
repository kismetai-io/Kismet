// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "KismetAISourceConfinement.h"
#include "KismetAICancellation.h"
#include "KismetAIToolResult.h"

struct FKismetAISourceSnapshot
{
	FKismetAISourceRoots Roots;
	TArray<FString> DiscoveredPluginDirs;
};

namespace KismetAISourceService
{
	// Captures engine-owned configuration on the game thread; no filesystem traversal.
	FKismetAISourceSnapshot Prepare();
	FToolExecutionResult Execute(const TSharedPtr<FJsonObject>& Args,
		const FKismetAISourceSnapshot& Snapshot, const FKismetAICancellationToken& Token);
}
