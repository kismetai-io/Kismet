// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Reconcile uncertain writes on resume.
//
// The crash window documents: a mutating tool changes the asset, then the
// process dies before the call's outcome record reaches the journal. Reload
// projects OUTCOME_UNRECORDED for that call. These tests drive the real resume
// entry point (FKismetAIAgent::RunTask over a journal file read by the
// production loader) with a scripted model that does what a model plausibly
// does when told "outcome unrecorded": it sends the same edit again. Asset
// state comes from the scripted invoker; KismetAI.CheckpointRecovery covers the
// editor's real fingerprints.

#include "Fakes/ScriptedAgentSeams.h"
#include "KismetAIJournalReducer.h"
#include "KismetAIMutationIntent.h"
#include "KismetAISessionStorage.h"
#include "KismetAIToolCallJournal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

using namespace KismetAITestFakes;

constexpr auto MutationReconciliationTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace MutationReconciliationTest
{
	FKismetAIAgentConfig MakeConfig()
	{
		FKismetAIAgentConfig C;
		C.ApiKey = TEXT("test-key-unused");
		C.Model = TEXT("claude-sonnet-4-6");
		C.MaxSteps = 4;
		C.MaxRetries = 0;
		C.RetryJitterFactor = 0.0;
		return C;
	}

	struct FJournalFile
	{
		FString SessionId;
		explicit FJournalFile(const FString& Jsonl)
		{
			SessionId = FKismetAISessionStorage::CreateSessionId();
			FFileHelper::SaveStringToFile(Jsonl, *FKismetAISessionStorage::GetSessionFilePath(SessionId),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}
		~FJournalFile() { FKismetAISessionStorage::DeleteSession(SessionId); }
	};

	const TCHAR* BlueprintPath() { return TEXT("/Game/__KismetAITest_P111/BP_Hero"); }

	TSharedPtr<FJsonObject> InsertArgs()
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("blueprint"), BlueprintPath());
		A->SetStringField(TEXT("node"), TEXT("Print String"));
		return A;
	}

	FString Line(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), W);
		return Out + TEXT("\n");
	}

	FKismetAIAssetFingerprint BeforeState()
	{
		return MakeFingerprint(BlueprintPath(), TEXT("D0"), { { TEXT("BP_Hero"), TEXT("b1") }, { TEXT("BP_Hero:EventGraph"), TEXT("g1") } }, TEXT("F0"));
	}

	// The insert landed: one new node object, the graph changed, the autosave
	// rewrote the file.
	FKismetAIAssetFingerprint AfterInsertState()
	{
		return MakeFingerprint(BlueprintPath(), TEXT("D1"), { { TEXT("BP_Hero"), TEXT("b1") }, { TEXT("BP_Hero:EventGraph"), TEXT("g2") },
			{ TEXT("BP_Hero:EventGraph.K2Node_CallFunction_7"), TEXT("n1") } }, TEXT("F1"));
	}

	// [user, assistant(insert_node stamped), mutation_intent?, <nothing>]: the
	// editor reached the call and was killed before its outcome was written.
	FString CrashJournal(bool bWithIntent)
	{
		FString Jsonl =
			TEXT("{\"type\":\"session_start\",\"v\":1,\"model\":\"claude-sonnet-4-6\"}\n")
			TEXT("{\"type\":\"user_message\",\"uuid\":\"u1\",\"content\":\"Add a Print String to BP_Hero.\"}\n")
			TEXT("{\"type\":\"assistant_message\",\"uuid\":\"a1\",\"run_id\":\"r\",\"turn_id\":\"r/t0\",\"tool_call_count\":1,\"content\":[")
				TEXT("{\"type\":\"tool_use\",\"id\":\"toolu_I\",\"name\":\"insert_node\",\"input\":{\"blueprint\":\"/Game/__KismetAITest_P111/BP_Hero\",\"node\":\"Print String\"}}]}\n");
		if (bWithIntent)
		{
			Jsonl += Line(KismetAIMutationIntent::BuildIntentEntry(TEXT("r"), TEXT("r/t0"), 0, TEXT("toolu_I"), TEXT("insert_node"),
				KismetAIMutationIntent::CanonicalArgs(InsertArgs()), { BeforeState() }, {}));
		}
		return Jsonl;
	}

	int32 CountEntries(const FString& SessionId, const TCHAR* Type)
	{
		int32 N = 0;
		for (const TSharedPtr<FJsonObject>& E : FKismetAISessionStorage::LoadAllEntries(SessionId))
		{
			FString T;
			if (E.IsValid() && E->TryGetStringField(TEXT("type"), T) && T == Type) { ++N; }
		}
		return N;
	}

	// The text of the tool_result answering ToolUseId in a captured request.
	FString ResultFor(const TArray<TSharedPtr<FJsonValue>>& Messages, const FString& ToolUseId)
	{
		for (const TSharedPtr<FJsonValue>& M : Messages)
		{
			const TSharedPtr<FJsonObject> O = M.IsValid() ? M->AsObject() : nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
			if (!O.IsValid() || !O->TryGetArrayField(TEXT("content"), Content)) continue;
			for (const TSharedPtr<FJsonValue>& B : *Content)
			{
				const TSharedPtr<FJsonObject> BO = B.IsValid() ? B->AsObject() : nullptr;
				FString Type, Id, Text;
				if (BO.IsValid() && BO->TryGetStringField(TEXT("type"), Type) && Type == TEXT("tool_result")
					&& BO->TryGetStringField(TEXT("tool_use_id"), Id) && Id == ToolUseId)
				{
					BO->TryGetStringField(TEXT("content"), Text);
					return Text;
				}
			}
		}
		return FString();
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Verdicts, one asset at a time.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationReconciliation_Verdicts,
	"KismetAI.MutationReconciliation.Verdicts", MutationReconciliationTestFlags)
bool FMutationReconciliation_Verdicts::RunTest(const FString& Parameters)
{
	using namespace MutationReconciliationTest;
	using namespace KismetAIMutationIntent;
	const FKismetAIAssetFingerprint Before = BeforeState();

	{
		const FKismetAIAssetReconciliation R = ReconcileAsset(Before, Before);
		TestTrue(TEXT("same memory and file: not applied"), R.Verdict == EKismetAIReconcileVerdict::NotApplied);
		TestTrue(TEXT("memory compared"), R.MemoryMatchesBefore.IsSet() && R.MemoryMatchesBefore.GetValue());
		TestTrue(TEXT("disk compared"), R.DiskMatchesBefore.IsSet() && R.DiskMatchesBefore.GetValue());
	}
	{
		const FKismetAIAssetReconciliation R = ReconcileAsset(Before, AfterInsertState());
		TestTrue(TEXT("changed graph: changed_since_intent"), R.Verdict == EKismetAIReconcileVerdict::ChangedSinceIntent);
		TestTrue(TEXT("the new node is named as evidence"),
			R.Differences.ContainsByPredicate([](const FString& D) { return D == TEXT("added: BP_Hero:EventGraph.K2Node_CallFunction_7"); }));
		TestTrue(TEXT("the changed graph is named as evidence"),
			R.Differences.ContainsByPredicate([](const FString& D) { return D == TEXT("changed: BP_Hero:EventGraph"); }));
	}
	{
		// Memory equal, file rewritten underneath: still not "not applied".
		FKismetAIAssetFingerprint Now = Before;
		Now.FileSha1 = TEXT("F9");
		const FKismetAIAssetReconciliation R = ReconcileAsset(Before, Now);
		TestTrue(TEXT("file changed alone: changed_since_intent"), R.Verdict == EKismetAIReconcileVerdict::ChangedSinceIntent);
		TestTrue(TEXT("disk mismatch reported"), R.DiskMatchesBefore.IsSet() && !R.DiskMatchesBefore.GetValue());
	}
	{
		FKismetAIAssetFingerprint Gone;
		Gone.AssetPath = Before.AssetPath;
		Gone.bCaptured = true;
		const FKismetAIAssetReconciliation R = ReconcileAsset(Before, Gone);
		TestTrue(TEXT("deleted: asset_missing"), R.Verdict == EKismetAIReconcileVerdict::AssetMissing);
	}
	{
		FKismetAIAssetFingerprint Absent;
		Absent.AssetPath = Before.AssetPath;
		Absent.bCaptured = true;
		TestTrue(TEXT("create target now present: changed"), ReconcileAsset(Absent, Before).Verdict == EKismetAIReconcileVerdict::ChangedSinceIntent);
		TestTrue(TEXT("create target still absent: not applied"), ReconcileAsset(Absent, Absent).Verdict == EKismetAIReconcileVerdict::NotApplied);
	}
	{
		// An invoker that cannot look is never evidence of "not applied", even
		// when both sides "look" identical.
		FKismetAIAssetFingerprint Blind;
		Blind.AssetPath = Before.AssetPath;
		TestTrue(TEXT("uncaptured before: no evidence"), ReconcileAsset(Blind, Blind).Verdict == EKismetAIReconcileVerdict::NoEvidence);
		TestTrue(TEXT("uncaptured now: no evidence"), ReconcileAsset(Before, Blind).Verdict == EKismetAIReconcileVerdict::NoEvidence);
		FKismetAIAssetFingerprint Unsupported = Before;
		Unsupported.bSupported = false;
		Unsupported.Digest.Reset();
		Unsupported.UnsupportedReason = TEXT("unsupported class");
		const FKismetAIAssetReconciliation R = ReconcileAsset(Unsupported, Unsupported);
		TestTrue(TEXT("unsupported class: no evidence"), R.Verdict == EKismetAIReconcileVerdict::NoEvidence);
		TestEqual(TEXT("the reason is carried"), R.Detail, FString(TEXT("unsupported class")));
	}
	{
		// The call takes its worst asset; an unrecorded call with no intent has no evidence.
		FKismetAIUncertainCall Call;
		Call.ToolName = TEXT("insert_node");
		TestTrue(TEXT("no intent: no evidence"), Reconcile(Call, {}).Verdict == EKismetAIReconcileVerdict::NoEvidence);
		TestTrue(TEXT("no evidence blocks replay"), Reconcile(Call, {}).BlocksReplay());
	}

	// Round trip of the journal record.
	FKismetAIUncertainCall Call;
	Call.RunId = TEXT("r"); Call.TurnId = TEXT("r/t0"); Call.CallId = TEXT("r/t0/c0"); Call.ToolUseId = TEXT("toolu_I");
	Call.ToolName = TEXT("insert_node"); Call.ArgsCanonical = CanonicalArgs(InsertArgs());
	Call.Intent = BuildIntentEntry(TEXT("r"), TEXT("r/t0"), 0, TEXT("toolu_I"), TEXT("insert_node"), Call.ArgsCanonical, { BeforeState() }, {});
	const FKismetAIReconciliation R = Reconcile(Call, { AfterInsertState() });
	FKismetAIReconciliation Back;
	TestTrue(TEXT("reconciliation entry reads back"), FKismetAIReconciliation::FromEntry(R.ToEntry(), Back));
	TestTrue(TEXT("verdict kept"), Back.Verdict == EKismetAIReconcileVerdict::ChangedSinceIntent);
	TestEqual(TEXT("call id kept"), Back.CallId, FString(TEXT("r/t0/c0")));
	TestEqual(TEXT("args kept"), Back.ArgsCanonical, Call.ArgsCanonical);
	TestEqual(TEXT("asset evidence kept"), Back.Assets.Num(), 1);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Adversarial: interrupt after a node is applied but before the outcome append.
// The resumed model sends the identical insert_node again; it must not run.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationReconciliation_CrashAfterApplyIsNotReplayed,
	"KismetAI.MutationReconciliation.CrashAfterApplyIsNotReplayed", MutationReconciliationTestFlags)
bool FMutationReconciliation_CrashAfterApplyIsNotReplayed::RunTest(const FString& Parameters)
{
	using namespace MutationReconciliationTest;
	FJournalFile File(CrashJournal(/*bWithIntent=*/true));

	{
		const FSessionSnapshot Snap = FKismetAISessionStorage::LoadSession(File.SessionId);
		TestEqual(TEXT("the reader finds one uncertain mutation"), Snap.UncertainCalls.Num(), 1);
		if (Snap.UncertainCalls.Num() == 1)
		{
			TestEqual(TEXT("identified by call id"), Snap.UncertainCalls[0].CallId, FString(TEXT("r/t0/c0")));
			TestTrue(TEXT("with its intent"), Snap.UncertainCalls[0].Intent.IsValid());
			TestFalse(TEXT("not yet reconciled"), Snap.UncertainCalls[0].Reconciliation.IsValid());
		}
	}

	FScriptedModelClient Model({
		MakeToolTurn(TEXT("toolu_again"), TEXT("insert_node"), InsertArgs()),
		MakeTextTurn(TEXT("Done.")) });
	FScriptedToolInvoker Tools;
	Tools.AssetStates.Add(BlueprintPath(), AfterInsertState());
	FKismetAIAgent Agent(Tools, Model, MakeConfig());
	FEventRecorder Events;
	Agent.RunTask(TEXT("Continue."), Events.MakeCallback(), TEXT(""), File.SessionId);

	TestEqual(TEXT("the uncertain insert is not executed a second time"), Tools.CountCalls(TEXT("insert_node")), 0);
	TestEqual(TEXT("the asset was looked at once"), Tools.CaptureLog.Num(), 1);
	TestEqual(TEXT("the reconciliation is announced"), Events.Count(KismetAIMutationIntent::ReconciliationEntryType), 1);
	if (const FString* Json = Events.FirstJsonOfType(KismetAIMutationIntent::ReconciliationEntryType))
	{
		TestTrue(TEXT("event names the verdict"), Json->Contains(TEXT("\"verdict\":\"changed_since_intent\"")));
		TestTrue(TEXT("event names the asset"), Json->Contains(BlueprintPath()));
		TestTrue(TEXT("event says replay is blocked"), Json->Contains(TEXT("\"replay_blocked\":true")));
	}
	TestEqual(TEXT("the reconciliation is journaled once"), CountEntries(File.SessionId, KismetAIMutationIntent::ReconciliationEntryType), 1);

	if (Model.NumCalls() >= 1)
	{
		const FString Uncertain = ResultFor(Model.GetRequests()[0].Messages, TEXT("toolu_I"));
		TestTrue(TEXT("still reported as unrecorded"), Uncertain.Contains(KismetAIToolCallJournal::OutcomeUnrecordedCode));
		TestTrue(TEXT("the model sees the verdict"), Uncertain.Contains(TEXT("changed_since_intent")));
		TestTrue(TEXT("the model sees the evidence"), Uncertain.Contains(TEXT("added: BP_Hero:EventGraph.K2Node_CallFunction_7")));
		const FString* RefusalEvent = Events.FirstJsonOfType(TEXT("tool_result"));
		const FString Refused = RefusalEvent ? *RefusalEvent : FString();
		TestTrue(TEXT("the repeat is refused with RECONCILIATION_REQUIRED"), Refused.Contains(KismetAIMutationIntent::ReconciliationRequiredCode));
	}
	else
	{
		AddError(TEXT("expected the initial resumed model request"));
	}

	// A second resume reuses the first verdict: the asset is not re-judged
	// (later states are not evidence about this call) and the guard holds.
	FScriptedModelClient Model2({
		MakeToolTurn(TEXT("toolu_third"), TEXT("insert_node"), InsertArgs()),
		MakeTextTurn(TEXT("Done.")) });
	FScriptedToolInvoker Tools2;
	FKismetAIAgent Agent2(Tools2, Model2, MakeConfig());
	Agent2.RunTask(TEXT("Try again."), FOnAgentStep(), TEXT(""), File.SessionId);
	TestEqual(TEXT("still not executed on a later resume"), Tools2.CountCalls(TEXT("insert_node")), 0);
	TestEqual(TEXT("not reconciled again"), Tools2.CaptureLog.Num(), 0);
	TestEqual(TEXT("still one reconciliation record"), CountEntries(File.SessionId, KismetAIMutationIntent::ReconciliationEntryType), 1);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// A journal without an intent (older build, or a crash before B2): no evidence,
// and the repeat is refused too.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationReconciliation_NoIntentIsNoEvidence,
	"KismetAI.MutationReconciliation.NoIntentIsNoEvidence", MutationReconciliationTestFlags)
bool FMutationReconciliation_NoIntentIsNoEvidence::RunTest(const FString& Parameters)
{
	using namespace MutationReconciliationTest;
	FJournalFile File(CrashJournal(/*bWithIntent=*/false));

	FScriptedModelClient Model({
		MakeToolTurn(TEXT("toolu_again"), TEXT("insert_node"), InsertArgs()),
		MakeTextTurn(TEXT("Done.")) });
	FScriptedToolInvoker Tools;
	Tools.AssetStates.Add(BlueprintPath(), BeforeState());
	FKismetAIAgent Agent(Tools, Model, MakeConfig());
	FEventRecorder Events;
	Agent.RunTask(TEXT("Continue."), Events.MakeCallback(), TEXT(""), File.SessionId);

	TestEqual(TEXT("not executed"), Tools.CountCalls(TEXT("insert_node")), 0);
	TestEqual(TEXT("nothing to compare, so nothing captured"), Tools.CaptureLog.Num(), 0);
	if (const FString* Json = Events.FirstJsonOfType(KismetAIMutationIntent::ReconciliationEntryType))
	{
		TestTrue(TEXT("no_evidence"), Json->Contains(TEXT("\"verdict\":\"no_evidence\"")));
	}
	else
	{
		AddError(TEXT("no reconciliation event"));
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Interrupted after the intent but before the edit landed: the asset equals its
// before-state. The verdict says so; the call is still never redone
// automatically, but the model may redo it deliberately.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationReconciliation_UnappliedMayBeRedoneByChoice,
	"KismetAI.MutationReconciliation.UnappliedMayBeRedoneByChoice", MutationReconciliationTestFlags)
bool FMutationReconciliation_UnappliedMayBeRedoneByChoice::RunTest(const FString& Parameters)
{
	using namespace MutationReconciliationTest;
	FJournalFile File(CrashJournal(/*bWithIntent=*/true));

	{
		// Resume with a model that only answers: nothing runs on its own.
		FScriptedModelClient Model({ MakeTextTurn(TEXT("The insert did not land.")) });
		FScriptedToolInvoker Tools;
		Tools.AssetStates.Add(BlueprintPath(), BeforeState());
		FKismetAIAgent Agent(Tools, Model, MakeConfig());
		Agent.RunTask(TEXT("What happened?"), FOnAgentStep(), TEXT(""), File.SessionId);
		TestEqual(TEXT("nothing is replayed automatically"), Tools.CallLog.Num(), 0);
		if (Model.NumCalls() >= 1)
		{
			const FString Uncertain = ResultFor(Model.GetRequests()[0].Messages, TEXT("toolu_I"));
			TestTrue(TEXT("the model sees not_applied"), Uncertain.Contains(TEXT("not_applied")));
		}
	}
	{
		FScriptedModelClient Model({
			MakeToolTurn(TEXT("toolu_redo"), TEXT("insert_node"), InsertArgs()),
			MakeTextTurn(TEXT("Done.")) });
		FScriptedToolInvoker Tools;
		Tools.AssetStates.Add(BlueprintPath(), BeforeState());
		FKismetAIAgent Agent(Tools, Model, MakeConfig());
		Agent.RunTask(TEXT("Add it."), FOnAgentStep(), TEXT(""), File.SessionId);
		TestEqual(TEXT("the model's deliberate redo runs once"), Tools.CountCalls(TEXT("insert_node")), 1);
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// The live side: a mutating call journals its before-state between the stamped
// batch and its outcome record, and the editor hooks bracket the call.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationReconciliation_IntentPrecedesTheCall,
	"KismetAI.MutationReconciliation.IntentPrecedesTheCall", MutationReconciliationTestFlags)
bool FMutationReconciliation_IntentPrecedesTheCall::RunTest(const FString& Parameters)
{
	using namespace MutationReconciliationTest;
	FScriptedModelClient Model({
		MakeToolTurn(TEXT("toolu_1"), TEXT("insert_node"), InsertArgs()),
		MakeTextTurn(TEXT("Done.")) });
	FScriptedToolInvoker Tools;
	Tools.AssetStates.Add(BlueprintPath(), BeforeState());
	TArray<FString> Order;
	Tools.OnInvoke = [&Order](const FString& Tool) { Order.Add(TEXT("invoke:") + Tool); };
	FKismetAIAgent Agent(Tools, Model, MakeConfig());
	Agent.SetJournalAppendHookForTests([&Order](const TSharedPtr<FJsonObject>& Entry)
	{
		FString Type;
		Entry->TryGetStringField(TEXT("type"), Type);
		Order.Add(TEXT("journal:") + Type);
		return true;
	});
	Agent.RunTask(TEXT("Add a Print String."), FOnAgentStep(), TEXT(""), TEXT(""));
	ON_SCOPE_EXIT { FKismetAISessionStorage::DeleteSession(Agent.GetSessionId()); };

	TestEqual(TEXT("ran once"), Tools.CountCalls(TEXT("insert_node")), 1);
	TestEqual(TEXT("one BeginMutation"), Tools.BeginLog.Num(), 1);
	TestEqual(TEXT("one EndMutation"), Tools.EndLog.Num(), 1);
	if (Tools.BeginLog.Num() == 1)
	{
		TestEqual(TEXT("target resolved from the call"), Tools.BeginLog[0].Targets, TArray<FString>{ BlueprintPath() });
		TestEqual(TEXT("session passed"), Tools.BeginLog[0].SessionId, Agent.GetSessionId());
		TestTrue(TEXT("call id passed"), Tools.BeginLog[0].CallId.EndsWith(TEXT("/t0/c0")));
	}
	const int32 Intent = Order.IndexOfByKey(FString(TEXT("journal:mutation_intent")));
	const int32 Invoke = Order.IndexOfByKey(FString(TEXT("invoke:insert_node")));
	const int32 Batch = Order.IndexOfByKey(FString(TEXT("journal:assistant_message")));
	int32 Outcome = INDEX_NONE;
	for (int32 i = Order.Num() - 1; i >= 0; --i) { if (Order[i] == TEXT("journal:tool_result_message")) { Outcome = i; break; } }
	TestTrue(TEXT("batch, then intent, then the call, then its outcome"),
		Batch != INDEX_NONE && Batch < Intent && Intent < Invoke && Invoke < Outcome);

	for (const TSharedPtr<FJsonObject>& E : FKismetAISessionStorage::LoadAllEntries(Agent.GetSessionId()))
	{
		FString Type;
		if (E->TryGetStringField(TEXT("type"), Type) && Type == KismetAIMutationIntent::IntentEntryType)
		{
			TArray<FKismetAIAssetFingerprint> Targets;
			TArray<FString> Unresolved;
			TestTrue(TEXT("intent reads back"), KismetAIMutationIntent::ReadIntentTargets(E, Targets, Unresolved));
			TestTrue(TEXT("records the before-state"), Targets.Num() == 1 && Targets[0].Digest == TEXT("D0"));
			TestEqual(TEXT("records the call's arguments"), E->GetStringField(TEXT("args_canonical")), KismetAIMutationIntent::CanonicalArgs(InsertArgs()));
		}
	}

	// A completed call is recorded: nothing is uncertain on reload.
	TestEqual(TEXT("no uncertain calls after a clean run"), FKismetAISessionStorage::LoadSession(Agent.GetSessionId()).UncertainCalls.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationReconciliation_UnwritableIntentStopsTheCall,
	"KismetAI.MutationReconciliation.UnwritableIntentStopsTheCall", MutationReconciliationTestFlags)
bool FMutationReconciliation_UnwritableIntentStopsTheCall::RunTest(const FString& Parameters)
{
	using namespace MutationReconciliationTest;
	FScriptedModelClient Model({
		MakeToolTurn(TEXT("toolu_1"), TEXT("insert_node"), InsertArgs()),
		MakeTextTurn(TEXT("Done.")) });
	FScriptedToolInvoker Tools;
	FKismetAIAgent Agent(Tools, Model, MakeConfig());
	Agent.SetJournalAppendHookForTests([](const TSharedPtr<FJsonObject>& Entry)
	{
		FString Type;
		Entry->TryGetStringField(TEXT("type"), Type);
		return Type != KismetAIMutationIntent::IntentEntryType;
	});
	Agent.RunTask(TEXT("Add a Print String."), FOnAgentStep(), TEXT(""), TEXT(""));
	ON_SCOPE_EXIT { FKismetAISessionStorage::DeleteSession(Agent.GetSessionId()); };

	TestEqual(TEXT("the call does not run without its intent"), Tools.CountCalls(TEXT("insert_node")), 0);
	bool bNotStarted = false;
	for (const TSharedPtr<FJsonObject>& E : FKismetAISessionStorage::LoadAllEntries(Agent.GetSessionId()))
	{
		FString Record, Status;
		if (E->TryGetStringField(TEXT("record"), Record) && Record == KismetAIToolCallJournal::RecordToolCall
			&& E->TryGetStringField(TEXT("status"), Status))
		{
			bNotStarted |= Status == TEXT("not_started");
		}
	}
	TestTrue(TEXT("its record says it never started"), bNotStarted);
	if (Model.NumCalls() >= 2)
	{
		TestTrue(TEXT("the model is told why"), ResultFor(Model.GetRequests()[1].Messages, TEXT("toolu_1")).Contains(TEXT("JOURNAL_WRITE_FAILED")));
	}
	return true;
}

// A read with no recorded outcome changed nothing: it is not reconciled.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationReconciliation_ReadsAreNotUncertain,
	"KismetAI.MutationReconciliation.ReadsAreNotUncertain", MutationReconciliationTestFlags)
bool FMutationReconciliation_ReadsAreNotUncertain::RunTest(const FString& Parameters)
{
	using namespace MutationReconciliationTest;
	FJournalFile File(
		TEXT("{\"type\":\"session_start\",\"v\":1,\"model\":\"claude-sonnet-4-6\"}\n")
		TEXT("{\"type\":\"user_message\",\"uuid\":\"u1\",\"content\":\"Look.\"}\n")
		TEXT("{\"type\":\"assistant_message\",\"uuid\":\"a1\",\"run_id\":\"r\",\"turn_id\":\"r/t0\",\"tool_call_count\":1,\"content\":[")
			TEXT("{\"type\":\"tool_use\",\"id\":\"toolu_R\",\"name\":\"read_blueprint\",\"input\":{\"blueprint\":\"/Game/X/BP\"}}]}\n"));
	const FSessionSnapshot Snap = FKismetAISessionStorage::LoadSession(File.SessionId);
	TestEqual(TEXT("unrecorded"), Snap.UnrecordedCalls, 1);
	TestEqual(TEXT("but not an uncertain mutation"), Snap.UncertainCalls.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationReconciliation_SemanticReplay, "KismetAI.MutationReconciliation.SemanticReplayIdentity", MutationReconciliationTestFlags)
bool FMutationReconciliation_SemanticReplay::RunTest(const FString& Parameters)
{
	const FString First = TEXT("{\"blueprint\":\"/Game/A.A\",\"node_class\":\"PrintString\",\"expected_revision\":\"old\"}");
	const FString Repeat = TEXT("{\"expected_revision\":\"new\",\"node_class\":\"PrintString\",\"blueprint\":\"/Game/A\"}");
	TestEqual(TEXT("cosmetic changes and fresh expectations cannot bypass uncertain replay"),
		KismetAIMutationIntent::ReplayKey(TEXT("insert_node"), First), KismetAIMutationIntent::ReplayKey(TEXT("insert_node"), Repeat));
	const FString Different = TEXT("{\"blueprint\":\"/Game/A\",\"node_class\":\"Branch\"}");
	TestNotEqual(TEXT("different operands remain distinct"), KismetAIMutationIntent::ReplayKey(TEXT("insert_node"), First), KismetAIMutationIntent::ReplayKey(TEXT("insert_node"), Different));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationReconciliation_SourceAlias, "KismetAI.MutationReconciliation.SourceAliasReplayIdentity", MutationReconciliationTestFlags)
bool FMutationReconciliation_SourceAlias::RunTest(const FString& Parameters)
{
	const FString First = TEXT("{\"action\":\"duplicate\",\"asset_path\":\"/Game/Source.Source\",\"new_path\":\"/Game/Copy\"}");
	const FString Alias = TEXT("{\"new_path\":\"/Game/Copy\",\"asset_path\":\"/Game/Source\",\"action\":\"duplicate\"}");
	TestEqual(TEXT("source aliases cannot bypass uncertain duplication barrier"),
		KismetAIMutationIntent::ReplayKey(TEXT("manage_asset"), First), KismetAIMutationIntent::ReplayKey(TEXT("manage_asset"), Alias));
	const FString Other = Alias.Replace(TEXT("/Game/Source"), TEXT("/Game/Other"));
	TestNotEqual(TEXT("different source operands remain distinct"),
		KismetAIMutationIntent::ReplayKey(TEXT("manage_asset"), First), KismetAIMutationIntent::ReplayKey(TEXT("manage_asset"), Other));
	const FString Target = TEXT("{\"blueprint\":\"/Game/A.A\",\"node_class\":\"PrintString\"}");
	for (const TCHAR* OtherTarget : {TEXT("/Game/A.Other"), TEXT("/Game/A.A:Subobject")})
	{
		const FString DifferentTarget = Target.Replace(TEXT("/Game/A.A"), OtherTarget);
		TestNotEqual(TEXT("different object and subobject targets remain distinct"),
			KismetAIMutationIntent::ReplayKey(TEXT("insert_node"), Target), KismetAIMutationIntent::ReplayKey(TEXT("insert_node"), DifferentTarget));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationReconciliation_UnknownEffectSameRun,
	"KismetAI.MutationReconciliation.UnknownEffectBlocksChangedStateReplay", MutationReconciliationTestFlags)
bool FMutationReconciliation_UnknownEffectSameRun::RunTest(const FString& Parameters)
{
	using namespace MutationReconciliationTest;
	const auto Args = InsertArgs();
	auto Read = MakeShared<FJsonObject>();
	Read->SetStringField(TEXT("blueprint"), BlueprintPath());
	FScriptedModelClient Model({ MakeToolTurn(TEXT("u1"), TEXT("insert_node"), Args),
		MakeToolTurn(TEXT("read"), TEXT("read_blueprint"), Read),
		MakeToolTurn(TEXT("u2"), TEXT("insert_node"), Args), MakeTextTurn(TEXT("Done.")) });
	FScriptedToolInvoker Tools;
	Tools.SetResult(TEXT("insert_node"), TEXT("{\"success\":true}"));
	auto Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_revision"), TEXT("new-observation"));
	Data->SetStringField(TEXT("asset_revision_path"), BlueprintPath());
	Tools.SetTypedResult(TEXT("read_blueprint"), FToolExecutionResult::Success(Data));
	FKismetAIAgent Agent(Tools, Model, MakeConfig());
	FEventRecorder Events;
	Agent.RunTask(TEXT("Continue after a changed observation."), Events.MakeCallback());
	TestEqual(TEXT("unknown executed mutation must not replay despite changed state"), Tools.CountCalls(TEXT("insert_node")), 1);
	TestEqual(TEXT("stop before another provider request"), Model.NumCalls(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationReconciliation_UnknownEffectResume,
	"KismetAI.MutationReconciliation.UnknownEffectBarrierSurvivesResume", MutationReconciliationTestFlags)
bool FMutationReconciliation_UnknownEffectResume::RunTest(const FString& Parameters)
{
	using namespace MutationReconciliationTest;
	FScriptedModelClient Model({ MakeToolTurn(TEXT("u1"), TEXT("insert_node"), InsertArgs()), MakeTextTurn(TEXT("Done.")) });
	FScriptedToolInvoker Tools;
	Tools.SetResult(TEXT("insert_node"), TEXT("{\"success\":true}"));
	FKismetAIAgent First(Tools, Model, MakeConfig());
	First.RunTask(TEXT("Insert once."), FOnAgentStep());
	auto Read = MakeShared<FJsonObject>();
	Read->SetStringField(TEXT("blueprint"), BlueprintPath());
	FScriptedModelClient ResumeModel({ MakeToolTurn(TEXT("read"), TEXT("read_blueprint"), Read),
		MakeToolTurn(TEXT("u2"), TEXT("insert_node"), InsertArgs()), MakeTextTurn(TEXT("Done.")) });
	FScriptedToolInvoker ResumeTools;
	auto Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_revision"), TEXT("new-observation"));
	Data->SetStringField(TEXT("asset_revision_path"), BlueprintPath());
	ResumeTools.SetTypedResult(TEXT("read_blueprint"), FToolExecutionResult::Success(Data));
	FKismetAIAgent Resume(ResumeTools, ResumeModel, MakeConfig());
	Resume.RunTask(TEXT("Continue."), FOnAgentStep(), TEXT(""), First.GetSessionId());
	TestEqual(TEXT("unknown committed effect survives continuation"), ResumeTools.CountCalls(TEXT("insert_node")), 0);
	TestEqual(TEXT("resume stops on uncertain repeat"), ResumeModel.NumCalls(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationReconciliation_RevisionRefusalRetry,
	"KismetAI.MutationReconciliation.RevisionRefusalAllowsChangedStateRetry", MutationReconciliationTestFlags)
bool FMutationReconciliation_RevisionRefusalRetry::RunTest(const FString& Parameters)
{
	using namespace MutationReconciliationTest;
	auto Read = MakeShared<FJsonObject>();
	Read->SetStringField(TEXT("blueprint"), BlueprintPath());
	FScriptedModelClient Model({ MakeToolTurn(TEXT("u1"), TEXT("insert_node"), InsertArgs()),
		MakeToolTurn(TEXT("read"), TEXT("read_blueprint"), Read),
		MakeToolTurn(TEXT("u2"), TEXT("insert_node"), InsertArgs()), MakeTextTurn(TEXT("Stopped.")) });
	FScriptedToolInvoker Tools;
	Tools.SetTypedResult(TEXT("insert_node"), FToolExecutionResult::NotStartedError(409, TEXT("ASSET_REVISION_CONFLICT"), TEXT("Not applied.")));
	auto Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_revision"), TEXT("new-observation"));
	Data->SetStringField(TEXT("asset_revision_path"), BlueprintPath());
	Tools.SetTypedResult(TEXT("read_blueprint"), FToolExecutionResult::Success(Data));
	FKismetAIAgent Agent(Tools, Model, MakeConfig());
	Agent.RunTask(TEXT("Read current state and retry."), FOnAgentStep());
	TestEqual(TEXT("pre-handler revision refusal is not unknown execution"), Tools.CountCalls(TEXT("insert_node")), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationReconciliation_ErrorCodeNotEvidence,
	"KismetAI.MutationReconciliation.ErrorCodeDoesNotProveNoEffects", MutationReconciliationTestFlags)
bool FMutationReconciliation_ErrorCodeNotEvidence::RunTest(const FString& Parameters)
{
	using namespace MutationReconciliationTest;
	const auto Args = InsertArgs();
	auto Read = MakeShared<FJsonObject>();
	Read->SetStringField(TEXT("blueprint"), BlueprintPath());
	FScriptedModelClient Model({ MakeToolTurn(TEXT("u1"), TEXT("insert_node"), Args),
		MakeToolTurn(TEXT("read"), TEXT("read_blueprint"), Read),
		MakeToolTurn(TEXT("u2"), TEXT("insert_node"), Args), MakeTextTurn(TEXT("Done.")) });
	FScriptedToolInvoker Tools;
	Tools.SetTypedResult(TEXT("insert_node"), FToolExecutionResult::Error(409, TEXT("ASSET_REVISION_CONFLICT"), TEXT("handler error after possible effects")));
	auto Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_revision"), TEXT("new-observation"));
	Data->SetStringField(TEXT("asset_revision_path"), BlueprintPath());
	Tools.SetTypedResult(TEXT("read_blueprint"), FToolExecutionResult::Success(Data));
	FKismetAIAgent Agent(Tools, Model, MakeConfig());
	FEventRecorder Events;
	Agent.RunTask(TEXT("Continue after a changed observation."), Events.MakeCallback());
	TestEqual(TEXT("unknown executed mutation must not replay despite changed state"), Tools.CountCalls(TEXT("insert_node")), 1);
	TestEqual(TEXT("stop before another provider request"), Model.NumCalls(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationReconciliation_LocalRefusal,
	"KismetAI.MutationReconciliation.LocalValidationRefusalIsNotUncertainExecution", MutationReconciliationTestFlags)
bool FMutationReconciliation_LocalRefusal::RunTest(const FString& Parameters)
{
	using namespace MutationReconciliationTest;
	auto Invalid = MakeShared<FJsonObject>();
	Invalid->SetStringField(TEXT("blueprint"), BlueprintPath());
	FScriptedModelClient Model({ MakeToolTurn(TEXT("bad1"), TEXT("insert_node"), Invalid), MakeTextTurn(TEXT("Invalid input.")) });
	FScriptedToolInvoker Tools;
	FKismetAIAgent First(Tools, Model, MakeConfig());
	First.RunTask(TEXT("Invalid call."), FOnAgentStep());
	FScriptedModelClient ResumeModel({ MakeToolTurn(TEXT("bad2"), TEXT("insert_node"), Invalid), MakeTextTurn(TEXT("Invalid input.")) });
	FKismetAIAgent Resume(Tools, ResumeModel, MakeConfig());
	FEventRecorder Events;
	Resume.RunTask(TEXT("Continue."), Events.MakeCallback(), TEXT(""), First.GetSessionId());
	TestEqual(TEXT("validation never invoked handler"), Tools.CountCalls(TEXT("insert_node")), 0);
	TestEqual(TEXT("validation refusal does not create uncertain replay barrier"), ResumeModel.NumCalls(), 2);
	const FString* Result = Events.FirstJsonOfType(TEXT("tool_result"));
	TestTrue(TEXT("result remains validation rather than reconciliation"), Result && !Result->Contains(TEXT("RECONCILIATION_REQUIRED")));
	return true;
}
