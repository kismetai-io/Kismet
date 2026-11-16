// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
#include "KismetAIModule.h"
#include "KismetAISourceConfinement.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceOutputRange, "KismetAI.ReadSource.Output.ActualRange", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceOutputRange::RunTest(const FString& Parameters)
{
	const FString Base = FString(FPlatformProcess::UserTempDir()) / (TEXT("KismetP206Output-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Base, false, true); };
	FKismetAISourceRoots Roots;
	Roots.ProjectDir = Base; Roots.EngineDir = Base / TEXT("Engine"); Roots.bIncludeDiscoveredPlugins = false;
	KismetAISourceConfinement::FScopedRootsOverride Override(Roots);
	const FString File = Base / TEXT("Source/Rows.h");
	FString Contents;
	for (int32 Index = 0; Index < 300; ++Index) Contents += TEXT("// xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n");
	if (!TestTrue(TEXT("Fixture created"), FFileHelper::SaveStringToFile(Contents, *File))) return false;
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("mode"), TEXT("raw")); Args->SetStringField(TEXT("scope"), TEXT("project"));
	Args->SetStringField(TEXT("file"), File); Args->SetNumberField(TEXT("limit"), 200);
	const auto Result = FKismetAIModule::Get().DoReadSource(Args);
	if (!TestTrue(TEXT("Bounded raw read succeeds"), Result.bSuccess && Result.Data.IsValid())) return false;
	const int32 End = static_cast<int32>(Result.Data->GetNumberField(TEXT("next_offset")));
	TestTrue(TEXT("Output budget stops before requested end"), End > 0 && End < 200);
	TestTrue(TEXT("Header states actual returned end"), Result.Data->GetStringField(TEXT("result")).Contains(FString::Printf(TEXT("showing lines 1-%d\n"), End)));
	TestEqual(TEXT("Exact serialized envelope metric"), Result.Data->GetObjectField(TEXT("metrics"))->GetNumberField(TEXT("result_bytes")), static_cast<double>(FTCHARToUTF8(*Result.ToEnvelopeString()).Length()));
	return true;
}
