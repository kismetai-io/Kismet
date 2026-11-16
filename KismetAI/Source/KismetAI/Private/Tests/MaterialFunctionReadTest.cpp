// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Misc/AutomationTest.h"
#include "KismetAIModule.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Materials/MaterialFunction.h"

constexpr auto MatFuncTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	static TSharedPtr<FJsonObject> CallMaterialHandler(
		bool (FKismetAIModule::*Handler)(const FHttpServerRequest&, const FHttpResultCallback&),
		const FString& MaterialPath)
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("material"), MaterialPath);

		FString Resp = FKismetAIModule::Get().ExecuteHandlerDirect(Handler, Body);
		Resp.RemoveFromStart(TEXT("<tool_use_error>"));
		Resp.RemoveFromEnd(TEXT("</tool_use_error>"));

		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp);
		FJsonSerializer::Deserialize(Reader, Parsed);
		return Parsed;
	}

	// A stock engine Material Function: one FunctionInput ("Power"), one FunctionOutput.
	static const TCHAR* EngineFunctionPath = TEXT("/Engine/Functions/Engine_MaterialFunctions01/Shading/PowerToRoughness");
}

// describe on a Material Function returns its interface + inner expression graph.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialFunctionDescribe_Engine, "KismetAI.Materials.FunctionDescribe_PowerToRoughness", MatFuncTestFlags)
bool FMaterialFunctionDescribe_Engine::RunTest(const FString& Parameters)
{
	if (!StaticLoadObject(UMaterialFunction::StaticClass(), nullptr, EngineFunctionPath))
	{
		AddWarning(TEXT("Engine material function not loadable in this environment; skipping"));
		return true;
	}

	TSharedPtr<FJsonObject> Env = CallMaterialHandler(&FKismetAIModule::HandleDescribeMaterial, EngineFunctionPath);
	TestTrue(TEXT("response parsed"), Env.IsValid());
	if (!Env.IsValid()) return false;

	TestTrue(TEXT("success=true"), Env->GetBoolField(TEXT("success")));
	const TSharedPtr<FJsonObject>* Data;
	if (Env->TryGetObjectField(TEXT("data"), Data) && Data)
	{
		TestEqual(TEXT("type is material_function"), (*Data)->GetStringField(TEXT("type")), FString(TEXT("material_function")));

		const TArray<TSharedPtr<FJsonValue>>* FuncInputs;
		TestTrue(TEXT("has function_inputs array"), (*Data)->TryGetArrayField(TEXT("function_inputs"), FuncInputs));
		if (FuncInputs)
		{
			TestTrue(TEXT("at least one function input"), FuncInputs->Num() >= 1);
		}

		const TArray<TSharedPtr<FJsonValue>>* FuncOutputs;
		TestTrue(TEXT("has function_outputs array"), (*Data)->TryGetArrayField(TEXT("function_outputs"), FuncOutputs));
		if (FuncOutputs)
		{
			TestTrue(TEXT("at least one function output"), FuncOutputs->Num() >= 1);
		}

		const TArray<TSharedPtr<FJsonValue>>* Exprs;
		TestTrue(TEXT("has expressions array"), (*Data)->TryGetArrayField(TEXT("expressions"), Exprs));
		if (Exprs)
		{
			TestTrue(TEXT("inner graph is not empty"), Exprs->Num() >= 2);
		}
	}
	return true;
}

// overview on a Material Function reports the interface summary without the graph.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialFunctionOverview_Engine, "KismetAI.Materials.FunctionOverview_PowerToRoughness", MatFuncTestFlags)
bool FMaterialFunctionOverview_Engine::RunTest(const FString& Parameters)
{
	if (!StaticLoadObject(UMaterialFunction::StaticClass(), nullptr, EngineFunctionPath))
	{
		AddWarning(TEXT("Engine material function not loadable in this environment; skipping"));
		return true;
	}

	TSharedPtr<FJsonObject> Env = CallMaterialHandler(&FKismetAIModule::HandleMaterialOverview, EngineFunctionPath);
	TestTrue(TEXT("response parsed"), Env.IsValid());
	if (!Env.IsValid()) return false;

	TestTrue(TEXT("success=true"), Env->GetBoolField(TEXT("success")));
	const TSharedPtr<FJsonObject>* Data;
	if (Env->TryGetObjectField(TEXT("data"), Data) && Data)
	{
		TestEqual(TEXT("type is material_function"), (*Data)->GetStringField(TEXT("type")), FString(TEXT("material_function")));
		TestTrue(TEXT("reports expression_count"), (*Data)->HasField(TEXT("expression_count")));
		TestFalse(TEXT("overview omits the expressions array"), (*Data)->HasField(TEXT("expressions")));
	}
	return true;
}
