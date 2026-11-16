// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIAgent.h"
#include "Fakes/ScriptedAgentSeams.h"
#include "Misc/AutomationTest.h"

constexpr auto AskUserTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// SubmitAnswer must reject answers when no question is outstanding — this is the
// guard that prevents a late/duplicate answer from landing on a question that was
// never asked (or already resolved). HandleAskUserTool is what arms a pending
// question and it blocks the worker thread, so the public reject path is what we
// can exercise deterministically without a live run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAskUser_SubmitAnswerRejectsWhenNoPending,
	"KismetAI.AskUser.SubmitAnswerRejectsWhenNoPending",
	AskUserTestFlags)

bool FAskUser_SubmitAnswerRejectsWhenNoPending::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Config;
	Config.Model = TEXT("claude-sonnet-4-6");
	FKismetAIAgent Agent(MakeUnique<KismetAITestFakes::FScriptedToolInvoker>(), Config);

	// Nothing pending: any answer is stale and must be rejected.
	TestFalse(TEXT("answer rejected when no question is pending"),
		Agent.SubmitAnswer(TEXT("some-question-id"), TEXT("{\"q\":\"a\"}")));

	// An empty question id must not match the (empty) pending state either —
	// otherwise a malformed request could spuriously satisfy a real question.
	TestFalse(TEXT("empty question id is rejected, not treated as a match"),
		Agent.SubmitAnswer(TEXT(""), TEXT("{}")));

	return true;
}
