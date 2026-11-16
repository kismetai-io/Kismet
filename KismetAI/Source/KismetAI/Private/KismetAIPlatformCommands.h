// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "CoreMinimal.h"

namespace KismetAIPlatformCommands
{
	FString Local(const FString& Action, const FString& Payload);
	void Registry(const FString& Action, const FString& Payload, TFunction<void(const FString&)> OnComplete);
}
