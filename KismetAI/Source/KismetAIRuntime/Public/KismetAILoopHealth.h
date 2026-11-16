// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Convergence guards for the agent loop.
//
// Six rules, developed one incident at a time, that keep a run from spinning:
//
//   R1  On what will be the model's last turn, tell it to commit now.
//   R2  While the turn budget is shrinking, say how much is left.
//   R3  An identical repeat of a read-only call is short-circuited — the model
//       already has that answer.
//   R3b An identical resend of a mutation that already failed is short-circuited.
//       Nothing has changed, so it would fail the same way. A changed observed state allows a new attempt; unrelated success does not.
//   R4  After several turns of tool calls that changed nothing, tell the model
//       to stop investigating and act.
//   R6  A text-only turn that describes an action it did not take gets one nudge
//       instead of ending the run.
//
// (R5 is not a rule here: it is the decision to express the budget in turns,
// which the notes below reflect.)
//
// These lived as five loose locals and three inline blocks inside RunTask. They
// are one state machine, they are pure, and they are the behavior most likely to
// be lost silently in a refactor — so they get their own type and their own
// tests.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// What the loop should do with a tool call before running it.
enum class EToolCallDisposition : uint8
{
	// Run it.
	Execute,
	// R3: identical read-only call already answered this run.
	SkipDuplicateRead,
	// R3b: identical mutation already failed this run and nothing has changed.
	SkipRepeatedFailedEdit,
};

class KISMETAIRUNTIME_API FKismetAILoopHealth
{
public:
	// --- per tool call ---------------------------------------------------

	// Decide whether to run this operation against its observed resource state.
	// Only successful read results are retained as observations.
	// CanonicalArgs must be a stable serialization — same arguments, same string.
	EToolCallDisposition DecideDisposition(const FString& ToolName, const FString& CanonicalArgs, const FString& ObservedState = FString());

	// Record what happened. bShortCircuited means DecideDisposition returned a
	// Skip, in which case nothing actually ran and no progress was made.
	void OnToolResult(const FString& ToolName, const FString& CanonicalArgs,
		bool bShortCircuited, bool bErrored, const FString& ObservedState = FString());

	// A revision conflict proves prior read answers are stale; allow an exact reread.
	void InvalidateReadObservations() { SeenReadOnlyCallHashes.Reset(); }

	static FString ObservedStateKey(const FString& ToolName, const TSharedPtr<FJsonObject>& Args,
		const TMap<FString, FString>& Revisions);

	// --- per turn --------------------------------------------------------

	// Call once after a turn's tool calls are done. NumToolCalls is that turn's
	// count; a turn with no tool calls does not count toward the no-progress
	// streak.
	void OnTurnEnd(int32 NumToolCalls);

	// The "[orchestrator] ..." note appended to the tool results, which the model
	// reads at the start of its next turn. Empty when there is nothing to say.
	//
	// TurnsRemaining is measured from the NEXT turn, because that is when the
	// model sees this: a note written when one turn remains reaches it on its
	// final working turn.
	FString BuildOrchestratorNote(int32 TurnsRemaining) const;

	// --- R6 --------------------------------------------------------------

	// True when a text-only end_turn should be nudged rather than accepted: the
	// model described an action, took none, and has not mutated anything all run.
	// One-shot — asking twice returns false the second time, and marks it used.
	bool ShouldNudgePrematureEndTurn(const FString& AssistantText, bool bPlanModeActive);

	// Whether any mutation tool has been invoked this run (gates R6).
	bool HasAnyMutation() const { return bAnyMutation; }

	// Consecutive turns that made tool calls but changed nothing.
	int32 GetBarrenTurnStreak() const { return ConsecutiveReadOnlyTurns; }

	// --- pure helpers, exposed for testing --------------------------------

	// Does this assistant text read as "I am about to do X" rather than a
	// finished answer? Heuristic over the model's own phrasing.
	static bool TextDescribesPendingAction(const FString& Text);

	// The message returned in place of a short-circuited call.
	static FString GetDuplicateReadMessage();
	static FString GetRepeatedFailedEditMessage();

private:
	// R3: (tool, args) of read-only calls already answered.
	TSet<FString> SeenReadOnlyCallHashes;
	// R3b: semantic operation plus observed state of mutations that failed.
	TSet<FString> SeenFailedMutationCallHashes;
	// R4: turns with tool calls but no successful mutation.
	int32 ConsecutiveReadOnlyTurns = 0;
	// R6: one-shot.
	bool bEndTurnNudged = false;
	// Gates R6: once anything has been mutated, a text-only turn is plausible.
	bool bAnyMutation = false;
	// Whether the turn in progress has successfully mutated anything.
	bool bTurnMutated = false;
};
