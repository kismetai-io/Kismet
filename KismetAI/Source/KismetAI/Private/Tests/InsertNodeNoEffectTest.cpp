// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIModule.h"
#include "KismetAIMutationOutcome.h"
#include "KismetAIToolResult.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInsertNodeNoEffectTest,
	"KismetAI.InsertNode.NodeNotFoundHasNoEffectEvidence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInsertNodeNoEffectTest::RunTest(const FString& Parameters)
{
	UPackage* Package = CreatePackage(TEXT("/Temp/KismetInsertNodeNoEffect"));
	const FName Name = MakeUniqueObjectName(Package, UBlueprint::StaticClass(), TEXT("BP_NoEffect"));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Package, Name, BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("fixture blueprint"), Blueprint)) return false;
	UEdGraph* Graph = Blueprint->UbergraphPages.IsEmpty() ? nullptr : Blueprint->UbergraphPages[0];
	if (!Graph)
	{
		Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("EventGraph"),
			UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
	}
	const auto NodesBefore = Graph->Nodes;
	const auto StatusBefore = Blueprint->Status;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
	Args->SetStringField(TEXT("graph"), Graph->GetName());
	Args->SetStringField(TEXT("node"), TEXT("Branch"));
	Args->SetStringField(TEXT("node_class"), TEXT("K2Node_NoEffectTestMissingClass"));

	for (const bool bDirtyBefore : {false, true})
	{
		Package->SetDirtyFlag(bDirtyBefore);
		const FToolExecutionResult Result = FKismetAIModule::Get().DoInsertNode(Args);
		TestFalse(TEXT("lookup failure stays a tool failure"), Result.bSuccess);
		TestEqual(TEXT("lookup error status"), Result.StatusCode, 404);
		TestEqual(TEXT("lookup error code"), Result.ErrorCode, FString(TEXT("NODE_NOT_FOUND")));
		TestFalse(TEXT("handler really ran"), Result.bHandlerNotStarted);
		TestTrue(TEXT("graph nodes unchanged"), Graph->Nodes == NodesBefore);
		TestTrue(TEXT("blueprint status unchanged"), Blueprint->Status == StatusBefore);
		TestEqual(TEXT("package dirtiness unchanged"), Package->IsDirty(), bDirtyBefore);

		// The agent journals typed Data even when the HTTP/model error envelope omits it.
		const TSharedPtr<FJsonObject>* OutcomeJson = nullptr;
		const bool bHasOutcome = Result.Data.IsValid()
			&& Result.Data->TryGetObjectField(TEXT("outcome"), OutcomeJson);
		if (!TestTrue(TEXT("failed lookup has typed no-effect evidence"), bHasOutcome)) continue;
		FKismetAIMutationOutcome Outcome;
		if (!TestTrue(TEXT("outcome parses"), FKismetAIMutationOutcome::FromJson(*OutcomeJson, Outcome))) continue;
		TestFalse(TEXT("nothing applied"), Outcome.bAppliedInMemory);
		TestTrue(TEXT("compile not run"), Outcome.CompileStatus == EKismetAICompileStatus::NotRun);
		TestTrue(TEXT("save not requested"), Outcome.SaveStatus == EKismetAISaveStatus::NotRequested);
		TestEqual(TEXT("skip reason"), Outcome.SaveSkipReason, FString(KismetAISaveSkipReason::NothingApplied));
		TestEqual(TEXT("actual target package"), Outcome.PackageName, Package->GetName());
		TestEqual(TEXT("original dirtiness recorded"), Outcome.bPackageWasDirtyBefore, bDirtyBefore);
	}
	Package->SetDirtyFlag(false);
	return true;
}
