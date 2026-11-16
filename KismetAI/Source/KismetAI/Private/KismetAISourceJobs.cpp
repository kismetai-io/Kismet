// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
#include "KismetAISourceJobs.h"
#include "KismetAISourceContract.h"
#include "KismetAISourceSearch.h"
#include "Async/Async.h"
#include "HAL/PlatformTime.h"

FKismetAISourceJobs::FKismetAISourceJobs()
	: DeliveryState(MakeShared<FDeliveryState>()), Clock([] { return FPlatformTime::Seconds(); })
{
	check(IsInGameThread());
	Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this](float)
	{
		PumpCompletions();
		return true;
	}));
}

FKismetAISourceJobs::~FKismetAISourceJobs()
{
	Close();
}

FKismetAISourceJobs::ESubmit FKismetAISourceJobs::Submit(const FString& Key, FWork Work, FCompletion Completion)
{
	check(IsInGameThread());
	if (DeliveryState->bClosed) return ESubmit::Closed;
	if (Entries.Contains(Key)) return ESubmit::Duplicate;
	if (Entries.Num() >= KismetAISourceContract::MaxJobs) return ESubmit::Full;
	TUniquePtr<FEntry> Entry = MakeUnique<FEntry>();
	Entry->Completion = MoveTemp(Completion);
	Entry->StartedAt = Clock();
	const FKismetAICancellationToken Token = Entry->Token;
	Entry->Future = Async(EAsyncExecution::ThreadPool, [Token, Work = MoveTemp(Work)]() mutable
	{
		return Work(Token);
	});
	Entries.Add(Key, MoveTemp(Entry));
	return ESubmit::Started;
}

bool FKismetAISourceJobs::Cancel(const FString& Key)
{
	check(IsInGameThread());
	TUniquePtr<FEntry>* Entry = Entries.Find(Key);
	if (!Entry) return false;
	(*Entry)->Token.Cancel();
	return true;
}

void FKismetAISourceJobs::PumpCompletions()
{
	check(IsInGameThread());
	KismetAISourceSearch::ReapExpired();
	// Detach ready entries before invoking a callback: it may stop/destroy the
	// service. The retained state revokes remaining deliveries in that case.
	const TSharedRef<FDeliveryState> State = DeliveryState;
	TArray<TUniquePtr<FEntry>> Ready;
	const double Now = Clock();
	for (auto It = Entries.CreateIterator(); It; ++It)
	{
		FEntry& Entry = *It.Value();
		if (Now - Entry.StartedAt >= KismetAISourceContract::JobSeconds) Entry.Token.Cancel();
		if (Entry.Future.IsReady())
		{
			Ready.Add(MoveTemp(It.Value()));
			It.RemoveCurrent();
		}
	}
	for (TUniquePtr<FEntry>& Entry : Ready)
	{
		if (State->bClosed) break;
		FResult Result = Entry->Future.Get();
		Result.bCancelled = Entry->Token.IsCancelled();
		if (Entry->Completion) Entry->Completion(MoveTemp(Result));
	}
}

void FKismetAISourceJobs::Close()
{
	check(IsInGameThread());
	if (DeliveryState->bClosed) return;
	DeliveryState->bClosed = true;
	FTSTicker::GetCoreTicker().RemoveTicker(Ticker);
	for (auto& Pair : Entries) Pair.Value->Token.Cancel();
	for (auto& Pair : Entries) Pair.Value->Future.Wait();
	Entries.Empty();
}

int32 FKismetAISourceJobs::NumActive() const
{
	check(IsInGameThread());
	return Entries.Num();
}

void FKismetAISourceJobs::SetClockForTests(FClock InClock)
{
	check(IsInGameThread());
	check(Entries.IsEmpty());
	Clock = MoveTemp(InClock);
}

void FKismetAISourceJobs::WaitWorkersForTests()
{
	check(IsInGameThread());
	for (auto& Pair : Entries) Pair.Value->Future.Wait();
}

int32 FKismetAISourceJobs::NumCancelledForTests() const
{
	check(IsInGameThread());
	int32 Count = 0;
	for (const auto& Pair : Entries) Count += Pair.Value->Token.IsCancelled() ? 1 : 0;
	return Count;
}
