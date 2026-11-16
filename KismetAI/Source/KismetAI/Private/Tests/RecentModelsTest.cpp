// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIBridge.h"
#include "KismetAIModule.h"
#include "KismetAIPaths.h"
#include "ScopedTempHome.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// The model picker shows the last few picked models in a "Recent" section.
// The list comes from UKismetAIBridge::SetModel, so it must stay most recent
// first, without duplicates, capped, and survive a restart via settings.json.

namespace
{
	TArray<FString> ParseIds(const FString& Json)
	{
		TArray<FString> Ids;
		TArray<TSharedPtr<FJsonValue>> Values;
		if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Values))
		{
			for (const TSharedPtr<FJsonValue>& Value : Values)
			{
				Ids.Add(Value->AsString());
			}
		}
		return Ids;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecentModelsMostRecentFirst, "KismetAI.Picker.RecentModels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRecentModelsMostRecentFirst::RunTest(const FString&)
{
	FScopedTempHome Home(TEXT("RecentModels"));
	FKismetAIModule& Module = FKismetAIModule::Get();
	const FKismetAIAgentConfig Original = Module.GetAgentConfig();
	ON_SCOPE_EXIT { Module.SetAgentConfig(Original); };

	FKismetAIAgentConfig Config = Original;
	Config.RecentModels.Reset();
	Module.SetAgentConfig(Config);

	UKismetAIBridge* Bridge = NewObject<UKismetAIBridge>();
	TestEqual(TEXT("empty to start"), ParseIds(Bridge->GetRecentModels()).Num(), 0);

	Bridge->SetModel(TEXT("opus"));
	Bridge->SetModel(TEXT("sonnet"));
	Bridge->SetModel(TEXT("opus"));
	TArray<FString> Ids = ParseIds(Bridge->GetRecentModels());
	TestEqual(TEXT("re-picking does not duplicate"), Ids.Num(), 2);
	TestEqual(TEXT("most recent first"), Ids.Num() > 0 ? Ids[0] : FString(), FString(TEXT("claude-opus-5-5")));

	Bridge->SetModel(TEXT("haiku"));
	Bridge->SetModel(TEXT("astra"));
	Ids = ParseIds(Bridge->GetRecentModels());
	TestEqual(TEXT("capped"), Ids.Num(), FKismetAIAgentConfig::MaxRecentModels);
	TestFalse(TEXT("oldest pick dropped"), Ids.Contains(TEXT("claude-sonnet-5-5")));
	TestEqual(TEXT("newest at the top"), Ids.Num() > 0 ? Ids[0] : FString(), FString(TEXT("gpt-6-astra")));

	TestEqual(TEXT("unknown alias leaves the list alone"), Bridge->SetModel(TEXT("")), FString());
	TestEqual(TEXT("still capped"), ParseIds(Bridge->GetRecentModels()).Num(), FKismetAIAgentConfig::MaxRecentModels);

	FString Json;
	TSharedPtr<FJsonObject> Saved;
	const FString SettingsPath = KismetAIPaths::GetConfigDir() / TEXT("settings.json");
	if (TestTrue(TEXT("settings.json written"), FFileHelper::LoadFileToString(Json, *SettingsPath))
		&& TestTrue(TEXT("settings.json parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Saved) && Saved.IsValid()))
	{
		const TArray<TSharedPtr<FJsonValue>>* SavedIds = nullptr;
		if (TestTrue(TEXT("recent_models persisted"), Saved->TryGetArrayField(TEXT("recent_models"), SavedIds)))
		{
			TestEqual(TEXT("persisted list matches"), SavedIds->Num(), FKismetAIAgentConfig::MaxRecentModels);
		}
	}
	return true;
}
