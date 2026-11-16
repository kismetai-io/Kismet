// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "KismetAICancellation.h"
#include "KismetAISourceContract.h"
#include "KismetAIToolResult.h"

namespace KismetAISourceSearch
{
	FToolExecutionResult Find(const TSharedPtr<FJsonObject>& Args,
		const TArray<FString>& CanonicalRoots, const FKismetAICancellationToken& Token,
		double DeadlineSeconds = 0, const TFunction<void(int32)>& OnEntryForTests = {});
	void Close();
	// Called by the job pump even when no new source request arrives. Nonzero time is a test seam.
	void ReapExpired(double NowSeconds = 0);
}
