// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Fakes/ScriptedAgentSeams.h"
#include "KismetAIMutationOutcome.h"
#include "KismetAISessionStorage.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformMisc.h"
#include "HAL/FileManager.h"
#include "Policies/CondensedJsonPrintPolicy.h"

using namespace KismetAITestFakes;

namespace KismetAICompletionEvidenceRunTest
{
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	FKismetAIAgentConfig Config()
	{
		FKismetAIAgentConfig C;
		C.ApiKey = TEXT("test-key-unused"); C.Model = TEXT("claude-sonnet-4-6");
		C.MaxSteps = 8; C.MaxRetries = 0; C.RetryJitterFactor = 0;
		C.ThinkingMode = EThinkingMode::Disabled;
		return C;
	}
	TSharedPtr<FJsonObject> Args(const TCHAR* Package = TEXT("/Game/A"))
	{
		auto Op = MakeShared<FJsonObject>();
		Op->SetStringField(TEXT("action"), TEXT("set_pin_default"));
		Op->SetStringField(TEXT("node"), TEXT("Node")); Op->SetStringField(TEXT("pin"), TEXT("InString"));
		Op->SetStringField(TEXT("value"), TEXT("changed"));
		auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("blueprint"), Package);
		A->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(Op) });
		return A;
	}
	FToolExecutionResult Evidence(const TCHAR* Package, EKismetAISaveStatus Save, EKismetAICompileStatus Compile = EKismetAICompileStatus::Succeeded)
	{
		FKismetAIMutationOutcome O;
		O.PackageName = Package; O.bAppliedInMemory = true; O.SaveStatus = Save; O.CompileStatus = Compile;
		if (Save == EKismetAISaveStatus::Failed) O.SaveError = TEXT("fixture: disk full");
		auto D = MakeShared<FJsonObject>(); D->SetObjectField(TEXT("outcome"), O.ToJson());
		return FToolExecutionResult::Success(D);
	}
	TSharedPtr<FJsonObject> Object(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Key)
	{
		const TSharedPtr<FJsonObject>* Value = nullptr;
		return Parent && Parent->TryGetObjectField(Key, Value) && Value ? *Value : nullptr;
	}
	FString Text(const TSharedPtr<FJsonObject>& J, const TCHAR* Key)
	{
		FString V; if (J) J->TryGetStringField(Key, V); return V;
	}
	FString Json(const TSharedPtr<FJsonObject>& J)
	{
		FString V; if (J) FJsonSerializer::Serialize(J.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&V)); return V;
	}
	TSharedPtr<FJsonObject> Terminal(const FEventRecorder& R)
	{
		const FString* S = R.FirstJsonOfType(TEXT("done")); if (!S) S = R.FirstJsonOfType(TEXT("error"));
		TSharedPtr<FJsonObject> J; if (S) FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(*S), J); return J;
	}
	void CaptureSample(FKismetAIAgent& Agent, const FEventRecorder& Recorder, const TCHAR* Name)
	{
		const FString Directory = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMET_COMPLETION_EVIDENCE_DIR"));
		if (Directory.IsEmpty()) return;
		IFileManager::Get().MakeDirectory(*Directory, true);
		auto Sample = MakeShared<FJsonObject>();
		Sample->SetStringField(TEXT("evidence_kind"), TEXT("production RunTask with scripted local provider and typed tool seam; no paid provider"));
		Sample->SetObjectField(TEXT("terminal"), Terminal(Recorder));
		TArray<TSharedPtr<FJsonValue>> Calls;
		for (const auto& Call : FKismetAISessionStorage::LoadSession(Agent.GetSessionId()).CommittedCalls)
			Calls.Add(MakeShared<FJsonValueObject>(Call));
		Sample->SetArrayField(TEXT("committed_calls"), Calls);
		FFileHelper::SaveStringToFile(Json(Sample), *(Directory / (FString(Name) + TEXT(".json"))));
	}
	bool CallRecord(const TSharedPtr<FJsonObject>& J) { return Text(J, TEXT("record")) == TEXT("tool_call"); }
	int32 MutationCount(const TSharedPtr<FJsonObject>& T)
	{
		const auto Ledger = Object(T, TEXT("mutation_outcomes"));
		const TArray<TSharedPtr<FJsonValue>>* Packages = nullptr; int32 Count = 0;
		if (Ledger && Ledger->TryGetArrayField(TEXT("packages"), Packages))
			for (const auto& P : *Packages) { int32 N = 0; if (P->AsObject()) P->AsObject()->TryGetNumberField(TEXT("mutation_count"), N); Count += N; }
		return Count;
	}
}
using namespace KismetAICompletionEvidenceRunTest;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompletionRun_SaveFailure, "KismetAI.CompletionEvidenceRun.SaveFailureOverridesDone", Flags)
bool FCompletionRun_SaveFailure::RunTest(const FString& Parameters)
{
	FScriptedToolInvoker Tools; Tools.SetTypedResult(TEXT("edit_blueprint"), Evidence(TEXT("/Game/A"), EKismetAISaveStatus::Failed));
	FScriptedModelClient Model({ MakeToolTurn(TEXT("a"), TEXT("edit_blueprint"), Args()), MakeTextTurn(TEXT("Done.")) });
	FKismetAIAgent Agent(Tools, Model, Config()); FEventRecorder R; Agent.RunTask(TEXT("Edit A"), R.MakeCallback());
	ON_SCOPE_EXIT { FKismetAISessionStorage::DeleteSession(Agent.GetSessionId()); };
	const auto T = Terminal(R);
	TestEqual(TEXT("save failure remains partial despite prose"), Text(Object(T, TEXT("completion_summary")), TEXT("status")), FString(TEXT("partial")));
	TestTrue(TEXT("unsaved asset is identified"), Json(T).Contains(TEXT("/Game/A")) && Json(T).Contains(TEXT("disk full")));
	TestEqual(TEXT("one committed mutation"), MutationCount(T), 1);
	CaptureSample(Agent, R, TEXT("save-failure"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompletionRun_MultiAsset, "KismetAI.CompletionEvidenceRun.UnrelatedSuccessPreservesFailure", Flags)
bool FCompletionRun_MultiAsset::RunTest(const FString& Parameters)
{
	FScriptedToolInvoker Tools;
	Tools.OnInvoke = [&](const FString&) { Tools.SetTypedResult(TEXT("edit_blueprint"), Tools.CallLog.Num() == 1
		? Evidence(TEXT("/Game/A"), EKismetAISaveStatus::Saved, EKismetAICompileStatus::Failed)
		: Evidence(TEXT("/Game/B"), EKismetAISaveStatus::Saved)); };
	FScriptedModelClient Model({ MakeResponse({ MakeToolUseBlock(TEXT("a"), TEXT("edit_blueprint"), Args()), MakeToolUseBlock(TEXT("b"), TEXT("edit_blueprint"), Args(TEXT("/Game/B"))) }, TEXT("tool_use")), MakeTextTurn(TEXT("Done.")) });
	FKismetAIAgent Agent(Tools, Model, Config()); FEventRecorder R; Agent.RunTask(TEXT("Edit A and B"), R.MakeCallback());
	ON_SCOPE_EXIT { FKismetAISessionStorage::DeleteSession(Agent.GetSessionId()); };
	const auto T = Terminal(R);
	TestEqual(TEXT("A failure survives B success"), Text(Object(T, TEXT("completion_summary")), TEXT("status")), FString(TEXT("partial")));
	const auto Ledger = Object(T, TEXT("mutation_outcomes")); const TArray<TSharedPtr<FJsonValue>>* Failed = nullptr;
	TestTrue(TEXT("A is still compile failed"), Ledger && Ledger->TryGetArrayField(TEXT("compile_failed_packages"), Failed) && Failed->ContainsByPredicate([](const auto& V) { return V->AsString() == TEXT("/Game/A"); }));
	TestEqual(TEXT("both mutations counted"), MutationCount(T), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompletionRun_ReadOnly, "KismetAI.CompletionEvidenceRun.ReadOnlyNeedsNoCompile", Flags)
bool FCompletionRun_ReadOnly::RunTest(const FString& Parameters)
{
	FScriptedToolInvoker Tools; FScriptedModelClient Model({ MakeTextTurn(TEXT("A Blueprint is an asset.")) });
	FKismetAIAgent Agent(Tools, Model, Config()); FEventRecorder R; Agent.RunTask(TEXT("What is a Blueprint?"), R.MakeCallback());
	ON_SCOPE_EXIT { FKismetAISessionStorage::DeleteSession(Agent.GetSessionId()); };
	TestEqual(TEXT("read-only completes"), Text(Object(Terminal(R), TEXT("completion_summary")), TEXT("status")), FString(TEXT("complete")));
	TestTrue(TEXT("no changes are claimed"), Json(Object(Terminal(R), TEXT("completion_summary"))).Contains(TEXT("No asset changes verified")));
	TestEqual(TEXT("no verification continuation"), Model.NumCalls(), 1); TestEqual(TEXT("no tools"), Tools.CallLog.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompletionRun_Cancel, "KismetAI.CompletionEvidenceRun.CancelPreservesUnverifiedEdit", Flags)
bool FCompletionRun_Cancel::RunTest(const FString& Parameters)
{
	FScriptedToolInvoker Tools; Tools.SetTypedResult(TEXT("edit_blueprint"), Evidence(TEXT("/Game/A"), EKismetAISaveStatus::Saved, EKismetAICompileStatus::NotRun));
	FScriptedModelClient Model({ MakeToolTurn(TEXT("a"), TEXT("edit_blueprint"), Args()), MakeTextTurn(TEXT("Done.")) });
	FKismetAIAgent Agent(Tools, Model, Config()); FEventRecorder R;
	Agent.SetJournalAppendHookForTests([&](const auto& J) { if (CallRecord(J)) Agent.Abort(); return true; });
	Agent.RunTask(TEXT("Edit A"), R.MakeCallback());
	ON_SCOPE_EXIT { FKismetAISessionStorage::DeleteSession(Agent.GetSessionId()); };
	TestEqual(TEXT("cancel summary"), Text(Object(Terminal(R), TEXT("completion_summary")), TEXT("status")), FString(TEXT("cancelled")));
	TestEqual(TEXT("committed edit remains"), MutationCount(Terminal(R)), 1);
	TestTrue(TEXT("missing compile remains visible"), Json(Terminal(R)).Contains(TEXT("not_run")));
	TestEqual(TEXT("cancel prevents verification request"), Model.NumCalls(), 1);
	CaptureSample(Agent, R, TEXT("cancelled"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompletionRun_Bounded, "KismetAI.CompletionEvidenceRun.MissingCompileContinuationIsBounded", Flags)
bool FCompletionRun_Bounded::RunTest(const FString& Parameters)
{
	FScriptedToolInvoker Tools; Tools.SetTypedResult(TEXT("edit_blueprint"), Evidence(TEXT("/Game/A"), EKismetAISaveStatus::Saved, EKismetAICompileStatus::NotRun));
	FScriptedModelClient Model({ MakeToolTurn(TEXT("a"), TEXT("edit_blueprint"), Args()), MakeTextTurn(TEXT("Done.")) });
	FKismetAIAgent Agent(Tools, Model, Config()); FEventRecorder R; Agent.RunTask(TEXT("Edit A"), R.MakeCallback());
	ON_SCOPE_EXIT { FKismetAISessionStorage::DeleteSession(Agent.GetSessionId()); };
	TestEqual(TEXT("one verification continuation only"), Model.NumCalls(), 3);
	TestEqual(TEXT("missing verification ends honestly"), Text(Object(Terminal(R), TEXT("completion_summary")), TEXT("status")), FString(TEXT("partial")));
	TestEqual(TEXT("nudge does not replay mutation"), Tools.CountCalls(TEXT("edit_blueprint")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompletionRun_JournalLoss, "KismetAI.CompletionEvidenceRun.JournalLossIsUncertain", Flags)
bool FCompletionRun_JournalLoss::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("Session journal write failed"), EAutomationExpectedErrorFlags::Contains, 1);
	FScriptedToolInvoker Tools; Tools.SetTypedResult(TEXT("edit_blueprint"), Evidence(TEXT("/Game/A"), EKismetAISaveStatus::Saved));
	FScriptedModelClient Model({ MakeToolTurn(TEXT("a"), TEXT("edit_blueprint"), Args()) });
	FKismetAIAgent Agent(Tools, Model, Config()); FEventRecorder R;
	Agent.SetJournalAppendHookForTests([](const auto& J) { return !CallRecord(J); }); Agent.RunTask(TEXT("Edit A"), R.MakeCallback());
	ON_SCOPE_EXIT { FKismetAISessionStorage::DeleteSession(Agent.GetSessionId()); };
	TestEqual(TEXT("uncommitted mutation uncertain"), Text(Object(Terminal(R), TEXT("completion_summary")), TEXT("status")), FString(TEXT("uncertain")));
	TestEqual(TEXT("no committed mutation claimed"), MutationCount(Terminal(R)), 0);
	TestEqual(TEXT("no second request"), Model.NumCalls(), 1);
	CaptureSample(Agent, R, TEXT("unknown"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompletionRun_Resume, "KismetAI.CompletionEvidenceRun.ResumeAttributesPriorFailure", Flags)
bool FCompletionRun_Resume::RunTest(const FString& Parameters)
{
	FScriptedToolInvoker Tools; Tools.SetTypedResult(TEXT("edit_blueprint"), Evidence(TEXT("/Game/A"), EKismetAISaveStatus::Failed));
	FScriptedModelClient First({ MakeToolTurn(TEXT("a"), TEXT("edit_blueprint"), Args()), MakeTextTurn(TEXT("Done.")) });
	FKismetAIAgent Agent(Tools, First, Config()); FEventRecorder Before; Agent.RunTask(TEXT("Edit A"), Before.MakeCallback());
	ON_SCOPE_EXIT { FKismetAISessionStorage::DeleteSession(Agent.GetSessionId()); };
	FScriptedModelClient Next({ MakeTextTurn(TEXT("The earlier edit remains unsaved.")) });
	FKismetAIAgent Resumed(Tools, Next, Config()); FEventRecorder After;
	Resumed.RunTask(TEXT("Summarize the earlier change."), After.MakeCallback(), TEXT(""), Agent.GetSessionId());
	const FString Summary = Json(Object(Terminal(After), TEXT("completion_summary")));
	TestTrue(TEXT("earlier failure retained and labelled prior evidence"), Summary.Contains(TEXT("/Game/A")) && Summary.Contains(TEXT("\"prior_evidence\":true")));
	TestEqual(TEXT("run identifiers stay distinct"), Text(Terminal(Before), TEXT("run_id")) == Text(Terminal(After), TEXT("run_id")), false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompletionRun_Success, "KismetAI.CompletionEvidenceRun.SuccessAndSamples", Flags)
bool FCompletionRun_Success::RunTest(const FString& Parameters)
{
	FScriptedToolInvoker Tools; Tools.SetTypedResult(TEXT("edit_blueprint"), Evidence(TEXT("/Game/A"), EKismetAISaveStatus::Saved));
	FScriptedModelClient Model({ MakeToolTurn(TEXT("a"), TEXT("edit_blueprint"), Args()), MakeTextTurn(TEXT("Done.")) });
	FKismetAIAgent Agent(Tools, Model, Config()); FEventRecorder R; Agent.RunTask(TEXT("Edit A"), R.MakeCallback());
	TestEqual(TEXT("verified execution complete"), Text(Object(Terminal(R), TEXT("completion_summary")), TEXT("status")), FString(TEXT("complete")));
	CaptureSample(Agent, R, TEXT("success"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompletionRun_ResumeReadOnly, "KismetAI.CompletionEvidenceRun.ResumeReadOnlyDoesNotCompileEarlierEdit", Flags)
bool FCompletionRun_ResumeReadOnly::RunTest(const FString& Parameters)
{
	FScriptedToolInvoker Tools; Tools.SetTypedResult(TEXT("edit_blueprint"), Evidence(TEXT("/Game/A"), EKismetAISaveStatus::Saved, EKismetAICompileStatus::NotRun));
	FScriptedModelClient First({ MakeToolTurn(TEXT("a"), TEXT("edit_blueprint"), Args()), MakeTextTurn(TEXT("Done.")) });
	FKismetAIAgent Agent(Tools, First, Config()); Agent.RunTask(TEXT("Edit A"), FOnAgentStep());
	FScriptedModelClient Next({ MakeTextTurn(TEXT("An explanation.")) });
	FKismetAIAgent Resumed(Tools, Next, Config()); FEventRecorder R;
	Resumed.RunTask(TEXT("Explain what a Blueprint is."), R.MakeCallback(), TEXT(""), Agent.GetSessionId());
	TestEqual(TEXT("prior verification does not nudge read-only request"), Next.NumCalls(), 1);
	TestTrue(TEXT("prior unverified asset disclosed"), Json(Terminal(R)).Contains(TEXT("/Game/A")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompletionRun_AllWritesLost, "KismetAI.CompletionEvidenceRun.LostTerminalPreservesUncertainty", Flags)
bool FCompletionRun_AllWritesLost::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("Session journal write failed"), EAutomationExpectedErrorFlags::Contains, 1);
	FScriptedToolInvoker Tools; Tools.SetTypedResult(TEXT("edit_blueprint"), Evidence(TEXT("/Game/A"), EKismetAISaveStatus::Saved));
	FScriptedModelClient Model({ MakeToolTurn(TEXT("a"), TEXT("edit_blueprint"), Args()) });
	FKismetAIAgent Agent(Tools, Model, Config()); FEventRecorder R; bool bReject = false;
	Agent.SetJournalAppendHookForTests([&](const auto& J) { bReject |= CallRecord(J); return !bReject; });
	Agent.RunTask(TEXT("Edit A"), R.MakeCallback());
	TestEqual(TEXT("terminal and summary agree uncertain"), Text(Terminal(R), TEXT("outcome")), FString(TEXT("uncertain")));
	TestEqual(TEXT("uncertainty summary"), Text(Object(Terminal(R), TEXT("completion_summary")), TEXT("status")), FString(TEXT("uncertain")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompletionRun_Budget, "KismetAI.CompletionEvidenceRun.VerificationBudgetEndsPartial", Flags)
bool FCompletionRun_Budget::RunTest(const FString& Parameters)
{
	FScriptedToolInvoker Tools; Tools.SetTypedResult(TEXT("edit_blueprint"), Evidence(TEXT("/Game/A"), EKismetAISaveStatus::Saved, EKismetAICompileStatus::NotRun));
	FScriptedModelClient Model({ MakeToolTurn(TEXT("a"), TEXT("edit_blueprint"), Args()) });
	FKismetAIAgentConfig Limited = Config(); Limited.MaxSteps = 1;
	FKismetAIAgent Agent(Tools, Model, Limited); FEventRecorder R; Agent.RunTask(TEXT("Edit A"), R.MakeCallback());
	TestEqual(TEXT("budget stops with explicit partial evidence"), Text(Object(Terminal(R), TEXT("completion_summary")), TEXT("status")), FString(TEXT("partial")));
	TestEqual(TEXT("verification cannot exceed configured turns"), Model.NumCalls(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompletionRun_Untyped, "KismetAI.CompletionEvidenceRun.UntypedMutationCannotClaimCompletion", Flags)
bool FCompletionRun_Untyped::RunTest(const FString& Parameters)
{
	FScriptedToolInvoker Tools; Tools.SetResult(TEXT("edit_blueprint"), Evidence(TEXT("/Game/A"), EKismetAISaveStatus::Saved).ToModelString());
	FScriptedModelClient Model({ MakeToolTurn(TEXT("a"), TEXT("edit_blueprint"), Args()), MakeTextTurn(TEXT("Done.")) });
	FKismetAIAgent Agent(Tools, Model, Config()); FEventRecorder R; Agent.RunTask(TEXT("Edit A"), R.MakeCallback());
	TestEqual(TEXT("untyped success lacks verified mutation evidence"), Text(Object(Terminal(R), TEXT("completion_summary")), TEXT("status")), FString(TEXT("uncertain")));
	TestEqual(TEXT("model-visible payload cannot prove a change"), MutationCount(Terminal(R)), 0);
	return true;
}
