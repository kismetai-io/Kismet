// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// The one tool-call lifecycle.
//
// KismetAI.AgentLoop already covers these steps end to end through RunTask.
// These cover the ordering directly — specifically that each step refuses
// *before* the next one can do anything, which is the property that makes the
// order meaningful rather than incidental.

#include "KismetAIToolExecutor.h"
#include "KismetAIToolInvoker.h"
#include "Misc/AutomationTest.h"

constexpr auto ExecFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	// Records whether the editor was reached at all.
	class FRecordingInvoker final : public IKismetAIToolInvoker
	{
	public:
		virtual FKismetAIInvokeResult Invoke(const FKismetAIToolInvocation& Invocation) override
		{
			if (OnBeforeAdmission) { OnBeforeAdmission(); }
			if (const TOptional<FToolExecutionResult> Refused = Invocation.RunAdmission())
			{
				++Refusals;
				return FKismetAIInvokeResult::FromTyped(Refused.GetValue());
			}
			++Calls;
			LastMode = Invocation.Mode;
			LastCaller = Invocation.Caller;
			return FKismetAIInvokeResult::FromModelString(Canned);
		}
		virtual const IKismetAIToolPolicy* GetPolicy() const override { return InjectedPolicy; }
		virtual TSet<FString> GetDispatchableToolNames() const override { return {}; }
		virtual FString ResolveHandlerName(const FString&, const FString&) const override { return {}; }

		int32 Calls = 0;
		int32 Refusals = 0;
		FString LastMode;
		FKismetAICallerContext LastCaller;
		// Runs after the executor's pre-dispatch checks, before admission:
		// the window in which a queued call waits for the game thread.
		TFunction<void()> OnBeforeAdmission;
		const IKismetAIToolPolicy* InjectedPolicy = nullptr;
		FString Canned = TEXT("{\"success\":true}");
	};

	TSharedPtr<FJsonObject> ExecArgs(const TCHAR* Path, const TCHAR* Mode = nullptr)
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("blueprint"), Path);
		if (Mode) { A->SetStringField(TEXT("mode"), Mode); }
		return A;
	}

	FToolCallContext MakeCtx(const TCHAR* Tool, TSharedPtr<FJsonObject> Args, bool bPlanMode = false)
	{
		FToolCallContext C;
		C.ToolName = Tool;
		C.Caller = FKismetAICallerContext::InProcess();
		C.Arguments = Args;
		C.RequestBody = Args;
		C.bPlanModeActive = bPlanMode;
		C.DefaultMaxResultChars = 30000;
		return C;
	}
}

// Step 1 refuses before step 4: a mutation in plan mode must not reach the editor.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExec_PlanModeRefusesBeforeDispatch, "KismetAI.ToolExecutor.PlanModeRefusesBeforeDispatch", ExecFlags)
bool FExec_PlanModeRefusesBeforeDispatch::RunTest(const FString& Parameters)
{
	FRecordingInvoker Invoker;
	FKismetAIToolExecutor Executor(Invoker);

	const FToolCallOutcome Out = Executor.Execute(
		MakeCtx(TEXT("edit_blueprint"), ExecArgs(TEXT("/Game/BP")), /*bPlanMode=*/true));

	TestTrue(TEXT("refused"), Out.bIsError);
	TestTrue(TEXT("with a plan-mode error code"), Out.ResultForModel.Contains(TEXT("PLAN_MODE_READONLY")));
	TestEqual(TEXT("and the editor was never reached"), Invoker.Calls, 0);
	return true;
}

// A read is permitted in plan mode and does reach the editor.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExec_PlanModeAllowsReads, "KismetAI.ToolExecutor.PlanModeAllowsReads", ExecFlags)
bool FExec_PlanModeAllowsReads::RunTest(const FString& Parameters)
{
	FRecordingInvoker Invoker;
	FKismetAIToolExecutor Executor(Invoker);

	const FToolCallOutcome Out = Executor.Execute(
		MakeCtx(TEXT("read_blueprint"), ExecArgs(TEXT("/Game/BP")), /*bPlanMode=*/true));

	TestFalse(TEXT("not refused"), Out.bIsError);
	TestEqual(TEXT("dispatched once"), Invoker.Calls, 1);
	return true;
}

// Step 2 refuses before step 4: malformed arguments must not reach the editor.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExec_ValidationRefusesBeforeDispatch, "KismetAI.ToolExecutor.ValidationRefusesBeforeDispatch", ExecFlags)
bool FExec_ValidationRefusesBeforeDispatch::RunTest(const FString& Parameters)
{
	FRecordingInvoker Invoker;
	FKismetAIToolExecutor Executor(Invoker);

	// read_blueprint requires a non-empty 'blueprint'.
	const FToolCallOutcome Out = Executor.Execute(
		MakeCtx(TEXT("read_blueprint"), MakeShared<FJsonObject>()));

	TestTrue(TEXT("refused"), Out.bIsError);
	TestTrue(TEXT("as a structured validation error"),
		Out.ResultForModel.Contains(TEXT("<tool_use_error>")) && Out.ResultForModel.Contains(TEXT("blueprint")));
	TestEqual(TEXT("and the editor was never reached"), Invoker.Calls, 0);
	return true;
}

// Policy runs before validation: a plan-mode refusal wins over a malformed call,
// so the model is told the real reason it cannot proceed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExec_PolicyPrecedesValidation, "KismetAI.ToolExecutor.PolicyPrecedesValidation", ExecFlags)
bool FExec_PolicyPrecedesValidation::RunTest(const FString& Parameters)
{
	FRecordingInvoker Invoker;
	FKismetAIToolExecutor Executor(Invoker);

	// Both wrong: a mutation in plan mode AND missing required arguments.
	const FToolCallOutcome Out = Executor.Execute(
		MakeCtx(TEXT("edit_blueprint"), MakeShared<FJsonObject>(), /*bPlanMode=*/true));

	TestTrue(TEXT("reports the plan-mode refusal, not the validation error"),
		Out.ResultForModel.Contains(TEXT("PLAN_MODE_READONLY")));
	return true;
}

// Step 5: a result larger than the tool's cap is cut, and the cap is the
// registry's, not the caller's default.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExec_TruncatesToRegistryCap, "KismetAI.ToolExecutor.TruncatesToRegistryCap", ExecFlags)
bool FExec_TruncatesToRegistryCap::RunTest(const FString& Parameters)
{
	FRecordingInvoker Invoker;
	// compile_blueprint's cap is 5000, well below the 30000 default.
	Invoker.Canned = FString::ChrN(20000, TEXT('x'));
	FKismetAIToolExecutor Executor(Invoker);

	const FToolCallOutcome Out = Executor.Execute(
		MakeCtx(TEXT("compile_blueprint"), ExecArgs(TEXT("/Game/BP"))));

	TestTrue(TEXT("was truncated"), Out.bWasTruncated);
	TestTrue(TEXT("to roughly the registry cap, not the default"), Out.ResultForModel.Len() < 6000);
	return true;
}

// read_blueprint's cap depends on its mode, and the mode also reaches dispatch.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExec_ModeDrivesCapAndDispatch, "KismetAI.ToolExecutor.ModeDrivesCapAndDispatch", ExecFlags)
bool FExec_ModeDrivesCapAndDispatch::RunTest(const FString& Parameters)
{
	FRecordingInvoker Invoker;
	Invoker.Canned = FString::ChrN(25000, TEXT('y'));
	FKismetAIToolExecutor Executor(Invoker);

	// describe caps at 15000.
	const FToolCallOutcome Describe = Executor.Execute(
		MakeCtx(TEXT("read_blueprint"), ExecArgs(TEXT("/Game/BP"), TEXT("describe"))));
	TestEqual(TEXT("mode reached dispatch"), Invoker.LastMode, FString(TEXT("describe")));
	TestTrue(TEXT("describe was truncated"), Describe.bWasTruncated);

	// t3d caps at 30000, so 25000 passes through whole.
	const FToolCallOutcome T3D = Executor.Execute(
		MakeCtx(TEXT("read_blueprint"), ExecArgs(TEXT("/Game/BP"), TEXT("t3d"))));
	TestEqual(TEXT("t3d mode reached dispatch"), Invoker.LastMode, FString(TEXT("t3d")));
	TestFalse(TEXT("t3d was not truncated"), T3D.bWasTruncated);
	return true;
}

// Structure-aware truncation: T3D is cut at a node boundary, not mid-token.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExec_TruncationIsStructureAware, "KismetAI.ToolExecutor.TruncationIsStructureAware", ExecFlags)
bool FExec_TruncationIsStructureAware::RunTest(const FString& Parameters)
{
	FString T3D;
	for (int32 i = 0; i < 200; ++i)
	{
		T3D += FString::Printf(
			TEXT("Begin Object Class=/Script/BlueprintGraph.K2Node_CallFunction Name=\"N%d\"\n   NodePosX=%d\nEnd Object\n"),
			i, i * 100);
	}

	bool bTruncated = false;
	const FString Cut = FKismetAIToolExecutor::TruncateResult(T3D, 2000, bTruncated);
	TestTrue(TEXT("was truncated"), bTruncated);
	TestTrue(TEXT("kept whole objects"),
		Cut.Contains(TEXT("Begin Object")) && Cut.Contains(TEXT("End Object")));

	// A result inside the cap is returned untouched.
	bool bShort = false;
	const FString Small = FKismetAIToolExecutor::TruncateResult(TEXT("tiny"), 2000, bShort);
	TestFalse(TEXT("short results are not touched"), bShort);
	TestEqual(TEXT("and come back intact"), Small, FString(TEXT("tiny")));
	return true;
}

// The registry decides what is a tool before anything else runs, so no
// transport can be routed to a dispatcher's default entry for a name nobody
// advertised. The model-facing code is unchanged (UNKNOWN_TOOL).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExec_UnknownToolRefusedBeforeDispatch, "KismetAI.ToolExecutor.UnknownToolRefusedBeforeDispatch", ExecFlags)
bool FExec_UnknownToolRefusedBeforeDispatch::RunTest(const FString& Parameters)
{
	FRecordingInvoker Invoker;
	FKismetAIToolExecutor Executor(Invoker);

	const FToolCallOutcome Unknown = Executor.Execute(MakeCtx(TEXT("no_such_tool"), MakeShared<FJsonObject>()));
	TestEqual(TEXT("unknown tool never reaches dispatch"), Invoker.Calls + Invoker.Refusals, 0);
	TestTrue(TEXT("reported as UNKNOWN_TOOL"), Unknown.ResultForModel.Contains(TEXT("UNKNOWN_TOOL")));
	TestEqual(TEXT("typed code"), Unknown.Typed.ErrorCode, FString(TEXT("UNKNOWN_TOOL")));
	TestEqual(TEXT("typed status"), Unknown.Typed.StatusCode, 404);

	// Agent-local tools act on one agent's run state; the shared service never
	// executes them, whoever asks.
	const FToolCallOutcome Local = Executor.Execute(MakeCtx(TEXT("enter_plan_mode"), MakeShared<FJsonObject>()));
	TestEqual(TEXT("agent-local tool never reaches dispatch"), Invoker.Calls + Invoker.Refusals, 0);
	TestEqual(TEXT("agent-local tool is UNKNOWN_TOOL here"), Local.Typed.ErrorCode, FString(TEXT("UNKNOWN_TOOL")));
	return true;
}

// An adapter that does not say whom it verified is refused before
// policy, validation or dispatch — the service does not invent an identity.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExec_UnverifiedCallerRefused, "KismetAI.ToolExecutor.UnverifiedCallerRefused", ExecFlags)
bool FExec_UnverifiedCallerRefused::RunTest(const FString& Parameters)
{
	FRecordingInvoker Invoker;
	FKismetAIToolExecutor Executor(Invoker);

	FToolCallContext Ctx = MakeCtx(TEXT("read_blueprint"), ExecArgs(TEXT("/Game/BP")));
	Ctx.Caller = FKismetAICallerContext();   // nobody said
	const FToolCallOutcome Out = Executor.Execute(Ctx);
	TestTrue(TEXT("refused"), Out.bIsError);
	TestEqual(TEXT("code"), Out.Typed.ErrorCode, FString(TEXT("CALLER_NOT_VERIFIED")));
	TestEqual(TEXT("status"), Out.Typed.StatusCode, 401);
	TestEqual(TEXT("never dispatched"), Invoker.Calls + Invoker.Refusals, 0);

	// A context with an entry point but no verification flag is also refused.
	FKismetAICallerContext Forged;
	Forged.EntryPoint = EKismetAIEntryPoint::Rest;
	Ctx.Caller = Forged;
	TestEqual(TEXT("forged context refused"), Executor.Execute(Ctx).Typed.ErrorCode, FString(TEXT("CALLER_NOT_VERIFIED")));

	// A verified one runs and reaches the invoker with the same identity.
	Ctx.Caller = FKismetAICallerContext::Verified(EKismetAIEntryPoint::Mcp, TEXT("mcp-bearer"));
	TestFalse(TEXT("verified caller runs"), Executor.Execute(Ctx).bIsError);
	TestEqual(TEXT("caller reached the invoker"), Invoker.LastCaller.EntryPoint, EKismetAIEntryPoint::Mcp);
	return true;
}

namespace
{
	// Records what policy saw at each of its two points, and refuses where told.
	class FProbePolicy final : public IKismetAIToolPolicy
	{
	public:
		virtual FPolicyVerdict Evaluate(const FPolicyContext& Context) const override
		{
			PreCalls++;
			LastPre = Context;
			return bDenyPre ? FPolicyVerdict::Deny(TEXT("pre-dispatch deny")) : FPolicyVerdict::Allow();
		}
		virtual FPolicyVerdict EvaluateAdmission(const FPolicyContext& Context) const override
		{
			AdmissionCalls++;
			LastAdmission = Context;
			return bDenyAdmission ? FPolicyVerdict::Deny(TEXT("admission deny"), TEXT("OUT_OF_SCOPE_DENIED")) : FPolicyVerdict::Allow();
		}
		bool bDenyPre = false;
		bool bDenyAdmission = false;
		mutable int32 PreCalls = 0;
		mutable int32 AdmissionCalls = 0;
		mutable FPolicyContext LastPre;
		mutable FPolicyContext LastAdmission;
	};
}

// The invoker's policy is the policy. This is how one module-owned
// policy reaches the panel, /agent/run, MCP and REST alike.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExec_InvokerPolicyGoverns, "KismetAI.ToolExecutor.InvokerPolicyGoverns", ExecFlags)
bool FExec_InvokerPolicyGoverns::RunTest(const FString& Parameters)
{
	FProbePolicy Policy;
	Policy.bDenyPre = true;
	FRecordingInvoker Invoker;
	Invoker.InjectedPolicy = &Policy;
	FKismetAIToolExecutor Executor(Invoker);

	FToolCallContext Ctx = MakeCtx(TEXT("edit_blueprint"), ExecArgs(TEXT("/Game/BP")));
	Ctx.Caller = FKismetAICallerContext::Verified(EKismetAIEntryPoint::Rest, TEXT("x-kismet-token"));
	const FToolCallOutcome Out = Executor.Execute(Ctx);
	TestTrue(TEXT("denied"), Out.bWasDenied);
	TestEqual(TEXT("deny code is the policy's"), Out.Typed.ErrorCode, FString(TEXT("POLICY_DENIED")));
	TestEqual(TEXT("403"), Out.Typed.StatusCode, 403);
	TestTrue(TEXT("model string is valid JSON inside the wrapper"), Out.ResultForModel.Contains(TEXT("\"error_code\":\"POLICY_DENIED\"")));
	TestEqual(TEXT("never dispatched"), Invoker.Calls + Invoker.Refusals, 0);
	TestEqual(TEXT("policy saw the caller"), Policy.LastPre.Caller.EntryPoint, EKismetAIEntryPoint::Rest);
	return true;
}

// Seam: cancellation that lands after the pre-dispatch checks — while
// the call waits for the game thread — is caught at admission, the handler
// never runs, and the outcome is the same CANCELLED result.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExec_AdmissionCatchesLateCancel, "KismetAI.ToolExecutor.AdmissionCatchesLateCancel", ExecFlags)
bool FExec_AdmissionCatchesLateCancel::RunTest(const FString& Parameters)
{
	FRecordingInvoker Invoker;
	FKismetAIToolExecutor Executor(Invoker);

	FToolCallContext Ctx = MakeCtx(TEXT("edit_blueprint"), MakeShared<FJsonObject>());
	TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("delete_node"));
	Op->SetStringField(TEXT("node"), TEXT("K2Node_0"));
	TArray<TSharedPtr<FJsonValue>> Ops = { MakeShared<FJsonValueObject>(Op) };
	Ctx.Arguments->SetStringField(TEXT("blueprint"), TEXT("/Game/BP"));
	Ctx.Arguments->SetArrayField(TEXT("operations"), Ops);
	Ctx.RequestBody = Ctx.Arguments;

	FKismetAICancellationToken Token = Ctx.CancelToken;
	Invoker.OnBeforeAdmission = [&Token]() { Token.Cancel(); };

	const FToolCallOutcome Out = Executor.Execute(Ctx);
	TestEqual(TEXT("handler never ran"), Invoker.Calls, 0);
	TestEqual(TEXT("refused at admission"), Invoker.Refusals, 1);
	TestTrue(TEXT("marked cancelled"), Out.bWasCancelled);
	TestEqual(TEXT("same code as a pre-dispatch cancel"), Out.Typed.ErrorCode, FString(TEXT("CANCELLED")));
	TestTrue(TEXT("model told it was cancelled"), Out.ResultForModel.Contains(TEXT("CANCELLED")));
	TestTrue(TEXT("model told nothing changed"), Out.ResultForModel.Contains(TEXT("Nothing was changed")));
	return true;
}

// Seam: the admission policy sees the caller, tool, mode and the
// normalized arguments, and its Deny stops the handler.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExec_AdmissionPolicyDenies, "KismetAI.ToolExecutor.AdmissionPolicyDenies", ExecFlags)
bool FExec_AdmissionPolicyDenies::RunTest(const FString& Parameters)
{
	FProbePolicy Policy;
	Policy.bDenyAdmission = true;
	FRecordingInvoker Invoker;
	Invoker.InjectedPolicy = &Policy;
	FKismetAIToolExecutor Executor(Invoker);

	FToolCallContext Ctx = MakeCtx(TEXT("read_blueprint"), ExecArgs(TEXT("/Game/BP"), TEXT("t3d")));
	Ctx.Caller = FKismetAICallerContext::Verified(EKismetAIEntryPoint::Panel, TEXT("editor-user"));
	const FToolCallOutcome Out = Executor.Execute(Ctx);

	TestEqual(TEXT("pre-dispatch policy ran once"), Policy.PreCalls, 1);
	TestEqual(TEXT("admission policy ran once"), Policy.AdmissionCalls, 1);
	TestEqual(TEXT("handler never ran"), Invoker.Calls, 0);
	TestTrue(TEXT("marked denied"), Out.bWasDenied);
	TestEqual(TEXT("policy's code"), Out.Typed.ErrorCode, FString(TEXT("OUT_OF_SCOPE_DENIED")));
	TestEqual(TEXT("admission saw the caller"), Policy.LastAdmission.Caller.EntryPoint, EKismetAIEntryPoint::Panel);
	TestEqual(TEXT("admission saw the mode"), Policy.LastAdmission.Mode, FString(TEXT("t3d")));
	TestTrue(TEXT("admission saw the arguments"), Policy.LastAdmission.Arguments.IsValid()
		&& Policy.LastAdmission.Arguments->GetStringField(TEXT("blueprint")) == TEXT("/Game/BP"));
	return true;
}

// The shared normalization: stringified arrays become arrays, in place.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExec_NormalizeArguments, "KismetAI.ToolExecutor.NormalizeArguments", ExecFlags)
bool FExec_NormalizeArguments::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("operations"), TEXT(" [{\"action\":\"delete_node\",\"node\":\"N\"}] "));
	Args->SetStringField(TEXT("parameters"), TEXT("[1,2]"));
	Args->SetStringField(TEXT("graph"), TEXT("[not touched]"));
	FKismetAIToolExecutor::NormalizeArguments(Args);

	const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
	TestTrue(TEXT("operations is an array"), Args->TryGetArrayField(TEXT("operations"), Ops) && Ops->Num() == 1);
	const TArray<TSharedPtr<FJsonValue>>* Params = nullptr;
	TestTrue(TEXT("parameters is an array"), Args->TryGetArrayField(TEXT("parameters"), Params) && Params->Num() == 2);
	TestEqual(TEXT("other string fields untouched"), Args->GetStringField(TEXT("graph")), FString(TEXT("[not touched]")));

	FKismetAIToolExecutor::NormalizeArguments(nullptr);   // must not crash
	return true;
}

// Validation refusals are typed for REST with the same code the model sees.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExec_ValidationIsTyped, "KismetAI.ToolExecutor.ValidationIsTyped", ExecFlags)
bool FExec_ValidationIsTyped::RunTest(const FString& Parameters)
{
	FRecordingInvoker Invoker;
	FKismetAIToolExecutor Executor(Invoker);
	const FToolCallOutcome Out = Executor.Execute(MakeCtx(TEXT("read_blueprint"), MakeShared<FJsonObject>()));
	TestEqual(TEXT("typed code"), Out.Typed.ErrorCode, FString(TEXT("VALIDATION_ERROR")));
	TestEqual(TEXT("typed status"), Out.Typed.StatusCode, 400);
	TestTrue(TEXT("field named in suggestions"), Out.Typed.Suggestions.Num() > 0 && Out.Typed.Suggestions[0].Contains(TEXT("blueprint")));
	TestTrue(TEXT("model sees the same code"), Out.ResultForModel.Contains(TEXT("VALIDATION_ERROR")));
	return true;
}
