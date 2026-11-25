// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "KismetAIModelCatalog.h"
#include "KismetAIModelDiscovery.h"
#include "HAL/CriticalSection.h"

struct FKismetAIAgentConfig;
struct FKismetAIProviderProfile;
class FKismetAIProviderRegistry;

// Where a provider's picker rows came from.
struct KISMETAIRUNTIME_API FKismetAIProviderModelState
{
	FString ProviderId;
	bool bLive = false;            // rows come from the provider's list (merged or discovered)
	bool bLocal = false;           // discovered rows of a local or custom profile
	bool bReachable = true;        // false: nothing answered at the list URL
	FDateTime FetchedAtUtc;
	FString Error;                 // last failure, kept for the settings line
	TArray<FKismetAIModelEntry> Rows;
	TArray<FKismetAILiveModel> Discovered;   // local profiles: the raw list, for context edits
};

// The picker's model list at runtime: the curated catalog, refreshed from each
// provider's Models API once per session (with a short disk cache), plus the
// models installed in local runtimes. A failure of any kind keeps the curated
// rows and records why; nothing here ever blocks the game thread.
class KISMETAIRUNTIME_API FKismetAIModelRegistry
{
public:
	FKismetAIModelRegistry(FKismetAIModelCatalog& InCatalog, const FKismetAIProviderRegistry& InProviders,
		TSharedRef<IKismetAIHttpFetcher> InFetcher, const FString& InCacheDir);

	// Shipped catalog and profiles, curl fetcher, cache in ~/.kismet/models_cache.
	static FKismetAIModelRegistry& Get();

	static constexpr double CacheTtlSeconds = 6.0 * 3600.0;

	// Blocking: call off the game thread (tests call it directly with a fake
	// fetcher). Skips a cloud profile without a key. bUseCache=false refetches.
	void RefreshProvider(const FKismetAIAgentConfig& Config, const FKismetAIProviderProfile& Profile, bool bUseCache = true);

	// Every profile that has a list, on the thread pool. Returns at once.
	void RefreshAllAsync(const FKismetAIAgentConfig& Config, bool bLocalOnly = false);

	// Picker rows: the catalog's groups in file order, each replaced by its live
	// rows where a refresh succeeded, then the discovered local groups.
	TArray<FKismetAIModelEntry> GetPickerRows() const;

	// "curated", "live, 14:32", "not running" or "no list" — shown in the picker.
	FString GetSourceLabel(const FString& ProviderId) const;
	TOptional<FKismetAIProviderModelState> GetState(const FString& ProviderId) const;

	// Rebuild a local provider's rows after the user set a context window
	// (Config.ModelContextWindows already holds the new figure).
	void ApplyUserContext(const FKismetAIAgentConfig& Config, const FString& ProviderId, const FString& ModelId);

	// A model id typed for a local or custom profile that the server did not
	// list: resolvable, priced 0, with the user's or the assumed context window.
	void RegisterTypedModel(const FKismetAIAgentConfig& Config, const FString& ProviderId, const FString& ModelId);

private:
	bool FetchList(const FKismetAIAgentConfig& Config, const FKismetAIProviderProfile& Profile, bool bUseCache,
		FString& OutBody, FDateTime& OutFetchedAt, bool& bOutReachable, FString& OutError);
	void DiscoverOllamaDetails(const FKismetAIAgentConfig& Config, const FKismetAIProviderProfile& Profile,
		TArray<FKismetAILiveModel>& Models);
	void Publish(FKismetAIProviderModelState&& State);
	FString CachePath(const FString& ProviderId) const;

	FKismetAIModelCatalog& Catalog;
	const FKismetAIProviderRegistry& Providers;
	TSharedRef<IKismetAIHttpFetcher> Fetcher;
	FString CacheDir;

	mutable FCriticalSection Lock;
	TMap<FString, FKismetAIProviderModelState> States;
	TSet<FString> InFlight;
};
