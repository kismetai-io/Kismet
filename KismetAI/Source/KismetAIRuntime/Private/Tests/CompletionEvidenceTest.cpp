// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
#include "KismetAIMutationOutcome.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompletionEvidenceLedger,
	"KismetAI.CompletionEvidence.Ledger", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCompletionEvidenceLedger::RunTest(const FString& Parameters)
{
	FKismetAIRunOutcomeLedger Ledger;
	FKismetAIMutationOutcome A;
	A.PackageName = TEXT("/Game/A");
	A.CompileStatus = EKismetAICompileStatus::Failed;
	Ledger.Record(TEXT("compile_blueprint"), A);
	TestEqual(TEXT("compile-only failure remains evidence"), Ledger.GetCompileFailedPackages().Num(), 1);
	FKismetAIMutationOutcome B;
	B.PackageName = TEXT("/Game/B");
	B.bAppliedInMemory = true;
	B.CompileStatus = EKismetAICompileStatus::Succeeded;
	B.SaveStatus = EKismetAISaveStatus::Saved;
	Ledger.Record(TEXT("edit_blueprint"), B);
	TestTrue(TEXT("A failure survives B success"), Ledger.NeedsAttention());
	B.CompileStatus = EKismetAICompileStatus::NotRun;
	B.SaveStatus = EKismetAISaveStatus::NotRequested;
	B.SaveSkipReason = KismetAISaveSkipReason::NotAutosaved;
	Ledger.Record(TEXT("edit_blueprint"), B);
	TestEqual(TEXT("not-autosaved changed package is unsaved"), Ledger.GetUnsavedPackages().Num(), 1);
	const auto Packages = Ledger.BuildEventJson()->GetArrayField(TEXT("packages"));
	for (const auto& Value : Packages)
	{
		const auto Entry = Value->AsObject();
		if (Entry->GetStringField(TEXT("package")) == TEXT("/Game/B"))
			TestEqual(TEXT("earlier compile success is stale after mutation"), Entry->GetStringField(TEXT("compile_status")), FString(TEXT("not_run")));
	}
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompletionEvidenceIsolation,
	"KismetAI.CompletionEvidence.OperationAndRunIsolation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCompletionEvidenceIsolation::RunTest(const FString& Parameters)
{
	auto Call = [](const TCHAR* Tool, const TCHAR* Status, const FKismetAIMutationOutcome& Outcome, const TCHAR* Run, const TCHAR* Value = TEXT("one"))
	{
		auto Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("tool_name"), Tool); Entry->SetStringField(TEXT("status"), Status);
		Entry->SetStringField(TEXT("run_id"), Run); Entry->SetStringField(TEXT("call_id"), Run);
		auto Input = MakeShared<FJsonObject>();
		Input->SetStringField(TEXT("blueprint"), TEXT("/Game/A.A")); Input->SetStringField(TEXT("value"), Value);
		Entry->SetObjectField(TEXT("input"), Input); Entry->SetObjectField(TEXT("mutation_outcome"), Outcome.ToJson());
		return Entry;
	};
	auto Package = [](const FKismetAIRunOutcomeLedger& Ledger)
	{
		const auto Event = Ledger.BuildEventJson();
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		return Event->TryGetArrayField(TEXT("packages"), Entries) && !Entries->IsEmpty() ? (*Entries)[0]->AsObject() : MakeShared<FJsonObject>();
	};
	FKismetAIMutationOutcome Edit;
	Edit.PackageName = TEXT("/Game/A"); Edit.bAppliedInMemory = true;
	Edit.SaveStatus = EKismetAISaveStatus::Saved; Edit.CompileStatus = EKismetAICompileStatus::Succeeded;
	FKismetAIRunOutcomeLedger Failed;
	Failed.RecordCommittedCall(Call(TEXT("edit_blueprint"), TEXT("failed"), Edit, TEXT("old")));
	FKismetAIMutationOutcome Compile = Edit; Compile.bAppliedInMemory = false; Compile.SaveStatus = EKismetAISaveStatus::NotRequested;
	Failed.RecordCommittedCall(Call(TEXT("compile_blueprint"), TEXT("succeeded"), Compile, TEXT("new")));
	TestEqual(TEXT("compile cannot erase rejected edit"), Failed.BuildCompletionSummary(TEXT("completed"))->GetStringField(TEXT("status")), FString(TEXT("partial")));
	Failed.RecordCommittedCall(Call(TEXT("edit_blueprint"), TEXT("succeeded"), Edit, TEXT("new"), TEXT("different")));
	TestTrue(TEXT("different edit cannot erase rejected operation"), Failed.HasUnresolvedEvidence());
	Failed.RecordCommittedCall(Call(TEXT("edit_blueprint"), TEXT("succeeded"), Edit, TEXT("new")));
	TestFalse(TEXT("exact successful applied edit supersedes its failure"), Failed.HasUnresolvedEvidence());

	FKismetAIRunOutcomeLedger Prior;
	Edit.SaveStatus = EKismetAISaveStatus::Failed;
	Prior.RecordCommittedCall(Call(TEXT("edit_blueprint"), TEXT("succeeded"), Edit, TEXT("old")));
	Prior.RetainPriorUnresolved();
	Edit.SaveStatus = EKismetAISaveStatus::Saved;
	Prior.RecordCommittedCall(Call(TEXT("edit_blueprint"), TEXT("succeeded"), Edit, TEXT("new")));
	TestFalse(TEXT("fully resolved new edit is current evidence"), Package(Prior)->GetBoolField(TEXT("prior_evidence")));
	TestEqual(TEXT("new edit has new run identity"), Package(Prior)->GetStringField(TEXT("evidence_run_id")), FString(TEXT("new")));

	FKismetAIRunOutcomeLedger Missing;
	Edit.CompileStatus = EKismetAICompileStatus::NotRun;
	Missing.RecordCommittedCall(Call(TEXT("edit_blueprint"), TEXT("succeeded"), Edit, TEXT("old")));
	Missing.RetainPriorUnresolved();
	TestEqual(TEXT("historical missing compile remains visible"), Missing.GetUnverifiedPackages().Num(), 1);
	TestEqual(TEXT("historical missing compile does not nudge read-only turn"), Missing.GetUnverifiedPackages(true).Num(), 0);
	Missing.RecordCommittedCall(Call(TEXT("edit_blueprint"), TEXT("succeeded"), Edit, TEXT("new")));
	TestEqual(TEXT("current unverified mutation is nudge candidate"), Missing.GetUnverifiedPackages(true).Num(), 1);
	TestEqual(TEXT("mixed evidence retains original run"), Package(Missing)->GetStringField(TEXT("prior_run_id")), FString(TEXT("old")));
	TestTrue(TEXT("mixed attribution is explicit"), Package(Missing)->GetStringField(TEXT("summary")).Contains(TEXT("unresolved earlier-run state")));

	FKismetAIRunOutcomeLedger Human;
	Edit.CompileStatus = EKismetAICompileStatus::Succeeded; Edit.bPackageWasDirtyBefore = true;
	Human.Record(TEXT("edit_blueprint"), Edit); Human.Record(TEXT("compile_blueprint"), Compile);
	TestTrue(TEXT("compile preserves human-save disclosure text"), Package(Human)->GetStringField(TEXT("summary")).Contains(TEXT("pre-existing unsaved user changes")));

	FKismetAIRunOutcomeLedger PendingCompile;
	Compile.CompileStatus = EKismetAICompileStatus::NotRun;
	PendingCompile.Record(TEXT("compile_material"), Compile);
	TestEqual(TEXT("compile without finished resource is missing verification"), PendingCompile.GetUnverifiedPackages().Num(), 1);
	TestEqual(TEXT("compile-only pending state is partial"), PendingCompile.BuildCompletionSummary(TEXT("completed"))->GetStringField(TEXT("status")), FString(TEXT("partial")));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompletionEvidenceSemanticRepair,
	"KismetAI.CompletionEvidence.SemanticRepairIdentity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCompletionEvidenceSemanticRepair::RunTest(const FString& Parameters)
{
	FKismetAIMutationOutcome Outcome;
	Outcome.PackageName = TEXT("/Game/A"); Outcome.bAppliedInMemory = true;
	Outcome.SaveStatus = EKismetAISaveStatus::Saved; Outcome.CompileStatus = EKismetAICompileStatus::Succeeded;
	auto Call = [&Outcome](const TCHAR* Status, const FString& Json)
	{
		auto Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("tool_name"), TEXT("edit_blueprint")); Entry->SetStringField(TEXT("status"), Status);
		TSharedPtr<FJsonObject> Input;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Input);
		Entry->SetObjectField(TEXT("input"), Input); Entry->SetObjectField(TEXT("mutation_outcome"), Outcome.ToJson());
		return Entry;
	};
	const FString Original = TEXT("{\"blueprint\":\"/Game/A.A\",\"expected_revision\":\"old\",\"operations\":[{\"action\":\"set_pin_default\",\"node\":\"N\",\"pin\":\"P\",\"value\":\"one\"}]}");
	const FString Reordered = TEXT("{\"operations\":[{\"value\":\"one\",\"pin\":\"P\",\"node\":\"N\",\"action\":\"set_pin_default\"}],\"expected_revision\":\"fresh\",\"blueprint\":\"/game/a\"}");
	FKismetAIRunOutcomeLedger Ledger;
	Ledger.RecordCommittedCall(Call(TEXT("failed"), Original));
	Ledger.RecordCommittedCall(Call(TEXT("succeeded"), Reordered.Replace(TEXT("one"), TEXT("different"))));
	TestTrue(TEXT("a different operand does not supersede failed edit"), Ledger.HasUnresolvedEvidence());
	Ledger.RecordCommittedCall(Call(TEXT("succeeded"), Reordered));
	TestFalse(TEXT("same edit with fresh revision and reordered keys resolves failure"), Ledger.HasUnresolvedEvidence());
	return true;
}
