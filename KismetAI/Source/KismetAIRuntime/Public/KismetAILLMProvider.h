// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "KismetAIAgentTypes.h"
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Native provider metadata is retained for journal fidelity, never sent as
// user content or echoed to a different provider after a model switch.
namespace KismetAIProviderCodec
{
	inline TSharedPtr<FJsonObject> CopyWithout(const TSharedPtr<FJsonObject>& Source, const TArray<FString>& Keys)
	{
		TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
		FJsonObject::Duplicate(Source, Copy);
		for (const FString& Key : Keys) { Copy->RemoveField(Key); }
		return Copy;
	}

	inline TSharedPtr<FJsonValue> Metadata(const FString& Protocol, const TSharedPtr<FJsonObject>& Payload)
	{
		TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
		Block->SetStringField(TEXT("type"), TEXT("_kismet_provider_metadata"));
		Block->SetStringField(TEXT("protocol"), Protocol);
		Block->SetObjectField(TEXT("payload"), CopyWithout(Payload, {}));
		return MakeShared<FJsonValueObject>(Block);
	}

	inline TArray<TSharedPtr<FJsonValue>> EncodeMessages(const TArray<FAgentMessage>& Messages)
	{
		TArray<FAgentMessage> Visible;
		for (const FAgentMessage& Message : Messages)
		{
			FAgentMessage Copy = Message;
			const int32 Removed = Copy.Parts.RemoveAll([](const FAgentContentPart& Part)
			{
				return Part.Type == EAgentPartType::ProviderMetadata;
			});
			if (Removed > 0 && Copy.Parts.IsEmpty()) { continue; }
			Visible.Add(MoveTemp(Copy));
		}
		return KismetAIAgentCodec::ToAnthropicArray(Visible);
	}
}

// Forward-declare types from KismetAIAgent.h so this header can be included
// independently (e.g. from provider .cpp files). KismetAIAgent.h includes this
// header only via a forward declaration for TUniquePtr<ILLMProvider>, not inline.
struct FKismetAIAgentConfig;
struct FModelCallOutcome;

// ─────────────────────────────────────────────────────────────────────────────
// Request shape flags — set by BuildRequestBody so CallLLMOnce can conditionally
// add provider-specific beta headers and track which features are active.
// ─────────────────────────────────────────────────────────────────────────────
struct FLLMRequestShape
{
	bool bThinkingEnabled = false;  // A thinking field was emitted into the body
	bool bEffortEnabled = false;    // An output_config.effort field was emitted
};

// ─────────────────────────────────────────────────────────────────────────────
// ILLMStreamState — provider stream-state accumulator.
// Mirrors FAnthropicStreamState's contract (AnthropicSSEParser.h:173-302).
// ─────────────────────────────────────────────────────────────────────────────
class ILLMStreamState
{
public:
	virtual ~ILLMStreamState() = default;

	// Callback invoked when a complete thinking block is finalized.
	// Wire this up from CallLLMOnce's OnThinkingComplete argument.
	TFunction<void(const FString& ThinkingText)> OnThinkingComplete;

	// Callback invoked for each incremental thinking chunk as it streams, so the
	// UI can show live progress during long thinking phases (not only at the end).
	// Wire this up from CallLLMOnce's OnThinkingDelta argument.
	TFunction<void(const FString& ThinkingDelta)> OnThinkingDelta;

	// Process a single parsed SSE event.
	// Returns true if the event carried a text delta (OutDelta is set).
	virtual bool ProcessEvent(
		const FString& EventType,
		const TSharedPtr<FJsonObject>& Data,
		FString& OutDelta) = 0;

	// Build the canonical response JSON (content[], stop_reason, usage)
	// equivalent to a non-streaming Messages API response.
	// Matches the shape that RunTask's loop and token-tracking code expects.
	virtual TSharedPtr<FJsonObject> BuildResponseJson() const = 0;

	// Report a terminal error/failure that arrived *inside* an otherwise-200
	// stream (e.g. the OpenAI Responses API delivers `error` / `response.failed`
	// events on an HTTP-200 connection). Returns true if such an error occurred,
	// filling OutMessage/OutCode with the upstream detail. The agent's success
	// path uses this to reclassify the call as a retryable error instead of
	// silently treating the empty stream as a normal end_turn.
	// Providers override this when their protocol supports in-stream errors.
	virtual bool HasStreamError(FString& OutMessage, FString& OutCode) const
	{
		return false;
	}
};

// ─────────────────────────────────────────────────────────────────────────────
// ILLMProvider — the narrow seam that isolates provider differences.
// Respects only URL, headers, body construction, stream parsing,
// and error parsing differ per provider.
// ─────────────────────────────────────────────────────────────────────────────
class ILLMProvider
{
public:
	virtual ~ILLMProvider() = default;

	// Short identifying name, e.g. "anthropic" or "openai".
	virtual FString GetName() const = 0;

	// Prompt-cache TTL in seconds for this provider's current configuration.
	// Single source of truth for per-provider TTL.
	// Default is 300s (Anthropic ephemeral / OpenAI in_memory conservative).
	// Non-pure so existing ILLMProvider implementors need no override; a new
	// provider whose TTL differs from the 300s default must override explicitly.
	virtual int32 CacheTtlSeconds() const { return 300; }

	// Build the full JSON request body for the provider's messages API.
	// Populates OutShape with flags that affect header construction.
	// SystemPrompt is passed explicitly so providers that need the system
	// block in the body (Anthropic) vs. in a header (OpenAI) can handle it.
	virtual TSharedPtr<FJsonObject> BuildRequestBody(
		const FKismetAIAgentConfig& Config,
		const TArray<FAgentMessage>& Messages,
		const TArray<TSharedPtr<FJsonValue>>& Tools,
		const FString& SystemPrompt,
		FLLMRequestShape& OutShape) const = 0;

	// Full endpoint URL for the streaming messages call.
	virtual FString GetEndpointUrl(const FKismetAIAgentConfig& Config) const = 0;

	// Provider-specific curl -H headers, returned as strings WITHOUT the
	// surrounding `-H "..."` wrapper (CallLLMOnce builds the wrapper).
	// Includes auth (e.g. x-api-key or Authorization: Bearer),
	// version headers, and conditional beta headers.
	// Content-Type stays in CallLLMOnce (common to all providers).
	virtual TArray<FString> GetExtraCurlHeaders(
		const FKismetAIAgentConfig& Config,
		const FLLMRequestShape& Shape) const = 0;

	// Allocate a fresh stream-state accumulator for one streaming API call.
	virtual TSharedPtr<ILLMStreamState> CreateStreamState() const = 0;

	// Parse a non-200 response body into Outcome's error fields.
	// Called only on the error path.
	virtual void ParseErrorBody(
		const TSharedPtr<FJsonObject>& Body,
		FModelCallOutcome& Outcome) const = 0;
};

// Provider routing lives in provider profiles (KismetAIProviderProfile.h,
// Config/providers.json): a run resolves a profile — Config.ProviderId, else the
// catalog's provider for the model — and the profile's wire protocol picks the
// ILLMProvider. These two helpers answer the old questions from that routing,
// for callers and tests that still ask them by model id.
KISMETAIRUNTIME_API bool IsOpenAIModel(const FString& Model);   // routes to an OpenAI Responses profile
KISMETAIRUNTIME_API bool IsMinimaxModel(const FString& Model);  // routes to the minimax profile

// Factory: the ILLMProvider for the run's resolved profile.
KISMETAIRUNTIME_API TUniquePtr<ILLMProvider> MakeProviderForConfig(const FKismetAIAgentConfig& Config);

// Same, for a model id with no explicit profile (tests, one-off requests).
KISMETAIRUNTIME_API TUniquePtr<ILLMProvider> MakeProviderForModel(const FString& Model);
