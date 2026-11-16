// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Admission captures run preferences and caller identity. Each model step
// derives a const value with its composed schemas, budget and journal identity.
// Cancellation and the authority store remain live; no UObject is captured.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "KismetAIAgent.h"
#include "KismetAICancellation.h"

struct KISMETAIRUNTIME_API FKismetAITurnContext
{
	// Correlates this turn's events, telemetry and logs.
	double AdmittedAtSeconds = 0;
	FString TurnId;
	FString RunId;
	FString SessionId;
	FKismetAICallerContext Caller;
	bool bInteractiveAllowed = false;

	// Zero-based. TurnsRemaining is measured from the NEXT turn, because the
	// budget note built from it is read by the model on its following turn.
	int32 TurnIndex = 0;
	int32 TurnsRemaining = 0;

	// Value-owned preferences and the output budget selected for this step.
	FKismetAIAgentConfig Config;

	FString SystemPrompt;
	TArray<TSharedPtr<FJsonValue>> VisibleTools;  // Deep-copied when the model step is composed

	// True when reasoning tokens are billed against the output cap, which is why
	// the budget has a floor. Recorded so later turns do not have to re-derive it
	// from the model id.
	bool bThinkingActive = false;

	bool bPlanModeActive = false;

	// Cancels this turn. A child of the run token.
	FKismetAICancellationToken Token;
};

// What to do about context pressure before the next call.
enum class ECompactionAction : uint8
{
	None,
	// Clear old tool-result bodies, keep the conversation shape.
	Microcompact,
	// Drop whole message groups, preserving tool_use/tool_result pairing.
	Trim,
};

// History projection and context policy.
//
// Pure with respect to the conversation: it decides, the runtime acts. That
// split is what lets the compaction thresholds be tested without a model, a
// network or a 200k-token transcript.
class KISMETAIRUNTIME_API FKismetAIContextPolicy
{
public:
	// Prepare history for sending. Applies the provider invariants in the order
	// they depend on each other: thinking blocks are stripped first (an assistant
	// message may not END with one), then pairing is repaired, because stripping
	// can leave a message that no longer satisfies pairing. Neither step removes
	// a genuine user message (KismetAIHistoryRepair.h).
	static void PrepareHistory(TArray<TSharedPtr<FJsonValue>>& Messages, bool bThinkingEnabled);
	static void PrepareHistory(TArray<FAgentMessage>& Messages, bool bThinkingEnabled);

	// What to do given the estimated input of the request about to be sent
	// (FKismetAIRequestEstimate::Tokens, KismetAIRequestBudget.h) — never the
	// count reported for the previous request on its own: that request no longer
	// exists once results are appended or cleared (review R3).
	//
	// Two phases rather than one: microcompaction is cheap and often enough, and
	// a full trim loses conversation the model may still need. Trim only when the
	// estimate RECOMPUTED after microcompaction is still too high.
	static ECompactionAction DecideCompaction(int32 EstimatedInputTokens, int32 EffectiveContextLimit,
		float OverflowThreshold);

	// Whether a full trim is still needed, given the estimate recomputed after a
	// microcompaction pass.
	static bool NeedsTrimAfterMicrocompact(int32 EstimatedInputTokens, int32 EffectiveContextLimit);

	// Rough token estimate for a message array (about 4 characters per token,
	// attachments at a flat cost). The message part of
	// FKismetAIRequestEstimate, which adds the system prompt, tool schemas and
	// provider calibration; use that for any budget decision.
	static int32 EstimateMessageTokens(const TArray<TSharedPtr<FJsonValue>>& Messages);

	// The output budget for the next turn, given what was called on this one.
	// Low wins over High when a turn mixed both: a turn that wrote T3D is
	// followed by verification even if it also read.
	static int32 DecideNextMaxTokens(const TArray<FString>& ToolNamesCalledThisTurn,
		bool bFirstTurnWithNoTools, bool bThinkingActive, const FKismetAIAgentConfig& Config);
};
