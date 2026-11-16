// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIValidation.h"
#include "Misc/AutomationTest.h"

constexpr auto ValidationTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// --- ValidateReadBlueprint ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidation_ReadBP_MissingBlueprint, "KismetAI.Validation.ReadBlueprint.MissingBlueprint", ValidationTestFlags)
bool FValidation_ReadBP_MissingBlueprint::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	FValidationResult R = FKismetAIValidation::ValidateReadBlueprint(Args);
	TestFalse(TEXT("Missing blueprint is invalid"), R.bIsValid);
	TestTrue(TEXT("Error mentions blueprint"), R.Errors[0].Field == TEXT("blueprint"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidation_ReadBP_InvalidMode, "KismetAI.Validation.ReadBlueprint.InvalidMode", ValidationTestFlags)
bool FValidation_ReadBP_InvalidMode::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_Test"));
	Args->SetStringField(TEXT("mode"), TEXT("invalid_mode"));
	FValidationResult R = FKismetAIValidation::ValidateReadBlueprint(Args);
	TestFalse(TEXT("Invalid mode is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidation_ReadBP_Valid, "KismetAI.Validation.ReadBlueprint.Valid", ValidationTestFlags)
bool FValidation_ReadBP_Valid::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_Test"));
	Args->SetStringField(TEXT("mode"), TEXT("describe"));
	FValidationResult R = FKismetAIValidation::ValidateReadBlueprint(Args);
	TestTrue(TEXT("Valid input passes"), R.bIsValid);
	return true;
}

// 0 means "use default" — OpenAI/Codex models fill optional ints with 0.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidation_ReadBP_LimitZeroAccepted, "KismetAI.Validation.ReadBlueprint.LimitZeroAccepted", ValidationTestFlags)
bool FValidation_ReadBP_LimitZeroAccepted::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_Test"));
	Args->SetNumberField(TEXT("limit"), 0);
	FValidationResult R = FKismetAIValidation::ValidateReadBlueprint(Args);
	TestTrue(TEXT("limit:0 is accepted (treated as default)"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidation_ReadBP_OffsetZeroAccepted, "KismetAI.Validation.ReadBlueprint.OffsetZeroAccepted", ValidationTestFlags)
bool FValidation_ReadBP_OffsetZeroAccepted::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_Test"));
	Args->SetNumberField(TEXT("offset"), 0);
	Args->SetNumberField(TEXT("limit"), 0);
	FValidationResult R = FKismetAIValidation::ValidateReadBlueprint(Args);
	TestTrue(TEXT("offset:0 and limit:0 are accepted"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidation_ReadBP_NegativeLimitRejected, "KismetAI.Validation.ReadBlueprint.NegativeLimitRejected", ValidationTestFlags)
bool FValidation_ReadBP_NegativeLimitRejected::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_Test"));
	Args->SetNumberField(TEXT("limit"), -1);
	FValidationResult R = FKismetAIValidation::ValidateReadBlueprint(Args);
	TestFalse(TEXT("negative limit is still rejected"), R.bIsValid);
	return true;
}

// --- ValidateWriteBlueprint ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidation_WriteBP_MissingT3D, "KismetAI.Validation.WriteBlueprint.MissingT3D", ValidationTestFlags)
bool FValidation_WriteBP_MissingT3D::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_Test"));
	FValidationResult R = FKismetAIValidation::ValidateWriteBlueprint(Args);
	TestFalse(TEXT("Missing t3d is invalid"), R.bIsValid);
	return true;
}

// --- ValidateEditBlueprint ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidation_EditBP_EmptyOperations, "KismetAI.Validation.EditBlueprint.EmptyOperations", ValidationTestFlags)
bool FValidation_EditBP_EmptyOperations::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_Test"));
	TArray<TSharedPtr<FJsonValue>> EmptyOps;
	Args->SetArrayField(TEXT("operations"), EmptyOps);
	FValidationResult R = FKismetAIValidation::ValidateEditBlueprint(Args);
	TestFalse(TEXT("Empty operations is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidation_EditBP_MissingActionInOp, "KismetAI.Validation.EditBlueprint.MissingActionInOp", ValidationTestFlags)
bool FValidation_EditBP_MissingActionInOp::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_Test"));
	TArray<TSharedPtr<FJsonValue>> Ops;
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("source_node"), TEXT("Node_0"));
	Ops.Add(MakeShared<FJsonValueObject>(Op));
	Args->SetArrayField(TEXT("operations"), Ops);
	FValidationResult R = FKismetAIValidation::ValidateEditBlueprint(Args);
	TestFalse(TEXT("Missing action is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidation_EditBP_ConnectMissingPins, "KismetAI.Validation.EditBlueprint.ConnectMissingPins", ValidationTestFlags)
bool FValidation_EditBP_ConnectMissingPins::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_Test"));
	TArray<TSharedPtr<FJsonValue>> Ops;
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("connect"));
	Op->SetStringField(TEXT("source_node"), TEXT("Node_0"));
	// Missing source_pin, target_node, target_pin
	Ops.Add(MakeShared<FJsonValueObject>(Op));
	Args->SetArrayField(TEXT("operations"), Ops);
	FValidationResult R = FKismetAIValidation::ValidateEditBlueprint(Args);
	TestFalse(TEXT("Connect missing pins is invalid"), R.bIsValid);
	TestTrue(TEXT("Multiple errors reported"), R.Errors.Num() >= 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidation_EditBP_ValidConnect, "KismetAI.Validation.EditBlueprint.ValidConnect", ValidationTestFlags)
bool FValidation_EditBP_ValidConnect::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_Test"));
	TArray<TSharedPtr<FJsonValue>> Ops;
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("connect"));
	Op->SetStringField(TEXT("source_node"), TEXT("Node_0"));
	Op->SetStringField(TEXT("source_pin"), TEXT("then"));
	Op->SetStringField(TEXT("target_node"), TEXT("Node_1"));
	Op->SetStringField(TEXT("target_pin"), TEXT("execute"));
	Ops.Add(MakeShared<FJsonValueObject>(Op));
	Args->SetArrayField(TEXT("operations"), Ops);
	FValidationResult R = FKismetAIValidation::ValidateEditBlueprint(Args);
	TestTrue(TEXT("Valid connect passes"), R.bIsValid);
	return true;
}

// --- Null args ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidation_NullArgs, "KismetAI.Validation.NullArgs", ValidationTestFlags)
bool FValidation_NullArgs::RunTest(const FString& Parameters)
{
	FValidationResult R = FKismetAIValidation::ValidateReadBlueprint(nullptr);
	TestFalse(TEXT("Null args is invalid"), R.bIsValid);
	R = FKismetAIValidation::ValidateWriteBlueprint(nullptr);
	TestFalse(TEXT("Null args for write is invalid"), R.bIsValid);
	R = FKismetAIValidation::ValidateCompileBlueprint(nullptr);
	TestFalse(TEXT("Null args for compile is invalid"), R.bIsValid);
	R = FKismetAIValidation::ValidateEditBlueprint(nullptr);
	TestFalse(TEXT("Null args for edit is invalid"), R.bIsValid);
	return true;
}

// --- ValidateInsertNode: connect_from/connect_to are optional (Fix A) ---

// Build a minimally-valid insert_node arg set (blueprint + node menu name).
static TSharedPtr<FJsonObject> MakeInsertNodeArgs()
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_Test"));
	Args->SetStringField(TEXT("node"), TEXT("Divide"));
	return Args;
}

static TSharedPtr<FJsonObject> MakeWire(const FString& Node, const FString& Pin)
{
	auto W = MakeShared<FJsonObject>();
	W->SetStringField(TEXT("node"), Node);
	W->SetStringField(TEXT("pin"), Pin);
	W->SetBoolField(TEXT("splice"), false);
	return W;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidation_InsertNode_EmptyWireSpecIsValid, "KismetAI.Validation.InsertNode.EmptyWireSpecIsValid", ValidationTestFlags)
bool FValidation_InsertNode_EmptyWireSpecIsValid::RunTest(const FString& Parameters)
{
	// The schema sends connect_from/connect_to as objects with empty defaults; an empty
	// 'node' means "insert unwired" and must be VALID (regression for the bug where
	// {node:""} was rejected, blocking every unwired insert).
	auto Args = MakeInsertNodeArgs();
	Args->SetObjectField(TEXT("connect_from"), MakeWire(TEXT(""), TEXT("")));
	Args->SetObjectField(TEXT("connect_to"), MakeWire(TEXT(""), TEXT("")));
	FValidationResult R = FKismetAIValidation::ValidateInsertNode(Args);
	TestTrue(TEXT("Empty connect_from/connect_to (insert unwired) is valid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidation_InsertNode_AbsentWireSpecIsValid, "KismetAI.Validation.InsertNode.AbsentWireSpecIsValid", ValidationTestFlags)
bool FValidation_InsertNode_AbsentWireSpecIsValid::RunTest(const FString& Parameters)
{
	FValidationResult R = FKismetAIValidation::ValidateInsertNode(MakeInsertNodeArgs());
	TestTrue(TEXT("No connect_from/connect_to at all is valid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidation_InsertNode_PopulatedWireSpecIsValid, "KismetAI.Validation.InsertNode.PopulatedWireSpecIsValid", ValidationTestFlags)
bool FValidation_InsertNode_PopulatedWireSpecIsValid::RunTest(const FString& Parameters)
{
	auto Args = MakeInsertNodeArgs();
	Args->SetObjectField(TEXT("connect_from"), MakeWire(TEXT("K2Node_Event_0"), TEXT("then")));
	FValidationResult R = FKismetAIValidation::ValidateInsertNode(Args);
	TestTrue(TEXT("A populated wire spec is valid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidation_InsertNode_PinWithoutNodeIsInvalid, "KismetAI.Validation.InsertNode.PinWithoutNodeIsInvalid", ValidationTestFlags)
bool FValidation_InsertNode_PinWithoutNodeIsInvalid::RunTest(const FString& Parameters)
{
	// A pin given with no node is a malformed partial spec — still flagged.
	auto Args = MakeInsertNodeArgs();
	Args->SetObjectField(TEXT("connect_from"), MakeWire(TEXT(""), TEXT("then")));
	FValidationResult R = FKismetAIValidation::ValidateInsertNode(Args);
	TestFalse(TEXT("pin without node is invalid"), R.bIsValid);
	TestTrue(TEXT("error is on connect_from"), R.Errors.Num() > 0 && R.Errors[0].Field == TEXT("connect_from"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidation_InsertNode_MissingNodeMenuNameIsInvalid, "KismetAI.Validation.InsertNode.MissingNodeMenuNameIsInvalid", ValidationTestFlags)
bool FValidation_InsertNode_MissingNodeMenuNameIsInvalid::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_Test"));
	FValidationResult R = FKismetAIValidation::ValidateInsertNode(Args);
	TestFalse(TEXT("Missing 'node' menu name is invalid"), R.bIsValid);
	return true;
}

// --- ToJsonString format ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidation_ToJsonString, "KismetAI.Validation.ToJsonString.Format", ValidationTestFlags)
bool FValidation_ToJsonString::RunTest(const FString& Parameters)
{
	FValidationResult R;
	R.AddError(TEXT("blueprint"), TEXT("field is required"));
	FString Json = R.ToJsonString();
	TestTrue(TEXT("Contains success:false"), Json.Contains(TEXT("\"success\":false")) || Json.Contains(TEXT("\"success\": false")));
	TestTrue(TEXT("Contains error_code"), Json.Contains(TEXT("VALIDATION_ERROR")));
	TestTrue(TEXT("Contains message"), Json.Contains(TEXT("field is required")));
	TestTrue(TEXT("Contains suggestions"), Json.Contains(TEXT("suggestions")));
	return true;
}
