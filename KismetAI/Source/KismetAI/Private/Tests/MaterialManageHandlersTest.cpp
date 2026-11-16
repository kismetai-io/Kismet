// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Misc/AutomationTest.h"
#include "KismetAIModule.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "Materials/Material.h"

constexpr auto MatManageFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// manage_material changes blend mode + two-sided on a base material.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialManage_Settings, "KismetAI.Materials.Manage_Settings", MatManageFlags)
bool FMaterialManage_Settings::RunTest(const FString& Parameters)
{
	UPackage* Pkg = CreatePackage(TEXT("/Temp/KismetMatManageTest"));
	UMaterial* Mat = NewObject<UMaterial>(Pkg, TEXT("M_ManageTest"), RF_Standalone | RF_Transactional);
	const FString Path = Mat->GetPathName();

	TestEqual(TEXT("starts opaque"), (int32)Mat->GetBlendMode(), (int32)BLEND_Opaque);

	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("material"), Path);
	Body->SetStringField(TEXT("blend_mode"), TEXT("Translucent"));
	Body->SetBoolField(TEXT("two_sided"), true);

	FString Resp = FKismetAIModule::Get().ExecuteHandlerDirect(&FKismetAIModule::HandleManageMaterial, Body);
	Resp.RemoveFromStart(TEXT("<tool_use_error>"));
	Resp.RemoveFromEnd(TEXT("</tool_use_error>"));
	TSharedPtr<FJsonObject> Env;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp);
	FJsonSerializer::Deserialize(Reader, Env);

	TestTrue(TEXT("handler success"), Env.IsValid() && Env->GetBoolField(TEXT("success")));
	TestEqual(TEXT("now translucent"), (int32)Mat->GetBlendMode(), (int32)BLEND_Translucent);
	TestTrue(TEXT("now two-sided"), Mat->IsTwoSided());
	return true;
}
