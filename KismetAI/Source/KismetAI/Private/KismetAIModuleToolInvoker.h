// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "KismetAIToolInvoker.h"

#include "KismetAIGameThreadDispatcher.h"

class FKismetAIModule;
struct FKismetAIToolPolicySlot;

// Production IKismetAIToolInvoker: owns the tool dispatch table and calls the
// editor module's handlers. This is the adapter that keeps FKismetAIAgent free
// of any editor dependency — see KismetAIToolInvoker.h.
//
// Ownership: Invoke and GetPolicy run on the agent's worker thread, so
// they read nothing through the module. The dispatcher and the policy slot are
// held by shared reference, taken when the invoker is built (game thread).
// The module is only dereferenced inside the game-thread body, which the
// dispatcher never runs after Close — so a worker that outlives a timed-out
// shutdown cannot reach the destroyed module object.
class FKismetAIModuleToolInvoker final : public IKismetAIToolInvoker
{
public:
	explicit FKismetAIModuleToolInvoker(FKismetAIModule& InModule);

	// Tests: a dispatcher of the test's own, so a test can Close it
	// without closing the module's.
	FKismetAIModuleToolInvoker(FKismetAIModule& InModule,
		TSharedRef<FKismetAIGameThreadDispatcher, ESPMode::ThreadSafe> InDispatcher);

	// Resolves (tool, mode) to a Do* handler and runs it through
	// FKismetAIModule::RunOnGameThread with the invocation's admission seam.
	virtual FKismetAIInvokeResult Invoke(const FKismetAIToolInvocation& Invocation) override;

	// The module's one policy, shared by every entry point.
	virtual const IKismetAIToolPolicy* GetPolicy() const override;

	virtual TSet<FString> GetDispatchableToolNames() const override;
	virtual FString ResolveHandlerName(const FString& ToolName, const FString& Mode) const override;

	// Asset fingerprints and run checkpoints, on the game thread through
	// the same dispatcher (never cancelled by the run: a checkpoint that is
	// half recorded is worse than one that is late). Abandoned only when the
	// dispatcher is closed; the result then says "not captured".
	virtual TArray<FKismetAIAssetFingerprint> CaptureAssetState(const TArray<FString>& PackagePaths) override;
	virtual TArray<FKismetAIAssetFingerprint> BeginMutation(const FKismetAIMutationContext& Context) override;
	virtual void EndMutation(const FKismetAIMutationContext& Context) override;

	// Static mirrors so the manifest guard can run without building an invoker.
	static TSet<FString> DispatchableToolNames();
	static FString HandlerNameFor(const FString& ToolName, const FString& Mode);

private:
	// Runs Work(Module) on the game thread and waits. False when it never ran.
	bool RunOnGameThread(TFunction<void(FKismetAIModule&)> Work);

	FKismetAIModule& Module;   // game-thread body only
	TSharedRef<FKismetAIGameThreadDispatcher, ESPMode::ThreadSafe> Dispatcher;
	TSharedRef<FKismetAIToolPolicySlot, ESPMode::ThreadSafe> PolicySlot;
};
