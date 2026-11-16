// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIToolExecutor.h"
#include "KismetAIToolInvoker.h"
#include "KismetAIToolRegistry.h"
#include "KismetAIValidation.h"
#include "KismetAITruncation.h"
#include "KismetAIAgent.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

const TCHAR* LexToString(EKismetAIEntryPoint EntryPoint)
{
	switch (EntryPoint)
	{
	case EKismetAIEntryPoint::InProcess:    return TEXT("in_process");
	case EKismetAIEntryPoint::Panel:        return TEXT("panel");
	case EKismetAIEntryPoint::HttpAgentRun: return TEXT("http_agent_run");
	case EKismetAIEntryPoint::Mcp:          return TEXT("mcp");
	case EKismetAIEntryPoint::Rest:         return TEXT("rest");
	case EKismetAIEntryPoint::Unspecified:
	default:                                return TEXT("unspecified");
	}
}

FKismetAIToolExecutor::FKismetAIToolExecutor(IKismetAIToolInvoker& InInvoker)
	: Invoker(InInvoker)
	, Policy(InInvoker.GetPolicy() ? *InInvoker.GetPolicy() : static_cast<const IKismetAIToolPolicy&>(DefaultPolicy))
{
}

void FKismetAIToolExecutor::NormalizeArguments(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return;
	}
	for (const TCHAR* Field : { TEXT("operations"), TEXT("parameters") })
	{
		const TSharedPtr<FJsonValue> Val = Arguments->Values.FindRef(Field);
		if (!Val.IsValid() || Val->Type != EJson::String)
		{
			continue;
		}
		const FString Str = Val->AsString().TrimStartAndEnd();
		if (!Str.StartsWith(TEXT("[")))
		{
			continue;
		}
		TArray<TSharedPtr<FJsonValue>> Parsed;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Str);
		if (FJsonSerializer::Deserialize(Reader, Parsed))
		{
			Arguments->SetArrayField(Field, Parsed);
		}
	}
}

namespace
{
	// Executor refusals keep the exact model strings the loop and the system
	// prompt already key on; Typed carries the same code for REST.
	FString RefusalModelString(const FString& Code, const FString& Message)
	{
		// Built with the JSON writer rather than Printf so a reason containing
		// quotes or newlines still yields valid JSON (the old plan-mode string
		// escaped apostrophes as \', which no JSON parser accepts).
		TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetStringField(TEXT("error_code"), Code);
		Error->SetStringField(TEXT("message"), Message);
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetBoolField(TEXT("success"), false);
		Root->SetObjectField(TEXT("error"), Error);
		FString Json;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Root, Writer);
		return FString::Printf(TEXT("<tool_use_error>%s</tool_use_error>"), *Json);
	}

	FToolExecutionResult ValidationTyped(const FValidationResult& Validation)
	{
		TArray<FString> Suggestions;
		for (const FValidationError& Err : Validation.Errors)
		{
			Suggestions.Add(FString::Printf(TEXT("Field '%s': %s"), *Err.Field, *Err.Message));
		}
		return FToolExecutionResult::Error(400, TEXT("VALIDATION_ERROR"),
			Validation.Errors.Num() > 0 ? Validation.Errors[0].Message : FString(), Suggestions);
	}

	FString CancelledMessage(const FString& ToolName)
	{
		return FString::Printf(TEXT("'%s' was cancelled before it ran. Nothing was changed."), *ToolName);
	}

	// What the admission seam observed, written on the game thread and read by
	// the executor after the synchronous Invoke returns.
	struct FAdmissionRecord
	{
		bool bCancelled = false;
		bool bDenied = false;
	};
}



FToolExecutionResult FKismetAIToolExecutor::CancelledBeforeStartResult(const FString& ToolName)
{
	return FToolExecutionResult::Error(409, TEXT("CANCELLED"), CancelledMessage(ToolName));
}

FString FKismetAIToolExecutor::CancelledBeforeStartModelString(const FString& ToolName)
{
	return RefusalModelString(TEXT("CANCELLED"), CancelledMessage(ToolName));
}

FString FKismetAIToolExecutor::TruncateResult(const FString& Result, int32 MaxChars, bool& bOutTruncated)
{
	bOutTruncated = false;
	if (MaxChars <= 0 || Result.Len() <= MaxChars)
	{
		return Result;
	}

	bOutTruncated = true;

	// Structure-aware truncation where the shape is recognisable, so the model
	// gets whole nodes or whole execution chains rather than a string cut
	// mid-token.
	if (Result.Contains(TEXT("Begin Object")) && Result.Contains(TEXT("End Object")))
	{
		return FKismetAITruncation::TruncateT3DByNodeBoundary(Result, MaxChars).TruncatedText;
	}
	if (Result.Contains(TEXT("### Execution Flow")))
	{
		return FKismetAITruncation::TruncateDescribeByChain(Result, MaxChars).TruncatedText;
	}
	return Result.Left(MaxChars) + TEXT("\n... [truncated]");
}

FToolCallOutcome FKismetAIToolExecutor::Execute(const FToolCallContext& Context)
{
	FToolCallOutcome Outcome;
	const double StartSec = FPlatformTime::Seconds();

	auto Refuse = [&Outcome, StartSec](const FString& ModelString, const FToolExecutionResult& Typed) -> FToolCallOutcome
	{
		Outcome.ResultForModel = ModelString;
		Outcome.Typed = Typed;
		Outcome.Typed.bHandlerNotStarted = true;
		Outcome.bIsError = true;
		Outcome.DurationMs = (int32)((FPlatformTime::Seconds() - StartSec) * 1000.0);
		return Outcome;
	};

	// 0. Caller. The transport authenticates; the executor only insists that it
	//    said so. A default-constructed context is an adapter bug, and running it
	//    would mean inventing an identity the policy would then trust.
	if (!Context.Caller.IsVerified())
	{
		const FString Msg = TEXT("The entry point did not supply a verified caller. Nothing was run.");
		return Refuse(RefusalModelString(TEXT("CALLER_NOT_VERIFIED"), Msg),
			FToolExecutionResult::Error(401, TEXT("CALLER_NOT_VERIFIED"), Msg));
	}

	// 0b. Registry lookup. Only registry tools that run in the editor reach an
	//     invoker; agent-local tools act on one agent's run state and are never
	//     executed here. Refusing here means no transport can be routed to a
	//     dispatcher's default entry for a name nobody advertised.
	const FKismetAIToolDefinition* Def = FKismetAIToolRegistry::Find(Context.ToolName);
	if (!Def || Def->bIsInProcess)
	{
		return Refuse(
			TEXT("<tool_use_error>{\"success\":false,\"error\":{\"error_code\":\"UNKNOWN_TOOL\",\"message\":\"Unknown tool\"}}</tool_use_error>"),
			FToolExecutionResult::Error(404, TEXT("UNKNOWN_TOOL"), FString::Printf(TEXT("Unknown tool: %s"), *Context.ToolName)));
	}

	FString Mode;
	if (Context.Arguments.IsValid())
	{
		Context.Arguments->TryGetStringField(TEXT("mode"), Mode);
	}

	// 1. Policy — allow, deny, or ask, before anything happens.
	FPolicyContext PolicyCtx;
	PolicyCtx.ToolName = Context.ToolName;
	PolicyCtx.bPlanModeActive = Context.bPlanModeActive;
	PolicyCtx.bInteractiveAllowed = Context.bInteractiveAllowed;
	PolicyCtx.Caller = Context.Caller;
	PolicyCtx.Mode = Mode;
	PolicyCtx.Arguments = Context.Arguments;
	PolicyCtx.RequestBody = Context.RequestBody;
	PolicyCtx.SessionId = Context.SessionId;

	const FPolicyVerdict Verdict = Policy.Evaluate(PolicyCtx);

	if (Verdict.Decision == EPolicyDecision::Deny)
	{
		// PLAN_MODE_READONLY is the code the system prompt teaches the model to
		// recognise; other denials carry their own code.
		const FString Code = Verdict.ErrorCode.IsEmpty() ? FString(TEXT("POLICY_DENIED")) : Verdict.ErrorCode;
		Outcome.bWasDenied = true;
		return Refuse(RefusalModelString(Code, Verdict.Reason),
			FToolExecutionResult::Error(403, Code, Verdict.Reason));
	}

	if (Verdict.Decision == EPolicyDecision::Ask)
	{
		// No approver installed means nobody can answer; treat it as a refusal
		// rather than silently proceeding with a side effect. An answer is only
		// as good as the policy says: a stale, replayed or mismatched one is
		// refused.
		FKismetAIApprovalAnswer Answer;
		Answer.RequestId = Verdict.Approval.IsValid() ? Verdict.Approval->RequestId : FString();
		if (OnApprovalAnswer)
		{
			Answer = OnApprovalAnswer(Verdict);
		}
		else if (OnApprovalRequired)
		{
			Answer.Decision = OnApprovalRequired(Verdict)
				? EKismetAIApprovalDecision::AllowOnce : EKismetAIApprovalDecision::Deny;
		}
		const EKismetAIApprovalResolution Resolution = Policy.ResolveApproval(PolicyCtx, Verdict, Answer);
		if (Resolution != EKismetAIApprovalResolution::Granted)
		{
			const bool bDeclined = Resolution == EKismetAIApprovalResolution::Declined;
			const FString Code = bDeclined ? FString(TEXT("APPROVAL_DENIED")) : FString(TEXT("APPROVAL_INVALID"));
			const FString Msg = bDeclined
				? FString::Printf(TEXT("The user declined to allow '%s'. Do not retry it; explain what you needed it for, or propose a different approach."),
					*Context.ToolName)
				: FString::Printf(TEXT("The approval for '%s' was not valid for this request (%s). Nothing was changed."),
					*Context.ToolName, LexToString(Resolution));
			Outcome.bWasDenied = true;
			return Refuse(RefusalModelString(Code, Msg), FToolExecutionResult::Error(403, Code, Msg));
		}
	}

	// 2. Validate. Rejecting here means the editor is never touched with
	//    arguments we already know are malformed.
	const FValidationResult Validation =
		FKismetAIToolRegistry::ValidateArgs(Context.ToolName, Context.Arguments);
	if (!Validation.bIsValid)
	{
		if (Context.ToolName == TEXT("read_source") && Validation.Errors.Num() > 0)
		{
			FToolExecutionResult Typed = ValidationTyped(Validation);
			if (Validation.Errors[0].Field == TEXT("scope")) Typed.ErrorCode = TEXT("INVALID_SCOPE");
			if (Validation.Errors[0].Field == TEXT("pattern")) Typed.ErrorCode = TEXT("UNSUPPORTED_PATTERN");
			return Refuse(Typed.ToModelString(), Typed);
		}
		return Refuse(FString::Printf(TEXT("<tool_use_error>%s</tool_use_error>"), *Validation.ToJsonString()),
			ValidationTyped(Validation));
	}

	// 3. Cancellation — the last check on this thread.
	//
	// After approval on purpose: an approval that arrives while the user is
	// cancelling must not start the work. The synthetic result matters as much as
	// the skip — a tool_use with no tool_result breaks the provider's pairing
	// invariant and the session cannot be resumed.
	if (Context.CancelToken.IsCancelled())
	{
		Outcome.bWasCancelled = true;
		return Refuse(CancelledBeforeStartModelString(Context.ToolName), CancelledBeforeStartResult(Context.ToolName));
	}

	// 4. Dispatch, with the admission seam the invoker runs on the game thread
	//    immediately before the handler. Captures are by value except the policy,
	//    which outlives every call of Admit: Admit only runs while this
	//    synchronous Invoke is still waiting (see FKismetAIToolInvocation).
	TSharedRef<FAdmissionRecord, ESPMode::ThreadSafe> Admission = MakeShared<FAdmissionRecord, ESPMode::ThreadSafe>();
	FKismetAIToolInvocation Invocation;
	Invocation.ToolName = Context.ToolName;
	Invocation.Mode = Mode;
	Invocation.RequestBody = Context.RequestBody;
	Invocation.ObservedAssetRevisions = Context.ObservedAssetRevisions;
	Invocation.bRequireReadRevision = Context.bRequireReadRevision;
	Invocation.Caller = Context.Caller;
	Invocation.CancelToken = Context.CancelToken;
	Invocation.CancelledBeforeStart = CancelledBeforeStartResult(Context.ToolName);
	{
		const IKismetAIToolPolicy* AdmissionPolicy = &Policy;
		const FKismetAICancellationToken Token = Context.CancelToken;
		const FString ToolName = Context.ToolName;
		Invocation.Admit = [AdmissionPolicy, Token, PolicyCtx, ToolName, Admission]() -> TOptional<FToolExecutionResult>
		{
			// Cancelled while queued for the game thread: the handler never starts.
			if (Token.IsCancelled())
			{
				Admission->bCancelled = true;
				return CancelledBeforeStartResult(ToolName);
			}
			const FPolicyVerdict AdmissionVerdict = AdmissionPolicy->EvaluateAdmission(PolicyCtx);
			if (AdmissionVerdict.Decision != EPolicyDecision::Allow)
			{
				Admission->bDenied = true;
				const FString Code = AdmissionVerdict.ErrorCode.IsEmpty() ? FString(TEXT("POLICY_DENIED")) : AdmissionVerdict.ErrorCode;
				return FToolExecutionResult::Error(403, Code, AdmissionVerdict.Reason);
			}
			return {};
		};
	}

	const FKismetAIInvokeResult Invoked = Invoker.Invoke(Invocation);
	Outcome.bWasCancelled = Admission->bCancelled || Invoked.bStoppedBeforeStart;
	Outcome.bWasDenied = Admission->bDenied;
	Outcome.bCancelRequestedWhileRunning = !Outcome.bWasCancelled && Invoked.bCancelRequestedWhileRunning;
	if (Outcome.bWasCancelled)
	{
		// However it was caught, a call stopped before its handler started
		// reads the same to the model, the journal and REST.
		Outcome.Typed = CancelledBeforeStartResult(Context.ToolName);
		Outcome.Typed.bHandlerNotStarted = true;
		Outcome.ResultForModel = CancelledBeforeStartModelString(Context.ToolName);
		Outcome.bIsError = true;
		Outcome.DurationMs = (int32)((FPlatformTime::Seconds() - StartSec) * 1000.0);
		return Outcome;
	}
	if (Invoked.Typed.IsSet())
	{
		Outcome.Typed = Invoked.Typed.GetValue();
	}
	else
	{
		// A fake invoker with only a model string. Keep the success bit honest
		// for anyone reading Typed.
		Outcome.Typed = FKismetAIAgent::IsToolResultError(Invoked.ModelString)
			? FToolExecutionResult::Error(500, TEXT("UNTYPED_ERROR"), Invoked.ModelString)
			: FToolExecutionResult::Success();
	}

	if (Admission->bDenied) Outcome.Typed.bHandlerNotStarted = true;

	// 5. Shape. One tool must not be able to consume the context window.
	const int32 MaxChars = FKismetAIToolRegistry::GetMaxResultChars(
		Context.ToolName, Mode, Context.DefaultMaxResultChars);
	Outcome.ResultForModel = TruncateResult(Invoked.ModelString, MaxChars, Outcome.bWasTruncated);
	Outcome.bIsError = !Outcome.Typed.bSuccess || !Outcome.Typed.bOperationsSucceeded;
	Outcome.DurationMs = (int32)((FPlatformTime::Seconds() - StartSec) * 1000.0);
	return Outcome;
}
