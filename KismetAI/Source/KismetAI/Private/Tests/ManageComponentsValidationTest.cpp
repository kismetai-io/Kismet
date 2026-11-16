// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Misc/AutomationTest.h"
#include "KismetAIValidation.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

constexpr auto ManageCompTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace ManageCompTestHelper
{
	/** Build a manage_components Args object with a single operation. */
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageComp_MissingBlueprint, "KismetAI.Validation.ManageComponents_MissingBlueprint", ManageCompTestFlags)
bool FManageComp_MissingBlueprint::RunTest(const FString& Parameters)
{
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("add_component"));
	Op->SetStringField(TEXT("class"), TEXT("StaticMeshComponent"));
	Op->SetStringField(TEXT("name"), TEXT("MyMesh"));

	auto Args = ManageCompTestHelper::MakeArgs(TEXT(""), Op);
	FValidationResult R = FKismetAIValidation::ValidateManageComponents(Args);
	TestFalse(TEXT("Missing blueprint is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageComp_EmptyOperations, "KismetAI.Validation.ManageComponents_EmptyOperations", ManageCompTestFlags)
bool FManageComp_EmptyOperations::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_Test"));
	TArray<TSharedPtr<FJsonValue>> EmptyOps;
	Args->SetArrayField(TEXT("operations"), EmptyOps);

	FValidationResult R = FKismetAIValidation::ValidateManageComponents(Args);
	TestFalse(TEXT("Empty operations is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageComp_AddComponent_MissingClass, "KismetAI.Validation.ManageComponents_AddComponent_MissingClass", ManageCompTestFlags)
bool FManageComp_AddComponent_MissingClass::RunTest(const FString& Parameters)
{
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("add_component"));
	Op->SetStringField(TEXT("name"), TEXT("MyMesh"));

	auto Args = ManageCompTestHelper::MakeArgs(TEXT("/Game/BP_Test"), Op);
	FValidationResult R = FKismetAIValidation::ValidateManageComponents(Args);
	TestFalse(TEXT("add_component without class is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageComp_AddComponent_MissingName, "KismetAI.Validation.ManageComponents_AddComponent_MissingName", ManageCompTestFlags)
bool FManageComp_AddComponent_MissingName::RunTest(const FString& Parameters)
{
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("add_component"));
	Op->SetStringField(TEXT("class"), TEXT("StaticMeshComponent"));

	auto Args = ManageCompTestHelper::MakeArgs(TEXT("/Game/BP_Test"), Op);
	FValidationResult R = FKismetAIValidation::ValidateManageComponents(Args);
	TestFalse(TEXT("add_component without name is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageComp_AddComponent_Valid, "KismetAI.Validation.ManageComponents_AddComponent_Valid", ManageCompTestFlags)
bool FManageComp_AddComponent_Valid::RunTest(const FString& Parameters)
{
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("add_component"));
	Op->SetStringField(TEXT("class"), TEXT("StaticMeshComponent"));
	Op->SetStringField(TEXT("name"), TEXT("MyMesh"));

	auto Args = ManageCompTestHelper::MakeArgs(TEXT("/Game/BP_Test"), Op);
	FValidationResult R = FKismetAIValidation::ValidateManageComponents(Args);
	TestTrue(TEXT("add_component with class+name is valid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageComp_SetProperty_MissingFields, "KismetAI.Validation.ManageComponents_SetProperty_MissingFields", ManageCompTestFlags)
bool FManageComp_SetProperty_MissingFields::RunTest(const FString& Parameters)
{
	// Missing name, property, and value
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("set_component_property"));

	auto Args = ManageCompTestHelper::MakeArgs(TEXT("/Game/BP_Test"), Op);
	FValidationResult R = FKismetAIValidation::ValidateManageComponents(Args);
	TestFalse(TEXT("set_component_property without required fields is invalid"), R.bIsValid);
	TestTrue(TEXT("Multiple errors reported"), R.Errors.Num() >= 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageComp_RemoveComponent_Valid, "KismetAI.Validation.ManageComponents_RemoveComponent_Valid", ManageCompTestFlags)
bool FManageComp_RemoveComponent_Valid::RunTest(const FString& Parameters)
{
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("remove_component"));
	Op->SetStringField(TEXT("name"), TEXT("MyMesh"));

	auto Args = ManageCompTestHelper::MakeArgs(TEXT("/Game/BP_Test"), Op);
	FValidationResult R = FKismetAIValidation::ValidateManageComponents(Args);
	TestTrue(TEXT("remove_component with name is valid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageComp_SetPostProcessMaterial_Valid, "KismetAI.Validation.ManageComponents_SetPostProcessMaterial_Valid", ManageCompTestFlags)
bool FManageComp_SetPostProcessMaterial_Valid::RunTest(const FString& Parameters)
{
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("set_post_process_material"));
	Op->SetStringField(TEXT("name"), TEXT("PostProcess"));
	Op->SetStringField(TEXT("material"), TEXT("/Game/Materials/M_Vision"));

	auto Args = ManageCompTestHelper::MakeArgs(TEXT("/Game/BP_Test"), Op);
	FValidationResult R = FKismetAIValidation::ValidateManageComponents(Args);
	TestTrue(TEXT("set_post_process_material with name+material is valid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageComp_SetPostProcessMaterial_MissingMaterial, "KismetAI.Validation.ManageComponents_SetPostProcessMaterial_MissingMaterial", ManageCompTestFlags)
bool FManageComp_SetPostProcessMaterial_MissingMaterial::RunTest(const FString& Parameters)
{
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("set_post_process_material"));
	Op->SetStringField(TEXT("name"), TEXT("PostProcess"));

	auto Args = ManageCompTestHelper::MakeArgs(TEXT("/Game/BP_Test"), Op);
	FValidationResult R = FKismetAIValidation::ValidateManageComponents(Args);
	TestFalse(TEXT("set_post_process_material without material is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageComp_GetComponentProperty_Valid, "KismetAI.Validation.ManageComponents_GetComponentProperty_Valid", ManageCompTestFlags)
bool FManageComp_GetComponentProperty_Valid::RunTest(const FString& Parameters)
{
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("get_component_property"));
	Op->SetStringField(TEXT("name"), TEXT("MyMesh"));

	auto Args = ManageCompTestHelper::MakeArgs(TEXT("/Game/BP_Test"), Op);
	FValidationResult R = FKismetAIValidation::ValidateManageComponents(Args);
	TestTrue(TEXT("get_component_property with name is valid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageComp_GetComponentProperty_WithProperty_Valid, "KismetAI.Validation.ManageComponents_GetComponentProperty_WithProperty_Valid", ManageCompTestFlags)
bool FManageComp_GetComponentProperty_WithProperty_Valid::RunTest(const FString& Parameters)
{
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("get_component_property"));
	Op->SetStringField(TEXT("name"), TEXT("MyMesh"));
	Op->SetStringField(TEXT("property"), TEXT("OverrideMaterials"));

	auto Args = ManageCompTestHelper::MakeArgs(TEXT("/Game/BP_Test"), Op);
	FValidationResult R = FKismetAIValidation::ValidateManageComponents(Args);
	TestTrue(TEXT("get_component_property with name+property is valid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManageComp_GetComponentProperty_MissingName, "KismetAI.Validation.ManageComponents_GetComponentProperty_MissingName", ManageCompTestFlags)
bool FManageComp_GetComponentProperty_MissingName::RunTest(const FString& Parameters)
{
	auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("get_component_property"));

	auto Args = ManageCompTestHelper::MakeArgs(TEXT("/Game/BP_Test"), Op);
	FValidationResult R = FKismetAIValidation::ValidateManageComponents(Args);
	TestFalse(TEXT("get_component_property without name is invalid"), R.bIsValid);
	return true;
}
