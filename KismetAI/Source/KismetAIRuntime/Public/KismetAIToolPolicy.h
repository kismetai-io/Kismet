// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Authorization at the executor boundary.
//
// Before this, the only thing between the model and an asset mutation was a
// name allowlist consulted inside the dispatcher, plus provider-key checks at
// the entry points. That is a workflow restriction, not an authorization
// boundary: it answers "is the agent in plan mode", not "is this call allowed to
// happen". KismetAI mutates project assets inside a long-lived editor process,
// and every production coding agent surveyed decides allow/deny/ask immediately
// before the side effect. Unreal transactions help recovery; they are not
// authorization.
//
// Deliberately coarse. A rule language before there are real cases to serve
// would be speculative, and the review's own warning is prompt fatigue — so the
// mechanism ships ON and the prompting ships OFF (see FDefaultToolPolicy).
//
// The module's shipping policy is FKismetAIScopedToolPolicy, which adds
// scoped authority (KismetAIAuthority.h) on top of the plan-mode rule. A
// mutation needs a grant covering the caller, the operation class and the
// normalized asset; an interactive caller is asked, an unattended one is
// refused. Inability to ask is never permission.

#pragma once

#include "CoreMinimal.h"
#include "KismetAIToolRegistry.h"
#include "KismetAIToolExecution.h"
#include "KismetAIAuthority.h"

enum class EPolicyDecision : uint8
{
	Allow,
	// Refused outright. Reason is returned to the model so it can adapt.
	Deny,
	// Needs a human. Resolved through the executor's approval callback; with
	// none installed the executor refuses (APPROVAL_DENIED). Policies only
	// return Ask when the caller is interactive.
	Ask,
};

// How long a granted approval lasts.
enum class EApprovalScope : uint8
{
	Once,
	Session,
	Project,
};

struct KISMETAIRUNTIME_API FPolicyVerdict
{
	EPolicyDecision Decision = EPolicyDecision::Allow;

	// Shown to the user on Ask, and returned to the model on Deny. On Deny this
	// is the model's only explanation, so it must say what to do instead.
	FString Reason;

	EApprovalScope Scope = EApprovalScope::Once;

	// Stable error code a Deny is reported with. PLAN_MODE_READONLY is the one
	// the system prompt teaches the model; other refusals use POLICY_DENIED.
	FString ErrorCode;

	// Set on an Ask from a policy that binds approvals to one normalized
	// operation. The answer must carry this request's id.
	TSharedPtr<const FKismetAIApprovalRequest, ESPMode::ThreadSafe> Approval;

	static FPolicyVerdict Allow() { return FPolicyVerdict{}; }

	static FPolicyVerdict Deny(const FString& InReason, const FString& InErrorCode = TEXT("POLICY_DENIED"))
	{
		FPolicyVerdict V;
		V.Decision = EPolicyDecision::Deny;
		V.Reason = InReason;
		V.ErrorCode = InErrorCode;
		return V;
	}

	static FPolicyVerdict Ask(const FString& InReason)
	{
		FPolicyVerdict V;
		V.Decision = EPolicyDecision::Ask;
		V.Reason = InReason;
		return V;
	}
};

// What the policy is deciding about.
struct KISMETAIRUNTIME_API FPolicyContext
{
	FString ToolName;
	bool bPlanModeActive = false;

	// False for headless runs (evals, automation). An Ask cannot be answered by
	// anyone, so policies refuse instead of asking — never allow.
	bool bInteractiveAllowed = false;

	// Who is asking, as the transport verified it.
	FKismetAICallerContext Caller;

	// The "mode" argument of a mode-routed tool; empty otherwise.
	FString Mode;

	// The normalized, validated arguments (null in Evaluate's pre-validation
	// call is possible for a malformed request; always set at admission).
	TSharedPtr<FJsonObject> Arguments;

	// The body the handler will read. Scoped authority derives the affected
	// assets from this, not from the model-facing arguments, so what is judged
	// is what runs. Falls back to Arguments when unset.
	TSharedPtr<FJsonObject> RequestBody;

	// The agent session the call belongs to; empty for REST and MCP.
	FString SessionId;
};

class KISMETAIRUNTIME_API IKismetAIToolPolicy
{
public:
	virtual ~IKismetAIToolPolicy() = default;
	// Before validation, on the calling thread. May return Ask, which can block
	// on a human, so it must never be called on the game thread.
	virtual FPolicyVerdict Evaluate(const FPolicyContext& Context) const = 0;

	// At the game-thread admission seam, immediately before the handler runs
	// (KismetAIToolExecution.h). Allow or Deny only — nobody can be asked here
	// without stalling the editor. This is where a decision that must hold at
	// the moment editor work starts belongs: scoped authority over the normalized
	// asset and operation. Default: allow.
	virtual FPolicyVerdict EvaluateAdmission(const FPolicyContext& Context) const
	{
		return FPolicyVerdict::Allow();
	}

	// Settles an Ask with the approver's answer, on the calling thread. Granted
	// lets the call continue to validation and admission; anything else refuses
	// it. Default: any non-Deny answer is granted (no binding to check).
	virtual EKismetAIApprovalResolution ResolveApproval(const FPolicyContext& Context, const FPolicyVerdict& Verdict,
		const FKismetAIApprovalAnswer& Answer) const
	{
		return Answer.Decision == EKismetAIApprovalDecision::Deny
			? EKismetAIApprovalResolution::Declined : EKismetAIApprovalResolution::Granted;
	}
};

// The executor's fallback policy (fake invokers, tests). Three rules, in order.
class KISMETAIRUNTIME_API FKismetAIDefaultToolPolicy final : public IKismetAIToolPolicy
{
public:
	virtual FPolicyVerdict Evaluate(const FPolicyContext& Context) const override;

	// Ask before asset mutations. OFF by default and deliberately so: KismetAI's
	// product is an agent that edits Blueprints unattended, and a confirmation on
	// every edit_blueprint would make it unusable. What matters is that the
	// boundary exists, is typed, and is tested — so the first genuinely dangerous
	// capability can be gated without inventing the mechanism under pressure.
	bool bAskBeforeMutations = false;

	// Approvals granted for the rest of this session, by capability.
	void GrantForSession(EToolCapability Capability) { SessionGrants.Add(Capability); }
	void ClearSessionGrants() { SessionGrants.Reset(); }
	bool HasSessionGrant(EToolCapability Capability) const { return SessionGrants.Contains(Capability); }

	// Rule 1 on its own, shared with the scoped policy.
	static TOptional<FPolicyVerdict> EvaluatePlanMode(const FPolicyContext& Context);

private:
	TSet<EToolCapability> SessionGrants;
};

// The shipping policy: plan mode, then scoped authority.
//
//   - Inspect / AgentState tools, and read-only batches, need no authority.
//   - InProcess callers keep the editor's authority (code already inside the
//     editor process can call the handlers directly; a check here would be
//     theatre). Every transport — panel, /agent/run, MCP, REST — needs a grant.
//   - Evaluate (calling thread): a covered call proceeds; an uncovered one is
//     Ask when the caller is interactive and AUTHORITY_REQUIRED otherwise. A
//     target given as a bare name is left to admission, which can resolve it.
//   - EvaluateAdmission (game thread, immediately before the handler): the
//     targets are resolved without loading anything (SetTargetResolver), and the
//     store is checked again — consuming a single-use grant — so a grant revoked
//     while the call was queued stops it (AUTHORITY_DENIED).
class KISMETAIRUNTIME_API FKismetAIScopedToolPolicy final : public IKismetAIToolPolicy
{
public:
	FKismetAIScopedToolPolicy();
	explicit FKismetAIScopedToolPolicy(TSharedRef<FKismetAIAuthorityStore, ESPMode::ThreadSafe> InStore);

	virtual FPolicyVerdict Evaluate(const FPolicyContext& Context) const override;
	virtual FPolicyVerdict EvaluateAdmission(const FPolicyContext& Context) const override;
	virtual EKismetAIApprovalResolution ResolveApproval(const FPolicyContext& Context, const FPolicyVerdict& Verdict,
		const FKismetAIApprovalAnswer& Answer) const override;

	FKismetAIAuthorityStore& GetStore() const { return *Store; }
	TSharedRef<FKismetAIAuthorityStore, ESPMode::ThreadSafe> GetStoreRef() const { return Store; }

	// Maps one existing-asset target to the package(s) the handler would act on,
	// without loading anything. Several candidates mean the handler's lookup is
	// ambiguous; all of them must be covered. Empty means "nothing found" (the
	// target is judged as written). Called only at admission, on the game thread.
	using FTargetResolver = TFunction<TArray<FString>(const FKismetAIAuthorityTarget&)>;
	void SetTargetResolver(FTargetResolver InResolver) { Resolver = MoveTemp(InResolver); }

	// The operation admission would judge: described from the body, then
	// resolved. Exposed for tests and the authority REST view.
	FKismetAIOperation DescribeForAdmission(const FPolicyContext& Context) const;

private:
	FString RecordDenial(const TCHAR* Phase, const FPolicyContext& Context, const FKismetAIOperation& Op,
		const FString& Code, const FString& Reason) const;
	static FString GuidanceFor(const FKismetAICallerContext& Caller);

	TSharedRef<FKismetAIAuthorityStore, ESPMode::ThreadSafe> Store;
	FTargetResolver Resolver;
};
