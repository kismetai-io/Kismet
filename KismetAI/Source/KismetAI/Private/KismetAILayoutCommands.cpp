// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAILayoutCommands.h"
#include "Framework/Commands/InputChord.h"
#include "InputCoreTypes.h"

#define LOCTEXT_NAMESPACE "KismetAILayoutCommands"

void FKismetAILayoutCommands::RegisterCommands()
{
	UI_COMMAND(
		FormatGraph,
		"Format Graph",
		"Auto-layout the current Blueprint graph",
		EUserInterfaceActionType::Button,
		FInputChord(EModifierKey::Control | EModifierKey::Shift, EKeys::L));
}

#undef LOCTEXT_NAMESPACE
