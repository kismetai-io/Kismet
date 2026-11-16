// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "KismetAIToolResult.h"

namespace KismetAIReadRevision
{
	// Game thread, after editor/policy admission. The lease spans comparison,
	// handler execution and result capture, including callbacks pumped by saves.
	FToolExecutionResult Run(const FString& ToolName, const TSharedPtr<FJsonObject>& Body,
		const TMap<FString, FString>& ObservedRevisions, bool bRequireObservation,
		const TFunction<FToolExecutionResult(const TSharedPtr<FJsonObject>&)>& Work);
}
