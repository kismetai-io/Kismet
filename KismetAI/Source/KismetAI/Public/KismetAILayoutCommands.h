// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "KismetAIStyle.h"

/**
 * UI command set for manual layout actions (toolbar button + hotkey).
 */
class KISMETAI_API FKismetAILayoutCommands : public TCommands<FKismetAILayoutCommands>
{
public:
	FKismetAILayoutCommands()
		: TCommands<FKismetAILayoutCommands>(
			TEXT("KismetAILayout"),
			NSLOCTEXT("KismetAI", "KismetAILayoutCommands", "KismetAI Layout"),
			NAME_None,
			FKismetAIStyle::GetStyleSetName())
	{
	}

	virtual void RegisterCommands() override;

	/** Auto-layout the currently focused Blueprint graph. */
	TSharedPtr<FUICommandInfo> FormatGraph;
};
