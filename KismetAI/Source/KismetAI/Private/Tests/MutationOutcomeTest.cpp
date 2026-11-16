// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Mutation, compilation and persistence outcomes are reported truthfully.
//
// Every assertion here is about a state that used to be invisible behind
// `success: true`: a save that failed, a graph that does not compile, a save
// that also wrote edits the user had not saved, and eval mode's deliberate
// no-save. Fixtures are created by the tests themselves — transient packages,
// or a disposable /Game/__KismetAITest_P001 folder that is deleted afterwards.

#include "Misc/AutomationTest.h"
#include "KismetAIModule.h"
#include "KismetAIAgent.h"
#include "KismetAIToolResult.h"
#include "KismetAIMutationOutcome.h"
#include "KismetAIPersistence.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "DiffUtils.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"

constexpr auto MutationOutcomeTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace MutationOutcomeTest
{
	static UBlueprint* MakeBlueprint(const FString& PackageName, const FString& AssetName)
	{
		UPackage* Pkg = CreatePackage(*PackageName);
		if (!Pkg) return nullptr;
		return FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Pkg, FName(*AssetName), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	}

	// A disposable, file-backed package name. Never project content.
	static FString DisposablePackageName(const TCHAR* Stem)
	{
		return FString::Printf(TEXT("/Game/__KismetAITest_P001/%s_%s"), Stem,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	}

	static void DisposeBlueprint(UBlueprint* BP)
	{
		if (!BP) return;
		UPackage* Pkg = BP->GetOutermost();
		if (UClass* Gen = BP->GeneratedClass)
		{
			Gen->ClearFlags(RF_Public | RF_Standalone);
			Gen->MarkAsGarbage();
		}
		BP->ClearFlags(RF_Public | RF_Standalone);
		BP->MarkAsGarbage();
		if (Pkg)
		{
			Pkg->SetDirtyFlag(false);
			Pkg->MarkAsGarbage();
		}
	}

	static TSharedPtr<FJsonObject> ParseEnvelope(FString Text)
	{
		Text.RemoveFromStart(TEXT("<tool_use_error>"));
		Text.RemoveFromEnd(TEXT("</tool_use_error>"));
		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		FJsonSerializer::Deserialize(Reader, Parsed);
		return Parsed;
	}

	static TSharedPtr<FJsonObject> GetOutcomeJson(const FToolExecutionResult& R)
	{
		const TSharedPtr<FJsonObject>* O = nullptr;
		if (R.Data.IsValid() && R.Data->TryGetObjectField(TEXT("outcome"), O) && O)
		{
			return *O;
		}
		return nullptr;
	}

	static TSharedPtr<FJsonObject> AddVariableArgs(const UBlueprint* BP, const FString& VarName)
	{
		TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
		Op->SetStringField(TEXT("action"), TEXT("add_variable"));
		Op->SetStringField(TEXT("name"), VarName);
		Op->SetStringField(TEXT("type"), TEXT("bool"));
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Args->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(Op) });
		return Args;
	}

	static bool IsExactly(const TArray<FString>& Actual, const FString& Expected)
	{
		return Actual.Num() == 1 && Actual[0] == Expected;
	}

	static bool HasVariable(const UBlueprint* BP, const FString& Name)
	{
		if (!BP) return false;
		for (const FBPVariableDescription& V : BP->NewVariables)
		{
			if (V.VarName.ToString() == Name) return true;
		}
		return false;
	}

	// Load what is actually on disk, independently of the live in-memory
	// Blueprint: copy the file to Saved/Diff and load it under a /Temp name the
	// way the editor's diff tool does. This is the persistence evidence.
	struct FDiskSnapshot
	{
		UBlueprint* Blueprint = nullptr;
		UPackage* Package = nullptr;
		FString TempFile;
		~FDiskSnapshot()
		{
			if (Package)
			{
				ResetLoaders(Package);
				Package->MarkAsGarbage();
			}
			if (!TempFile.IsEmpty())
			{
				IFileManager::Get().Delete(*TempFile, false, true, true);
			}
		}
	};

	static void LoadFromDisk(const FString& PackageFile, FDiskSnapshot& Out)
	{
		Out.TempFile = FPaths::CreateTempFilename(*FPaths::DiffDir(), TEXT("P001_"), TEXT(".uasset"));
		if (IFileManager::Get().Copy(*Out.TempFile, *PackageFile) != COPY_OK)
		{
			Out.TempFile.Reset();
			return;
		}
		Out.Package = DiffUtils::LoadPackageForDiff(
			FPackagePath::FromLocalPath(Out.TempFile), FPackagePath::FromLocalPath(PackageFile));
		if (!Out.Package) return;
		ForEachObjectWithPackage(Out.Package, [&Out](UObject* Obj)
		{
			if (UBlueprint* BP = Cast<UBlueprint>(Obj))
			{
				Out.Blueprint = BP;
				return false;
			}
			return true;
		});
	}

	// Wire a custom event into a call to a function that does not exist, so the
	// next compile of this Blueprint fails with an error.
	static void BreakCompile(UBlueprint* BP)
	{
		UEdGraph* Graph = BP->UbergraphPages.Num() > 0 ? BP->UbergraphPages[0] : nullptr;
		if (!Graph) return;
		UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
		Event->CustomFunctionName = FName(TEXT("P001_BrokenEntry"));
		Graph->AddNode(Event, false, false);
		Event->CreateNewGuid();
		Event->AllocateDefaultPins();

		UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
		Call->FunctionReference.SetExternalMember(FName(TEXT("P001_NoSuchFunction")), UKismetSystemLibrary::StaticClass());
		Graph->AddNode(Call, false, false);
		Call->CreateNewGuid();
		Call->AllocateDefaultPins();

		UEdGraphPin* Then = Event->FindPin(UEdGraphSchema_K2::PN_Then);
		UEdGraphPin* Exec = Call->FindPin(UEdGraphSchema_K2::PN_Execute);
		if (Then && Exec)
		{
			Then->MakeLinkTo(Exec);
		}
	}
}

using namespace MutationOutcomeTest;

// ─────────────────────────────────────────────────────────────────────────────
// Contract: JSON shape, summary wording, round trip.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationOutcome_Contract, "KismetAI.MutationOutcome.Contract", MutationOutcomeTestFlags)
bool FMutationOutcome_Contract::RunTest(const FString& Parameters)
{
	FKismetAIMutationOutcome O;
	O.bAppliedInMemory = true;
	O.CompileStatus = EKismetAICompileStatus::Succeeded;
	O.SaveStatus = EKismetAISaveStatus::Failed;
	O.SaveError = TEXT("target file is read-only");
	O.PackageName = TEXT("/Game/BP_X");
	O.SavePolicy = KismetAISavePolicy::AutosaveEvenIfCompileFailed;

	const TSharedPtr<FJsonObject> J = O.ToJson();
	TestTrue(TEXT("applied_in_memory"), J->GetBoolField(TEXT("applied_in_memory")));
	TestEqual(TEXT("compile_status"), J->GetStringField(TEXT("compile_status")), FString(TEXT("succeeded")));
	TestEqual(TEXT("save_status"), J->GetStringField(TEXT("save_status")), FString(TEXT("failed")));
	TestEqual(TEXT("save_error"), J->GetStringField(TEXT("save_error")), FString(TEXT("target file is read-only")));
	TestEqual(TEXT("recovery_status not_tracked outside atomic rollback"), J->GetStringField(TEXT("recovery_status")), FString(TEXT("not_tracked")));
	TestFalse(TEXT("no skip reason on a failed save"), J->HasField(TEXT("save_skip_reason")));

	const FString Summary = J->GetStringField(TEXT("summary"));
	TestTrue(TEXT("failed save says NOT saved"), Summary.Contains(TEXT("NOT saved")));
	TestFalse(TEXT("failed save never claims a save to disk"), Summary.Contains(TEXT("saved /Game/BP_X to disk")));
	TestTrue(TEXT("needs attention"), O.NeedsAttention());

	// The outcome must not trip the loop's substring error check: a changed-
	// but-unsaved edit is still an applied edit, and R3b would otherwise refuse
	// an identical resend and the model would assume nothing changed.
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetObjectField(TEXT("outcome"), J);
	const FString ModelString = FToolExecutionResult::Success(Data).ToModelString();
	TestFalse(TEXT("outcome alone does not flag the result as an error"), FKismetAIAgent::IsToolResultError(ModelString));

	FKismetAIMutationOutcome Back;
	TestTrue(TEXT("extract from a model string"), FKismetAIMutationOutcome::TryExtractFromToolResult(ModelString, Back));
	TestEqual(TEXT("round-trip save status"), (int32)Back.SaveStatus, (int32)EKismetAISaveStatus::Failed);
	TestEqual(TEXT("round-trip error"), Back.SaveError, O.SaveError);
	TestEqual(TEXT("round-trip package"), Back.PackageName, O.PackageName);

	// Wrapped (error) results still yield their outcome.
	const FString Wrapped = FString::Printf(TEXT("<tool_use_error>%s</tool_use_error>"), *FToolExecutionResult::Success(Data).ToEnvelopeString());
	TestTrue(TEXT("extract from a wrapped result"), FKismetAIMutationOutcome::TryExtractFromToolResult(Wrapped, Back));

	// Saved + compile failed + pre-dirty: every implication spelled out.
	FKismetAIMutationOutcome S;
	S.bAppliedInMemory = true;
	S.CompileStatus = EKismetAICompileStatus::Failed;
	S.CompileErrorCount = 2;
	S.SaveStatus = EKismetAISaveStatus::Saved;
	S.bPackageWasDirtyBefore = true;
	S.PackageName = TEXT("/Game/BP_Y");
	const FString SSum = S.BuildSummary();
	TestTrue(TEXT("compile failure stated"), SSum.Contains(TEXT("compile FAILED (2 errors)")));
	TestTrue(TEXT("uncompilable state on disk stated"), SSum.Contains(TEXT("does not compile")));
	TestTrue(TEXT("pre-existing edits stated"), SSum.Contains(TEXT("already had unsaved changes")));
	TestTrue(TEXT("flag"), S.ToJson()->GetBoolField(TEXT("save_included_preexisting_changes")));

	// Not requested: reason is present, no save claimed.
	FKismetAIMutationOutcome N;
	N.bAppliedInMemory = true;
	N.SaveSkipReason = KismetAISaveSkipReason::EvalMode;
	const TSharedPtr<FJsonObject> NJ = N.ToJson();
	TestEqual(TEXT("not_requested"), NJ->GetStringField(TEXT("save_status")), FString(TEXT("not_requested")));
	TestEqual(TEXT("reason"), NJ->GetStringField(TEXT("save_skip_reason")), FString(TEXT("eval_mode")));
	TestFalse(TEXT("pre-existing flag false when not saved"), NJ->GetBoolField(TEXT("save_included_preexisting_changes")));
	TestTrue(TEXT("summary says not saved"), NJ->GetStringField(TEXT("summary")).Contains(TEXT("not saved (eval mode")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Run ledger: what the end-of-run event reports.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationOutcome_Ledger, "KismetAI.MutationOutcome.Ledger", MutationOutcomeTestFlags)
bool FMutationOutcome_Ledger::RunTest(const FString& Parameters)
{
	FKismetAIRunOutcomeLedger Ledger;

	FKismetAIMutationOutcome Failed;
	Failed.bAppliedInMemory = true;
	Failed.CompileStatus = EKismetAICompileStatus::Failed;
	Failed.CompileErrorCount = 1;
	Failed.SaveStatus = EKismetAISaveStatus::Failed;
	Failed.SaveError = TEXT("locked");
	Failed.PackageName = TEXT("/Game/A");
	Ledger.Record(TEXT("edit_blueprint"), Failed);

	// A layout call after it: no compile, nothing to save (say the package was
	// clean). Neither the compile failure nor the unsaved change may vanish.
	FKismetAIMutationOutcome Layout;
	Layout.bAppliedInMemory = true;
	Layout.SaveSkipReason = KismetAISaveSkipReason::PackageNotDirty;
	Layout.PackageName = TEXT("/Game/A");
	Ledger.Record(TEXT("layout_graph"), Layout);

	TestTrue(TEXT("A still unsaved"), IsExactly(Ledger.GetUnsavedPackages(), TEXT("/Game/A")));
	TestTrue(TEXT("A still not compiling"), IsExactly(Ledger.GetCompileFailedPackages(), TEXT("/Game/A")));

	// Pre-dirty save on B is sticky even after a clean follow-up.
	FKismetAIMutationOutcome PreDirty;
	PreDirty.bAppliedInMemory = true;
	PreDirty.CompileStatus = EKismetAICompileStatus::Succeeded;
	PreDirty.SaveStatus = EKismetAISaveStatus::Saved;
	PreDirty.bPackageWasDirtyBefore = true;
	PreDirty.PackageName = TEXT("/Game/B");
	Ledger.Record(TEXT("insert_node"), PreDirty);
	FKismetAIMutationOutcome Clean = PreDirty;
	Clean.bPackageWasDirtyBefore = false;
	Ledger.Record(TEXT("insert_node"), Clean);
	TestTrue(TEXT("B disclosed"), IsExactly(Ledger.GetPackagesSavedWithPreexistingChanges(), TEXT("/Game/B")));

	// A compile-only call (compile_blueprint) refreshes the compile state of a
	// package already in the ledger without touching its save state.
	FKismetAIMutationOutcome CompileOnly;
	CompileOnly.PackageName = TEXT("/Game/A");
	CompileOnly.CompileStatus = EKismetAICompileStatus::Succeeded;
	CompileOnly.SaveSkipReason = KismetAISaveSkipReason::NothingApplied;
	Ledger.Record(TEXT("compile_blueprint"), CompileOnly);
	TestEqual(TEXT("A compiles after compile-only call"), Ledger.GetCompileFailedPackages().Num(), 0);
	TestTrue(TEXT("A is still unsaved"), IsExactly(Ledger.GetUnsavedPackages(), TEXT("/Game/A")));
	// Compile-only work is evidence even when the run did not edit the package.
	FKismetAIMutationOutcome OtherCompile = CompileOnly;
	OtherCompile.PackageName = TEXT("/Game/Untouched");
	OtherCompile.CompileStatus = EKismetAICompileStatus::Failed;
	Ledger.Record(TEXT("compile_blueprint"), OtherCompile);
	TestTrue(TEXT("compile-only failure reported"), IsExactly(Ledger.GetCompileFailedPackages(), TEXT("/Game/Untouched")));

	// A later successful save + compile clears A.
	FKismetAIMutationOutcome Fixed;
	Fixed.bAppliedInMemory = true;
	Fixed.CompileStatus = EKismetAICompileStatus::Succeeded;
	Fixed.SaveStatus = EKismetAISaveStatus::Saved;
	Fixed.PackageName = TEXT("/Game/A");
	Ledger.Record(TEXT("edit_blueprint"), Fixed);
	TestEqual(TEXT("A saved now"), Ledger.GetUnsavedPackages().Num(), 0);
	TestTrue(TEXT("only the unrelated compile failure remains"), IsExactly(Ledger.GetCompileFailedPackages(), TEXT("/Game/Untouched")));
	TestTrue(TEXT("disclosure still needs attention"), Ledger.NeedsAttention());

	const TSharedPtr<FJsonObject> Event = Ledger.BuildEventJson();
	TestEqual(TEXT("event type"), Event->GetStringField(TEXT("type")), FString(TEXT("mutation_outcomes")));
	TestEqual(TEXT("three packages including compile-only"), Event->GetArrayField(TEXT("packages")).Num(), 3);

	// Outcomes that changed nothing are not recorded.
	FKismetAIRunOutcomeLedger Empty;
	FKismetAIMutationOutcome Nothing;
	Nothing.PackageName = TEXT("/Game/C");
	Nothing.SaveSkipReason = KismetAISaveSkipReason::NothingApplied;
	Empty.Record(TEXT("manage_blueprint"), Nothing);
	TestTrue(TEXT("no-op not recorded"), Empty.IsEmpty());
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Save boundary: eval mode, no file backing, nothing applied, injected failure.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationOutcome_SaveBoundary, "KismetAI.MutationOutcome.SaveBoundary", MutationOutcomeTestFlags)
bool FMutationOutcome_SaveBoundary::RunTest(const FString& Parameters)
{
	UBlueprint* BP = MakeBlueprint(DisposablePackageName(TEXT("BP_P001_Boundary")), TEXT("BP_P001_Boundary"));
	UBlueprint* TempBP = MakeBlueprint(TEXT("/Temp/KismetAIP001_Transient"), TEXT("BP_P001_Transient"));
	if (!TestNotNull(TEXT("fixture"), BP) || !TestNotNull(TEXT("transient fixture"), TempBP)) return false;
	ON_SCOPE_EXIT { DisposeBlueprint(BP); DisposeBlueprint(TempBP); };
	BP->MarkPackageDirty();

	int32 SaveCalls = 0;
	KismetAIPersistence::FScopedSaveFunctionOverride Inject(
		[&SaveCalls](UPackage*, UObject*, const FString&, FString& OutError)
		{
			SaveCalls++;
			OutError = TEXT("injected: disk full");
			return false;
		});

	{
		KismetAIPersistence::FScopedEvalModeOverride Eval(true);
		FKismetAIMutationOutcome O;
		O.bAppliedInMemory = true;
		KismetAIPersistence::SaveAfterMutation(BP, O);
		TestEqual(TEXT("eval: not_requested"), (int32)O.SaveStatus, (int32)EKismetAISaveStatus::NotRequested);
		TestEqual(TEXT("eval: reason"), O.SaveSkipReason, FString(KismetAISaveSkipReason::EvalMode));
		TestEqual(TEXT("eval: save never called"), SaveCalls, 0);
	}

	KismetAIPersistence::FScopedEvalModeOverride NotEval(false);
	{
		FKismetAIMutationOutcome O;
		O.bAppliedInMemory = true;
		KismetAIPersistence::SaveAfterMutation(TempBP, O);
		TestEqual(TEXT("transient: reason"), O.SaveSkipReason, FString(KismetAISaveSkipReason::NoFileBacking));
		TestEqual(TEXT("transient: save never called"), SaveCalls, 0);
	}
	{
		FKismetAIMutationOutcome O;
		O.bAppliedInMemory = false;
		KismetAIPersistence::SaveAfterMutation(BP, O);
		TestEqual(TEXT("nothing applied: reason"), O.SaveSkipReason, FString(KismetAISaveSkipReason::NothingApplied));
		TestEqual(TEXT("nothing applied: save never called"), SaveCalls, 0);
	}
	{
		AddExpectedMessage(TEXT("Save failed for"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
		FKismetAIMutationOutcome O;
		O.bAppliedInMemory = true;
		KismetAIPersistence::SaveAfterMutation(BP, O);
		TestEqual(TEXT("injected: failed"), (int32)O.SaveStatus, (int32)EKismetAISaveStatus::Failed);
		TestEqual(TEXT("injected: reason carried"), O.SaveError, FString(TEXT("injected: disk full")));
		TestEqual(TEXT("injected: package identity"), O.PackageName, BP->GetOutermost()->GetName());
		TestTrue(TEXT("injected: target file named"), O.PackageFilename.EndsWith(TEXT(".uasset")));
		TestEqual(TEXT("save called once"), SaveCalls, 1);
		TestTrue(TEXT("package still dirty — the change is unsaved"), BP->GetOutermost()->IsDirty());
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Through a real handler: injected save failure -> changed-but-unsaved.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationOutcome_HandlerSaveFailure, "KismetAI.MutationOutcome.HandlerSaveFailure", MutationOutcomeTestFlags)
bool FMutationOutcome_HandlerSaveFailure::RunTest(const FString& Parameters)
{
	UBlueprint* BP = MakeBlueprint(DisposablePackageName(TEXT("BP_P001_SaveFail")), TEXT("BP_P001_SaveFail"));
	if (!TestNotNull(TEXT("fixture"), BP)) return false;
	ON_SCOPE_EXIT { DisposeBlueprint(BP); };
	BP->GetOutermost()->SetDirtyFlag(false);

	AddExpectedMessage(TEXT("Save failed for"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	KismetAIPersistence::FScopedEvalModeOverride NotEval(false);
	KismetAIPersistence::FScopedSaveFunctionOverride Inject(
		[](UPackage*, UObject*, const FString&, FString& OutError)
		{
			OutError = TEXT("injected: source control lock");
			return false;
		});

	FKismetAIModule& Module = FKismetAIModule::Get();
	const FToolExecutionResult R = Module.DoManageBlueprint(AddVariableArgs(BP, TEXT("P001_Unsaved")));

	TestTrue(TEXT("outer result still success: the edit was applied"), R.bSuccess);
	TestTrue(TEXT("variable exists in memory"), HasVariable(BP, TEXT("P001_Unsaved")));
	const TSharedPtr<FJsonObject> O = GetOutcomeJson(R);
	if (!TestTrue(TEXT("outcome attached"), O.IsValid())) return false;
	TestTrue(TEXT("applied_in_memory"), O->GetBoolField(TEXT("applied_in_memory")));
	TestEqual(TEXT("compile ran and succeeded"), O->GetStringField(TEXT("compile_status")), FString(TEXT("succeeded")));
	TestEqual(TEXT("save failed"), O->GetStringField(TEXT("save_status")), FString(TEXT("failed")));
	TestTrue(TEXT("reason surfaced"), O->GetStringField(TEXT("save_error")).Contains(TEXT("source control lock")));
	TestFalse(TEXT("not pre-dirty"), O->GetBoolField(TEXT("package_was_dirty_before")));
	const FString Summary = O->GetStringField(TEXT("summary"));
	TestTrue(TEXT("summary says NOT saved"), Summary.Contains(TEXT("NOT saved")));
	TestFalse(TEXT("no persisted-success text"), Summary.Contains(TEXT("to disk.")));

	// The model sees the same thing (outcome first, so truncation keeps it).
	const FString ModelString = R.ToModelString();
	const int32 OutcomeAt = ModelString.Find(TEXT("\"outcome\""));
	const int32 ResultsAt = ModelString.Find(TEXT("\"results\""));
	TestTrue(TEXT("outcome precedes the bulky fields"), OutcomeAt != INDEX_NONE && ResultsAt != INDEX_NONE && OutcomeAt < ResultsAt);

	// And the HTTP route reports it too.
	const FString Http = Module.ExecuteHandlerDirect(&FKismetAIModule::HandleManageBlueprint, AddVariableArgs(BP, TEXT("P001_Unsaved2")));
	const TSharedPtr<FJsonObject> HttpEnv = ParseEnvelope(Http);
	const TSharedPtr<FJsonObject>* HttpData = nullptr;
	const TSharedPtr<FJsonObject>* HttpOutcome = nullptr;
	TestTrue(TEXT("http carries outcome"),
		HttpEnv.IsValid() && HttpEnv->TryGetObjectField(TEXT("data"), HttpData) && HttpData
		&& (*HttpData)->TryGetObjectField(TEXT("outcome"), HttpOutcome) && HttpOutcome);
	if (HttpOutcome)
	{
		TestEqual(TEXT("http: save failed"), (*HttpOutcome)->GetStringField(TEXT("save_status")), FString(TEXT("failed")));
		// The first failed save left the package dirty: the second call must
		// disclose that the package already had unsaved changes.
		TestTrue(TEXT("http: second call sees pre-dirty package"), (*HttpOutcome)->GetBoolField(TEXT("package_was_dirty_before")));
	}

	// explicit manage_asset save fails as an error, with the injected reason.
	TSharedPtr<FJsonObject> SaveArgs = MakeShared<FJsonObject>();
	SaveArgs->SetStringField(TEXT("action"), TEXT("save"));
	SaveArgs->SetStringField(TEXT("asset_path"), BP->GetPathName());
	const FToolExecutionResult SaveR = Module.DoManageAsset(SaveArgs);
	TestFalse(TEXT("explicit save fails"), SaveR.bSuccess);
	TestEqual(TEXT("SAVE_FAILED"), SaveR.ErrorCode, FString(TEXT("SAVE_FAILED")));
	TestTrue(TEXT("explicit save names the cause"), SaveR.Message.Contains(TEXT("source control lock")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Compile failure survives every wrapper: typed result, model string, HTTP
// envelope, run ledger. Saved anyway (documented policy) and said so.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationOutcome_CompileFailureSurvives, "KismetAI.MutationOutcome.CompileFailureSurvives", MutationOutcomeTestFlags)
bool FMutationOutcome_CompileFailureSurvives::RunTest(const FString& Parameters)
{
	UBlueprint* BP = MakeBlueprint(DisposablePackageName(TEXT("BP_P001_Broken")), TEXT("BP_P001_Broken"));
	if (!TestNotNull(TEXT("fixture"), BP)) return false;
	ON_SCOPE_EXIT { DisposeBlueprint(BP); };
	BreakCompile(BP);
	BP->GetOutermost()->SetDirtyFlag(false);

	// The compiler logs the missing function as an error.
	AddExpectedError(TEXT("P001_NoSuchFunction"), EAutomationExpectedErrorFlags::Contains, 0);
	KismetAIPersistence::FScopedEvalModeOverride NotEval(false);
	int32 SaveCalls = 0;
	KismetAIPersistence::FScopedSaveFunctionOverride Inject(
		[&SaveCalls](UPackage*, UObject*, const FString&, FString&)
		{
			SaveCalls++;
			return true; // pretend the write worked; no file is touched
		});

	FKismetAIModule& Module = FKismetAIModule::Get();
	const FToolExecutionResult R = Module.DoManageBlueprint(AddVariableArgs(BP, TEXT("P001_WhileBroken")));
	if (!TestTrue(TEXT("compile fixture actually fails"), BP->Status == BS_Error)) return false;

	const TSharedPtr<FJsonObject> O = GetOutcomeJson(R);
	if (!TestTrue(TEXT("outcome attached"), O.IsValid())) return false;
	TestTrue(TEXT("applied"), O->GetBoolField(TEXT("applied_in_memory")));
	TestEqual(TEXT("compile failed"), O->GetStringField(TEXT("compile_status")), FString(TEXT("failed")));
	TestTrue(TEXT("error count"), O->GetNumberField(TEXT("compile_error_count")) >= 1);
	TestEqual(TEXT("policy: saved anyway"), O->GetStringField(TEXT("save_status")), FString(TEXT("saved")));
	TestEqual(TEXT("policy named"), O->GetStringField(TEXT("save_policy")), FString(KismetAISavePolicy::AutosaveEvenIfCompileFailed));
	TestEqual(TEXT("one save"), SaveCalls, 1);
	TestTrue(TEXT("summary says the saved file does not compile"),
		O->GetStringField(TEXT("summary")).Contains(TEXT("does not compile")));

	// Model string -> extracted outcome -> ledger -> end-of-run event.
	FKismetAIMutationOutcome Extracted;
	TestTrue(TEXT("extractable from the model string"),
		FKismetAIMutationOutcome::TryExtractFromToolResult(R.ToModelString(), Extracted));
	TestEqual(TEXT("extracted compile failed"), (int32)Extracted.CompileStatus, (int32)EKismetAICompileStatus::Failed);
	FKismetAIRunOutcomeLedger Ledger;
	Ledger.Record(TEXT("manage_blueprint"), Extracted);
	TestTrue(TEXT("ledger names the package"), IsExactly(Ledger.GetCompileFailedPackages(), BP->GetOutermost()->GetName()));
	TestTrue(TEXT("event needs attention"), Ledger.BuildEventJson()->GetBoolField(TEXT("needs_attention")));

	// HTTP route: same compile state.
	const FString Http = Module.ExecuteHandlerDirect(&FKismetAIModule::HandleManageBlueprint, AddVariableArgs(BP, TEXT("P001_WhileBroken2")));
	FKismetAIMutationOutcome HttpOutcome;
	TestTrue(TEXT("http outcome extractable"), FKismetAIMutationOutcome::TryExtractFromToolResult(Http, HttpOutcome));
	TestEqual(TEXT("http compile failed"), (int32)HttpOutcome.CompileStatus, (int32)EKismetAICompileStatus::Failed);

	// Layout after a failed compile does not compile; the ledger keeps the failure.
	TSharedPtr<FJsonObject> LayoutArgs = MakeShared<FJsonObject>();
	LayoutArgs->SetStringField(TEXT("blueprint"), BP->GetPathName());
	const FToolExecutionResult L = Module.DoLayoutGraph(LayoutArgs);
	FKismetAIMutationOutcome LayoutOutcome;
	if (L.bSuccess && FKismetAIMutationOutcome::TryExtractFromToolResult(L.ToModelString(), LayoutOutcome))
	{
		TestEqual(TEXT("layout: compile not run"), (int32)LayoutOutcome.CompileStatus, (int32)EKismetAICompileStatus::NotRun);
		Ledger.Record(TEXT("layout_graph"), LayoutOutcome);
		TestEqual(TEXT("ledger still reports the compile failure"), Ledger.GetCompileFailedPackages().Num(), 1);
	}
	else
	{
		AddError(FString::Printf(TEXT("layout_graph did not return an outcome: %s"), *L.ToModelString().Left(400)));
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Real disk: save, reopen from disk, pre-dirty disclosure with the human edit
// preserved, then a read-only file -> changed-but-unsaved, and the disk copy
// is shown to differ from the live editor graph.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationOutcome_DiskRoundTrip, "KismetAI.MutationOutcome.DiskRoundTrip", MutationOutcomeTestFlags)
bool FMutationOutcome_DiskRoundTrip::RunTest(const FString& Parameters)
{
	const FString PackageName = DisposablePackageName(TEXT("BP_P001_Disk"));
	UBlueprint* BP = MakeBlueprint(PackageName, TEXT("BP_P001_Disk"));
	if (!TestNotNull(TEXT("fixture"), BP)) return false;
	const FString PackageFile = FPaths::ConvertRelativePathToFull(
		FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()));
	const FString PackageDir = FPaths::GetPath(PackageFile);
	ON_SCOPE_EXIT
	{
		DisposeBlueprint(BP);
		IFileManager::Get().Delete(*PackageFile, false, /*EvenReadOnly*/ true, true);
		IFileManager::Get().DeleteDirectory(*PackageDir, false, true);
	};
	TestTrue(TEXT("fixture lives in the disposable test folder"), PackageFile.Contains(TEXT("__KismetAITest_P001")));

	KismetAIPersistence::FScopedEvalModeOverride NotEval(false);
	FKismetAIModule& Module = FKismetAIModule::Get();

	// 1. Baseline: explicit save through the same boundary.
	{
		FKismetAIMutationOutcome Baseline;
		KismetAIPersistence::SaveExplicitly(BP, Baseline);
		if (!TestEqual(TEXT("baseline saved"), (int32)Baseline.SaveStatus, (int32)EKismetAISaveStatus::Saved)) return false;
		TestTrue(TEXT("file exists"), IFileManager::Get().FileExists(*PackageFile));
		TestFalse(TEXT("package clean after save"), BP->GetOutermost()->IsDirty());
	}

	// 2. A "human" edit the user has not saved.
	FEdGraphPinType BoolType;
	BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	FBlueprintEditorUtils::AddMemberVariable(BP, FName(TEXT("P001_HumanVar")), BoolType);
	BP->MarkPackageDirty();

	// 3. Agent edit with a real save: discloses that the human edit went too,
	//    and the disk copy proves both are there.
	{
		const FToolExecutionResult R = Module.DoManageBlueprint(AddVariableArgs(BP, TEXT("P001_AgentVar")));
		const TSharedPtr<FJsonObject> O = GetOutcomeJson(R);
		if (!TestTrue(TEXT("outcome attached"), O.IsValid())) return false;
		TestEqual(TEXT("saved"), O->GetStringField(TEXT("save_status")), FString(TEXT("saved")));
		TestTrue(TEXT("pre-dirty disclosed"), O->GetBoolField(TEXT("package_was_dirty_before")));
		TestTrue(TEXT("whole-package implication flagged"), O->GetBoolField(TEXT("save_included_preexisting_changes")));
		TestTrue(TEXT("summary explains it"), O->GetStringField(TEXT("summary")).Contains(TEXT("already had unsaved changes")));
		TestEqual(TEXT("filename reported"), O->GetStringField(TEXT("package_file")), PackageFile);

		FDiskSnapshot Disk;
		LoadFromDisk(PackageFile, Disk);
		if (TestNotNull(TEXT("reopened from disk"), Disk.Blueprint))
		{
			TestTrue(TEXT("disk has the agent's change"), HasVariable(Disk.Blueprint, TEXT("P001_AgentVar")));
			TestTrue(TEXT("disk preserved the human change"), HasVariable(Disk.Blueprint, TEXT("P001_HumanVar")));
		}
	}

	// 4. Read-only file: the edit applies in memory, the save fails, and the
	//    disk copy does not have it.
	if (!TestTrue(TEXT("made read-only"), FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*PackageFile, true))) return false;
	AddExpectedMessage(TEXT("Save failed for"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	{
		const FToolExecutionResult R = Module.DoManageBlueprint(AddVariableArgs(BP, TEXT("P001_ReadOnlyVar")));
		TestTrue(TEXT("outer success: applied"), R.bSuccess);
		const TSharedPtr<FJsonObject> O = GetOutcomeJson(R);
		if (!TestTrue(TEXT("outcome attached"), O.IsValid())) return false;
		TestTrue(TEXT("applied in memory"), O->GetBoolField(TEXT("applied_in_memory")));
		TestEqual(TEXT("save failed"), O->GetStringField(TEXT("save_status")), FString(TEXT("failed")));
		TestTrue(TEXT("reason names read-only"), O->GetStringField(TEXT("save_error")).Contains(TEXT("read-only")));
		TestFalse(TEXT("no persisted-success text"), O->GetStringField(TEXT("summary")).Contains(TEXT("to disk.")));
		TestTrue(TEXT("editor has the change"), HasVariable(BP, TEXT("P001_ReadOnlyVar")));
		TestTrue(TEXT("package left dirty"), BP->GetOutermost()->IsDirty());

		FDiskSnapshot Disk;
		LoadFromDisk(PackageFile, Disk);
		if (TestNotNull(TEXT("reopened from disk"), Disk.Blueprint))
		{
			TestFalse(TEXT("disk does NOT have the unsaved change"), HasVariable(Disk.Blueprint, TEXT("P001_ReadOnlyVar")));
			TestTrue(TEXT("disk still has the last saved state"), HasVariable(Disk.Blueprint, TEXT("P001_AgentVar")));
		}
	}
	FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*PackageFile, false);
	return true;
}
