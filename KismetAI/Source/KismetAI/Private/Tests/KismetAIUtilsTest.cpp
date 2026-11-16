// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIUtils.h"
#include "Misc/AutomationTest.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

// ─────────────────────────────────────────────────────────────────────────────
// FindBlueprint tests
// ─────────────────────────────────────────────────────────────────────────────

constexpr auto KismetAITestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFindBlueprint_InvalidPath, "KismetAI.Utils.FindBlueprint.InvalidPath", KismetAITestFlags)

bool FFindBlueprint_InvalidPath::RunTest(const FString& Parameters)
{
	UBlueprint* Result = KismetAIUtils::FindBlueprint(TEXT("/Game/NonExistent/BP_DoesNotExist"));
	TestNull(TEXT("FindBlueprint with invalid path returns nullptr"), Result);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFindBlueprint_EmptyPath, "KismetAI.Utils.FindBlueprint.EmptyPath", KismetAITestFlags)

bool FFindBlueprint_EmptyPath::RunTest(const FString& Parameters)
{
	UBlueprint* Result = KismetAIUtils::FindBlueprint(TEXT(""));
	TestNull(TEXT("FindBlueprint with empty path returns nullptr"), Result);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFindBlueprint_StripsSuffix, "KismetAI.Utils.FindBlueprint.StripsCClassSuffix", KismetAITestFlags)

bool FFindBlueprint_StripsSuffix::RunTest(const FString& Parameters)
{
	// Both paths should resolve identically (to nullptr for a non-existent asset)
	UBlueprint* WithSuffix = KismetAIUtils::FindBlueprint(TEXT("/Game/NonExistent/BP_Test_C"));
	UBlueprint* WithoutSuffix = KismetAIUtils::FindBlueprint(TEXT("/Game/NonExistent/BP_Test"));
	TestNull(TEXT("Both return nullptr for non-existent asset"), WithSuffix);
	TestEqual(TEXT("_C suffix stripping produces same result"), WithSuffix, WithoutSuffix);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FindGraph tests
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFindGraph_NullBlueprint, "KismetAI.Utils.FindGraph.NullBlueprint", KismetAITestFlags)

bool FFindGraph_NullBlueprint::RunTest(const FString& Parameters)
{
	UEdGraph* Result = KismetAIUtils::FindGraph(nullptr, TEXT("EventGraph"));
	TestNull(TEXT("FindGraph with null Blueprint returns nullptr"), Result);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFindGraph_TransientBlueprint, "KismetAI.Utils.FindGraph.TransientBlueprint", KismetAITestFlags)

bool FFindGraph_TransientBlueprint::RunTest(const FString& Parameters)
{
	// Create a transient Blueprint for testing
	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		TEXT("BP_TestFindGraph"),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()
	);
	TestNotNull(TEXT("Transient Blueprint created"), BP);
	if (!BP)
	{
		return false;
	}

	// Should have at least one ubergraph page (EventGraph)
	TestTrue(TEXT("Blueprint has UbergraphPages"), BP->UbergraphPages.Num() > 0);

	// FindGraph with the default graph name should find it
	UEdGraph* EventGraph = KismetAIUtils::FindGraph(BP, TEXT("EventGraph"));
	TestNotNull(TEXT("FindGraph finds EventGraph by name"), EventGraph);

	// FindGraph with empty name should return first ubergraph
	UEdGraph* DefaultGraph = KismetAIUtils::FindGraph(BP, TEXT(""));
	TestNotNull(TEXT("FindGraph with empty name returns first ubergraph"), DefaultGraph);
	TestEqual(TEXT("Default graph is the EventGraph"), DefaultGraph, EventGraph);

	// FindGraph with nonexistent name should return nullptr
	UEdGraph* Missing = KismetAIUtils::FindGraph(BP, TEXT("NoSuchGraph"));
	TestNull(TEXT("FindGraph with nonexistent name returns nullptr"), Missing);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFindGraph_FunctionGraph, "KismetAI.Utils.FindGraph.FunctionGraph", KismetAITestFlags)

bool FFindGraph_FunctionGraph::RunTest(const FString& Parameters)
{
	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		TEXT("BP_TestFindFuncGraph"),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()
	);
	TestNotNull(TEXT("Transient Blueprint created"), BP);
	if (!BP)
	{
		return false;
	}

	// Add a function graph
	UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
		BP,
		TEXT("TestFunction"),
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass()
	);
	FBlueprintEditorUtils::AddFunctionGraph(BP, FuncGraph, /*bIsUserCreated=*/true, static_cast<UFunction*>(nullptr));

	// FindGraph should locate the function graph by name
	UEdGraph* Found = KismetAIUtils::FindGraph(BP, TEXT("TestFunction"));
	TestNotNull(TEXT("FindGraph finds function graph"), Found);
	TestEqual(TEXT("Found graph matches created graph"), Found, FuncGraph);

	return true;
}
