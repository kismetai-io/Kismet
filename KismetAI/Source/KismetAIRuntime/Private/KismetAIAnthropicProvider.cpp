// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIAnthropicProvider.h"
#include "KismetAIAgent.h"
#include "KismetAILog.h"
#include "KismetAIProviderProfile.h"
#include "KismetAIModelCatalog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// ─────────────────────────────────────────────────────────────────────────────
// FAnthropicStreamStateAdapter
// ─────────────────────────────────────────────────────────────────────────────

FAnthropicStreamStateAdapter::FAnthropicStreamStateAdapter()
{
	// Wire OnThinkingComplete from the ILLMStreamState base into the inner state.
	// Lambda captures 'this' — safe because the adapter outlives all ProcessEvent calls.
	Inner.OnThinkingComplete = [this](const FString& ThinkingText)
	{
		if (OnThinkingComplete)
		{
			OnThinkingComplete(ThinkingText);
		}
	};

	// Wire incremental thinking deltas through to the base (UI live progress).
	Inner.OnThinkingDelta = [this](const FString& ThinkingDelta)
	{
		if (OnThinkingDelta)
		{
			OnThinkingDelta(ThinkingDelta);
		}
	};
}

bool FAnthropicStreamStateAdapter::ProcessEvent(
	const FString& EventType,
	const TSharedPtr<FJsonObject>& Data,
	FString& OutDelta)
{
	if (EventType == TEXT("error") && Data.IsValid())
	{
		bStreamError = true;
		const TSharedPtr<FJsonObject>* Error;
		if (Data->TryGetObjectField(TEXT("error"), Error))
		{
			(*Error)->TryGetStringField(TEXT("type"), StreamErrorCode);
			(*Error)->TryGetStringField(TEXT("message"), StreamErrorMessage);
		}
		return false;
	}
	// Reconstruct FKismetAISSEEvent from the decomposed type + data
	// (the SSE loop in CallLLMOnce already parsed these from the raw stream).
	FKismetAISSEEvent Event;
	Event.EventType = EventType;
	Event.Data = Data;

	return Inner.ProcessEvent(Event, OutDelta);
}

TSharedPtr<FJsonObject> FAnthropicStreamStateAdapter::BuildResponseJson() const
{
	return Inner.BuildResponseJson();
}

bool FAnthropicStreamStateAdapter::HasStreamError(FString& OutMessage, FString& OutCode) const
{
	if (Inner.bMalformedToolInput)
	{
		OutCode = TEXT("malformed_tool_arguments");
		OutMessage = TEXT("The model returned incomplete or invalid tool arguments. No tools from this response were executed. Retry with a smaller task.");
		return true;
	}
	OutMessage = StreamErrorMessage;
	OutCode = StreamErrorCode;
	return bStreamError;
}

// ─────────────────────────────────────────────────────────────────────────────
// FAnthropicProvider
// ─────────────────────────────────────────────────────────────────────────────

FAnthropicProvider::FAnthropicProvider()
{
	FKismetAIAgentConfig Default;
	Default.ProviderId = TEXT("anthropic");
	Profile = KismetAIProviders::ResolveProfile(Default);
}

FAnthropicProvider::FAnthropicProvider(const FKismetAIProviderProfile& InProfile)
	: Profile(InProfile)
{
}

FString FAnthropicProvider::GetName() const
{
	// The profile id ("anthropic", "deepseek", "ollama", ...). Never "openai":
	// usage accounting treats that name as OpenAI's inclusive cached-token counts.
	return Profile.Id.IsEmpty() ? FString(TEXT("anthropic")) : Profile.Id;
}

int32 FAnthropicProvider::CacheTtlSeconds() const
{
	// 300s matches Anthropic's ephemeral cache_control default.
	// Becomes 3600 if cache_control.ttl:"1h" is ever set (AS2).
	// The 5-min TTL is refreshed on every cache READ, so a back-to-back agent
	// loop (turns seconds apart) keeps the transcript prefix warm.
	return 300;
}

// Place a single prompt-cache breakpoint on the LAST content block of the LAST
// message so the entire conversation prefix (system + tools + all prior turns)
// is cached and re-read at ~0.1x on the next turn, instead of re-billing the
// whole transcript every turn. Anthropic matches the longest cached prefix, so
// moving the breakpoint forward each turn caches the growing history for free.
//
// Idempotent + clean-slate: strips any prior message-level cache_control first,
// so repeated calls (retries) and the moving breakpoint never accumulate markers
// (Anthropic caps total breakpoints at 4; we use system + last tool + here = 3).
// Mutates the shared FJsonValue pointees through the const-ref array — the same
// in-place pattern the agent's tool-cache and history helpers already use.
static void ApplyMessageCacheBreakpoint(const TArray<TSharedPtr<FJsonValue>>& Messages)
{
	if (Messages.Num() == 0)
	{
		return;
	}

	// Clean slate: remove cache_control from every content block of every message.
	for (const TSharedPtr<FJsonValue>& MsgVal : Messages)
	{
		TSharedPtr<FJsonObject> MsgObj = MsgVal.IsValid() ? MsgVal->AsObject() : nullptr;
		if (!MsgObj.IsValid())
		{
			continue;
		}
		const TArray<TSharedPtr<FJsonValue>>* ContentArr = nullptr;
		if (MsgObj->TryGetArrayField(TEXT("content"), ContentArr) && ContentArr)
		{
			for (const TSharedPtr<FJsonValue>& BlockVal : *ContentArr)
			{
				if (TSharedPtr<FJsonObject> BlockObj = BlockVal.IsValid() ? BlockVal->AsObject() : nullptr)
				{
					BlockObj->RemoveField(TEXT("cache_control"));
				}
			}
		}
	}

	TSharedPtr<FJsonObject> LastMsg = Messages.Last().IsValid() ? Messages.Last()->AsObject() : nullptr;
	if (!LastMsg.IsValid())
	{
		return;
	}

	TSharedPtr<FJsonObject> CacheControl = MakeShared<FJsonObject>();
	CacheControl->SetStringField(TEXT("type"), TEXT("ephemeral"));

	const TArray<TSharedPtr<FJsonValue>>* LastContent = nullptr;
	if (LastMsg->TryGetArrayField(TEXT("content"), LastContent) && LastContent && LastContent->Num() > 0)
	{
		// Array content (tool_result / multi-block message): mark the last block.
		if (TSharedPtr<FJsonObject> LastBlock = LastContent->Last()->AsObject())
		{
			LastBlock->SetObjectField(TEXT("cache_control"), CacheControl);
		}
		return;
	}

	// Plain-string content (typically the first user message): convert to a single
	// text block so the breakpoint has a block to attach to. Anthropic-equivalent.
	FString StringContent;
	if (LastMsg->TryGetStringField(TEXT("content"), StringContent))
	{
		TSharedPtr<FJsonObject> TextBlock = MakeShared<FJsonObject>();
		TextBlock->SetStringField(TEXT("type"), TEXT("text"));
		TextBlock->SetStringField(TEXT("text"), StringContent);
		TextBlock->SetObjectField(TEXT("cache_control"), CacheControl);

		TArray<TSharedPtr<FJsonValue>> NewContent;
		NewContent.Add(MakeShared<FJsonValueObject>(TextBlock));
		LastMsg->SetArrayField(TEXT("content"), NewContent);
	}
}

TSharedPtr<FJsonObject> FAnthropicProvider::BuildRequestBody(
	const FKismetAIAgentConfig& Config,
	const TArray<FAgentMessage>& CanonicalMessages,
	const TArray<TSharedPtr<FJsonValue>>& Tools,
	const FString& SystemPrompt,
	FLLMRequestShape& OutShape) const
{
	const TArray<TSharedPtr<FJsonValue>> Messages = KismetAIProviderCodec::EncodeMessages(CanonicalMessages);
	// Body construction lifted verbatim from KismetAIAgent.cpp:578-604.
	// BEHAVIOR-PRESERVING: do not change this logic.
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("model"), KismetAIProviders::WireModelId(Config, Profile));
	Body->SetNumberField(TEXT("max_tokens"), Config.MaxTokens);
	Body->SetBoolField(TEXT("stream"), true);

	// Thinking, temperature, and effort — delegated to the testable helper.
	bool bThinkingEnabled = false;
	bool bEffortEnabled = false;
	FKismetAIAgent::ApplyModelRequestShape(Body, Config, bThinkingEnabled, bEffortEnabled);

	// Compatible hosts must not receive Claude-only fields for unknown models,
	// independently of their optional-capability fallback.
	if (Profile.Id != TEXT("anthropic") && !FKismetAIModelCatalog::Get().Find(Config.Model))
	{
		Body->RemoveField(TEXT("thinking"));
		Body->RemoveField(TEXT("output_config"));
		Body->RemoveField(TEXT("speed"));
		bThinkingEnabled = false;
		bEffortEnabled = false;
	}
	OutShape.bThinkingEnabled = bThinkingEnabled;
	OutShape.bEffortEnabled = bEffortEnabled;

	// System prompt. Anthropic gets a cache_control breakpoint; compatible
	// hosts that reject structured system blocks get a plain string.
	if (Profile.bSystemAsString)
	{
		Body->SetStringField(TEXT("system"), SystemPrompt);
	}
	else
	{
		TSharedPtr<FJsonObject> SystemBlock = MakeShared<FJsonObject>();
		SystemBlock->SetStringField(TEXT("type"), TEXT("text"));
		SystemBlock->SetStringField(TEXT("text"), SystemPrompt);
		if (Profile.bPromptCacheMarkers)
		{
			TSharedPtr<FJsonObject> SysCacheControl = MakeShared<FJsonObject>();
			SysCacheControl->SetStringField(TEXT("type"), TEXT("ephemeral"));
			SystemBlock->SetObjectField(TEXT("cache_control"), SysCacheControl);
		}
		TArray<TSharedPtr<FJsonValue>> SystemArray;
		SystemArray.Add(MakeShared<FJsonValueObject>(SystemBlock));
		Body->SetArrayField(TEXT("system"), SystemArray);
	}

	// Tools (dynamically composed: core + discovered extended tools). A request
	// with no tools (the compaction summary) omits the field entirely.
	if (Tools.Num() > 0)
	{
		Body->SetArrayField(TEXT("tools"), Tools);
	}

	// Messages — mark the last block so the whole transcript prefix is cached.
	if (Profile.bPromptCacheMarkers)
	{
		ApplyMessageCacheBreakpoint(Messages);
	}
	Body->SetArrayField(TEXT("messages"), Messages);

	return Body;
}

FString FAnthropicProvider::GetEndpointUrl(const FKismetAIAgentConfig& Config) const
{
	return FString::Printf(TEXT("%s/v1/messages"), *KismetAIProviders::GetBaseUrl(Config, Profile));
}

TArray<FString> FAnthropicProvider::GetExtraCurlHeaders(
	const FKismetAIAgentConfig& Config,
	const FLLMRequestShape& Shape) const
{
	// Returns header strings WITHOUT the -H "..." wrapper.
	// CallLLMOnce wraps each entry as: -H "<entry>"
	// This matches the pre-refactor CallAnthropicOnce inline header building (lines 648-657).
	TArray<FString> Headers;

	Headers.Append(KismetAIProviders::BuildAuthHeaders(Config, Profile));
	Headers.Add(TEXT("anthropic-version: 2023-06-01"));

	// Conditional anthropic-beta header (may be empty)
	FString BetaValues = FKismetAIAgent::BuildAnthropicBetaHeader(
		Config, Shape.bThinkingEnabled, Shape.bEffortEnabled);
	if (!BetaValues.IsEmpty())
	{
		Headers.Add(FString::Printf(TEXT("anthropic-beta: %s"), *BetaValues));
	}

	return Headers;
}

TSharedPtr<ILLMStreamState> FAnthropicProvider::CreateStreamState() const
{
	return MakeShared<FAnthropicStreamStateAdapter>();
}

void FAnthropicProvider::ParseErrorBody(
	const TSharedPtr<FJsonObject>& Body,
	FModelCallOutcome& Outcome) const
{
	// Lifted verbatim from KismetAIAgent.cpp:829-840.
	// Extracts error.type and error.message from the parsed JSON body.
	// The ExitCode-based fallback and MaxTokensOverflow regex remain in CallLLMOnce.
	if (!Body.IsValid()) return;

	Outcome.Response = Body;
	const TSharedPtr<FJsonObject>* ErrorObj;
	if (Body->TryGetObjectField(TEXT("error"), ErrorObj))
	{
		(*ErrorObj)->TryGetStringField(TEXT("type"), Outcome.ErrorType);
		(*ErrorObj)->TryGetStringField(TEXT("message"), Outcome.ErrorMessage);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Factory
// ─────────────────────────────────────────────────────────────────────────────

#include "KismetAIMinimaxProvider.h"
#include "KismetAIOpenAIProvider.h"
#include "KismetAIOpenAIChatProvider.h"

bool IsOpenAIModel(const FString& Model)
{
	const FKismetAIProviderProfile* Profile = FKismetAIProviderRegistry::Get().Find(KismetAIProviders::ProviderIdForModel(Model));
	return Profile && Profile->Protocol == EKismetAIWireProtocol::OpenAIResponses;
}

bool IsMinimaxModel(const FString& Model)
{
	return KismetAIProviders::ProviderIdForModel(Model) == TEXT("minimax");
}

TUniquePtr<ILLMProvider> MakeProviderForConfig(const FKismetAIAgentConfig& Config)
{
	const FKismetAIProviderProfile& Profile = KismetAIProviders::ResolveProfile(Config);
	switch (Profile.Protocol)
	{
	case EKismetAIWireProtocol::OpenAIResponses:
		return MakeUnique<FOpenAIProvider>(Profile);
	case EKismetAIWireProtocol::OpenAIChat:
		return MakeUnique<FOpenAIChatProvider>(Profile);
	case EKismetAIWireProtocol::AnthropicMessages:
		if (Profile.Id == TEXT("minimax"))
		{
			return MakeUnique<FMinimaxProvider>(Profile);
		}
		return MakeUnique<FAnthropicProvider>(Profile);
	}
	return nullptr;
}

TUniquePtr<ILLMProvider> MakeProviderForModel(const FString& Model)
{
	FKismetAIAgentConfig Config;
	Config.Model = Model;
	return MakeProviderForConfig(Config);
}
