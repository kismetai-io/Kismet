// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Misc/AutomationTest.h"
#include "KismetAIValidation.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

constexpr auto CreateAssetTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// Tests
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateAsset_NullArgs, "KismetAI.Validation.CreateAsset_NullArgs", CreateAssetTestFlags)
bool FCreateAsset_NullArgs::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> NullArgs;
	FValidationResult R = FKismetAIValidation::ValidateCreateAsset(NullArgs);
	TestFalse(TEXT("Null args is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateAsset_MissingRequired, "KismetAI.Validation.CreateAsset_MissingRequired", CreateAssetTestFlags)
bool FCreateAsset_MissingRequired::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	FValidationResult R = FKismetAIValidation::ValidateCreateAsset(Args);
	TestFalse(TEXT("Missing all required fields is invalid"), R.bIsValid);
	TestTrue(TEXT("Multiple errors for missing fields"), R.Errors.Num() >= 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateAsset_InvalidAssetType, "KismetAI.Validation.CreateAsset_InvalidAssetType", CreateAssetTestFlags)
bool FCreateAsset_InvalidAssetType::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_type"), TEXT("Widget"));
	Args->SetStringField(TEXT("name"), TEXT("MyWidget"));
	Args->SetStringField(TEXT("path"), TEXT("/Game/UI"));
	FValidationResult R = FKismetAIValidation::ValidateCreateAsset(Args);
	TestFalse(TEXT("Invalid asset_type is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateAsset_InvalidName_Spaces, "KismetAI.Validation.CreateAsset_InvalidName_Spaces", CreateAssetTestFlags)
bool FCreateAsset_InvalidName_Spaces::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_type"), TEXT("Blueprint"));
	Args->SetStringField(TEXT("name"), TEXT("My Blueprint"));
	Args->SetStringField(TEXT("path"), TEXT("/Game/Blueprints"));
	FValidationResult R = FKismetAIValidation::ValidateCreateAsset(Args);
	TestFalse(TEXT("Name with spaces is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateAsset_InvalidName_LeadingDigit, "KismetAI.Validation.CreateAsset_InvalidName_LeadingDigit", CreateAssetTestFlags)
bool FCreateAsset_InvalidName_LeadingDigit::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_type"), TEXT("Blueprint"));
	Args->SetStringField(TEXT("name"), TEXT("3DModel"));
	Args->SetStringField(TEXT("path"), TEXT("/Game/Blueprints"));
	FValidationResult R = FKismetAIValidation::ValidateCreateAsset(Args);
	TestFalse(TEXT("Name starting with digit is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateAsset_InvalidPath, "KismetAI.Validation.CreateAsset_InvalidPath", CreateAssetTestFlags)
bool FCreateAsset_InvalidPath::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_type"), TEXT("Blueprint"));
	Args->SetStringField(TEXT("name"), TEXT("BP_Test"));
	Args->SetStringField(TEXT("path"), TEXT("/Engine/Blueprints"));
	FValidationResult R = FKismetAIValidation::ValidateCreateAsset(Args);
	TestFalse(TEXT("Non /Game/ path is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateAsset_ValidBlueprint, "KismetAI.Validation.CreateAsset_ValidBlueprint", CreateAssetTestFlags)
bool FCreateAsset_ValidBlueprint::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_type"), TEXT("Blueprint"));
	Args->SetStringField(TEXT("name"), TEXT("BP_Enemy"));
	Args->SetStringField(TEXT("path"), TEXT("/Game/Characters"));
	Args->SetStringField(TEXT("parent_class"), TEXT("Character"));
	FValidationResult R = FKismetAIValidation::ValidateCreateAsset(Args);
	TestTrue(TEXT("Valid Blueprint args passes"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateAsset_InvalidBlueprintType, "KismetAI.Validation.CreateAsset_InvalidBlueprintType", CreateAssetTestFlags)
bool FCreateAsset_InvalidBlueprintType::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_type"), TEXT("Blueprint"));
	Args->SetStringField(TEXT("name"), TEXT("BP_Test"));
	Args->SetStringField(TEXT("path"), TEXT("/Game/Blueprints"));
	Args->SetStringField(TEXT("blueprint_type"), TEXT("widget"));
	FValidationResult R = FKismetAIValidation::ValidateCreateAsset(Args);
	TestFalse(TEXT("Invalid blueprint_type is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateAsset_StructDuplicateFields, "KismetAI.Validation.CreateAsset_StructDuplicateFields", CreateAssetTestFlags)
bool FCreateAsset_StructDuplicateFields::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_type"), TEXT("UserDefinedStruct"));
	Args->SetStringField(TEXT("name"), TEXT("S_Test"));
	Args->SetStringField(TEXT("path"), TEXT("/Game/Data"));

	TArray<TSharedPtr<FJsonValue>> Fields;
	auto Field1 = MakeShared<FJsonObject>();
	Field1->SetStringField(TEXT("name"), TEXT("Health"));
	Field1->SetStringField(TEXT("type"), TEXT("float"));
	Fields.Add(MakeShared<FJsonValueObject>(Field1));

	auto Field2 = MakeShared<FJsonObject>();
	Field2->SetStringField(TEXT("name"), TEXT("Health"));
	Field2->SetStringField(TEXT("type"), TEXT("int"));
	Fields.Add(MakeShared<FJsonValueObject>(Field2));

	Args->SetArrayField(TEXT("fields"), Fields);
	FValidationResult R = FKismetAIValidation::ValidateCreateAsset(Args);
	TestFalse(TEXT("Duplicate struct field names rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateAsset_StructMissingFieldType, "KismetAI.Validation.CreateAsset_StructMissingFieldType", CreateAssetTestFlags)
bool FCreateAsset_StructMissingFieldType::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_type"), TEXT("UserDefinedStruct"));
	Args->SetStringField(TEXT("name"), TEXT("S_Test"));
	Args->SetStringField(TEXT("path"), TEXT("/Game/Data"));

	TArray<TSharedPtr<FJsonValue>> Fields;
	auto Field1 = MakeShared<FJsonObject>();
	Field1->SetStringField(TEXT("name"), TEXT("Health"));
	// missing type
	Fields.Add(MakeShared<FJsonValueObject>(Field1));

	Args->SetArrayField(TEXT("fields"), Fields);
	FValidationResult R = FKismetAIValidation::ValidateCreateAsset(Args);
	TestFalse(TEXT("Struct field without type rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateAsset_EnumDuplicateEntries, "KismetAI.Validation.CreateAsset_EnumDuplicateEntries", CreateAssetTestFlags)
bool FCreateAsset_EnumDuplicateEntries::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_type"), TEXT("UserDefinedEnum"));
	Args->SetStringField(TEXT("name"), TEXT("E_Test"));
	Args->SetStringField(TEXT("path"), TEXT("/Game/Data"));

	TArray<TSharedPtr<FJsonValue>> Entries;
	Entries.Add(MakeShared<FJsonValueString>(TEXT("Low")));
	Entries.Add(MakeShared<FJsonValueString>(TEXT("High")));
	Entries.Add(MakeShared<FJsonValueString>(TEXT("Low")));
	Args->SetArrayField(TEXT("entries"), Entries);

	FValidationResult R = FKismetAIValidation::ValidateCreateAsset(Args);
	TestFalse(TEXT("Duplicate enum entries rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateAsset_ValidStruct, "KismetAI.Validation.CreateAsset_ValidStruct", CreateAssetTestFlags)
bool FCreateAsset_ValidStruct::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_type"), TEXT("UserDefinedStruct"));
	Args->SetStringField(TEXT("name"), TEXT("S_ItemData"));
	Args->SetStringField(TEXT("path"), TEXT("/Game/Data"));

	TArray<TSharedPtr<FJsonValue>> Fields;
	auto Field1 = MakeShared<FJsonObject>();
	Field1->SetStringField(TEXT("name"), TEXT("Name"));
	Field1->SetStringField(TEXT("type"), TEXT("string"));
	Fields.Add(MakeShared<FJsonValueObject>(Field1));
	auto Field2 = MakeShared<FJsonObject>();
	Field2->SetStringField(TEXT("name"), TEXT("Value"));
	Field2->SetStringField(TEXT("type"), TEXT("int"));
	Fields.Add(MakeShared<FJsonValueObject>(Field2));
	Args->SetArrayField(TEXT("fields"), Fields);

	FValidationResult R = FKismetAIValidation::ValidateCreateAsset(Args);
	TestTrue(TEXT("Valid struct args passes"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateAsset_ValidEnum, "KismetAI.Validation.CreateAsset_ValidEnum", CreateAssetTestFlags)
bool FCreateAsset_ValidEnum::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_type"), TEXT("UserDefinedEnum"));
	Args->SetStringField(TEXT("name"), TEXT("E_Rarity"));
	Args->SetStringField(TEXT("path"), TEXT("/Game/Data"));

	TArray<TSharedPtr<FJsonValue>> Entries;
	Entries.Add(MakeShared<FJsonValueString>(TEXT("Common")));
	Entries.Add(MakeShared<FJsonValueString>(TEXT("Rare")));
	Entries.Add(MakeShared<FJsonValueString>(TEXT("Epic")));
	Args->SetArrayField(TEXT("entries"), Entries);

	FValidationResult R = FKismetAIValidation::ValidateCreateAsset(Args);
	TestTrue(TEXT("Valid enum args passes"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateAsset_NameWithUnderscore, "KismetAI.Validation.CreateAsset_NameWithUnderscore", CreateAssetTestFlags)
bool FCreateAsset_NameWithUnderscore::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_type"), TEXT("Blueprint"));
	Args->SetStringField(TEXT("name"), TEXT("_BP_Test_01"));
	Args->SetStringField(TEXT("path"), TEXT("/Game/Blueprints"));
	FValidationResult R = FKismetAIValidation::ValidateCreateAsset(Args);
	TestTrue(TEXT("Name with underscores and digits is valid"), R.bIsValid);
	return true;
}

// ── Phase 4: animation asset types ───────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateAsset_AnimMontageNeedsSkeleton, "KismetAI.Validation.CreateAsset_AnimMontageNeedsSkeleton", CreateAssetTestFlags)
bool FCreateAsset_AnimMontageNeedsSkeleton::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_type"), TEXT("AnimMontage"));
	Args->SetStringField(TEXT("name"), TEXT("AM_Attack"));
	Args->SetStringField(TEXT("path"), TEXT("/Game/Anim"));
	FValidationResult R = FKismetAIValidation::ValidateCreateAsset(Args);
	TestFalse(TEXT("Montage without skeleton fails validation"), R.bIsValid);

	Args->SetStringField(TEXT("skeleton"), TEXT("/Game/Anim/SK_Hero_Skeleton"));
	FValidationResult R2 = FKismetAIValidation::ValidateCreateAsset(Args);
	TestTrue(TEXT("Montage with skeleton passes validation"), R2.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateAsset_AnimBlueprintTemplate, "KismetAI.Validation.CreateAsset_AnimBlueprintTemplate", CreateAssetTestFlags)
bool FCreateAsset_AnimBlueprintTemplate::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_type"), TEXT("AnimBlueprint"));
	Args->SetStringField(TEXT("name"), TEXT("ABP_Hero"));
	Args->SetStringField(TEXT("path"), TEXT("/Game/Anim"));
	FValidationResult R = FKismetAIValidation::ValidateCreateAsset(Args);
	TestFalse(TEXT("AnimBlueprint without skeleton/template fails"), R.bIsValid);

	Args->SetBoolField(TEXT("template"), true);
	FValidationResult R2 = FKismetAIValidation::ValidateCreateAsset(Args);
	TestTrue(TEXT("AnimBlueprint template passes without skeleton"), R2.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimation_ValidateBatch, "KismetAI.Validation.Animation_Batch", CreateAssetTestFlags)
bool FAnimation_ValidateBatch::RunTest(const FString& Parameters)
{
	// Missing asset.
	{
		auto Args = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Ops;
		auto Get = MakeShared<FJsonObject>(); Get->SetStringField(TEXT("action"), TEXT("get"));
		Ops.Add(MakeShared<FJsonValueObject>(Get));
		Args->SetArrayField(TEXT("operations"), Ops);
		TestFalse(TEXT("Missing asset fails"), FKismetAIValidation::ValidateAnimation(Args).bIsValid);
	}
	// add_segment without animation.
	{
		auto Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset"), TEXT("/Game/Anim/AM_X"));
		auto Op = MakeShared<FJsonObject>(); Op->SetStringField(TEXT("action"), TEXT("add_segment"));
		TArray<TSharedPtr<FJsonValue>> Ops; Ops.Add(MakeShared<FJsonValueObject>(Op));
		Args->SetArrayField(TEXT("operations"), Ops);
		TestFalse(TEXT("add_segment without animation fails"), FKismetAIValidation::ValidateAnimation(Args).bIsValid);
	}
	// add_notify with neither name nor class.
	{
		auto Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset"), TEXT("/Game/Anim/A_X"));
		auto Op = MakeShared<FJsonObject>(); Op->SetStringField(TEXT("action"), TEXT("add_notify"));
		Op->SetNumberField(TEXT("time"), 0.0);
		TArray<TSharedPtr<FJsonValue>> Ops; Ops.Add(MakeShared<FJsonValueObject>(Op));
		Args->SetArrayField(TEXT("operations"), Ops);
		TestFalse(TEXT("add_notify needs name or class"), FKismetAIValidation::ValidateAnimation(Args).bIsValid);
	}
	// set_axis with bad axis.
	{
		auto Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset"), TEXT("/Game/Anim/BS_X"));
		auto Op = MakeShared<FJsonObject>(); Op->SetStringField(TEXT("action"), TEXT("set_axis"));
		Op->SetStringField(TEXT("axis"), TEXT("z"));
		Op->SetStringField(TEXT("name"), TEXT("Speed"));
		Op->SetNumberField(TEXT("min"), 0.0); Op->SetNumberField(TEXT("max"), 1.0);
		TArray<TSharedPtr<FJsonValue>> Ops; Ops.Add(MakeShared<FJsonValueObject>(Op));
		Args->SetArrayField(TEXT("operations"), Ops);
		TestFalse(TEXT("set_axis z is invalid"), FKismetAIValidation::ValidateAnimation(Args).bIsValid);
	}
	// Valid batch.
	{
		auto Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset"), TEXT("/Game/Anim/AM_X"));
		auto Op = MakeShared<FJsonObject>(); Op->SetStringField(TEXT("action"), TEXT("add_slot_track"));
		Op->SetStringField(TEXT("slot_name"), TEXT("DefaultSlot"));
		TArray<TSharedPtr<FJsonValue>> Ops; Ops.Add(MakeShared<FJsonValueObject>(Op));
		Args->SetArrayField(TEXT("operations"), Ops);
		TestTrue(TEXT("valid add_slot_track passes"), FKismetAIValidation::ValidateAnimation(Args).bIsValid);
	}
	return true;
}
