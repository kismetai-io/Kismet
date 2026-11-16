// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// The model catalog: every fact KismetAI knows about a model — its wire id,
// picker row, context window, prices and the request-shape capabilities the
// providers branch on — loaded from Config/models.json.
//
// Before the catalog these facts lived in a dozen hand-written string-matching
// functions in KismetAIAgent.cpp (context map, cost ladder, nine ModelSupports*
// helpers, alias map, display names, picker rows) plus a JavaScript copy in
// ui.html. Adding a model meant editing all of them, and a model that matched
// none of them was silently treated as a 2025-era Claude: a Claude 5 id got a
// manual thinking budget, which the API rejects. The catalog is the single
// source; the old functions now read from it.

// $ per million tokens.
struct KISMETAIRUNTIME_API FKismetAIModelPrice
{
	double Input = 0.0;
	double Output = 0.0;
	double CacheRead = 0.0;
	double CacheWrite = 0.0;
};

// How a request asks for the model's fast tier.
enum class EKismetAIFastModeWire : uint8
{
	None,                // no fast tier
	AnthropicSpeed,      // body "speed":"fast"
	OpenAIPriority,      // body "service_tier":"priority"
	ModelSwap,           // send FastModeModelId instead of the model id
};

struct KISMETAIRUNTIME_API FKismetAIModelCapabilities
{
	// Anthropic-shaped thinking.
	bool bThinking = true;                   // a thinking field may be sent at all
	bool bAdaptiveThinking = false;          // {type:"adaptive"} accepted
	bool bBudgetThinking = true;             // {type:"enabled", budget_tokens} accepted
	bool bThinkingAlwaysOn = false;          // thinking cannot be turned off; always send adaptive
	bool bSummarizedThinkingDisplay = true;  // add display:"summarized" to adaptive thinking
	bool bSamplingParams = true;             // temperature/top_p/top_k accepted
	bool bLegacyThinkingBeta = true;         // needs interleaved-thinking-2025-05-14
	TArray<FString> EffortLevels;            // accepted effort values, low..max order; empty = no effort
	bool bEffortBeta = false;                // effort needs effort-2025-11-24
	bool bMidConversationSystem = false;     // {"role":"system"} turns accepted
	bool bThinkingBlockBinding = false;      // send thinking.block_binding (Claude 5.1+)

	// OpenAI-shaped reasoning.
	bool bReasoning = false;                 // reasoning model: send reasoning.effort, no temperature

	EKismetAIFastModeWire FastModeWire = EKismetAIFastModeWire::None;
	FString FastModeModelId;                 // for ModelSwap
};

struct KISMETAIRUNTIME_API FKismetAIModelEntry
{
	FString Id;                      // canonical wire id; empty for pattern-only entries
	TArray<FString> Ids;             // other full ids that resolve to themselves
	FString Alias;                   // the picker's short name
	TArray<FString> Aliases;         // extra short names -> Id
	TArray<FString> MatchPatterns;   // lowercase; "^x" = prefix, else substring
	FString DisplayName;
	FString Provider;                // "anthropic" / "openai" / "minimax" — informational
	FString Tier;                    // picker group
	bool bInPicker = false;
	bool bPatternOnly = false;
	FString RecommendedEffort;
	int32 ContextWindow = 200000;
	int32 MaxOutputTokens = 0;       // 0 = unknown

	FKismetAIModelPrice Price;
	TOptional<FKismetAIModelPrice> FastPrice;
	int32 LongContextThreshold = 0;  // prompt tokens above which LongContextPrice applies; 0 = none
	TOptional<FKismetAIModelPrice> LongContextPrice;

	FKismetAIModelCapabilities Caps;
};

class KISMETAIRUNTIME_API FKismetAIModelCatalog
{
public:
	FKismetAIModelCatalog();

	// The shipped catalog (Config/models.json, embedded copy first). Loaded once,
	// thread-safe. If the file is missing or invalid the catalog is empty, every
	// lookup uses conservative unknown capabilities, and an error is logged.
	static const FKismetAIModelCatalog& Get();

	// Parse a catalog document. Used by Get() and by tests. On failure the
	// catalog keeps no entries and OutError says why.
	bool LoadFromJson(const FString& Json, FString& OutError);

	bool IsLoaded() const { return bLoaded; }

	// Exact id / ids / alias match (case-insensitive) first, then the longest
	// matching pattern; ties go to the earlier entry. Null when nothing matches.
	const FKismetAIModelEntry* Find(const FString& ModelName) const;

	// Find(), or conservative unknown capabilities with default context/price assumptions.
	const FKismetAIModelEntry& FindOrDefaults(const FString& ModelName) const;

	// A -1m suffix cannot enlarge a named model beyond its declared capacity.
	bool IsKnownContextSuffix(const FString& ModelName) const;

	// Alias or full id -> the id to send. Pattern matches do not count: an
	// unrecognised name returns empty, as ResolveModelAlias always has.
	FString Resolve(const FString& AliasOrId) const;

	const FKismetAIModelEntry& GetDefaults() const { return Defaults; }
	const TArray<FKismetAIModelEntry>& GetEntries() const { return Entries; }

	// Picker rows, in file order.
	TArray<const FKismetAIModelEntry*> GetPickerModels() const;

	// The highest level in Supported that does not exceed Requested, in
	// low < medium < high < xhigh < max order. Falls back to the lowest supported
	// level when every supported level is above Requested; empty when Supported is.
	static FString ClampEffort(const TArray<FString>& Supported, const FString& Requested);

	// Price for one request, choosing the long-context tier by prompt size and
	// the fast tier when requested and available.
	static FKismetAIModelPrice PriceFor(const FKismetAIModelEntry& Entry, bool bFastMode, int32 PromptTokens);

private:
	static bool ParseEntry(const FJsonObject& Obj, const FKismetAIModelEntry& Base, FKismetAIModelEntry& Out, FString& OutError);

	TArray<FKismetAIModelEntry> Entries;
	FKismetAIModelEntry Defaults;
	FKismetAIModelEntry UnknownDefaults;
	bool bLoaded = false;
};
