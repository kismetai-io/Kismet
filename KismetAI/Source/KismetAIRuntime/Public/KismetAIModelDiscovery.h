// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "KismetAIModelCatalog.h"

struct FKismetAIProviderProfile;
struct FKismetAIAgentConfig;

// What providers report about their models, and how that meets the curated
// catalog. Two sources:
//
// - Cloud Models APIs (OpenRouter, Anthropic, OpenAI, Google, DeepSeek,
//   Moonshot): the curated table keeps the names and the default; the live list
//   adds limits, capabilities and newer models of the same families
//   (MergeCatalog, the same rules as Eden's merge_catalog).
// - Local runtimes (Ollama, LM Studio, llama.cpp/vLLM behind a custom
//   endpoint): there is no curated table, so the discovered models become the
//   rows (MakeLocalEntries), priced at 0, with the reported context window,
//   the user's own figure, or a conservative 32k.
//
// Everything here is pure except FKismetAICurlHttpFetcher; tests drive it with
// a fake IKismetAIHttpFetcher (Tests/Fakes/FixtureTransport.h).

struct FKismetAIHttpRequest
{
	FString Verb = TEXT("GET");
	FString Url;
	TArray<FString> Headers;    // "Name: value"
	FString Body;               // sent as application/json when not empty
	int32 TimeoutSeconds = 10;
};

struct FKismetAIHttpResponse
{
	int32 Status = 0;           // 0 = nothing answered (refused, DNS, timeout)
	FString Body;
	bool Answered() const { return Status > 0; }
	bool Ok() const { return Status >= 200 && Status < 300; }
};

// Blocking request. Call it off the game thread.
class KISMETAIRUNTIME_API IKismetAIHttpFetcher
{
public:
	virtual ~IKismetAIHttpFetcher() = default;
	virtual FKismetAIHttpResponse Send(const FKismetAIHttpRequest& Request) = 0;
};

// curl, as the model client uses: headers (which carry keys) go in a private
// --config file, never in argv.
class KISMETAIRUNTIME_API FKismetAICurlHttpFetcher final : public IKismetAIHttpFetcher
{
public:
	virtual FKismetAIHttpResponse Send(const FKismetAIHttpRequest& Request) override;
};

// One model as a provider's list endpoint reported it. Zero / unset = not reported.
struct KISMETAIRUNTIME_API FKismetAILiveModel
{
	FString Id;
	FString DisplayName;
	int32 ContextWindow = 0;
	int32 MaxOutputTokens = 0;
	int64 Created = 0;                       // unix seconds
	TOptional<FKismetAIModelPrice> Price;    // $ per million tokens
	TOptional<bool> bImageInput;
	TOptional<bool> bToolCalls;
	TOptional<bool> bReasoning;
	bool bLoaded = false;                    // LM Studio: loaded in memory now
};

enum class EKismetAIModelsFormat : uint8
{
	None,        // the provider publishes no list (Z.ai, MiniMax)
	OpenRouter,  // {data:[{id,name,created,context_length,top_provider,architecture,supported_parameters,pricing}]}
	Anthropic,   // {data:[{id,display_name,created_at,max_input_tokens,max_tokens,capabilities}]}
	OpenAI,      // {data:[{id,created,...}]}: OpenAI, DeepSeek, Moonshot, vLLM, llama.cpp, LM Studio /v1
	Google,      // {models:[{name:"models/x",displayName,inputTokenLimit,outputTokenLimit}]}
	Ollama,      // /api/tags {models:[{name,modified_at,details}]} + /api/show per model
	LMStudio,    // /api/v1/models {models:[{type,key,display_name,max_context_length,loaded_instances,capabilities}]}
};

namespace KismetAIModelsApi
{
	// Context assumed for a local model that reports none. Most local runtimes
	// load far less than a cloud model's window; the UI says it is assumed.
	inline constexpr int32 LocalDefaultContext = 32768;
	// Models asked for details (/api/show) per Ollama refresh.
	inline constexpr int32 MaxOllamaShowRequests = 64;

	KISMETAIRUNTIME_API bool ParseFormat(const FString& Name, EKismetAIModelsFormat& Out);

	// Parse a list body. False (and OutError) on a body that is not the format,
	// so the caller falls back to the curated list.
	KISMETAIRUNTIME_API bool Parse(EKismetAIModelsFormat Format, const FString& Body,
		TArray<FKismetAILiveModel>& OutModels, FString& OutError);

	// Fold an Ollama /api/show body into Model: context from
	// model_info["<arch>.context_length"] (or num_ctx in parameters), tool and
	// vision support from capabilities. False on an unparsable body.
	KISMETAIRUNTIME_API bool ApplyOllamaShow(const FString& Body, FKismetAILiveModel& Model);

	// "claude-opus-5-5" and "claude-opus-5-6" share "claude-opus"; "gpt-6.1-sol"
	// is "gpt-sol"; "kimi-k3.1-pro" is "kimi-k-pro". Digits never name a family.
	KISMETAIRUNTIME_API FString FamilyKey(const FString& Id);

	// Dated snapshots and pinned variants (claude-opus-5-5-20261101,
	// gpt-6.1-sol-2026-11-01, x-ai/grok-4.7:batch) are never added as new rows.
	KISMETAIRUNTIME_API bool IsSnapshotOrVariant(const FString& Id);

	struct FMergeResult
	{
		TArray<FKismetAIModelEntry> PickerRows;   // curated (updated) in curated order, then added, newest first
		TArray<FKismetAIModelEntry> Overlay;      // every row that differs from the shipped entry
		TArray<FString> Dropped;                  // curated ids the vendor no longer lists
		TArray<FString> Added;
		bool bUsedLive = false;                   // false: nothing matched, curated rows returned unchanged
	};

	// Curated = the provider's shipped picker rows. A curated model keeps its
	// name and takes the live limits and capabilities (and OpenRouter's price);
	// one the vendor no longer lists is dropped from the picker (it still
	// resolves); a live model newer than the newest curated model of its family
	// is added after the curated ones. A list that matches no curated model at
	// all is treated as wrong, and the curated rows are kept.
	KISMETAIRUNTIME_API FMergeResult MergeCatalog(const TArray<const FKismetAIModelEntry*>& Curated,
		const TArray<FKismetAILiveModel>& Live);

	// Rows for discovered local models. Context: the user's figure
	// (UserContext, lowercase id), else the reported one, else LocalDefaultContext.
	// Capabilities are the catalog's conservative unknown-model set; price 0.
	KISMETAIRUNTIME_API TArray<FKismetAIModelEntry> MakeLocalEntries(const FString& ProviderId,
		const TArray<FKismetAILiveModel>& Models, const TMap<FString, int32>& UserContext);

	// One row for a model id the user typed on a local or custom profile that
	// the server did not list (or has no list).
	KISMETAIRUNTIME_API FKismetAIModelEntry MakeTypedLocalEntry(const FString& ProviderId,
		const FString& ModelId, const TMap<FString, int32>& UserContext);

	// "{base}" = the profile's resolved base URL, "{origin}" = its scheme://host[:port].
	KISMETAIRUNTIME_API FString ExpandUrl(const FString& Template, const FString& BaseUrl);

	// GET target that proves the server is up. Any HTTP answer counts, even 404.
	KISMETAIRUNTIME_API FString HealthUrl(const FKismetAIProviderProfile& Profile, const FString& BaseUrl);

	// "Ollama not running at http://localhost:11434. ..." for an unanswered probe.
	KISMETAIRUNTIME_API FString NotRunningMessage(const FKismetAIProviderProfile& Profile, const FString& BaseUrl);

	// Ollama: "... does not support tools"; llama.cpp, LM Studio and vLLM word it differently.
	KISMETAIRUNTIME_API bool IsToolsUnsupportedMessage(const FString& Message);
	KISMETAIRUNTIME_API FString ToolsUnsupportedMessage(const FString& ProviderName, const FString& ModelId);

	// Before the first request of a run on a local or custom profile: is the
	// server up, and does the model (as the server reported it) take tools?
	// False with OutErrorType "local_server_unreachable" or "tools_unsupported"
	// and a message for the user. Cloud profiles always pass.
	KISMETAIRUNTIME_API bool PreflightLocal(const FKismetAIAgentConfig& Config, const FKismetAIProviderProfile& Profile,
		IKismetAIHttpFetcher& Fetcher, FString& OutErrorType, FString& OutMessage,
		const FKismetAIModelCatalog& Catalog = FKismetAIModelCatalog::Get());
}
