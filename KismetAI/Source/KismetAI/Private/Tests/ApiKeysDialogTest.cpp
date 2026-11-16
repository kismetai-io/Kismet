// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "SKismetAISettingsDialog.h"
#include "KismetAIAgent.h"
#include "Misc/AutomationTest.h"

// The API keys window used to list three hand-written providers, so Gemini,
// DeepSeek, GLM and Kimi were in the model menu with no way to enter their
// key. Rows now come from providers.json; these tests keep the two in step.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApiKeysDialog_CoversEveryPickerProvider,
	"KismetAI.Picker.ApiKeys.CoversEveryPickerProvider",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FApiKeysDialog_CoversEveryPickerProvider::RunTest(const FString&)
{
	const TArray<FString> KeyIds = SKismetAISettingsDialog::GetKeyProviderIds();
	for (const FKismetAIAgent::FModelAliasInfo& Model : FKismetAIAgent::GetAvailableModels())
	{
		TestTrue(FString::Printf(TEXT("%s: provider '%s' has an API key row"), *Model.ModelId, *Model.Provider),
			KeyIds.Contains(Model.Provider));
	}
	TestTrue(TEXT("OpenRouter (used via /provider) has a row"), KeyIds.Contains(TEXT("openrouter")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApiKeysDialog_OrderAndExclusions,
	"KismetAI.Picker.ApiKeys.OrderMatchesMenuAndSkipsKeylessProfiles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FApiKeysDialog_OrderAndExclusions::RunTest(const FString&)
{
	const TArray<FString> KeyIds = SKismetAISettingsDialog::GetKeyProviderIds();

	// Rows follow the model menu's section order.
	TArray<FString> MenuOrder;
	for (const FKismetAIAgent::FModelAliasInfo& Model : FKismetAIAgent::GetAvailableModels())
	{
		MenuOrder.AddUnique(Model.Provider);
	}
	int32 Previous = -1;
	for (const FString& Provider : MenuOrder)
	{
		const int32 Index = KeyIds.IndexOfByKey(Provider);
		TestTrue(FString::Printf(TEXT("%s row comes after the previous menu section"), *Provider), Index > Previous);
		Previous = Index;
	}

	for (const TCHAR* Keyless : { TEXT("ollama"), TEXT("lmstudio") })
	{
		TestFalse(FString::Printf(TEXT("%s takes no user key and has no row"), Keyless), KeyIds.Contains(Keyless));
	}
	return true;
}
