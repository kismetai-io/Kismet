// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "KismetAIToolResult.h"

// Widget Designer operations only. The invoker owns editor-state checks and live
// game-thread admission; persistence and run recovery retain their shared owners.
// Call only from admitted game-thread work, never from a transport directly.
class FKismetAIWidgetService
{
public:
	static FToolExecutionResult Describe(const TSharedPtr<FJsonObject>& Args);
	static FToolExecutionResult Overview(const TSharedPtr<FJsonObject>& Args);
	static FToolExecutionResult Edit(const TSharedPtr<FJsonObject>& Args);
};
