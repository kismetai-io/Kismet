// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// The shared execution request.
//
// Editor tools are reachable from four places: the Slate panel's agent run,
// POST /api/v1/agent/run, MCP tools/call, and the REST tool routes under
// /api/v1/. All four now run a call through FKismetAIToolExecutor::Execute, so
// argument normalization, registry lookup and validation, policy, cancellation,
// dispatch and typed outcomes are decided once. What stays per transport:
//
//   - authentication (X-Kismet-Token, MCP bearer + loopback peer, the editor
//     user at the panel) — the transport verifies it and hands the executor an
//     FKismetAICallerContext saying who it verified. The executor refuses a
//     context that does not claim verification instead of assuming one;
//   - request size caps and body parsing;
//   - response rendering (model string with <tool_use_error>, MCP content
//     block, REST {success,data}/{success,error} envelope with a status code).
//
// The game-thread admission seam
// ------------------------------
// FKismetAIToolInvocation::Admit is the ONE point where a decision can be
// enforced immediately before editor work begins, on the game thread, for every
// entry point. The production invoker (FKismetAIModuleToolInvoker) runs, in
// FKismetAIModule::RunOnGameThread, in this order and on the game thread:
//
//   1. the editor-state guard (saving, garbage collection, slow task) —
//      GetEditorAccessError;
//   2. Admit() — built by FKismetAIToolExecutor::Execute; today it re-checks
//      the call's cancellation token and then asks the active policy's
//      IKismetAIToolPolicy::EvaluateAdmission with the caller, tool, mode and
//      normalized arguments;
//   3. the handler.
//
// A refusal from 1 or 2 means the handler is not called. Scoped authority
// (KismetAIAuthority.h) makes its asset/operation/caller decision in
// FKismetAIScopedToolPolicy::EvaluateAdmission. Neither needs another hook per
// transport.
//
// Cancellation at the point editor work begins
// ----------------------------------------------------
// The invocation carries the call's own CancelToken (owned, a child of the
// run's). A call moves through these phases, and each reports differently:
//
//   queued    — handed to the game-thread dispatcher, callback not started.
//               Cancelling now abandons it: the waiting worker is woken at
//               once (the game thread need not run anything), the callback
//               becomes a no-op whenever it runs, and the call is reported
//               CANCELLED / not started (bStoppedBeforeStart). Admission is
//               never reached, so no single-use grant is spent.
//   entry     — the callback starts on the game thread and checks the token
//               first, before the editor-state guard; then Admit checks it
//               again immediately before the handler. Either refusal is also
//               CANCELLED / not started.
//   running   — the handler is a native Unreal operation and is NOT
//               preempted. A Stop now does not wake the worker into a guess:
//               the worker keeps waiting (the game thread is busy with the
//               very work it waits for), the handler's real result is
//               returned, and bCancelRequestedWhileRunning says Stop arrived
//               during it. Completed edits are not undone.
//   completed — the result is the handler's; cancellation afterwards only
//               stops the NEXT call from being admitted.
//
// "Not started" is therefore only ever reported for work that never entered
// the handler.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "KismetAIToolResult.h"
#include "KismetAICancellation.h"

// Which transport produced a call. Scoped authority grants name the entry
// points they apply to; the executor itself only needs to know the
// transport verified the caller.
enum class EKismetAIEntryPoint : uint8
{
	// Default-constructed. The executor refuses it: an adapter that forgot to
	// say who it authenticated must not run with an invented identity.
	Unspecified,
	// Code inside the editor process calling directly (automation tests, the
	// eval fixture generator, an agent created without a transport).
	InProcess,
	// The Slate panel's agent run; the caller is the editor user.
	Panel,
	// POST /api/v1/agent/run, after the X-Kismet-Token gate.
	HttpAgentRun,
	// POST /mcp tools/call, after the bearer token + loopback + Origin gate.
	Mcp,
	// A REST tool route under /api/v1/, after the X-Kismet-Token gate.
	Rest,
};

KISMETAIRUNTIME_API const TCHAR* LexToString(EKismetAIEntryPoint EntryPoint);

// Who is asking, as established by the transport that received the call.
struct KISMETAIRUNTIME_API FKismetAICallerContext
{
	EKismetAIEntryPoint EntryPoint = EKismetAIEntryPoint::Unspecified;

	// How the transport established the caller ("x-kismet-token",
	// "mcp-bearer", "editor-user", "in-process"). Never the secret itself.
	FString Principal;

	// Set only by the factories below, i.e. by an adapter after its own check.
	bool bTransportVerified = false;

	bool IsVerified() const
	{
		return bTransportVerified && EntryPoint != EKismetAIEntryPoint::Unspecified;
	}

	// For an adapter whose authentication check has just passed.
	static FKismetAICallerContext Verified(EKismetAIEntryPoint InEntryPoint, const FString& InPrincipal)
	{
		FKismetAICallerContext C;
		C.EntryPoint = InEntryPoint;
		C.Principal = InPrincipal;
		C.bTransportVerified = InEntryPoint != EKismetAIEntryPoint::Unspecified;
		return C;
	}

	// Code already running inside the editor process. Plan mode still applies;
	// scoped authority does not (it already holds the editor's authority).
	static FKismetAICallerContext InProcess()
	{
		return Verified(EKismetAIEntryPoint::InProcess, TEXT("in-process"));
	}
};

// What the executor hands an invoker. Invoke is synchronous, and an invoker
// that queues work elsewhere must copy what it needs out of the invocation
// (the invocation itself lives on the caller's stack).
//
// Lifetime of Admit: it owns its captures except the policy, which it holds by
// pointer (module-owned, or the executor's default policy). It is only safe to
// call while Invoke has not returned. The production dispatcher guarantees
// that: it runs Admit only after moving the job to "running", and a running
// job is never abandoned, so Invoke is still waiting whenever Admit runs. A
// job abandoned while queued never calls Admit.
struct KISMETAIRUNTIME_API FKismetAIToolInvocation
{
	FString ToolName;

	// The "mode" argument for mode-routed reads; empty otherwise.
	FString Mode;

	// The body the handler reads.
	TSharedPtr<FJsonObject> RequestBody;

	// Owned observations from this admitted agent run; empty for legacy external calls.
	TMap<FString, FString> ObservedAssetRevisions;
	bool bRequireReadRevision = false; // Resumed history must not authorize unobserved edits.

	FKismetAICallerContext Caller;

	// The admission seam (see the file comment). An invoker that does editor
	// work MUST call this on the game thread immediately before the handler and
	// return its result instead of running the handler when it is set. Unset
	// means "admit".
	TFunction<TOptional<FToolExecutionResult>()> Admit;

	// This call's cancellation. An invoker that queues the work must
	// stop waiting when it is cancelled before the work starts, and must not
	// start the work after that; see the file comment for each phase.
	FKismetAICancellationToken CancelToken;

	// What a call stopped before its handler started reports. Built by the
	// executor so every "not started" result has the same code and wording no
	// matter where cancellation was caught.
	FToolExecutionResult CancelledBeforeStart;

	// Convenience for invokers: run Admit if present.
	TOptional<FToolExecutionResult> RunAdmission() const
	{
		return Admit ? Admit() : TOptional<FToolExecutionResult>();
	}
};

// What an invoker returns. Production invokers always fill Typed; test fakes
// may return only a model string. Transports that render their own envelope
// (REST) need Typed.
struct KISMETAIRUNTIME_API FKismetAIInvokeResult
{
	FString ModelString;
	TOptional<FToolExecutionResult> Typed;

	// Cancelled while queued or at callback entry: the handler did not
	// run and never will. Typed is the invocation's CancelledBeforeStart.
	bool bStoppedBeforeStart = false;

	// Cancellation arrived after the handler started. The result is
	// still the handler's own (applied, failed, rolled back — whatever it
	// says); nothing was preempted or undone.
	bool bCancelRequestedWhileRunning = false;

	static FKismetAIInvokeResult FromTyped(const FToolExecutionResult& InTyped)
	{
		FKismetAIInvokeResult R;
		R.ModelString = InTyped.ToModelString();
		R.Typed = InTyped;
		return R;
	}

	static FKismetAIInvokeResult FromModelString(const FString& InModelString)
	{
		FKismetAIInvokeResult R;
		R.ModelString = InModelString;
		return R;
	}
};
