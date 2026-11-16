// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
class FJsonObject;

// Best-effort local observations, never recovery authority or a network sink.
namespace KismetAIDiagnostics
{
	KISMETAIRUNTIME_API TSharedPtr<FJsonObject> SafeProjection(const TSharedPtr<FJsonObject>& Payload);
	KISMETAIRUNTIME_API void Record(const FString& Event, const TSharedPtr<FJsonObject>& Payload);
	KISMETAIRUNTIME_API void CapturePrompt(const FString& RunId, const FString& Prompt);
	KISMETAIRUNTIME_API FString GetSafeLogPath();
}
