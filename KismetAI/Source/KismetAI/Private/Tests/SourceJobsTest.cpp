// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
#include "KismetAISourceJobs.h"
#include "KismetAISourceContract.h"
#include "Misc/AutomationTest.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/ThreadSafeCounter.h"

namespace KismetAISourceJobsTests
{
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	struct FWait
	{
		FEvent* Started = FPlatformProcess::GetSynchEventFromPool(true);
		FEvent* Released = FPlatformProcess::GetSynchEventFromPool(true);
		~FWait()
		{
			FPlatformProcess::ReturnSynchEventToPool(Started);
			FPlatformProcess::ReturnSynchEventToPool(Released);
		}
	};
	FKismetAISourceJobs::FWork UntilCancelled(const TSharedRef<FWait, ESPMode::ThreadSafe>& Wait)
	{
		return [Wait](const FKismetAICancellationToken& Token)
		{
			const uint64 Subscription = Token.Subscribe([Wait] { Wait->Released->Trigger(); });
			Wait->Started->Trigger();
			Wait->Released->Wait();
			Token.Unsubscribe(Subscription);
			FKismetAISourceJobs::FResult Result;
			Result.Text = Token.IsCancelled() && !IsInGameThread() ? TEXT("cancelled_on_worker") : TEXT("incorrect_thread_or_token");
			return Result;
		};
	}
	bool DrainByPumping(FKismetAISourceJobs& Jobs)
	{
		const double End = FPlatformTime::Seconds() + 5.0;
		while (Jobs.NumActive() && FPlatformTime::Seconds() < End)
		{
			Jobs.PumpCompletions();
			FPlatformProcess::SleepNoStats(0.001f);
		}
		return Jobs.NumActive() == 0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAISourceJobsCapacityTest, "KismetAI.SourceJobs.CapacityAndCancellation", KismetAISourceJobsTests::Flags)
bool FKismetAISourceJobsCapacityTest::RunTest(const FString& Parameters)
{
	using namespace KismetAISourceJobsTests;
	FKismetAISourceJobs Jobs;
	int32 Completions = 0;
	for (int32 Index = 0; Index < KismetAISourceContract::MaxJobs; ++Index)
	{
		const auto Wait = MakeShared<FWait, ESPMode::ThreadSafe>();
		const auto Result = Jobs.Submit(FString::FromInt(Index), UntilCancelled(Wait),
			[this, &Completions](FKismetAISourceJobs::FResult&& Out)
			{
				TestTrue(TEXT("completion is on GT"), IsInGameThread());
				TestEqual(TEXT("work cancelled on worker"), Out.Text, FString(TEXT("cancelled_on_worker")));
				++Completions;
			});
		TestTrue(TEXT("admitted within cap"), Result == FKismetAISourceJobs::ESubmit::Started);
	}
	TestTrue(TEXT("duplicate key rejected"), Jobs.Submit(TEXT("0"), {}, {}) == FKismetAISourceJobs::ESubmit::Duplicate);
	TestTrue(TEXT("ninth request rejected before worker allocation"), Jobs.Submit(TEXT("overflow"), {}, {}) == FKismetAISourceJobs::ESubmit::Full);
	TestEqual(TEXT("exact active cap"), Jobs.NumActive(), KismetAISourceContract::MaxJobs);
	for (int32 Index = 0; Index < KismetAISourceContract::MaxJobs; ++Index) TestTrue(TEXT("cancel accepted"), Jobs.Cancel(FString::FromInt(Index)));
	TestTrue(TEXT("cancelled workers drain"), DrainByPumping(Jobs));
	TestEqual(TEXT("one completion per accepted job"), Completions, KismetAISourceContract::MaxJobs);
	TestFalse(TEXT("finished id cannot be cancelled again"), Jobs.Cancel(TEXT("0")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAISourceJobsCloseTest, "KismetAI.SourceJobs.CloseDrainsAndRevokes", KismetAISourceJobsTests::Flags)
bool FKismetAISourceJobsCloseTest::RunTest(const FString& Parameters)
{
	using namespace KismetAISourceJobsTests;
	FKismetAISourceJobs Jobs;
	const auto Wait = MakeShared<FWait, ESPMode::ThreadSafe>();
	int32 Completions = 0;
	Jobs.Submit(TEXT("close"), UntilCancelled(Wait), [&Completions](FKismetAISourceJobs::FResult&&) { ++Completions; });
	TestTrue(TEXT("worker actually started"), Wait->Started->Wait(5000));
	Jobs.Close();
	Jobs.PumpCompletions();
	TestEqual(TEXT("no owned jobs after close"), Jobs.NumActive(), 0);
	TestEqual(TEXT("late transport callback revoked"), Completions, 0);
	TestTrue(TEXT("closed owner rejects new work"), Jobs.Submit(TEXT("new"), {}, {}) == FKismetAISourceJobs::ESubmit::Closed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAISourceJobsDeadlineTest, "KismetAI.SourceJobs.CooperativeDeadline", KismetAISourceJobsTests::Flags)
bool FKismetAISourceJobsDeadlineTest::RunTest(const FString& Parameters)
{
	using namespace KismetAISourceJobsTests;
	FKismetAISourceJobs Jobs;
	double Now = 0;
	Jobs.SetClockForTests([&Now] { return Now; });
	const auto Wait = MakeShared<FWait, ESPMode::ThreadSafe>();
	int32 Completions = 0;
	Jobs.Submit(TEXT("deadline"), UntilCancelled(Wait), [this, &Completions](FKismetAISourceJobs::FResult&& Out)
	{
		TestEqual(TEXT("deadline reaches worker token"), Out.Text, FString(TEXT("cancelled_on_worker")));
		++Completions;
	});
	TestTrue(TEXT("worker actually started"), Wait->Started->Wait(5000));
	Now = KismetAISourceContract::JobSeconds - 0.001;
	Jobs.PumpCompletions();
	TestFalse(TEXT("before deadline token is live"), Wait->Released->Wait(0));
	Now = KismetAISourceContract::JobSeconds;
	Jobs.PumpCompletions();
	TestTrue(TEXT("deadline drains cooperative worker"), DrainByPumping(Jobs));
	TestEqual(TEXT("deadline gives one completion"), Completions, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAISourceJobsReentrantCloseTest, "KismetAI.SourceJobs.ReentrantClose", KismetAISourceJobsTests::Flags)
bool FKismetAISourceJobsReentrantCloseTest::RunTest(const FString& Parameters)
{
	FKismetAISourceJobs Jobs;
	int32 Completions = 0;
	for (int32 Index = 0; Index < 2; ++Index)
	{
		Jobs.Submit(FString::FromInt(Index), [](const FKismetAICancellationToken&) { return FKismetAISourceJobs::FResult(); },
			[&Jobs, &Completions](FKismetAISourceJobs::FResult&&) { ++Completions; Jobs.Close(); });
	}
	TestTrue(TEXT("reentrant close drains"), KismetAISourceJobsTests::DrainByPumping(Jobs));
	TestEqual(TEXT("close revokes detached second callback"), Completions, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAISourceJobsLateCancelTest, "KismetAI.SourceJobs.CancelCompletedBeforeDelivery", KismetAISourceJobsTests::Flags)
bool FKismetAISourceJobsLateCancelTest::RunTest(const FString& Parameters)
{
	FKismetAISourceJobs Jobs;
	bool bDelivered = false;
	Jobs.Submit(TEXT("late"), [](const FKismetAICancellationToken&)
	{
		FKismetAISourceJobs::FResult Result;
		Result.Text = TEXT("completed evidence");
		return Result;
	}, [this, &bDelivered](FKismetAISourceJobs::FResult&& Result)
	{
		TestTrue(TEXT("cancel observed after work but before delivery"), Result.bCancelled);
		TestEqual(TEXT("completed evidence is retained for adapter"), Result.Text, FString(TEXT("completed evidence")));
		bDelivered = true;
	});
	Jobs.WaitWorkersForTests();
	TestTrue(TEXT("ready result still cancellable before delivery"), Jobs.Cancel(TEXT("late")));
	Jobs.PumpCompletions();
	TestTrue(TEXT("exact terminal delivery"), bDelivered);
	return true;
}
