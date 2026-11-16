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
#include "Materials/MaterialExpression.h"
#include "MaterialEditingLibrary.h"
#include "SceneTypes.h"

constexpr auto MatEditFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	static TSharedPtr<FJsonObject> RunHandler(
		bool (FKismetAIModule::*Handler)(const FHttpServerRequest&, const FHttpResultCallback&),
		TSharedPtr<FJsonObject> Body)
	{
		FString Resp = FKismetAIModule::Get().ExecuteHandlerDirect(Handler, Body);
		Resp.RemoveFromStart(TEXT("<tool_use_error>"));
		Resp.RemoveFromEnd(TEXT("</tool_use_error>"));
		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp);
		FJsonSerializer::Deserialize(Reader, Parsed);
		return Parsed;
	}

	static FString DataString(const TSharedPtr<FJsonObject>& Env, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Data;
		if (Env.IsValid() && Env->TryGetObjectField(TEXT("data"), Data))
		{
			return (*Data)->GetStringField(Field);
		}
		return FString();
	}
}

// Build a small material in memory, add two nodes, set a value, wire them, and feed BaseColor.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialEdit_CreateConnect, "KismetAI.Materials.Edit_CreateAndConnect", MatEditFlags)
bool FMaterialEdit_CreateConnect::RunTest(const FString& Parameters)
{
	UPackage* Pkg = CreatePackage(TEXT("/Temp/KismetMatEditTest"));
	if (!Pkg) { AddError(TEXT("could not create package")); return false; }
	UMaterial* Mat = NewObject<UMaterial>(Pkg, TEXT("M_KismetEditTest"), RF_Standalone | RF_Transactional);
	if (!Mat) { AddError(TEXT("could not create material")); return false; }
	const FString Path = Mat->GetPathName();

	// create_material_expression: a Constant3Vector and a Multiply.
	auto MakeNode = [&](const FString& NodeType) -> FString
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("material"), Path);
		Body->SetStringField(TEXT("node"), NodeType);
		TSharedPtr<FJsonObject> Env = RunHandler(&FKismetAIModule::HandleCreateMaterialExpression, Body);
		TestTrue(FString::Printf(TEXT("create %s success"), *NodeType), Env.IsValid() && Env->GetBoolField(TEXT("success")));
		return DataString(Env, TEXT("id"));
	};

	const FString ColorId = MakeNode(TEXT("Constant3Vector"));
	const FString MulId = MakeNode(TEXT("Multiply"));
	TestTrue(TEXT("color id non-empty"), !ColorId.IsEmpty());
	TestTrue(TEXT("multiply id non-empty"), !MulId.IsEmpty());
	if (ColorId.IsEmpty() || MulId.IsEmpty()) return false;

	// edit_material: set the color, wire color->Multiply.A, wire Multiply->BaseColor.
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("material"), Path);

		TArray<TSharedPtr<FJsonValue>> Ops;

		// set_property Constant3Vector.Constant = [1,0,0]
		{
			TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
			Op->SetStringField(TEXT("action"), TEXT("set_property"));
			Op->SetStringField(TEXT("node"), ColorId);
			Op->SetStringField(TEXT("property"), TEXT("Constant"));
			TArray<TSharedPtr<FJsonValue>> Color = {
				MakeShared<FJsonValueNumber>(1.0), MakeShared<FJsonValueNumber>(0.0), MakeShared<FJsonValueNumber>(0.0) };
			Op->SetArrayField(TEXT("value"), Color);
			Ops.Add(MakeShared<FJsonValueObject>(Op));
		}
		// connect color -> Multiply input "A"
		{
			TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
			Op->SetStringField(TEXT("action"), TEXT("connect"));
			Op->SetStringField(TEXT("from_node"), ColorId);
			Op->SetStringField(TEXT("to_node"), MulId);
			Op->SetStringField(TEXT("to_input"), TEXT("A"));
			Ops.Add(MakeShared<FJsonValueObject>(Op));
		}
		// connect Multiply -> BaseColor
		{
			TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
			Op->SetStringField(TEXT("action"), TEXT("connect"));
			Op->SetStringField(TEXT("from_node"), MulId);
			Op->SetStringField(TEXT("to_property"), TEXT("BaseColor"));
			Ops.Add(MakeShared<FJsonValueObject>(Op));
		}

		Body->SetArrayField(TEXT("operations"), Ops);
		TSharedPtr<FJsonObject> Env = RunHandler(&FKismetAIModule::HandleEditMaterial, Body);
		TestTrue(TEXT("edit success envelope"), Env.IsValid() && Env->GetBoolField(TEXT("success")));

		const TSharedPtr<FJsonObject>* Data;
		if (Env.IsValid() && Env->TryGetObjectField(TEXT("data"), Data))
		{
			int32 Failures = 0;
			(*Data)->TryGetNumberField(TEXT("failures"), Failures);
			TestEqual(TEXT("no failed operations"), Failures, 0);
		}
	}

	// Verify the graph: BaseColor is now fed by the Multiply node.
	UMaterialExpression* BaseColorNode = UMaterialEditingLibrary::GetMaterialPropertyInputNode(Mat, MP_BaseColor);
	TestTrue(TEXT("BaseColor has an input node"), BaseColorNode != nullptr);
	if (BaseColorNode)
	{
		TestEqual(TEXT("BaseColor fed by the Multiply"), BaseColorNode->GetName(), MulId);
	}

	return true;
}

// delete_node removes an expression from the material.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialEdit_Delete, "KismetAI.Materials.Edit_DeleteNode", MatEditFlags)
bool FMaterialEdit_Delete::RunTest(const FString& Parameters)
{
	UPackage* Pkg = CreatePackage(TEXT("/Temp/KismetMatDeleteTest"));
	UMaterial* Mat = NewObject<UMaterial>(Pkg, TEXT("M_KismetDeleteTest"), RF_Standalone | RF_Transactional);
	const FString Path = Mat->GetPathName();

	TSharedPtr<FJsonObject> CreateBody = MakeShared<FJsonObject>();
	CreateBody->SetStringField(TEXT("material"), Path);
	CreateBody->SetStringField(TEXT("node"), TEXT("ScalarParameter"));
	TSharedPtr<FJsonObject> CreateEnv = RunHandler(&FKismetAIModule::HandleCreateMaterialExpression, CreateBody);
	const FString NodeId = DataString(CreateEnv, TEXT("id"));
	TestEqual(TEXT("one expression before delete"), UMaterialEditingLibrary::GetNumMaterialExpressions(Mat), 1);

	TSharedPtr<FJsonObject> EditBody = MakeShared<FJsonObject>();
	EditBody->SetStringField(TEXT("material"), Path);
	TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("delete_node"));
	Op->SetStringField(TEXT("node"), NodeId);
	EditBody->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(Op) });
	RunHandler(&FKismetAIModule::HandleEditMaterial, EditBody);

	TestEqual(TEXT("zero expressions after delete"), UMaterialEditingLibrary::GetNumMaterialExpressions(Mat), 0);
	return true;
}
