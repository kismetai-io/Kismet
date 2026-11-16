// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Misc/AutomationTest.h"
#include "KismetAIModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "MaterialEditingLibrary.h"

constexpr auto MatInstFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// set_material_instance_parameter overrides a scalar on an instance and refreshes it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialInstance_SetScalar, "KismetAI.Materials.Instance_SetScalarParameter", MatInstFlags)
bool FMaterialInstance_SetScalar::RunTest(const FString& Parameters)
{
	UPackage* Pkg = CreatePackage(TEXT("/Temp/KismetMatInstTest"));
	if (!Pkg) { AddError(TEXT("could not create package")); return false; }

	// Parent material exposing a scalar parameter "Rough".
	UMaterial* Parent = NewObject<UMaterial>(Pkg, TEXT("M_InstParent"), RF_Standalone | RF_Transactional);
	UMaterialExpressionScalarParameter* Param = Cast<UMaterialExpressionScalarParameter>(
		UMaterialEditingLibrary::CreateMaterialExpression(Parent, UMaterialExpressionScalarParameter::StaticClass(), -300, 0));
	if (!Param) { AddError(TEXT("could not create scalar parameter")); return false; }
	Param->ParameterName = FName(TEXT("Rough"));
	Param->DefaultValue = 0.5f;

	// Instance of that parent.
	UMaterialInstanceConstant* MIC = NewObject<UMaterialInstanceConstant>(Pkg, TEXT("MI_Inst"), RF_Standalone | RF_Transactional);
	MIC->SetParentEditorOnly(Parent);
	const FString InstPath = MIC->GetPathName();

	// Override "Rough" -> 0.25 via the handler.
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("instance"), InstPath);
	TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
	P->SetStringField(TEXT("type"), TEXT("scalar"));
	P->SetStringField(TEXT("name"), TEXT("Rough"));
	P->SetNumberField(TEXT("value"), 0.25);
	Body->SetArrayField(TEXT("parameters"), { MakeShared<FJsonValueObject>(P) });

	FString Resp = FKismetAIModule::Get().ExecuteHandlerDirect(&FKismetAIModule::HandleSetMaterialInstanceParameter, Body);
	Resp.RemoveFromStart(TEXT("<tool_use_error>"));
	Resp.RemoveFromEnd(TEXT("</tool_use_error>"));
	TSharedPtr<FJsonObject> Env;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp);
	FJsonSerializer::Deserialize(Reader, Env);

	TestTrue(TEXT("handler success"), Env.IsValid() && Env->GetBoolField(TEXT("success")));

	// Verify the override took.
	const float Value = UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(MIC, FName(TEXT("Rough")));
	TestEqual(TEXT("Rough overridden to 0.25"), Value, 0.25f, KINDA_SMALL_NUMBER);
	return true;
}

// read_material overview on an instance reports value, overridden, and parent_value per parameter.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialInstance_OverviewValues, "KismetAI.Materials.Instance_OverviewParameterValues", MatInstFlags)
bool FMaterialInstance_OverviewValues::RunTest(const FString& Parameters)
{
	UPackage* Pkg = CreatePackage(TEXT("/Temp/KismetMatInstOverviewTest"));
	if (!Pkg) { AddError(TEXT("could not create package")); return false; }

	UMaterial* Parent = NewObject<UMaterial>(Pkg, TEXT("M_OverviewParent"), RF_Standalone | RF_Transactional);
	UMaterialExpressionScalarParameter* Param = Cast<UMaterialExpressionScalarParameter>(
		UMaterialEditingLibrary::CreateMaterialExpression(Parent, UMaterialExpressionScalarParameter::StaticClass(), -300, 0));
	if (!Param) { AddError(TEXT("could not create scalar parameter")); return false; }
	Param->ParameterName = FName(TEXT("Bright"));
	Param->DefaultValue = 1.0f;
	// Rebuild cached expression data so the parameter enumerates on the parent/instance.
	Parent->PostEditChange();

	UMaterialInstanceConstant* MIC = NewObject<UMaterialInstanceConstant>(Pkg, TEXT("MI_Overview"), RF_Standalone | RF_Transactional);
	MIC->SetParentEditorOnly(Parent);
	UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MIC, FName(TEXT("Bright")), 0.3f);
	UMaterialEditingLibrary::UpdateMaterialInstance(MIC);

	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("material"), MIC->GetPathName());

	FString Resp = FKismetAIModule::Get().ExecuteHandlerDirect(&FKismetAIModule::HandleMaterialOverview, Body);
	Resp.RemoveFromStart(TEXT("<tool_use_error>"));
	Resp.RemoveFromEnd(TEXT("</tool_use_error>"));
	TSharedPtr<FJsonObject> Env;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp);
	FJsonSerializer::Deserialize(Reader, Env);
	if (!Env.IsValid() || !Env->GetBoolField(TEXT("success"))) { AddError(TEXT("overview handler failed")); return false; }

	const TSharedPtr<FJsonObject> Data = Env->GetObjectField(TEXT("data"));
	const TArray<TSharedPtr<FJsonValue>>* Scalars = nullptr;
	if (!Data.IsValid() || !Data->TryGetArrayField(TEXT("scalar_parameters"), Scalars) || Scalars->Num() == 0)
	{
		AddError(TEXT("scalar_parameters missing or empty"));
		return false;
	}

	const TSharedPtr<FJsonObject> P = (*Scalars)[0]->AsObject();
	TestTrue(TEXT("parameter is an object with name"), P.IsValid() && P->GetStringField(TEXT("name")) == TEXT("Bright"));
	TestEqual(TEXT("value reflects the override"), P->GetNumberField(TEXT("value")), 0.3, 0.001);
	TestTrue(TEXT("overridden flag set"), P->GetBoolField(TEXT("overridden")));
	TestEqual(TEXT("parent_value reflects the parent default"), P->GetNumberField(TEXT("parent_value")), 1.0, 0.001);
	return true;
}

// A parameters[] entry wrapped one level deep fails with an error naming the wrapper key.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialInstance_SetParam_WrapperError, "KismetAI.Materials.Instance_SetParameterWrapperError", MatInstFlags)
bool FMaterialInstance_SetParam_WrapperError::RunTest(const FString& Parameters)
{
	UPackage* Pkg = CreatePackage(TEXT("/Temp/KismetMatInstWrapTest"));
	if (!Pkg) { AddError(TEXT("could not create package")); return false; }

	UMaterial* Parent = NewObject<UMaterial>(Pkg, TEXT("M_WrapParent"), RF_Standalone | RF_Transactional);
	UMaterialInstanceConstant* MIC = NewObject<UMaterialInstanceConstant>(Pkg, TEXT("MI_Wrap"), RF_Standalone | RF_Transactional);
	MIC->SetParentEditorOnly(Parent);

	// {"parameter": {"type":"scalar","name":"Rough","value":0.2}} — the classic double-wrap.
	TSharedPtr<FJsonObject> Inner = MakeShared<FJsonObject>();
	Inner->SetStringField(TEXT("type"), TEXT("scalar"));
	Inner->SetStringField(TEXT("name"), TEXT("Rough"));
	Inner->SetNumberField(TEXT("value"), 0.2);
	TSharedPtr<FJsonObject> Wrapped = MakeShared<FJsonObject>();
	Wrapped->SetObjectField(TEXT("parameter"), Inner);

	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("instance"), MIC->GetPathName());
	Body->SetArrayField(TEXT("parameters"), { MakeShared<FJsonValueObject>(Wrapped) });

	FString Resp = FKismetAIModule::Get().ExecuteHandlerDirect(&FKismetAIModule::HandleSetMaterialInstanceParameter, Body);
	TestTrue(TEXT("error names the wrapper key"), Resp.Contains(TEXT("nests the parameter under 'parameter'")));
	TestTrue(TEXT("error states the expected flat shape"), Resp.Contains(TEXT("flat object")));
	return true;
}
