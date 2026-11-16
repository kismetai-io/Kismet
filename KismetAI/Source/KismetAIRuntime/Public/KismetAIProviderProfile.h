// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

struct FKismetAIAgentConfig;

// Provider profiles: where a request goes and how it authenticates, loaded from
// Config/providers.json.
//
// Routing used to be decided by the model id's prefix (IsOpenAIModel /
// IsMinimaxModel), which cannot express a model served by two
// hosts (DeepSeek direct vs OpenRouter), a local runtime, or an id with no
// recognisable prefix. A run now resolves a profile — explicitly chosen, or the
// catalog's provider for the model — and the profile names the wire protocol,
// base URL and key slot. There is no fallback to another profile's key.

enum class EKismetAIWireProtocol : uint8
{
	AnthropicMessages,  // POST {base}/v1/messages, Anthropic SSE
	OpenAIResponses,    // POST {base}/v1/responses, Responses SSE
	OpenAIChat,         // POST {base}/chat/completions, Chat Completions SSE
};

enum class EKismetAIAuthStyle : uint8
{
	None,             // local runtimes
	XApiKey,          // x-api-key: <key>
	Bearer,           // Authorization: Bearer <key>
	XApiKeyAndBearer, // both — third-party Anthropic-compatible hosts disagree on which they read
};

struct KISMETAIRUNTIME_API FKismetAIProviderProfile
{
	FString Id;
	FString DisplayName;
	EKismetAIWireProtocol Protocol = EKismetAIWireProtocol::AnthropicMessages;
	FString BaseUrl;            // default; may be empty for custom profiles
	FString BaseUrlEnvVar;      // optional override from the environment
	FString KeySlot;            // keystore slot; empty = no key (local)
	FString KeyEnvVar;          // optional key from the environment
	EKismetAIAuthStyle Auth = EKismetAIAuthStyle::XApiKey;
	TMap<FString, FString> ExtraHeaders;
	bool bPromptCacheMarkers = false;   // Anthropic cache_control breakpoints
	bool bSystemAsString = false;       // send "system" as a string (some compat hosts)
	bool bLocal = false;                // runs on this machine; no key required
	bool bCustomModels = false;         // the user types any model id
	FString StripModelSuffix;           // removed from the wire model id
	FString KeyUrl;                     // where the user gets a key
};

class KISMETAIRUNTIME_API FKismetAIProviderRegistry
{
public:
	// The shipped profiles (Config/providers.json, embedded copy first). Loaded once.
	static const FKismetAIProviderRegistry& Get();

	bool LoadFromJson(const FString& Json, FString& OutError);
	bool IsLoaded() const { return bLoaded; }

	const FKismetAIProviderProfile* Find(const FString& ProviderId) const;
	const TArray<FKismetAIProviderProfile>& GetProfiles() const { return Profiles; }

private:
	TArray<FKismetAIProviderProfile> Profiles;
	bool bLoaded = false;
};

namespace KismetAIProviders
{
	// The profile id a run uses: Config.ProviderId when set, else the catalog's
	// provider for Config.Model, else the legacy prefix rule (gpt-/o-series ->
	// openai, minimax- -> minimax, anything else -> anthropic) so
	// a saved model id that predates the catalog still goes where it always went.
	KISMETAIRUNTIME_API FString ResolveProviderId(const FKismetAIAgentConfig& Config);

	// Refuse unavailable routes before admission or transport. Anthropic has a
	// built-in profile even if providers.json could not be loaded.
	KISMETAIRUNTIME_API bool ValidateSelection(const FKismetAIAgentConfig& Config, FString& OutError,
		const FKismetAIProviderRegistry& Registry = FKismetAIProviderRegistry::Get());

	// Same, for a model id alone.
	KISMETAIRUNTIME_API FString ProviderIdForModel(const FString& Model);

	// The resolved profile. Never null: an unknown id resolves to a built-in
	// Anthropic profile and logs an error, matching the pre-profile default.
	KISMETAIRUNTIME_API const FKismetAIProviderProfile& ResolveProfile(const FKismetAIAgentConfig& Config);

	// Base URL without a trailing slash: settings override, then the profile's
	// environment variable, then the profile default. For the anthropic profile
	// a non-default Config.ApiBaseUrl is honoured too (it predates profiles).
	KISMETAIRUNTIME_API FString GetBaseUrl(const FKismetAIAgentConfig& Config, const FKismetAIProviderProfile& Profile);

	// The key for this profile's slot. The anthropic/openai/minimax slots
	// live in their historical config fields; every other slot in
	// Config.ProviderKeys. Empty when unset. Never another slot's key.
	KISMETAIRUNTIME_API FString GetApiKey(const FKismetAIAgentConfig& Config, const FKismetAIProviderProfile& Profile);

	// Write a key into the config field that GetApiKey reads for Slot.
	KISMETAIRUNTIME_API void SetApiKey(FKismetAIAgentConfig& Config, const FString& Slot, const FString& Key);

	// True when the profile needs no key or has one.
	KISMETAIRUNTIME_API bool HasUsableKey(const FKismetAIAgentConfig& Config, const FKismetAIProviderProfile& Profile);

	// Auth headers for curl (without the -H wrapper), per the profile's style.
	KISMETAIRUNTIME_API TArray<FString> BuildAuthHeaders(const FKismetAIAgentConfig& Config, const FKismetAIProviderProfile& Profile);

	// Model id as sent on the wire (StripModelSuffix applied).
	KISMETAIRUNTIME_API FString WireModelId(const FKismetAIAgentConfig& Config, const FKismetAIProviderProfile& Profile);
}
