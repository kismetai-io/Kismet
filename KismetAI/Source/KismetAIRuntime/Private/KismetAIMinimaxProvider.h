// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "KismetAILLMProvider.h"
#include "KismetAIProviderProfile.h"

// FMinimaxProvider — ILLMProvider implementation for MiniMax language models.
// MiniMax exposes an Anthropic-compatible Messages API, so this provider keeps
// Anthropic-shaped messages/tools and stream parsing while isolating endpoint,
// auth, and MiniMax-specific body details.
class FMinimaxProvider : public ILLMProvider
{
public:
	// The shipped "minimax" profile.
	FMinimaxProvider();
	explicit FMinimaxProvider(const FKismetAIProviderProfile& InProfile);

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

private:
	FKismetAIProviderProfile Profile;
};
