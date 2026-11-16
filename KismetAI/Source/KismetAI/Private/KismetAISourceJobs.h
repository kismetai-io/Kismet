// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CoreMinimal.h"
#include "KismetAICancellation.h"
#include "KismetAIToolResult.h"
#include "Async/Future.h"
#include "Containers/Ticker.h"

// Game-thread owner of bounded external source/protocol requests. Workers never
// receive the transport completion callback; only PumpCompletions may invoke it.
class FKismetAISourceJobs
{
public:
	struct FResult
	{
		FToolExecutionResult Tool;
		FString Text;
		bool bNoContent = false;
		bool bCancelled = false;
	};
	using FWork = TFunction<FResult(const FKismetAICancellationToken&)>;
	using FCompletion = TFunction<void(FResult&&)>;
	using FClock = TFunction<double()>;
	enum class ESubmit : uint8 { Started, Full, Duplicate, Closed };

	FKismetAISourceJobs();
	~FKismetAISourceJobs();
	ESubmit Submit(const FString& Key, FWork Work, FCompletion Completion);
	bool Cancel(const FString& Key);
	void PumpCompletions();
	// Cancels and drains workers, discarding every transport callback. A worker
	// awaiting GT admission must be cancellation-aware; no GT pump is performed.
	void Close();
	int32 NumActive() const;
	void SetClockForTests(FClock InClock);
	void WaitWorkersForTests();
	int32 NumCancelledForTests() const;

private:
	struct FEntry
	{
		FKismetAICancellationToken Token;
		TFuture<FResult> Future;
		FCompletion Completion;
		double StartedAt = 0;
	};
	struct FDeliveryState { bool bClosed = false; };
	TSharedRef<FDeliveryState> DeliveryState;
	TMap<FString, TUniquePtr<FEntry>> Entries;
	FTSTicker::FDelegateHandle Ticker;
	FClock Clock;
};
