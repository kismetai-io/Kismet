// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CoreMinimal.h"

namespace KismetAISourceContract
{
	inline constexpr int32 MaxEntries = 1024;
	inline constexpr int32 MaxDirectories = 128;
	inline constexpr int32 MaxDepth = 64;
	inline constexpr int32 MaxResults = 20;
	inline constexpr int32 MaxOutputBytes = 16 * 1024;
	inline constexpr int32 MaxSerializedResultBytes = 32 * 1024;
	inline constexpr int32 MaxReadBytes = 1024 * 1024;
	inline constexpr double PageSeconds = 0.25;
	inline constexpr int32 MaxCursors = 8;
	inline constexpr double CursorIdleSeconds = 60.0;
	inline constexpr int32 MaxDiscoveredPluginDirs = 4096;
	inline constexpr int32 MaxJobs = 8;
	inline constexpr double JobSeconds = 5.0;
	inline constexpr int32 MaxMcpBatch = 32;

	inline FString PatternError(const FString& Pattern)
	{
		if (Pattern.IsEmpty() || Pattern.Len() > 128 || Pattern != Pattern.TrimStartAndEnd())
		{
			return TEXT("Use a basename glob of 1–128 characters without surrounding whitespace.");
		}
		if (Pattern.Contains(TEXT("**")))
		{
			return TEXT("Recursive ** patterns are unsupported; traversal is already recursive.");
		}
		bool bLiteral = false;
		for (const TCHAR Ch : Pattern)
		{
			if (Ch < 32 || Ch == 127 || FString(TEXT("/\\[]{}:")).Contains(FString::Chr(Ch)))
			{
				return TEXT("Only basename literals, * and ? wildcards are supported; no paths, character classes or brace expansion.");
			}
			bLiteral |= Ch != TEXT('*') && Ch != TEXT('?') && Ch != TEXT('.');
		}
		return bLiteral ? FString() : FString(TEXT("Include a literal filename fragment or extension."));
	}

	// Iterative basename glob matching avoids recursive wildcard expansion.
	inline bool Matches(const FString& Name, const FString& Pattern)
	{
		int32 NameIndex = 0, PatternIndex = 0, Star = INDEX_NONE, Retry = 0;
		while (NameIndex < Name.Len())
		{
			if (PatternIndex < Pattern.Len() && (Pattern[PatternIndex] == TEXT('?') ||
				FChar::ToLower(Pattern[PatternIndex]) == FChar::ToLower(Name[NameIndex])))
			{
				++NameIndex; ++PatternIndex;
			}
			else if (PatternIndex < Pattern.Len() && Pattern[PatternIndex] == TEXT('*'))
			{
				Star = PatternIndex++; Retry = NameIndex;
			}
			else if (Star != INDEX_NONE)
			{
				PatternIndex = Star + 1; NameIndex = ++Retry;
			}
			else return false;
		}
		while (PatternIndex < Pattern.Len() && Pattern[PatternIndex] == TEXT('*')) ++PatternIndex;
		return PatternIndex == Pattern.Len();
	}
}
