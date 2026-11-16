// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIModuleToolInvoker.h"
#include "KismetAIModule.h"
#include "KismetAIWidgetService.h"
#include "KismetAIToolPolicy.h"
#include "KismetAIToolExecutor.h"
#include "KismetAISourceService.h"
#include "KismetAIReadRevision.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"

// ─────────────────────────────────────────────────────────────────────────────
// Tool dispatch
//
// One table, mapping a tool name to the function that does the work. The agent
// calls that function and reads FToolExecutionResult::bSuccess.
//
// It used to reach handlers by fabricating an FHttpServerRequest, running the
// HTTP route, capturing the response body and deciding whether the call failed
// by searching that body for the substring "success":false — so transport
// formatting sat in the middle of local execution and failure was a string
// convention. The HTTP routes are now thin adapters over these same
// functions.
//
// The table is also the answer to "which tools can actually run", which is what
// KismetAI.ToolManifest asserts against the shipped JSON schemas. That guard
// exists because layout_graph was advertised to the model for the life of the
// feature with no dispatch branch at all.
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	using FToolWorkFunc = TFunction<FToolExecutionResult(FKismetAIModule&, const TSharedPtr<FJsonObject>&)>;

	// Function plus its name, so tests can assert routing without running
	// handlers. TREF keeps the two from drifting apart.
	struct FToolWorkRef
	{
		FToolWorkFunc Fn = nullptr;
		const TCHAR* Name = nullptr;
	};

#define TREF(Method) FToolWorkRef{ [](FKismetAIModule& M, const TSharedPtr<FJsonObject>& Args) { return M.Method(Args); }, TEXT(#Method) }
#define WREF(Method) FToolWorkRef{ [](FKismetAIModule&, const TSharedPtr<FJsonObject>& Args) { return FKismetAIWidgetService::Method(Args); }, TEXT("FKismetAIWidgetService::") TEXT(#Method) }

	// Mode-routed reads expose one tool name and pick a function from their
	// "mode" argument. Defaults are the documented ones: read_blueprint and
	// read_material default to describe, read_widget to tree.
	struct FToolDispatchEntry
	{
		FToolWorkRef Default;
		bool bModeRouted = false;
		TMap<FString, FToolWorkRef> ByMode;
	};

	const TMap<FString, FToolDispatchEntry>& GetToolDispatchTable()
	{
		static const TMap<FString, FToolDispatchEntry> Table = []
		{
			TMap<FString, FToolDispatchEntry> T;

			auto Add = [&T](const TCHAR* ToolName, const FToolWorkRef& Ref)
			{
				FToolDispatchEntry E;
				E.Default = Ref;
				T.Add(FString(ToolName), MoveTemp(E));
			};

			auto AddModed = [&T](const TCHAR* ToolName, const FToolWorkRef& Fallback,
								 std::initializer_list<TPair<const TCHAR*, FToolWorkRef>> Modes)
			{
				FToolDispatchEntry E;
				E.Default = Fallback;
				E.bModeRouted = true;
				for (const TPair<const TCHAR*, FToolWorkRef>& M : Modes)
				{
					E.ByMode.Add(FString(M.Key), M.Value);
				}
				T.Add(FString(ToolName), MoveTemp(E));
			};

			// --- reads (mode-routed) ---
			AddModed(TEXT("read_blueprint"), TREF(DoDescribeGraph), {
				{ TEXT("t3d"),      TREF(DoExportT3D) },
				{ TEXT("overview"), TREF(DoBlueprintOverview) },
			});
			AddModed(TEXT("read_material"), TREF(DoDescribeMaterial), {
				{ TEXT("overview"), TREF(DoMaterialOverview) },
			});
			AddModed(TEXT("read_widget"), WREF(Describe), {
				{ TEXT("overview"), WREF(Overview) },
			});

			// --- blueprint ---
			Add(TEXT("write_blueprint"),       TREF(DoInjectT3D));
			Add(TEXT("compile_blueprint"),     TREF(DoCompileBlueprint));
			Add(TEXT("edit_blueprint"),        TREF(DoEditGraph));
			Add(TEXT("insert_node"),           TREF(DoInsertNode));
			Add(TEXT("layout_graph"),          TREF(DoLayoutGraph));
			Add(TEXT("manage_blueprint"),      TREF(DoManageBlueprint));
			Add(TEXT("manage_components"),     TREF(DoManageComponents));
			Add(TEXT("manage_state_machine"),  TREF(DoManageStateMachine));
			Add(TEXT("manage_animation"),      TREF(DoManageAnimation));

			// --- material ---
			Add(TEXT("compile_material"),                TREF(DoCompileMaterial));
			Add(TEXT("create_material_expression"),      TREF(DoCreateMaterialExpression));
			Add(TEXT("edit_material"),                   TREF(DoEditMaterial));
			Add(TEXT("create_material_instance"),        TREF(DoCreateMaterialInstance));
			Add(TEXT("set_material_instance_parameter"), TREF(DoSetMaterialInstanceParameter));
			Add(TEXT("manage_material"),                 TREF(DoManageMaterial));

			// --- widget ---
			Add(TEXT("edit_widget"), WREF(Edit));

			// --- project / discovery ---
			Add(TEXT("search_assets"),  TREF(DoSearchAssets));
			Add(TEXT("get_references"), TREF(DoGetReferences));
			Add(TEXT("inspect_class"),  TREF(DoInspectClass));
			Add(TEXT("create_asset"),   TREF(DoCreateAsset));
			Add(TEXT("manage_asset"),   TREF(DoManageAsset));
			Add(TEXT("search_nodes"),   TREF(DoSearchNodes));
			Add(TEXT("read_source"),    TREF(DoReadSource));
			Add(TEXT("read_logs"),      TREF(DoReadLogs));

			return T;
		}();
		return Table;
	}

#undef TREF
#undef WREF

	FKismetAIDispatchOutcome DispatchWithAdmissionRetry(
		const TSharedRef<FKismetAIGameThreadDispatcher, ESPMode::ThreadSafe>& Dispatcher,
		FKismetAIGameThreadDispatcher::FBody Body, const FKismetAICancellationToken& Token,
		const FToolExecutionResult& CancelledBeforeStart)
	{
		FKismetAIDispatchOutcome Outcome = Dispatcher->Dispatch(Body, Token, CancelledBeforeStart);
		if (IsInGameThread() || !Outcome.bEditorBusyBeforeAdmission || !Outcome.Result.bHandlerNotStarted)
		{
			return Outcome;
		}

		// A checkpoint's save can schedule validation that temporarily blocks the
		// next tool. Wait only on the worker, without weakening the editor guard.
		// This is an admission-retry budget from the first refusal, not a hard
		// deadline for queued dispatch or native work: those retain the dispatcher's
		// cancellation/Close lifetime contract. Late callbacks cannot admit work.
		const double Deadline = FPlatformTime::Seconds() + 5.0;
		const FKismetAIDispatchOutcome OriginalRefusal = Outcome;
		const auto Cancelled = [CancelledBeforeStart]()
		{
			FKismetAIDispatchOutcome Stopped;
			Stopped.Result = CancelledBeforeStart;
			Stopped.Result.bHandlerNotStarted = true;
			Stopped.bStoppedBeforeStart = true;
			return Stopped;
		};
		for (;;)
		{
			if (Token.IsCancelled()) return Cancelled();
			if (Dispatcher->IsClosed()) return Dispatcher->Dispatch(Body, Token, CancelledBeforeStart);
			const double Remaining = Deadline - FPlatformTime::Seconds();
			if (Remaining <= 0.0) return OriginalRefusal;
			FPlatformProcess::SleepNoStats(static_cast<float>(FMath::Min(0.05, Remaining)));
			if (Token.IsCancelled()) return Cancelled();
			if (Dispatcher->IsClosed()) return Dispatcher->Dispatch(Body, Token, CancelledBeforeStart);
			if (FPlatformTime::Seconds() >= Deadline) return OriginalRefusal;

			// Each attempt owns a fresh copy. Cancellation may release the worker
			// before a queued callback drains, so nothing borrows this stack.
			Outcome = Dispatcher->Dispatch([Body, Token, Deadline, OriginalRefusal, Cancelled]()
				{
					if (Token.IsCancelled()) return Cancelled();
					if (FPlatformTime::Seconds() >= Deadline) return OriginalRefusal;
					return Body(); // Rechecks editor state, live policy and read revision.
				}, Token, CancelledBeforeStart);
			if (!Outcome.bEditorBusyBeforeAdmission || !Outcome.Result.bHandlerNotStarted)
			{
				return Outcome;
			}
		}
	}

	// Resolve (tool, mode) to the function that will run. Mode may be empty.
	const FToolWorkRef* ResolveToolWork(const FString& ToolName, const FString& Mode)
	{
		const FToolDispatchEntry* Entry = GetToolDispatchTable().Find(ToolName);
		if (!Entry)
		{
			return nullptr;
		}
		if (Entry->bModeRouted && !Mode.IsEmpty())
		{
			if (const FToolWorkRef* Found = Entry->ByMode.Find(Mode))
			{
				return Found;
			}
		}
		return &Entry->Default;
	}
}

FKismetAIInvokeResult FKismetAIModuleToolInvoker::Invoke(const FKismetAIToolInvocation& Invocation)
{
	const FToolWorkRef* Work = ResolveToolWork(Invocation.ToolName, Invocation.Mode);
	if (!Work || !Work->Fn)
	{
		return FKismetAIInvokeResult::FromTyped(FToolExecutionResult::Error(404, TEXT("UNKNOWN_TOOL"), TEXT("Unknown tool")));
	}

	// Straight to the work, on the game thread. No synthetic request, no
	// response body to parse back, no string-matching for failure. The admission
	// seam runs there too, after the editor-state guard and before the handler.
	//
	// Everything handed to the dispatcher is owned — the handler pointer
	// and body by value, and a COPY of the admission function, never a
	// reference into Invocation (which lives on the caller's stack and is gone
	// once a cancelled worker has been released). The call's token goes with it
	// so it can be abandoned while queued and checked at callback entry.
	//
	// Built with the static body factory and handed to the invoker's own
	// dispatcher reference, so this worker-side code reads no module memory.
	FKismetAIModule& M = Module;
	const FToolWorkFunc Fn = Work->Fn;
	const TSharedPtr<FJsonObject> RequestBody = Invocation.RequestBody;
	const FToolExecutionResult CancelledBeforeStart = Invocation.CancelledBeforeStart.ErrorCode.IsEmpty()
		? FKismetAIToolExecutor::CancelledBeforeStartResult(Invocation.ToolName)
		: Invocation.CancelledBeforeStart;
	if (Invocation.ToolName == TEXT("read_source") && !IsInGameThread())
	{
		// Admission and plugin discovery stay on GT; the worker receives only
		// an owned snapshot, and keeps the executor alive until all work ends.
		const auto Snapshot = MakeShared<TOptional<FKismetAISourceSnapshot>, ESPMode::ThreadSafe>();
		const FKismetAIDispatchOutcome Prepared = DispatchWithAdmissionRetry(Dispatcher,
			FKismetAIModule::MakeAdmittedWorkBody(M,
				[Snapshot]()
				{
					*Snapshot = KismetAISourceService::Prepare();
					return FToolExecutionResult::Success();
				}, TFunction<TOptional<FToolExecutionResult>()>(Invocation.Admit),
				Invocation.CancelToken, CancelledBeforeStart), Invocation.CancelToken, CancelledBeforeStart);
		FKismetAIInvokeResult Result = FKismetAIInvokeResult::FromTyped(Prepared.Result);
		Result.bStoppedBeforeStart = Prepared.bStoppedBeforeStart;
		if (!Snapshot->IsSet()) return Result;
		Result = FKismetAIInvokeResult::FromTyped(KismetAISourceService::Execute(
			RequestBody, Snapshot->GetValue(), Invocation.CancelToken));
		Result.bCancelRequestedWhileRunning = Invocation.CancelToken.IsCancelled();
		return Result;
	}

	const FKismetAIDispatchOutcome Dispatched = DispatchWithAdmissionRetry(Dispatcher,
		FKismetAIModule::MakeAdmittedWorkBody(M,
			[&M, Fn, RequestBody, ToolName = Invocation.ToolName, ObservedRevisions = Invocation.ObservedAssetRevisions, bRequireRevision = Invocation.bRequireReadRevision]()
			{
				return KismetAIReadRevision::Run(ToolName, RequestBody, ObservedRevisions, bRequireRevision,
					[&M, Fn](const TSharedPtr<FJsonObject>& Body) { return Fn(M, Body); });
			},
			TFunction<TOptional<FToolExecutionResult>()>(Invocation.Admit),
			Invocation.CancelToken, CancelledBeforeStart),
		Invocation.CancelToken, CancelledBeforeStart);

	FKismetAIInvokeResult Result = FKismetAIInvokeResult::FromTyped(Dispatched.Result);
	Result.bStoppedBeforeStart = Dispatched.bStoppedBeforeStart;
	Result.bCancelRequestedWhileRunning = Dispatched.bCancelRequestedWhileRunning;
	return Result;
}

FKismetAIModuleToolInvoker::FKismetAIModuleToolInvoker(FKismetAIModule& InModule)
	: FKismetAIModuleToolInvoker(InModule, InModule.GetGameThreadDispatcher())
{
}

FKismetAIModuleToolInvoker::FKismetAIModuleToolInvoker(FKismetAIModule& InModule,
	TSharedRef<FKismetAIGameThreadDispatcher, ESPMode::ThreadSafe> InDispatcher)
	: Module(InModule)
	, Dispatcher(MoveTemp(InDispatcher))
	, PolicySlot(InModule.GetToolPolicySlot())
{
}

bool FKismetAIModuleToolInvoker::RunOnGameThread(TFunction<void(FKismetAIModule&)> Work)
{
	FKismetAIModule& M = Module;
	TSharedRef<bool, ESPMode::ThreadSafe> bRan = MakeShared<bool, ESPMode::ThreadSafe>(false);
	TSharedRef<bool, ESPMode::ThreadSafe> bBusy = MakeShared<bool, ESPMode::ThreadSafe>(false);
	const FKismetAICancellationToken NeverCancelled;
	// The body is a queued game-thread task, and saves and slow tasks
	// pump that queue. Capturing there (FindPackage/LoadPackage under the
	// saving flag) was the engine Fatal "Illegal call to StaticFindObjectFast()
	// while serializing object data!". A busy editor is waited out, not
	// skipped: a late checkpoint is still a checkpoint. The wait is on this
	// worker, re-dispatching after a short sleep, so nothing spins inside the
	// save's own pump; dispatcher Close (shutdown) ends it.
	for (;;)
	{
		*bBusy = false;
		Dispatcher->Dispatch([&M, Work, bRan, bBusy]()
			{
				if (FKismetAIModule::GetEditorObjectAccessError().IsSet())
				{
					*bBusy = true;
					return FKismetAIDispatchOutcome();
				}
				Work(M);
				*bRan = true;
				return FKismetAIDispatchOutcome();
			},
			NeverCancelled, FToolExecutionResult::Error(503, TEXT("EDITOR_UNAVAILABLE"), TEXT("The editor is shutting down")));
		// On the game thread the body ran inline: waiting here would block the
		// very thread that has to finish the save.
		if (!*bBusy || IsInGameThread() || Dispatcher->IsClosed())
		{
			break;
		}
		FPlatformProcess::SleepNoStats(0.05f);
	}
	return *bRan;
}

namespace
{
	TArray<FKismetAIAssetFingerprint> NotCaptured(const TArray<FString>& Paths)
	{
		TArray<FKismetAIAssetFingerprint> Out;
		for (const FString& Path : Paths)
		{
			FKismetAIAssetFingerprint F;
			F.AssetPath = Path;
			F.UnsupportedReason = TEXT("the editor did not run the capture (shutting down, or called on the game thread while it was saving, collecting garbage or in a progress task)");
			Out.Add(MoveTemp(F));
		}
		return Out;
	}
}

TArray<FKismetAIAssetFingerprint> FKismetAIModuleToolInvoker::CaptureAssetState(const TArray<FString>& PackagePaths)
{
	TSharedRef<TArray<FKismetAIAssetFingerprint>, ESPMode::ThreadSafe> Out = MakeShared<TArray<FKismetAIAssetFingerprint>, ESPMode::ThreadSafe>();
	if (!RunOnGameThread([Out, PackagePaths](FKismetAIModule& M) { *Out = M.CaptureAssetStates(PackagePaths); }))
	{
		return NotCaptured(PackagePaths);
	}
	return *Out;
}

TArray<FKismetAIAssetFingerprint> FKismetAIModuleToolInvoker::BeginMutation(const FKismetAIMutationContext& Context)
{
	TSharedRef<TArray<FKismetAIAssetFingerprint>, ESPMode::ThreadSafe> Out = MakeShared<TArray<FKismetAIAssetFingerprint>, ESPMode::ThreadSafe>();
	if (!RunOnGameThread([Out, Context](FKismetAIModule& M) { *Out = M.BeginAgentMutation(Context); }))
	{
		return NotCaptured(Context.Targets);
	}
	return *Out;
}

void FKismetAIModuleToolInvoker::EndMutation(const FKismetAIMutationContext& Context)
{
	RunOnGameThread([Context](FKismetAIModule& M) { M.EndAgentMutation(Context); });
}

const IKismetAIToolPolicy* FKismetAIModuleToolInvoker::GetPolicy() const
{
	return &PolicySlot->Get();
}

TSet<FString> FKismetAIModuleToolInvoker::GetDispatchableToolNames() const
{
	return DispatchableToolNames();
}

FString FKismetAIModuleToolInvoker::ResolveHandlerName(const FString& ToolName, const FString& Mode) const
{
	return HandlerNameFor(ToolName, Mode);
}

TSet<FString> FKismetAIModuleToolInvoker::DispatchableToolNames()
{
	TArray<FString> Keys;
	GetToolDispatchTable().GetKeys(Keys);
	return TSet<FString>(Keys);
}

FString FKismetAIModuleToolInvoker::HandlerNameFor(const FString& ToolName, const FString& Mode)
{
	const FToolWorkRef* Ref = ResolveToolWork(ToolName, Mode);
	return (Ref && Ref->Name) ? FString(Ref->Name) : FString();
}
