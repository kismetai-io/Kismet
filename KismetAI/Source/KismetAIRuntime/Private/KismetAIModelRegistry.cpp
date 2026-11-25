// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIModelRegistry.h"
#include "KismetAIAgent.h"
#include "KismetAILog.h"
#include "KismetAIPaths.h"
#include "KismetAIProviderProfile.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogKismetAIModelRegistry, Log, All);

FKismetAIModelRegistry::FKismetAIModelRegistry(FKismetAIModelCatalog& InCatalog, const FKismetAIProviderRegistry& InProviders,
	TSharedRef<IKismetAIHttpFetcher> InFetcher, const FString& InCacheDir)
	: Catalog(InCatalog)
	, Providers(InProviders)
	, Fetcher(InFetcher)
	, CacheDir(InCacheDir)
{
}

FKismetAIModelRegistry& FKismetAIModelRegistry::Get()
{
	static FKismetAIModelRegistry Registry(FKismetAIModelCatalog::GetMutable(), FKismetAIProviderRegistry::Get(),
		MakeShared<FKismetAICurlHttpFetcher>(), KismetAIPaths::GetConfigDir() / TEXT("models_cache"));
	return Registry;
}

FString FKismetAIModelRegistry::CachePath(const FString& ProviderId) const
{
	return CacheDir.IsEmpty() ? FString() : CacheDir / (ProviderId + TEXT(".json"));
}

bool FKismetAIModelRegistry::FetchList(const FKismetAIAgentConfig& Config, const FKismetAIProviderProfile& Profile, bool bUseCache,
	FString& OutBody, FDateTime& OutFetchedAt, bool& bOutReachable, FString& OutError)
{
	bOutReachable = true;
	const FString BaseUrl = KismetAIProviders::GetBaseUrl(Config, Profile);
	const FString Url = KismetAIModelsApi::ExpandUrl(Profile.ModelsUrl, BaseUrl);
	const FString Path = KismetAIProviders::IsSelfHosted(Profile) ? FString() : CachePath(Profile.Id);

	// The cache holds the raw body, keyed by URL, so a changed base URL refetches.
	if (bUseCache && !Path.IsEmpty())
	{
		FString Cached;
		TSharedPtr<FJsonObject> Root;
		if (FFileHelper::LoadFileToString(Cached, *Path)
			&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Cached), Root) && Root.IsValid())
		{
			FString CachedUrl;
			FString Body;
			double FetchedUnix = 0.0;
			Root->TryGetStringField(TEXT("url"), CachedUrl);
			Root->TryGetStringField(TEXT("body"), Body);
			Root->TryGetNumberField(TEXT("fetched_at"), FetchedUnix);
			const FDateTime Fetched = FDateTime::FromUnixTimestamp(static_cast<int64>(FetchedUnix));
			if (CachedUrl == Url && !Body.IsEmpty() && (FDateTime::UtcNow() - Fetched).GetTotalSeconds() < CacheTtlSeconds)
			{
				OutBody = MoveTemp(Body);
				OutFetchedAt = Fetched;
				return true;
			}
		}
	}

	FKismetAIHttpRequest Request;
	Request.Url = Url;
	Request.Headers = KismetAIProviders::BuildModelsHeaders(Config, Profile);
	Request.TimeoutSeconds = Profile.bLocal ? 4 : 10;
	const FKismetAIHttpResponse Response = Fetcher->Send(Request);
	if (!Response.Answered())
	{
		bOutReachable = false;
		OutError = FString::Printf(TEXT("nothing answered at %s"), *Url);
		return false;
	}
	if (!Response.Ok())
	{
		OutError = FString::Printf(TEXT("HTTP %d from %s"), Response.Status, *Url);
		return false;
	}
	OutBody = Response.Body;
	OutFetchedAt = FDateTime::UtcNow();

	if (!Path.IsEmpty())
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("url"), Url);
		Root->SetNumberField(TEXT("fetched_at"), static_cast<double>(OutFetchedAt.ToUnixTimestamp()));
		Root->SetStringField(TEXT("body"), OutBody);
		FString Text;
		FJsonSerializer::Serialize(Root.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
		IFileManager::Get().MakeDirectory(*CacheDir, /*Tree=*/true);
		FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
	return true;
}

void FKismetAIModelRegistry::DiscoverOllamaDetails(const FKismetAIAgentConfig& Config, const FKismetAIProviderProfile& Profile,
	TArray<FKismetAILiveModel>& Models)
{
	const FString Url = KismetAIModelsApi::ExpandUrl(TEXT("{base}/api/show"), KismetAIProviders::GetBaseUrl(Config, Profile));
	const int32 Count = FMath::Min(Models.Num(), KismetAIModelsApi::MaxOllamaShowRequests);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("model"), Models[Index].Id);
		FKismetAIHttpRequest Request;
		Request.Verb = TEXT("POST");
		Request.Url = Url;
		FJsonSerializer::Serialize(Body.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Request.Body));
		Request.TimeoutSeconds = 4;
		const FKismetAIHttpResponse Response = Fetcher->Send(Request);
		if (Response.Ok())
		{
			KismetAIModelsApi::ApplyOllamaShow(Response.Body, Models[Index]);
		}
	}
}

void FKismetAIModelRegistry::RefreshProvider(const FKismetAIAgentConfig& Config, const FKismetAIProviderProfile& Profile, bool bUseCache)
{
	EKismetAIModelsFormat Format = EKismetAIModelsFormat::None;
	if (Profile.ModelsUrl.IsEmpty() || !KismetAIModelsApi::ParseFormat(Profile.ModelsFormat, Format) || Format == EKismetAIModelsFormat::None)
	{
		return;
	}
	const bool bDiscovery = KismetAIProviders::IsSelfHosted(Profile);
	if (!bDiscovery && !KismetAIProviders::HasUsableKey(Config, Profile))
	{
		return;  // no key, no list: the curated rows stand
	}
	if (bDiscovery && KismetAIProviders::GetBaseUrl(Config, Profile).IsEmpty())
	{
		return;  // a custom profile nobody configured
	}
	{
		FScopeLock Guard(&Lock);
		if (InFlight.Contains(Profile.Id))
		{
			return;
		}
		InFlight.Add(Profile.Id);
	}

	FKismetAIProviderModelState State;
	State.ProviderId = Profile.Id;
	State.bLocal = bDiscovery;
	FString Body;
	FString Error;
	bool bReachable = true;
	TArray<FKismetAILiveModel> Live;
	if (FetchList(Config, Profile, bUseCache, Body, State.FetchedAtUtc, bReachable, Error)
		&& KismetAIModelsApi::Parse(Format, Body, Live, Error))
	{
		if (bDiscovery)
		{
			if (Format == EKismetAIModelsFormat::Ollama)
			{
				DiscoverOllamaDetails(Config, Profile, Live);
			}
			State.Discovered = Live;
			State.Rows = KismetAIModelsApi::MakeLocalEntries(Profile.Id, Live, Config.ModelContextWindows);
			State.bLive = true;
		}
		else
		{
			TArray<const FKismetAIModelEntry*> Curated;
			for (const FKismetAIModelEntry* Entry : Catalog.GetPickerModels())
			{
				if (Entry->Provider == Profile.Id)
				{
					Curated.Add(Entry);
				}
			}
			KismetAIModelsApi::FMergeResult Merged = KismetAIModelsApi::MergeCatalog(Curated, Live);
			if (Merged.bUsedLive)
			{
				State.bLive = true;
				State.Rows = MoveTemp(Merged.PickerRows);
				Catalog.SetRuntimeEntries(Profile.Id, Merged.Overlay);
				KISMETAI_LOG(LogKismetAIModelRegistry, Log, TEXT("%s model list: %d rows, %d added, %d no longer listed"),
					*Profile.Id, State.Rows.Num(), Merged.Added.Num(), Merged.Dropped.Num());
			}
			else
			{
				Error = TEXT("the list names none of the curated models");
			}
		}
	}
	State.bReachable = bReachable;
	State.Error = Error;
	if (bDiscovery && State.bLive)
	{
		// Keep a typed model the server did not list resolvable.
		TArray<FKismetAIModelEntry> Overlay = State.Rows;
		Catalog.SetRuntimeEntries(Profile.Id, Overlay);
	}
	if (!Error.IsEmpty())
	{
		KISMETAI_LOG(LogKismetAIModelRegistry, Verbose, TEXT("%s model list unavailable (%s); keeping the curated rows"), *Profile.Id, *Error);
	}
	Publish(MoveTemp(State));
}

void FKismetAIModelRegistry::Publish(FKismetAIProviderModelState&& State)
{
	FScopeLock Guard(&Lock);
	InFlight.Remove(State.ProviderId);
	if (!State.bLive)
	{
		// A failed refresh never throws away rows an earlier one got.
		if (FKismetAIProviderModelState* Previous = States.Find(State.ProviderId); Previous && Previous->bLive && State.bReachable)
		{
			Previous->Error = State.Error;
			return;
		}
	}
	States.Add(State.ProviderId, MoveTemp(State));
}

void FKismetAIModelRegistry::RefreshAllAsync(const FKismetAIAgentConfig& Config, bool bLocalOnly)
{
	TArray<FKismetAIProviderProfile> Profiles;
	for (const FKismetAIProviderProfile& Profile : Providers.GetProfiles())
	{
		const bool bDiscovery = KismetAIProviders::IsSelfHosted(Profile);
		if (!Profile.ModelsUrl.IsEmpty() && (!bLocalOnly || bDiscovery))
		{
			Profiles.Add(Profile);
		}
	}
	if (Profiles.Num() == 0)
	{
		return;
	}
	Async(EAsyncExecution::ThreadPool, [this, Config, Profiles = MoveTemp(Profiles)]()
	{
		for (const FKismetAIProviderProfile& Profile : Profiles)
		{
			RefreshProvider(Config, Profile);
		}
	});
}

TArray<FKismetAIModelEntry> FKismetAIModelRegistry::GetPickerRows() const
{
	TMap<FString, FKismetAIProviderModelState> Snapshot;
	{
		FScopeLock Guard(&Lock);
		Snapshot = States;
	}
	auto IsLiveGroup = [&Snapshot](const FString& ProviderId)
	{
		const FKismetAIProviderModelState* State = Snapshot.Find(ProviderId);
		return State && State->bLive && !State->bLocal;
	};
	TArray<FKismetAIModelEntry> Rows;
	TSet<FString> Replaced;
	for (const FKismetAIModelEntry* Entry : Catalog.GetPickerModels())
	{
		if (!IsLiveGroup(Entry->Provider))
		{
			Rows.Add(*Entry);
		}
		else if (!Replaced.Contains(Entry->Provider))
		{
			// The whole group is replaced by its merged rows where it starts.
			Replaced.Add(Entry->Provider);
			Rows.Append(Snapshot.FindChecked(Entry->Provider).Rows);
		}
	}
	for (const FKismetAIProviderProfile& Profile : Providers.GetProfiles())
	{
		const FKismetAIProviderModelState* State = Snapshot.Find(Profile.Id);
		if (State && State->bLive && State->bLocal)
		{
			for (const FKismetAIModelEntry& Row : State->Rows)
			{
				if (Row.bInPicker)
				{
					Rows.Add(Row);
				}
			}
		}
	}
	return Rows;
}

TOptional<FKismetAIProviderModelState> FKismetAIModelRegistry::GetState(const FString& ProviderId) const
{
	FScopeLock Guard(&Lock);
	if (const FKismetAIProviderModelState* State = States.Find(ProviderId))
	{
		return *State;
	}
	return TOptional<FKismetAIProviderModelState>();
}

FString FKismetAIModelRegistry::GetSourceLabel(const FString& ProviderId) const
{
	const FKismetAIProviderProfile* Profile = Providers.Find(ProviderId);
	const TOptional<FKismetAIProviderModelState> State = GetState(ProviderId);
	if (State.IsSet() && State->bLive)
	{
		return FString::Printf(TEXT("live, %s"), *State->FetchedAtUtc.ToString(TEXT("%Y-%m-%d %H:%M UTC")));
	}
	if (State.IsSet() && !State->bReachable && Profile && KismetAIProviders::IsSelfHosted(*Profile))
	{
		return TEXT("not running");
	}
	if (Profile && Profile->ModelsUrl.IsEmpty())
	{
		return TEXT("curated (no list endpoint)");
	}
	return TEXT("curated");
}

void FKismetAIModelRegistry::ApplyUserContext(const FKismetAIAgentConfig& Config, const FString& ProviderId, const FString& ModelId)
{
	TArray<FKismetAIModelEntry> Rows;
	{
		FScopeLock Guard(&Lock);
		FKismetAIProviderModelState* State = States.Find(ProviderId);
		if (State && State->bLocal)
		{
			State->Rows = KismetAIModelsApi::MakeLocalEntries(ProviderId, State->Discovered, Config.ModelContextWindows);
			Rows = State->Rows;
		}
	}
	if (Rows.Num() > 0)
	{
		Catalog.SetRuntimeEntries(ProviderId, Rows);
	}
	if (!ModelId.IsEmpty() && !Rows.ContainsByPredicate([&ModelId](const FKismetAIModelEntry& Row) { return Row.Id.Equals(ModelId, ESearchCase::IgnoreCase); }))
	{
		RegisterTypedModel(Config, ProviderId, ModelId);
	}
}

void FKismetAIModelRegistry::RegisterTypedModel(const FKismetAIAgentConfig& Config, const FString& ProviderId, const FString& ModelId)
{
	if (ModelId.IsEmpty())
	{
		return;
	}
	const FKismetAIModelEntry* Existing = Catalog.Find(ModelId);
	if (Existing && !Existing->bRuntime && !Existing->bPatternOnly)
	{
		return;  // a catalog model typed on a custom endpoint keeps its own facts
	}
	TArray<FKismetAIModelEntry> Rows;
	{
		FScopeLock Guard(&Lock);
		FKismetAIProviderModelState& State = States.FindOrAdd(ProviderId);
		State.ProviderId = ProviderId;
		State.bLocal = true;
		bool bListed = false;
		for (FKismetAIModelEntry& Row : State.Rows)
		{
			if (Row.Id.Equals(ModelId, ESearchCase::IgnoreCase))
			{
				bListed = true;
			}
		}
		if (!bListed)
		{
			FKismetAIModelEntry Typed = KismetAIModelsApi::MakeTypedLocalEntry(ProviderId, ModelId, Config.ModelContextWindows);
			Typed.bInPicker = false;  // typed, not discovered: resolvable but not offered
			State.Rows.Add(Typed);
		}
		Rows = State.Rows;
	}
	Catalog.SetRuntimeEntries(ProviderId, Rows);
}
