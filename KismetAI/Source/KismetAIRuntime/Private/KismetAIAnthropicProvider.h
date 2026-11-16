// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "KismetAILLMProvider.h"
#include "KismetAISSEParser.h"
#include "KismetAIProviderProfile.h"

// ─────────────────────────────────────────────────────────────────────────────
// FAnthropicStreamStateAdapter — wraps FAnthropicStreamState to satisfy the
// ILLMStreamState contract. ProcessEvent reconstructs FKismetAISSEEvent from
// EventType + Data and forwards to the inner state; BuildResponseJson delegates
// to inner.BuildResponseJson(); OnThinkingComplete is wired to the inner state.
// ─────────────────────────────────────────────────────────────────────────────
class FAnthropicStreamStateAdapter : public ILLMStreamState
{
public:
	FAnthropicStreamStateAdapter();

	// ILLMStreamState
	virtual bool ProcessEvent(
		const FString& EventType,
		const TSharedPtr<FJsonObject>& Data,
		FString& OutDelta) override;

	virtual TSharedPtr<FJsonObject> BuildResponseJson() const override;
	virtual bool HasStreamError(FString& OutMessage, FString& OutCode) const override;

private:
	FAnthropicStreamState Inner;
	bool bStreamError = false;
	FString StreamErrorMessage;
	FString StreamErrorCode;
};

// ─────────────────────────────────────────────────────────────────────────────
// FAnthropicProvider — ILLMProvider implementation for Anthropic/Claude.
// Logic is lifted verbatim from FKismetAIAgent::CallAnthropicOnce to preserve
// byte-identical request body, headers, URL, and error parsing (parity).
// ─────────────────────────────────────────────────────────────────────────────
class FAnthropicProvider : public ILLMProvider
{
public:
	// The shipped "anthropic" profile.
	FAnthropicProvider();
	// Any Anthropic-Messages profile: Anthropic, DeepSeek, Z.ai, Moonshot,
	// Ollama, LM Studio, custom endpoints.
	explicit FAnthropicProvider(const FKismetAIProviderProfile& InProfile);

	// ILLMProvider
	virtual FString GetName() const override;
	virtual int32 CacheTtlSeconds() const override;

	virtual TSharedPtr<FJsonObject> BuildRequestBody(
		const FKismetAIAgentConfig& Config,
		const TArray<FAgentMessage>& Messages,
		const TArray<TSharedPtr<FJsonValue>>& Tools,
		const FString& SystemPrompt,
		FLLMRequestShape& OutShape) const override;

	virtual FString GetEndpointUrl(const FKismetAIAgentConfig& Config) const override;

	virtual TArray<FString> GetExtraCurlHeaders(
		const FKismetAIAgentConfig& Config,
		const FLLMRequestShape& Shape) const override;

	virtual TSharedPtr<ILLMStreamState> CreateStreamState() const override;

	virtual void ParseErrorBody(
		const TSharedPtr<FJsonObject>& Body,
		FModelCallOutcome& Outcome) const override;

protected:
	FKismetAIProviderProfile Profile;
};
