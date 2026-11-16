// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAITurnContext.h"
#include "KismetAIToolRegistry.h"
#include "KismetAIRequestBudget.h"
#include "Math/UnrealMathUtility.h"

void FKismetAIContextPolicy::PrepareHistory(TArray<TSharedPtr<FJsonValue>>& Messages, bool bThinkingEnabled)
{
	// Order matters. Stripping trailing thinking blocks can leave an assistant
	// message that no longer pairs with the tool_result after it, so pairing is
	// repaired second — never the other way round.
	if (bThinkingEnabled)
	{
		FKismetAIAgent::FilterThinkingFromHistory(Messages);
	}
	FKismetAIAgent::ValidateToolPairing(Messages);
}

ECompactionAction FKismetAIContextPolicy::DecideCompaction(int32 EstimatedInputTokens,
	int32 EffectiveContextLimit, float OverflowThreshold)
{
	if (EffectiveContextLimit <= 0)
	{
		return ECompactionAction::None;
	}
	const int32 Threshold = FMath::FloorToInt(EffectiveContextLimit * OverflowThreshold);
	return (EstimatedInputTokens > Threshold) ? ECompactionAction::Microcompact : ECompactionAction::None;
}

bool FKismetAIContextPolicy::NeedsTrimAfterMicrocompact(int32 EstimatedInputTokens, int32 EffectiveContextLimit)
{
	if (EffectiveContextLimit <= 0)
	{
		return false;
	}
	// Microcompaction only clears old tool-result bodies. If the input is still
	// this close to the limit afterwards, the conversation itself is too long and
	// dropping message groups is the only remaining option.
	return EstimatedInputTokens > FMath::FloorToInt(EffectiveContextLimit * 0.95f);
}

int32 FKismetAIContextPolicy::EstimateMessageTokens(const TArray<TSharedPtr<FJsonValue>>& Messages)
{
	// One counting rule for messages, shared with the whole-request estimate.
	return FKismetAIRequestEstimate::Estimate(FString(), {}, Messages, FKismetAIUsageCalibration(),
		FKismetAIContextBudget()).MessageTokens;
}

int32 FKismetAIContextPolicy::DecideNextMaxTokens(const TArray<FString>& ToolNamesCalledThisTurn,
	bool bFirstTurnWithNoTools, bool bThinkingActive, const FKismetAIAgentConfig& Config)
{
	int32 Next = Config.MaxTokensDefault;

	bool bWantsHigh = false;
	bool bWantsLow = false;
	for (const FString& Name : ToolNamesCalledThisTurn)
	{
		switch (FKismetAIToolRegistry::GetBudgetHint(Name))
		{
		case ENextTurnBudget::High: bWantsHigh = true; break;
		case ENextTurnBudget::Low:  bWantsLow  = true; break;
		default: break;
		}
	}

	if (bFirstTurnWithNoTools)
	{
		// The model is planning, not producing.
		Next = Config.MaxTokensLow;
	}
	else if (bWantsLow)
	{
		Next = Config.MaxTokensLow;
	}
	else if (bWantsHigh)
	{
		Next = Config.MaxTokensHigh;
	}

	// Reasoning tokens are billed against the output cap, so the tool-intent
	// heuristic above (which can pick Low) would starve a thinking turn and
	// truncate it before it emits a tool_use.
	if (bThinkingActive)
	{
		Next = FMath::Max(Next, Config.MaxTokensThinkingFloor);
	}

	// Escape hatch for measurement and debugging.
	const FString& Override = Config.AdaptiveOutputTokensOverride;
	if (!Override.IsEmpty())
	{
		Next = FCString::Atoi(*Override);
	}
	return Next;
}

void FKismetAIContextPolicy::PrepareHistory(TArray<FAgentMessage>& Messages, bool bThinkingEnabled)
{
	auto Legacy = KismetAIAgentCodec::ToAnthropicArray(Messages);
	PrepareHistory(Legacy, bThinkingEnabled);
	Messages = KismetAIAgentCodec::FromAnthropicArray(Legacy);
}
