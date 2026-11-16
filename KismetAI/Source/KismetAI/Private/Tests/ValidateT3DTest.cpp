// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIModule.h"
#include "KismetAIValidation.h"
#include "KismetAIUtils.h"
#include "Misc/AutomationTest.h"

constexpr auto ValidateT3DTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// ValidateT3DForImport — the load-bearing guard that keeps malformed pin records
// (the editor-crash trigger) out of FEdGraphUtilities::ImportNodesFromText.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidateT3D_AcceptsWellFormed, "KismetAI.ValidateT3D.AcceptsWellFormed", ValidateT3DTestFlags)
bool FValidateT3D_AcceptsWellFormed::RunTest(const FString& Parameters)
{
	// Resolve placeholders first (mirrors the real pipeline) so PinIds are real GUIDs.
	FString T3D = FKismetAIModule::Get().ResolvePlaceholders(TEXT(
		"Begin Object Class=/Script/BlueprintGraph.K2Node_CallFunction Name=\"K2Node_CallFunction_NEW\"\n"
		"   FunctionReference=(MemberParent=\"/Script/CoreUObject.Class'/Script/Engine.KismetSystemLibrary'\",MemberName=\"PrintString\")\n"
		"   NodeGuid=GUID_MYNODE\n"
		"CustomProperties Pin (PinId=LINK_EXEC,PinName=\"execute\",PinType.PinCategory=\"exec\")\n"
		"CustomProperties Pin (PinId=LINK_THEN,PinName=\"then\",Direction=\"EGPD_Output\",PinType.PinCategory=\"exec\",LinkedTo=(K2Node_CallFunction_NEW LINK_EXEC,))\n"
		"End Object\n"));

	FString Error;
	const bool bOk = FKismetAIModule::Get().ValidateT3DForImport(T3D, nullptr, Error);
	TestTrue(FString::Printf(TEXT("well-formed T3D should validate (err=%s)"), *Error), bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidateT3D_AcceptsUnquotedPinName, "KismetAI.ValidateT3D.AcceptsUnquotedPinName", ValidateT3DTestFlags)
bool FValidateT3D_AcceptsUnquotedPinName::RunTest(const FString& Parameters)
{
	// Unquoted PinName=execute is legal hand-authored T3D; it must NOT be rejected
	// as "missing PinName".
	FString T3D = FKismetAIModule::Get().ResolvePlaceholders(TEXT(
		"CustomProperties Pin (PinId=LINK_EXEC,PinName=execute,PinType.PinCategory=\"exec\")\n"));
	FString Error;
	const bool bOk = FKismetAIModule::Get().ValidateT3DForImport(T3D, nullptr, Error);
	TestTrue(FString::Printf(TEXT("unquoted PinName should validate (err=%s)"), *Error), bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidateT3D_RejectsMissingPinName, "KismetAI.ValidateT3D.RejectsMissingPinName", ValidateT3DTestFlags)
bool FValidateT3D_RejectsMissingPinName::RunTest(const FString& Parameters)
{
	FString T3D = FKismetAIModule::Get().ResolvePlaceholders(TEXT(
		"CustomProperties Pin (PinId=LINK_EXEC,PinType.PinCategory=\"exec\")\n"));
	FString Error;
	const bool bOk = FKismetAIModule::Get().ValidateT3DForImport(T3D, nullptr, Error);
	TestFalse(TEXT("pin with no PinName must be rejected"), bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidateT3D_RejectsBadPinId, "KismetAI.ValidateT3D.RejectsBadPinId", ValidateT3DTestFlags)
bool FValidateT3D_RejectsBadPinId::RunTest(const FString& Parameters)
{
	// PinId is not a GUID.
	const FString T3D = TEXT(
		"CustomProperties Pin (PinId=notaguid,PinName=\"execute\",PinType.PinCategory=\"exec\")\n");
	FString Error;
	const bool bOk = FKismetAIModule::Get().ValidateT3DForImport(T3D, nullptr, Error);
	TestFalse(TEXT("pin with non-GUID PinId must be rejected"), bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidateT3D_RejectsUnbalancedParens, "KismetAI.ValidateT3D.RejectsUnbalancedParens", ValidateT3DTestFlags)
bool FValidateT3D_RejectsUnbalancedParens::RunTest(const FString& Parameters)
{
	FString T3D = FKismetAIModule::Get().ResolvePlaceholders(TEXT(
		"CustomProperties Pin (PinId=LINK_EXEC,PinName=\"execute\",PinType.PinCategory=\"exec\"\n"));
	FString Error;
	const bool bOk = FKismetAIModule::Get().ValidateT3DForImport(T3D, nullptr, Error);
	TestFalse(TEXT("unbalanced parens must be rejected"), bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidateT3D_RejectsResidualPlaceholder, "KismetAI.ValidateT3D.RejectsResidualPlaceholder", ValidateT3DTestFlags)
bool FValidateT3D_RejectsResidualPlaceholder::RunTest(const FString& Parameters)
{
	// A real GUID pin, but a leftover LINK_ token elsewhere (un-resolved).
	FString T3D = FKismetAIModule::Get().ResolvePlaceholders(TEXT(
		"CustomProperties Pin (PinId=LINK_EXEC,PinName=\"execute\",PinType.PinCategory=\"exec\")\n"));
	T3D += TEXT("   NodeGuid=GUID_LEFTOVER\n");
	FString Error;
	const bool bOk = FKismetAIModule::Get().ValidateT3DForImport(T3D, nullptr, Error);
	TestFalse(TEXT("residual GUID_/LINK_ placeholder must be rejected"), bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidateT3D_RejectsDanglingLinkedTo, "KismetAI.ValidateT3D.RejectsDanglingLinkedTo", ValidateT3DTestFlags)
bool FValidateT3D_RejectsDanglingLinkedTo::RunTest(const FString& Parameters)
{
	// LinkedTo references a PinId that is neither defined here nor in the (null) graph.
	const FString T3D = TEXT(
		"CustomProperties Pin (PinId=11111111111111111111111111111111,PinName=\"then\",Direction=\"EGPD_Output\",PinType.PinCategory=\"exec\",LinkedTo=(SomeNode 22222222222222222222222222222222,))\n");
	FString Error;
	const bool bOk = FKismetAIModule::Get().ValidateT3DForImport(T3D, nullptr, Error);
	TestFalse(TEXT("dangling LinkedTo target must be rejected"), bOk);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ResolvePlaceholders — prefix-colliding tokens (LINK_A vs LINK_AB) must each map
// to their own GUID; the old substring-replace corrupted the longer one.
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// insert_node argument validation
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInsertNode_RequiresBlueprintAndNode, "KismetAI.ValidateInsertNode.RequiresFields", ValidateT3DTestFlags)
bool FInsertNode_RequiresBlueprintAndNode::RunTest(const FString& Parameters)
{
	// Missing both -> invalid.
	TestFalse(TEXT("empty args invalid"), FKismetAIValidation::ValidateInsertNode(MakeShared<FJsonObject>()).bIsValid);

	// Missing 'node' -> invalid.
	TSharedPtr<FJsonObject> NoNode = MakeShared<FJsonObject>();
	NoNode->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_X"));
	TestFalse(TEXT("missing node invalid"), FKismetAIValidation::ValidateInsertNode(NoNode).bIsValid);

	// Both present -> valid.
	TSharedPtr<FJsonObject> Ok = MakeShared<FJsonObject>();
	Ok->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_X"));
	Ok->SetStringField(TEXT("node"), TEXT("Branch"));
	TestTrue(TEXT("blueprint+node valid"), FKismetAIValidation::ValidateInsertNode(Ok).bIsValid);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ParsePinTypeString — container types (array<T> / set<T> / map<K,V> / T[])
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParsePinType_Containers, "KismetAI.ParsePinType.Containers", ValidateT3DTestFlags)
bool FParsePinType_Containers::RunTest(const FString& Parameters)
{
	FString Err;

	const FEdGraphPinType ArrInt = KismetAIUtils::ParsePinTypeString(TEXT("array<int>"), &Err);
	TestEqual(TEXT("array<int> is Array"), (int)ArrInt.ContainerType, (int)EPinContainerType::Array);
	TestEqual(TEXT("array<int> element is int"), ArrInt.PinCategory, UEdGraphSchema_K2::PC_Int);

	const FEdGraphPinType ArrShort = KismetAIUtils::ParsePinTypeString(TEXT("int[]"), &Err);
	TestEqual(TEXT("int[] is Array"), (int)ArrShort.ContainerType, (int)EPinContainerType::Array);

	const FEdGraphPinType SetStr = KismetAIUtils::ParsePinTypeString(TEXT("set<string>"), &Err);
	TestEqual(TEXT("set<string> is Set"), (int)SetStr.ContainerType, (int)EPinContainerType::Set);
	TestEqual(TEXT("set<string> element is string"), SetStr.PinCategory, UEdGraphSchema_K2::PC_String);

	const FEdGraphPinType MapType = KismetAIUtils::ParsePinTypeString(TEXT("map<name,int>"), &Err);
	TestEqual(TEXT("map<name,int> is Map"), (int)MapType.ContainerType, (int)EPinContainerType::Map);
	TestEqual(TEXT("map key is name"), MapType.PinCategory, UEdGraphSchema_K2::PC_Name);
	TestEqual(TEXT("map value is int"), MapType.PinValueType.TerminalCategory, UEdGraphSchema_K2::PC_Int);

	// Scalar still parses as non-container.
	const FEdGraphPinType Scalar = KismetAIUtils::ParsePinTypeString(TEXT("int"), &Err);
	TestEqual(TEXT("int is not a container"), (int)Scalar.ContainerType, (int)EPinContainerType::None);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolvePlaceholders_PrefixCollision, "KismetAI.ResolvePlaceholders.PrefixCollision", ValidateT3DTestFlags)
bool FResolvePlaceholders_PrefixCollision::RunTest(const FString& Parameters)
{
	const FString Input = TEXT("A=LINK_A B=LINK_AB C=LINK_A");
	const FString Out = FKismetAIModule::Get().ResolvePlaceholders(Input);

	// No placeholder tokens should survive.
	TestFalse(TEXT("no LINK_ token remains"), Out.Contains(TEXT("LINK_")));

	// LINK_A resolved to a single GUID used in both A= and C=; LINK_AB resolved to a
	// different GUID. Validate by parsing the three values back out.
	TArray<FString> Parts;
	Out.ParseIntoArray(Parts, TEXT(" "), true);
	if (!TestEqual(TEXT("three tokens"), Parts.Num(), 3))
	{
		return false;
	}
	const FString A = Parts[0].RightChop(2);   // strip "A="
	const FString B = Parts[1].RightChop(2);   // strip "B="
	const FString C = Parts[2].RightChop(2);   // strip "C="

	FGuid G;
	TestTrue(TEXT("A is a valid GUID"), FGuid::Parse(A, G));
	TestTrue(TEXT("B is a valid GUID"), FGuid::Parse(B, G));
	TestEqual(TEXT("LINK_A maps consistently (A==C)"), A, C);
	TestNotEqual(TEXT("LINK_A and LINK_AB map to distinct GUIDs"), A, B);
	return true;
}
