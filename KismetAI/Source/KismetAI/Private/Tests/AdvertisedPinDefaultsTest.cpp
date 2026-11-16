// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIEmbeddedConfig.h"
#include "Misc/AutomationTest.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAdvertisedPinDefaultsTest, "KismetAI.ToolManifest.AdvertisedPinDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAdvertisedPinDefaultsTest::RunTest(const FString& Parameters)
{
	const FString ModuleDir = FPaths::GetPath(FModuleManager::Get().GetModuleFilename(TEXT("KismetAI")));
	const FString ConfigDir = FPaths::GetPath(FPaths::GetPath(ModuleDir)) / TEXT("Config");
	FString Prompt;
	if (!TestTrue(TEXT("load shipped prompt"), KismetAIEmbeddedConfig::LoadConfigFile(
		TEXT("system_prompt.txt"), ConfigDir / TEXT("system_prompt.txt"), Prompt))) return false;

	auto Example = [&](const TCHAR* Type)
	{
		const FString Prefix = FString(TEXT("- ")) + Type + TEXT(": \"");
		const int32 Start = Prompt.Find(Prefix);
		if (!TestTrue(FString::Printf(TEXT("prompt documents %s"), Type), Start != INDEX_NONE)) return FString();
		const int32 ValueStart = Start + Prefix.Len();
		const int32 End = Prompt.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, ValueStart);
		if (!TestTrue(TEXT("example has closing quote"), End != INDEX_NONE)) return FString();
		return Prompt.Mid(ValueStart, End - ValueStart);
	};

	// These are the same validators used by UEdGraphSchema_K2 for struct pins.
	// Test the shipped examples, not a second hardcoded copy of the guidance.
	const FString VectorExample = Example(TEXT("vector"));
	const FString RotatorExample = Example(TEXT("rotator"));
	const FString ColorExample = Example(TEXT("color"));
	TestTrue(TEXT("advertised vector passes K2 default validation"), FDefaultValueHelper::IsStringValidVector(VectorExample));
	TestTrue(TEXT("advertised rotator passes K2 default validation"), FDefaultValueHelper::IsStringValidRotator(RotatorExample));
	FVector Vector = FVector::ZeroVector;
	TestTrue(TEXT("vector parses"), FDefaultValueHelper::ParseVector(VectorExample, Vector));
	TestTrue(TEXT("vector example preserves all components"), Vector.Equals(FVector(1, 2, 3)));
	FRotator Rotator = FRotator::ZeroRotator;
	TestTrue(TEXT("rotator parses"), FDefaultValueHelper::ParseRotator(RotatorExample, Rotator));
	TestTrue(TEXT("rotator example preserves yaw"), Rotator.Equals(FRotator(0, 90, 0)));
	FLinearColor Color(0, 0, 0, 0);
	TestTrue(TEXT("advertised color passes K2 default validation"), Color.InitFromString(ColorExample));
	TestTrue(TEXT("color example preserves channels"), Color.Equals(FLinearColor(1, .5f, 0, 1)));
	return true;
}
