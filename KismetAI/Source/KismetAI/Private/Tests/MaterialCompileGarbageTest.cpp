// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Compile_material on a material that is marked as garbage but not yet
// collected. A reset or delete leaves such an object resolvable by path, and
// UMaterialEditingLibrary::RecompileMaterial on it crashed the editor.
// The finder must treat it as not found and the tool must return a typed
// error instead of recompiling.

#include "Misc/AutomationTest.h"
#include "KismetAIModule.h"
#include "KismetAIUtils.h"
#include "KismetAIMutationOutcome.h"
#include "Dom/JsonObject.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Materials/Material.h"

constexpr auto MatCompileGarbageFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialCompile_GarbageIsNotFound, "KismetAI.Materials.Compile_GarbageIsNotFound", MatCompileGarbageFlags)
bool FMaterialCompile_GarbageIsNotFound::RunTest(const FString& Parameters)
{
	UPackage* Pkg = CreatePackage(TEXT("/Temp/KismetMatCompileGarbageTest"));
	// Held strongly so the object stays resolvable after MarkAsGarbage, as it does between a
	// reset and the next garbage collection.
	TStrongObjectPtr<UMaterial> Mat(NewObject<UMaterial>(Pkg, TEXT("M_CompileGarbage"), RF_Transactional));
	const FString Path = Mat->GetPathName();

	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("material"), Path);

	// Control: the live material is found and compiles.
	TestTrue(TEXT("live material is found"), KismetAIUtils::FindMaterialInterface(Path) == Mat.Get());
	const FToolExecutionResult Live = FKismetAIModule::Get().DoCompileMaterial(Body);

	TestTrue(TEXT("compile transport succeeds"), Live.bSuccess);
	FKismetAIMutationOutcome CompileEvidence;
	TestTrue(TEXT("material compile has typed evidence"), FKismetAIMutationOutcome::TryExtractFromToolResult(Live.ToModelString(), CompileEvidence));
	TestEqual(TEXT("compile success requires verified shaders"), Live.bOperationsSucceeded, CompileEvidence.CompileStatus == EKismetAICompileStatus::Succeeded);
	TestEqual(TEXT("material identity in evidence"), CompileEvidence.PackageName, Pkg->GetName());
	TestFalse(TEXT("compile does not claim a mutation or save"), CompileEvidence.bAppliedInMemory || CompileEvidence.SaveStatus == EKismetAISaveStatus::Saved);


	Mat->MarkAsGarbage();

	TestTrue(TEXT("garbage material is not found"), KismetAIUtils::FindMaterialInterface(Path) == nullptr);
	TestTrue(TEXT("garbage material is not found as a base material"), KismetAIUtils::FindMaterial(Path) == nullptr);

	const FToolExecutionResult Gone = FKismetAIModule::Get().DoCompileMaterial(Body);
	TestFalse(TEXT("compile of a garbage material fails"), Gone.bSuccess);
	TestEqual(TEXT("typed error code"), Gone.ErrorCode, FString(TEXT("MATERIAL_NOT_FOUND")));
	TestEqual(TEXT("status code"), Gone.StatusCode, 404);

	Mat.Reset();
	return true;
}
