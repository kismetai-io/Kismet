// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIAgent.h"
#include "KismetAIModuleToolInvoker.h"
#include "KismetAIModule.h"
#include "Misc/AutomationTest.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

constexpr auto KismetAIAgentTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// Live agent smoke test — explicitly opt in; routine automation never spends API quota.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentRunTask_SmokeTest, "KismetAI.Agent.RunTask.SmokeTest", KismetAIAgentTestFlags)

bool FAgentRunTask_SmokeTest::RunTest(const FString& Parameters)
{
	if (FPlatformMisc::GetEnvironmentVariable(TEXT("KISMET_RUN_LIVE_AGENT_TEST")) != TEXT("1"))
	{
		AddInfo(TEXT("Skipping live agent test — set KISMET_RUN_LIVE_AGENT_TEST=1 to opt in"));
		return true;
	}

	// Skip if no API key
	FString ApiKey = FPlatformMisc::GetEnvironmentVariable(TEXT("ANTHROPIC_API_KEY"));
	if (ApiKey.IsEmpty())
	{
		AddInfo(TEXT("Skipping agent test — ANTHROPIC_API_KEY not set"));
		return true;
	}

	// Create a transient Blueprint with Event BeginPlay
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_AgentTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());

	TestNotNull(TEXT("Transient Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	// Get asset path for the tool
	FString AssetPath = Blueprint->GetPathName();

	// Configure agent
	FKismetAIAgentConfig Config;
	Config.ApiKey = ApiKey;
	const FString BaseUrl = FPlatformMisc::GetEnvironmentVariable(TEXT("ANTHROPIC_BASE_URL"));
	if (!BaseUrl.IsEmpty()) Config.ApiBaseUrl = BaseUrl;
	Config.MaxSteps = 5;
	Config.MaxTokens = 1024;

	// Track events
	bool bGotToolCall = false;
	bool bGotDone = false;

	FOnAgentStep StepCallback;
	StepCallback.BindLambda([&](const FString& EventType, const FString& EventJson)
	{
		if (EventType == TEXT("tool_call"))
		{
			bGotToolCall = true;
		}
		if (EventType == TEXT("done"))
		{
			bGotDone = true;
		}
		AddInfo(FString::Printf(TEXT("[%s] %s"), *EventType, *EventJson));
	});

	// Run agent with a simple task
	FKismetAIAgent Agent(MakeUnique<FKismetAIModuleToolInvoker>(FKismetAIModule::Get()), Config);
	FString Result = Agent.RunTask(
		FString::Printf(TEXT("Describe this blueprint: %s"), *AssetPath),
		StepCallback);

	// Verify
	TestTrue(TEXT("Agent returned non-empty response"), !Result.IsEmpty());
	TestTrue(TEXT("At least one tool_call event fired"), bGotToolCall);
	TestTrue(TEXT("Done event fired"), bGotDone);

	return true;
}
