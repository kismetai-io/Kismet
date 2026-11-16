// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "CoreMinimal.h"
struct FKismetAIAgentConfig;
class FJsonObject;
namespace KismetAIRunSetup
{
	KISMETAIRUNTIME_API bool ReadOperatingContextLimit(const FJsonObject& Settings, int32& OutLimit, FString& OutError);
	KISMETAIRUNTIME_API bool RequiresProviderConfirmation(const FJsonObject& Settings);
	// Checks provider migration and snapshots a verified skill before either launch path.
	KISMETAIRUNTIME_API bool Prepare(FKismetAIAgentConfig& Config, FString& Error);
}
