// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIMinimaxProvider.h"
#include "KismetAIAgent.h"
#include "KismetAIAnthropicProvider.h"
#include "KismetAIModelCatalog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformMisc.h"

FMinimaxProvider::FMinimaxProvider()
{
	FKismetAIAgentConfig Default;
	Default.ProviderId = TEXT("minimax");
	Profile = KismetAIProviders::ResolveProfile(Default);
}

FMinimaxProvider::FMinimaxProvider(const FKismetAIProviderProfile& InProfile)
	: Profile(InProfile)
{
}

FString FMinimaxProvider::GetName() const
{
	return TEXT("minimax");
}

int32 FMinimaxProvider::CacheTtlSeconds() const
{
	// Explicit Anthropic-compatible cache breakpoints are 5 minutes. MiniMax-M3
	// also has passive caching, but the agent accounting uses this conservative TTL.
	return 300;
}

TSharedPtr<FJsonObject> FMinimaxProvider::BuildRequestBody(
	const FKismetAIAgentConfig& Config,
	const TArray<FAgentMessage>& CanonicalMessages,
	const TArray<TSharedPtr<FJsonValue>>& Tools,
	const FString& SystemPrompt,
	FLLMRequestShape& OutShape) const
{
	const TArray<TSharedPtr<FJsonValue>> Messages = KismetAIProviderCodec::EncodeMessages(CanonicalMessages);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	// Fast mode on MiniMax is a different model id (M2.7 -> M2.7-highspeed).
	const FKismetAIModelCapabilities& Caps = FKismetAIModelCatalog::Get().FindOrDefaults(Config.Model).Caps;
	const bool bSwapForFast = Config.bFastMode
		&& Caps.FastModeWire == EKismetAIFastModeWire::ModelSwap
		&& !Caps.FastModeModelId.IsEmpty();
	Body->SetStringField(TEXT("model"), bSwapForFast ? Caps.FastModeModelId : Config.Model);
	Body->SetNumberField(TEXT("max_tokens"), Config.MaxTokens);
	Body->SetBoolField(TEXT("stream"), true);

	bool bThinkingEnabled = false;
	bool bEffortEnabled = false;
	FKismetAIAgent::ApplyModelRequestShape(Body, Config, bThinkingEnabled, bEffortEnabled);
	OutShape.bThinkingEnabled = bThinkingEnabled;
	OutShape.bEffortEnabled = bEffortEnabled;

	// MiniMax Anthropic compatibility supports a plain system string. Avoid
	// Anthropic-only cache_control on M3, which uses passive prompt caching.
	Body->SetStringField(TEXT("system"), SystemPrompt);
	if (Tools.Num() > 0)
	{
		Body->SetArrayField(TEXT("tools"), Tools);  // omitted for a tool-less request
	}
	Body->SetArrayField(TEXT("messages"), Messages);

	return Body;
}

FString FMinimaxProvider::GetEndpointUrl(const FKismetAIAgentConfig& Config) const
{
	// MINIMAX_BASE_URL or a settings override, else https://api.minimax.io/anthropic.
	return KismetAIProviders::GetBaseUrl(Config, Profile) + TEXT("/v1/messages");
}

TArray<FString> FMinimaxProvider::GetExtraCurlHeaders(
	const FKismetAIAgentConfig& Config,
	const FLLMRequestShape& /*Shape*/) const
{
	return KismetAIProviders::BuildAuthHeaders(Config, Profile);
}

TSharedPtr<ILLMStreamState> FMinimaxProvider::CreateStreamState() const
{
	return MakeShared<FAnthropicStreamStateAdapter>();
}

void FMinimaxProvider::ParseErrorBody(
	const TSharedPtr<FJsonObject>& Body,
	FModelCallOutcome& Outcome) const
{
	if (!Body.IsValid()) return;

	Outcome.Response = Body;
	const TSharedPtr<FJsonObject>* ErrorObj;
	if (Body->TryGetObjectField(TEXT("error"), ErrorObj))
	{
		(*ErrorObj)->TryGetStringField(TEXT("type"), Outcome.ErrorType);
		(*ErrorObj)->TryGetStringField(TEXT("code"), Outcome.ErrorType);
		(*ErrorObj)->TryGetStringField(TEXT("message"), Outcome.ErrorMessage);
	}
}
