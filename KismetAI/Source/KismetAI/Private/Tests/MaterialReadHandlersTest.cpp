// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Misc/AutomationTest.h"
#include "KismetAIModule.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Materials/Material.h"

constexpr auto MatHandlerFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	// Run a handler in-process and return the parsed {"success":..,"data":..} envelope.
	static TSharedPtr<FJsonObject> CallHandler(
		bool (FKismetAIModule::*Handler)(const FHttpServerRequest&, const FHttpResultCallback&),
		const FString& MaterialPath)
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("material"), MaterialPath);

		FString Resp = FKismetAIModule::Get().ExecuteHandlerDirect(Handler, Body);

		// ExecuteHandlerDirect wraps failure payloads in <tool_use_error>...</tool_use_error>.
		Resp.RemoveFromStart(TEXT("<tool_use_error>"));
		Resp.RemoveFromEnd(TEXT("</tool_use_error>"));

		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp);
		FJsonSerializer::Deserialize(Reader, Parsed);
		return Parsed;
	}

	static const TCHAR* DefaultMaterialPath = TEXT("/Engine/EngineMaterials/DefaultMaterial");
}

// material_overview on a real base material reports type/domain/parameter arrays.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialOverview_Default, "KismetAI.Materials.Overview_DefaultMaterial", MatHandlerFlags)
bool FMaterialOverview_Default::RunTest(const FString& Parameters)
{
	if (!StaticLoadObject(UMaterial::StaticClass(), nullptr, DefaultMaterialPath))
	{
		AddWarning(TEXT("DefaultMaterial not loadable in this environment; skipping"));
		return true;
	}

	TSharedPtr<FJsonObject> Env = CallHandler(&FKismetAIModule::HandleMaterialOverview, DefaultMaterialPath);
	TestTrue(TEXT("response parsed"), Env.IsValid());
	if (!Env.IsValid()) return false;

	TestTrue(TEXT("success=true"), Env->GetBoolField(TEXT("success")));
	const TSharedPtr<FJsonObject>* Data;
	TestTrue(TEXT("has data"), Env->TryGetObjectField(TEXT("data"), Data));
	if (Data)
	{
		TestEqual(TEXT("type is material"), (*Data)->GetStringField(TEXT("type")), FString(TEXT("material")));
		const TArray<TSharedPtr<FJsonValue>>* Scalars;
		TestTrue(TEXT("has scalar_parameters array"), (*Data)->TryGetArrayField(TEXT("scalar_parameters"), Scalars));
	}
	return true;
}

// describe_material returns the data-flow view: expressions array + root_connections.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialDescribe_Default, "KismetAI.Materials.Describe_DefaultMaterial", MatHandlerFlags)
bool FMaterialDescribe_Default::RunTest(const FString& Parameters)
{
	if (!StaticLoadObject(UMaterial::StaticClass(), nullptr, DefaultMaterialPath))
	{
		AddWarning(TEXT("DefaultMaterial not loadable in this environment; skipping"));
		return true;
	}

	TSharedPtr<FJsonObject> Env = CallHandler(&FKismetAIModule::HandleDescribeMaterial, DefaultMaterialPath);
	TestTrue(TEXT("response parsed"), Env.IsValid());
	if (!Env.IsValid()) return false;

	TestTrue(TEXT("success=true"), Env->GetBoolField(TEXT("success")));
	const TSharedPtr<FJsonObject>* Data;
	if (Env->TryGetObjectField(TEXT("data"), Data) && Data)
	{
		const TArray<TSharedPtr<FJsonValue>>* Exprs;
		TestTrue(TEXT("has expressions array"), (*Data)->TryGetArrayField(TEXT("expressions"), Exprs));
		const TArray<TSharedPtr<FJsonValue>>* Roots;
		TestTrue(TEXT("has root_connections array"), (*Data)->TryGetArrayField(TEXT("root_connections"), Roots));
	}
	return true;
}

// A bogus path yields a structured not-found error envelope, not a crash.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialOverview_NotFound, "KismetAI.Materials.Overview_NotFound", MatHandlerFlags)
bool FMaterialOverview_NotFound::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Env = CallHandler(&FKismetAIModule::HandleMaterialOverview,
		TEXT("/Game/__DoesNotExist__/M_Nope"));
	TestTrue(TEXT("response parsed"), Env.IsValid());
	// ExecuteHandlerDirect wraps failures; success must not be true.
	if (Env.IsValid())
	{
		bool bSuccess = false;
		Env->TryGetBoolField(TEXT("success"), bSuccess);
		TestFalse(TEXT("success=false for missing material"), bSuccess);
	}
	return true;
}
