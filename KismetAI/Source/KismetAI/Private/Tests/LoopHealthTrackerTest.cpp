// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Direct unit tests for the convergence guards.
//
// KismetAI.AgentLoop already covers these end to end through RunTask. These
// cover the state machine itself — the edge cases that are awkward to reach
// through a whole run, and that the rules were originally written for.

#include "KismetAILoopHealth.h"
#include "Misc/AutomationTest.h"

constexpr auto LoopHealthFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	const TCHAR* const ReadTool  = TEXT("read_blueprint");
	const TCHAR* const EditTool  = TEXT("edit_blueprint");
	const TCHAR* const OtherEdit = TEXT("insert_node");
	const TCHAR* const ArgsA     = TEXT("{\"blueprint\":\"/Game/A\"}");
	const TCHAR* const ArgsB     = TEXT("{\"blueprint\":\"/Game/B\"}");
}

// R3: identical read repeats are refused; different arguments are not.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoopHealth_R3_DuplicateReads, "KismetAI.LoopHealthTracker.R3_DuplicateReads", LoopHealthFlags)
bool FLoopHealth_R3_DuplicateReads::RunTest(const FString& Parameters)
{
	FKismetAILoopHealth H;
	TestTrue(TEXT("first read runs"),
		H.DecideDisposition(ReadTool, ArgsA) == EToolCallDisposition::Execute);
	H.OnToolResult(ReadTool, ArgsA, false, false);
	TestTrue(TEXT("identical repeat is refused"),
		H.DecideDisposition(ReadTool, ArgsA) == EToolCallDisposition::SkipDuplicateRead);
	TestTrue(TEXT("different arguments still run"),
		H.DecideDisposition(ReadTool, ArgsB) == EToolCallDisposition::Execute);
	return true;
}

// R3b: a failed edit blocks its identical resend, and only its identical resend.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoopHealth_R3b_FailedEditBlocksResend, "KismetAI.LoopHealthTracker.R3b_FailedEditBlocksResend", LoopHealthFlags)
bool FLoopHealth_R3b_FailedEditBlocksResend::RunTest(const FString& Parameters)
{
	FKismetAILoopHealth H;

	TestTrue(TEXT("edit runs the first time"),
		H.DecideDisposition(EditTool, ArgsA) == EToolCallDisposition::Execute);
	H.OnToolResult(EditTool, ArgsA, /*bShortCircuited=*/false, /*bErrored=*/true);

	TestTrue(TEXT("identical resend is refused"),
		H.DecideDisposition(EditTool, ArgsA) == EToolCallDisposition::SkipRepeatedFailedEdit);
	TestTrue(TEXT("a different edit is unaffected"),
		H.DecideDisposition(EditTool, ArgsB) == EToolCallDisposition::Execute);
	return true;
}

// A successful operation on another resource cannot clear this failure.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoopHealth_R3b_SuccessClearsGuard, "KismetAI.LoopHealthTracker.R3b_SuccessClearsGuard", LoopHealthFlags)
bool FLoopHealth_R3b_SuccessClearsGuard::RunTest(const FString& Parameters)
{
	FKismetAILoopHealth H;

	H.DecideDisposition(EditTool, ArgsA);
	H.OnToolResult(EditTool, ArgsA, false, /*bErrored=*/true);
	TestTrue(TEXT("blocked while nothing has changed"),
		H.DecideDisposition(EditTool, ArgsA) == EToolCallDisposition::SkipRepeatedFailedEdit);

	// A different mutation succeeds.
	H.DecideDisposition(OtherEdit, ArgsB);
	H.OnToolResult(OtherEdit, ArgsB, false, /*bErrored=*/false);

	TestTrue(TEXT("unrelated resource success cannot reset the failure"),
		H.DecideDisposition(EditTool, ArgsA) == EToolCallDisposition::SkipRepeatedFailedEdit);
	return true;
}

// A short-circuited call did not run, so it must not be recorded as a failure.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoopHealth_ShortCircuitIsNotAFailure, "KismetAI.LoopHealthTracker.ShortCircuitIsNotAFailure", LoopHealthFlags)
bool FLoopHealth_ShortCircuitIsNotAFailure::RunTest(const FString& Parameters)
{
	FKismetAILoopHealth H;
	H.DecideDisposition(EditTool, ArgsA);
	H.OnToolResult(EditTool, ArgsA, /*bShortCircuited=*/true, /*bErrored=*/true);

	// The guard should be empty, so the edit is still allowed to run once.
	TestTrue(TEXT("a skipped call leaves no failure record"),
		H.DecideDisposition(EditTool, ArgsA) == EToolCallDisposition::Execute);
	return true;
}

// R4: barren turns accumulate; a successful mutation resets the streak.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoopHealth_R4_BarrenStreak, "KismetAI.LoopHealthTracker.R4_BarrenStreak", LoopHealthFlags)
bool FLoopHealth_R4_BarrenStreak::RunTest(const FString& Parameters)
{
	FKismetAILoopHealth H;

	for (int32 i = 0; i < 3; ++i)
	{
		H.DecideDisposition(ReadTool, *FString::Printf(TEXT("{\"blueprint\":\"/Game/%d\"}"), i));
		H.OnTurnEnd(/*NumToolCalls=*/1);
	}
	TestEqual(TEXT("three barren turns counted"), H.GetBarrenTurnStreak(), 3);
	TestTrue(TEXT("NO PROGRESS note fires"),
		H.BuildOrchestratorNote(/*TurnsRemaining=*/20).Contains(TEXT("NO PROGRESS")));

	// A successful mutation resets it.
	H.DecideDisposition(EditTool, ArgsA);
	H.OnToolResult(EditTool, ArgsA, false, /*bErrored=*/false);
	H.OnTurnEnd(1);
	TestEqual(TEXT("streak reset by a real change"), H.GetBarrenTurnStreak(), 0);
	TestFalse(TEXT("note goes quiet"),
		H.BuildOrchestratorNote(20).Contains(TEXT("NO PROGRESS")));
	return true;
}

// A turn with no tool calls is not investigation and must not extend the streak.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoopHealth_R4_NoToolsIsNotBarren, "KismetAI.LoopHealthTracker.R4_NoToolsIsNotBarren", LoopHealthFlags)
bool FLoopHealth_R4_NoToolsIsNotBarren::RunTest(const FString& Parameters)
{
	FKismetAILoopHealth H;
	H.OnTurnEnd(/*NumToolCalls=*/0);
	H.OnTurnEnd(0);
	H.OnTurnEnd(0);
	TestEqual(TEXT("text-only turns do not count as barren"), H.GetBarrenTurnStreak(), 0);
	return true;
}

// A failed mutation must NOT reset the streak — that was the bug R3b/R4 were
// written for: a loop that kept "making progress" by failing the same edit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoopHealth_R4_FailedEditIsNotProgress, "KismetAI.LoopHealthTracker.R4_FailedEditIsNotProgress", LoopHealthFlags)
bool FLoopHealth_R4_FailedEditIsNotProgress::RunTest(const FString& Parameters)
{
	FKismetAILoopHealth H;
	for (int32 i = 0; i < 3; ++i)
	{
		const FString Args = FString::Printf(TEXT("{\"blueprint\":\"/Game/%d\"}"), i);
		H.DecideDisposition(EditTool, Args);
		H.OnToolResult(EditTool, Args, false, /*bErrored=*/true);
		H.OnTurnEnd(1);
	}
	TestEqual(TEXT("failed edits do not count as progress"), H.GetBarrenTurnStreak(), 3);
	return true;
}

// R1/R2: budget notes at the right remaining-turn counts.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoopHealth_R1R2_BudgetNotes, "KismetAI.LoopHealthTracker.R1R2_BudgetNotes", LoopHealthFlags)
bool FLoopHealth_R1R2_BudgetNotes::RunTest(const FString& Parameters)
{
	FKismetAILoopHealth H;

	TestTrue(TEXT("1 remaining -> FINAL TURN"),
		H.BuildOrchestratorNote(1).Contains(TEXT("FINAL TURN NEXT")));
	TestTrue(TEXT("2 remaining -> budget note"),
		H.BuildOrchestratorNote(2).Contains(TEXT("2 working turns remain")));
	TestTrue(TEXT("5 remaining -> budget note"),
		H.BuildOrchestratorNote(5).Contains(TEXT("working turns remain")));
	TestTrue(TEXT("6 remaining -> silent"), H.BuildOrchestratorNote(6).IsEmpty());
	TestTrue(TEXT("0 remaining -> silent"), H.BuildOrchestratorNote(0).IsEmpty());

	// The final-turn note replaces the countdown rather than joining it.
	TestFalse(TEXT("1 remaining does not also say 'turns remain'"),
		H.BuildOrchestratorNote(1).Contains(TEXT("working turns remain")));
	return true;
}

// R6 is one-shot, and gated on plan mode and on having mutated nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoopHealth_R6_NudgeIsOneShot, "KismetAI.LoopHealthTracker.R6_NudgeIsOneShot", LoopHealthFlags)
bool FLoopHealth_R6_NudgeIsOneShot::RunTest(const FString& Parameters)
{
	const FString Intent = TEXT("I'll add a PrintString node now.");

	{
		FKismetAILoopHealth H;
		TestTrue(TEXT("fires once"), H.ShouldNudgePrematureEndTurn(Intent, false));
		TestFalse(TEXT("never twice"), H.ShouldNudgePrematureEndTurn(Intent, false));
	}
	{
		FKismetAILoopHealth H;
		TestFalse(TEXT("silent in plan mode"), H.ShouldNudgePrematureEndTurn(Intent, /*bPlanMode=*/true));
	}
	{
		// Once something has been mutated, a text-only turn is plausible.
		FKismetAILoopHealth H;
		H.DecideDisposition(EditTool, ArgsA);
		TestTrue(TEXT("attempting a mutation is recorded"), H.HasAnyMutation());
		TestFalse(TEXT("silent after a mutation was attempted"),
			H.ShouldNudgePrematureEndTurn(Intent, false));
	}
	{
		FKismetAILoopHealth H;
		TestFalse(TEXT("silent on a finished-sounding answer"),
			H.ShouldNudgePrematureEndTurn(TEXT("The graph already contains that node."), false));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoopHealth_IntentDetection, "KismetAI.LoopHealthTracker.IntentDetection", LoopHealthFlags)
bool FLoopHealth_IntentDetection::RunTest(const FString& Parameters)
{
	auto Intent = [](const TCHAR* T) { return FKismetAILoopHealth::TextDescribesPendingAction(T); };

	TestTrue(TEXT("I'll"),         Intent(TEXT("I'll wire the exec pin.")));
	TestTrue(TEXT("Let me"),       Intent(TEXT("Let me check the graph first.")));
	TestTrue(TEXT("I am going to"), Intent(TEXT("I am going to create a variable.")));
	TestTrue(TEXT("case-insensitive"), Intent(TEXT("NOW I WILL ADD THE NODE")));

	TestFalse(TEXT("empty"), Intent(TEXT("")));
	TestFalse(TEXT("past tense is not intent"), Intent(TEXT("I added the node and compiled.")));
	TestFalse(TEXT("a plain answer"), Intent(TEXT("The Blueprint has three functions.")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoopHealth_FailedReadRetries, "KismetAI.LoopHealthTracker.FailedReadRetries", LoopHealthFlags)
bool FLoopHealth_FailedReadRetries::RunTest(const FString& Parameters)
{
	FKismetAILoopHealth H;
	H.DecideDisposition(ReadTool, ArgsA);
	H.OnToolResult(ReadTool, ArgsA, false, true);
	TestTrue(TEXT("a failed read was not an observation"), H.DecideDisposition(ReadTool, ArgsA) == EToolCallDisposition::Execute);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoopHealth_ChangedStateRetries, "KismetAI.LoopHealthTracker.ChangedStateRetries", LoopHealthFlags)
bool FLoopHealth_ChangedStateRetries::RunTest(const FString& Parameters)
{
	FKismetAILoopHealth H;
	H.OnToolResult(EditTool, ArgsA, false, true, TEXT("revision1"));
	TestTrue(TEXT("unchanged revision stays blocked"), H.DecideDisposition(EditTool, ArgsA, TEXT("revision1")) == EToolCallDisposition::SkipRepeatedFailedEdit);
	TestTrue(TEXT("a changed observation permits the same operation"), H.DecideDisposition(EditTool, ArgsA, TEXT("revision2")) == EToolCallDisposition::Execute);
	const FString ReadBatch = TEXT("{\"blueprint\":\"/Game/A\",\"operations\":[{\"action\":\"get_variables\"}]}");
	H.OnToolResult(TEXT("manage_blueprint"), ReadBatch, false, false);
	TestTrue(TEXT("mixed tool read-only action is a read"), H.DecideDisposition(TEXT("manage_blueprint"), ReadBatch) == EToolCallDisposition::SkipDuplicateRead);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoopHealth_CanonicalState, "KismetAI.LoopHealthTracker.CanonicalStateWinsOverStaleAlias", LoopHealthFlags)
bool FLoopHealth_CanonicalState::RunTest(const FString& Parameters)
{
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), TEXT("/Game/A.A"));
	TMap<FString, FString> Revisions;
	Revisions.Add(TEXT("/Game/A.A"), TEXT("old"));
	Revisions.Add(TEXT("/game/a"), TEXT("new"));
	const FString State = FKismetAILoopHealth::ObservedStateKey(TEXT("edit_blueprint"), Args, Revisions);
	TestTrue(TEXT("canonical latest observation wins"), State.Contains(TEXT("=new")));
	TestFalse(TEXT("stale alias does not define progress"), State.Contains(TEXT("=old")));
	return true;
}
