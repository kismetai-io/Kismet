#include "Misc/AutomationTest.h"
#include "KismetAIModule.h"
#include "KismetAIMutationOutcome.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialMutationEvidence, "KismetAI.Materials.MutationEvidence", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialMutationEvidence::RunTest(const FString& Parameters)
{
	UPackage* Package = CreatePackage(TEXT("/Temp/KismetMaterialEvidence"));
	UMaterial* Material = NewObject<UMaterial>(Package, TEXT("M_Evidence"), RF_Standalone | RF_Transactional);
	auto Args = [&]()
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("material"), Material->GetPathName());
		return Body;
	};
	auto Check = [&](const FToolExecutionResult& Result, bool bApplied, bool bDirty)
	{
		TestTrue(TEXT("handler transport succeeds"), Result.bSuccess);
		FKismetAIMutationOutcome Outcome;
		if (!TestTrue(TEXT("typed material evidence exists"), FKismetAIMutationOutcome::TryExtractFromToolResult(Result.ToModelString(), Outcome))) return;
		TestEqual(TEXT("asset package identity"), Outcome.PackageName, Package->GetName());
		TestEqual(TEXT("applied evidence"), Outcome.bAppliedInMemory, bApplied);
		TestEqual(TEXT("preexisting dirty state"), Outcome.bPackageWasDirtyBefore, bDirty);
		TestTrue(TEXT("material edits do not claim save"), Outcome.SaveStatus == EKismetAISaveStatus::NotRequested);
		TestEqual(TEXT("material save policy"), Outcome.SavePolicy, FString(KismetAISavePolicy::NoAutosave));
		TestEqual(TEXT("save skip reason"), Outcome.SaveSkipReason, FString(bApplied ? KismetAISaveSkipReason::NotAutosaved : KismetAISaveSkipReason::NothingApplied));
	};
	Package->SetDirtyFlag(false);
	auto Create = Args(); Create->SetStringField(TEXT("node"), TEXT("Constant"));
	const FToolExecutionResult Created = FKismetAIModule::Get().DoCreateMaterialExpression(Create);
	TestTrue(TEXT("create succeeded"), Created.bSuccess);
	Check(Created, true, false);
	Package->SetDirtyFlag(true);
	auto Manage = Args(); Manage->SetBoolField(TEXT("two_sided"), true);
	Check(FKismetAIModule::Get().DoManageMaterial(Manage), true, true);
	auto Rejected = Args(); Rejected->SetStringField(TEXT("domain"), TEXT("NotADomain"));
	const FToolExecutionResult RejectedResult = FKismetAIModule::Get().DoManageMaterial(Rejected);
	Check(RejectedResult, false, true);
	TestFalse(TEXT("rejected settings remain unsuccessful"), RejectedResult.bOperationsSucceeded);
	auto Edit = Args();
	auto Invalid = MakeShared<FJsonObject>(); Invalid->SetStringField(TEXT("action"), TEXT("invalid_action"));
	Edit->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(Invalid) });
	const FToolExecutionResult Failed = FKismetAIModule::Get().DoEditMaterial(Edit);
	Check(Failed, false, true);
	TestFalse(TEXT("failed operations remain unsuccessful"), Failed.bOperationsSucceeded);
	if (TestEqual(TEXT("one authored expression"), Material->GetExpressions().Num(), 1))
	{
		UMaterialExpressionConstant* Constant = Cast<UMaterialExpressionConstant>(Material->GetExpressions()[0]);
		if (TestNotNull(TEXT("constant expression exists"), Constant))
		{
			auto SetValue = MakeShared<FJsonObject>();
			SetValue->SetStringField(TEXT("action"), TEXT("set_property"));
			SetValue->SetStringField(TEXT("node"), Constant->GetName());
			SetValue->SetStringField(TEXT("property"), TEXT("R"));
			SetValue->SetNumberField(TEXT("value"), 0.75);
			Edit->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(SetValue), MakeShared<FJsonValueObject>(Invalid) });
			const FToolExecutionResult Partial = FKismetAIModule::Get().DoEditMaterial(Edit);
			Check(Partial, true, true);
			TestFalse(TEXT("partial batch preserves failed operation"), Partial.bOperationsSucceeded);
			TestEqual(TEXT("successful operation remains applied"), Constant->R, 0.75f);
		}
	}
	const FToolExecutionResult Compiled = FKismetAIModule::Get().DoCompileMaterial(Args());
	FKismetAIMutationOutcome CompileOutcome;
	if (TestTrue(TEXT("compile has typed evidence"), FKismetAIMutationOutcome::TryExtractFromToolResult(Compiled.ToModelString(), CompileOutcome)))
	{
		const bool bVerified = CompileOutcome.CompileStatus == EKismetAICompileStatus::Succeeded;
		TestEqual(TEXT("compile success requires verified compile evidence"), Compiled.bOperationsSucceeded, bVerified);
		TestTrue(TEXT("compile transport succeeds"), Compiled.bSuccess);
		if (TestTrue(TEXT("compile payload exists"), Compiled.Data.IsValid()))
		{
			TestEqual(TEXT("compile payload success agrees"), Compiled.Data->GetBoolField(TEXT("success")), bVerified);
			TestEqual(TEXT("compiled status agrees"), Compiled.Data->GetStringField(TEXT("status")) == TEXT("compiled"), bVerified);
		}
	}
	return true;
}
