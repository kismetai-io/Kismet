// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Misc/AutomationTest.h"
#include "KismetAIModule.h"

constexpr auto RefsTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// KismetAI.References.*
// All tests exercise FormatReferencesBlock only — a pure function with no
// asset-registry dependency.
// ─────────────────────────────────────────────────────────────────────────────

// Empty array → empty string (no block at all)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReferences_EmptyArray, "KismetAI.References.EmptyArray", RefsTestFlags)
bool FReferences_EmptyArray::RunTest(const FString& Parameters)
{
	TArray<FAssetRef> Refs;
	FString Result = FKismetAIModule::FormatReferencesBlock(Refs);
	TestTrue(TEXT("Empty refs → empty string"), Result.IsEmpty());
	return true;
}

// Single focused Blueprint ref → block with (focused) annotation + wrapper
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReferences_SingleFocusedBlueprint, "KismetAI.References.SingleFocusedBlueprint", RefsTestFlags)
bool FReferences_SingleFocusedBlueprint::RunTest(const FString& Parameters)
{
	TArray<FAssetRef> Refs;
	FAssetRef Ref;
	Ref.Path     = TEXT("/Game/Blueprints/BP_Player.BP_Player");
	Ref.Name     = TEXT("BP_Player");
	Ref.Class    = TEXT("Blueprint");
	Ref.bFocused = true;
	Refs.Add(Ref);

	FString Result = FKismetAIModule::FormatReferencesBlock(Refs);

	TestTrue(TEXT("Has opening tag"),  Result.Contains(TEXT("<references>")));
	TestTrue(TEXT("Has closing tag"),  Result.Contains(TEXT("</references>")));
	TestTrue(TEXT("Has class lower"), Result.Contains(TEXT("blueprint")));
	TestTrue(TEXT("Has path"),        Result.Contains(TEXT("/Game/Blueprints/BP_Player.BP_Player")));
	TestTrue(TEXT("Has (focused)"),   Result.Contains(TEXT("(focused)")));
	return true;
}

// Multiple refs are preserved; dedup removes exact duplicate paths (case-insensitive)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReferences_MultipleAndDedup, "KismetAI.References.MultipleAndDedup", RefsTestFlags)
bool FReferences_MultipleAndDedup::RunTest(const FString& Parameters)
{
	TArray<FAssetRef> Refs;

	FAssetRef A;
	A.Path  = TEXT("/Game/Blueprints/BP_Enemy.BP_Enemy");
	A.Name  = TEXT("BP_Enemy");
	A.Class = TEXT("Blueprint");
	Refs.Add(A);

	FAssetRef B;
	B.Path  = TEXT("/Game/Data/DT_Items.DT_Items");
	B.Name  = TEXT("DT_Items");
	B.Class = TEXT("DataTable");
	Refs.Add(B);

	// Duplicate of A — different case in path, should be deduped
	FAssetRef Dup;
	Dup.Path  = TEXT("/game/blueprints/BP_Enemy.BP_Enemy");  // lower-case path
	Dup.Name  = TEXT("BP_Enemy");
	Dup.Class = TEXT("Blueprint");
	Refs.Add(Dup);

	FString Result = FKismetAIModule::FormatReferencesBlock(Refs);

	// Should contain exactly ONE occurrence of the enemy path (deduped)
	int32 Count = 0;
	int32 SearchFrom = 0;
	while (true)
	{
		int32 Found = Result.Find(TEXT("BP_Enemy"), ESearchCase::IgnoreCase, ESearchDir::FromStart, SearchFrom);
		if (Found == INDEX_NONE) break;
		Count++;
		SearchFrom = Found + 1;
	}
	// Two occurrences: one from original path segment, dedup removes the second entry
	// The path appears once as a line item; within that line the name also appears — but
	// we only care that there is exactly 1 *line* for BP_Enemy. Count via line parsing.
	int32 LineCount = 0;
	TArray<FString> Lines;
	Result.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		if (Line.Contains(TEXT("BP_Enemy")))
		{
			LineCount++;
		}
	}
	TestEqual(TEXT("Exactly one line for BP_Enemy after dedup"), LineCount, 1);

	// Both non-duplicate classes appear
	TestTrue(TEXT("Has blueprint line"), Result.Contains(TEXT("blueprint")));
	TestTrue(TEXT("Has datatable line"), Result.Contains(TEXT("datatable")));
	return true;
}

// A ref with bMissing → "- (missing) <path>" line
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReferences_MissingRef, "KismetAI.References.MissingRef", RefsTestFlags)
bool FReferences_MissingRef::RunTest(const FString& Parameters)
{
	TArray<FAssetRef> Refs;
	FAssetRef Ref;
	Ref.Path     = TEXT("/Game/Blueprints/BP_Deleted.BP_Deleted");
	Ref.Name     = TEXT("BP_Deleted");
	Ref.Class    = TEXT("Blueprint");
	Ref.bMissing = true;
	Refs.Add(Ref);

	FString Result = FKismetAIModule::FormatReferencesBlock(Refs);

	TestTrue(TEXT("Has (missing) prefix"), Result.Contains(TEXT("(missing)")));
	TestTrue(TEXT("Has path"),             Result.Contains(TEXT("/Game/Blueprints/BP_Deleted")));
	return true;
}

// A ref with a non-empty Summary → "(summary text)" appended to the line
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReferences_SummaryRendered, "KismetAI.References.SummaryRendered", RefsTestFlags)
bool FReferences_SummaryRendered::RunTest(const FString& Parameters)
{
	TArray<FAssetRef> Refs;
	FAssetRef Ref;
	Ref.Path    = TEXT("/Game/Blueprints/BP_Npc.BP_Npc");
	Ref.Name    = TEXT("BP_Npc");
	Ref.Class   = TEXT("Blueprint");
	Ref.Summary = TEXT("parent: ACharacter");
	Refs.Add(Ref);

	FString Result = FKismetAIModule::FormatReferencesBlock(Refs);

	TestTrue(TEXT("Has summary text"), Result.Contains(TEXT("parent: ACharacter")));
	// Summary must be wrapped in parens on the same line
	TestTrue(TEXT("Summary in parens"), Result.Contains(TEXT("(parent: ACharacter)")));
	return true;
}

// Refs with empty Summary do NOT get a trailing "()" on the line
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReferences_NoEmptySummaryParens, "KismetAI.References.NoEmptySummaryParens", RefsTestFlags)
bool FReferences_NoEmptySummaryParens::RunTest(const FString& Parameters)
{
	TArray<FAssetRef> Refs;
	FAssetRef Ref;
	Ref.Path    = TEXT("/Game/Blueprints/BP_Simple.BP_Simple");
	Ref.Name    = TEXT("BP_Simple");
	Ref.Class   = TEXT("Blueprint");
	// Summary intentionally empty
	Refs.Add(Ref);

	FString Result = FKismetAIModule::FormatReferencesBlock(Refs);

	// No "()" should appear (would mean an empty summary was rendered)
	TestFalse(TEXT("No empty parens ()"), Result.Contains(TEXT("()")));
	return true;
}
