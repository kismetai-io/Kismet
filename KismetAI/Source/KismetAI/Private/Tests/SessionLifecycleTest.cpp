// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// One owner for each run's lifecycle, and teardown that drains every
// worker wherever it is blocked.
//
// Every interleaving here is forced by a latch: a model client that parks
// inside the call until cancelled, the ask_user "question" event, and a held
// game-thread scheduler on a dispatcher of the test's own. Teardown is
// FKismetAISessionController::Shutdown — the exact sequence ShutdownModule
// runs — called on the game thread, which does not pump while it waits, as in
// a real shutdown. Time only bounds a failure; nothing passes by waiting.

#include "Misc/AutomationTest.h"
#include "KismetAISessionController.h"
#include "KismetAIModule.h"
#include "KismetAIModuleToolInvoker.h"
#include "KismetAIGameThreadDispatcher.h"
#include "KismetAIAgent.h"
#include "KismetAIModelClientInterface.h"
#include "KismetAIToolInvoker.h"
#include "KismetAIToolPolicy.h"
#include "Async/TaskGraphInterfaces.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include <atomic>

namespace KismetAISessionLifecycleTest
{
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	// Upper bound for something that must happen promptly.
	constexpr uint32 PromptMs = 5000;

	// A pooled event that returns itself.
	struct FLatch
	{
		FEvent* Event;
		explicit FLatch(bool bManualReset = true) : Event(FPlatformProcess::GetSynchEventFromPool(bManualReset)) {}
		~FLatch() { FPlatformProcess::ReturnSynchEventToPool(Event); }
		void Trigger() { Event->Trigger(); }
		bool Wait(uint32 Ms) { return Event->Wait(Ms); }
	};

	FModelCallOutcome TextTurn(const FString& Text)
	{
		TSharedPtr<FJsonObject> Usage = MakeShared<FJsonObject>();
		Usage->SetNumberField(TEXT("input_tokens"), 100);
		Usage->SetNumberField(TEXT("output_tokens"), 10);
		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetStringField(TEXT("stop_reason"), TEXT("end_turn"));
		Resp->SetObjectField(TEXT("usage"), Usage);
		FModelCallOutcome Out;
		Out.Result = EModelCallResult::Success;
		Out.Response = Resp;
		Out.AssistantMessage.Role = EAgentRole::Assistant;
		Out.AssistantMessage.Parts = { FAgentContentPart::MakeText(Text) };
		Out.HttpStatusCode = 200;
		Out.RequestCount = 1;
		return Out;
	}

	FModelCallOutcome ToolTurn(const FString& Id, const FString& Name, const TSharedPtr<FJsonObject>& Input)
	{
		FModelCallOutcome Out = TextTurn(TEXT(""));
		Out.AssistantMessage.Parts = { FAgentContentPart::MakeToolUse(Id, Name, Input) };
		Out.Response->SetStringField(TEXT("stop_reason"), TEXT("tool_use"));
		return Out;
	}

	// Scripted model client. Call number BlockOnCall parks inside the call
	// (Entered fires) until Cancel() — or, with bIgnoreCancel, until the test
	// calls Unblock(): a worker stuck somewhere cancellation cannot reach.
	class FLatchedModelClient final : public IKismetAIModelClient
	{
	public:
		TArray<FModelCallOutcome> Script;
		int32 BlockOnCall = -1;
		bool bIgnoreCancel = false;
		FLatch Entered;
		FLatch Wake;
		std::atomic<int32> Calls{ 0 };
		std::atomic<bool> bCancelled{ false };

		virtual FModelCallOutcome Call(const FModelCallRequest& Request, const FModelCallbacks& Callbacks) override { return Respond(); }
		virtual FModelCallOutcome CallWithRetry(const FModelCallRequest& Request, const FModelCallbacks& Callbacks) override { return Respond(); }
		virtual void Cancel() override
		{
			bCancelled = true;
			if (!bIgnoreCancel) { Wake.Trigger(); }
		}
		virtual FString GetProviderName() const override { return TEXT("anthropic"); }
		virtual int32 CacheTtlSeconds() const override { return 300; }
		void Unblock() { Wake.Trigger(); }

	private:
		FModelCallOutcome Respond()
		{
			const int32 Index = Calls++;
			if (Index == BlockOnCall)
			{
				Entered.Trigger();
				Wake.Wait(MAX_uint32);
				if (bCancelled)
				{
					FModelCallOutcome Aborted;
					Aborted.Result = EModelCallResult::FatalError;
					Aborted.ErrorMessage = TEXT("Aborted");
					return Aborted;
				}
			}
			if (Script.Num() == 0) { return TextTurn(TEXT("done")); }
			return Script[FMath::Min(Index, Script.Num() - 1)];
		}
	};

	// Tools that never reach the editor.
	class FNullInvoker final : public IKismetAIToolInvoker
	{
	public:
		std::atomic<int32> Invocations{ 0 };
		virtual FKismetAIInvokeResult Invoke(const FKismetAIToolInvocation& Invocation) override
		{
			++Invocations;
			return FKismetAIInvokeResult::FromModelString(TEXT("{\"success\":true}"));
		}
		virtual TSet<FString> GetDispatchableToolNames() const override { return {}; }
		virtual FString ResolveHandlerName(const FString& ToolName, const FString& Mode) const override { return FString(); }
	};

	FKismetAIAgentConfig MakeConfig()
	{
		FKismetAIAgentConfig C;
		C.ApiKey = TEXT("test-key-unused");
		C.Model = TEXT("claude-sonnet-4-6");
		C.MaxSteps = 4;
		C.MaxRetries = 0;
		C.RetryJitterFactor = 0.0;
		C.ThinkingMode = EThinkingMode::Disabled;
		return C;
	}

	using FAgentRef = TSharedRef<FKismetAIAgent, ESPMode::ThreadSafe>;

	// Seams owned together with the agent, so whichever of the test or the
	// worker lets go last frees them; a timed-out worker never borrows from the
	// test's stack.
	struct FRunFixture
	{
		TSharedRef<FLatchedModelClient, ESPMode::ThreadSafe> Model = MakeShared<FLatchedModelClient, ESPMode::ThreadSafe>();
		TSharedRef<FNullInvoker, ESPMode::ThreadSafe> Invoker = MakeShared<FNullInvoker, ESPMode::ThreadSafe>();
		TSharedPtr<FKismetAIAgent, ESPMode::ThreadSafe> Agent;

		FAgentRef MakeAgent(IKismetAIToolInvoker* InvokerOverride = nullptr)
		{
			Agent = MakeShared<FKismetAIAgent, ESPMode::ThreadSafe>(
				InvokerOverride ? *InvokerOverride : static_cast<IKismetAIToolInvoker&>(*Invoker), *Model, MakeConfig());
			return Agent.ToSharedRef();
		}
	};

	// The worker body every test starts: RunTask, holding the seams for as
	// long as the worker runs. Question events are forwarded to OnQuestion.
	TFunction<void()> RunBody(const TSharedRef<FRunFixture, ESPMode::ThreadSafe>& Fixture, const FString& Input,
		TFunction<void(const FString&)> OnQuestion = nullptr)
	{
		TSharedPtr<FKismetAIAgent, ESPMode::ThreadSafe> Agent = Fixture->Agent;
		return [Fixture, Agent, Input, OnQuestion]()
		{
			FOnAgentStep Step;
			Step.BindLambda([OnQuestion](const FString& Type, const FString& Json)
			{
				if (Type == TEXT("question") && OnQuestion) { OnQuestion(Json); }
			});
			Agent->RunTask(Input, Step);
		};
	}

	TSharedPtr<FJsonObject> AskUserArgs()
	{
		auto Option = [](const TCHAR* Label)
		{
			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("label"), Label);
			O->SetStringField(TEXT("description"), Label);
			return MakeShared<FJsonValueObject>(O);
		};
		TSharedPtr<FJsonObject> Q = MakeShared<FJsonObject>();
		Q->SetStringField(TEXT("question"), TEXT("Which one?"));
		Q->SetStringField(TEXT("header"), TEXT("Pick"));
		Q->SetBoolField(TEXT("multiSelect"), false);
		Q->SetArrayField(TEXT("options"), { Option(TEXT("A")), Option(TEXT("B")) });
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetArrayField(TEXT("questions"), { MakeShared<FJsonValueObject>(Q) });
		return Args;
	}

	// Captures the question id the run emitted and signals that the worker is
	// about to wait for an answer.
	struct FQuestionProbe
	{
		FCriticalSection Mutex;
		FString QuestionId;
		FLatch Asked;
		FString Get() { FScopeLock Lock(&Mutex); return QuestionId; }
	};

	TFunction<void(const FString&)> RecordQuestion(const TSharedRef<FQuestionProbe, ESPMode::ThreadSafe>& Probe)
	{
		return [Probe](const FString& Json)
		{
			TSharedPtr<FJsonObject> Obj;
			if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Obj) && Obj.IsValid())
			{
				FScopeLock Lock(&Probe->Mutex);
				Probe->QuestionId = Obj->GetStringField(TEXT("question_id"));
			}
			Probe->Asked.Trigger();
		};
	}

	// A dispatcher of the test's own whose game-thread callbacks are held
	// until the test releases them (seam). Closing it does not close the
	// module's.
	struct FHeldDispatcher
	{
		TSharedRef<FKismetAIGameThreadDispatcher, ESPMode::ThreadSafe> Dispatcher = MakeShared<FKismetAIGameThreadDispatcher, ESPMode::ThreadSafe>();
		FCriticalSection Mutex;
		TArray<TFunction<void()>> Held;
		FLatch Queued{ false };

		FHeldDispatcher()
		{
			Dispatcher->SetSchedulerForTests([this](TFunction<void()> Callback)
			{
				{
					FScopeLock Lock(&Mutex);
					Held.Add(MoveTemp(Callback));
				}
				Queued.Trigger();
			});
		}

		int32 Release()
		{
			check(IsInGameThread());
			TArray<TFunction<void()>> ToRun;
			{
				FScopeLock Lock(&Mutex);
				ToRun = MoveTemp(Held);
				Held.Reset();
			}
			for (TFunction<void()>& Callback : ToRun) { Callback(); }
			return ToRun.Num();
		}

		~FHeldDispatcher()
		{
			Release();
		}
	};

	class FAllowAllPolicy final : public IKismetAIToolPolicy
	{
	public:
		virtual FPolicyVerdict Evaluate(const FPolicyContext& Context) const override { return FPolicyVerdict::Allow(); }
		virtual FPolicyVerdict EvaluateAdmission(const FPolicyContext& Context) const override
		{
			++Admissions;
			return FPolicyVerdict::Allow();
		}
		mutable std::atomic<int32> Admissions{ 0 };
	};

	struct FScopedModulePolicy
	{
		explicit FScopedModulePolicy(const IKismetAIToolPolicy& Policy) { FKismetAIModule::Get().SetToolPolicyOverrideForTests(&Policy); }
		~FScopedModulePolicy() { FKismetAIModule::Get().SetToolPolicyOverrideForTests(nullptr); }
	};

	struct FPrintFixture
	{
		UBlueprint* BP = nullptr;
		UK2Node_CallFunction* Print = nullptr;

		explicit FPrintFixture(const TCHAR* Name)
		{
			const FString PackagePath = FString::Printf(TEXT("/Temp/KismetP108/%s"), Name);
			UPackage* Pkg = CreatePackage(*PackagePath);
			BP = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Pkg, Name, BPTYPE_Normal,
				UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
			if (!BP || BP->UbergraphPages.Num() == 0) { return; }
			UEdGraph* Graph = BP->UbergraphPages[0];
			Print = NewObject<UK2Node_CallFunction>(Graph);
			Print->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
				UKismetSystemLibrary::StaticClass());
			Graph->AddNode(Print, false, false);
			Print->CreateNewGuid();
			Print->AllocateDefaultPins();
			BP->GetOutermost()->SetDirtyFlag(false);
		}

		FString Duration() const
		{
			UEdGraphPin* Pin = Print ? Print->FindPin(TEXT("Duration"), EGPD_Input) : nullptr;
			return Pin ? Pin->DefaultValue : FString(TEXT("<no pin>"));
		}

		TSharedPtr<FJsonObject> SetDurationArgs(const TCHAR* Value) const
		{
			TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
			Op->SetStringField(TEXT("action"), TEXT("set_pin_default"));
			Op->SetStringField(TEXT("node"), Print->GetName());
			Op->SetStringField(TEXT("pin"), TEXT("Duration"));
			Op->SetStringField(TEXT("value"), Value);
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("blueprint"), BP->GetPathName());
			Args->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(Op) });
			return Args;
		}
	};

	TSharedPtr<FJsonObject> ParseResponse(const TUniquePtr<FHttpServerResponse>& Response)
	{
		TSharedPtr<FJsonObject> Out;
		if (Response.IsValid())
		{
			const FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Response->Body.GetData()), Response->Body.Num());
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(Conv.Length(), Conv.Get())), Out);
		}
		return Out;
	}
}

using namespace KismetAISessionLifecycleTest;

// The lease: one run at a time whatever started it; refusal changes nothing;
// closed admission after shutdown.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionLifecycle_ExclusiveLease,
	"KismetAI.SessionLifecycle.ExclusiveLease", Flags)
bool FSessionLifecycle_ExclusiveLease::RunTest(const FString& Parameters)
{
	FKismetAISessionController Session;
	TSharedRef<FRunFixture, ESPMode::ThreadSafe> A = MakeShared<FRunFixture, ESPMode::ThreadSafe>();
	TSharedRef<FRunFixture, ESPMode::ThreadSafe> B = MakeShared<FRunFixture, ESPMode::ThreadSafe>();

	const FKismetAIAdmission Panel = Session.TryAdmit(EKismetAIRunOrigin::Panel, A->MakeAgent());
	if (!TestTrue(TEXT("first run admitted"), Panel.IsAdmitted())) return false;

	const FKismetAIAdmission Http = Session.TryAdmit(EKismetAIRunOrigin::HttpAgentRun, B->MakeAgent());
	TestFalse(TEXT("an overlapping agent/run is refused"), Http.IsAdmitted());
	TestTrue(TEXT("refused as busy"), Http.Refusal == EKismetAIAdmissionRefusal::Busy);
	TestEqual(TEXT("refusal names the lease holder"), Http.ActiveRunId, Panel.Run->GetRunId());
	TestTrue(TEXT("refusal names the holder's origin"), Http.ActiveOrigin == EKismetAIRunOrigin::Panel);
	TestEqual(TEXT("lease holder unchanged"), Session.GetActiveRun()->RunId, Panel.Run->GetRunId());

	// The lease spans the turn: still held while the worker is between phases
	// and after it exits, until the owner releases it.
	TestTrue(TEXT("worker started"), Session.StartWorker(Panel.Run.ToSharedRef(), RunBody(A, TEXT("hi"))));
	TestTrue(TEXT("worker exited"), Session.WaitForWorkerExit(Panel.Run.ToSharedRef(), PromptMs));
	TestTrue(TEXT("lease still held after the worker exits, until released"), Session.IsRunning());
	TestFalse(TEXT("still refused before release"), Session.TryAdmit(EKismetAIRunOrigin::HttpAgentRun, B->MakeAgent()).IsAdmitted());
	TestFalse(TEXT("a run gets one worker"), Session.StartWorker(Panel.Run.ToSharedRef(), [](){}));

	Session.Release(Panel.Run.ToSharedRef());
	Session.Release(Panel.Run.ToSharedRef());   // idempotent
	const FKismetAIAdmission Next = Session.TryAdmit(EKismetAIRunOrigin::HttpAgentRun, B->MakeAgent());
	TestTrue(TEXT("admitted once released"), Next.IsAdmitted());
	TestFalse(TEXT("and now the panel is refused"), Session.TryAdmit(EKismetAIRunOrigin::Panel, A->MakeAgent()).IsAdmitted());

	// Admitted but never started: shutdown settles it and refuses the worker.
	const FKismetAISessionController::FDrainReport Report = Session.Shutdown(nullptr, PromptMs);
	TestTrue(TEXT("nothing left running"), Report.AllDrained());
	bool bBodyRan = false;
	TestFalse(TEXT("no worker after shutdown"), Session.StartWorker(Next.Run.ToSharedRef(), [&bBodyRan]() { bBodyRan = true; }));
	TestFalse(TEXT("the refused body never ran"), bBodyRan);
	const FKismetAIAdmission AfterClose = Session.TryAdmit(EKismetAIRunOrigin::Panel, A->MakeAgent());
	TestTrue(TEXT("admission closed after shutdown"), AfterClose.Refusal == EKismetAIAdmissionRefusal::Closed);
	TestFalse(TEXT("lease gone after shutdown"), Session.IsRunning());
	TestTrue(TEXT("second shutdown is a no-op"), Session.Shutdown(nullptr, PromptMs).bAlreadyClosed);
	return true;
}

// Module shutdown while the worker is inside a model request.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionLifecycle_ShutdownDuringModelCall,
	"KismetAI.SessionLifecycle.ShutdownDuringModelCall", Flags)
bool FSessionLifecycle_ShutdownDuringModelCall::RunTest(const FString& Parameters)
{
	FKismetAISessionController Session;
	TSharedRef<FRunFixture, ESPMode::ThreadSafe> F = MakeShared<FRunFixture, ESPMode::ThreadSafe>();
	F->Model->BlockOnCall = 0;
	const FKismetAIAdmission Admission = Session.TryAdmit(EKismetAIRunOrigin::Panel, F->MakeAgent());
	const FKismetAIRunRef Run = Admission.Run.ToSharedRef();
	Session.StartWorker(Run, RunBody(F, TEXT("hi")));
	if (!TestTrue(TEXT("worker is inside the model call"), F->Model->Entered.Wait(PromptMs)))
	{
		Session.Shutdown(nullptr, PromptMs);
		return false;
	}

	const double Start = FPlatformTime::Seconds();
	const FKismetAISessionController::FDrainReport Report = Session.Shutdown(nullptr, PromptMs);
	const double Elapsed = FPlatformTime::Seconds() - Start;

	TestEqual(TEXT("one run cancelled"), Report.Cancelled, 1);
	TestEqual(TEXT("one worker drained"), Report.Drained, 1);
	TestTrue(TEXT("none left running"), Report.AllDrained());
	TestTrue(TEXT("model client was cancelled"), F->Model->bCancelled.load());
	TestTrue(TEXT("drained well before the deadline"), Elapsed < PromptMs / 1000.0 / 2);
	TestEqual(TEXT("no live worker"), Session.NumLiveWorkers(), 0);
	TestTrue(TEXT("run exited"), Session.GetRunStatus(Run)->State == EKismetAIRunState::Exited);
	TestEqual(TEXT("no second model request"), F->Model->Calls.load(), 1);
	return true;
}

// Module shutdown while the worker waits for an ask_user answer; the stale
// answer is then refused.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionLifecycle_ShutdownDuringAskUser,
	"KismetAI.SessionLifecycle.ShutdownDuringAskUser", Flags)
bool FSessionLifecycle_ShutdownDuringAskUser::RunTest(const FString& Parameters)
{
	FKismetAISessionController Session;
	TSharedRef<FRunFixture, ESPMode::ThreadSafe> F = MakeShared<FRunFixture, ESPMode::ThreadSafe>();
	F->Model->Script = { ToolTurn(TEXT("toolu_ask"), TEXT("ask_user"), AskUserArgs()), TextTurn(TEXT("unreached")) };
	FAgentRef Agent = F->MakeAgent();
	Agent->SetInteractiveAllowed(true);
	TSharedRef<FQuestionProbe, ESPMode::ThreadSafe> Probe = MakeShared<FQuestionProbe, ESPMode::ThreadSafe>();
	const FKismetAIRunRef Run = Session.TryAdmit(EKismetAIRunOrigin::Panel, Agent).Run.ToSharedRef();
	Session.StartWorker(Run, RunBody(F, TEXT("ask"), RecordQuestion(Probe)));
	if (!TestTrue(TEXT("question asked"), Probe->Asked.Wait(PromptMs)))
	{
		Session.Shutdown(nullptr, PromptMs);
		return false;
	}

	const double Start = FPlatformTime::Seconds();
	const FKismetAISessionController::FDrainReport Report = Session.Shutdown(nullptr, PromptMs);
	TestTrue(TEXT("answer wait drained"), Report.AllDrained() && Report.Drained == 1);
	TestTrue(TEXT("promptly"), FPlatformTime::Seconds() - Start < PromptMs / 1000.0 / 2);
	TestFalse(TEXT("an answer after shutdown is refused"),
		Session.SubmitAnswer(Probe->Get(), TEXT("{\"Which one?\":\"A\"}"), EKismetAIEntryPoint::Panel));
	TestEqual(TEXT("the run did not continue"), F->Model->Calls.load(), 1);
	return true;
}

// Module shutdown while the worker's tool call is queued for the game thread
// and the game thread is the one waiting. Releasing the queued callback
// afterwards changes nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionLifecycle_ShutdownDuringQueuedEditorCall,
	"KismetAI.SessionLifecycle.ShutdownDuringQueuedEditorCall", Flags)
bool FSessionLifecycle_ShutdownDuringQueuedEditorCall::RunTest(const FString& Parameters)
{
	FPrintFixture Fixture(TEXT("BP_KismetP108Queued"));
	if (!TestNotNull(TEXT("fixture"), Fixture.Print)) return false;
	const FString Original = Fixture.Duration();
	FAllowAllPolicy Policy;
	FScopedModulePolicy ScopedPolicy(Policy);
	FHeldDispatcher Held;

	FKismetAISessionController Session;
	TSharedRef<FRunFixture, ESPMode::ThreadSafe> F = MakeShared<FRunFixture, ESPMode::ThreadSafe>();
	F->Model->Script = { ToolTurn(TEXT("toolu_edit"), TEXT("edit_blueprint"), Fixture.SetDurationArgs(TEXT("7.5"))), TextTurn(TEXT("unreached")) };
	FKismetAIModuleToolInvoker EditorInvoker(FKismetAIModule::Get(), Held.Dispatcher);
	FAgentRef Agent = F->MakeAgent(&EditorInvoker);
	Agent->SetCallerContext(FKismetAICallerContext::Verified(EKismetAIEntryPoint::Panel, TEXT("editor-user")));
	const FKismetAIRunRef Run = Session.TryAdmit(EKismetAIRunOrigin::Panel, Agent).Run.ToSharedRef();
	Session.StartWorker(Run, RunBody(F, TEXT("edit")));
	if (!TestTrue(TEXT("tool call queued for the game thread"), Held.Queued.Wait(PromptMs)))
	{
		Session.Shutdown(&Held.Dispatcher.Get(), PromptMs);
		return false;
	}

	// The game thread does not pump inside Shutdown.
	const FKismetAISessionController::FDrainReport Report = Session.Shutdown(&Held.Dispatcher.Get(), PromptMs);
	TestTrue(TEXT("worker drained without the game thread running its callback"), Report.AllDrained() && Report.Drained == 1);
	TestTrue(TEXT("dispatcher closed"), Held.Dispatcher->IsClosed());
	const FKismetAIGameThreadDispatcher::FStats Stats = Held.Dispatcher->GetStatsForTests();
	TestEqual(TEXT("queued job abandoned"), Stats.Abandoned, 1);
	TestEqual(TEXT("nothing waiting on the dispatcher"), Stats.InFlight, 0);

	// The abandoned callback runs now, after teardown: it must do nothing.
	TestEqual(TEXT("one held callback"), Held.Release(), 1);
	FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	TestEqual(TEXT("pin unchanged"), Fixture.Duration(), Original);
	TestFalse(TEXT("package not dirtied"), Fixture.BP->GetOutermost()->IsDirty());
	TestEqual(TEXT("never reached admission"), Policy.Admissions.load(), 0);
	TestEqual(TEXT("the run made no further model request"), F->Model->Calls.load(), 1);
	return true;
}

// Two workers at once — one whose lease has already been handed on (the panel
// finalization runs on the game thread while its worker is still unwinding)
// and the lease holder — one parked on a queued editor callback, one in a
// model call. Shutdown drains both, not just the first.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionLifecycle_ShutdownDrainsEveryWorker,
	"KismetAI.SessionLifecycle.ShutdownDrainsEveryWorker", Flags)
bool FSessionLifecycle_ShutdownDrainsEveryWorker::RunTest(const FString& Parameters)
{
	FPrintFixture Fixture(TEXT("BP_KismetP108TwoWorkers"));
	if (!TestNotNull(TEXT("fixture"), Fixture.Print)) return false;
	const FString Original = Fixture.Duration();
	FAllowAllPolicy Policy;
	FScopedModulePolicy ScopedPolicy(Policy);
	FHeldDispatcher Held;
	FKismetAISessionController Session;

	// Worker A: panel run, parked on a queued game-thread tool call.
	TSharedRef<FRunFixture, ESPMode::ThreadSafe> A = MakeShared<FRunFixture, ESPMode::ThreadSafe>();
	A->Model->Script = { ToolTurn(TEXT("toolu_a"), TEXT("edit_blueprint"), Fixture.SetDurationArgs(TEXT("3.5"))), TextTurn(TEXT("unreached")) };
	FKismetAIModuleToolInvoker EditorInvoker(FKismetAIModule::Get(), Held.Dispatcher);
	FAgentRef AgentA = A->MakeAgent(&EditorInvoker);
	AgentA->SetCallerContext(FKismetAICallerContext::Verified(EKismetAIEntryPoint::Panel, TEXT("editor-user")));
	const FKismetAIRunRef RunA = Session.TryAdmit(EKismetAIRunOrigin::Panel, AgentA).Run.ToSharedRef();
	Session.StartWorker(RunA, RunBody(A, TEXT("a")));
	if (!TestTrue(TEXT("A queued"), Held.Queued.Wait(PromptMs))) { Session.Shutdown(&Held.Dispatcher.Get(), PromptMs); return false; }

	// A's lease handed on while its worker is still alive.
	Session.Release(RunA);

	// Worker B: agent/run, inside a model call.
	TSharedRef<FRunFixture, ESPMode::ThreadSafe> B = MakeShared<FRunFixture, ESPMode::ThreadSafe>();
	B->Model->BlockOnCall = 0;
	const FKismetAIAdmission AdmitB = Session.TryAdmit(EKismetAIRunOrigin::HttpAgentRun, B->MakeAgent());
	if (!TestTrue(TEXT("B admitted"), AdmitB.IsAdmitted())) { Session.Shutdown(&Held.Dispatcher.Get(), PromptMs); return false; }
	Session.StartWorker(AdmitB.Run.ToSharedRef(), RunBody(B, TEXT("b")));
	if (!TestTrue(TEXT("B in model call"), B->Model->Entered.Wait(PromptMs))) { Session.Shutdown(&Held.Dispatcher.Get(), PromptMs); return false; }
	TestEqual(TEXT("two live workers"), Session.NumLiveWorkers(), 2);

	const FKismetAISessionController::FDrainReport Report = Session.Shutdown(&Held.Dispatcher.Get(), PromptMs);
	TestEqual(TEXT("both cancelled"), Report.Cancelled, 2);
	TestEqual(TEXT("both drained"), Report.Drained, 2);
	TestTrue(TEXT("none undrained"), Report.AllDrained());
	TestEqual(TEXT("no live worker"), Session.NumLiveWorkers(), 0);

	Held.Release();
	FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	TestEqual(TEXT("A's queued edit never applied"), Fixture.Duration(), Original);
	TestEqual(TEXT("never admitted"), Policy.Admissions.load(), 0);
	return true;
}

// A worker that cancellation cannot reach (stuck somewhere no wake exists).
// Shutdown returns at the deadline, reports it, and the worker keeps shared
// ownership of everything it touches: the controller is destroyed before the
// worker finishes, and the worker still exits cleanly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionLifecycle_ShutdownTimeoutKeepsOwnership,
	"KismetAI.SessionLifecycle.ShutdownTimeoutKeepsOwnership", Flags)
bool FSessionLifecycle_ShutdownTimeoutKeepsOwnership::RunTest(const FString& Parameters)
{
	TUniquePtr<FKismetAISessionController> Session = MakeUnique<FKismetAISessionController>();
	TSharedRef<FRunFixture, ESPMode::ThreadSafe> F = MakeShared<FRunFixture, ESPMode::ThreadSafe>();
	F->Model->BlockOnCall = 0;
	F->Model->bIgnoreCancel = true;
	TSharedRef<FLatch, ESPMode::ThreadSafe> BodyDone = MakeShared<FLatch, ESPMode::ThreadSafe>();
	TWeakPtr<FKismetAIRun, ESPMode::ThreadSafe> WeakRun;
	TSharedRef<FLatchedModelClient, ESPMode::ThreadSafe> Model = F->Model;
	{
		const FKismetAIRunRef Run = Session->TryAdmit(EKismetAIRunOrigin::Panel, F->MakeAgent()).Run.ToSharedRef();
		WeakRun = Run;
		TFunction<void()> Body = RunBody(F, TEXT("stuck"));
		Session->StartWorker(Run, [Body, BodyDone]() { Body(); BodyDone->Trigger(); });
	}
	if (!TestTrue(TEXT("worker inside the call"), Model->Entered.Wait(PromptMs)))
	{
		Model->Unblock();
		return false;
	}

	// The controller reports an undrained worker at Error level; expected here.
	AddExpectedError(TEXT("did not exit within"), EAutomationExpectedErrorFlags::Contains, 1);
	const FKismetAISessionController::FDrainReport Report = Session->Shutdown(nullptr, 200);
	TestEqual(TEXT("one undrained run reported"), Report.UndrainedRunIds.Num(), 1);
	TestEqual(TEXT("none drained"), Report.Drained, 0);

	// Tear down everything the "module" owned while the worker is still inside.
	Session.Reset();
	F->Agent.Reset();
	TestTrue(TEXT("the run record is still owned by its worker"), WeakRun.IsValid());

	Model->Unblock();
	TestTrue(TEXT("worker finished after the controller was destroyed"), BodyDone->Wait(PromptMs));
	const double Deadline = FPlatformTime::Seconds() + PromptMs / 1000.0;
	while (WeakRun.IsValid() && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::YieldThread();
	}
	TestFalse(TEXT("the worker released the run record on its own"), WeakRun.IsValid());
	return true;
}

// A stale answer — to a question from a run that was cancelled — after a new
// run has asked its own question: refused, and neither run moves.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionLifecycle_StaleAnswerAcrossRuns,
	"KismetAI.SessionLifecycle.StaleAnswerAcrossRuns", Flags)
bool FSessionLifecycle_StaleAnswerAcrossRuns::RunTest(const FString& Parameters)
{
	FKismetAISessionController Session;

	TSharedRef<FRunFixture, ESPMode::ThreadSafe> A = MakeShared<FRunFixture, ESPMode::ThreadSafe>();
	A->Model->Script = { ToolTurn(TEXT("toolu_a"), TEXT("ask_user"), AskUserArgs()), TextTurn(TEXT("a resumed")) };
	FAgentRef AgentA = A->MakeAgent();
	AgentA->SetInteractiveAllowed(true);
	TSharedRef<FQuestionProbe, ESPMode::ThreadSafe> ProbeA = MakeShared<FQuestionProbe, ESPMode::ThreadSafe>();
	const FKismetAIRunRef RunA = Session.TryAdmit(EKismetAIRunOrigin::Panel, AgentA).Run.ToSharedRef();
	Session.StartWorker(RunA, RunBody(A, TEXT("a"), RecordQuestion(ProbeA)));
	if (!TestTrue(TEXT("A asked"), ProbeA->Asked.Wait(PromptMs))) { Session.Shutdown(nullptr, PromptMs); return false; }
	const FString StaleId = ProbeA->Get();

	// Stop A; its finalization releases the lease.
	TestTrue(TEXT("Stop reaches the lease holder"), Session.CancelActive());
	TestFalse(TEXT("A's question is refused once A is stopped"),
		Session.SubmitAnswer(StaleId, TEXT("{\"Which one?\":\"A\"}"), EKismetAIEntryPoint::Panel));
	TestTrue(TEXT("A exited"), Session.WaitForWorkerExit(RunA, PromptMs));
	Session.Release(RunA);

	TSharedRef<FRunFixture, ESPMode::ThreadSafe> B = MakeShared<FRunFixture, ESPMode::ThreadSafe>();
	B->Model->Script = { ToolTurn(TEXT("toolu_b"), TEXT("ask_user"), AskUserArgs()), TextTurn(TEXT("b done")) };
	FAgentRef AgentB = B->MakeAgent();
	AgentB->SetInteractiveAllowed(true);
	TSharedRef<FQuestionProbe, ESPMode::ThreadSafe> ProbeB = MakeShared<FQuestionProbe, ESPMode::ThreadSafe>();
	const FKismetAIRunRef RunB = Session.TryAdmit(EKismetAIRunOrigin::Panel, AgentB).Run.ToSharedRef();
	Session.StartWorker(RunB, RunBody(B, TEXT("b"), RecordQuestion(ProbeB)));
	if (!TestTrue(TEXT("B asked"), ProbeB->Asked.Wait(PromptMs))) { Session.Shutdown(nullptr, PromptMs); return false; }

	TestFalse(TEXT("the stale answer is refused while B waits"),
		Session.SubmitAnswer(StaleId, TEXT("{\"Which one?\":\"A\"}"), EKismetAIEntryPoint::Panel));
	TestEqual(TEXT("A did not resume"), A->Model->Calls.load(), 1);
	TestEqual(TEXT("B is still waiting"), B->Model->Calls.load(), 1);
	TestEqual(TEXT("B's worker is alive"), Session.NumLiveWorkers(), 1);

	TestTrue(TEXT("B's own answer is accepted"),
		Session.SubmitAnswer(ProbeB->Get(), TEXT("{\"Which one?\":\"B\"}"), EKismetAIEntryPoint::Panel));
	TestTrue(TEXT("B finished"), Session.WaitForWorkerExit(RunB, PromptMs));
	TestEqual(TEXT("B continued exactly once"), B->Model->Calls.load(), 2);
	TestEqual(TEXT("A never resumed"), A->Model->Calls.load(), 1);
	Session.Release(RunB);
	Session.Shutdown(nullptr, PromptMs);
	return true;
}

// The module: an /agent/run that arrives while a panel run holds the lease is
// refused with 409 before any side effect.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionLifecycle_HttpRunRefusedWhileLeaseHeld,
	"KismetAI.SessionLifecycle.HttpRunRefusedWhileLeaseHeld", Flags)
bool FSessionLifecycle_HttpRunRefusedWhileLeaseHeld::RunTest(const FString& Parameters)
{
	FKismetAIModule& Module = FKismetAIModule::Get();
	if (Module.Session->IsRunning())
	{
		AddError(TEXT("a run is already active in this editor"));
		return false;
	}
	TSharedRef<FRunFixture, ESPMode::ThreadSafe> Panel = MakeShared<FRunFixture, ESPMode::ThreadSafe>();
	const FKismetAIAdmission Admission = Module.Session->TryAdmit(EKismetAIRunOrigin::Panel, Panel->MakeAgent());
	if (!TestTrue(TEXT("panel lease taken"), Admission.IsAdmitted())) return false;
	const FString SessionBefore = Module.GetCurrentSessionId();

	FHttpServerRequest Req;
	const FString Body = TEXT("{\"input\":\"edit it\",\"session_id\":\"p108-http-session\",\"blueprint\":\"/Game/Nope\"}");
	FTCHARToUTF8 Utf8(*Body);
	Req.Body.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	int32 Code = 0;
	TSharedPtr<FJsonObject> Json;
	Module.HandleAgentRun(Req, [&](TUniquePtr<FHttpServerResponse>&& Response)
	{
		Code = Response.IsValid() ? static_cast<int32>(Response->Code) : 0;
		Json = ParseResponse(Response);
	});
	Module.Session->Release(Admission.Run.ToSharedRef());

	TestEqual(TEXT("409"), Code, 409);
	const TSharedPtr<FJsonObject>* Error = nullptr;
	const bool bHasError = Json.IsValid() && Json->TryGetObjectField(TEXT("error"), Error) && Error;
	TestTrue(TEXT("agent_busy"), bHasError && (*Error)->GetStringField(TEXT("error_code")) == TEXT("agent_busy"));
	TestTrue(TEXT("names the active run"), bHasError && (*Error)->GetStringField(TEXT("message")).Contains(Admission.Run->GetRunId()));
	TestEqual(TEXT("module session id untouched"), Module.GetCurrentSessionId(), SessionBefore);
	TestFalse(TEXT("lease released"), Module.Session->IsRunning());
	return true;
}

// The module: a panel message that arrives while an /agent/run holds the
// lease is queued, not dropped and not run alongside.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionLifecycle_PanelMessageQueuedWhileHttpHoldsLease,
	"KismetAI.SessionLifecycle.PanelMessageQueuedWhileHttpHoldsLease", Flags)
bool FSessionLifecycle_PanelMessageQueuedWhileHttpHoldsLease::RunTest(const FString& Parameters)
{
	FKismetAIModule& Module = FKismetAIModule::Get();
	if (Module.Session->IsRunning() || !Module.Session->Queue().IsEmpty())
	{
		AddError(TEXT("a run or queued message is already present in this editor"));
		return false;
	}
	TSharedRef<FRunFixture, ESPMode::ThreadSafe> Http = MakeShared<FRunFixture, ESPMode::ThreadSafe>();
	const FKismetAIAdmission Admission = Module.Session->TryAdmit(EKismetAIRunOrigin::HttpAgentRun, Http->MakeAgent());
	if (!TestTrue(TEXT("agent/run lease taken"), Admission.IsAdmitted())) return false;

	Module.LaunchAgent(TEXT("p108 queued while busy"), TEXT(""), TEXT(""), TEXT("[]"));

	TestEqual(TEXT("lease holder unchanged"), Module.Session->GetActiveRun()->RunId, Admission.Run->GetRunId());
	TestEqual(TEXT("no second worker"), Module.Session->NumLiveWorkers(), 0);
	TOptional<FQueuedMessage> Queued = Module.Session->Queue().PickNext();
	TestTrue(TEXT("the message waits in the queue"), Queued.IsSet() && Queued->Text == TEXT("p108 queued while busy"));
	TestTrue(TEXT("queue drained by the test"), Module.Session->Queue().IsEmpty());
	Module.Session->Release(Admission.Run.ToSharedRef());
	return true;
}

// Dynamic unload is unsupported and enforced: the module manager abandons
// instead of unmapping the library under a worker.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionLifecycle_DynamicUnloadUnsupported,
	"KismetAI.SessionLifecycle.DynamicUnloadUnsupported", Flags)
bool FSessionLifecycle_DynamicUnloadUnsupported::RunTest(const FString& Parameters)
{
	IModuleInterface* Module = FModuleManager::Get().GetModule(TEXT("KismetAI"));
	if (!TestNotNull(TEXT("module loaded"), Module)) return false;
	TestFalse(TEXT("SupportsDynamicReloading is false"), Module->SupportsDynamicReloading());
	return true;
}

namespace KismetAIAdmissionInputsTest
{
	class FCallerInvoker final : public IKismetAIToolInvoker
	{
	public:
		virtual FKismetAIInvokeResult Invoke(const FKismetAIToolInvocation& Invocation) override
		{
			if (const TOptional<FToolExecutionResult> Refused = Invocation.RunAdmission())
			{
				return FKismetAIInvokeResult::FromTyped(Refused.GetValue());
			}
			FScopeLock Lock(&Mutex);
			Callers.Add(Invocation.Caller);
			return FKismetAIInvokeResult::FromModelString(TEXT("{\"success\":true}"));
		}
		virtual TSet<FString> GetDispatchableToolNames() const override { return { TEXT("search_assets") }; }
		virtual FString ResolveHandlerName(const FString& ToolName, const FString& Mode) const override { return TEXT("fixture"); }
		TArray<FKismetAICallerContext> GetCallers() const { FScopeLock Lock(&Mutex); return Callers; }
	private:
		mutable FCriticalSection Mutex;
		TArray<FKismetAICallerContext> Callers;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionLifecycle_AdmissionFreezesCaller,
	"KismetAI.TurnInputs.ControllerAdmissionBoundary", KismetAISessionLifecycleTest::Flags)
bool FSessionLifecycle_AdmissionFreezesCaller::RunTest(const FString& Parameters)
{
	using namespace KismetAISessionLifecycleTest;
	using namespace KismetAIAdmissionInputsTest;
	FKismetAISessionController Controller;
	const TSharedRef<FRunFixture, ESPMode::ThreadSafe> Fixture = MakeShared<FRunFixture, ESPMode::ThreadSafe>();
	const TSharedRef<FCallerInvoker, ESPMode::ThreadSafe> Invoker = MakeShared<FCallerInvoker, ESPMode::ThreadSafe>();
	const FAgentRef Agent = Fixture->MakeAgent(&Invoker.Get());
	Agent->SetCallerContext(FKismetAICallerContext::Verified(EKismetAIEntryPoint::HttpAgentRun, TEXT("admitted-caller")));
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("query"), TEXT("admission-fixture"));
	Fixture->Model->Script = { ToolTurn(TEXT("search"), TEXT("search_assets"), Args), TextTurn(TEXT("done")) };
	Fixture->Model->BlockOnCall = 0;
	const FKismetAIAdmission Admission = Controller.TryAdmit(EKismetAIRunOrigin::HttpAgentRun, Agent);
	if (!TestTrue(TEXT("run admitted"), Admission.IsAdmitted())) { return false; }
	const FKismetAIRunRef Run = Admission.Run.ToSharedRef();
	Agent->SetCallerContext(FKismetAICallerContext::Verified(EKismetAIEntryPoint::Panel, TEXT("later-caller")));
	// Keep both borrowed seams alive even if a failed worker misses its deadline.
	const bool bStarted = Controller.StartWorker(Run, [Fixture, Invoker, Agent]() { Agent->RunTask(TEXT("search")); });
	TestTrue(TEXT("worker started after input mutation"), bStarted);
	if (bStarted) { TestTrue(TEXT("model call entered"), Fixture->Model->Entered.Wait(PromptMs)); }
	Fixture->Model->Unblock();
	const bool bExited = bStarted && Controller.WaitForWorkerExit(Run, PromptMs);
	TestTrue(TEXT("worker finished"), bExited);
	const FKismetAISessionController::FDrainReport Drain = Controller.Shutdown(nullptr, PromptMs);
	TestTrue(TEXT("controller drains all workers"), Drain.AllDrained());
	const TArray<FKismetAICallerContext> Callers = Invoker->GetCallers();
	if (!TestEqual(TEXT("one tool reached executor admission"), Callers.Num(), 1)) { return false; }
	TestEqual(TEXT("admission pins the principal before worker starts"), Callers[0].Principal, FString(TEXT("admitted-caller")));
	TestTrue(TEXT("admission pins the entry point"), Callers[0].EntryPoint == EKismetAIEntryPoint::HttpAgentRun);
	return true;
}
