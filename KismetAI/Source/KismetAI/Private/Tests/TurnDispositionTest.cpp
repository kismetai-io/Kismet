// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

// Tests for FKismetAIAgent::DecideTurnAction — the loop's per-turn disposition.
// Guards the "agent stops after reasoning" fix: a max_tokens truncation with no
// tool call must continue (up to a cap), not be treated as completion. Applies to
// both providers (Anthropic + OpenAI normalize stop_reason to the same vocabulary).

#include "Misc/AutomationTest.h"
#include "KismetAIAgent.h"

constexpr auto TurnDispositionTestFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

using ETurnAction = FKismetAIAgent::ETurnAction;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTurnDispositionToolUseTest,
	"KismetAI.TurnDisposition.ToolUseExecutes", TurnDispositionTestFlags)
bool FTurnDispositionToolUseTest::RunTest(const FString&)
{
	TestEqual(TEXT("tool_use + tools -> ExecuteTools"),
		FKismetAIAgent::DecideTurnAction(TEXT("tool_use"), 1, 0, 3), ETurnAction::ExecuteTools);
	TestEqual(TEXT("tool_use + no tools -> Done"),
		FKismetAIAgent::DecideTurnAction(TEXT("tool_use"), 0, 0, 3), ETurnAction::Done);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTurnDispositionEndTurnTest,
	"KismetAI.TurnDisposition.EndTurnDone", TurnDispositionTestFlags)
bool FTurnDispositionEndTurnTest::RunTest(const FString&)
{
	TestEqual(TEXT("end_turn + no tools -> Done"),
		FKismetAIAgent::DecideTurnAction(TEXT("end_turn"), 0, 0, 3), ETurnAction::Done);
	// end_turn with tools is anomalous (providers set tool_use when tools exist);
	// preserve the original conservative behavior of NOT executing on end_turn.
	TestEqual(TEXT("end_turn + tools -> Done"),
		FKismetAIAgent::DecideTurnAction(TEXT("end_turn"), 2, 0, 3), ETurnAction::Done);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTurnDispositionMaxTokensTest,
	"KismetAI.TurnDisposition.MaxTokensContinues", TurnDispositionTestFlags)
bool FTurnDispositionMaxTokensTest::RunTest(const FString&)
{
	// The core fix: truncation with no tool call continues while under the cap.
	TestEqual(TEXT("max_tokens + no tools + under cap -> ContinueTruncated"),
		FKismetAIAgent::DecideTurnAction(TEXT("max_tokens"), 0, 0, 3), ETurnAction::ContinueTruncated);
	TestEqual(TEXT("max_tokens + no tools + just under cap -> ContinueTruncated"),
		FKismetAIAgent::DecideTurnAction(TEXT("max_tokens"), 0, 2, 3), ETurnAction::ContinueTruncated);
	// At/over the cap, give up gracefully rather than loop forever.
	TestEqual(TEXT("max_tokens + no tools + at cap -> Done"),
		FKismetAIAgent::DecideTurnAction(TEXT("max_tokens"), 0, 3, 3), ETurnAction::Done);
	TestEqual(TEXT("max_tokens + no tools + over cap -> Done"),
		FKismetAIAgent::DecideTurnAction(TEXT("max_tokens"), 0, 5, 3), ETurnAction::Done);
	// Truncated but tool calls did parse — act on them.
	TestEqual(TEXT("max_tokens + tools -> ExecuteTools"),
		FKismetAIAgent::DecideTurnAction(TEXT("max_tokens"), 1, 0, 3), ETurnAction::ExecuteTools);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTurnDispositionUnknownTest,
	"KismetAI.TurnDisposition.UnknownReasonDone", TurnDispositionTestFlags)
bool FTurnDispositionUnknownTest::RunTest(const FString&)
{
	// pause_turn / refusal / empty / anything else with no tools -> Done.
	TestEqual(TEXT("pause_turn + no tools -> Done"),
		FKismetAIAgent::DecideTurnAction(TEXT("pause_turn"), 0, 0, 3), ETurnAction::Done);
	TestEqual(TEXT("empty + no tools -> Done"),
		FKismetAIAgent::DecideTurnAction(TEXT(""), 0, 0, 3), ETurnAction::Done);
	return true;
}
