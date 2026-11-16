// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIModule.h"
#include "Misc/AutomationTest.h"

constexpr auto StripT3DTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// StripT3D tests — verify T3D stripping preserves structure while removing noise
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStripT3D_PreservesBeginEnd, "KismetAI.StripT3D.PreservesBeginEnd", StripT3DTestFlags)

bool FStripT3D_PreservesBeginEnd::RunTest(const FString& Parameters)
{
	FString Input = TEXT(
		"Begin Object Class=/Script/BlueprintGraph.K2Node_CallFunction Name=\"K2Node_CallFunction_0\"\n"
		"   FunctionReference=(MemberParent=\"/Script/CoreUObject.Class'/Script/Engine.KismetSystemLibrary'\",MemberName=\"PrintString\")\n"
		"End Object\n"
	);

	FString Result = FKismetAIModule::Get().StripT3D(Input);

	TestTrue(TEXT("Output contains Begin Object"), Result.Contains(TEXT("Begin Object")));
	TestTrue(TEXT("Output contains End Object"), Result.Contains(TEXT("End Object")));
	TestTrue(TEXT("Output contains Class="), Result.Contains(TEXT("Class=")));
	TestTrue(TEXT("Output contains Name="), Result.Contains(TEXT("Name=")));
	TestTrue(TEXT("Output contains FunctionReference"), Result.Contains(TEXT("FunctionReference")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStripT3D_StripsExportPath, "KismetAI.StripT3D.StripsExportPath", StripT3DTestFlags)

bool FStripT3D_StripsExportPath::RunTest(const FString& Parameters)
{
	FString Input = TEXT(
		"Begin Object Class=/Script/BlueprintGraph.K2Node_Event Name=\"K2Node_Event_0\" ExportPath=/Script/BlueprintGraph.K2Node_Event'\"/Game/Test.EventGraph.K2Node_Event_0\"'\n"
		"End Object\n"
	);

	FString Result = FKismetAIModule::Get().StripT3D(Input);

	TestFalse(TEXT("Output does not contain ExportPath"), Result.Contains(TEXT("ExportPath=")));
	TestTrue(TEXT("Output still contains Begin Object"), Result.Contains(TEXT("Begin Object")));
	TestTrue(TEXT("Output still contains Name="), Result.Contains(TEXT("Name=")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStripT3D_StripsHiddenPins, "KismetAI.StripT3D.StripsHiddenPins", StripT3DTestFlags)

bool FStripT3D_StripsHiddenPins::RunTest(const FString& Parameters)
{
	FString Input = TEXT(
		"Begin Object Class=/Script/BlueprintGraph.K2Node_CallFunction Name=\"K2Node_CallFunction_0\"\n"
		"   CustomProperties Pin (PinId=ABC,PinName=\"self\",bHidden=True)\n"
		"   CustomProperties Pin (PinId=DEF,PinName=\"execute\",Direction=EGPD_Input,PinType.PinCategory=\"exec\")\n"
		"   CustomProperties Pin (PinId=GHI,PinName=\"WorldContextObject\",bHidden=True)\n"
		"   CustomProperties Pin (PinId=JKL,PinName=\"InString\",Direction=EGPD_Input,PinType.PinCategory=\"string\",DefaultValue=\"Hello\")\n"
		"End Object\n"
	);

	FString Result = FKismetAIModule::Get().StripT3D(Input);

	TestFalse(TEXT("Hidden 'self' pin stripped"), Result.Contains(TEXT("PinName=\"self\"")));
	TestFalse(TEXT("Hidden 'WorldContextObject' pin stripped"), Result.Contains(TEXT("PinName=\"WorldContextObject\"")));
	TestTrue(TEXT("Visible 'execute' pin preserved"), Result.Contains(TEXT("PinName=\"execute\"")));
	TestTrue(TEXT("Visible 'InString' pin preserved"), Result.Contains(TEXT("PinName=\"InString\"")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStripT3D_StripsDefaultFalseFlags, "KismetAI.StripT3D.StripsDefaultFalseFlags", StripT3DTestFlags)

bool FStripT3D_StripsDefaultFalseFlags::RunTest(const FString& Parameters)
{
	FString Input = TEXT(
		"Begin Object Class=/Script/BlueprintGraph.K2Node_CallFunction Name=\"K2Node_CallFunction_0\"\n"
		"   CustomProperties Pin (PinId=DEF,PinName=\"execute\",Direction=EGPD_Input,PinType.PinCategory=\"exec\","
		"bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,"
		"bAdvancedView=False,bOrphanedPin=False,bOverridePinName=False,"
		"PinType.bIsReference=False,PinType.bIsConst=False,"
		"PinType.ContainerType=None,PinType.PinSubCategory=\"\","
		"PersistentGuid=00000000000000000000000000000000,DefaultObject=None)\n"
		"End Object\n"
	);

	FString Result = FKismetAIModule::Get().StripT3D(Input);

	TestFalse(TEXT("bNotConnectable=False stripped"), Result.Contains(TEXT("bNotConnectable=False")));
	TestFalse(TEXT("PinType.bIsReference=False stripped"), Result.Contains(TEXT("PinType.bIsReference=False")));
	TestFalse(TEXT("PersistentGuid=000... stripped"), Result.Contains(TEXT("PersistentGuid=00000000")));
	TestFalse(TEXT("DefaultObject=None stripped"), Result.Contains(TEXT("DefaultObject=None")));
	TestFalse(TEXT("PinType.ContainerType=None stripped"), Result.Contains(TEXT("PinType.ContainerType=None")));
	// But the pin itself is still there
	TestTrue(TEXT("Pin line still exists"), Result.Contains(TEXT("PinName=\"execute\"")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStripT3D_PreservesUserComments, "KismetAI.StripT3D.PreservesUserComments", StripT3DTestFlags)

bool FStripT3D_PreservesUserComments::RunTest(const FString& Parameters)
{
	FString Input = TEXT(
		"Begin Object Class=/Script/BlueprintGraph.K2Node_CallFunction Name=\"K2Node_CallFunction_0\"\n"
		"   NodeComment=\"Important user note\"\n"
		"End Object\n"
	);

	FString Result = FKismetAIModule::Get().StripT3D(Input);

	TestTrue(TEXT("User comment preserved"), Result.Contains(TEXT("Important user note")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStripT3D_StripsEmptyInput, "KismetAI.StripT3D.EmptyInput", StripT3DTestFlags)

bool FStripT3D_StripsEmptyInput::RunTest(const FString& Parameters)
{
	FString Result = FKismetAIModule::Get().StripT3D(TEXT(""));
	TestTrue(TEXT("Empty input returns empty output"), Result.IsEmpty() || Result.TrimStartAndEnd().IsEmpty());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStripT3D_OutputShorterThanInput, "KismetAI.StripT3D.OutputShorterThanInput", StripT3DTestFlags)

bool FStripT3D_OutputShorterThanInput::RunTest(const FString& Parameters)
{
	// Realistic T3D for a PrintString node with full UE boilerplate
	FString Input = TEXT(
		"Begin Object Class=/Script/BlueprintGraph.K2Node_CallFunction Name=\"K2Node_CallFunction_0\" ExportPath=/Script/BlueprintGraph.K2Node_CallFunction'\"/Game/Test.EventGraph.K2Node_CallFunction_0\"'\n"
		"   FunctionReference=(MemberParent=\"/Script/CoreUObject.Class'/Script/Engine.KismetSystemLibrary'\",MemberName=\"PrintString\")\n"
		"   CustomProperties Pin (PinId=ABC123,PinName=\"self\",PinFriendlyName=NSLOCTEXT(\"K2Node\",\"Target\",\"Target\"),PinToolTip=\"Target\",Direction=EGPD_Input,PinType.PinCategory=\"object\",bHidden=True)\n"
		"   CustomProperties Pin (PinId=DEF456,PinName=\"execute\",Direction=EGPD_Input,PinType.PinCategory=\"exec\",bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,PinType.ContainerType=None,PinType.PinSubCategory=\"\",PinType.PinValueType=(),PinType.PinSubCategoryObject=None,PinType.PinSubCategoryMemberReference=(),PersistentGuid=00000000000000000000000000000000,DefaultObject=None)\n"
		"   CustomProperties Pin (PinId=GHI789,PinName=\"then\",Direction=EGPD_Output,PinType.PinCategory=\"exec\",bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,PinType.ContainerType=None,PinType.PinSubCategory=\"\",PinType.PinValueType=(),PinType.PinSubCategoryObject=None,PinType.PinSubCategoryMemberReference=(),PersistentGuid=00000000000000000000000000000000,DefaultObject=None)\n"
		"   CustomProperties Pin (PinId=JKL012,PinName=\"WorldContextObject\",PinToolTip=\"World Context Object\",Direction=EGPD_Input,PinType.PinCategory=\"object\",bHidden=True)\n"
		"   CustomProperties Pin (PinId=MNO345,PinName=\"InString\",PinFriendlyName=NSLOCTEXT(\"K2Node\",\"PrintString\",\"In String\"),Direction=EGPD_Input,PinType.PinCategory=\"string\",DefaultValue=\"Hello\",bNotConnectable=False,bDefaultValueIsReadOnly=False,PinType.bIsReference=False,PinType.bIsConst=False,PersistentGuid=00000000000000000000000000000000)\n"
		"   bCommentBubblePinned=False\n"
		"   bCommentBubbleVisible=False\n"
		"End Object\n"
	);

	FString Result = FKismetAIModule::Get().StripT3D(Input);

	TestTrue(TEXT("Output is shorter than input"), Result.Len() < Input.Len());

	// Verify key content preserved
	TestTrue(TEXT("FunctionReference preserved"), Result.Contains(TEXT("FunctionReference")));
	TestTrue(TEXT("PrintString preserved"), Result.Contains(TEXT("PrintString")));
	TestTrue(TEXT("InString pin preserved"), Result.Contains(TEXT("InString")));
	TestTrue(TEXT("DefaultValue Hello preserved"), Result.Contains(TEXT("Hello")));

	// Verify noise removed
	TestFalse(TEXT("ExportPath removed"), Result.Contains(TEXT("ExportPath=")));
	TestFalse(TEXT("Hidden self pin removed"), Result.Contains(TEXT("PinName=\"self\"")));
	TestFalse(TEXT("Hidden WorldContextObject removed"), Result.Contains(TEXT("WorldContextObject")));
	TestFalse(TEXT("bCommentBubblePinned removed"), Result.Contains(TEXT("bCommentBubblePinned")));

	return true;
}
