// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAITruncation.h"
#include "Misc/AutomationTest.h"

constexpr auto TruncTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// --- SplitT3DIntoNodeBlocks ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTrunc_SplitT3D_Basic, "KismetAI.Truncation.SplitT3D.Basic", TruncTestFlags)
bool FTrunc_SplitT3D_Basic::RunTest(const FString& Parameters)
{
	FString Input = TEXT(
		"Begin Object Class=A Name=\"Node_0\"\n"
		"   Foo=Bar\n"
		"End Object\n"
		"Begin Object Class=B Name=\"Node_1\"\n"
		"   Baz=Qux\n"
		"End Object\n"
	);
	TArray<FString> Blocks = FKismetAITruncation::SplitT3DIntoNodeBlocks(Input);
	TestEqual(TEXT("Two node blocks"), Blocks.Num(), 2);
	TestTrue(TEXT("First block has Node_0"), Blocks[0].Contains(TEXT("Node_0")));
	TestTrue(TEXT("Second block has Node_1"), Blocks[1].Contains(TEXT("Node_1")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTrunc_SplitT3D_Empty, "KismetAI.Truncation.SplitT3D.Empty", TruncTestFlags)
bool FTrunc_SplitT3D_Empty::RunTest(const FString& Parameters)
{
	TArray<FString> Blocks = FKismetAITruncation::SplitT3DIntoNodeBlocks(TEXT(""));
	TestEqual(TEXT("Empty input gives empty output"), Blocks.Num(), 0);
	return true;
}

// --- TruncateT3DByNodeBoundary ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTrunc_T3D_NoTruncation, "KismetAI.Truncation.T3DByBoundary.NoTruncation", TruncTestFlags)
bool FTrunc_T3D_NoTruncation::RunTest(const FString& Parameters)
{
	FString Input = TEXT("Begin Object Class=A Name=\"N0\"\nEnd Object\n");
	FTruncationResult R = FKismetAITruncation::TruncateT3DByNodeBoundary(Input, 10000);
	TestEqual(TEXT("No truncation when under limit"), R.OmittedItems, 0);
	TestEqual(TEXT("Included items equals total"), R.IncludedItems, R.TotalItems);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTrunc_T3D_TruncatesCorrectly, "KismetAI.Truncation.T3DByBoundary.TruncatesCorrectly", TruncTestFlags)
bool FTrunc_T3D_TruncatesCorrectly::RunTest(const FString& Parameters)
{
	// Create 5 nodes, each ~100 chars
	FString Input;
	for (int32 i = 0; i < 5; i++)
	{
		Input += FString::Printf(
			TEXT("Begin Object Class=/Script/BlueprintGraph.K2Node_CallFunction Name=\"Node_%d\"\n")
			TEXT("   FunctionReference=(MemberName=\"Func%d\")\n")
			TEXT("End Object\n"), i, i);
	}

	// Limit to ~250 chars -- should fit 1-2 nodes
	FTruncationResult R = FKismetAITruncation::TruncateT3DByNodeBoundary(Input, 250);
	TestTrue(TEXT("Some nodes omitted"), R.OmittedItems > 0);
	TestTrue(TEXT("At least one node included"), R.IncludedItems > 0);
	TestTrue(TEXT("Truncation marker present"), R.TruncatedText.Contains(TEXT("[truncated:")));
	TestTrue(TEXT("Marker mentions node count"), R.TruncatedText.Contains(TEXT("more nodes")));
	// Verify included blocks are complete
	TestTrue(TEXT("Complete Begin Object"), R.TruncatedText.Contains(TEXT("Begin Object")));
	TestTrue(TEXT("Complete End Object"), R.TruncatedText.Contains(TEXT("End Object")));
	return true;
}

// --- TruncateDescribeByChain ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTrunc_Describe_TruncatesChains, "KismetAI.Truncation.DescribeByChain.TruncatesChains", TruncTestFlags)
bool FTrunc_Describe_TruncatesChains::RunTest(const FString& Parameters)
{
	FString Input = TEXT(
		"## BP_Test / EventGraph\n\n"
		"### Execution Flow\n\n"
		"Event BeginPlay\n  PrintString(InString=\"Hello\")\n\n"
		"Event Tick\n  SetActorLocation\n\n"
		"Custom Event\n  Delay\n  PrintString\n\n"
		"### Nodes (10 total)\n\n"
		"Node_0: Event BeginPlay\nNode_1: PrintString\n");

	// Limit small enough that not all chains fit
	FTruncationResult R = FKismetAITruncation::TruncateDescribeByChain(Input, 200);
	TestTrue(TEXT("Some chains omitted"), R.OmittedItems > 0);
	TestTrue(TEXT("Nodes section preserved"), R.TruncatedText.Contains(TEXT("### Nodes")));
	TestTrue(TEXT("Truncation marker present"),
		R.TruncatedText.Contains(TEXT("[truncated:")) || R.TruncatedText.Contains(TEXT("more execution chains")));
	return true;
}

// --- PaginateT3DNodes ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTrunc_Paginate_Basic, "KismetAI.Truncation.Paginate.Basic", TruncTestFlags)
bool FTrunc_Paginate_Basic::RunTest(const FString& Parameters)
{
	FString Input;
	for (int32 i = 0; i < 10; i++)
	{
		Input += FString::Printf(TEXT("Begin Object Name=\"Node_%d\"\nEnd Object\n"), i);
	}

	FTruncationResult R = FKismetAITruncation::PaginateT3DNodes(Input, 0, 3);
	TestEqual(TEXT("Total is 10"), R.TotalItems, 10);
	TestEqual(TEXT("Included is 3"), R.IncludedItems, 3);
	TestTrue(TEXT("Contains Node_0"), R.TruncatedText.Contains(TEXT("Node_0")));
	TestTrue(TEXT("Contains Node_2"), R.TruncatedText.Contains(TEXT("Node_2")));
	TestFalse(TEXT("Does not contain Node_3"), R.TruncatedText.Contains(TEXT("Node_3")));

	// Page 2
	FTruncationResult R2 = FKismetAITruncation::PaginateT3DNodes(Input, 3, 3);
	TestTrue(TEXT("Page 2 contains Node_3"), R2.TruncatedText.Contains(TEXT("Node_3")));
	TestTrue(TEXT("Page 2 contains Node_5"), R2.TruncatedText.Contains(TEXT("Node_5")));
	TestFalse(TEXT("Page 2 does not contain Node_0"), R2.TruncatedText.Contains(TEXT("Node_0")));
	return true;
}
