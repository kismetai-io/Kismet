// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIToolPolicy.h"

TOptional<FPolicyVerdict> FKismetAIDefaultToolPolicy::EvaluatePlanMode(const FPolicyContext& Context)
{
	const FKismetAIToolDefinition* Def = FKismetAIToolRegistry::Find(Context.ToolName);

	// This replaces IsToolAllowedInPlanMode's hand-kept name list. The verdict is
	// derived from the tool's capability, so what plan mode permits and what the
	// rest of the tool's metadata says cannot drift apart.
	if (Def && Context.bPlanModeActive && !Def->IsAllowedInPlanMode())
	{
		return FPolicyVerdict::Deny(FString::Printf(
			TEXT("'%s' modifies the project and is not allowed in plan mode. Finish exploring, then call exit_plan_mode with your plan."),
			*Context.ToolName), TEXT("PLAN_MODE_READONLY"));
	}
	return {};
}

FPolicyVerdict FKismetAIDefaultToolPolicy::Evaluate(const FPolicyContext& Context) const
{
	const FKismetAIToolDefinition* Def = FKismetAIToolRegistry::Find(Context.ToolName);

	// An unknown tool is not the policy's problem — dispatch rejects it with
	// UNKNOWN_TOOL, which is the clearer error. Allow it through to get there.
	if (!Def)
	{
		return FPolicyVerdict::Allow();
	}

	// Rule 1 — plan mode is read-only.
	if (TOptional<FPolicyVerdict> PlanMode = EvaluatePlanMode(Context))
	{
		return PlanMode.GetValue();
	}

	// Rule 2 — an approval already granted for this capability stands.
	if (HasSessionGrant(Def->Capability))
	{
		return FPolicyVerdict::Allow();
	}

	// Rule 3 — ask before a side effect when asking is switched on. A headless
	// run has nobody to ask; it used to degrade to Allow, which made "cannot
	// ask" mean "may proceed". It is refused instead.
	const bool bWantsApproval =
		Def->Capability == EToolCapability::AssetMutate ||
		Def->Capability == EToolCapability::ProjectFileWrite;

	if (bAskBeforeMutations && bWantsApproval)
	{
		if (!Context.bInteractiveAllowed)
		{
			return FPolicyVerdict::Deny(FString::Printf(
				TEXT("'%s' needs approval and nobody can approve it in this run. Do not retry it; explain what you needed it for."),
				*Context.ToolName), TEXT("APPROVAL_UNAVAILABLE"));
		}
		return FPolicyVerdict::Ask(FString::Printf(
			TEXT("Allow '%s' to modify the project?"), *Context.ToolName));
	}

	return FPolicyVerdict::Allow();
}

// ─────────────────────────────────────────────────────────────────────────────
// Scoped authority
// ─────────────────────────────────────────────────────────────────────────────

FKismetAIScopedToolPolicy::FKismetAIScopedToolPolicy()
	: Store(MakeShared<FKismetAIAuthorityStore, ESPMode::ThreadSafe>())
{
}

FKismetAIScopedToolPolicy::FKismetAIScopedToolPolicy(TSharedRef<FKismetAIAuthorityStore, ESPMode::ThreadSafe> InStore)
	: Store(InStore)
{
}

namespace
{
	TSharedPtr<FJsonObject> BodyOf(const FPolicyContext& Context)
	{
		return Context.RequestBody.IsValid() ? Context.RequestBody : Context.Arguments;
	}

	bool NeedsAuthority(const FPolicyContext& Context)
	{
		const FKismetAIToolDefinition* Def = FKismetAIToolRegistry::Find(Context.ToolName);
		return Def && !Def->IsAllowedInPlanMode()
			&& Context.Caller.EntryPoint != EKismetAIEntryPoint::InProcess;
	}
}

FString FKismetAIScopedToolPolicy::GuidanceFor(const FKismetAICallerContext& Caller)
{
	switch (Caller.EntryPoint)
	{
	case EKismetAIEntryPoint::Panel:
		return TEXT("Ask the user to grant it (or to approve it when prompted), or work on assets the panel may already edit.");
	case EKismetAIEntryPoint::HttpAgentRun:
		return TEXT("Unattended runs need an explicit grant: pass an 'authority' object to /api/v1/agent/run, or have the editor user run KismetAI.Authority.Grant.");
	case EKismetAIEntryPoint::Mcp:
		return TEXT("MCP callers need an explicit grant from the editor user: KismetAI.Authority.Grant mcp <operations> <scope>, or POST /api/v1/authority/grants.");
	case EKismetAIEntryPoint::Rest:
		return TEXT("REST callers need an explicit grant: POST /api/v1/authority/grants, or KismetAI.Authority.Grant rest <operations> <scope> in the editor.");
	default:
		return TEXT("This caller needs an explicit grant.");
	}
}

FString FKismetAIScopedToolPolicy::RecordDenial(const TCHAR* Phase, const FPolicyContext& Context,
	const FKismetAIOperation& Op, const FString& Code, const FString& Reason) const
{
	FKismetAIAuthorityDecision D;
	D.Phase = Phase;
	D.ToolName = Context.ToolName;
	D.Operation = Op.Describe();
	D.EntryPoint = Context.Caller.EntryPoint;
	D.Principal = Context.Caller.Principal;
	D.SessionId = Context.SessionId;
	D.bAllowed = false;
	D.Code = Code;
	D.Reason = Reason;
	return Store->RecordDecision(MoveTemp(D));
}

FKismetAIOperation FKismetAIScopedToolPolicy::DescribeForAdmission(const FPolicyContext& Context) const
{
	FKismetAIOperation Op = KismetAIAuthority::DescribeOperation(Context.ToolName, BodyOf(Context));
	if (!Resolver)
	{
		return Op;
	}

	// Existing assets are looked up the way the handler looks them up (exact
	// path, then the unique leaf name), but through the asset registry only, so
	// judging a call never loads or creates a package. A new package's path is
	// what the request says; there is nothing to resolve.
	TArray<FKismetAIAuthorityTarget> Resolved;
	for (const FKismetAIAuthorityTarget& T : Op.Targets)
	{
		if (T.Effect == EKismetAIAuthorityOp::Create || T.Raw.IsEmpty())
		{
			Resolved.Add(T);
			continue;
		}
		const TArray<FString> Candidates = Resolver(T);
		if (Candidates.Num() == 0)
		{
			Resolved.Add(T);
			continue;
		}
		for (const FString& Candidate : Candidates)
		{
			FKismetAIAuthorityTarget C = T;
			FString Normalized;
			C.PackagePath = KismetAIAuthority::NormalizeAssetReference(Candidate, Normalized) ? Normalized : FString();
			Resolved.Add(MoveTemp(C));
		}
	}
	Op.Targets = MoveTemp(Resolved);
	return Op;
}

FPolicyVerdict FKismetAIScopedToolPolicy::Evaluate(const FPolicyContext& Context) const
{
	if (!FKismetAIToolRegistry::Find(Context.ToolName))
	{
		return FPolicyVerdict::Allow();
	}
	if (TOptional<FPolicyVerdict> PlanMode = FKismetAIDefaultToolPolicy::EvaluatePlanMode(Context))
	{
		return PlanMode.GetValue();
	}
	if (!NeedsAuthority(Context))
	{
		return FPolicyVerdict::Allow();
	}

	// A malformed call is validation's to report, with the clearer error; it
	// cannot run either way, and admission judges whatever does run.
	if (!FKismetAIToolRegistry::ValidateArgs(Context.ToolName, Context.Arguments).bIsValid)
	{
		return FPolicyVerdict::Allow();
	}

	const FKismetAIOperation Op = KismetAIAuthority::DescribeOperation(Context.ToolName, BodyOf(Context));
	if (!Op.RequiresAuthority())
	{
		return FPolicyVerdict::Allow();
	}
	// A bare asset name can only be resolved through the asset registry, which
	// admission does on the game thread. Nothing is decided here for it.
	if (Op.HasUnresolvedTargets())
	{
		return FPolicyVerdict::Allow();
	}

	const FKismetAIAuthorityCheck Check = Store->Check(Op, Context.Caller, Context.SessionId, /*bConsume=*/false);
	if (Check.bAllowed)
	{
		return FPolicyVerdict::Allow();
	}

	if (Context.bInteractiveAllowed)
	{
		const FKismetAIApprovalRequest Request = Store->OpenApprovalRequest(Op, Context.Caller, Context.SessionId);
		FPolicyVerdict V = FPolicyVerdict::Ask(Request.Prompt);
		V.Approval = MakeShared<const FKismetAIApprovalRequest, ESPMode::ThreadSafe>(Request);
		return V;
	}

	const FString DecisionId = RecordDenial(TEXT("pre_dispatch"), Context, Op, TEXT("AUTHORITY_REQUIRED"), Check.Reason);
	return FPolicyVerdict::Deny(FString::Printf(TEXT("Not authorized: %s. %s Nothing was changed. [%s]"),
		*Check.Reason, *GuidanceFor(Context.Caller), *DecisionId), TEXT("AUTHORITY_REQUIRED"));
}

FPolicyVerdict FKismetAIScopedToolPolicy::EvaluateAdmission(const FPolicyContext& Context) const
{
	if (!FKismetAIToolRegistry::Find(Context.ToolName))
	{
		return FPolicyVerdict::Allow();
	}
	if (TOptional<FPolicyVerdict> PlanMode = FKismetAIDefaultToolPolicy::EvaluatePlanMode(Context))
	{
		return PlanMode.GetValue();
	}
	if (!NeedsAuthority(Context))
	{
		return FPolicyVerdict::Allow();
	}

	const FKismetAIOperation Op = DescribeForAdmission(Context);
	if (!Op.RequiresAuthority())
	{
		return FPolicyVerdict::Allow();
	}

	const FKismetAIAuthorityCheck Check = Store->Check(Op, Context.Caller, Context.SessionId, /*bConsume=*/true);
	if (Check.bAllowed)
	{
		FKismetAIAuthorityDecision D;
		D.Phase = TEXT("admission");
		D.ToolName = Context.ToolName;
		D.Operation = Op.Describe();
		D.EntryPoint = Context.Caller.EntryPoint;
		D.Principal = Context.Caller.Principal;
		D.SessionId = Context.SessionId;
		D.bAllowed = true;
		D.GrantIds = Check.GrantIds;
		Store->RecordDecision(MoveTemp(D));
		return FPolicyVerdict::Allow();
	}

	const FString DecisionId = RecordDenial(TEXT("admission"), Context, Op, TEXT("AUTHORITY_DENIED"), Check.Reason);
	return FPolicyVerdict::Deny(FString::Printf(TEXT("Not authorized: %s. %s Nothing was changed. [%s]"),
		*Check.Reason, *GuidanceFor(Context.Caller), *DecisionId), TEXT("AUTHORITY_DENIED"));
}

EKismetAIApprovalResolution FKismetAIScopedToolPolicy::ResolveApproval(const FPolicyContext& Context,
	const FPolicyVerdict& Verdict, const FKismetAIApprovalAnswer& Answer) const
{
	if (!Verdict.Approval.IsValid())
	{
		// An Ask this policy did not issue has nothing to bind to.
		return EKismetAIApprovalResolution::UnknownRequest;
	}
	// The answer must name the request it answers. An answer carrying another
	// id (a replay of an older approval) creates nothing, and this request is
	// settled as refused so it cannot be answered a second time either.
	if (Answer.RequestId != Verdict.Approval->RequestId)
	{
		FKismetAIApprovalAnswer Refusal;
		Refusal.RequestId = Verdict.Approval->RequestId;
		Refusal.Decision = EKismetAIApprovalDecision::Deny;
		Refusal.AnsweredBy = TEXT("mismatched-answer");
		Store->ResolveApproval(Refusal, Verdict.Approval->Operation, Context.Caller, Context.SessionId);
		return EKismetAIApprovalResolution::Mismatch;
	}
	// The operation is described again from the body that will run: if it no
	// longer matches what was approved, the store refuses it.
	const FKismetAIOperation Current = KismetAIAuthority::DescribeOperation(Context.ToolName, BodyOf(Context));
	return Store->ResolveApproval(Answer, Current, Context.Caller, Context.SessionId);
}
