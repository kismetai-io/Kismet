// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// The agent side of one run's lifecycle.
//
// The session controller (editor module) owns admission, cancellation and
// drain. What it needs from the agent is narrow and is pinned here, with the
// scripted seams and no editor:
//
//   - a Stop that lands before the worker reaches RunTask is not lost. RunTask
//     used to replace the run token unconditionally, so a Stop pressed between
//     launch and the worker's first instruction was overwritten and the run
//     went on to call the model;
//   - an answer that arrives after the run was cancelled is refused, so a
//     stale reply can never be reported as accepted for stopped work.
//
// Each interleaving is forced on one thread (from inside the step callback),
// not by timing.

#include "KismetAIAgent.h"
#include "Fakes/ScriptedAgentSeams.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "KismetAISessionStorage.h"
#include "KismetAIJournalReducer.h"
#include "KismetAIMutationOutcome.h"

using namespace KismetAITestFakes;

namespace KismetAIRunLifecycleTest
{
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

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

	FString QuestionIdOf(const FString& EventJson)
	{
		TSharedPtr<FJsonObject> Obj;
		if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(EventJson), Obj) && Obj.IsValid())
		{
			return Obj->GetStringField(TEXT("question_id"));
		}
		return FString();
	}
}

// A Stop delivered after the agent exists but before its worker enters RunTask
// (the panel's launch -> thread-pool start window, or shutdown arriving there).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunLifecycle_AbortBeforeRunTaskIsHonoured,
	"KismetAI.RunLifecycle.AbortBeforeRunTaskIsHonoured", KismetAIRunLifecycleTest::Flags)
bool FRunLifecycle_AbortBeforeRunTaskIsHonoured::RunTest(const FString& Parameters)
{
	using namespace KismetAIRunLifecycleTest;

	FScriptedToolInvoker Invoker;
	FScriptedModelClient Client({ MakeTextTurn(TEXT("should never be requested")) });
	FKismetAIAgent Agent(Invoker, Client, KismetAIRunLifecycleTest::MakeConfig());
	FEventRecorder Recorder;

	Agent.Abort();
	Agent.RunTask(TEXT("hello"), Recorder.MakeCallback());

	TestEqual(TEXT("no model request after Stop"), Client.NumCalls(), 0);
	TestEqual(TEXT("exactly one terminal done"), Recorder.Count(TEXT("done")), 1);
	const FString* Done = Recorder.FirstJsonOfType(TEXT("done"));
	TestTrue(TEXT("the run reports aborted"), Done && Done->Contains(TEXT("\"aborted\"")));

	// The Stop belonged to that run. The next run on the same agent is not
	// born cancelled.
	FScriptedModelClient Client2({ MakeTextTurn(TEXT("second run")) });
	FKismetAIAgent Agent2(Invoker, Client2, KismetAIRunLifecycleTest::MakeConfig());
	Agent2.Abort();
	Agent2.RunTask(TEXT("first"), FOnAgentStep());
	FEventRecorder Recorder2;
	Agent2.RunTask(TEXT("second"), Recorder2.MakeCallback());
	TestEqual(TEXT("a later run is not cancelled by an earlier Stop"), Client2.NumCalls(), 1);
	return true;
}

// The user answers at the instant the run is stopped: the question is still
// armed (the worker has not woken to disarm it yet), but the run it belongs
// to is cancelled. The answer must be refused, not reported as delivered.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunLifecycle_AnswerAfterCancelIsRefused,
	"KismetAI.RunLifecycle.AnswerAfterCancelIsRefused", KismetAIRunLifecycleTest::Flags)
bool FRunLifecycle_AnswerAfterCancelIsRefused::RunTest(const FString& Parameters)
{
	using namespace KismetAIRunLifecycleTest;

	FScriptedToolInvoker Invoker;
	FScriptedModelClient Client({
		MakeToolTurn(TEXT("toolu_ask"), TEXT("ask_user"), AskUserArgs()),
		MakeTextTurn(TEXT("must not be reached")) });
	FKismetAIAgent Agent(Invoker, Client, KismetAIRunLifecycleTest::MakeConfig());
	Agent.SetInteractiveAllowed(true);

	FString QuestionId;
	bool bAnswerAccepted = false;
	bool bFreshAnswerAccepted = true;
	int32 DoneCount = 0;
	FOnAgentStep Callback;
	Callback.BindLambda([&](const FString& Type, const FString& Json)
	{
		if (Type == TEXT("question"))
		{
			QuestionId = QuestionIdOf(Json);
			// Emitted with the channel armed, before the worker waits: the
			// exact point a Stop and a reply can both land.
			Agent.Abort();
			bAnswerAccepted = Agent.SubmitAnswer(QuestionId, TEXT("{\"Which one?\":\"A\"}"), EKismetAIEntryPoint::Panel);
		}
		else if (Type == TEXT("done"))
		{
			++DoneCount;
		}
	});

	Agent.RunTask(TEXT("ask me"), Callback);

	TestFalse(TEXT("question id was emitted"), QuestionId.IsEmpty());
	TestFalse(TEXT("an answer to a cancelled run is refused"), bAnswerAccepted);
	TestEqual(TEXT("the stopped run made only the first request"), Client.NumCalls(), 1);
	TestEqual(TEXT("one terminal event"), DoneCount, 1);

	// After the run, the same (now stale) question id is refused too.
	bFreshAnswerAccepted = Agent.SubmitAnswer(QuestionId, TEXT("{}"), EKismetAIEntryPoint::Panel);
	TestFalse(TEXT("a late answer after the run ended is refused"), bFreshAnswerAccepted);
	return true;
}

namespace KismetAIRunLifecycleTest
{
	FString TerminalField(const FEventRecorder& Recorder, const TCHAR* Field)
	{
		const FString* Json = Recorder.FirstJsonOfType(TEXT("done"));
		if (!Json) { Json = Recorder.FirstJsonOfType(TEXT("error")); }
		TSharedPtr<FJsonObject> Obj;
		FString Value;
		if (Json && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(*Json), Obj) && Obj)
		{
			Obj->TryGetStringField(Field, Value);
		}
		return Value;
	}

	TSharedPtr<FJsonObject> MutationArgs()
	{
		TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
		Op->SetStringField(TEXT("action"), TEXT("set_pin_default"));
		Op->SetStringField(TEXT("node"), TEXT("Node"));
		Op->SetStringField(TEXT("pin"), TEXT("InString"));
		Op->SetStringField(TEXT("value"), TEXT("changed"));
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("blueprint"), TEXT("/Game/LifecycleFixture"));
		Args->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(Op) });
		return Args;
	}

	bool IsCallOutcome(const TSharedPtr<FJsonObject>& Entry)
	{
		FString Type, Record;
		return Entry->TryGetStringField(TEXT("type"), Type) && Type == TEXT("tool_result_message")
			&& Entry->TryGetStringField(TEXT("record"), Record) && Record == TEXT("tool_call");
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunLifecycle_FinalAssistantCommitFailure,
	"KismetAI.RunLifecycle.FinalAssistantCommitFailure", KismetAIRunLifecycleTest::Flags)
bool FRunLifecycle_FinalAssistantCommitFailure::RunTest(const FString& Parameters)
{
	using namespace KismetAIRunLifecycleTest;
	FScriptedToolInvoker Invoker;
	FScriptedModelClient Client({ MakeTextTurn(TEXT("The answer is forty-two.")) });
	FKismetAIAgent Agent(Invoker, Client, KismetAIRunLifecycleTest::MakeConfig());
	FEventRecorder Recorder;
	bool bRejected = false;
	Agent.SetJournalAppendHookForTests([&](const TSharedPtr<FJsonObject>& Entry)
	{
		FString Type;
		if (Entry->TryGetStringField(TEXT("type"), Type) && Type == TEXT("assistant_message"))
		{
			bRejected = true;
			return false;
		}
		return true;
	});
	Agent.RunTask(TEXT("What is the answer?"), Recorder.MakeCallback());
	TestTrue(TEXT("fault reached final assistant commit"), bRejected);
	TestEqual(TEXT("one terminal event"), Recorder.Count(TEXT("done")) + Recorder.Count(TEXT("error")), 1);
	TestEqual(TEXT("uncommitted final answer is failed"), TerminalField(Recorder, TEXT("outcome")), FString(TEXT("failed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunLifecycle_SuccessAfterCancellation,
	"KismetAI.RunLifecycle.SuccessAfterCancellation", KismetAIRunLifecycleTest::Flags)
bool FRunLifecycle_SuccessAfterCancellation::RunTest(const FString& Parameters)
{
	using namespace KismetAIRunLifecycleTest;
	FScriptedToolInvoker Invoker;
	FScriptedModelClient Client({ MakeTextTurn(TEXT("Late answer")) });
	Client.TextDeltasByCall.Add(0, TEXT("provisional"));
	FKismetAIAgent Agent(Invoker, Client, KismetAIRunLifecycleTest::MakeConfig());
	FEventRecorder Recorder;
	FOnAgentStep Callback;
	Callback.BindLambda([&](const FString& Type, const FString& Json)
	{
		Recorder.Events.Emplace(Type, Json);
		if (Type == TEXT("text_delta")) { Agent.Abort(); }
	});
	Agent.RunTask(TEXT("answer"), Callback);
	TestTrue(TEXT("Stop reached model client"), Client.WasCancelled());
	TestEqual(TEXT("one terminal event"), Recorder.Count(TEXT("done")), 1);
	TestEqual(TEXT("late successful response cannot complete cancelled run"), TerminalField(Recorder, TEXT("status")), FString(TEXT("aborted")));
	TestEqual(TEXT("no tools after cancellation"), Invoker.CallLog.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunLifecycle_FatalModelHasTerminal,
	"KismetAI.RunLifecycle.FatalModelHasTerminal", KismetAIRunLifecycleTest::Flags)
bool FRunLifecycle_FatalModelHasTerminal::RunTest(const FString& Parameters)
{
	using namespace KismetAIRunLifecycleTest;
	FModelCallOutcome Failure;
	Failure.Result = EModelCallResult::FatalError;
	Failure.ErrorType = TEXT("fixture_failure");
	Failure.ErrorMessage = TEXT("Deterministic provider refusal");
	FScriptedModelClient Client({ Failure });
	FScriptedToolInvoker Invoker;
	FKismetAIAgent Agent(Invoker, Client, KismetAIRunLifecycleTest::MakeConfig());
	FEventRecorder Recorder;
	Agent.RunTask(TEXT("answer"), Recorder.MakeCallback());
	TestEqual(TEXT("error remains terminal"), Recorder.Count(TEXT("error")), 1);
	TestEqual(TEXT("failure publishes one terminal event"), Recorder.Count(TEXT("done")) + Recorder.Count(TEXT("error")), 1);
	TestEqual(TEXT("terminal outcome names failure"), TerminalField(Recorder, TEXT("outcome")), FString(TEXT("failed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunLifecycle_CancelAtFinalToolCommit,
	"KismetAI.RunLifecycle.CancelAtFinalToolCommit", KismetAIRunLifecycleTest::Flags)
bool FRunLifecycle_CancelAtFinalToolCommit::RunTest(const FString& Parameters)
{
	using namespace KismetAIRunLifecycleTest;
	FKismetAIAgentConfig Config = KismetAIRunLifecycleTest::MakeConfig();
	Config.MaxSteps = 1;
	FScriptedModelClient Client({ MakeToolTurn(TEXT("last"), TEXT("edit_blueprint"), MutationArgs()) });
	FScriptedToolInvoker Invoker;
	FKismetAIAgent Agent(Invoker, Client, Config);
	FEventRecorder Recorder;
	bool bCommitReached = false;
	Agent.SetJournalAppendHookForTests([&](const TSharedPtr<FJsonObject>& Entry)
	{
		if (IsCallOutcome(Entry)) { bCommitReached = true; Agent.Abort(); }
		return true;
	});
	Agent.RunTask(TEXT("edit"), Recorder.MakeCallback());
	TestTrue(TEXT("outcome commit reached"), bCommitReached);
	TestEqual(TEXT("one mutation executed"), Invoker.CountCalls(TEXT("edit_blueprint")), 1);
	TestEqual(TEXT("one terminal event"), Recorder.Count(TEXT("done")), 1);
	TestEqual(TEXT("cancellation wins over exhausted turn budget"), TerminalField(Recorder, TEXT("status")), FString(TEXT("aborted")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunLifecycle_QuestionResumesWithoutCompletion,
	"KismetAI.RunLifecycle.QuestionResumesWithoutCompletion", KismetAIRunLifecycleTest::Flags)
bool FRunLifecycle_QuestionResumesWithoutCompletion::RunTest(const FString& Parameters)
{
	using namespace KismetAIRunLifecycleTest;
	FScriptedModelClient Client({ MakeToolTurn(TEXT("question"), TEXT("ask_user"), AskUserArgs()), MakeTextTurn(TEXT("Selected A.")) });
	FScriptedToolInvoker Invoker;
	FKismetAIAgent Agent(Invoker, Client, KismetAIRunLifecycleTest::MakeConfig());
	Agent.SetInteractiveAllowed(true);
	FEventRecorder Recorder;
	FString WaitingSession;
	bool bAccepted = false;
	FOnAgentStep Callback;
	Callback.BindLambda([&](const FString& Type, const FString& Json)
	{
		Recorder.Events.Emplace(Type, Json);
		if (Type == TEXT("question"))
		{
			WaitingSession = Agent.GetSessionId();
			TestTrue(TEXT("question exposes waiting phase"), Agent.GetRunPhase() == EKismetAIRunPhase::WaitingForUser);
			TestEqual(TEXT("waiting has no terminal event"), Recorder.Count(TEXT("done")), 0);
			TestEqual(TEXT("pending question remains armed"), Agent.GetPendingQuestionId(), QuestionIdOf(Json));
			bAccepted = Agent.SubmitAnswer(QuestionIdOf(Json), TEXT("{\"Which one?\":\"A\"}"), EKismetAIEntryPoint::Panel);
		}
	});
	Agent.RunTask(TEXT("ask"), Callback);
	TestTrue(TEXT("answer accepted"), bAccepted);
	TestTrue(TEXT("answered run leaves finished phase"), Agent.GetRunPhase() == EKismetAIRunPhase::Finished);
	TestFalse(TEXT("session identified while waiting"), WaitingSession.IsEmpty());
	TestEqual(TEXT("same session after answer"), Agent.GetSessionId(), WaitingSession);
	TestEqual(TEXT("model continues after answer"), Client.NumCalls(), 2);
	TestEqual(TEXT("only final answer completes run"), Recorder.Count(TEXT("done")), 1);
	TestEqual(TEXT("answered run completes"), TerminalField(Recorder, TEXT("status")), FString(TEXT("completed")));
	return true;
}


namespace KismetAIRunLifecycleTest
{
	FString MutationEvidence()
	{
		FKismetAIMutationOutcome Outcome;
		Outcome.bAppliedInMemory = true;
		Outcome.CompileStatus = EKismetAICompileStatus::Succeeded;
		Outcome.SaveStatus = EKismetAISaveStatus::Saved;
		Outcome.PackageName = TEXT("/Game/LifecycleFixture");
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetObjectField(TEXT("outcome"), Outcome.ToJson());
		Data->SetStringField(TEXT("marker"), TEXT("COMMITTED_LIFECYCLE_MUTATION"));
		TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
		Envelope->SetBoolField(TEXT("success"), true);
		Envelope->SetObjectField(TEXT("data"), Data);
		FString Json;
		FJsonSerializer::Serialize(Envelope.ToSharedRef(), TJsonWriterFactory<>::Create(&Json));
		return Json;
	}

	bool LedgerBeforeTerminal(const FEventRecorder& Recorder)
	{
		bool bLedgerSeen = false;
		for (const auto& Event : Recorder.Events)
		{
			if (Event.Key == TEXT("mutation_outcomes")) { bLedgerSeen = true; }
			if (Event.Key == TEXT("error") || Event.Key == TEXT("done")) { return bLedgerSeen; }
		}
		return false;
	}

	bool HasReplayEvidence(const FSessionSnapshot& Snapshot)
	{
		for (const FAgentMessage& Message : Snapshot.Messages)
		{
			for (const FAgentContentPart& Part : Message.Parts)
			{
				if (Part.Type == EAgentPartType::ToolResult && Part.ToolResultContent.Contains(TEXT("COMMITTED_LIFECYCLE_MUTATION"))) { return true; }
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunLifecycle_CommittedMutationThenProviderFailure,
	"KismetAI.RunLifecycle.CommittedMutationThenProviderFailure", KismetAIRunLifecycleTest::Flags)
bool FRunLifecycle_CommittedMutationThenProviderFailure::RunTest(const FString& Parameters)
{
	using namespace KismetAIRunLifecycleTest;
	FModelCallOutcome Failure;
	Failure.Result = EModelCallResult::FatalError;
	Failure.ErrorType = TEXT("fixture_failure");
	Failure.ErrorMessage = TEXT("Provider refused continuation");
	FScriptedModelClient Client({ MakeToolTurn(TEXT("mutation"), TEXT("edit_blueprint"), MutationArgs()), Failure });
	FScriptedToolInvoker Invoker;
	Invoker.SetResult(TEXT("edit_blueprint"), MutationEvidence());
	FKismetAIAgent Agent(Invoker, Client, KismetAIRunLifecycleTest::MakeConfig());
	FEventRecorder Recorder;
	Agent.RunTask(TEXT("edit"), Recorder.MakeCallback());
	TestEqual(TEXT("one mutation executed"), Invoker.CountCalls(TEXT("edit_blueprint")), 1);
	TestEqual(TEXT("one terminal event"), Recorder.Count(TEXT("error")) + Recorder.Count(TEXT("done")), 1);
	TestEqual(TEXT("provider failure outcome"), TerminalField(Recorder, TEXT("outcome")), FString(TEXT("failed")));
	TestTrue(TEXT("mutation ledger precedes failure terminal"), LedgerBeforeTerminal(Recorder));
	const FSessionSnapshot Snapshot = FKismetAISessionStorage::LoadSession(Agent.GetSessionId());
	TestEqual(TEXT("committed call remains certain"), Snapshot.UnrecordedCalls, 0);
	TestTrue(TEXT("recovery retains committed result"), HasReplayEvidence(Snapshot));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunLifecycle_MutationCommitFailureIsUncertain,
	"KismetAI.RunLifecycle.MutationCommitFailureIsUncertain", KismetAIRunLifecycleTest::Flags)
bool FRunLifecycle_MutationCommitFailureIsUncertain::RunTest(const FString& Parameters)
{
	using namespace KismetAIRunLifecycleTest;
	AddExpectedError(TEXT("Session journal write failed"), EAutomationExpectedErrorFlags::Contains, 1);
	FScriptedModelClient Client({ MakeToolTurn(TEXT("mutation"), TEXT("edit_blueprint"), MutationArgs()), MakeTextTurn(TEXT("not reached")) });
	FScriptedToolInvoker Invoker;
	Invoker.SetResult(TEXT("edit_blueprint"), MutationEvidence());
	FKismetAIAgent Agent(Invoker, Client, KismetAIRunLifecycleTest::MakeConfig());
	FEventRecorder Recorder;
	Agent.SetJournalAppendHookForTests([](const TSharedPtr<FJsonObject>& Entry) { return !IsCallOutcome(Entry); });
	Agent.RunTask(TEXT("edit"), Recorder.MakeCallback());
	TestEqual(TEXT("one mutation executed"), Invoker.CountCalls(TEXT("edit_blueprint")), 1);
	TestEqual(TEXT("no model continuation after lost outcome"), Client.NumCalls(), 1);
	TestEqual(TEXT("one terminal event"), Recorder.Count(TEXT("error")) + Recorder.Count(TEXT("done")), 1);
	TestEqual(TEXT("lost mutation outcome is uncertain"), TerminalField(Recorder, TEXT("outcome")), FString(TEXT("uncertain")));
	TestTrue(TEXT("real mutation evidence precedes uncertain terminal"), LedgerBeforeTerminal(Recorder));
	const FSessionSnapshot Snapshot = FKismetAISessionStorage::LoadSession(Agent.GetSessionId());
	TestEqual(TEXT("recovery does not fabricate outcome"), Snapshot.UnrecordedCalls, 1);
	TestEqual(TEXT("mutation intent remains available for reconciliation"), Snapshot.UncertainCalls.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunLifecycle_ProjectionCommitFailure,
	"KismetAI.RunLifecycle.ProjectionCommitFailurePreservesCommittedTools", KismetAIRunLifecycleTest::Flags)
bool FRunLifecycle_ProjectionCommitFailure::RunTest(const FString& Parameters)
{
	using namespace KismetAIRunLifecycleTest;
	FKismetAIAgentConfig Config = KismetAIRunLifecycleTest::MakeConfig();
	Config.ContextOverflowThreshold = 0.0f;
	FScriptedModelClient Client({
		MakeToolTurn(TEXT("mutation1"), TEXT("edit_blueprint"), MutationArgs()),
		MakeToolTurn(TEXT("mutation2"), TEXT("edit_blueprint"), MutationArgs()),
		MakeToolTurn(TEXT("mutation3"), TEXT("edit_blueprint"), MutationArgs()),
		MakeTextTurn(TEXT("must not be requested")) });
	FScriptedToolInvoker Invoker;
	// This scenario needs authoritative success so each edit reaches the projection fault.
	TSharedPtr<FJsonObject> Envelope;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(MutationEvidence()), Envelope);
	Invoker.SetTypedResult(TEXT("edit_blueprint"), FToolExecutionResult::Success(Envelope->GetObjectField(TEXT("data"))));
	FKismetAIAgent Agent(Invoker, Client, Config);
	FEventRecorder Recorder;
	bool bFaultReached = false;
	Agent.SetJournalAppendHookForTests([&](const TSharedPtr<FJsonObject>& Entry)
	{
		FString Type;
		if (Entry->TryGetStringField(TEXT("type"), Type) && Type == TEXT("context_compaction") && Entry->HasField(TEXT("projection")))
		{
			bFaultReached = true;
			return false;
		}
		return true;
	});
	Agent.RunTask(TEXT("edit"), Recorder.MakeCallback());
	TestTrue(TEXT("fault reached durable projection boundary"), bFaultReached);
	TestEqual(TEXT("no continuation with uncommitted projection"), Client.NumCalls(), 3);
	TestEqual(TEXT("committed tools never rerun"), Invoker.CountCalls(TEXT("edit_blueprint")), 3);
	TestEqual(TEXT("one terminal event"), Recorder.Count(TEXT("error")) + Recorder.Count(TEXT("done")), 1);
	TestEqual(TEXT("projection failure outcome"), TerminalField(Recorder, TEXT("outcome")), FString(TEXT("failed")));
	TestTrue(TEXT("mutation evidence precedes terminal"), LedgerBeforeTerminal(Recorder));
	const FSessionSnapshot Snapshot = FKismetAISessionStorage::LoadSession(Agent.GetSessionId());
	TestEqual(TEXT("all committed tools recover without uncertainty"), Snapshot.UnrecordedCalls, 0);
	TestTrue(TEXT("replay retains original result evidence"), HasReplayEvidence(Snapshot));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunLifecycle_HeaderCommitFailure,
	"KismetAI.RunLifecycle.HeaderCommitFailure", KismetAIRunLifecycleTest::Flags)
bool FRunLifecycle_HeaderCommitFailure::RunTest(const FString& Parameters)
{
	using namespace KismetAIRunLifecycleTest;
	FScriptedModelClient Client({ MakeTextTurn(TEXT("must not be requested")) });
	FScriptedToolInvoker Invoker;
	FKismetAIAgent Agent(Invoker, Client, KismetAIRunLifecycleTest::MakeConfig());
	FEventRecorder Recorder;
	bool bFaultReached = false;
	Agent.SetJournalAppendHookForTests([&](const TSharedPtr<FJsonObject>& Entry)
	{
		FString Type;
		if (Entry->TryGetStringField(TEXT("type"), Type) && Type == TEXT("session_start")) { bFaultReached = true; return false; }
		return true;
	});
	Agent.RunTask(TEXT("answer"), Recorder.MakeCallback());
	TestTrue(TEXT("header commit attempted"), bFaultReached);
	TestEqual(TEXT("uncommitted session never calls model"), Client.NumCalls(), 0);
	TestEqual(TEXT("one terminal event"), Recorder.Count(TEXT("error")) + Recorder.Count(TEXT("done")), 1);
	TestEqual(TEXT("header failure outcome"), TerminalField(Recorder, TEXT("outcome")), FString(TEXT("failed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunLifecycle_UserCommitFailure,
	"KismetAI.RunLifecycle.UserCommitFailure", KismetAIRunLifecycleTest::Flags)
bool FRunLifecycle_UserCommitFailure::RunTest(const FString& Parameters)
{
	using namespace KismetAIRunLifecycleTest;
	FScriptedModelClient Client({ MakeTextTurn(TEXT("must not be requested")) });
	FScriptedToolInvoker Invoker;
	FKismetAIAgent Agent(Invoker, Client, KismetAIRunLifecycleTest::MakeConfig());
	FEventRecorder Recorder;
	bool bFaultReached = false;
	Agent.SetJournalAppendHookForTests([&](const TSharedPtr<FJsonObject>& Entry)
	{
		FString Type;
		if (Entry->TryGetStringField(TEXT("type"), Type) && Type == TEXT("user_message")) { bFaultReached = true; return false; }
		return true;
	});
	Agent.RunTask(TEXT("answer"), Recorder.MakeCallback());
	TestTrue(TEXT("user commit attempted"), bFaultReached);
	TestEqual(TEXT("uncommitted user request never calls model"), Client.NumCalls(), 0);
	TestEqual(TEXT("one terminal event"), Recorder.Count(TEXT("error")) + Recorder.Count(TEXT("done")), 1);
	TestEqual(TEXT("user failure outcome"), TerminalField(Recorder, TEXT("outcome")), FString(TEXT("failed")));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunLifecycle_MetadataCommitFailure,
	"KismetAI.RunLifecycle.MetadataCommitFailurePreservesCommittedHistory", KismetAIRunLifecycleTest::Flags)
bool FRunLifecycle_MetadataCommitFailure::RunTest(const FString& Parameters)
{
	using namespace KismetAIRunLifecycleTest;
	FScriptedModelClient Client({ MakeToolTurn(TEXT("mutation"), TEXT("edit_blueprint"), MutationArgs()),
		MakeTextTurn(TEXT("Final durable answer.")) });
	FScriptedToolInvoker Invoker;
	Invoker.SetResult(TEXT("edit_blueprint"), MutationEvidence());
	FKismetAIAgent Agent(Invoker, Client, KismetAIRunLifecycleTest::MakeConfig());
	FEventRecorder Recorder;
	bool bFaultReached = false;
	Agent.SetJournalAppendHookForTests([&](const TSharedPtr<FJsonObject>& Entry)
	{
		FString Type;
		if (Entry->TryGetStringField(TEXT("type"), Type) && Type == TEXT("session_meta")) { bFaultReached = true; return false; }
		return true;
	});
	Agent.RunTask(TEXT("edit"), Recorder.MakeCallback());
	TestTrue(TEXT("metadata durability boundary reached"), bFaultReached);
	TestEqual(TEXT("one terminal event"), Recorder.Count(TEXT("error")) + Recorder.Count(TEXT("done")), 1);
	TestEqual(TEXT("metadata commit failure is failed"), TerminalField(Recorder, TEXT("outcome")), FString(TEXT("failed")));
	TestEqual(TEXT("metadata failure identifies persistence"), TerminalField(Recorder, TEXT("error_code")), FString(TEXT("session_journal_write_failed")));
	TestTrue(TEXT("mutation evidence precedes terminal"), LedgerBeforeTerminal(Recorder));
	TestTrue(TEXT("failed finish leaves finished phase"), Agent.GetRunPhase() == EKismetAIRunPhase::Finished);
	const FSessionSnapshot Snapshot = FKismetAISessionStorage::LoadSession(Agent.GetSessionId());
	TestEqual(TEXT("committed mutation still certain"), Snapshot.UnrecordedCalls, 0);
	TestEqual(TEXT("reducer reports failed finish from durable fallback"), Snapshot.Status, FString(TEXT("failed")));
	FString ListedStatus;
	for (const FKismetAISessionInfo& Session : FKismetAISessionStorage::ListSessions())
	{
		if (Session.SessionId == Agent.GetSessionId()) { ListedStatus = Session.Status; }
	}
	TestEqual(TEXT("session list reports failed finish"), ListedStatus, FString(TEXT("failed")));
	TestTrue(TEXT("committed mutation survives metadata failure"), HasReplayEvidence(Snapshot));
	bool bFinalAnswerPresent = false;
	for (const FAgentMessage& Message : Snapshot.Messages)
	{
		if (Message.GetText().Contains(TEXT("Final durable answer."))) { bFinalAnswerPresent = true; }
	}
	TestTrue(TEXT("committed final assistant survives metadata failure"), bFinalAnswerPresent);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunLifecycle_CancelledMetadataCommitFailure,
	"KismetAI.RunLifecycle.CancelledMetadataCommitFailureKeepsCancellation", KismetAIRunLifecycleTest::Flags)
bool FRunLifecycle_CancelledMetadataCommitFailure::RunTest(const FString& Parameters)
{
	using namespace KismetAIRunLifecycleTest;
	FScriptedModelClient Client({ MakeToolTurn(TEXT("mutation"), TEXT("edit_blueprint"), MutationArgs()),
		MakeTextTurn(TEXT("must not be requested")) });
	FScriptedToolInvoker Invoker;
	Invoker.SetResult(TEXT("edit_blueprint"), MutationEvidence());
	FKismetAIAgent Agent(Invoker, Client, KismetAIRunLifecycleTest::MakeConfig());
	FEventRecorder Recorder;
	bool bFaultReached = false;
	Agent.SetJournalAppendHookForTests([&](const TSharedPtr<FJsonObject>& Entry)
	{
		FString Type;
		if (Entry->TryGetStringField(TEXT("type"), Type) && Type == TEXT("session_meta")) { bFaultReached = true; return false; }
		return true;
	});
	FOnAgentStep Callback;
	Callback.BindLambda([&](const FString& Type, const FString& Json)
	{
		Recorder.Events.Emplace(Type, Json);
		if (Type == TEXT("tool_result")) { Agent.Abort(); }
	});
	Agent.RunTask(TEXT("edit"), Callback);
	TestTrue(TEXT("cancelled finish attempted metadata commit"), bFaultReached);
	TestEqual(TEXT("one mutation executed"), Invoker.CountCalls(TEXT("edit_blueprint")), 1);
	TestEqual(TEXT("cancel prevents model continuation"), Client.NumCalls(), 1);
	TestEqual(TEXT("one terminal event"), Recorder.Count(TEXT("error")) + Recorder.Count(TEXT("done")), 1);
	TestEqual(TEXT("metadata failure does not erase cancellation"), TerminalField(Recorder, TEXT("status")), FString(TEXT("aborted")));
	const FString* Done = Recorder.FirstJsonOfType(TEXT("done"));
	TSharedPtr<FJsonObject> Terminal;
	bool bPersistenceComplete = true;
	if (Done && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(*Done), Terminal) && Terminal)
	{
		Terminal->TryGetBoolField(TEXT("persistence_complete"), bPersistenceComplete);
	}
	TestFalse(TEXT("terminal states incomplete metadata persistence"), bPersistenceComplete);
	TestTrue(TEXT("mutation evidence precedes cancelled terminal"), LedgerBeforeTerminal(Recorder));
	const FSessionSnapshot Snapshot = FKismetAISessionStorage::LoadSession(Agent.GetSessionId());
	TestEqual(TEXT("reducer reports aborted fallback"), Snapshot.Status, FString(TEXT("aborted")));
	TestEqual(TEXT("mutation outcome remained durable"), Snapshot.UnrecordedCalls, 0);
	TestTrue(TEXT("replay retains mutation evidence"), HasReplayEvidence(Snapshot));
	FString ListedStatus;
	for (const FKismetAISessionInfo& Session : FKismetAISessionStorage::ListSessions())
	{
		if (Session.SessionId == Agent.GetSessionId()) { ListedStatus = Session.Status; }
	}
	TestEqual(TEXT("session list reports aborted fallback"), ListedStatus, FString(TEXT("aborted")));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunLifecycle_DamageNoticeCommitFailure,
	"KismetAI.RunLifecycle.DamageNoticeCommitFailureStopsResume", KismetAIRunLifecycleTest::Flags)
bool FRunLifecycle_DamageNoticeCommitFailure::RunTest(const FString& Parameters)
{
	using namespace KismetAIRunLifecycleTest;
	const FString SessionId = FKismetAISessionStorage::CreateSessionId();
	TSharedPtr<FJsonObject> Header = MakeShared<FJsonObject>();
	Header->SetStringField(TEXT("type"), TEXT("session_start"));
	Header->SetNumberField(TEXT("v"), KismetAIJournalReducer::JournalFormatVersion);
	if (!TestTrue(TEXT("fixture header saved"), FKismetAISessionStorage::AppendEntry(SessionId, Header))) { return false; }
	TSharedPtr<FJsonObject> User = MakeShared<FJsonObject>();
	User->SetStringField(TEXT("type"), TEXT("user_message"));
	User->SetStringField(TEXT("content"), TEXT("ORIGINAL_DAMAGED_SESSION_INSTRUCTION"));
	if (!TestTrue(TEXT("fixture user saved"), FKismetAISessionStorage::AppendEntry(SessionId, User))) { return false; }
	const FString Path = FKismetAISessionStorage::GetSessionFilePath(SessionId);
	const FString MalformedLine = TEXT("{\"type\":\"user_message\",\"content\":\"damaged fragment\n");
	if (!TestTrue(TEXT("terminated malformed record saved"), FFileHelper::SaveStringToFile(MalformedLine, *Path,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append))) { return false; }
	FScriptedModelClient Client({ MakeTextTurn(TEXT("must not be requested")) });
	FScriptedToolInvoker Invoker;
	FKismetAIAgent Agent(Invoker, Client, KismetAIRunLifecycleTest::MakeConfig());
	FEventRecorder Recorder;
	bool bFaultReached = false;
	Agent.SetJournalAppendHookForTests([&](const TSharedPtr<FJsonObject>& Entry)
	{
		FString Type;
		if (Entry->TryGetStringField(TEXT("type"), Type) && Type == TEXT("synthetic_message"))
		{
			bFaultReached = true;
			return false;
		}
		return true;
	});
	Agent.RunTask(TEXT("continue"), Recorder.MakeCallback(), TEXT(""), SessionId);
	TestTrue(TEXT("resume damage-notice durability boundary reached"), bFaultReached);
	TestEqual(TEXT("no model request with uncommitted damage notice"), Client.NumCalls(), 0);
	TestEqual(TEXT("no tools during failed resume"), Invoker.CallLog.Num(), 0);
	TestEqual(TEXT("one terminal event"), Recorder.Count(TEXT("error")) + Recorder.Count(TEXT("done")), 1);
	TestEqual(TEXT("notice failure is terminal failed"), TerminalField(Recorder, TEXT("outcome")), FString(TEXT("failed")));
	const FSessionSnapshot Failed = FKismetAISessionStorage::LoadSession(SessionId);
	TestEqual(TEXT("failed notice is not recorded as delivered"), Failed.NoticedDamageKeys.Num(), 0);
	FString Saved;
	TestTrue(TEXT("journal remains readable"), FFileHelper::LoadFileToString(Saved, *Path));
	TestTrue(TEXT("original user content preserved"), Saved.Contains(TEXT("ORIGINAL_DAMAGED_SESSION_INSTRUCTION")));
	TestTrue(TEXT("original damaged bytes preserved"), Saved.Contains(MalformedLine));

	FScriptedModelClient ResumedClient({ MakeTextTurn(TEXT("Recovered.")) });
	FKismetAIAgent Resumed(Invoker, ResumedClient, KismetAIRunLifecycleTest::MakeConfig());
	Resumed.RunTask(TEXT("resume safely"), FOnAgentStep(), TEXT(""), SessionId);
	TestEqual(TEXT("later healthy resume reaches model once"), ResumedClient.NumCalls(), 1);
	bool bNoticeInRequest = false;
	if (ResumedClient.NumCalls() == 1)
	{
		for (const TSharedPtr<FJsonValue>& Message : ResumedClient.GetRequests()[0].Messages)
		{
			TSharedPtr<FJsonObject> Object = Message->AsObject();
			FString Json;
			FJsonSerializer::Serialize(Object.ToSharedRef(), TJsonWriterFactory<>::Create(&Json));
			if (Json.Contains(TEXT("[Session log notice]"))) { bNoticeInRequest = true; }
		}
	}
	TestTrue(TEXT("healthy resume regenerates model-visible damage notice"), bNoticeInRequest);
	TestTrue(TEXT("healthy resume durably records notice"), FKismetAISessionStorage::LoadSession(SessionId).NoticedDamageKeys.Num() > 0);
	return true;
}
