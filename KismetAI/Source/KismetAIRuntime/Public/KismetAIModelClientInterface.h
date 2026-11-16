// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// The agent's model-call seam, public now so the editor
// module's lifecycle tests can drive a real FKismetAIAgent through a model
// call that blocks until cancelled. The production implementation
// (FKismetAICurlModelClient) stays private to the runtime.

#pragma once

#include "CoreMinimal.h"
#include "KismetAIAgent.h"

// Detached request values: retries cannot change session preferences or observe
// later edits to schemas, history, endpoint overrides or environment settings.
struct KISMETAIRUNTIME_API FModelCallRequest
{
	FModelCallRequest(const TArray<FAgentMessage>& InMessages,
		const TArray<TSharedPtr<FJsonValue>>& InTools, const FString& InSystemPrompt,
		const FKismetAIAgentConfig& InConfig);

	// Also used at run admission, before asynchronous work starts.
	static FKismetAIAgentConfig SnapshotConfig(const FKismetAIAgentConfig& InConfig);
	static bool ValidatePolicy(const FKismetAIAgentConfig& InConfig, FString& OutError);
	bool Validate(FString& OutError) const;

	const TArray<FAgentMessage> Messages;
	const TArray<TSharedPtr<FJsonValue>> Tools;
	const FString SystemPrompt;
	const FKismetAIAgentConfig Config;
};

// Incremental callbacks fired while the response is decoded.
//
// Text and thinking are PROVISIONAL: an attempt can still fail after they were
// delivered (a disconnect, an in-stream provider error, a cut tool argument),
// and CallWithRetry may then run another attempt that delivers different
// content through the same callbacks. The attempt hooks below mark those
// boundaries so a consumer never presents two attempts as one answer.
struct FModelCallbacks
{
	// Before each actual attempt, after backoff. A nonempty reason refuses it.
	TFunction<FString()> AdmitAttempt;
	// Read-only check before scheduling a retry/backoff.
	TFunction<FString()> CheckAttemptAllowed;
	// Raw single-attempt evidence, before retry aggregation. Never calls tools.
	TFunction<void(const FModelCallOutcome&)> OnAttemptCompleted;
	FOnTextDelta OnTextDelta;
	FOnThinkingComplete OnThinkingComplete;
	FOnThinkingDelta OnThinkingDelta;

	// CallWithRetry only. Fires before each transport attempt (0-based).
	TFunction<void(int32 AttemptIndex)> OnAttemptStart;
	// CallWithRetry only. Fires as soon as an attempt has failed AND the loop
	// has decided to make another one (before any backoff). The attempt that
	// ends the call — the success, or the failure that is returned — gets no
	// hook: the caller closes it once it has judged the returned outcome.
	TFunction<void(int32 AttemptIndex, const FModelCallOutcome& Failed)> OnAttemptRetrying;
};

// The whole model-call boundary: provider encoding, transport, stream decoding,
// usage, provider errors, retry classification and cancellation.
//
// Extracted in so the agent loop can be driven from an automation test
// with no network and no API key. Before this, RunTask reached curl through
// private methods, which is why a 1,288-line loop carrying six loop-health
// rules, adaptive budgeting, truncation continuation and two-phase compaction
// had exactly one test.
class IKismetAIModelClient
{
public:
	virtual ~IKismetAIModelClient() = default;

	// One call, no retry. Used by summarization, which falls back to a text
	// marker rather than retrying.
	virtual FModelCallOutcome Call(const FModelCallRequest& Request, const FModelCallbacks& Callbacks) = 0;

	// One call with exponential backoff. Used by the turn loop.
	virtual FModelCallOutcome CallWithRetry(const FModelCallRequest& Request, const FModelCallbacks& Callbacks) = 0;

	// Cancel an in-flight call. Must return promptly and be safe to call from
	// another thread; the call itself returns a failed outcome.
	virtual void Cancel() = 0;

	virtual FString GetProviderName() const = 0;
	virtual int32 CacheTtlSeconds() const = 0;
};
