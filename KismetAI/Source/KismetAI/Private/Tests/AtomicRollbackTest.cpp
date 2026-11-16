// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// An atomic edit batch that fails must leave the asset exactly as it
// found it, or say plainly that it did not.
//
// Each test snapshots a disposable /Temp asset (node set, identities, GUIDs,
// links, pin defaults, properties), runs a batch whose last operation is
// guaranteed to fail, and compares the live state with the baseline — the
// response fields alone are not evidence. Human transactions placed before
// the batch are checked in the real undo buffer.

#include "Misc/AutomationTest.h"
#include "KismetAIModule.h"
#include "KismetAIBatchRecovery.h"
#include "KismetAIToolResult.h"
#include "KismetAIMutationOutcome.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Package.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "MaterialEditingLibrary.h"
#include "SceneTypes.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "ScopedTransaction.h"

constexpr auto AtomicRollbackTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace AtomicRollbackTest
{
	struct FBlueprintFixture
	{
		UBlueprint* Blueprint = nullptr;
		UEdGraph* Graph = nullptr;
		UK2Node_CallFunction* PrintA = nullptr;
		UK2Node_CallFunction* PrintB = nullptr;
		UK2Node_CallFunction* MathNode = nullptr;
	};

	static UK2Node_CallFunction* AddCallNode(UEdGraph* Graph, UClass* Library, FName Function, int32 PosX)
	{
		// RF_Transactional like every editor-created node (UEdGraph::CreateNode,
		// the T3D importer), so a human Modify() is recorded too.
		UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph, NAME_None, RF_Transactional);
		Node->FunctionReference.SetExternalMember(Function, Library);
		Graph->AddNode(Node, false, false);
		Node->CreateNewGuid();
		Node->NodePosX = PosX;
		Node->AllocateDefaultPins();
		// What FGraphNodeCreator does for every editor-placed node; sets derived
		// state such as EnabledState=DevelopmentOnly for PrintString.
		Node->PostPlacedNewNode();
		return Node;
	}

	// Two PrintString nodes wired A.then -> B.execute, plus a math node whose
	// float output can never feed an exec pin (a guaranteed wiring failure).
	static bool MakeBlueprintFixture(const TCHAR* Name, FBlueprintFixture& Out)
	{
		const FString PackagePath = FString::Printf(TEXT("/Temp/KismetAtomicRollback/%s"), Name);
		UPackage* Pkg = CreatePackage(*PackagePath);
		if (!Pkg)
		{
			return false;
		}
		Out.Blueprint = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Pkg, Name, BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (!Out.Blueprint || Out.Blueprint->UbergraphPages.Num() == 0)
		{
			return false;
		}
		Out.Graph = Out.Blueprint->UbergraphPages[0];
		Out.PrintA = AddCallNode(Out.Graph, UKismetSystemLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString), 400);
		Out.PrintB = AddCallNode(Out.Graph, UKismetSystemLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString), 800);
		Out.MathNode = AddCallNode(Out.Graph, UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Abs), 400);

		UEdGraphPin* AThen = Out.PrintA->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
		UEdGraphPin* BExec = Out.PrintB->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
		if (!AThen || !BExec || !Out.Graph->GetSchema()->TryCreateConnection(AThen, BExec))
		{
			return false;
		}
		UEdGraphPin* BString = Out.PrintB->FindPin(TEXT("InString"), EGPD_Input);
		if (BString)
		{
			BString->DefaultValue = TEXT("baseline");
		}
		return true;
	}

	static TSharedRef<FJsonValueObject> Op(std::initializer_list<TPair<FString, FString>> Fields)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& F : Fields)
		{
			O->SetStringField(F.Key, F.Value);
		}
		return MakeShared<FJsonValueObject>(O);
	}

	static FToolExecutionResult RunEditGraph(UBlueprint* Blueprint, const TArray<TSharedPtr<FJsonValue>>& Ops, bool bAtomic = true)
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
		Body->SetArrayField(TEXT("operations"), Ops);
		Body->SetBoolField(TEXT("atomic"), bAtomic);
		return FKismetAIModule::Get().DoEditGraph(Body);
	}

	static FToolExecutionResult RunEditMaterial(UMaterial* Material, const TArray<TSharedPtr<FJsonValue>>& Ops, bool bAtomic = true)
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("material"), Material->GetPathName());
		Body->SetArrayField(TEXT("operations"), Ops);
		Body->SetBoolField(TEXT("atomic"), bAtomic);
		return FKismetAIModule::Get().DoEditMaterial(Body);
	}

	// The baseline-equality assertion every rollback test makes. Logs each
	// difference so a failure says exactly what survived the "rollback".
	static bool ExpectSnapshotEqual(FAutomationTestBase& Test, const TCHAR* What,
		const FKismetAIStateSnapshot& Before, const FKismetAIStateSnapshot& After)
	{
		TArray<FString> Diffs;
		const bool bEqual = FKismetAIStateSnapshot::Compare(Before, After, Diffs);
		for (const FString& D : Diffs)
		{
			Test.AddInfo(FString::Printf(TEXT("%s: %s"), What, *D));
		}
		Test.TestTrue(FString::Printf(TEXT("%s equals the pre-batch baseline"), What), bEqual);
		return bEqual;
	}

	static const TSharedPtr<FJsonObject>* Outcome(const FToolExecutionResult& R)
	{
		const TSharedPtr<FJsonObject>* O = nullptr;
		if (R.Data.IsValid() && R.Data->TryGetObjectField(TEXT("outcome"), O))
		{
			return O;
		}
		return nullptr;
	}

	// rolled_back=true, recovery_status=restored, nothing applied or saved.
	static void ExpectRestoredOutcome(FAutomationTestBase& Test, const FToolExecutionResult& R)
	{
		Test.TestTrue(TEXT("tool envelope is success"), R.bSuccess);
		if (!R.Data.IsValid())
		{
			Test.AddError(TEXT("no payload"));
			return;
		}
		bool bRolledBack = false;
		R.Data->TryGetBoolField(TEXT("rolled_back"), bRolledBack);
		Test.TestTrue(TEXT("rolled_back=true after verified restore"), bRolledBack);
		Test.TestEqual(TEXT("payload recovery_status"), R.Data->GetStringField(TEXT("recovery_status")), FString(TEXT("restored")));
		Test.TestTrue(TEXT("payload retry_safe"), R.Data->GetBoolField(TEXT("retry_safe")));
		const TSharedPtr<FJsonObject>* O = Outcome(R);
		if (!O)
		{
			Test.AddError(TEXT("no data.outcome"));
			return;
		}
		Test.TestEqual(TEXT("outcome.recovery_status"), (*O)->GetStringField(TEXT("recovery_status")), FString(TEXT("restored")));
		Test.TestTrue(TEXT("outcome.retry_safe"), (*O)->GetBoolField(TEXT("retry_safe")));
		Test.TestFalse(TEXT("outcome.applied_in_memory"), (*O)->GetBoolField(TEXT("applied_in_memory")));
		Test.TestEqual(TEXT("outcome.save_status"), (*O)->GetStringField(TEXT("save_status")), FString(TEXT("not_requested")));
		Test.TestEqual(TEXT("outcome.save_skip_reason"), (*O)->GetStringField(TEXT("save_skip_reason")), FString(TEXT("nothing_applied")));
	}

	struct FMaterialFixture
	{
		UMaterial* Material = nullptr;
		UMaterialExpressionConstant3Vector* Color = nullptr;
		UMaterialExpressionMultiply* Multiply = nullptr;
		UMaterialExpressionScalarParameter* Scalar = nullptr;
	};

	// Color -> Multiply.A, Scalar -> Multiply.B, Multiply -> BaseColor.
	static bool MakeMaterialFixture(const TCHAR* Name, FMaterialFixture& Out)
	{
		const FString PackagePath = FString::Printf(TEXT("/Temp/KismetAtomicRollback/%s"), Name);
		UPackage* Pkg = CreatePackage(*PackagePath);
		if (!Pkg)
		{
			return false;
		}
		Out.Material = NewObject<UMaterial>(Pkg, Name, RF_Public | RF_Standalone | RF_Transactional);
		if (!Out.Material)
		{
			return false;
		}
		Out.Color = Cast<UMaterialExpressionConstant3Vector>(UMaterialEditingLibrary::CreateMaterialExpression(
			Out.Material, UMaterialExpressionConstant3Vector::StaticClass(), -600, 0));
		Out.Multiply = Cast<UMaterialExpressionMultiply>(UMaterialEditingLibrary::CreateMaterialExpression(
			Out.Material, UMaterialExpressionMultiply::StaticClass(), -300, 0));
		Out.Scalar = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(
			Out.Material, UMaterialExpressionScalarParameter::StaticClass(), -600, 200));
		if (!Out.Color || !Out.Multiply || !Out.Scalar)
		{
			return false;
		}
		Out.Color->Constant = FLinearColor(1.f, 0.f, 0.f, 1.f);
		Out.Scalar->ParameterName = TEXT("Brightness");
		Out.Scalar->DefaultValue = 0.5f;
		return UMaterialEditingLibrary::ConnectMaterialExpressions(Out.Color, TEXT(""), Out.Multiply, TEXT("A"))
			&& UMaterialEditingLibrary::ConnectMaterialExpressions(Out.Scalar, TEXT(""), Out.Multiply, TEXT("B"))
			&& UMaterialEditingLibrary::ConnectMaterialProperty(Out.Multiply, TEXT(""), MP_BaseColor);
	}
}

using namespace AtomicRollbackTest;

// ─────────────────────────────────────────────────────────────────────────────
// The comparison helper itself: two captures of an untouched asset are equal,
// and a one-field change is reported.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAtomicRollback_SnapshotHelper, "KismetAI.AtomicRollback.SnapshotHelper", AtomicRollbackTestFlags)
bool FAtomicRollback_SnapshotHelper::RunTest(const FString& Parameters)
{
	FBlueprintFixture F;
	if (!MakeBlueprintFixture(TEXT("BP_AtomicSnapshot"), F))
	{
		AddError(TEXT("could not build blueprint fixture"));
		return false;
	}
	const FKismetAIStateSnapshot A = KismetAIBatchRecovery::CaptureGraph(F.Graph);
	const FKismetAIStateSnapshot B = KismetAIBatchRecovery::CaptureGraph(F.Graph);
	TestFalse(TEXT("graph snapshot is not empty"), A.IsEmpty());
	ExpectSnapshotEqual(*this, TEXT("untouched graph"), A, B);

	F.PrintB->FindPin(TEXT("InString"), EGPD_Input)->DefaultValue = TEXT("changed");
	TArray<FString> Diffs;
	TestFalse(TEXT("a changed pin default is detected"),
		FKismetAIStateSnapshot::Compare(A, KismetAIBatchRecovery::CaptureGraph(F.Graph), Diffs));
	TestEqual(TEXT("exactly one difference"), Diffs.Num(), 1);

	FMaterialFixture M;
	if (!MakeMaterialFixture(TEXT("M_AtomicSnapshot"), M))
	{
		AddError(TEXT("could not build material fixture"));
		return false;
	}
	const FKismetAIStateSnapshot MA = KismetAIBatchRecovery::CaptureMaterial(M.Material);
	ExpectSnapshotEqual(*this, TEXT("untouched material"), MA, KismetAIBatchRecovery::CaptureMaterial(M.Material));
	M.Multiply->A.Expression = nullptr;
	Diffs.Reset();
	TestFalse(TEXT("a broken expression input is detected"),
		FKismetAIStateSnapshot::Compare(MA, KismetAIBatchRecovery::CaptureMaterial(M.Material), Diffs));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Blueprint: a valid edit, then an operation on a node that does not exist.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAtomicRollback_BlueprintValidThenInvalid, "KismetAI.AtomicRollback.Blueprint_ValidThenInvalid", AtomicRollbackTestFlags)
bool FAtomicRollback_BlueprintValidThenInvalid::RunTest(const FString& Parameters)
{
	FBlueprintFixture F;
	if (!MakeBlueprintFixture(TEXT("BP_AtomicValidThenInvalid"), F))
	{
		AddError(TEXT("could not build blueprint fixture"));
		return false;
	}
	const FKismetAIStateSnapshot Baseline = KismetAIBatchRecovery::CaptureGraph(F.Graph);

	TArray<TSharedPtr<FJsonValue>> Ops;
	Ops.Add(Op({ {TEXT("action"), TEXT("set_pin_default")}, {TEXT("node"), F.PrintB->GetName()}, {TEXT("pin"), TEXT("InString")}, {TEXT("value"), TEXT("agent")} }));
	Ops.Add(Op({ {TEXT("action"), TEXT("disconnect")}, {TEXT("source_node"), F.PrintA->GetName()}, {TEXT("source_pin"), TEXT("then")}, {TEXT("target_node"), F.PrintB->GetName()}, {TEXT("target_pin"), TEXT("execute")} }));
	Ops.Add(Op({ {TEXT("action"), TEXT("connect")}, {TEXT("source_node"), TEXT("K2Node_DoesNotExist_0")}, {TEXT("source_pin"), TEXT("then")}, {TEXT("target_node"), F.PrintB->GetName()}, {TEXT("target_pin"), TEXT("execute")} }));

	const FToolExecutionResult R = RunEditGraph(F.Blueprint, Ops);
	TestTrue(TEXT("handler returned a payload"), R.Data.IsValid());

	const bool bEqual = ExpectSnapshotEqual(*this, TEXT("graph after failed atomic batch"), Baseline, KismetAIBatchRecovery::CaptureGraph(F.Graph));
	bool bRolledBack = false;
	if (R.Data.IsValid())
	{
		R.Data->TryGetBoolField(TEXT("rolled_back"), bRolledBack);
	}
	// The contract: rolled_back=true is only allowed when the graph really is
	// back at baseline.
	TestTrue(TEXT("rolled_back=true only with a restored graph"), !bRolledBack || bEqual);
	ExpectRestoredOutcome(*this, R);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Blueprint: delete a connected node, then fail. The node must come back as
// the same object with the same GUID and its connection.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAtomicRollback_BlueprintDeleteThenFail, "KismetAI.AtomicRollback.Blueprint_DeleteConnectedThenFail", AtomicRollbackTestFlags)
bool FAtomicRollback_BlueprintDeleteThenFail::RunTest(const FString& Parameters)
{
	FBlueprintFixture F;
	if (!MakeBlueprintFixture(TEXT("BP_AtomicDeleteThenFail"), F))
	{
		AddError(TEXT("could not build blueprint fixture"));
		return false;
	}
	const FKismetAIStateSnapshot Baseline = KismetAIBatchRecovery::CaptureGraph(F.Graph);
	UK2Node_CallFunction* Deleted = F.PrintB;
	const FGuid DeletedGuid = Deleted->NodeGuid;
	const FString DeletedName = Deleted->GetName();

	TArray<TSharedPtr<FJsonValue>> Ops;
	Ops.Add(Op({ {TEXT("action"), TEXT("delete_node")}, {TEXT("node"), DeletedName} }));
	Ops.Add(Op({ {TEXT("action"), TEXT("delete_node")}, {TEXT("node"), TEXT("K2Node_DoesNotExist_0")} }));

	const FToolExecutionResult R = RunEditGraph(F.Blueprint, Ops);
	TestTrue(TEXT("handler returned a payload"), R.Data.IsValid());

	TestTrue(TEXT("deleted node is back in the graph (same object)"), F.Graph->Nodes.Contains(Deleted));
	TestEqual(TEXT("deleted node kept its GUID"), Deleted->NodeGuid, DeletedGuid);
	UEdGraphPin* AThen = F.PrintA->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* BExec = Deleted->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	TestTrue(TEXT("A.then -> B.execute link restored"), AThen && BExec && AThen->LinkedTo.Contains(BExec) && BExec->LinkedTo.Contains(AThen));
	ExpectSnapshotEqual(*this, TEXT("graph after delete-then-fail"), Baseline, KismetAIBatchRecovery::CaptureGraph(F.Graph));
	ExpectRestoredOutcome(*this, R);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Blueprint: change a pin default, then fail wiring validation (float -> exec).
// The default and the package's dirty flag must both return to baseline.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAtomicRollback_BlueprintPinDefaultThenWiringFails, "KismetAI.AtomicRollback.Blueprint_PinDefaultThenWiringFails", AtomicRollbackTestFlags)
bool FAtomicRollback_BlueprintPinDefaultThenWiringFails::RunTest(const FString& Parameters)
{
	FBlueprintFixture F;
	if (!MakeBlueprintFixture(TEXT("BP_AtomicPinDefaultThenWiring"), F))
	{
		AddError(TEXT("could not build blueprint fixture"));
		return false;
	}
	UPackage* Package = F.Blueprint->GetOutermost();
	Package->SetDirtyFlag(false);
	const FKismetAIStateSnapshot Baseline = KismetAIBatchRecovery::CaptureGraph(F.Graph);

	TArray<TSharedPtr<FJsonValue>> Ops;
	Ops.Add(Op({ {TEXT("action"), TEXT("set_pin_default")}, {TEXT("node"), F.PrintB->GetName()}, {TEXT("pin"), TEXT("InString")}, {TEXT("value"), TEXT("agent")} }));
	Ops.Add(Op({ {TEXT("action"), TEXT("connect")}, {TEXT("source_node"), F.MathNode->GetName()}, {TEXT("source_pin"), TEXT("ReturnValue")}, {TEXT("target_node"), F.PrintA->GetName()}, {TEXT("target_pin"), TEXT("execute")} }));

	const FToolExecutionResult R = RunEditGraph(F.Blueprint, Ops);
	TestTrue(TEXT("handler returned a payload"), R.Data.IsValid());

	UEdGraphPin* BString = F.PrintB->FindPin(TEXT("InString"), EGPD_Input);
	TestEqual(TEXT("pin default restored"), BString ? BString->DefaultValue : FString(), FString(TEXT("baseline")));
	ExpectSnapshotEqual(*this, TEXT("graph after pin-default-then-wiring-failure"), Baseline, KismetAIBatchRecovery::CaptureGraph(F.Graph));
	TestFalse(TEXT("package is clean again (it was clean before the batch)"), Package->IsDirty());
	ExpectRestoredOutcome(*this, R);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Material: valid edits (property, disconnect, delete, connect), then a
// reference to a node that does not exist.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAtomicRollback_MaterialValidThenInvalid, "KismetAI.AtomicRollback.Material_ValidThenInvalid", AtomicRollbackTestFlags)
bool FAtomicRollback_MaterialValidThenInvalid::RunTest(const FString& Parameters)
{
	FMaterialFixture M;
	if (!MakeMaterialFixture(TEXT("M_AtomicValidThenInvalid"), M))
	{
		AddError(TEXT("could not build material fixture"));
		return false;
	}
	const FKismetAIStateSnapshot Baseline = KismetAIBatchRecovery::CaptureMaterial(M.Material);
	const int32 ExpressionCount = UMaterialEditingLibrary::GetNumMaterialExpressions(M.Material);
	UMaterialExpressionScalarParameter* Scalar = M.Scalar;
	const FGuid ScalarGuid = Scalar->MaterialExpressionGuid;

	TArray<TSharedPtr<FJsonValue>> Ops;
	{
		TSharedPtr<FJsonObject> SetProp = MakeShared<FJsonObject>();
		SetProp->SetStringField(TEXT("action"), TEXT("set_property"));
		SetProp->SetStringField(TEXT("node"), M.Color->GetName());
		SetProp->SetStringField(TEXT("property"), TEXT("Constant"));
		SetProp->SetArrayField(TEXT("value"), { MakeShared<FJsonValueNumber>(0.0), MakeShared<FJsonValueNumber>(1.0), MakeShared<FJsonValueNumber>(0.0) });
		Ops.Add(MakeShared<FJsonValueObject>(SetProp));
	}
	Ops.Add(Op({ {TEXT("action"), TEXT("disconnect")}, {TEXT("to_node"), M.Multiply->GetName()}, {TEXT("to_input"), TEXT("A")} }));
	Ops.Add(Op({ {TEXT("action"), TEXT("delete_node")}, {TEXT("node"), Scalar->GetName()} }));
	Ops.Add(Op({ {TEXT("action"), TEXT("connect")}, {TEXT("from_node"), M.Color->GetName()}, {TEXT("to_property"), TEXT("EmissiveColor")} }));
	Ops.Add(Op({ {TEXT("action"), TEXT("connect")}, {TEXT("from_node"), TEXT("MaterialExpressionDoesNotExist_0")}, {TEXT("to_property"), TEXT("Roughness")} }));

	const FToolExecutionResult R = RunEditMaterial(M.Material, Ops);
	TestTrue(TEXT("handler returned a payload"), R.Data.IsValid());

	TestEqual(TEXT("expression count restored"), UMaterialEditingLibrary::GetNumMaterialExpressions(M.Material), ExpressionCount);
	TestTrue(TEXT("deleted parameter is back (same object)"), M.Material->GetExpressions().Contains(Scalar));
	TestEqual(TEXT("deleted parameter kept its GUID"), Scalar->MaterialExpressionGuid, ScalarGuid);
	TestTrue(TEXT("deleted parameter is not garbage"), IsValid(Scalar));
	TestTrue(TEXT("Multiply.A reconnected to Color"), M.Multiply->A.Expression == M.Color);
	TestTrue(TEXT("Multiply.B reconnected to Scalar"), M.Multiply->B.Expression == Scalar);
	TestEqual(TEXT("Color constant restored"), M.Color->Constant, FLinearColor(1.f, 0.f, 0.f, 1.f));
	TestTrue(TEXT("EmissiveColor still unconnected"), UMaterialEditingLibrary::GetMaterialPropertyInputNode(M.Material, MP_EmissiveColor) == nullptr);
	ExpectSnapshotEqual(*this, TEXT("material after failed atomic batch"), Baseline, KismetAIBatchRecovery::CaptureMaterial(M.Material));
	ExpectRestoredOutcome(*this, R);
	TestEqual(TEXT("material status"), R.Data.IsValid() ? R.Data->GetStringField(TEXT("status")) : FString(), FString(TEXT("rolled_back")));
	TestFalse(TEXT("material applied=false"), R.Data.IsValid() && R.Data->GetBoolField(TEXT("applied")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// A human transaction placed before the batch keeps its record and its
// effect, and undoing it afterwards still works.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAtomicRollback_HumanTransactionPreserved, "KismetAI.AtomicRollback.HumanTransactionPreserved", AtomicRollbackTestFlags)
bool FAtomicRollback_HumanTransactionPreserved::RunTest(const FString& Parameters)
{
	if (!GEditor || !GEditor->Trans)
	{
		AddError(TEXT("no editor transaction buffer"));
		return false;
	}
	FBlueprintFixture F;
	if (!MakeBlueprintFixture(TEXT("BP_AtomicHumanTransaction"), F))
	{
		AddError(TEXT("could not build blueprint fixture"));
		return false;
	}

	const FText HumanTitle = NSLOCTEXT("KismetAITest", "HumanEdit", "Human Edit (test)");
	{
		FScopedTransaction Human(HumanTitle);
		F.PrintA->Modify();
		F.PrintA->FindPin(TEXT("InString"), EGPD_Input)->DefaultValue = TEXT("human");
	}
	const int32 QueueBefore = GEditor->Trans->GetQueueLength();
	const int32 UndoCountBefore = GEditor->Trans->GetUndoCount();
	TestEqual(TEXT("human transaction is the newest undo entry"),
		GEditor->Trans->GetUndoContext(false).Title.ToString(), HumanTitle.ToString());
	const FKismetAIStateSnapshot Baseline = KismetAIBatchRecovery::CaptureGraph(F.Graph);

	TArray<TSharedPtr<FJsonValue>> Ops;
	Ops.Add(Op({ {TEXT("action"), TEXT("set_pin_default")}, {TEXT("node"), F.PrintA->GetName()}, {TEXT("pin"), TEXT("InString")}, {TEXT("value"), TEXT("agent")} }));
	Ops.Add(Op({ {TEXT("action"), TEXT("delete_node")}, {TEXT("node"), F.PrintB->GetName()} }));
	Ops.Add(Op({ {TEXT("action"), TEXT("delete_node")}, {TEXT("node"), TEXT("K2Node_DoesNotExist_0")} }));
	const FToolExecutionResult R = RunEditGraph(F.Blueprint, Ops);
	ExpectRestoredOutcome(*this, R);

	ExpectSnapshotEqual(*this, TEXT("graph after failed batch (human edit kept)"), Baseline, KismetAIBatchRecovery::CaptureGraph(F.Graph));
	TestEqual(TEXT("human edit still in effect"), F.PrintA->FindPin(TEXT("InString"), EGPD_Input)->DefaultValue, FString(TEXT("human")));
	TestEqual(TEXT("undo buffer length unchanged"), GEditor->Trans->GetQueueLength(), QueueBefore);
	TestEqual(TEXT("undo position unchanged"), GEditor->Trans->GetUndoCount(), UndoCountBefore);
	TestEqual(TEXT("human transaction is still the newest undo entry"),
		GEditor->Trans->GetUndoContext(false).Title.ToString(), HumanTitle.ToString());

	// Undoing the human transaction still works and only undoes the human edit.
	TestTrue(TEXT("undo of the human transaction ran"), GEditor->UndoTransaction());
	UEdGraphPin* AString = F.PrintA->FindPin(TEXT("InString"), EGPD_Input);
	TestTrue(TEXT("human edit undone"), AString && AString->DefaultValue != TEXT("human"));
	TestTrue(TEXT("PrintB still present after the human undo"), F.Graph->Nodes.Contains(F.PrintB));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Material: a human transaction before the batch survives the rollback too.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAtomicRollback_MaterialHumanTransactionPreserved, "KismetAI.AtomicRollback.Material_HumanTransactionPreserved", AtomicRollbackTestFlags)
bool FAtomicRollback_MaterialHumanTransactionPreserved::RunTest(const FString& Parameters)
{
	FMaterialFixture M;
	if (!GEditor || !GEditor->Trans || !MakeMaterialFixture(TEXT("M_AtomicHumanTransaction"), M))
	{
		AddError(TEXT("could not build material fixture"));
		return false;
	}
	const FText HumanTitle = NSLOCTEXT("KismetAITest", "HumanMaterialEdit", "Human Material Edit (test)");
	{
		FScopedTransaction Human(HumanTitle);
		M.Scalar->Modify();
		M.Scalar->DefaultValue = 0.75f;
	}
	const int32 QueueBefore = GEditor->Trans->GetQueueLength();
	const FKismetAIStateSnapshot Baseline = KismetAIBatchRecovery::CaptureMaterial(M.Material);

	TArray<TSharedPtr<FJsonValue>> Ops;
	Ops.Add(Op({ {TEXT("action"), TEXT("delete_node")}, {TEXT("node"), M.Scalar->GetName()} }));
	Ops.Add(Op({ {TEXT("action"), TEXT("delete_node")}, {TEXT("node"), TEXT("MaterialExpressionDoesNotExist_0")} }));
	const FToolExecutionResult R = RunEditMaterial(M.Material, Ops);
	ExpectRestoredOutcome(*this, R);

	ExpectSnapshotEqual(*this, TEXT("material after failed batch (human edit kept)"), Baseline, KismetAIBatchRecovery::CaptureMaterial(M.Material));
	TestEqual(TEXT("human edit still in effect"), M.Scalar->DefaultValue, 0.75f);
	TestEqual(TEXT("undo buffer length unchanged"), GEditor->Trans->GetQueueLength(), QueueBefore);
	TestEqual(TEXT("human transaction is still the newest undo entry"),
		GEditor->Trans->GetUndoContext(false).Title.ToString(), HumanTitle.ToString());
	TestTrue(TEXT("undo of the human transaction ran"), GEditor->UndoTransaction());
	TestEqual(TEXT("human edit undone"), M.Scalar->DefaultValue, 0.5f);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Forced restoration failure: the revert never runs. The result must say so —
// no rolled_back, restore_failed, retry unsafe, the succeeded operations
// reported as still applied — and the batch stays undoable by a human.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAtomicRollback_ForcedRestoreFailure, "KismetAI.AtomicRollback.ForcedRestoreFailure", AtomicRollbackTestFlags)
bool FAtomicRollback_ForcedRestoreFailure::RunTest(const FString& Parameters)
{
	FBlueprintFixture F;
	if (!GEditor || !GEditor->Trans || !MakeBlueprintFixture(TEXT("BP_AtomicForcedFailure"), F))
	{
		AddError(TEXT("could not build blueprint fixture"));
		return false;
	}
	UPackage* Package = F.Blueprint->GetOutermost();
	Package->SetDirtyFlag(false);
	const FKismetAIStateSnapshot Baseline = KismetAIBatchRecovery::CaptureGraph(F.Graph);
	const int32 QueueBefore = GEditor->Trans->GetQueueLength();

	TArray<TSharedPtr<FJsonValue>> Ops;
	Ops.Add(Op({ {TEXT("action"), TEXT("set_pin_default")}, {TEXT("node"), F.PrintB->GetName()}, {TEXT("pin"), TEXT("InString")}, {TEXT("value"), TEXT("agent")} }));
	Ops.Add(Op({ {TEXT("action"), TEXT("delete_node")}, {TEXT("node"), TEXT("K2Node_DoesNotExist_0")} }));

	FToolExecutionResult R;
	{
		KismetAIBatchRecovery::FScopedRestoreFaultInjection Fault(KismetAIBatchRecovery::ERestoreFault::SkipRevert);
		R = RunEditGraph(F.Blueprint, Ops);
	}
	if (!R.Data.IsValid())
	{
		AddError(TEXT("no payload"));
		return false;
	}
	bool bRolledBack = true;
	R.Data->TryGetBoolField(TEXT("rolled_back"), bRolledBack);
	TestFalse(TEXT("rolled_back is not claimed"), bRolledBack);
	TestEqual(TEXT("recovery_status"), R.Data->GetStringField(TEXT("recovery_status")), FString(TEXT("restore_failed")));
	TestFalse(TEXT("retry_safe=false"), R.Data->GetBoolField(TEXT("retry_safe")));
	TestEqual(TEXT("the succeeded operation is reported as applied"), (int32)R.Data->GetNumberField(TEXT("operations_succeeded")), 1);
	TestTrue(TEXT("undo entry kept for the human"), R.Data->GetBoolField(TEXT("undo_entry_kept")));
	const TArray<TSharedPtr<FJsonValue>>* Diffs = nullptr;
	TestTrue(TEXT("restore_differences listed"), R.Data->TryGetArrayField(TEXT("restore_differences"), Diffs) && Diffs->Num() > 0);
	TestTrue(TEXT("message forbids resending"), R.Data->GetStringField(TEXT("message")).Contains(TEXT("Do NOT resend")));

	const TSharedPtr<FJsonObject>* O = Outcome(R);
	TestTrue(TEXT("outcome present"), O != nullptr);
	if (O)
	{
		TestEqual(TEXT("outcome.recovery_status"), (*O)->GetStringField(TEXT("recovery_status")), FString(TEXT("restore_failed")));
		TestFalse(TEXT("outcome.retry_safe"), (*O)->GetBoolField(TEXT("retry_safe")));
		TestTrue(TEXT("outcome.applied_in_memory"), (*O)->GetBoolField(TEXT("applied_in_memory")));
		TestEqual(TEXT("not saved"), (*O)->GetStringField(TEXT("save_status")), FString(TEXT("not_requested")));
		TestEqual(TEXT("save skipped because restore incomplete"), (*O)->GetStringField(TEXT("save_skip_reason")), FString(TEXT("restore_incomplete")));
		TestTrue(TEXT("summary leads with ROLLBACK FAILED"), (*O)->GetStringField(TEXT("summary")).StartsWith(TEXT("ROLLBACK FAILED")));
	}

	// The state really is the half-applied one the result describes...
	TArray<FString> Live;
	TestFalse(TEXT("graph differs from baseline (as reported)"),
		FKismetAIStateSnapshot::Compare(Baseline, KismetAIBatchRecovery::CaptureGraph(F.Graph), Live));
	// ...and the batch is an ordinary undo entry a human can undo.
	TestEqual(TEXT("one new undo entry (the batch)"), GEditor->Trans->GetQueueLength(), QueueBefore + 1);
	TestTrue(TEXT("human undo of the batch ran"), GEditor->UndoTransaction());
	ExpectSnapshotEqual(*this, TEXT("graph after human undo of the kept batch"), Baseline, KismetAIBatchRecovery::CaptureGraph(F.Graph));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Forced partial restoration: the revert runs but something it did not cover
// is left changed. Verification must catch it: partially_restored, unsafe.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAtomicRollback_ForcedPartialRestore, "KismetAI.AtomicRollback.ForcedPartialRestore", AtomicRollbackTestFlags)
bool FAtomicRollback_ForcedPartialRestore::RunTest(const FString& Parameters)
{
	FMaterialFixture M;
	if (!MakeMaterialFixture(TEXT("M_AtomicForcedPartial"), M))
	{
		AddError(TEXT("could not build material fixture"));
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Ops;
	Ops.Add(Op({ {TEXT("action"), TEXT("disconnect")}, {TEXT("to_node"), M.Multiply->GetName()}, {TEXT("to_input"), TEXT("A")} }));
	Ops.Add(Op({ {TEXT("action"), TEXT("delete_node")}, {TEXT("node"), TEXT("MaterialExpressionDoesNotExist_0")} }));

	UMaterialExpressionMultiply* Multiply = M.Multiply;
	FToolExecutionResult R;
	{
		// Stands in for state the transaction record did not hold.
		KismetAIBatchRecovery::FScopedRestoreFaultInjection Fault(KismetAIBatchRecovery::ERestoreFault::AfterRevert,
			[Multiply]() { Multiply->B.Expression = nullptr; });
		R = RunEditMaterial(M.Material, Ops);
	}
	if (!R.Data.IsValid())
	{
		AddError(TEXT("no payload"));
		return false;
	}
	bool bRolledBack = true;
	R.Data->TryGetBoolField(TEXT("rolled_back"), bRolledBack);
	TestFalse(TEXT("rolled_back is not claimed"), bRolledBack);
	TestEqual(TEXT("status"), R.Data->GetStringField(TEXT("status")), FString(TEXT("rollback_incomplete")));
	TestTrue(TEXT("applied=true (part of the batch may remain)"), R.Data->GetBoolField(TEXT("applied")));
	TestEqual(TEXT("recovery_status"), R.Data->GetStringField(TEXT("recovery_status")), FString(TEXT("partially_restored")));
	TestFalse(TEXT("retry_safe=false"), R.Data->GetBoolField(TEXT("retry_safe")));
	TestFalse(TEXT("revert consumed the undo entry"), R.Data->GetBoolField(TEXT("undo_entry_kept")));
	const TSharedPtr<FJsonObject>* O = Outcome(R);
	if (O)
	{
		TestEqual(TEXT("outcome.recovery_status"), (*O)->GetStringField(TEXT("recovery_status")), FString(TEXT("partially_restored")));
		TestFalse(TEXT("outcome.retry_safe"), (*O)->GetBoolField(TEXT("retry_safe")));
		TestTrue(TEXT("outcome.applied_in_memory"), (*O)->GetBoolField(TEXT("applied_in_memory")));
	}
	else
	{
		AddError(TEXT("no data.outcome"));
	}
	// The part the revert did cover is restored.
	TestTrue(TEXT("Multiply.A reconnected by the revert"), M.Multiply->A.Expression == M.Color);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// An atomic batch inside someone else's open transaction cannot be isolated:
// it is refused before anything changes, and the outer transaction survives.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAtomicRollback_NestedTransactionRefused, "KismetAI.AtomicRollback.NestedTransactionRefused", AtomicRollbackTestFlags)
bool FAtomicRollback_NestedTransactionRefused::RunTest(const FString& Parameters)
{
	FBlueprintFixture F;
	if (!GEditor || !GEditor->Trans || !MakeBlueprintFixture(TEXT("BP_AtomicNested"), F))
	{
		AddError(TEXT("could not build blueprint fixture"));
		return false;
	}
	const FKismetAIStateSnapshot Baseline = KismetAIBatchRecovery::CaptureGraph(F.Graph);
	const FText OuterTitle = NSLOCTEXT("KismetAITest", "OuterEdit", "Outer Human Edit (test)");
	const int32 QueueBefore = GEditor->Trans->GetQueueLength();

	TArray<TSharedPtr<FJsonValue>> Ops;
	Ops.Add(Op({ {TEXT("action"), TEXT("set_pin_default")}, {TEXT("node"), F.PrintB->GetName()}, {TEXT("pin"), TEXT("InString")}, {TEXT("value"), TEXT("agent")} }));
	Ops.Add(Op({ {TEXT("action"), TEXT("delete_node")}, {TEXT("node"), TEXT("K2Node_DoesNotExist_0")} }));

	FToolExecutionResult Atomic;
	{
		FScopedTransaction Outer(OuterTitle);
		F.PrintA->Modify();
		F.PrintA->FindPin(TEXT("InString"), EGPD_Input)->DefaultValue = TEXT("outer");
		Atomic = RunEditGraph(F.Blueprint, Ops);
	}
	TestFalse(TEXT("atomic batch refused inside an open transaction"), Atomic.bSuccess);
	TestEqual(TEXT("error code"), Atomic.ErrorCode, FString(TEXT("TRANSACTION_ACTIVE")));
	TestEqual(TEXT("batch did not touch the graph"), F.PrintB->FindPin(TEXT("InString"), EGPD_Input)->DefaultValue, FString(TEXT("baseline")));
	TestEqual(TEXT("outer edit intact"), F.PrintA->FindPin(TEXT("InString"), EGPD_Input)->DefaultValue, FString(TEXT("outer")));
	TestEqual(TEXT("outer transaction committed"), GEditor->Trans->GetQueueLength(), QueueBefore + 1);
	TestEqual(TEXT("outer transaction is the newest entry"), GEditor->Trans->GetUndoContext(false).Title.ToString(), OuterTitle.ToString());
	TestTrue(TEXT("undo of the outer transaction ran"), GEditor->UndoTransaction());
	ExpectSnapshotEqual(*this, TEXT("graph after undoing the outer transaction"), Baseline, KismetAIBatchRecovery::CaptureGraph(F.Graph));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Non-atomic batches keep best-effort semantics: what succeeds stays applied
// and no recovery is claimed.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAtomicRollback_NonAtomicUnchanged, "KismetAI.AtomicRollback.NonAtomicBestEffort", AtomicRollbackTestFlags)
bool FAtomicRollback_NonAtomicUnchanged::RunTest(const FString& Parameters)
{
	FBlueprintFixture F;
	if (!MakeBlueprintFixture(TEXT("BP_AtomicNonAtomic"), F))
	{
		AddError(TEXT("could not build blueprint fixture"));
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Ops;
	Ops.Add(Op({ {TEXT("action"), TEXT("set_pin_default")}, {TEXT("node"), F.PrintB->GetName()}, {TEXT("pin"), TEXT("InString")}, {TEXT("value"), TEXT("agent")} }));
	Ops.Add(Op({ {TEXT("action"), TEXT("delete_node")}, {TEXT("node"), TEXT("K2Node_DoesNotExist_0")} }));
	const FToolExecutionResult R = RunEditGraph(F.Blueprint, Ops, /*bAtomic=*/false);
	TestTrue(TEXT("success envelope"), R.bSuccess);
	TestEqual(TEXT("valid op applied"), F.PrintB->FindPin(TEXT("InString"), EGPD_Input)->DefaultValue, FString(TEXT("agent")));
	TestFalse(TEXT("no rolled_back field"), R.Data.IsValid() && R.Data->HasField(TEXT("rolled_back")));
	const TSharedPtr<FJsonObject>* O = Outcome(R);
	TestTrue(TEXT("recovery not tracked"), O && (*O)->GetStringField(TEXT("recovery_status")) == TEXT("not_tracked"));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Outcome contract: the recovery states round-trip through data.outcome, only
// a verified restore is retry-safe, and the run ledger keeps an incomplete
// rollback visible even after later edits to the same package.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAtomicRollback_OutcomeContract, "KismetAI.AtomicRollback.OutcomeContract", AtomicRollbackTestFlags)
bool FAtomicRollback_OutcomeContract::RunTest(const FString& Parameters)
{
	const EKismetAIRecoveryStatus All[] = {
		EKismetAIRecoveryStatus::NotTracked, EKismetAIRecoveryStatus::Restored,
		EKismetAIRecoveryStatus::PartiallyRestored, EKismetAIRecoveryStatus::RestoreFailed };
	for (EKismetAIRecoveryStatus Status : All)
	{
		FKismetAIMutationOutcome O;
		O.PackageName = TEXT("/Game/P002");
		O.RecoveryStatus = Status;
		O.bAppliedInMemory = Status != EKismetAIRecoveryStatus::Restored && Status != EKismetAIRecoveryStatus::NotTracked;
		const TSharedPtr<FJsonObject> J = O.ToJson();
		FKismetAIMutationOutcome Back;
		TestTrue(TEXT("round-trips"), FKismetAIMutationOutcome::FromJson(J, Back));
		TestEqual(TEXT("recovery status survives"), (int32)Back.RecoveryStatus, (int32)Status);
		TestEqual(TEXT("retry_safe present only when tracked"), J->HasField(TEXT("retry_safe")), Status != EKismetAIRecoveryStatus::NotTracked);
		const bool bSafe = Status == EKismetAIRecoveryStatus::Restored || Status == EKismetAIRecoveryStatus::NotTracked;
		TestEqual(TEXT("retry safe only when nothing is uncertain"), Back.IsRetrySafe(), bSafe);
		TestEqual(TEXT("needs attention when uncertain"), Back.NeedsAttention(), !bSafe);
	}

	// An unknown future value must not read as safe.
	TSharedPtr<FJsonObject> Unknown = FKismetAIMutationOutcome().ToJson();
	Unknown->SetStringField(TEXT("recovery_status"), TEXT("something_new"));
	FKismetAIMutationOutcome Parsed;
	TestTrue(TEXT("parses"), FKismetAIMutationOutcome::FromJson(Unknown, Parsed));
	TestFalse(TEXT("unknown recovery status is not retry-safe"), Parsed.IsRetrySafe());

	FKismetAIRunOutcomeLedger Ledger;
	FKismetAIMutationOutcome Partial;
	Partial.PackageName = TEXT("/Game/P002");
	Partial.bAppliedInMemory = true;
	Partial.RecoveryStatus = EKismetAIRecoveryStatus::PartiallyRestored;
	Ledger.Record(TEXT("edit_blueprint"), Partial);
	FKismetAIMutationOutcome Later;
	Later.PackageName = TEXT("/Game/P002");
	Later.bAppliedInMemory = true;
	Later.CompileStatus = EKismetAICompileStatus::Succeeded;
	Ledger.Record(TEXT("edit_blueprint"), Later);
	TestEqual(TEXT("unrestored package stays listed"), Ledger.GetUnrestoredPackages().Num(), 1);
	TestTrue(TEXT("ledger needs attention"), Ledger.NeedsAttention());
	TestTrue(TEXT("event lists unrestored_packages"), Ledger.BuildEventJson()->HasField(TEXT("unrestored_packages")));

	FKismetAIRunOutcomeLedger CleanLedger;
	FKismetAIMutationOutcome Restored;
	Restored.PackageName = TEXT("/Game/P002");
	Restored.RecoveryStatus = EKismetAIRecoveryStatus::Restored;
	CleanLedger.Record(TEXT("edit_blueprint"), Restored);
	TestTrue(TEXT("a verified rollback leaves nothing to report"), CleanLedger.IsEmpty());
	return true;
}
