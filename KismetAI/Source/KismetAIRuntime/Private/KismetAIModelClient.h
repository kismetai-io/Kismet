// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformProcess.h"
#include "HAL/ThreadSafeBool.h"
#include "KismetAIAgent.h"
#include "KismetAIModelClientInterface.h"
#include "KismetAISSEParser.h"

class ILLMProvider;
class ILLMStreamState;

// Incremental decode of one attempt's response bytes: SSE framing, the
// provider's stream state and the text/thinking callbacks. Bytes may arrive in
// any chunking (curl's file grows by whatever the socket delivered).
class FKismetAIStreamDecoder
{
public:
	FKismetAIStreamDecoder(ILLMStreamState& InState, const FModelCallbacks& InCallbacks);

	// Feed newly arrived bytes; complete events are dispatched immediately.
	void Feed(const uint8* Bytes, int64 Length);

	// End of the byte stream: dispatch a final event that has no trailing
	// newline. Never dispatches a partial line as anything but what it parses to.
	void Finish();

private:
	void DispatchPending();

	ILLMStreamState& State;
	const FModelCallbacks& Callbacks;
	FKismetAISSEParser Parser;
	bool bLastByteWasNewline = true;
	bool bAnyBytes = false;
};

// Production client: curl subprocess + the provider's SSE stream state.
class FKismetAICurlModelClient final : public IKismetAIModelClient
{
public:
	explicit FKismetAICurlModelClient(const FString& ModelId);
	explicit FKismetAICurlModelClient(const FKismetAIAgentConfig& Config);
	virtual ~FKismetAICurlModelClient() override;

	virtual FModelCallOutcome Call(const FModelCallRequest& Request, const FModelCallbacks& Callbacks) override;
	virtual FModelCallOutcome CallWithRetry(const FModelCallRequest& Request, const FModelCallbacks& Callbacks) override;
	virtual void Cancel() override;
	virtual FString GetProviderName() const override;
	virtual int32 CacheTtlSeconds() const override;

	// --- curl invocation (pure, exposed for testability) ---
	// Credentials must never reach the curl command line: argv is world-readable
	// through `ps` for the lifetime of the call.
	static FString BuildCurlConfigFile(const TArray<FString>& Headers);
	static FString BuildCurlArgs(const FString& Url, const FString& ConfigPath,
		const FString& BodyPath, const FString& OutputPath);
	static FString MakeCallScratchDir();

	// --- post-transport classification and retry (pure, exposed for tests) ---
	//
	// Everything Call does after the transport finished: turn the provider's
	// decoded stream state, the raw body, curl's exit code and the HTTP status
	// into an outcome. Partial tool calls never become a Success here.
	static FModelCallOutcome ClassifyAttempt(const ILLMProvider& Provider, const ILLMStreamState& State,
		const FString& RawOutput, int32 ExitCode, int32 ResponseCode, bool bExecOk, bool bCancelled);

	// The retry policy CallWithRetry runs, with the single attempt and the
	// cancellation flag supplied by the caller. CallWithRetry is exactly
	// RunRetryLoop(Request, Callbacks, Call, bCancelled).
	static FModelCallOutcome RunRetryLoop(const FModelCallRequest& Request, const FModelCallbacks& Callbacks,
		TFunctionRef<FModelCallOutcome(const FModelCallRequest&)> CallOnce, TFunctionRef<bool()> IsCancelled);

private:
	TUniquePtr<ILLMProvider> Provider;
	FString ProviderProfileId;  // profile Provider was built for

	// Set by Cancel(); observed by the curl wait loop and the retry backoff.
	FThreadSafeBool bCancelled = false;

	// In-flight curl subprocess, terminated by Cancel().
	FCriticalSection CurlProcMutex;
	FProcHandle ActiveCurlProc;
};
