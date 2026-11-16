// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "KismetAILLMProvider.h"
#include "KismetAIProviderProfile.h"
#include "KismetAIModelCatalog.h"
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ─────────────────────────────────────────────────────────────────────────────
// FOpenAIChatProvider — ILLMProvider for the OpenAI Chat Completions wire
// protocol (POST {base}/chat/completions, data-only SSE ending in [DONE]).
//
// One class serves every host that speaks it: OpenRouter, Gemini's
// OpenAI-compatible endpoint, vLLM, llama.cpp, Groq/Together/Fireworks and
// custom servers. What differs between them — base URL, auth style, extra
// headers, key slot, wire model id — comes from the FKismetAIProviderProfile
// the provider is constructed with, never from the model id.
//
// Translates KismetAI's canonical (Anthropic-shaped) messages/tools into a
// Chat Completions request, and the chunk stream back into the canonical
// response JSON (content[] text/thinking/tool_use, stop_reason, usage).
//
// Usage is reported with Anthropic's disjoint semantics (input_tokens excludes
// cached tokens), which is what the agent assumes for every provider whose
// GetName() is not "openai". GetName() therefore returns the profile id and
// never "openai".
// ─────────────────────────────────────────────────────────────────────────────
class FOpenAIChatProvider : public ILLMProvider
{
public:
	explicit FOpenAIChatProvider(const FKismetAIProviderProfile& InProfile);

	// ILLMProvider
	virtual FString GetName() const override;
	virtual int32 CacheTtlSeconds() const override;
	virtual FString GetEndpointUrl(const FKismetAIAgentConfig& Config) const override;
	virtual TSharedPtr<FJsonObject> BuildRequestBody(
		const FKismetAIAgentConfig& Config,
		const TArray<FAgentMessage>& Messages,
		const TArray<TSharedPtr<FJsonValue>>& Tools,
		const FString& SystemPrompt,
		FLLMRequestShape& OutShape) const override;
	virtual TArray<FString> GetExtraCurlHeaders(
		const FKismetAIAgentConfig& Config,
		const FLLMRequestShape& Shape) const override;
	virtual TSharedPtr<ILLMStreamState> CreateStreamState() const override;
	virtual void ParseErrorBody(
		const TSharedPtr<FJsonObject>& Body,
		FModelCallOutcome& Outcome) const override;

	// BuildRequestBody with the model's capabilities passed in rather than read
	// from the shipped catalog. BuildRequestBody calls this with
	// FKismetAIModelCatalog::Get().FindOrDefaults(Config.Model).Caps; tests call
	// it directly so they do not depend on what models.json currently says.
	TSharedPtr<FJsonObject> BuildRequestBodyWithCaps(
		const FKismetAIAgentConfig& Config,
		const TArray<FAgentMessage>& Messages,
		const TArray<TSharedPtr<FJsonValue>>& Tools,
		const FString& SystemPrompt,
		const FKismetAIModelCapabilities& Caps,
		FLLMRequestShape& OutShape) const;

	const FKismetAIProviderProfile& GetProfile() const { return Profile; }

	// Text prepended to a tool_result that carried is_error, so the model can
	// tell a failed call from a successful one (Chat Completions has no flag).
	static const TCHAR* ToolErrorPrefix;

private:
	// Translate one canonical message into zero or more Chat Completions
	// messages appended to OutMessages.
	static void TranslateMessage(
		const TSharedPtr<FJsonObject>& CanonicalMsg,
		TArray<TSharedPtr<FJsonValue>>& OutMessages);

	FKismetAIProviderProfile Profile;
};

// ─────────────────────────────────────────────────────────────────────────────
// FOpenAIChatStreamState — accumulates Chat Completions chunks.
//
// The stream is data-only: FKismetAISSEParser hands every chunk over with an
// EMPTY EventType (there are no `event:` lines), and the final `data: [DONE]`
// line is not JSON, so its event carries a null Data and FCurlModelClient
// drops it before it reaches this state. Completion is therefore decided by
// finish_reason, never by [DONE]. A stream that ends without a finish_reason
// reports stream_complete=false and the client retries it as incomplete_stream.
//
// Tool calls are accumulated by their `index`: id and name arrive in the first
// fragment for an index, function.arguments arrive in pieces, and several
// calls may interleave. Arguments are parsed when finish_reason arrives; a
// call whose arguments are not a JSON object rejects the whole turn with
// malformed_tool_arguments (the Responses provider's rule), so no tool from a
// broken response is ever executed. Empty arguments ("") mean "no arguments"
// and become {} — several hosts send that for zero-parameter tools.
// ─────────────────────────────────────────────────────────────────────────────
class FOpenAIChatStreamState : public ILLMStreamState
{
public:
	FOpenAIChatStreamState() = default;

	virtual bool ProcessEvent(
		const FString& EventType,
		const TSharedPtr<FJsonObject>& Data,
		FString& OutDelta) override;

	virtual TSharedPtr<FJsonObject> BuildResponseJson() const override;

	virtual bool HasStreamError(FString& OutMessage, FString& OutCode) const override;

private:
	struct FToolCallAcc
	{
		int32 Index = 0;
		FString Id;
		FString Name;
		FString Arguments;
	};

	void AppendReasoning(const FString& Delta);
	void CloseThinkingSegment();
	void AccumulateToolCalls(const TArray<TSharedPtr<FJsonValue>>& ToolCallValues);
	void ApplyFinishReason(const FString& Reason);
	void Finalize();
	void ExtractUsage(const TSharedPtr<FJsonObject>& UsageObj);
	void RecordStreamError(const TSharedPtr<FJsonObject>& ErrorObj, const FString& FallbackMessage);

	// Thinking: the open segment, plus finished segments in arrival order.
	FString OpenThinking;
	TArray<FString> ThinkingSegments;

	FString Text;
	TArray<FToolCallAcc> ToolCalls;   // in first-seen order

	// Built by Finalize() (on finish_reason).
	TArray<TSharedPtr<FJsonValue>> ToolUseBlocks;
	TArray<TSharedPtr<FJsonValue>> NativeMetadataBlocks;

	FString FinishReason;
	bool bComplete = false;
	bool bFinalized = false;

	bool bHasUsage = false;
	int32 InputTokens = 0;       // disjoint: prompt - cached
	int32 OutputTokens = 0;
	int32 CacheReadTokens = 0;

	bool bStreamError = false;
	FString StreamErrorMessage;
	FString StreamErrorCode;
};
