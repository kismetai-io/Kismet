// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Misc/AutomationTest.h"
#include "KismetAIValidation.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

constexpr auto ManageBPTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace ManageBPTestHelper
{
	/** Build a valid manage_blueprint Args object with a single operation. */
	TSharedPtr<FJsonObject> MakeArgs(const FString& Blueprint, TSharedPtr<FJsonObject> Op)
	{
		auto Args = MakeShared<FJsonObject>();
		if (!Blueprint.IsEmpty())
		{
			Args->SetStringField(TEXT("blueprint"), Blueprint);
		}

		TArray<TSharedPtr<FJsonValue>> Ops;
		if (Op.IsValid())
		{
			Ops.Add(MakeShared<FJsonValueObject>(Op));
		}
		Args->SetArrayField(TEXT("operations"), Ops);

		return Args;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageBP_MissingBlueprint, "KismetAI.Validation.ManageBlueprint_MissingBlueprint", ManageBPTestFlags)
bool FManageBP_MissingBlueprint::RunTest(const FString& Parameters)
{
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("add_variable"));
	Op->SetStringField(TEXT("name"), TEXT("Health"));
	Op->SetStringField(TEXT("type"), TEXT("float"));

	auto Args = ManageBPTestHelper::MakeArgs(TEXT(""), Op);
	FValidationResult R = FKismetAIValidation::ValidateManageBlueprint(Args);
	TestFalse(TEXT("Missing blueprint is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageBP_EmptyOperations, "KismetAI.Validation.ManageBlueprint_EmptyOperations", ManageBPTestFlags)
bool FManageBP_EmptyOperations::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_Test"));
	TArray<TSharedPtr<FJsonValue>> EmptyOps;
	Args->SetArrayField(TEXT("operations"), EmptyOps);

	FValidationResult R = FKismetAIValidation::ValidateManageBlueprint(Args);
	TestFalse(TEXT("Empty operations is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageBP_UnknownAction, "KismetAI.Validation.ManageBlueprint_UnknownAction", ManageBPTestFlags)
bool FManageBP_UnknownAction::RunTest(const FString& Parameters)
{
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("foo"));

	auto Args = ManageBPTestHelper::MakeArgs(TEXT("/Game/BP_Test"), Op);
	FValidationResult R = FKismetAIValidation::ValidateManageBlueprint(Args);
	TestFalse(TEXT("Unknown action is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageBP_AddVariable_MissingName, "KismetAI.Validation.ManageBlueprint_AddVariable_MissingName", ManageBPTestFlags)
bool FManageBP_AddVariable_MissingName::RunTest(const FString& Parameters)
{
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("add_variable"));
	Op->SetStringField(TEXT("type"), TEXT("float"));

	auto Args = ManageBPTestHelper::MakeArgs(TEXT("/Game/BP_Test"), Op);
	FValidationResult R = FKismetAIValidation::ValidateManageBlueprint(Args);
	TestFalse(TEXT("add_variable without name is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageBP_AddVariable_MissingType, "KismetAI.Validation.ManageBlueprint_AddVariable_MissingType", ManageBPTestFlags)
bool FManageBP_AddVariable_MissingType::RunTest(const FString& Parameters)
{
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("add_variable"));
	Op->SetStringField(TEXT("name"), TEXT("Health"));

	auto Args = ManageBPTestHelper::MakeArgs(TEXT("/Game/BP_Test"), Op);
	FValidationResult R = FKismetAIValidation::ValidateManageBlueprint(Args);
	TestFalse(TEXT("add_variable without type is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageBP_AddVariable_Valid, "KismetAI.Validation.ManageBlueprint_AddVariable_Valid", ManageBPTestFlags)
bool FManageBP_AddVariable_Valid::RunTest(const FString& Parameters)
{
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("add_variable"));
	Op->SetStringField(TEXT("name"), TEXT("Health"));
	Op->SetStringField(TEXT("type"), TEXT("float"));

	auto Args = ManageBPTestHelper::MakeArgs(TEXT("/Game/BP_Test"), Op);
	FValidationResult R = FKismetAIValidation::ValidateManageBlueprint(Args);
	TestTrue(TEXT("add_variable with name+type is valid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageBP_AddFunction_Valid, "KismetAI.Validation.ManageBlueprint_AddFunction_Valid", ManageBPTestFlags)
bool FManageBP_AddFunction_Valid::RunTest(const FString& Parameters)
{
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("add_function"));
	Op->SetStringField(TEXT("name"), TEXT("CalculateDamage"));

	auto Args = ManageBPTestHelper::MakeArgs(TEXT("/Game/BP_Test"), Op);
	FValidationResult R = FKismetAIValidation::ValidateManageBlueprint(Args);
	TestTrue(TEXT("add_function with name is valid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageBP_AddInterface_MissingPath, "KismetAI.Validation.ManageBlueprint_AddInterface_MissingPath", ManageBPTestFlags)
bool FManageBP_AddInterface_MissingPath::RunTest(const FString& Parameters)
{
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("add_interface"));

	auto Args = ManageBPTestHelper::MakeArgs(TEXT("/Game/BP_Test"), Op);
	FValidationResult R = FKismetAIValidation::ValidateManageBlueprint(Args);
	TestFalse(TEXT("add_interface without interface_path is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageBP_SetParentClass_MissingPath, "KismetAI.Validation.ManageBlueprint_SetParentClass_MissingPath", ManageBPTestFlags)
bool FManageBP_SetParentClass_MissingPath::RunTest(const FString& Parameters)
{
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("set_parent_class"));

	auto Args = ManageBPTestHelper::MakeArgs(TEXT("/Game/BP_Test"), Op);
	FValidationResult R = FKismetAIValidation::ValidateManageBlueprint(Args);
	TestFalse(TEXT("set_parent_class without parent_class_path is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageBP_GetClassDefault_Valid, "KismetAI.Validation.ManageBlueprint_GetClassDefault_Valid", ManageBPTestFlags)
bool FManageBP_GetClassDefault_Valid::RunTest(const FString& Parameters)
{
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("get_class_default"));
	Op->SetStringField(TEXT("property"), TEXT("DefaultPawnClass"));

	auto Args = ManageBPTestHelper::MakeArgs(TEXT("/Game/BP_Test"), Op);
	FValidationResult R = FKismetAIValidation::ValidateManageBlueprint(Args);
	TestTrue(TEXT("get_class_default with property is valid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageBP_GetClassDefault_NoProperty_Valid, "KismetAI.Validation.ManageBlueprint_GetClassDefault_NoProperty_Valid", ManageBPTestFlags)
bool FManageBP_GetClassDefault_NoProperty_Valid::RunTest(const FString& Parameters)
{
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("get_class_default"));

	auto Args = ManageBPTestHelper::MakeArgs(TEXT("/Game/BP_Test"), Op);
	FValidationResult R = FKismetAIValidation::ValidateManageBlueprint(Args);
	TestTrue(TEXT("get_class_default without property is valid (lists modified defaults)"), R.bIsValid);
	return true;
}
