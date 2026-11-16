// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Each tool call's outcome is journaled before the next call starts.
//
// These drive the real RunTask loop through the scripted seams and then read
// the journal back through the production loader
// (FKismetAISessionStorage::LoadMessages) — the path a resumed run takes — not
// only through the pure reducer. The fake invoker stands in for the asset: its
// invocation count is the "physical" effect each scenario checks.
//
// Two deterministic hooks: FScriptedToolInvoker::OnInvoke fires inside a tool
// call, and FKismetAIAgent::SetJournalAppendHookForTests fires before each
// journal write (and can refuse it, simulating a failed disk write).

#include "Fakes/ScriptedAgentSeams.h"
#include "KismetAIJournalReducer.h"
#include "KismetAIMutationOutcome.h"
#include "KismetAISessionStorage.h"
#include "KismetAIToolCallJournal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Policies/CondensedJsonPrintPolicy.h"

using namespace KismetAITestFakes;

constexpr auto ToolJournalTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	FKismetAIAgentConfig MakeJournalTestConfig()
	{
		FKismetAIAgentConfig C;
		C.ApiKey = TEXT("test-key-unused");
		C.Model = TEXT("claude-sonnet-4-6");
		C.MaxSteps = 6;
		C.MaxRetries = 0;
		C.RetryJitterFactor = 0.0;
		C.ThinkingMode = EThinkingMode::Disabled;
		return C;
	}

	// edit_blueprint arguments the real validator accepts. NodeName differs per
	// call so loop health does not treat B as a duplicate of A.
	TSharedPtr<FJsonObject> EditArgs(const TCHAR* NodeName)
	{
		TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
		Op->SetStringField(TEXT("action"), TEXT("set_pin_default"));
		Op->SetStringField(TEXT("node"), NodeName);
		Op->SetStringField(TEXT("pin"), TEXT("InString"));
		Op->SetStringField(TEXT("value"), TEXT("hi"));

		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("blueprint"), TEXT("/Game/P003/BP_Journal"));
		A->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(Op) });
		return A;
	}

	// One assistant turn requesting two mutations, A then B.
	FModelCallOutcome MakeTwoMutationTurn()
	{
		return MakeResponse({
			MakeToolUseBlock(TEXT("toolu_A"), TEXT("edit_blueprint"), EditArgs(TEXT("NodeA"))),
			MakeToolUseBlock(TEXT("toolu_B"), TEXT("edit_blueprint"), EditArgs(TEXT("NodeB"))),
		}, TEXT("tool_use"));
	}

	// A successful mutation result carrying an outcome.
	FToolExecutionResult MutationSuccessResult()
	{
		FKismetAIMutationOutcome Outcome;
		Outcome.bAppliedInMemory = true;
		Outcome.CompileStatus = EKismetAICompileStatus::Succeeded;
		Outcome.SaveStatus = EKismetAISaveStatus::Saved;
		Outcome.SavePolicy = KismetAISavePolicy::AutosaveEvenIfCompileFailed;
		Outcome.PackageName = TEXT("/Game/P003/BP_Journal");
		Outcome.PackageFilename = TEXT("/tmp/BP_Journal.uasset");

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetObjectField(TEXT("outcome"), Outcome.ToJson());
		Data->SetStringField(TEXT("marker"), TEXT("A_REAL_RESULT"));
		return FToolExecutionResult::Success(Data);
	}

	bool IsCallRecordFor(const TSharedPtr<FJsonObject>& Entry, const TCHAR* ToolUseId)
	{
		FString Type, Record, Id;
		return Entry.IsValid()
			&& Entry->TryGetStringField(TEXT("type"), Type) && Type == TEXT("tool_result_message")
			&& Entry->TryGetStringField(TEXT("record"), Record) && Record == KismetAIToolCallJournal::RecordToolCall
			&& Entry->TryGetStringField(TEXT("tool_use_id"), Id) && Id == ToolUseId;
	}

	TArray<TSharedPtr<FJsonObject>> CallRecordsFor(const TArray<TSharedPtr<FJsonObject>>& Entries, const TCHAR* ToolUseId)
	{
		TArray<TSharedPtr<FJsonObject>> Out;
		for (const TSharedPtr<FJsonObject>& E : Entries)
		{
			if (IsCallRecordFor(E, ToolUseId)) { Out.Add(E); }
		}
		return Out;
	}

	FString CallJournalRoleOf(const TSharedPtr<FJsonValue>& Msg)
	{
		const TSharedPtr<FJsonObject> Obj = Msg.IsValid() ? Msg->AsObject() : nullptr;
		return Obj.IsValid() ? Obj->GetStringField(TEXT("role")) : FString();
	}

	// tool_result blocks of a user message, keyed by tool_use_id (in order).
	TArray<TSharedPtr<FJsonObject>> ToolResultsOf(const TSharedPtr<FJsonValue>& Msg)
	{
		TArray<TSharedPtr<FJsonObject>> Out;
		const TSharedPtr<FJsonObject> Obj = Msg.IsValid() ? Msg->AsObject() : nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
		if (Obj.IsValid() && Obj->TryGetArrayField(TEXT("content"), Content))
		{
			for (const TSharedPtr<FJsonValue>& B : *Content)
			{
				const TSharedPtr<FJsonObject> BO = B->AsObject();
				if (BO.IsValid() && BO->GetStringField(TEXT("type")) == TEXT("tool_result")) { Out.Add(BO); }
			}
		}
		return Out;
	}

	int32 CountToolUses(const TSharedPtr<FJsonValue>& Msg)
	{
		int32 N = 0;
		const TSharedPtr<FJsonObject> Obj = Msg.IsValid() ? Msg->AsObject() : nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
		if (Obj.IsValid() && Obj->TryGetArrayField(TEXT("content"), Content))
		{
			for (const TSharedPtr<FJsonValue>& B : *Content)
			{
				const TSharedPtr<FJsonObject> BO = B->AsObject();
				if (BO.IsValid() && BO->GetStringField(TEXT("type")) == TEXT("tool_use")) { ++N; }
			}
		}
		return N;
	}

	// Index of the (single) assistant message that carries tool_use blocks.
	int32 FindToolBatch(const TArray<TSharedPtr<FJsonValue>>& Messages)
	{
		for (int32 i = 0; i < Messages.Num(); ++i)
		{
			if (CallJournalRoleOf(Messages[i]) == TEXT("assistant") && CountToolUses(Messages[i]) > 0) { return i; }
		}
		return INDEX_NONE;
	}

	bool StringFieldIs(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, const TCHAR* Expected)
	{
		FString V;
		return Obj.IsValid() && Obj->TryGetStringField(Field, V) && V == Expected;
	}

	FString ContentOf(const TSharedPtr<FJsonObject>& Block)
	{
		FString V;
		if (Block.IsValid()) { Block->TryGetStringField(TEXT("content"), V); }
		return V;
	}

	bool IsErrorBlock(const TSharedPtr<FJsonObject>& Block)
	{
		bool b = false;
		return Block.IsValid() && Block->TryGetBoolField(TEXT("is_error"), b) && b;
	}

	// Removes the session file whatever the test outcome.
	struct FSessionCleanup
	{
		FString SessionId;
		~FSessionCleanup()
		{
			if (!SessionId.IsEmpty()) { FKismetAISessionStorage::DeleteSession(SessionId); }
		}
	};
}

// ─────────────────────────────────────────────────────────────────────────────
// The R1 reproduction: A completes, Stop lands before B, reload.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FToolJournal_CancelBetweenCallsSurvivesReload,
	"KismetAI.ToolCallJournal.CancelBetweenCallsSurvivesReload", ToolJournalTestFlags)
bool FToolJournal_CancelBetweenCallsSurvivesReload::RunTest(const FString& Parameters)
{
	FScriptedModelClient Model({ MakeTwoMutationTurn(), MakeTextTurn(TEXT("never reached")) });
	FScriptedToolInvoker Tools;
	Tools.SetTypedResult(TEXT("edit_blueprint"), MutationSuccessResult());
	FEventRecorder Rec;

	FKismetAIAgent Agent(Tools, Model, MakeJournalTestConfig());
	FSessionCleanup Cleanup;

	// Deterministic Stop: right after A's record is durably appended, before B.
	Agent.SetJournalAppendHookForTests([&Agent](const TSharedPtr<FJsonObject>& Entry)
	{
		if (IsCallRecordFor(Entry, TEXT("toolu_A"))) { Agent.Abort(); }
		return true;
	});
	Agent.RunTask(TEXT("edit two nodes"), Rec.MakeCallback());
	Cleanup.SessionId = Agent.GetSessionId();

	TestEqual(TEXT("A ran and B did not (asset changed exactly once)"), Tools.CountCalls(TEXT("edit_blueprint")), 1);
	const FString* Done = Rec.FirstJsonOfType(TEXT("done"));
	TestTrue(TEXT("run ends aborted"), Done && Done->Contains(TEXT("\"status\":\"aborted\"")));

	// --- the journal: exactly one terminal record per call, ids intact ---
	const TArray<TSharedPtr<FJsonObject>> Entries = FKismetAISessionStorage::LoadAllEntries(Cleanup.SessionId);
	const TArray<TSharedPtr<FJsonObject>> RecA = CallRecordsFor(Entries, TEXT("toolu_A"));
	const TArray<TSharedPtr<FJsonObject>> RecB = CallRecordsFor(Entries, TEXT("toolu_B"));
	TestEqual(TEXT("one record for A"), RecA.Num(), 1);
	TestEqual(TEXT("one record for B"), RecB.Num(), 1);
	if (RecA.Num() != 1 || RecB.Num() != 1) { return false; }

	TestTrue(TEXT("A succeeded"), StringFieldIs(RecA[0], TEXT("status"), TEXT("succeeded")));
	TestTrue(TEXT("A was attempted"), RecA[0]->GetBoolField(TEXT("attempted")));
	TestTrue(TEXT("A is a mutation"), RecA[0]->GetBoolField(TEXT("is_mutation")));
	TestTrue(TEXT("A carries its typed mutation outcome"), RecA[0]->HasTypedField<EJson::Object>(TEXT("mutation_outcome")));
	TestTrue(TEXT("B is not_started"), StringFieldIs(RecB[0], TEXT("status"), TEXT("not_started")));
	TestFalse(TEXT("B was not attempted"), RecB[0]->GetBoolField(TEXT("attempted")));
	TestTrue(TEXT("B says why"), StringFieldIs(RecB[0], TEXT("not_started_reason"), TEXT("cancelled")));

	// The batch stamp and both records share run and turn; call ids differ.
	TSharedPtr<FJsonObject> Batch;
	for (const TSharedPtr<FJsonObject>& E : Entries)
	{
		if (StringFieldIs(E, TEXT("type"), TEXT("assistant_message")) && E->HasField(TEXT("turn_id"))) { Batch = E; }
	}
	TestTrue(TEXT("assistant batch is stamped"), Batch.IsValid());
	if (Batch.IsValid())
	{
		const FString TurnId = Batch->GetStringField(TEXT("turn_id"));
		TestEqual(TEXT("A in the batch's turn"), RecA[0]->GetStringField(TEXT("turn_id")), TurnId);
		TestEqual(TEXT("B in the batch's turn"), RecB[0]->GetStringField(TEXT("turn_id")), TurnId);
		TestEqual(TEXT("same run"), RecA[0]->GetStringField(TEXT("run_id")), Batch->GetStringField(TEXT("run_id")));
		TestEqual(TEXT("A call index"), (int32)RecA[0]->GetNumberField(TEXT("call_index")), 0);
		TestEqual(TEXT("B call index"), (int32)RecB[0]->GetNumberField(TEXT("call_index")), 1);
		TestNotEqual(TEXT("distinct call ids"), RecA[0]->GetStringField(TEXT("call_id")), RecB[0]->GetStringField(TEXT("call_id")));
	}

	// --- production reload ---
	const TArray<TSharedPtr<FJsonValue>> Loaded = FKismetAISessionStorage::LoadMessages(Cleanup.SessionId);
	const int32 BatchIdx = FindToolBatch(Loaded);
	TestTrue(TEXT("reload keeps the tool batch"), BatchIdx != INDEX_NONE && BatchIdx + 1 < Loaded.Num());
	if (BatchIdx == INDEX_NONE || BatchIdx + 1 >= Loaded.Num()) { return false; }
	TestEqual(TEXT("both tool_use blocks survive"), CountToolUses(Loaded[BatchIdx]), 2);
	TestEqual(TEXT("one user message follows"), CallJournalRoleOf(Loaded[BatchIdx + 1]), FString(TEXT("user")));
	const TArray<TSharedPtr<FJsonObject>> Results = ToolResultsOf(Loaded[BatchIdx + 1]);
	TestEqual(TEXT("one tool_result per tool_use"), Results.Num(), 2);
	if (Results.Num() == 2)
	{
		TestTrue(TEXT("A first"), StringFieldIs(Results[0], TEXT("tool_use_id"), TEXT("toolu_A")));
		TestTrue(TEXT("A keeps its real result"), ContentOf(Results[0]).Contains(TEXT("A_REAL_RESULT")));
		TestFalse(TEXT("A is not an error"), IsErrorBlock(Results[0]));
		TestTrue(TEXT("B second"), StringFieldIs(Results[1], TEXT("tool_use_id"), TEXT("toolu_B")));
		TestTrue(TEXT("B is explicitly not executed"), ContentOf(Results[1]).Contains(KismetAIToolCallJournal::NotExecutedCode));
		TestTrue(TEXT("B is an error result"), IsErrorBlock(Results[1]));
	}

	// The pure reducer agrees with the production loader.
	const FSessionSnapshot Snap = KismetAIJournalReducer::Reduce(Entries);
	const int32 SnapBatch = FindToolBatch(KismetAIAgentCodec::ToAnthropicArray(Snap.Messages));
	TestTrue(TEXT("reducer pairs the batch"), SnapBatch != INDEX_NONE && SnapBatch + 1 < Snap.Messages.Num()
		&& ToolResultsOf(MakeShared<FJsonValueObject>(KismetAIAgentCodec::ToAnthropicJson(Snap.Messages[SnapBatch + 1]))).Num() == 2);

	// --- the next outgoing request after resume ---
	FScriptedModelClient Model2({ MakeTextTurn(TEXT("A was applied; B never ran.")) });
	FKismetAIAgent Resumed(Tools, Model2, MakeJournalTestConfig());
	Resumed.RunTask(TEXT("Stop changing this asset; only explain what happened."), FOnAgentStep(), TEXT(""), Cleanup.SessionId);

	TestEqual(TEXT("resume replays nothing"), Tools.CountCalls(TEXT("edit_blueprint")), 1);
	TestTrue(TEXT("resumed run made a request"), Model2.NumCalls() >= 1);
	if (Model2.NumCalls() >= 1)
	{
		const TArray<TSharedPtr<FJsonValue>>& Sent = Model2.GetRequests()[0].Messages;
		const int32 SentBatch = FindToolBatch(Sent);
		TestTrue(TEXT("outgoing history keeps the batch"), SentBatch != INDEX_NONE && SentBatch + 1 < Sent.Num());
		if (SentBatch != INDEX_NONE && SentBatch + 1 < Sent.Num())
		{
			const TArray<TSharedPtr<FJsonObject>> SentResults = ToolResultsOf(Sent[SentBatch + 1]);
			TestEqual(TEXT("outgoing history pairs both calls"), SentResults.Num(), 2);
		}
		const TSharedPtr<FJsonObject> Last = Sent.Last()->AsObject();
		TestTrue(TEXT("the new instruction is the last message"),
			Last.IsValid() && Last->GetStringField(TEXT("content")).Contains(TEXT("only explain")));
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Append failure after a mutation: B must not run, the user must be told.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FToolJournal_AppendFailureStopsBatch,
	"KismetAI.ToolCallJournal.AppendFailureStopsBatch", ToolJournalTestFlags)
bool FToolJournal_AppendFailureStopsBatch::RunTest(const FString& Parameters)
{
	// The loop logs the journal loss at Error severity; that is the point.
	AddExpectedError(TEXT("Session journal write failed"), EAutomationExpectedErrorFlags::Contains, 1);
	FScriptedModelClient Model({ MakeTwoMutationTurn(), MakeTextTurn(TEXT("never reached")) });
	FScriptedToolInvoker Tools;
	Tools.SetTypedResult(TEXT("edit_blueprint"), MutationSuccessResult());
	FEventRecorder Rec;

	FKismetAIAgent Agent(Tools, Model, MakeJournalTestConfig());
	FSessionCleanup Cleanup;

	// The disk "fills up" exactly when A's outcome is written.
	Agent.SetJournalAppendHookForTests([](const TSharedPtr<FJsonObject>& Entry)
	{
		return !IsCallRecordFor(Entry, TEXT("toolu_A"));
	});
	const FString Final = Agent.RunTask(TEXT("edit two nodes"), Rec.MakeCallback());
	Cleanup.SessionId = Agent.GetSessionId();

	TestEqual(TEXT("A ran, B never invoked"), Tools.CountCalls(TEXT("edit_blueprint")), 1);
	TestEqual(TEXT("no further model call"), Model.NumCalls(), 1);

	// The real outcome is not relabelled as a failure.
	const FString* ToolResult = Rec.FirstJsonOfType(TEXT("tool_result"));
	TestTrue(TEXT("A's tool_result event carries its real result"), ToolResult && ToolResult->Contains(TEXT("A_REAL_RESULT")));
	TestTrue(TEXT("the ledger still reports A's package"), Rec.HasType(TEXT("mutation_outcomes")));

	// The user sees the uncertainty.
	const FString* Err = Rec.FirstJsonOfType(TEXT("error"));
	TestTrue(TEXT("an error event is emitted"), Err != nullptr);
	if (Err)
	{
		TestTrue(TEXT("error code names the journal"), Err->Contains(TEXT("session_journal_write_failed")));
		TestTrue(TEXT("persistence is flagged uncertain"), Err->Contains(TEXT("\"persistence_uncertain\":true")));
		TestTrue(TEXT("message says the edit may lack a durable record"), Err->Contains(TEXT("without a durable record")));
		TestTrue(TEXT("message says B did not run"), Err->Contains(TEXT("were not run")));
	}
	TestTrue(TEXT("the run's final text says so too"), Final.Contains(TEXT("without a durable record")));
	TestFalse(TEXT("no 'completed' done"), Rec.FirstJsonOfType(TEXT("done")) && Rec.FirstJsonOfType(TEXT("done"))->Contains(TEXT("completed")));

	// Nothing claims either call completed: the journal has no record for
	// either, and reload projects them as unrecorded, not as success.
	const TArray<TSharedPtr<FJsonObject>> Entries = FKismetAISessionStorage::LoadAllEntries(Cleanup.SessionId);
	TestEqual(TEXT("no record for A was written"), CallRecordsFor(Entries, TEXT("toolu_A")).Num(), 0);
	TestEqual(TEXT("no record for B was written"), CallRecordsFor(Entries, TEXT("toolu_B")).Num(), 0);

	const TArray<TSharedPtr<FJsonValue>> Loaded = FKismetAISessionStorage::LoadMessages(Cleanup.SessionId);
	const int32 BatchIdx = FindToolBatch(Loaded);
	TestTrue(TEXT("reload keeps the batch"), BatchIdx != INDEX_NONE && BatchIdx + 1 < Loaded.Num());
	if (BatchIdx != INDEX_NONE && BatchIdx + 1 < Loaded.Num())
	{
		const TArray<TSharedPtr<FJsonObject>> Results = ToolResultsOf(Loaded[BatchIdx + 1]);
		TestEqual(TEXT("both calls paired"), Results.Num(), 2);
		for (const TSharedPtr<FJsonObject>& R : Results)
		{
			TestTrue(TEXT("projected as unrecorded"), ContentOf(R).Contains(KismetAIToolCallJournal::OutcomeUnrecordedCode));
			TestTrue(TEXT("never as success"), IsErrorBlock(R));
		}
	}
	return true;
}

// If the assistant batch itself cannot be journaled, no tool runs at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FToolJournal_BatchAppendFailureRunsNothing,
	"KismetAI.ToolCallJournal.BatchAppendFailureRunsNothing", ToolJournalTestFlags)
bool FToolJournal_BatchAppendFailureRunsNothing::RunTest(const FString& Parameters)
{
	// The loop logs the journal loss at Error severity; that is the point.
	AddExpectedError(TEXT("Session journal write failed"), EAutomationExpectedErrorFlags::Contains, 1);
	FScriptedModelClient Model({ MakeTwoMutationTurn() });
	FScriptedToolInvoker Tools;
	Tools.SetTypedResult(TEXT("edit_blueprint"), MutationSuccessResult());
	FEventRecorder Rec;

	FKismetAIAgent Agent(Tools, Model, MakeJournalTestConfig());
	FSessionCleanup Cleanup;
	Agent.SetJournalAppendHookForTests([](const TSharedPtr<FJsonObject>& Entry)
	{
		return !(StringFieldIs(Entry, TEXT("type"), TEXT("assistant_message")) && Entry->HasField(TEXT("turn_id")));
	});
	Agent.RunTask(TEXT("edit two nodes"), Rec.MakeCallback());
	Cleanup.SessionId = Agent.GetSessionId();

	TestEqual(TEXT("no tool ran"), Tools.CallLog.Num(), 0);
	const FString* Err = Rec.FirstJsonOfType(TEXT("error"));
	TestTrue(TEXT("error event emitted"), Err && Err->Contains(TEXT("session_journal_write_failed")));
	TestTrue(TEXT("nothing was changed, and it says so"), Err && Err->Contains(TEXT("\"persistence_uncertain\":false")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Stop arrives while a failing call is returning.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FToolJournal_CancelDuringFailingCallKeepsFailure,
	"KismetAI.ToolCallJournal.CancelDuringFailingCallKeepsFailure", ToolJournalTestFlags)
bool FToolJournal_CancelDuringFailingCallKeepsFailure::RunTest(const FString& Parameters)
{
	FScriptedModelClient Model({ MakeTwoMutationTurn(), MakeTextTurn(TEXT("never reached")) });
	FScriptedToolInvoker Tools;
	Tools.SetResult(TEXT("edit_blueprint"), ToolError(TEXT("PIN_NOT_FOUND"), TEXT("No pin InString on NodeA")));
	FEventRecorder Rec;

	FKismetAIAgent Agent(Tools, Model, MakeJournalTestConfig());
	FSessionCleanup Cleanup;
	// The Stop lands inside A, before A's (failing) result is returned.
	Tools.OnInvoke = [&Agent](const FString&) { Agent.Abort(); };

	Agent.RunTask(TEXT("edit two nodes"), Rec.MakeCallback());
	Cleanup.SessionId = Agent.GetSessionId();

	TestEqual(TEXT("only A was invoked"), Tools.CountCalls(TEXT("edit_blueprint")), 1);

	const TArray<TSharedPtr<FJsonObject>> Entries = FKismetAISessionStorage::LoadAllEntries(Cleanup.SessionId);
	const TArray<TSharedPtr<FJsonObject>> RecA = CallRecordsFor(Entries, TEXT("toolu_A"));
	const TArray<TSharedPtr<FJsonObject>> RecB = CallRecordsFor(Entries, TEXT("toolu_B"));
	TestEqual(TEXT("exactly one terminal record for A"), RecA.Num(), 1);
	TestEqual(TEXT("exactly one terminal record for B"), RecB.Num(), 1);
	if (RecA.Num() == 1)
	{
		TestTrue(TEXT("A recorded as failed, not cancelled"), StringFieldIs(RecA[0], TEXT("status"), TEXT("failed")));
		TestTrue(TEXT("A was attempted"), RecA[0]->GetBoolField(TEXT("attempted")));
		const TArray<TSharedPtr<FJsonValue>>& Res = RecA[0]->GetArrayField(TEXT("results"));
		const FString Content = Res.Num() == 1 ? ContentOf(Res[0]->AsObject()) : FString();
		TestTrue(TEXT("A keeps its real error"), Content.Contains(TEXT("PIN_NOT_FOUND")));
		TestFalse(TEXT("A is not overwritten as not executed"), Content.Contains(KismetAIToolCallJournal::NotExecutedCode));
	}
	if (RecB.Num() == 1)
	{
		TestTrue(TEXT("B not started"), StringFieldIs(RecB[0], TEXT("status"), TEXT("not_started")));
	}

	const TArray<TSharedPtr<FJsonValue>> Loaded = FKismetAISessionStorage::LoadMessages(Cleanup.SessionId);
	const int32 BatchIdx = FindToolBatch(Loaded);
	if (BatchIdx != INDEX_NONE && BatchIdx + 1 < Loaded.Num())
	{
		const TArray<TSharedPtr<FJsonObject>> Results = ToolResultsOf(Loaded[BatchIdx + 1]);
		TestEqual(TEXT("reload pairs both"), Results.Num(), 2);
		if (Results.Num() == 2)
		{
			TestTrue(TEXT("reload keeps A's real failure"), ContentOf(Results[0]).Contains(TEXT("PIN_NOT_FOUND")));
			TestTrue(TEXT("reload marks B not executed"), ContentOf(Results[1]).Contains(KismetAIToolCallJournal::NotExecutedCode));
		}
	}
	else
	{
		AddError(TEXT("reload lost the batch"));
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// A completed batch: per-call records reload to exactly what the model saw.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FToolJournal_CompletedBatchReloadsAsSent,
	"KismetAI.ToolCallJournal.CompletedBatchReloadsAsSent", ToolJournalTestFlags)
bool FToolJournal_CompletedBatchReloadsAsSent::RunTest(const FString& Parameters)
{
	FScriptedModelClient Model({ MakeTwoMutationTurn(), MakeTextTurn(TEXT("Both applied.")) });
	FScriptedToolInvoker Tools;
	Tools.SetTypedResult(TEXT("edit_blueprint"), MutationSuccessResult());

	FKismetAIAgent Agent(Tools, Model, MakeJournalTestConfig());
	FSessionCleanup Cleanup;

	// Order check: A's record must be on disk before B is invoked.
	int32 RecordsBeforeSecondInvoke = -1;
	int32 RecordsWritten = 0;
	Agent.SetJournalAppendHookForTests([&RecordsWritten](const TSharedPtr<FJsonObject>& Entry)
	{
		if (IsCallRecordFor(Entry, TEXT("toolu_A")) || IsCallRecordFor(Entry, TEXT("toolu_B"))) { ++RecordsWritten; }
		return true;
	});
	Tools.OnInvoke = [&Tools, &RecordsWritten, &RecordsBeforeSecondInvoke](const FString&)
	{
		if (Tools.CallLog.Num() == 2) { RecordsBeforeSecondInvoke = RecordsWritten; }
	};

	Agent.RunTask(TEXT("edit two nodes"), FOnAgentStep());
	Cleanup.SessionId = Agent.GetSessionId();

	TestEqual(TEXT("both ran"), Tools.CountCalls(TEXT("edit_blueprint")), 2);
	TestEqual(TEXT("A was recorded before B started"), RecordsBeforeSecondInvoke, 1);

	if (Model.NumCalls() < 2)
	{
		AddError(TEXT("expected a second model call"));
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& Sent = Model.GetRequests()[1].Messages;
	const int32 SentBatch = FindToolBatch(Sent);
	const TArray<TSharedPtr<FJsonValue>> Loaded = FKismetAISessionStorage::LoadMessages(Cleanup.SessionId);
	const int32 LoadedBatch = FindToolBatch(Loaded);
	if (SentBatch == INDEX_NONE || LoadedBatch == INDEX_NONE || SentBatch + 1 >= Sent.Num() || LoadedBatch + 1 >= Loaded.Num())
	{
		AddError(TEXT("batch missing from sent or reloaded history"));
		return false;
	}
	const TArray<TSharedPtr<FJsonObject>> SentResults = ToolResultsOf(Sent[SentBatch + 1]);
	const TArray<TSharedPtr<FJsonObject>> LoadedResults = ToolResultsOf(Loaded[LoadedBatch + 1]);
	TestEqual(TEXT("same number of results"), LoadedResults.Num(), SentResults.Num());
	for (int32 i = 0; i < FMath::Min(SentResults.Num(), LoadedResults.Num()); ++i)
	{
		TestEqual(TEXT("same tool_use_id"), LoadedResults[i]->GetStringField(TEXT("tool_use_id")), SentResults[i]->GetStringField(TEXT("tool_use_id")));
		TestEqual(TEXT("same content"), ContentOf(LoadedResults[i]), ContentOf(SentResults[i]));
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Reload shapes: a crash mid-batch, and journals written earlier.
// ─────────────────────────────────────────────────────────────────────────────

// A crash after A's record and before B's: B is projected as unrecorded — never
// as success, rollback or something safe to replay.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FToolJournal_CrashMidBatchProjectsUnrecorded,
	"KismetAI.ToolCallJournal.CrashMidBatchProjectsUnrecorded", ToolJournalTestFlags)
bool FToolJournal_CrashMidBatchProjectsUnrecorded::RunTest(const FString& Parameters)
{
	const FString Jsonl =
		TEXT("{\"type\":\"user_message\",\"content\":\"edit\"}\n")
		TEXT("{\"type\":\"assistant_message\",\"run_id\":\"r\",\"turn_id\":\"r/t0\",\"tool_call_count\":2,\"content\":[")
			TEXT("{\"type\":\"tool_use\",\"id\":\"toolu_A\",\"name\":\"edit_blueprint\",\"input\":{}},")
			TEXT("{\"type\":\"tool_use\",\"id\":\"toolu_B\",\"name\":\"edit_blueprint\",\"input\":{}}]}\n")
		TEXT("{\"type\":\"tool_result_message\",\"record\":\"tool_call\",\"schema\":1,\"run_id\":\"r\",\"turn_id\":\"r/t0\",\"call_id\":\"r/t0/c0\",\"call_index\":0,\"call_count\":2,\"tool_use_id\":\"toolu_A\",\"tool_name\":\"edit_blueprint\",\"status\":\"succeeded\",\"attempted\":true,")
			TEXT("\"results\":[{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_A\",\"content\":\"{\\\"success\\\":true}\"}]}\n")
		// A truncated final line: the crash happened while writing B's record.
		TEXT("{\"type\":\"tool_result_message\",\"record\":\"tool_c");

	const FSessionSnapshot Snap = KismetAIJournalReducer::ReduceJsonl(Jsonl);
	TestEqual(TEXT("truncated line skipped"), Snap.SkippedEntryCount, 1);
	TestEqual(TEXT("user, assistant, results"), Snap.Messages.Num(), 3);
	if (Snap.Messages.Num() == 3)
	{
		const TArray<TSharedPtr<FJsonObject>> Results = ToolResultsOf(MakeShared<FJsonValueObject>(KismetAIAgentCodec::ToAnthropicJson(Snap.Messages[2])));
		TestEqual(TEXT("both paired"), Results.Num(), 2);
		if (Results.Num() == 2)
		{
			TestTrue(TEXT("A keeps its recorded result"), ContentOf(Results[0]).Contains(TEXT("success")));
			TestFalse(TEXT("A is not an error"), IsErrorBlock(Results[0]));
			TestTrue(TEXT("B is unrecorded"), ContentOf(Results[1]).Contains(KismetAIToolCallJournal::OutcomeUnrecordedCode));
			TestTrue(TEXT("B is an error, not a success"), IsErrorBlock(Results[1]));
			TestTrue(TEXT("B forbids blind replay"), ContentOf(Results[1]).Contains(TEXT("do not replay")));
		}
	}

	// The same file through the production loader.
	const FString Sid = FKismetAISessionStorage::CreateSessionId();
	FSessionCleanup Cleanup{ Sid };
	FFileHelper::SaveStringToFile(Jsonl, *FKismetAISessionStorage::GetSessionFilePath(Sid),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const TArray<TSharedPtr<FJsonValue>> Loaded = FKismetAISessionStorage::LoadMessages(Sid);
	TestEqual(TEXT("loader: user, assistant, results"), Loaded.Num(), 3);
	if (Loaded.Num() == 3)
	{
		const TArray<TSharedPtr<FJsonObject>> Results = ToolResultsOf(Loaded[2]);
		TestTrue(TEXT("loader projects B as unrecorded"), Results.Num() == 2
			&& ContentOf(Results[1]).Contains(KismetAIToolCallJournal::OutcomeUnrecordedCode));
	}
	return true;
}

// Journals written earlier have no turn_id: one batch tool_result_message,
// and an interrupted batch has no results at all. Both still load.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FToolJournal_LegacyJournalStillLoads,
	"KismetAI.ToolCallJournal.LegacyJournalStillLoads", ToolJournalTestFlags)
bool FToolJournal_LegacyJournalStillLoads::RunTest(const FString& Parameters)
{
	const FString Jsonl =
		TEXT("{\"type\":\"session_start\",\"v\":1,\"model\":\"claude-sonnet-4-6\"}\n")
		TEXT("{\"type\":\"user_message\",\"content\":\"edit\"}\n")
		TEXT("{\"type\":\"assistant_message\",\"content\":[")
			TEXT("{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"edit_blueprint\",\"input\":{}},")
			TEXT("{\"type\":\"tool_use\",\"id\":\"t2\",\"name\":\"compile_blueprint\",\"input\":{}}]}\n")
		TEXT("{\"type\":\"tool_result_message\",\"results\":[")
			TEXT("{\"type\":\"tool_result\",\"tool_use_id\":\"t1\",\"content\":\"ok1\"},")
			TEXT("{\"type\":\"tool_result\",\"tool_use_id\":\"t2\",\"content\":\"ok2\"},")
			TEXT("{\"type\":\"text\",\"text\":\"[orchestrator] note\"}]}\n")
		TEXT("{\"type\":\"assistant_message\",\"content\":[{\"type\":\"text\",\"text\":\"done\"}]}\n")
		TEXT("{\"type\":\"user_message\",\"content\":\"again\"}\n")
		// Interrupted legacy batch: no results. Now the one loader
		// answers it itself (OUTCOME_UNRECORDED); RunTask applied exactly that
		// repair to the old loader's output, so the resumed request is unchanged.
		TEXT("{\"type\":\"assistant_message\",\"content\":[{\"type\":\"tool_use\",\"id\":\"t3\",\"name\":\"edit_blueprint\",\"input\":{}}]}\n");

	const FString Sid = FKismetAISessionStorage::CreateSessionId();
	FSessionCleanup Cleanup{ Sid };
	FFileHelper::SaveStringToFile(Jsonl, *FKismetAISessionStorage::GetSessionFilePath(Sid),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	const TArray<TSharedPtr<FJsonValue>> Loaded = FKismetAISessionStorage::LoadMessages(Sid);
	TestEqual(TEXT("six journal messages plus the answer to the open call"), Loaded.Num(), 7);
	if (Loaded.Num() == 7)
	{
		TestEqual(TEXT("legacy batch results intact"), ToolResultsOf(Loaded[2]).Num(), 2);
		const TArray<TSharedPtr<FJsonValue>>& Content = Loaded[2]->AsObject()->GetArrayField(TEXT("content"));
		TestEqual(TEXT("legacy note kept in the same message"), Content.Num(), 3);
		TestEqual(TEXT("interrupted legacy batch kept"), CallJournalRoleOf(Loaded[5]), FString(TEXT("assistant")));
		const TArray<TSharedPtr<FJsonObject>> Answer = ToolResultsOf(Loaded[6]);
		TestTrue(TEXT("the open call is answered as unrecorded, not as a success"), Answer.Num() == 1
			&& ContentOf(Answer[0]).Contains(KismetAIToolCallJournal::OutcomeUnrecordedCode) && IsErrorBlock(Answer[0]));
	}
	for (int32 i = 0; i < FMath::Min(Loaded.Num(), 6); ++i)
	{
		for (const TSharedPtr<FJsonObject>& R : ToolResultsOf(Loaded[i]))
		{
			TestFalse(TEXT("recorded legacy results are not rewritten"),
				ContentOf(R).Contains(KismetAIToolCallJournal::OutcomeUnrecordedCode));
		}
	}

	// And the reducer, which also repairs pairing, still pairs the legacy batch.
	const FSessionSnapshot Snap = KismetAIJournalReducer::ReduceJsonl(Jsonl);
	TestEqual(TEXT("reducer keeps the completed legacy batch"), ToolResultsOf(MakeShared<FJsonValueObject>(KismetAIAgentCodec::ToAnthropicJson(Snap.Messages[2]))).Num(), 2);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Stop lands after the executor's pre-dispatch check, while the call is
// queued for the game thread, and the admission seam refuses it there.
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	// Stands in for the production dispatcher: BeforeAdmission runs where the
	// game-thread callback would start, then the real admission seam decides.
	class FCancelAtAdmissionInvoker final : public IKismetAIToolInvoker
	{
	public:
		virtual FKismetAIInvokeResult Invoke(const FKismetAIToolInvocation& Invocation) override
		{
			if (BeforeAdmission) { BeforeAdmission(); }
			if (const TOptional<FToolExecutionResult> Refused = Invocation.RunAdmission())
			{
				return FKismetAIInvokeResult::FromTyped(Refused.GetValue());
			}
			++HandlerRuns;
			return FKismetAIInvokeResult::FromTyped(MutationSuccessResult());
		}
		virtual TSet<FString> GetDispatchableToolNames() const override { return { TEXT("edit_blueprint") }; }
		virtual FString ResolveHandlerName(const FString& ToolName, const FString& Mode) const override
		{
			return ToolName == TEXT("edit_blueprint") ? TEXT("Scripted") : FString();
		}

		TFunction<void()> BeforeAdmission;
		int32 HandlerRuns = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FToolJournal_CancelAtAdmissionIsNotStarted,
	"KismetAI.ToolCallJournal.CancelAtAdmissionIsNotStarted", ToolJournalTestFlags)
bool FToolJournal_CancelAtAdmissionIsNotStarted::RunTest(const FString& Parameters)
{
	FScriptedModelClient Model({ MakeTwoMutationTurn(), MakeTextTurn(TEXT("never reached")) });
	FCancelAtAdmissionInvoker Tools;
	FEventRecorder Rec;

	FKismetAIAgent Agent(Tools, Model, MakeJournalTestConfig());
	FSessionCleanup Cleanup;
	Tools.BeforeAdmission = [&Agent]() { Agent.Abort(); };

	Agent.RunTask(TEXT("edit two nodes"), Rec.MakeCallback());
	Cleanup.SessionId = Agent.GetSessionId();

	TestEqual(TEXT("the handler never ran"), Tools.HandlerRuns, 0);

	const TArray<TSharedPtr<FJsonObject>> Entries = FKismetAISessionStorage::LoadAllEntries(Cleanup.SessionId);
	const TArray<TSharedPtr<FJsonObject>> RecA = CallRecordsFor(Entries, TEXT("toolu_A"));
	const TArray<TSharedPtr<FJsonObject>> RecB = CallRecordsFor(Entries, TEXT("toolu_B"));
	TestEqual(TEXT("exactly one terminal record for A"), RecA.Num(), 1);
	TestEqual(TEXT("exactly one terminal record for B"), RecB.Num(), 1);
	if (RecA.Num() == 1)
	{
		TestTrue(TEXT("A refused at admission is not started, not failed"), StringFieldIs(RecA[0], TEXT("status"), TEXT("not_started")));
		TestFalse(TEXT("A was not attempted"), RecA[0]->GetBoolField(TEXT("attempted")));
	}
	if (RecB.Num() == 1)
	{
		TestTrue(TEXT("B not started"), StringFieldIs(RecB[0], TEXT("status"), TEXT("not_started")));
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Stop arrives while A's handler is running. A is not preempted; its
// real outcome is journaled as attempted, with the Stop noted, and B is not
// started.
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	class FStopWhileRunningInvoker final : public IKismetAIToolInvoker
	{
	public:
		virtual FKismetAIInvokeResult Invoke(const FKismetAIToolInvocation& Invocation) override
		{
			if (const TOptional<FToolExecutionResult> Refused = Invocation.RunAdmission())
			{
				return FKismetAIInvokeResult::FromTyped(Refused.GetValue());
			}
			++HandlerRuns;
			if (DuringHandler) { DuringHandler(); }
			FKismetAIInvokeResult R = FKismetAIInvokeResult::FromTyped(MutationSuccessResult());
			R.bCancelRequestedWhileRunning = Invocation.CancelToken.IsCancelled();
			return R;
		}
		virtual TSet<FString> GetDispatchableToolNames() const override { return { TEXT("edit_blueprint") }; }
		virtual FString ResolveHandlerName(const FString& ToolName, const FString& Mode) const override
		{
			return ToolName == TEXT("edit_blueprint") ? TEXT("Scripted") : FString();
		}

		TFunction<void()> DuringHandler;
		int32 HandlerRuns = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FToolJournal_StopWhileRunningKeepsAppliedOutcome,
	"KismetAI.ToolCallJournal.StopWhileRunningKeepsAppliedOutcome", ToolJournalTestFlags)
bool FToolJournal_StopWhileRunningKeepsAppliedOutcome::RunTest(const FString& Parameters)
{
	FScriptedModelClient Model({ MakeTwoMutationTurn(), MakeTextTurn(TEXT("never reached")) });
	FStopWhileRunningInvoker Tools;
	FEventRecorder Rec;

	FKismetAIAgent Agent(Tools, Model, MakeJournalTestConfig());
	FSessionCleanup Cleanup;
	// Stop twice, mid-handler.
	Tools.DuringHandler = [&Agent]() { Agent.Abort(); Agent.Abort(); };

	Agent.RunTask(TEXT("edit two nodes"), Rec.MakeCallback());
	Cleanup.SessionId = Agent.GetSessionId();

	TestEqual(TEXT("only A's handler ran"), Tools.HandlerRuns, 1);
	TestEqual(TEXT("one terminal done event"), Rec.Count(TEXT("done")), 1);

	const TArray<TSharedPtr<FJsonObject>> Entries = FKismetAISessionStorage::LoadAllEntries(Cleanup.SessionId);
	const TArray<TSharedPtr<FJsonObject>> RecA = CallRecordsFor(Entries, TEXT("toolu_A"));
	const TArray<TSharedPtr<FJsonObject>> RecB = CallRecordsFor(Entries, TEXT("toolu_B"));
	TestEqual(TEXT("exactly one terminal record for A"), RecA.Num(), 1);
	TestEqual(TEXT("exactly one terminal record for B"), RecB.Num(), 1);
	if (RecA.Num() == 1)
	{
		TestTrue(TEXT("A succeeded"), StringFieldIs(RecA[0], TEXT("status"), TEXT("succeeded")));
		TestTrue(TEXT("A was attempted"), RecA[0]->GetBoolField(TEXT("attempted")));
		bool bStopNoted = false;
		TestTrue(TEXT("A notes the Stop"), RecA[0]->TryGetBoolField(TEXT("cancel_requested_while_running"), bStopNoted) && bStopNoted);
		TestTrue(TEXT("A keeps its outcome"), RecA[0]->HasField(TEXT("mutation_outcome")));
		const TArray<TSharedPtr<FJsonValue>>& Res = RecA[0]->GetArrayField(TEXT("results"));
		const FString Content = Res.Num() == 1 ? ContentOf(Res[0]->AsObject()) : FString();
		TestTrue(TEXT("A keeps its real result"), Content.Contains(TEXT("A_REAL_RESULT")));
		TestFalse(TEXT("A is never described as not executed"), Content.Contains(KismetAIToolCallJournal::NotExecutedCode));
	}
	if (RecB.Num() == 1)
	{
		TestTrue(TEXT("B not started"), StringFieldIs(RecB[0], TEXT("status"), TEXT("not_started")));
		TestFalse(TEXT("B has no Stop-while-running note"), RecB[0]->HasField(TEXT("cancel_requested_while_running")));
	}

	const FString* ResultEvent = Rec.FirstJsonOfType(TEXT("tool_result"));
	TestTrue(TEXT("the UI event says Stop arrived during A"),
		ResultEvent && ResultEvent->Contains(TEXT("\"cancel_requested_while_running\":true")));
	return true;
}
