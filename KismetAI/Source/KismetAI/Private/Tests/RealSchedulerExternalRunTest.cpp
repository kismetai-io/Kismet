// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "RunEntryHarness.h"
#include "Misc/AutomationTest.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace KismetAIRealSchedulerTest
{
	using namespace KismetAIRunEntryHarness;
	struct FState
	{
		FScopedModuleSeams Seams{FScopedModuleSeams::EPolicy::CountingAllowAll, true};
		TStrongObjectPtr<UBlueprint> Blueprint;
		UK2Node_CallFunction* Print = nullptr;
		FKismetAIExternalRunPtr Record;
		double Deadline = FPlatformTime::Seconds() + 20.0;
	};

	class FVerify final : public IAutomationLatentCommand
	{
	public:
		FVerify(FAutomationTestBase* InTest, TSharedRef<FState> InState) : Test(InTest), State(InState) {}
		virtual bool Update() override
		{
			const bool bSettled = State->Record->IsFinished() && FAccess::Session(State->Seams.Module).NumLiveWorkers() == 0;
			if (!bSettled && FPlatformTime::Seconds() < State->Deadline) return false;
			if (!Test->TestTrue(TEXT("real scheduler mutation run and worker finish"), bSettled))
			{
				FAccess::Session(State->Seams.Module).CancelActive();
				State->Seams.Seams->Dispatcher->Close();
				return true;
			}
			Test->TestEqual(TEXT("the real scheduled mutation applied"), State->Print->FindPin(TEXT("Duration"))->DefaultValue, FString(TEXT("7.5")));
			Test->TestEqual(TEXT("exactly one mutation admission"), State->Seams.Policy.Admissions.load(), 1);
			Test->TestEqual(TEXT("model received tool result and finished"), State->Seams.Seams->Model.Calls.load(), 2);
			const auto Stats = State->Seams.Seams->Dispatcher->GetStatsForTests();
			Test->TestTrue(TEXT("production frame dispatcher actually executed work"), Stats.Started > 0);
			Test->TestEqual(TEXT("all queued dispatcher work completed"), Stats.Completed, Stats.Started);
			Test->TestEqual(TEXT("no dispatcher work remains in flight"), Stats.InFlight, 0);
			return true;
		}
	private:
		FAutomationTestBase* Test;
		TSharedRef<FState> State;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRealSchedulerExternalMutationTest, "KismetAI.ExternalRuns.RealSchedulerMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRealSchedulerExternalMutationTest::RunTest(const FString& Parameters)
{
	using namespace KismetAIRealSchedulerTest;
	auto State = MakeShared<FState>();
	if (FAccess::Session(State->Seams.Module).IsRunning()) { AddError(TEXT("a run is already active")); return false; }
	const FString Name = TEXT("BP_FrameDispatch_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UPackage* Package = CreatePackage(*(TEXT("/Temp/KismetFrameDispatch/") + Name));
	State->Blueprint.Reset(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, FName(*Name), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()));
	if (!TestNotNull(TEXT("disposable Blueprint"), State->Blueprint.Get())) return false;
	UEdGraph* Graph = State->Blueprint->UbergraphPages[0];
	State->Print = NewObject<UK2Node_CallFunction>(Graph);
	State->Print->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString), UKismetSystemLibrary::StaticClass());
	Graph->AddNode(State->Print, false, false);
	State->Print->CreateNewGuid();
	State->Print->AllocateDefaultPins();
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("set_pin_default"));
	Op->SetStringField(TEXT("node"), State->Print->GetName());
	Op->SetStringField(TEXT("pin"), TEXT("Duration"));
	Op->SetStringField(TEXT("value"), TEXT("7.5"));
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), State->Blueprint->GetPathName());
	Args->SetArrayField(TEXT("operations"), {MakeShared<FJsonValueObject>(Op)});
	State->Seams.Seams->Model.Script = {ToolTurn(TEXT("real_frame_tool"), TEXT("edit_blueprint"), Args), TextTurn(TEXT("done"))};
	const FRunReply Reply = Call(&FAccess::Submit, MakeRequest(TEXT("{\"input\":\"frame scheduler mutation regression\"}")));
	if (!TestEqual(TEXT("external mutation admitted"), Reply.Code, 202)) return false;
	State->Record = FAccess::Registry(State->Seams.Module).Find(Reply.Data()->GetStringField(TEXT("run_id")));
	if (!TestTrue(TEXT("external run is tracked"), State->Record.IsValid())) return false;
	ADD_LATENT_AUTOMATION_COMMAND(FVerify(this, State));
	return true;
}
