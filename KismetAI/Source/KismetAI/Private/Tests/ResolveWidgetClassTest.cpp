// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Misc/AutomationTest.h"
#include "KismetAIUtils.h"
#include "Components/TextBlock.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/Border.h"

constexpr auto WidgetResolveTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// Exact short class names resolve to the concrete UWidget subclass.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveWidget_Exact, "KismetAI.Widgets.ResolveWidgetClass_Exact", WidgetResolveTestFlags)
bool FResolveWidget_Exact::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("TextBlock"), KismetAIUtils::ResolveWidgetClass(TEXT("TextBlock"))
		== UTextBlock::StaticClass());
	TestTrue(TEXT("CanvasPanel"), KismetAIUtils::ResolveWidgetClass(TEXT("CanvasPanel"))
		== UCanvasPanel::StaticClass());
	return true;
}

// Matching is case-insensitive.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveWidget_Case, "KismetAI.Widgets.ResolveWidgetClass_Case", WidgetResolveTestFlags)
bool FResolveWidget_Case::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("lowercase button"), KismetAIUtils::ResolveWidgetClass(TEXT("button"))
		== UButton::StaticClass());
	return true;
}

// An exact name wins over substring matches (e.g. "Border" should not be shadowed by
// classes whose names merely contain "Border").
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveWidget_ExactBeatsSubstring, "KismetAI.Widgets.ResolveWidgetClass_ExactBeatsSubstring", WidgetResolveTestFlags)
bool FResolveWidget_ExactBeatsSubstring::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Border resolves to UBorder"), KismetAIUtils::ResolveWidgetClass(TEXT("Border"))
		== UBorder::StaticClass());
	return true;
}

// Garbage resolves to null rather than guessing, with no spurious candidates.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveWidget_Unknown, "KismetAI.Widgets.ResolveWidgetClass_Unknown", WidgetResolveTestFlags)
bool FResolveWidget_Unknown::RunTest(const FString& Parameters)
{
	TArray<FString> Candidates;
	TestNull(TEXT("Nonsense -> null"), KismetAIUtils::ResolveWidgetClass(TEXT("NotARealWidgetXyzzy"), &Candidates));
	TestEqual(TEXT("no candidates for nonsense"), Candidates.Num(), 0);
	TestNull(TEXT("Empty -> null"), KismetAIUtils::ResolveWidgetClass(TEXT("")));
	return true;
}
