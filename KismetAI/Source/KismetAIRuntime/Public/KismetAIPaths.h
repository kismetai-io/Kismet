// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Filesystem locations the runtime needs.
//
// GetConfigDir lived on FKismetAIModule, which meant KismetAIAuth and
// KismetAIKeyStore — both otherwise Core-only — had to reach into the editor
// module for a path. It is pure: an env var and two directory probes. Moving it
// here is one of the two inversions that let the agent live in a module that
// cannot see the editor.

#pragma once

#include "CoreMinimal.h"

namespace KismetAIPaths
{
	// ~/.kismet, or the legacy ~/.kismetai when that exists and the new one does
	// not. Holds settings.json and the encrypted key files.
	KISMETAIRUNTIME_API FString GetConfigDir();
}
