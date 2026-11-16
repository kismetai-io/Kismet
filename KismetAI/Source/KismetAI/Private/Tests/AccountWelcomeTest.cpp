// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIBridge.h"
#include "KismetAIModule.h"
#include "KismetAIPaths.h"
#include "ScopedTempHome.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// The first-run account welcome recommends signing in once. Answering it
// (signing in or "Continue without an account") must persist, or the panel
// would ask again on every launch — the opposite of optional.

namespace
{
	bool ReadSavedWelcomeFlag(FAutomationTestBase& Test, bool& bOutSeen)
	{
		const FString SettingsPath = KismetAIPaths::GetConfigDir() / TEXT("settings.json");
		FString Json;
		if (!Test.TestTrue(TEXT("settings.json written"), FFileHelper::LoadFileToString(Json, *SettingsPath)))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Saved;
		if (!Test.TestTrue(TEXT("settings.json parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Saved) && Saved.IsValid()))
		{
			return false;
		}
		return Test.TestTrue(TEXT("account_welcome_seen present"), Saved->TryGetBoolField(TEXT("account_welcome_seen"), bOutSeen));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAccountWelcomePersists, "KismetAI.Account.WelcomeAnswerPersists",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAccountWelcomePersists::RunTest(const FString&)
{
	FScopedTempHome Home(TEXT("AccountWelcome"));
	FKismetAIModule& Module = FKismetAIModule::Get();
	const FKismetAIAgentConfig Original = Module.GetAgentConfig();
	ON_SCOPE_EXIT { Module.SetAgentConfig(Original); };

	FKismetAIAgentConfig Config = Original;
	Config.bAccountWelcomeSeen = false;
	Module.SetAgentConfig(Config);
	Module.SaveSettings();

	UKismetAIBridge* Bridge = NewObject<UKismetAIBridge>();
	TestFalse(TEXT("welcome not yet answered"), Bridge->IsAccountWelcomeSeen());

	bool bSeen = true;
	if (ReadSavedWelcomeFlag(*this, bSeen))
	{
		TestFalse(TEXT("default saved as unanswered"), bSeen);
	}

	Bridge->MarkAccountWelcomeSeen();
	TestTrue(TEXT("answer held in memory"), Bridge->IsAccountWelcomeSeen());
	if (ReadSavedWelcomeFlag(*this, bSeen))
	{
		TestTrue(TEXT("answer persisted to settings.json"), bSeen);
	}
	return true;
}
