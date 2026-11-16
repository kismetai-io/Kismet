// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "KismetAIToolExecution.h"
#include "KismetAIMutationIntent.h"

class IKismetAIToolPolicy;

// Who is mutating what, for the asset-state hooks below.
struct FKismetAIMutationContext
{
	FString SessionId;
	FString RunId;
	FString CallId;
	FString ToolName;
	// Normalized long package paths the call names (KismetAIAuthority::DescribeOperation).
	TArray<FString> Targets;
};

// The agent's entire dependency on the editor, behind one method.
//
// Previously, FKismetAIAgent held an FKismetAIModule& and used it for exactly
// one thing: Module.ExecuteHandlerDirect(Handler, Body) at the bottom of
// ExecuteTool. That single reference is what tied the agent loop — retry,
// context management, loop-health rules, plan mode — to a 17k-line editor module
// and made it untestable without a loaded editor.
//
// Implementations:
//   FKismetAIModuleToolInvoker  — production; owns the tool dispatch table and
//                                 calls the module's handlers.
//   FScriptedToolInvoker        — tests; canned results plus a call log.
class IKismetAIToolInvoker
{
public:
	virtual ~IKismetAIToolInvoker() = default;

	// Run a tool backed by an editor handler. Called only by
	// FKismetAIToolExecutor::Execute, after normalization, validation, policy and
	// the pre-dispatch cancellation check.
	//
	// Synchronous. An implementation that touches the editor MUST call
	// Invocation.RunAdmission() on the game thread immediately before the
	// handler and return that refusal instead of running the handler (see
	// KismetAIToolExecution.h, "The game-thread admission seam").
	//
	// Never throws; an unknown tool returns the UNKNOWN_TOOL error.
	virtual FKismetAIInvokeResult Invoke(const FKismetAIToolInvocation& Invocation) = 0;

	// The policy every call dispatched through this invoker is judged by, so the
	// panel, /agent/run, MCP and REST cannot each carry a different one. Null
	// means the executor's built-in default policy (tests).
	virtual const IKismetAIToolPolicy* GetPolicy() const { return nullptr; }

	// Tools this invoker can dispatch. The KismetAI.ToolManifest guard asserts
	// this equals the advertised set minus the agent's in-process tools.
	virtual TSet<FString> GetDispatchableToolNames() const = 0;

	// Name of the handler a (tool, mode) pair resolves to, or empty when the tool
	// is not dispatchable. Lets tests pin routing without invoking handlers.
	virtual FString ResolveHandlerName(const FString& ToolName, const FString& Mode) const = 0;

	// ── Asset state (KismetAIMutationIntent.h) ─────────────────────────
	// All three run the editor work on the game thread and block the caller. The
	// defaults describe nothing: a call journaled through an invoker without asset
	// state is reconciled as no_evidence, never as not_applied.

	// The state of each asset now (one fingerprint per path, in order). A path
	// that cannot be described gets bSupported=false and a reason.
	virtual TArray<FKismetAIAssetFingerprint> CaptureAssetState(const TArray<FString>& PackagePaths)
	{
		TArray<FKismetAIAssetFingerprint> Out;
		for (const FString& Path : PackagePaths)
		{
			FKismetAIAssetFingerprint F;
			F.AssetPath = Path;
			F.UnsupportedReason = TEXT("this tool invoker cannot describe asset state");
			Out.Add(MoveTemp(F));
		}
		return Out;
	}

	// Immediately before an agent run's mutating call: the before-state of its
	// targets (what the mutation_intent records). The editor also makes sure the
	// run has a checkpoint of each target and notes any change made outside the
	// run since the run's previous call (KismetAICheckpoints.h).
	virtual TArray<FKismetAIAssetFingerprint> BeginMutation(const FKismetAIMutationContext& Context)
	{
		return CaptureAssetState(Context.Targets);
	}

	// Immediately after that call returned, whatever its result.
	virtual void EndMutation(const FKismetAIMutationContext& Context) {}
};
