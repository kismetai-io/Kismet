// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Fakes/ScriptedAgentSeams.h"
#include "KismetAISessionStorage.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionStart_ExistingJournalRefused,
	"KismetAI.SessionStart.ExistingJournalRefused", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSessionStart_ExistingJournalRefused::RunTest(const FString& Parameters)
{
	using namespace KismetAITestFakes;
	for (const FString& Original : { FString(TEXT("{\"type\":\"session_start\",\"v\":1,\"model\":\"original\"}\n")), FString(TEXT("{corrupt journal bytes\n")), FString() })
	{
		const FString SessionId = FKismetAISessionStorage::CreateSessionId();
		const FString Path = FKismetAISessionStorage::GetSessionFilePath(SessionId);
		ON_SCOPE_EXIT { FKismetAISessionStorage::DeleteSession(SessionId); };
		if (!TestTrue(TEXT("existing journal fixture saved"), FFileHelper::SaveStringToFile(Original, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
		FScriptedModelClient Model({ MakeTextTurn(TEXT("must not execute")) });
		FScriptedToolInvoker Tools;
		FKismetAIAgentConfig Config;
		Config.ApiKey = TEXT("test-key-unused");
		Config.Model = TEXT("claude-sonnet-4-6");
		Config.ThinkingMode = EThinkingMode::Disabled;
		FKismetAIAgent Agent(Tools, Model, Config);
		FString Failure;
		Agent.RunTask(TEXT("Start with an already allocated ID"), FOnAgentStep::CreateLambda([&Failure](const FString& Type, const FString& Json)
		{
			if (Type == TEXT("error")) Failure = Json;
		}), TEXT(""), SessionId, /*bStartNewSession=*/true);
		TestEqual(TEXT("existing journal never reaches model"), Model.NumCalls(), 0);
		TestTrue(TEXT("collision refusal is explicit"), Failure.Contains(TEXT("session_journal_already_exists")));
		FString After;
		TestTrue(TEXT("original journal still readable as bytes"), FFileHelper::LoadFileToString(After, *Path));
		TestEqual(TEXT("valid, corrupt and empty journals remain unchanged"), After, Original);
	}
	return true;
}
