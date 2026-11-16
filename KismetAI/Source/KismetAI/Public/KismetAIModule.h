// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "HttpRequestHandler.h"
#include "HttpRouteHandle.h"
class FKismetAIHttpTransport;
class FKismetAISourceJobs;
#include "KismetAIAgent.h"
#include "KismetAIMessageQueue.h"

class FKismetAISessionController;
class FKismetAICheckpointStore;
struct FKismetAIRestoreReport;
struct FKismetAIMutationContext;
struct FKismetAIAssetFingerprint;
class FKismetAIExternalRunRecord;
class FKismetAIExternalRunRegistry;
#include "KismetAIToolResult.h"
#include "KismetAIToolPolicy.h"
#include "KismetAIToolExecution.h"
#include "KismetAIGameThreadDispatcher.h"
#include "AssetRegistry/AssetData.h"
#include <atomic>

class SDockTab;
class FSpawnTabArgs;
class FBlueprintEditor;
class FWorkflowAllowedTabSet;
class FLayoutExtender;
class SKismetAIPanel;
struct FHttpServerRequest;
struct FHttpServerResponse;
typedef TFunction<void(TUniquePtr<FHttpServerResponse>&& Response)> FHttpResultCallback;

/**
 * Represents one resolved asset reference, passed to FormatReferencesBlock.
 * Used across KismetAIModule and SKismetAIPanel for the @-reference injection pipeline.
 */
struct FAssetRef
{
	FString Path;        // Full object path e.g. "/Game/Blueprints/BP_Player.BP_Player"
	FString Name;        // Short asset name e.g. "BP_Player"
	FString Class;       // Friendly class name e.g. "Blueprint"
	FString Summary;     // Optional cheap summary from FAssetData tags (no asset load)
	bool bFocused = false;  // True for the primary "focused" blueprint
	bool bMissing = false;  // True when asset not found in the Asset Registry
};

// The policy every tool call is judged by, in a shared slot
//. Invokers hold the slot, not the module, so a worker thread never
// reaches the policy through module memory.
struct FKismetAIToolPolicySlot
{
	FKismetAIScopedToolPolicy Scoped;
	std::atomic<const IKismetAIToolPolicy*> Override{ nullptr };   // tests only

	const IKismetAIToolPolicy& Get() const
	{
		const IKismetAIToolPolicy* Current = Override.load();
		return Current ? *Current : static_cast<const IKismetAIToolPolicy&>(Scoped);
	}
};

class FKismetAIModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	// Unloading this module while the editor keeps running is not
	// supported. ShutdownModule drains every agent worker, but a worker that
	// outlives the drain deadline is still executing this module's code; with
	// this false, "Module Unload/Reload" and hot reload abandon the module
	// (ShutdownModule, then the object is destroyed) instead of unmapping the
	// library under it.
	virtual bool SupportsDynamicReloading() override { return false; }

	static inline FKismetAIModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FKismetAIModule>("KismetAI");
	}

	// Tab ID for the KismetAI editor panel
	static const FName KismetAIPanelTabId;

	// Server status accessor
	bool IsHttpServerRunning() const { return bIsRunning; }

	// Agent config accessors
	const FKismetAIAgentConfig& GetAgentConfig() const { return AgentConfig; }

	// --- Shared tool execution ---

	// The one policy every entry point's tool calls are judged by: the panel and
	// /agent/run agents, MCP and the REST tool routes all dispatch through
	// FKismetAIModuleToolInvoker, whose GetPolicy() returns this.
	//
	// Now that is FKismetAIScopedToolPolicy: plan mode plus scoped
	// authority over the normalized asset, operation class and caller.
	KISMETAI_API const IKismetAIToolPolicy& GetToolPolicy() const
	{
		return ToolPolicySlot->Get();
	}

	// --- Scoped authority ---

	// The grants, approvals and decision log every entry point is judged by.
	// Users grant and revoke through KismetAI.Authority.* console commands (the
	// editor user) or /api/v1/authority (X-Kismet-Token holders); an HTTP agent
	// run may carry its own run-scoped grant.
	KISMETAI_API FKismetAIAuthorityStore& GetAuthorityStore() const { return ToolPolicySlot->Scoped.GetStore(); }

	// What admission resolves an existing-asset target to: the package(s) the
	// handler's lookup (exact path, then unique leaf name) would act on, found
	// through the asset registry and in-memory packages only — nothing is
	// loaded. Game thread only.
	KISMETAI_API static TArray<FString> ResolveAuthorityTarget(const FKismetAIAuthorityTarget& Target);

	// The default panel grant: ordinary edits and new assets under /Game for the
	// editor user's own panel runs. Re-created at startup; revocable.
	static constexpr const TCHAR* PanelDefaultGrantLabel = TEXT("panel-default");
	KISMETAI_API void InstallDefaultAuthority();

	// /api/v1/authority (behind the X-Kismet-Token gate): GET lists active
	// grants and recent decisions; POST /grants creates a grant for REST, MCP or
	// HTTP agent-run callers (never the panel or in-process); POST /revoke
	// revokes by id or label, or everything with {"all": true}.
	KISMETAI_API bool HandleAuthorityList(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	KISMETAI_API bool HandleAuthorityGrant(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	KISMETAI_API bool HandleAuthorityRevoke(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	// Tests only: route every entry point through a different policy. Pass
	// nullptr to restore the default. The pointee must outlive its use.
	KISMETAI_API void SetToolPolicyOverrideForTests(const IKismetAIToolPolicy* InPolicy) { ToolPolicySlot->Override = InPolicy; }

	// The REST tool-route adapter. Parses the body, runs it through
	// FKismetAIToolExecutor (normalization, registry lookup and validation,
	// policy, cancellation, game-thread admission, dispatch) as ToolName with
	// the route's fixed Mode (nullptr when the route is not a mode alias), and
	// renders the typed result as the {success,data}/{success,error} envelope.
	// Caller is what the transport verified; the Handle* methods below pass
	// InProcess because only in-editor code can call them directly.
	KISMETAI_API bool ServeToolRoute(const TCHAR* ToolName, const TCHAR* Mode, const FKismetAICallerContext& Caller,
		const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	void SetAgentConfig(const FKismetAIAgentConfig& InConfig) { AgentConfig = InConfig; }

	// Settings persistence
	void SaveSettings();

	// --- Panel registry (multi-panel sync) ---
	void RegisterPanel(SKismetAIPanel* Panel);
	void UnregisterPanel(SKismetAIPanel* Panel);
	void BroadcastJS(const FString& Script);

	// --- Agent lifecycle ---
	// Every run is admitted, cancelled, answered and drained by the session
	// controller (KismetAISessionController.h). One run holds the
	// execution lease at a time, panel or /agent/run.
	/**
	 * Panel entry point — accepts a pre-built JSON refs array from the UI.
	 * Calls ResolveRefs, builds the <references> block, snapshots all Blueprint refs,
	 * and starts the agent. While another run holds the lease the message is
	 * queued instead and starts when the lease is released.
	 * Must be called on the game thread.
	 */
	void LaunchAgent(const FString& UserMessage, const FString& ImagesJson, const FString& SessionId, const FString& RefsJson);
	// Stop the run holding the lease, whatever started it. Returns at once;
	// the run's own finalization releases the lease. Never blocks.
	void AbortAgent();

	// --- Message queue ---
	void EnqueueUserMessage(const FString& Text, const FString& RefsJson, const FString& ImagesJson);
	void SteerNow(const FString& Text, const FString& RefsJson, const FString& ImagesJson);
	void SteerQueued(const FString& Id);
	void RemoveQueued(const FString& Id);
	void MoveQueued(const FString& Id, int32 NewIndex);
	void EditQueued(const FString& Id, const FString& NewText);
	FString GetQueueJson();
	void BroadcastQueueState();
	bool IsAgentRunning() const;
	const FString& GetCurrentSessionId() const;
	void SetCurrentSessionId(const FString& SessionId);

	// Snapshot a graph as the active session's baseline. The agent-run entry point
	// does this automatically at run start; exposed so tests can reproduce the
	// "agent-created this session" tracking that reposition-on-first-wire relies on.
	// A node whose GUID is absent from the baseline counts as agent-created.
	KISMETAI_API void TakeSessionBaseline(const FString& SessionId, const FString& BlueprintPath, const FString& GraphName);

	// Arm the per-session diff baseline for a fresh agent run started over HTTP
	// (/api/v1/agent/run). Mirrors LaunchAgent's panel-path setup: sets the module
	// CurrentSessionId, resets that session's diff state, and snapshots the target
	// blueprint's primary editing graph so run-scoped "agent-created" tracking (and
	// reposition-on-first-wire) is armed. BlueprintPath may be empty (the session
	// id is still set; no baseline is taken). Returns the session id used. Public so
	// tests can drive the exact setup HandleAgentRun performs.
	KISMETAI_API FString ArmDiffSessionForRun(const FString& RequestedSessionId, const FString& BlueprintPath);

	// Deliver a user's answer to the lease holder's pending ask_user question.
	// Returns true if that run's agent was waiting on this question_id; a
	// cancelled or finished run never takes one.
	// Called on the game thread from the JS bridge and the HTTP answer route.
	// AnsweredVia says which transport delivered it: approval questions accept
	// only the panel (the editor user), never a REST token holder.
	bool SubmitAgentAnswer(const FString& QuestionId, const FString& AnswersJson,
		EKismetAIEntryPoint AnsweredVia = EKismetAIEntryPoint::Unspecified);


	// --- Blueprint diff (accept/reject/reapply changes) ---
	// Compare the active session's baseline with the graph, tint changed nodes
	// and offer accept/reject. Runs when an agent run ends; public for tests.
	KISMETAI_API void ComputeDiffAndMarkNodes();
	// Accept deletes the run's checkpoints. Reject restores every asset of the
	// active session's latest run from its checkpoint (KismetAICheckpoints.h)
	// and reports each asset's outcome; Reapply puts the run's result back.
	KISMETAI_API void AcceptChanges();
	KISMETAI_API void RejectChanges();
	KISMETAI_API void ReapplyChanges();

	// ── asset state, run checkpoints and recovery ─────────────────────
	// Game thread. The agent reaches these through FKismetAIModuleToolInvoker.
	// Fingerprint each package now (KismetAIAssetState.h).
	TArray<FKismetAIAssetFingerprint> CaptureAssetStates(const TArray<FString>& PackagePaths);
	// Before / after an agent run's mutating call: capture the targets, keep
	// the run's checkpoint of each and record the run's latest state.
	TArray<FKismetAIAssetFingerprint> BeginAgentMutation(const FKismetAIMutationContext& Context);
	void EndAgentMutation(const FKismetAIMutationContext& Context);
	// The run that just ended (RunId): note changes made after its last call,
	// mark the diff, record the marked state as the run's result, and offer
	// Reject when any checkpointed asset changed even if no graph node did. Only
	// that run's checkpoints are touched; an earlier run's chain is never
	// advanced over edits made since.
	void FinalizeRunDiff(const FString& RunId);
	// Restore (or, with bReapply, put back the result of) every asset the run
	// checkpointed. An empty RunId means the session's newest pending run. A
	// session with no checkpoint yields one checkpoint_unavailable report.
	TArray<FKismetAIRestoreReport> RestoreRunCheckpoints(const FString& SessionId, const FString& RunId, bool bReapply);
	FKismetAICheckpointStore& GetCheckpointStore() { return *Checkpoints; }
	// Test seam: checkpoints go under Root instead of
	// <Saved>/KismetAI/checkpoints; empty restores the default. Game thread,
	// with no run active. Installed per test by Tests/KismetAITestSandbox.cpp.
	KISMETAI_API void SetCheckpointRootForTests(const FString& Root);
	// The run whose checkpoints the session's pending diff covers.
	FString GetDiffRunId(const FString& SessionId) const;
	bool HasPendingDiff() const;
	bool CanReapply() const;
	void PushDiffStateToPanel(SKismetAIPanel* Panel);


	// Strip T3D boilerplate from exported text (public for testing)
	KISMETAI_API FString StripT3D(const FString& RawT3D);

	// Resolve GUID placeholders (LINK_*, GUID_*) in T3D text (public for testing)
	KISMETAI_API FString ResolvePlaceholders(const FString& T3DWithPlaceholders);

	// Validate resolved T3D before it reaches FEdGraphUtilities::ImportNodesFromText.
	// A malformed CustomProperties Pin record (missing PinName, invalid/missing PinId,
	// unbalanced parens, a LinkedTo target that resolves to no pin, or a residual
	// LINK_/GUID_/NODEREF_/ID_ placeholder) makes UEdGraphPin::ImportTextItem append a
	// nullptr pin or dangle a pin pointer, crashing the editor (uncatchable on macOS).
	// Returns false and fills OutError with an actionable message; true if safe to import.
	// Public for testing. Graph may be null (LinkedTo targets then resolve in-buffer only).
	KISMETAI_API bool ValidateT3DForImport(const FString& ResolvedT3D, UEdGraph* Graph, FString& OutError);

	/**
	 * Core asset query shared by HandleSearchAssets (HTTP) and UKismetAIBridge::SearchAssets (JS bridge).
	 *
	 * Scans the Asset Registry for /Game assets whose name contains Query (case-insensitive).
	 * TypeFilter maps to the same friendly class names as search_assets class_filter
	 * (Blueprint, Material, Texture2D, …); empty string = all classes.
	 * Unknown TypeFilter produces an empty result (no crash).
	 * Results are sorted by name and capped at Max entries.
	 * No assets are loaded — only FAssetData tag reads.
	 * Must be called on the game thread.
	 */
	KISMETAI_API static void QueryAssets(const FString& Query, const FString& TypeFilter, int32 Max, TArray<FAssetData>& Out);

	/**
	 * Format (auto-layout) a focused Blueprint graph.
	 * Wraps FKismetAILayout::FormatNodes in a single undoable transaction
	 * and marks the Blueprint as modified.
	 * Null graph → no-op + warning log.
	 */
	KISMETAI_API static void FormatFocusedGraph(UEdGraph* Graph);

	/**
	 * Pure formatting function — builds the <references> block from an already-resolved
	 * FAssetRef array. No asset registry access; unit-testable without a populated registry.
	 * Deduplicates by Path (case-insensitive). Returns "" when Refs is empty.
	 * Each line: "- <class-lowercased> <path>" with optional " (focused)" and " (<Summary>)".
	 * Missing refs: "- (missing) <path>".
	 * Wraps output in "<references>\n...\n</references>\n\n".
	 * Pure function — no side-effects, no registry access.
	 */
	KISMETAI_API static FString FormatReferencesBlock(const TArray<FAssetRef>& Refs);

	/**
	 * Resolves a JSON refs array (from UI @-mentions) into FAssetRef structs.
	 * Parses [{path,name,class,focused},...]; looks each up in the Asset Registry;
	 * sets bMissing=true when not found; fills Summary from FAssetData tags (no load).
	 * Must be called on the game thread.
	 * On malformed JSON: treats as empty (no crash), Out remains empty.
	 */
	KISMETAI_API static void ResolveRefs(const FString& RefsJson, TArray<FAssetRef>& Out);

	/**
	 * Combines ResolveRefs + FormatReferencesBlock. Also out-params the Blueprint-class
	 * ref paths for snapshotting. Returns the formatted block string.
	 * Must be called on the game thread.
	 */
	KISMETAI_API static FString BuildReferencesBlock(const FString& RefsJson, TArray<FString>& OutBlueprintPaths);

	// Execute a tool handler directly (no HTTP round-trip).
	// Builds a fake FHttpServerRequest from the JSON body, calls the handler,
	// and captures the response string from the callback.
	// Thread-safe: if called from a background thread, bounces to game thread automatically.
	KISMETAI_API FString ExecuteHandlerDirect(
		bool (FKismetAIModule::*Handler)(const FHttpServerRequest&, const FHttpResultCallback&),
		TSharedPtr<FJsonObject> Body);

	// --- Route handlers (public so agent can call them directly) ---
	bool HandleExportT3D(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleInjectT3D(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	KISMETAI_API bool HandleCompile(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	// --- inverted tool handlers ---
	// The work, decoupled from HTTP. The Handle* route above is a thin adapter;
	// the agent calls these directly. Migrating the remaining tools here is what
	// removes ExecuteHandlerDirect's synthetic request entirely.
	KISMETAI_API FToolExecutionResult DoCompileBlueprint(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoReadLogs(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoReadSource(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoSearchAssets(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoGetReferences(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoInspectClass(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoSearchNodes(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoDescribeMaterial(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoMaterialOverview(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoCompileMaterial(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoCreateMaterialExpression(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoEditMaterial(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoCreateMaterialInstance(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoSetMaterialInstanceParameter(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoManageMaterial(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoExportT3D(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoInjectT3D(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoDescribeGraph(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoEditGraph(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoLayoutGraph(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoBlueprintOverview(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoManageBlueprint(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoManageComponents(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoManageStateMachine(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoManageAnimation(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoCreateAsset(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoManageAsset(const TSharedPtr<FJsonObject>& Args);
	KISMETAI_API FToolExecutionResult DoInsertNode(const TSharedPtr<FJsonObject>& Args);

	// Run a typed handler on the game thread (UObject access) and return its
	// result. The typed counterpart of ExecuteHandlerDirect's bounce, for
	// callers with no cancellation (a never-cancelled token).
	//
	// This is where the game-thread admission seam lives (KismetAIToolExecution.h):
	// on the game thread, in order, the editor-state guard, then Admit, then
	// Work. A result from either of the first two is returned and Work never
	// runs. Admit may be null.
	KISMETAI_API FToolExecutionResult RunOnGameThread(TFunction<FToolExecutionResult()> Work,
		TFunction<TOptional<FToolExecutionResult>()> Admit = nullptr);

	// The same, carrying the call's cancellation into the queued work.
	// Work and Admit are taken by value and owned by the queued job, so the
	// caller may be released before the game thread runs anything. On the game
	// thread, in order: Token check at callback entry, editor-state guard,
	// Admit (which re-checks the token and runs the policy's admission), Work.
	// A Token cancelled before Work starts returns CancelledBeforeStart with
	// bStoppedBeforeStart and Work never runs; see KismetAIGameThreadDispatcher.h.
	KISMETAI_API FKismetAIDispatchOutcome DispatchToGameThread(TFunction<FToolExecutionResult()> Work,
		TFunction<TOptional<FToolExecutionResult>()> Admit, const FKismetAICancellationToken& Token,
		const FToolExecutionResult& CancelledBeforeStart);

	// The editor-state guard on its own: set when object lookup or
	// mutation is not allowed right now — off the game thread, while a package
	// is being serialized, during garbage collection, or inside a slow task.
	// Saves and slow tasks pump the game thread's task queue (progress UI,
	// render sync), so any queued game-thread callback can find itself inside
	// one, and FindPackage/StaticLoadObject there is an engine Fatal ("Illegal
	// call to StaticFindObjectFast() while serializing object data!"). Every
	// game-thread callback that touches UObjects checks this first.
	KISMETAI_API static TOptional<FToolExecutionResult> GetEditorObjectAccessError();

	// Run Work on the game thread once object access is allowed: now if it is,
	// otherwise on the first core-ticker tick where it is. For callbacks that
	// must still happen (run finalization); tool work is refused with
	// EDITOR_BUSY instead. Game thread only. Work still pending at shutdown is
	// run by ShutdownModule after Lifeline is dropped, so its own lifetime
	// checks see the module going away and skip the module's state.
	void RunWhenObjectAccessAllowed(TFunction<void()> Work);

	// What an invoker shares with the module: the dispatcher and the
	// policy slot, by reference count. A worker reaches the editor only
	// through these and through bodies the dispatcher runs on the game thread
	// while the module is open.
	TSharedRef<FKismetAIGameThreadDispatcher, ESPMode::ThreadSafe> GetGameThreadDispatcher() const { return GameThreadDispatcher; }
	TSharedRef<FKismetAIToolPolicySlot, ESPMode::ThreadSafe> GetToolPolicySlot() const { return ToolPolicySlot; }

	// The game-thread body DispatchToGameThread queues. Static, so building it
	// on a worker reads no module memory; Module is only dereferenced when the
	// dispatcher runs the body on the game thread, which it never does after
	// Close (ShutdownModule).
	static FKismetAIGameThreadDispatcher::FBody MakeAdmittedWorkBody(FKismetAIModule& Module,
		TFunction<FToolExecutionResult()> Work, TFunction<TOptional<FToolExecutionResult>()> Admit,
		const FKismetAICancellationToken& Token, const FToolExecutionResult& CancelledBeforeStart);

	// Tests only: replace how the dispatcher posts work to the game
	// thread, so a test can hold one callback pending without blocking the game
	// thread, cancel, then release it. Pass nullptr to restore AsyncTask.
	KISMETAI_API void SetGameThreadSchedulerForTests(FKismetAIGameThreadDispatcher::FScheduler InScheduler);
	KISMETAI_API FKismetAIGameThreadDispatcher::FStats GetGameThreadDispatchStatsForTests() const;
	bool HandleDescribeGraph(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	KISMETAI_API bool HandleEditGraph(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleLayoutGraph(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	// --- Animation Blueprint helpers ---
	/** If Graph is an animation graph kind (UAnimationGraph / state-machine / state / transition /
	 *  conduit / custom-transition), render a compact pose-tree or state-machine description into
	 *  OutDescription and return true. Returns false for non-anim graphs (caller falls back to the
	 *  standard exec-flow describe). */
	bool DescribeAnimGraph(UBlueprint* Blueprint, UEdGraph* Graph, FString& OutDescription);

	/** Append the Animation-Blueprint-specific overview section (target skeleton + graph hierarchy:
	 *  state machines, states, transitions, conduits, aliases) to a read_blueprint overview. No-op for
	 *  non-anim Blueprints. */
	void AppendAnimOverview(UBlueprint* Blueprint, FString& Description);
	bool HandleDescribeMaterial(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleMaterialOverview(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleCompileMaterial(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleCreateMaterialExpression(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleEditMaterial(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleCreateMaterialInstance(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleSetMaterialInstanceParameter(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleManageMaterial(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleDescribeWidget(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleWidgetOverview(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleEditWidget(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleManageBlueprint(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	// KISMETAI_API: driven directly by the KismetAIEval fixture generator (component-anim fixtures).
	KISMETAI_API bool HandleManageComponents(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	// Author Animation Blueprint state machines (add SM/states/conduits/aliases/transitions, set
	// transition props, set entry, rename/delete). Anim BPs only. Batch-op, graph-addressed.
	KISMETAI_API bool HandleManageStateMachine(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleSearchAssets(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleGetReferences(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleInspectClass(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	KISMETAI_API bool HandleCreateAsset(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleManageAsset(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	// Edit/read animation ASSETS (montages, composites, sequences, blend spaces): slot tracks,
	// segments, sections, sync group, notifies, blend-space axes/samples. Batch-op + 'get' read.
	KISMETAI_API bool HandleManageAnimation(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleSearchNodes(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	KISMETAI_API bool HandleInsertNode(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	FKismetAISourceJobs& GetSourceJobs() const { return *SourceJobs; }
	bool HandleCancelSource(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleReadSource(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleReadLogs(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	// Returns ~/.kismet/ if it exists, else ~/.kismetai/ (legacy) if it exists, else ~/.kismet/.
	static FString GetConfigDir();

private:
	friend class FKismetAIHttpStartupRetryTest;
	friend class FEditorSafety_SaveRejectsHttpWork;
	// Drives HandleAgentRun and the module's controller.
	friend class FSessionLifecycle_HttpRunRefusedWhileLeaseHeld;
	friend class FSessionLifecycle_PanelMessageQueuedWhileHttpHoldsLease;
	// Drive the external run routes, the panel launch path and
	// the agent factory seam.
	friend struct FKismetAIExternalRunTestAccess;

	// HTTP server
	void StartHttpServer();
	void StopHttpServer();

	TSharedPtr<FKismetAIHttpTransport> HttpRouter;
	TArray<FHttpRouteHandle> RouteHandles;
	uint32 ServerPort = 8323;
	bool bIsRunning = false;
	FTSTicker::FDelegateHandle HttpStartupRetryHandle;
	double HttpStartupRetryDeadline = 0.0;

	bool HandleSkills(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleStatus(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	// POST /api/v1/agent/run — compatibility route: submits a run (or attaches
	// to the one a repeated client_request_id names) and streams its events as
	// they happen. The run is the same resource as POST /api/v1/agent/runs.
	bool HandleAgentRun(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleAgentAnswer(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	// --- External runs ---
	// POST /api/v1/agent/runs: submit, answers 202 with the run id at once.
	bool HandleAgentRunSubmit(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	// GET /api/v1/agent/runs/status?run_id=|client_request_id=
	bool HandleAgentRunStatus(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	// GET /api/v1/agent/runs/events?run_id=&after=&limit=[&stream=1]
	bool HandleAgentRunEvents(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	// POST /api/v1/agent/runs/cancel {run_id}
	bool HandleAgentRunCancel(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	struct FExternalSubmit
	{
		TSharedPtr<FKismetAIExternalRunRecord, ESPMode::ThreadSafe> Record;
		bool bExisting = false;       // a repeated client_request_id: nothing was started
		int32 ErrorStatus = 0;        // non-zero: refused, nothing was started
		FString ErrorCode;
		FString ErrorMessage;
	};
	// Validate, admit and start one external run on a controller worker.
	// Game thread. Returns at once; the run continues on its worker.
	FExternalSubmit SubmitExternalRun(const FHttpServerRequest& Request);
	// Answer with a live SSE stream of Record's events after AfterSeq, until
	// its terminal outcome. Game thread.
	void StreamExternalRunEvents(const TSharedRef<FKismetAIExternalRunRecord, ESPMode::ThreadSafe>& Record,
		int64 AfterSeq, const FHttpResultCallback& OnComplete);

	// Created in StartupModule, with the session controller.
	TSharedPtr<FKismetAIExternalRunRegistry, ESPMode::ThreadSafe> ExternalRuns;

	// Test seam: build the agent a run uses (a scripted model client in tests),
	// for both run entry points: /agent/runs and /agent/run, and the panel's
	// LaunchAgent. Everything else on those paths — admission, caller
	// context, grants, the worker, finalization — stays production. When set,
	// the provider key gate is skipped.
	TFunction<TSharedRef<FKismetAIAgent, ESPMode::ThreadSafe>(const FKismetAIAgentConfig&)> AgentFactoryForTests;
	// Return a session's full canonical JSONL record (LoadAllEntries) so callers
	// — the eval solver in particular — can collect the whole conversation
	// (both sides + errors + meta) regardless of where the Saved dir lives.
	bool HandleGetSession(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	TSharedPtr<FKismetAISourceJobs> SourceJobs;

	// Agent config (populated from env vars on startup)
	FKismetAIAgentConfig AgentConfig;

	// Shared tool policy (see GetToolPolicy and FKismetAIToolPolicySlot).
	TSharedRef<FKismetAIToolPolicySlot, ESPMode::ThreadSafe> ToolPolicySlot =
		MakeShared<FKismetAIToolPolicySlot, ESPMode::ThreadSafe>();

	// Every worker-thread tool call reaches the game thread through this
	//. Closed first in ShutdownModule. Created in the constructor so it
	// exists before StartupModule and after ShutdownModule.
	TSharedRef<FKismetAIGameThreadDispatcher, ESPMode::ThreadSafe> GameThreadDispatcher =
		MakeShared<FKismetAIGameThreadDispatcher, ESPMode::ThreadSafe>();

	// The game-thread side of DispatchToGameThread.
	FKismetAIDispatchOutcome RunAdmittedWork(const TFunction<FToolExecutionResult()>& Work,
		const TFunction<TOptional<FToolExecutionResult>()>& Admit, const FKismetAICancellationToken& Token,
		const FToolExecutionResult& CancelledBeforeStart);

	// --- UI registration ---
	void RegisterTabSpawner();
	void UnregisterTabSpawner();
	TSharedRef<SDockTab> SpawnKismetAITab(const FSpawnTabArgs& Args);
	void RegisterToolbarButton();

	// Blueprint Editor integration — docked tab
	void RegisterBlueprintEditorExtension();
	void HandleRegisterBlueprintEditorTabs(FWorkflowAllowedTabSet& TabSet, FName ModeName, TSharedPtr<FBlueprintEditor> BlueprintEditor);
	void HandleRegisterBlueprintEditorLayout(FLayoutExtender& Extender);
	FDelegateHandle BlueprintEditorTabsHandle;
	FDelegateHandle BlueprintEditorLayoutHandle;

	// Tab focus management — prevent Details tab from stealing focus during agent operations
	friend class FKismetAIBlueprintTabFactory;
	TWeakPtr<SDockTab> BlueprintEditorKismetTab;

	// --- Panel registry & agent state ---
	TArray<SKismetAIPanel*> RegisteredPanels;

	// The run lifecycle owner — admission/lease, workers, cancel, answers,
	// drain — plus the queue and session id: FKismetAISessionController
	// (Private/KismetAISessionController.h). The module keeps composition,
	// routes, UI and diff state.
	TUniquePtr<FKismetAISessionController> Session;

	// Start the next queued panel message if the lease is free (after any run
	// releases it, panel or HTTP). Game thread.
	void LaunchNextQueuedMessage();



	// Lifetime sentinel for thread-pool callbacks. The agent worker captures
	// a TWeakPtr to this; its game-thread AsyncTask continuations bail out if
	// the pin fails. Reset first thing in ShutdownModule so any continuation
	// queued after we start tearing down no-ops instead of dereferencing the
	// dead module.
	TSharedPtr<uint8> Lifeline;

	// RunWhenObjectAccessAllowed's pending work, keyed by its ticker.
	TMap<uint64, TPair<FTSTicker::FDelegateHandle, TFunction<void()>>> DeferredObjectWork;
	uint64 NextDeferredObjectWorkId = 0;

	// --- Blueprint diff state (per-session) ---
	struct FPinSnapshot
	{
		FString PinName;
		FString DefaultValue;
	};

	struct FNodeSnapshot
	{
		FString NodeName;
		FGuid NodeGuid;
		TArray<FPinSnapshot> PinDefaults;      // Pin default values at snapshot time
	};

	// Per-blueprint baseline for a Blueprint other than the primary one in a
	// session. Only the baseline is kept: it drives IsAgentCreatedNode.
	struct FBlueprintDiffEntry
	{
		FString BlueprintPath;
		FString GraphName;
		TArray<FNodeSnapshot> BaselineNodes;
	};

	struct FDiffState
	{
		// Legacy single-blueprint accessors (delegates to first/active entry)
		FString BlueprintPath;
		FString GraphName;
		TArray<FNodeSnapshot> BaselineNodes;   // Snapshot before agent ran
		TArray<FString> AddedNodeNames;        // Names of nodes added by agent
		TArray<FGuid> AddedNodeGuids;          // GUIDs of nodes added by agent
		TArray<FGuid> ModifiedNodeGuids;       // GUIDs of nodes with changed pin defaults
		bool bHasPendingDiff = false;
		bool bCanReapply = false;              // True after a verified restore kept the run's result
		// The agent run whose checkpoints Reject/Reapply use. Set by the
		// run's first mutating call.
		FString RunId;

		// Multi-blueprint tracking: additional blueprints modified in this session
		TMap<FString, FBlueprintDiffEntry> AdditionalBlueprints;

		// Reposition-on-first-wire tracking. When insert_node / inject_t3d places a
		// node with NO anchor (an island, off to the right), we record its GUID and
		// the exact position it was parked at. When that node is first wired by an
		// edit_blueprint connect, and it is still sitting at that recorded spot, it is
		// safe to pull it next to its new peer. One-shot: the entry is cleared the
		// moment the node is repositioned (or wired). Only agent-created island nodes
		// land here, so user/pre-session nodes are never eligible to move.
		TMap<FGuid, FIntPoint> UnanchoredSpawnPositions;
	};

	TMap<FString, FDiffState> DiffStates;      // Keyed by session ID

	FDiffState& GetOrCreateDiffState(const FString& SessionId);
	FDiffState* GetActiveDiffState();

	// Fire an editor-side "generation complete" notification when an agent run
	// finishes: in-editor toast + Dock/taskbar attention + sound. Cross-platform
	// (Slate/UnrealEd APIs, works on Mac and Windows). Status is the "done" event
	// status — "completed", "max_steps", or "aborted"; aborted is silent.
	void NotifyAgentRunComplete(const FString& Status);

	void TakeGraphSnapshot(const FString& SessionId, const FString& BlueprintPath, const FString& GraphName);


	// --- Reposition-on-first-wire support ---
	// Record island (unanchored) nodes the layout just parked, so a later connect
	// can pull them next to their peer. No-op when there is no active session.
	void RecordUnanchoredSpawns(const FString& BlueprintPath, const TSet<UEdGraphNode*>& IslandNodes);
	// True if Node is not in the active session's baseline for OwnerBlueprint
	// (i.e. the agent created it during this session). False with no active session.
	bool IsAgentCreatedNode(const FString& BlueprintPath, const UEdGraphNode* Node) const;
	// If WiredNode is an agent-created island still sitting at its recorded spawn
	// position, reposition it next to PeerNode and clear the mark (one-shot).
	// Always clears any mark for WiredNode once it has been wired. No-op without a
	// session / when WiredNode isn't a tracked island.
	void MaybeRepositionOnFirstWire(UEdGraph* Graph, const FString& BlueprintPath,
		UEdGraphNode* WiredNode, UEdGraphNode* PeerNode);
	void RefreshDiffedGraphNodes();
	void SyncFactoryWithDiffState();           // Repopulate factory GUIDs from active session
	void SaveDiffStates();
	void LoadDiffStates();
	void RecoverPendingCheckpoints();

	// Per-run checkpoints. Created in StartupModule.
	TUniquePtr<FKismetAICheckpointStore> Checkpoints;
	void BroadcastCheckpointReports(const TArray<FKismetAIRestoreReport>& Reports);

	// Node factory for diff tinting (registered globally)
	TSharedPtr<class FKismetAIDiffNodeFactory> DiffNodeFactory;

	// --- Settings persistence (~/.kismet/ with fallback to legacy ~/.kismetai/) ---
	void LoadSettings();
	static FString GetSettingsPath();

	// --- Helpers ---

	// Send JSON success response
	void SendSuccess(const FHttpResultCallback& OnComplete, TSharedPtr<FJsonObject> Data);

	// Send JSON error response
	void SendError(const FHttpResultCallback& OnComplete, int32 StatusCode, const FString& ErrorCode, const FString& Message, const TArray<FString>& Suggestions = {});

	// Parse JSON body from request
	TSharedPtr<FJsonObject> ParseRequestBody(const FHttpServerRequest& Request);

	// Fill in incomplete K2Node_MacroInstance references (MacroGraphReference) before import.
	// The LLM can only supply the macro graph name; GraphBlueprint + GraphGuid are unguessable,
	// and a macro instance with a null GraphBlueprint is silently dropped by the import factory.
	// Loads the StandardMacros libraries, matches the macro graph by name, and rewrites the
	// MacroGraphReference in-place. Returns the number of macro references completed.
	int32 CompleteMacroReferences(FString& InOutT3D);

	// Sanitize imported nodes: remove orphaned LinkedTo references, fix bidirectional links
	void SanitizeImportedNodes(const TSet<UEdGraphNode*>& ImportedNodes);

	// Wire connections from imported nodes to existing nodes in the graph.
	// Parses the resolved T3D for LinkedTo references that point to existing (non-imported) nodes.
	void WireExistingNodeConnections(UEdGraph* Graph, const TSet<UEdGraphNode*>& ImportedNodes, const FString& ResolvedT3D);
};
