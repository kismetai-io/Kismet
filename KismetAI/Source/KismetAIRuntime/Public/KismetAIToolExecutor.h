// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// One lifecycle for every tool call, and now the one execution
// service every entry point uses: the panel's agent run, /api/v1/agent/run,
// MCP tools/call and the REST tool routes. KismetAIToolExecution.h describes
// what stays per transport and the game-thread admission seam.
//
// The order matters and is deliberate:
//
//   0. caller      — the transport must say whom it verified; an unverified
//                    context is refused before anything else
//   0b. lookup     — the tool must be a registry tool an invoker can run;
//                    unknown and agent-local names are refused here rather than
//                    handed to a dispatcher that might substitute a default
//   1. policy      — IKismetAIToolPolicy returns allow / deny / ask before
//                    anything happens
//   2. validate    — reject malformed arguments with a structured error the
//                    model can act on, before touching the editor
//   3. cancel      — last check on the calling thread; the token then travels
//                    with the invocation into the game-thread dispatcher
//   4. dispatch    — the invoker runs the work on the game thread, calling the
//                    admission seam (editor-state guard, then cancellation and
//                    IKismetAIToolPolicy::EvaluateAdmission) immediately before
//                    the handler
//   5. shape       — truncate oversized results so one tool cannot blow the
//                    context window (model string only; the typed result a REST
//                    caller receives is not truncated)
//
// Cancellation sits between validation and dispatch on purpose: there is no
// point refusing a call we were going to reject anyway, and no point starting
// one the user has already abandoned.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "KismetAIToolPolicy.h"
#include "KismetAICancellation.h"
#include "KismetAIToolExecution.h"
#include "KismetAIToolResult.h"

class IKismetAIToolInvoker;

// Everything one call needs to know about itself: the shared execution request.
struct KISMETAIRUNTIME_API FToolCallContext
{
	FString ToolName;

	// Who is asking, as verified by the transport. Default-constructed means
	// "nobody said", and the executor refuses it (CALLER_NOT_VERIFIED).
	FKismetAICallerContext Caller;

	// The marshalled request body the handler expects.
	TSharedPtr<FJsonObject> RequestBody;

	// Owned observations from this admitted agent run; empty for legacy external calls.
	TMap<FString, FString> ObservedAssetRevisions;
	bool bRequireReadRevision = false; // Resumed history must not authorize unobserved edits.

	// The model's raw arguments, used for the mode lookup that decides
	// read_blueprint's result cap.
	TSharedPtr<FJsonObject> Arguments;

	// Plan mode refuses anything that is not Inspect or AgentState.
	bool bPlanModeActive = false;

	// False for headless runs. An Ask verdict cannot be answered by anyone, so
	// policies refuse instead of asking (inability to ask is not permission).
	bool bInteractiveAllowed = false;

	// The agent session the call belongs to; empty for REST and MCP. Session
	// grants and approvals are bound to it.
	FString SessionId;

	// Config fallback for tools with no explicit cap.
	int32 DefaultMaxResultChars = 30000;

	// Checked immediately before dispatch. A cancelled call is not attempted.
	FKismetAICancellationToken CancelToken;
};

// What the model is handed, plus what the loop needs to know about it.
struct KISMETAIRUNTIME_API FToolCallOutcome
{
	// Exactly the string that becomes the tool_result content.
	FString ResultForModel;

	// True when the result carries an error marker. Read from the lifecycle
	// rather than re-derived by string matching at the call site.
	bool bIsError = false;

	// True when the result was truncated to fit the cap.
	bool bWasTruncated = false;

	// True when policy refused the call. Distinct from a tool that ran and
	// failed: nothing was attempted, so loop-health must not record it as a
	// failed mutation.
	bool bWasDenied = false;

	// True when the call was cancelled before its handler started: before
	// dispatch, while queued for the game thread, at callback entry or at
	// admission. Also "nothing was attempted", but for a different
	// reason, and the model is told so. Never set for a handler that ran.
	bool bWasCancelled = false;

	// Cancellation arrived while the handler was running. The handler
	// was not preempted; ResultForModel / Typed are its real outcome, and any
	// change it applied stays applied.
	bool bCancelRequestedWhileRunning = false;

	int32 DurationMs = 0;

	// The same outcome as a typed result, for transports that render their own
	// envelope (REST). Set on every path: from the invoker when the call ran,
	// otherwise from the refusal. Not truncated.
	FToolExecutionResult Typed;
};

class KISMETAIRUNTIME_API FKismetAIToolExecutor
{
public:
	// Uses the default policy.
	explicit FKismetAIToolExecutor(IKismetAIToolInvoker& InInvoker);

	// Uses the invoker's policy when it supplies one (production: the module's
	// shared policy), else the default policy.
	//
	// Inject a policy explicitly — tests, and any future per-session policy.
	FKismetAIToolExecutor(IKismetAIToolInvoker& InInvoker, const IKismetAIToolPolicy& InPolicy)
		: Invoker(InInvoker), Policy(InPolicy) {}

	// Called when the policy returns Ask. Returns true to proceed (an
	// allow-once answer to the verdict's request). Unset means nobody can be
	// asked, and the call is refused.
	TFunction<bool(const FPolicyVerdict&)> OnApprovalRequired;

	// The richer form: the approver answers the verdict's approval
	// request by id, once or for the session. Takes precedence when set. Either
	// way the policy's ResolveApproval decides whether the answer is valid.
	TFunction<FKismetAIApprovalAnswer(const FPolicyVerdict&)> OnApprovalAnswer;

	FToolCallOutcome Execute(const FToolCallContext& Context);

	// The one "cancelled before it ran" result, wherever the cancellation was
	// caught (pre-dispatch, queued, callback entry, admission). The journal
	// recognises it (KismetAIToolCallJournal::IsExecutorCancelledResult).
	static FToolExecutionResult CancelledBeforeStartResult(const FString& ToolName);
	static FString CancelledBeforeStartModelString(const FString& ToolName);

	// Step 5 in isolation: cap a result and note whether it was cut. Exposed
	// because the truncation rules are worth testing without a live invoker.
	static FString TruncateResult(const FString& Result, int32 MaxChars, bool& bOutTruncated);

	// Argument normalization every entry point shares. Some providers serialize
	// nested array arguments (operations[], parameters[]) as a JSON-encoded
	// string; this turns them back into arrays in place so validation and the
	// handler see the same shape. Null is left null.
	static void NormalizeArguments(const TSharedPtr<FJsonObject>& Arguments);

private:
	IKismetAIToolInvoker& Invoker;
	FKismetAIDefaultToolPolicy DefaultPolicy;
	const IKismetAIToolPolicy& Policy;
};
