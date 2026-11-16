// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

// Tests for the agent loop's convergence guards (R3b/R4): a failed/rolled-back
// mutation must NOT count as progress, and an identical failing edit must be
// short-circuited instead of re-run. Guards the "Kismet loops resending the same
// failing edit_blueprint forever" bug.

#include "Misc/AutomationTest.h"
#include "KismetAIAgent.h"

constexpr auto LoopHealthTestFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

using EMutationOutcome = FKismetAIAgent::EMutationOutcome;

// ── IsToolResultError ────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoopHealth_IsToolResultError,
	"KismetAI.LoopHealth.IsToolResultError", LoopHealthTestFlags)
bool FLoopHealth_IsToolResultError::RunTest(const FString&)
{
	// The rolled-back edit_blueprint result from the real loop: wrapped in
	// <tool_use_error> even though the inner envelope says "success": true.
	const FString RolledBack = TEXT("<tool_use_error>{\"success\":true,\"data\":{\"operations_succeeded\":0,\"operations_failed\":1,\"rolled_back\":true}}</tool_use_error>");
	TestTrue(TEXT("rolled-back edit is an error"), FKismetAIAgent::IsToolResultError(RolledBack));

	TestTrue(TEXT("success:false flagged"), FKismetAIAgent::IsToolResultError(TEXT("{\"success\":false}")));
	TestTrue(TEXT("success: false (spaced) flagged"), FKismetAIAgent::IsToolResultError(TEXT("{\"success\": false}")));
	TestFalse(TEXT("plain success not an error"), FKismetAIAgent::IsToolResultError(TEXT("{\"success\":true,\"data\":{}}")));
	return true;
}

// ── ClassifyMutationOutcome ──────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoopHealth_ClassifyMutationOutcome,
	"KismetAI.LoopHealth.ClassifyMutationOutcome", LoopHealthTestFlags)
bool FLoopHealth_ClassifyMutationOutcome::RunTest(const FString&)
{
	// Non-mutation tool — never touches the bookkeeping.
	TestEqual(TEXT("read-only tool -> NotAMutation"),
		(int32)FKismetAIAgent::ClassifyMutationOutcome(/*mutation*/false, /*sc*/false, /*err*/false),
		(int32)EMutationOutcome::NotAMutation);

	// Short-circuited mutation didn't actually run.
	TestEqual(TEXT("short-circuited mutation -> NotAMutation"),
		(int32)FKismetAIAgent::ClassifyMutationOutcome(true, /*sc*/true, false),
		(int32)EMutationOutcome::NotAMutation);

	// THE BUG: a failed mutation must be FailedRecord (not progress), not counted
	// as a successful mutation.
	TestEqual(TEXT("failed mutation -> FailedRecord"),
		(int32)FKismetAIAgent::ClassifyMutationOutcome(true, false, /*err*/true),
		(int32)EMutationOutcome::FailedRecord);

	// A real mutation that succeeded is progress and clears the failed set.
	TestEqual(TEXT("successful mutation -> SucceededClear"),
		(int32)FKismetAIAgent::ClassifyMutationOutcome(true, false, /*err*/false),
		(int32)EMutationOutcome::SucceededClear);
	return true;
}

// ── End-to-end bookkeeping: the loop converges instead of spinning ───────────
// Replays the failing-edit sequence from the bug through the same rules the loop
// applies, and asserts (a) the no-progress counter climbs (so the NO PROGRESS
// nudge fires) and (b) the identical resend is short-circuited.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoopHealth_RepeatedFailedEditConverges,
	"KismetAI.LoopHealth.RepeatedFailedEditConverges", LoopHealthTestFlags)
bool FLoopHealth_RepeatedFailedEditConverges::RunTest(const FString&)
{
	// Mirror the loop's state.
	TSet<FString> SeenFailedMutationCallHashes;
	int32 ConsecutiveReadOnlyTurns = 0;
	const FString CallHash = TEXT("edit_blueprint\x1f{\"node\":\"__nonexistent__\"}");
	const FString ErrResult = TEXT("<tool_use_error>{\"error\":{\"error_code\":\"NODE_NOT_FOUND\"}}</tool_use_error>");

	bool bWasShortCircuitedOnSecondTry = false;

	for (int32 Attempt = 0; Attempt < 4; ++Attempt)
	{
		// Pre-execution short-circuit decision (R3b).
		const bool bShortCircuited = SeenFailedMutationCallHashes.Contains(CallHash);
		if (Attempt == 1) bWasShortCircuitedOnSecondTry = bShortCircuited;

		// A short-circuited call doesn't run; treat its synthetic result as errored.
		const bool bToolErrored = bShortCircuited ? true : FKismetAIAgent::IsToolResultError(ErrResult);

		// Post-execution bookkeeping.
		bool bTurnMutated = false;
		switch (FKismetAIAgent::ClassifyMutationOutcome(/*mutation*/true, bShortCircuited, bToolErrored))
		{
		case EMutationOutcome::FailedRecord:   SeenFailedMutationCallHashes.Add(CallHash); break;
		case EMutationOutcome::SucceededClear: bTurnMutated = true; SeenFailedMutationCallHashes.Reset(); break;
		case EMutationOutcome::NotAMutation:   break;
		}
		ConsecutiveReadOnlyTurns = bTurnMutated ? 0 : (ConsecutiveReadOnlyTurns + 1);
	}

	// The identical resend is caught on the 2nd attempt (1st failure recorded it).
	TestTrue(TEXT("identical failing edit short-circuited on resend"), bWasShortCircuitedOnSecondTry);
	// Failed edits never reset the counter, so the NO PROGRESS nudge (>=3) fires.
	TestTrue(TEXT("no-progress counter climbs past the nudge threshold"), ConsecutiveReadOnlyTurns >= 3);
	return true;
}

// A successful mutation in between clears the failed-edit guard, so a once-failing
// edit can be legitimately retried after the graph has changed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoopHealth_SuccessClearsFailedGuard,
	"KismetAI.LoopHealth.SuccessClearsFailedGuard", LoopHealthTestFlags)
bool FLoopHealth_SuccessClearsFailedGuard::RunTest(const FString&)
{
	TSet<FString> SeenFailedMutationCallHashes;
	const FString CallHash = TEXT("edit_blueprint\x1f{\"node\":\"Foo\"}");

	// 1) Edit fails -> recorded.
	SeenFailedMutationCallHashes.Add(CallHash); // FailedRecord
	TestTrue(TEXT("failed edit recorded"), SeenFailedMutationCallHashes.Contains(CallHash));

	// 2) A DIFFERENT edit succeeds -> SucceededClear clears the whole set.
	if (FKismetAIAgent::ClassifyMutationOutcome(true, false, false) == EMutationOutcome::SucceededClear)
	{
		SeenFailedMutationCallHashes.Reset();
	}

	// 3) The once-failing edit is no longer short-circuited.
	TestFalse(TEXT("guard cleared after a successful mutation"),
		SeenFailedMutationCallHashes.Contains(CallHash));
	return true;
}
