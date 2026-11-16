// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include <type_traits>

// FJsonObject keys are FString in UE <= 5.7 and UE::FSharedString in 5.8+
// (UE_JSONOBJECT_LEGACY_STRING_KEYS=0 by default). Normalize a key to FString so
// the same iteration code compiles against every engine we ship for. The else
// branch is only instantiated when KeyType is the shared-string type, so this
// stays valid on engines whose key type is a plain FString.
template <typename KeyType>
inline FString KismetJsonKey(const KeyType& Key)
{
	if constexpr (std::is_same_v<KeyType, FString>)
	{
		return Key;
	}
	else
	{
		// operator* yields a null-terminated const CharType* (UTF16CHAR on 5.8);
		// FString's char-pointer constructor transcodes to TCHAR on every platform.
		return FString(*Key);
	}
}
