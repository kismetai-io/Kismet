// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "KismetAILLMProvider.h"
#include "KismetAIAgent.h"  // EEffortLevel (used by MapEffortLevel) + FKismetAIAgentConfig
#include "KismetAIProviderProfile.h"

// ─────────────────────────────────────────────────────────────────────────────
// FOpenAIProvider — ILLMProvider implementation for OpenAI Responses API.
//
// Translates the canonical (Anthropic-shaped) messages/tools to a /v1/responses
// request and the typed SSE stream back to the same canonical response JSON.
//
// OpenAIApiKey used for auth; Anthropic key never touched.
// All OpenAI-specific logic stays inside this class.
// AS1: Full input[] is sent on every call (stateless multi-turn).
// ─────────────────────────────────────────────────────────────────────────────
class FOpenAIProvider : public ILLMProvider
{
public:
	// The shipped "openai" profile.
	FOpenAIProvider();
	// Any Responses-API profile (openai, custom).
	explicit FOpenAIProvider(const FKismetAIProviderProfile& InProfile);

	// ILLMProvider

	// Returns "openai" — used by factory routing and provider identification.
	virtual FString GetName() const override;
	virtual int32 CacheTtlSeconds() const override;

	// Returns the OpenAI Responses API endpoint.
	// Reads env OPENAI_BASE_URL if set; falls back to https://api.openai.com.
	// Never uses Config.ApiBaseUrl (that is the Anthropic base).
	virtual FString GetEndpointUrl(const FKismetAIAgentConfig& Config) const override;

	// Builds the OpenAI Responses API request body.
	// Translates canonical Anthropic-shaped messages/tools into OpenAI Responses shape:
	//   - input[] (user/assistant text + function_call + function_call_output)
	//   - tools[] ({type:function, name, description, parameters})
	//   - reasoning block for reasoning models (gpt-5*, o1*, o3*, o4*, codex*)
	//   - temperature for non-reasoning models
	// Thinking blocks are dropped (OpenAI manages reasoning server-side).
	virtual TSharedPtr<FJsonObject> BuildRequestBody(
		const FKismetAIAgentConfig& Config,
		const TArray<FAgentMessage>& Messages,
		const TArray<TSharedPtr<FJsonValue>>& Tools,
		const FString& SystemPrompt,
		FLLMRequestShape& OutShape) const override;

	// Returns auth headers: "Authorization: Bearer <OpenAIApiKey>",
	// plus optional "OpenAI-Organization" and "OpenAI-Project" from env.
	// If OpenAIApiKey is empty, still emits Authorization header with empty bearer.
	virtual TArray<FString> GetExtraCurlHeaders(
		const FKismetAIAgentConfig& Config,
		const FLLMRequestShape& Shape) const override;

	// Returns a fresh FOpenAIStreamState that parses the Responses SSE stream
	// into the canonical response JSON (content[]/stop_reason/usage).
	virtual TSharedPtr<ILLMStreamState> CreateStreamState() const override;

	// Reads error.{message,type,code}; on context overflow, best-effort regex
	// fills Outcome.ContextLimit / Outcome.InputTokens.
	virtual void ParseErrorBody(
		const TSharedPtr<FJsonObject>& Body,
		FModelCallOutcome& Outcome) const override;

private:
	// Reasoning models (catalog capability "reasoning"): send reasoning.effort and
	// omit temperature. Covers gpt-5*, gpt5*, gpt-6*, o1/o3/o4, codex.
	static bool IsReasoningModel(const FString& Model);

	// Maps EEffortLevel to the closest reasoning effort the model accepts (catalog
	// "effort" list): GPT-6 takes low..max, GPT-5.x only low/medium/high.
	static FString MapEffortLevel(EEffortLevel Level, const FString& Model);

	// Serialize FJsonObject to a compact JSON string (for tool_use.arguments).
	static FString SerializeJsonObject(const TSharedPtr<FJsonObject>& Obj);

	// Translate a single canonical (Anthropic-shaped) message into zero or more
	// OpenAI Responses input items, appended to OutItems.
	static void TranslateMessage(
		const TSharedPtr<FJsonObject>& CanonicalMsg,
		TArray<TSharedPtr<FJsonValue>>& OutItems);

	// A default-constructed provider follows the run's resolved profile when it
	// is a Responses profile. An explicitly built one keeps its own.
	const FKismetAIProviderProfile& ProfileFor(const FKismetAIAgentConfig& Config) const;

	FKismetAIProviderProfile Profile;
	bool bFollowResolvedProfile = false;
};
