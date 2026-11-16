// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIGameThreadDispatcher.h"
#include "KismetAIEngineCompat.h"

// FAppTime and the inherited frame-time context this test observes were added
// in engine 5.8. Earlier engines have no such context to preserve.
#if KISMETAI_ENGINE_AT_LEAST_5_8

#include "Async/Async.h"
#include "Async/InheritedContext.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/PlatformTime.h"
#include "Misc/AppTime.h"
#include "Misc/AssertionMacros.h"
#include "Misc/AutomationTest.h"

namespace KismetAIDispatcherTimeTest
{
	struct FState
	{
		TSharedRef<FKismetAIGameThreadDispatcher, ESPMode::ThreadSafe> Dispatcher = MakeShared<FKismetAIGameThreadDispatcher, ESPMode::ThreadSafe>();
		TFuture<void> Worker;
		FGraphEventRef Child;
		SIZE_T EnsuresBefore = 0;
		double Deadline = 0;
		double ParentTime = 0;
		double ChildTime = -1;
		bool bOnGameThread = false;
	};

	class FVerify final : public IAutomationLatentCommand
	{
	public:
		FVerify(FAutomationTestBase* InTest, TSharedRef<FState, ESPMode::ThreadSafe> InState)
			: Test(InTest), State(InState) {}
		virtual bool Update() override
		{
			if (!State->Worker.IsReady())
			{
				if (FPlatformTime::Seconds() < State->Deadline) return false;
				State->Dispatcher->Close();
				Test->AddError(TEXT("Worker-to-game-thread dispatch did not complete before deadline"));
				return true;
			}
			if (State->Child.IsValid() && !State->Child->IsComplete())
			{
				if (FPlatformTime::Seconds() < State->Deadline) return false;
				Test->AddError(TEXT("Descendant time-context task did not complete"));
				return true;
			}
			Test->TestTrue(TEXT("Editor work ran on the game thread"), State->bOnGameThread);
			Test->TestTrue(TEXT("Descendant task was launched"), State->Child.IsValid());
			Test->TestEqual(TEXT("Descendant inherited its editor callback frame time"), State->ChildTime, State->ParentTime);
			Test->TestEqual(TEXT("No missing inherited time-context ensure"), FDebug::GetNumEnsureFailures(), State->EnsuresBefore);
			State->Dispatcher->Close();
			return true;
		}
	private:
		FAutomationTestBase* Test;
		TSharedRef<FState, ESPMode::ThreadSafe> State;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDispatcherTimeContextTest, "KismetAI.Dispatcher.WorkerTimeContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDispatcherTimeContextTest::RunTest(const FString& Parameters)
{
	using namespace KismetAIDispatcherTimeTest;
	auto State = MakeShared<FState, ESPMode::ThreadSafe>();
	State->EnsuresBefore = FDebug::GetNumEnsureFailures();
	State->Deadline = FPlatformTime::Seconds() + 15.0;
	State->Worker = Async(EAsyncExecution::Thread, [State]()
	{
		// Model/provider workers need not originate from a game-frame task.
		UE::FInheritedContextBase EmptyContext;
		auto EmptyScope = EmptyContext.RestoreInheritedContext();
		State->Dispatcher->Dispatch([State]()
		{
			State->bOnGameThread = IsInGameThread();
			State->ParentTime = FAppTime::GetCurrentTime();
			State->Child = FFunctionGraphTask::CreateAndDispatchWhenReady([State]()
			{
				State->ChildTime = FAppTime::GetCurrentTime();
			}, TStatId(), nullptr, ENamedThreads::AnyBackgroundThreadNormalTask);
			return FKismetAIDispatchOutcome();
		}, FKismetAICancellationToken(), FToolExecutionResult::Error(503, TEXT("CLOSED"), TEXT("Test dispatcher closed")));
	});
	ADD_LATENT_AUTOMATION_COMMAND(FVerify(this, State));
	return true;
}

#endif // KISMETAI_ENGINE_AT_LEAST_5_8
