// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Scoped authority, without an editor.
//
// The descriptor, the grant store, approvals and the scoped policy are pure
// runtime code, so the adversarial cases — a grant for A used on B, a replayed
// or duplicated approval, revocation while queued, headless with no grant — are
// asserted here directly. KismetAI.ScopedAuthority.* (editor module) repeats
// the important ones through the real panel, MCP and REST paths.

#include "KismetAIAuthority.h"
#include "KismetAIToolExecutor.h"
#include "KismetAIToolInvoker.h"
#include "KismetAIToolPolicy.h"
#include "KismetAIToolRegistry.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"

constexpr auto ScopedPolicyFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace KismetAIScopedPolicyTest
{
	FKismetAICallerContext Caller(EKismetAIEntryPoint E)
	{
		return FKismetAICallerContext::Verified(E, TEXT("test"));
	}

	TSharedPtr<FJsonObject> Compile(const TCHAR* Path)
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("blueprint"), Path);
		return A;
	}

	TSharedPtr<FJsonObject> ManageAsset(const TCHAR* Action, const TCHAR* Path, const TCHAR* NewName = nullptr, const TCHAR* NewPath = nullptr)
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("action"), Action);
		A->SetStringField(TEXT("asset_path"), Path);
		if (NewName) A->SetStringField(TEXT("new_name"), NewName);
		if (NewPath) A->SetStringField(TEXT("new_path"), NewPath);
		return A;
	}

	TSharedPtr<FJsonObject> CreateAsset(const TCHAR* Path, const TCHAR* Name)
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("asset_type"), TEXT("Blueprint"));
		A->SetStringField(TEXT("path"), Path);
		A->SetStringField(TEXT("name"), Name);
		A->SetStringField(TEXT("parent_class"), TEXT("Actor"));
		return A;
	}

	FString AddGrant(FKismetAIAuthorityStore& Store, EKismetAIEntryPoint E, EKismetAIAuthorityOp Ops,
		std::initializer_list<const TCHAR*> Scopes, EKismetAIGrantLifetime Lifetime = EKismetAIGrantLifetime::UntilRevoked,
		const FString& SessionId = FString())
	{
		FKismetAIAuthorityGrant G;
		G.CreatedBy = TEXT("test");
		G.EntryPoints = { E };
		G.Operations = Ops;
		for (const TCHAR* S : Scopes) G.PathScopes.Add(S);
		G.Lifetime = Lifetime;
		G.SessionId = SessionId;
		FString Error;
		return Store.AddGrant(G, Error);
	}

	FPolicyContext PolicyCtx(const TCHAR* Tool, TSharedPtr<FJsonObject> Body, EKismetAIEntryPoint E, bool bInteractive = false,
		const FString& SessionId = FString())
	{
		FPolicyContext C;
		C.ToolName = Tool;
		C.Arguments = Body;
		C.RequestBody = Body;
		C.Caller = Caller(E);
		C.bInteractiveAllowed = bInteractive;
		C.SessionId = SessionId;
		return C;
	}

	// The production invoker's shape: admission, then the "handler".
	class FAuthorityInvoker final : public IKismetAIToolInvoker
	{
	public:
		explicit FAuthorityInvoker(const IKismetAIToolPolicy& InPolicy) : Policy(InPolicy) {}
		virtual FKismetAIInvokeResult Invoke(const FKismetAIToolInvocation& Invocation) override
		{
			if (BeforeAdmission) { BeforeAdmission(); }
			if (const TOptional<FToolExecutionResult> Refused = Invocation.RunAdmission())
			{
				return FKismetAIInvokeResult::FromTyped(Refused.GetValue());
			}
			++HandlerRuns;
			return FKismetAIInvokeResult::FromTyped(FToolExecutionResult::Success());
		}
		virtual const IKismetAIToolPolicy* GetPolicy() const override { return &Policy; }
		virtual TSet<FString> GetDispatchableToolNames() const override { return {}; }
		virtual FString ResolveHandlerName(const FString&, const FString&) const override { return {}; }

		const IKismetAIToolPolicy& Policy;
		int32 HandlerRuns = 0;
		TFunction<void()> BeforeAdmission;
	};

	FToolCallContext CallCtx(const TCHAR* Tool, TSharedPtr<FJsonObject> Body, EKismetAIEntryPoint E, bool bInteractive = false,
		const FString& SessionId = FString())
	{
		FToolCallContext C;
		C.ToolName = Tool;
		C.Caller = Caller(E);
		C.Arguments = Body;
		C.RequestBody = Body;
		C.bInteractiveAllowed = bInteractive;
		C.SessionId = SessionId;
		return C;
	}
}

// Normalization is what every scope comparison stands on.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScopedPolicy_NormalizesPaths, "KismetAI.ScopedAuthority.Policy.NormalizesPaths", ScopedPolicyFlags)
bool FScopedPolicy_NormalizesPaths::RunTest(const FString& Parameters)
{
	using namespace KismetAIAuthority;
	FString P, Leaf;

	TestTrue(TEXT("plain"), NormalizeAssetReference(TEXT("/Game/A/BP_X"), P) && P == TEXT("/Game/A/BP_X"));
	TestTrue(TEXT("object path"), NormalizeAssetReference(TEXT("/Game/A/BP_X.BP_X"), P) && P == TEXT("/Game/A/BP_X"));
	TestTrue(TEXT("sub-object"), NormalizeAssetReference(TEXT("/Game/A/BP_X.BP_X:Sub"), P) && P == TEXT("/Game/A/BP_X"));
	TestTrue(TEXT("class wrapper"), NormalizeAssetReference(TEXT("Blueprint'/Game/A/BP_X.BP_X'"), P) && P == TEXT("/Game/A/BP_X"));
	TestTrue(TEXT("backslashes and doubled slashes"), NormalizeAssetReference(TEXT("\\Game\\A//BP_X/"), P) && P == TEXT("/Game/A/BP_X"));
	TestFalse(TEXT("traversal refused"), NormalizeAssetReference(TEXT("/Game/A/../../Engine/X"), P));
	TestFalse(TEXT("dot segment refused"), NormalizeAssetReference(TEXT("/Game/./A"), P));
	TestFalse(TEXT("empty refused"), NormalizeAssetReference(TEXT("  "), P));
	TestFalse(TEXT("wildcard refused"), NormalizeAssetReference(TEXT("/Game/*"), P));
	TestFalse(TEXT("bare name is not a path"), NormalizeAssetReference(TEXT("BP_Player"), P, &Leaf));
	TestEqual(TEXT("but its leaf is kept for lookup"), Leaf, FString(TEXT("BP_Player")));

	TestTrue(TEXT("scope covers itself"), IsPathWithinScope(TEXT("/Game/A"), TEXT("/Game/A")));
	TestTrue(TEXT("scope covers children"), IsPathWithinScope(TEXT("/Game/A/B/C"), TEXT("/Game/A")));
	TestFalse(TEXT("adjacent prefix is not inside"), IsPathWithinScope(TEXT("/Game/AB/C"), TEXT("/Game/A")));
	TestTrue(TEXT("case-insensitive like package names"), IsPathWithinScope(TEXT("/game/a/x"), TEXT("/Game/A")));
	TestTrue(TEXT("root scope covers every mount"), IsPathWithinScope(TEXT("/Engine/X"), TEXT("/")));
	TestFalse(TEXT("an unresolved path is never inside"), IsPathWithinScope(FString(), TEXT("/")));
	return true;
}

// Every registered tool that can change something has a rule; nothing falls
// through to "unclassified".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScopedPolicy_EveryMutatingToolClassified, "KismetAI.ScopedAuthority.EveryMutatingToolClassified", ScopedPolicyFlags)
bool FScopedPolicy_EveryMutatingToolClassified::RunTest(const FString& Parameters)
{
	for (const FString& Name : FKismetAIToolRegistry::GetAllNames())
	{
		const FKismetAIToolDefinition* Def = FKismetAIToolRegistry::Find(Name);
		const FKismetAIOperation Op = KismetAIAuthority::DescribeOperation(Name, MakeShared<FJsonObject>());
		if (Def->IsAllowedInPlanMode())
		{
			TestFalse(*FString::Printf(TEXT("%s needs no authority"), *Name), Op.RequiresAuthority());
			continue;
		}
		TestTrue(*FString::Printf(TEXT("%s needs authority"), *Name), Op.RequiresAuthority());
		for (const FKismetAIAuthorityTarget& T : Op.Targets)
		{
			TestNotEqual(*FString::Printf(TEXT("%s is classified"), *Name), T.Field, FString(TEXT("(unclassified)")));
		}
	}
	return true;
}

// What each kind of call does, by effect.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScopedPolicy_ClassifiesOperations, "KismetAI.ScopedAuthority.Policy.ClassifiesOperations", ScopedPolicyFlags)
bool FScopedPolicy_ClassifiesOperations::RunTest(const FString& Parameters)
{
	using namespace KismetAIScopedPolicyTest;
	using namespace KismetAIAuthority;

	const FKismetAIOperation Edit = DescribeOperation(TEXT("compile_blueprint"), Compile(TEXT("/Game/A/BP_X")));
	TestTrue(TEXT("compile is an edit"), Edit.Ops == EKismetAIAuthorityOp::Edit);

	const FKismetAIOperation Del = DescribeOperation(TEXT("manage_asset"), ManageAsset(TEXT("delete"), TEXT("/Game/A/BP_X")));
	TestTrue(TEXT("delete is a deletion"), Del.Ops == EKismetAIAuthorityOp::Delete);

	const FKismetAIOperation Ren = DescribeOperation(TEXT("manage_asset"), ManageAsset(TEXT("rename"), TEXT("/Game/A/BP_X"), TEXT("BP_Y")));
	TestTrue(TEXT("rename deletes the source and creates the target"),
		Ren.Ops == (EKismetAIAuthorityOp::Delete | EKismetAIAuthorityOp::Create));
	TestTrue(TEXT("rename target stays in the folder"),
		Ren.Targets.ContainsByPredicate([](const FKismetAIAuthorityTarget& T) { return T.PackagePath == TEXT("/Game/A/BP_Y"); }));

	const FKismetAIOperation Mv = DescribeOperation(TEXT("manage_asset"), ManageAsset(TEXT("move"), TEXT("/Game/A/BP_X"), nullptr, TEXT("/Game/B")));
	TestTrue(TEXT("move lands at new_path/name"),
		Mv.Targets.ContainsByPredicate([](const FKismetAIAuthorityTarget& T) { return T.PackagePath == TEXT("/Game/B/BP_X") && T.Effect == EKismetAIAuthorityOp::Create; }));

	const FKismetAIOperation Dup = DescribeOperation(TEXT("manage_asset"), ManageAsset(TEXT("duplicate"), TEXT("/Game/A/BP_X"), TEXT("BP_Copy"), TEXT("/Game/B")));
	TestTrue(TEXT("duplicate only creates"), Dup.Ops == EKismetAIAuthorityOp::Create);

	const FKismetAIOperation Save = DescribeOperation(TEXT("manage_asset"), ManageAsset(TEXT("save"), TEXT("/Game/A/BP_X")));
	TestTrue(TEXT("save is an edit"), Save.Ops == EKismetAIAuthorityOp::Edit);

	const FKismetAIOperation Create = DescribeOperation(TEXT("create_asset"), CreateAsset(TEXT("/Game/B/"), TEXT("BP_New")));
	TestTrue(TEXT("create_asset creates path/name"), Create.Ops == EKismetAIAuthorityOp::Create && Create.Targets[0].PackagePath == TEXT("/Game/B/BP_New"));

	// A name that walks out of its folder is an indirect broadening; it is left
	// unresolved, and an unresolved target is never covered.
	const FKismetAIOperation Escape = DescribeOperation(TEXT("create_asset"), CreateAsset(TEXT("/Game/B"), TEXT("../../Engine/BP_Evil")));
	TestTrue(TEXT("escaping name is unresolved"), Escape.HasUnresolvedTargets());

	// A batch of inspection actions is a read.
	TSharedPtr<FJsonObject> Get = MakeShared<FJsonObject>();
	Get->SetStringField(TEXT("blueprint"), TEXT("/Game/A/BP_X"));
	TSharedPtr<FJsonObject> GetOp = MakeShared<FJsonObject>();
	GetOp->SetStringField(TEXT("action"), TEXT("get_component_property"));
	Get->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(GetOp) });
	TestFalse(TEXT("read-only batch needs no authority"), DescribeOperation(TEXT("manage_components"), Get).RequiresAuthority());
	TSharedPtr<FJsonObject> SetOp = MakeShared<FJsonObject>();
	SetOp->SetStringField(TEXT("action"), TEXT("set_component_property"));
	Get->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(GetOp), MakeShared<FJsonValueObject>(SetOp) });
	TestTrue(TEXT("one write makes it an edit"), DescribeOperation(TEXT("manage_components"), Get).RequiresAuthority());
	return true;
}

// Grant coverage: entry point, operation class, component-aware scope,
// session, lifetime, revocation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScopedPolicy_GrantCoverage, "KismetAI.ScopedAuthority.Policy.GrantCoverage", ScopedPolicyFlags)
bool FScopedPolicy_GrantCoverage::RunTest(const FString& Parameters)
{
	using namespace KismetAIScopedPolicyTest;
	using namespace KismetAIAuthority;

	FKismetAIAuthorityStore Store;
	const FString GrantA = AddGrant(Store, EKismetAIEntryPoint::Mcp, EKismetAIAuthorityOp::Edit, { TEXT("/Game/A") });
	TestFalse(TEXT("grant created"), GrantA.IsEmpty());

	const FKismetAICallerContext Mcp = Caller(EKismetAIEntryPoint::Mcp);
	auto Allowed = [&Store](const FKismetAIOperation& Op, const FKismetAICallerContext& C, const FString& Session = FString())
	{
		return Store.Check(Op, C, Session, /*bConsume=*/true).bAllowed;
	};

	TestTrue(TEXT("edit inside A"), Allowed(DescribeOperation(TEXT("compile_blueprint"), Compile(TEXT("/Game/A/Sub/BP_X"))), Mcp));
	TestFalse(TEXT("edit in B"), Allowed(DescribeOperation(TEXT("compile_blueprint"), Compile(TEXT("/Game/B/BP_X"))), Mcp));
	TestFalse(TEXT("edit in the adjacent prefix AB"), Allowed(DescribeOperation(TEXT("compile_blueprint"), Compile(TEXT("/Game/AB/BP_X"))), Mcp));
	TestFalse(TEXT("delete inside A is not an edit"), Allowed(DescribeOperation(TEXT("manage_asset"), ManageAsset(TEXT("delete"), TEXT("/Game/A/BP_X"))), Mcp));
	TestFalse(TEXT("create inside A is not an edit"), Allowed(DescribeOperation(TEXT("create_asset"), CreateAsset(TEXT("/Game/A"), TEXT("BP_New"))), Mcp));
	TestFalse(TEXT("another entry point is not covered"),
		Allowed(DescribeOperation(TEXT("compile_blueprint"), Compile(TEXT("/Game/A/BP_X"))), Caller(EKismetAIEntryPoint::Rest)));

	// The refusal says which part is missing.
	const FKismetAIAuthorityCheck Missing = Store.Check(
		DescribeOperation(TEXT("manage_asset"), ManageAsset(TEXT("delete"), TEXT("/Game/A/BP_X"))), Mcp, FString(), false);
	TestTrue(TEXT("reason names the operation class"), Missing.Reason.Contains(TEXT("delete")));

	// A rename out of A needs delete in A AND create at the destination.
	AddGrant(Store, EKismetAIEntryPoint::Mcp, EKismetAIAuthorityOp::Delete, { TEXT("/Game/A") });
	const FKismetAIOperation Move = DescribeOperation(TEXT("manage_asset"), ManageAsset(TEXT("move"), TEXT("/Game/A/BP_X"), nullptr, TEXT("/Game/B")));
	TestFalse(TEXT("move to an ungranted destination"), Allowed(Move, Mcp));
	AddGrant(Store, EKismetAIEntryPoint::Mcp, EKismetAIAuthorityOp::Create, { TEXT("/Game/B") });
	TestTrue(TEXT("move with both halves granted"), Allowed(Move, Mcp));

	// Revocation.
	TestEqual(TEXT("revoked one grant"), Store.Revoke(GrantA, TEXT("test")), 1);
	TestFalse(TEXT("edit after revocation"), Allowed(DescribeOperation(TEXT("compile_blueprint"), Compile(TEXT("/Game/A/BP_X"))), Mcp));

	// Session binding.
	AddGrant(Store, EKismetAIEntryPoint::HttpAgentRun, EKismetAIAuthorityOp::Edit, { TEXT("/Game") }, EKismetAIGrantLifetime::Session, TEXT("s-1"));
	const FKismetAIOperation EditC = DescribeOperation(TEXT("compile_blueprint"), Compile(TEXT("/Game/C/BP_X")));
	TestTrue(TEXT("same session"), Allowed(EditC, Caller(EKismetAIEntryPoint::HttpAgentRun), TEXT("s-1")));
	TestFalse(TEXT("another session (a resumed or concurrent run)"), Allowed(EditC, Caller(EKismetAIEntryPoint::HttpAgentRun), TEXT("s-2")));

	// Single use.
	AddGrant(Store, EKismetAIEntryPoint::Rest, EKismetAIAuthorityOp::Edit, { TEXT("/Game/D") }, EKismetAIGrantLifetime::Once);
	const FKismetAIOperation EditD = DescribeOperation(TEXT("compile_blueprint"), Compile(TEXT("/Game/D/BP_X")));
	TestTrue(TEXT("once: first use"), Allowed(EditD, Caller(EKismetAIEntryPoint::Rest)));
	TestFalse(TEXT("once: second use"), Allowed(EditD, Caller(EKismetAIEntryPoint::Rest)));

	// Expiry.
	FKismetAIAuthorityGrant Expiring;
	Expiring.CreatedBy = TEXT("test");
	Expiring.EntryPoints = { EKismetAIEntryPoint::Rest };
	Expiring.Operations = EKismetAIAuthorityOp::Edit;
	Expiring.PathScopes = { TEXT("/Game/E") };
	Expiring.ExpiresAtSeconds = FPlatformTime::Seconds() - 1.0;
	FString Error;
	Store.AddGrant(Expiring, Error);
	TestFalse(TEXT("expired grant"), Allowed(DescribeOperation(TEXT("compile_blueprint"), Compile(TEXT("/Game/E/BP_X"))), Caller(EKismetAIEntryPoint::Rest)));

	// Unresolved targets are never covered, even by "/".
	AddGrant(Store, EKismetAIEntryPoint::Rest, EKismetAIAuthorityOp::Create, { TEXT("/") });
	TestFalse(TEXT("escaping create under a root grant"),
		Allowed(DescribeOperation(TEXT("create_asset"), CreateAsset(TEXT("/Game/B"), TEXT("../../Engine/X"))), Caller(EKismetAIEntryPoint::Rest)));

	// Malformed grants are refused.
	FKismetAIAuthorityGrant NoEntry;
	NoEntry.Operations = EKismetAIAuthorityOp::Edit;
	NoEntry.PathScopes = { TEXT("/Game") };
	TestTrue(TEXT("a grant without entry points is refused"), Store.AddGrant(NoEntry, Error).IsEmpty());
	return true;
}

// Approvals are bound to one request and one operation; answers are single-use.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScopedPolicy_ApprovalBinding, "KismetAI.ScopedAuthority.Policy.ApprovalBinding", ScopedPolicyFlags)
bool FScopedPolicy_ApprovalBinding::RunTest(const FString& Parameters)
{
	using namespace KismetAIScopedPolicyTest;
	using namespace KismetAIAuthority;

	FKismetAIAuthorityStore Store;
	const FKismetAICallerContext Panel = Caller(EKismetAIEntryPoint::Panel);
	const FKismetAIOperation DeleteX = DescribeOperation(TEXT("manage_asset"), ManageAsset(TEXT("delete"), TEXT("/Game/A/BP_X")));
	const FKismetAIOperation DeleteY = DescribeOperation(TEXT("manage_asset"), ManageAsset(TEXT("delete"), TEXT("/Game/A/BP_Y")));

	auto Answer = [](const FString& Id, EKismetAIApprovalDecision D)
	{
		FKismetAIApprovalAnswer A;
		A.RequestId = Id;
		A.Decision = D;
		A.AnsweredBy = TEXT("test");
		return A;
	};

	// Unknown id.
	TestTrue(TEXT("unknown request"), Store.ResolveApproval(Answer(TEXT("approval-nope"), EKismetAIApprovalDecision::AllowOnce), DeleteX, Panel, TEXT("s"))
		== EKismetAIApprovalResolution::UnknownRequest);

	// A request for X answered for Y (changed target) creates nothing.
	const FKismetAIApprovalRequest ForX = Store.OpenApprovalRequest(DeleteX, Panel, TEXT("s"));
	TestTrue(TEXT("answer applied to a different target"), Store.ResolveApproval(Answer(ForX.RequestId, EKismetAIApprovalDecision::AllowSession), DeleteY, Panel, TEXT("s"))
		== EKismetAIApprovalResolution::Mismatch);
	TestFalse(TEXT("and Y is not authorized"), Store.Check(DeleteY, Panel, TEXT("s"), false).bAllowed);
	TestTrue(TEXT("the request is settled: answering again is refused"),
		Store.ResolveApproval(Answer(ForX.RequestId, EKismetAIApprovalDecision::AllowOnce), DeleteX, Panel, TEXT("s"))
			== EKismetAIApprovalResolution::AlreadyAnswered);
	TestFalse(TEXT("so X is not authorized either"), Store.Check(DeleteX, Panel, TEXT("s"), false).bAllowed);

	// Another caller or session cannot use someone else's request.
	const FKismetAIApprovalRequest ForX2 = Store.OpenApprovalRequest(DeleteX, Panel, TEXT("s"));
	TestTrue(TEXT("answered in another session"), Store.ResolveApproval(Answer(ForX2.RequestId, EKismetAIApprovalDecision::AllowOnce), DeleteX, Panel, TEXT("other"))
		== EKismetAIApprovalResolution::Mismatch);

	// Allow once: exactly that operation, one time, for that tool.
	const FKismetAIApprovalRequest Once = Store.OpenApprovalRequest(DeleteX, Panel, TEXT("s"));
	FString GrantId;
	TestTrue(TEXT("granted"), Store.ResolveApproval(Answer(Once.RequestId, EKismetAIApprovalDecision::AllowOnce), DeleteX, Panel, TEXT("s"), &GrantId)
		== EKismetAIApprovalResolution::Granted);
	TestTrue(TEXT("duplicate answer refused"), Store.ResolveApproval(Answer(Once.RequestId, EKismetAIApprovalDecision::AllowSession), DeleteX, Panel, TEXT("s"))
		== EKismetAIApprovalResolution::AlreadyAnswered);
	TestFalse(TEXT("once does not cover a sibling asset"), Store.Check(DeleteY, Panel, TEXT("s"), false).bAllowed);
	TestFalse(TEXT("once does not cover another caller"), Store.Check(DeleteX, Caller(EKismetAIEntryPoint::Mcp), TEXT("s"), false).bAllowed);
	TestTrue(TEXT("once covers the approved call"), Store.Check(DeleteX, Panel, TEXT("s"), true).bAllowed);
	TestFalse(TEXT("and only once"), Store.Check(DeleteX, Panel, TEXT("s"), true).bAllowed);

	// Allow for the session: the same exact asset again, in this session only.
	const FKismetAIApprovalRequest Sess = Store.OpenApprovalRequest(DeleteX, Panel, TEXT("s"));
	Store.ResolveApproval(Answer(Sess.RequestId, EKismetAIApprovalDecision::AllowSession), DeleteX, Panel, TEXT("s"));
	TestTrue(TEXT("session approval, first"), Store.Check(DeleteX, Panel, TEXT("s"), true).bAllowed);
	TestTrue(TEXT("session approval, again"), Store.Check(DeleteX, Panel, TEXT("s"), true).bAllowed);
	TestFalse(TEXT("session approval, another session"), Store.Check(DeleteX, Panel, TEXT("s2"), true).bAllowed);
	TestFalse(TEXT("session approval, another asset"), Store.Check(DeleteY, Panel, TEXT("s"), true).bAllowed);

	// Declined and expired requests create nothing.
	const FKismetAIApprovalRequest Declined = Store.OpenApprovalRequest(DeleteY, Panel, TEXT("s"));
	TestTrue(TEXT("declined"), Store.ResolveApproval(Answer(Declined.RequestId, EKismetAIApprovalDecision::Deny), DeleteY, Panel, TEXT("s"))
		== EKismetAIApprovalResolution::Declined);
	const FKismetAIApprovalRequest Stale = Store.OpenApprovalRequest(DeleteY, Panel, TEXT("s"), /*TtlSeconds=*/0.0);
	FPlatformProcess::Sleep(1.05f);
	TestTrue(TEXT("expired"), Store.ResolveApproval(Answer(Stale.RequestId, EKismetAIApprovalDecision::AllowOnce), DeleteY, Panel, TEXT("s"))
		== EKismetAIApprovalResolution::Expired);
	TestFalse(TEXT("still not authorized"), Store.Check(DeleteY, Panel, TEXT("s"), false).bAllowed);
	return true;
}

// The policy's verdicts per caller kind: headless needs a grant, interactive
// asks, in-process keeps the editor's authority, reads never need one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScopedPolicy_VerdictsByCaller, "KismetAI.ScopedAuthority.Policy.VerdictsByCaller", ScopedPolicyFlags)
bool FScopedPolicy_VerdictsByCaller::RunTest(const FString& Parameters)
{
	using namespace KismetAIScopedPolicyTest;

	FKismetAIScopedToolPolicy Policy;
	const TSharedPtr<FJsonObject> Del = ManageAsset(TEXT("delete"), TEXT("/Game/A/BP_X"));

	const FPolicyVerdict Headless = Policy.Evaluate(PolicyCtx(TEXT("manage_asset"), Del, EKismetAIEntryPoint::Mcp));
	TestTrue(TEXT("headless MCP without a grant is refused"), Headless.Decision == EPolicyDecision::Deny);
	TestEqual(TEXT("as AUTHORITY_REQUIRED"), Headless.ErrorCode, FString(TEXT("AUTHORITY_REQUIRED")));
	TestTrue(TEXT("and the refusal carries a decision id"), Headless.Reason.Contains(TEXT("[authz-")));
	TestTrue(TEXT("and says how to get a grant"), Headless.Reason.Contains(TEXT("KismetAI.Authority.Grant")));

	const FPolicyVerdict HeadlessAdmission = Policy.EvaluateAdmission(PolicyCtx(TEXT("manage_asset"), Del, EKismetAIEntryPoint::HttpAgentRun));
	TestTrue(TEXT("admission refuses too"), HeadlessAdmission.Decision == EPolicyDecision::Deny);

	const FPolicyVerdict Interactive = Policy.Evaluate(PolicyCtx(TEXT("manage_asset"), Del, EKismetAIEntryPoint::Panel, /*interactive=*/true, TEXT("s")));
	TestTrue(TEXT("interactive caller is asked"), Interactive.Decision == EPolicyDecision::Ask);
	TestTrue(TEXT("with a bound request"), Interactive.Approval.IsValid() && Interactive.Approval->Fingerprint.Contains(TEXT("/game/a/bp_x")));

	TestTrue(TEXT("in-process keeps the editor's authority"),
		Policy.Evaluate(PolicyCtx(TEXT("manage_asset"), Del, EKismetAIEntryPoint::InProcess)).Decision == EPolicyDecision::Allow);
	TestTrue(TEXT("reads need no grant"),
		Policy.Evaluate(PolicyCtx(TEXT("read_blueprint"), Compile(TEXT("/Game/A/BP_X")), EKismetAIEntryPoint::Mcp)).Decision == EPolicyDecision::Allow);

	FPolicyContext Plan = PolicyCtx(TEXT("compile_blueprint"), Compile(TEXT("/Game/A/BP_X")), EKismetAIEntryPoint::InProcess);
	Plan.bPlanModeActive = true;
	TestEqual(TEXT("plan mode still refuses first"), Policy.Evaluate(Plan).ErrorCode, FString(TEXT("PLAN_MODE_READONLY")));

	// A bare name cannot be judged off the game thread; admission (with no
	// resolver to find it) refuses it rather than guessing.
	const TSharedPtr<FJsonObject> Bare = Compile(TEXT("BP_X"));
	TestTrue(TEXT("bare name deferred before dispatch"),
		Policy.Evaluate(PolicyCtx(TEXT("compile_blueprint"), Bare, EKismetAIEntryPoint::Mcp)).Decision == EPolicyDecision::Allow);
	AddGrant(Policy.GetStore(), EKismetAIEntryPoint::Mcp, EKismetAIAuthorityOp::Edit, { TEXT("/") });
	TestTrue(TEXT("and refused at admission even under a root grant"),
		Policy.EvaluateAdmission(PolicyCtx(TEXT("compile_blueprint"), Bare, EKismetAIEntryPoint::Mcp)).Decision == EPolicyDecision::Deny);

	// With a resolver, admission judges what the lookup would find.
	FKismetAIScopedToolPolicy Scoped;
	Scoped.SetTargetResolver([](const FKismetAIAuthorityTarget& T) -> TArray<FString>
	{
		return T.Raw == TEXT("BP_X") ? TArray<FString>{ TEXT("/Game/Other/BP_X") } : TArray<FString>{};
	});
	AddGrant(Scoped.GetStore(), EKismetAIEntryPoint::Mcp, EKismetAIAuthorityOp::Edit, { TEXT("/Game/A") });
	TestTrue(TEXT("a bare name resolving outside the grant is refused"),
		Scoped.EvaluateAdmission(PolicyCtx(TEXT("compile_blueprint"), Bare, EKismetAIEntryPoint::Mcp)).Decision == EPolicyDecision::Deny);
	AddGrant(Scoped.GetStore(), EKismetAIEntryPoint::Mcp, EKismetAIAuthorityOp::Edit, { TEXT("/Game/Other") });
	TestTrue(TEXT("and allowed once its real location is granted"),
		Scoped.EvaluateAdmission(PolicyCtx(TEXT("compile_blueprint"), Bare, EKismetAIEntryPoint::Mcp)).Decision == EPolicyDecision::Allow);
	return true;
}

// Through the executor: approval answers, replays, duplicates and revocation
// between approval and admission.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScopedPolicy_ExecutorApprovalFlow, "KismetAI.ScopedAuthority.Policy.ExecutorApprovalFlow", ScopedPolicyFlags)
bool FScopedPolicy_ExecutorApprovalFlow::RunTest(const FString& Parameters)
{
	using namespace KismetAIScopedPolicyTest;

	FKismetAIScopedToolPolicy Policy;
	FAuthorityInvoker Invoker(Policy);
	const TSharedPtr<FJsonObject> Del = ManageAsset(TEXT("delete"), TEXT("/Game/A/BP_X"));

	// Headless, no grant: refused before dispatch, the handler never runs.
	{
		FKismetAIToolExecutor Executor(Invoker);
		const FToolCallOutcome Out = Executor.Execute(CallCtx(TEXT("manage_asset"), Del, EKismetAIEntryPoint::HttpAgentRun));
		TestEqual(TEXT("headless refused"), Out.Typed.ErrorCode, FString(TEXT("AUTHORITY_REQUIRED")));
		TestTrue(TEXT("as a denial, not a failed mutation"), Out.bWasDenied);
		TestEqual(TEXT("handler not run"), Invoker.HandlerRuns, 0);
	}

	// Interactive with no approver installed: refused.
	{
		FKismetAIToolExecutor Executor(Invoker);
		const FToolCallOutcome Out = Executor.Execute(CallCtx(TEXT("manage_asset"), Del, EKismetAIEntryPoint::Panel, true, TEXT("s")));
		TestEqual(TEXT("no approver: refused"), Out.Typed.ErrorCode, FString(TEXT("APPROVAL_DENIED")));
		TestEqual(TEXT("handler not run"), Invoker.HandlerRuns, 0);
	}

	// Approve once: runs once; the next identical call asks again.
	FString LastRequestId;
	{
		FKismetAIToolExecutor Executor(Invoker);
		int32 Asked = 0;
		Executor.OnApprovalAnswer = [&Asked, &LastRequestId](const FPolicyVerdict& V)
		{
			++Asked;
			LastRequestId = V.Approval->RequestId;
			FKismetAIApprovalAnswer A;
			A.RequestId = V.Approval->RequestId;
			A.Decision = EKismetAIApprovalDecision::AllowOnce;
			return A;
		};
		TestFalse(TEXT("approved once: ran"), Executor.Execute(CallCtx(TEXT("manage_asset"), Del, EKismetAIEntryPoint::Panel, true, TEXT("s"))).bIsError);
		TestEqual(TEXT("handler ran once"), Invoker.HandlerRuns, 1);
		Executor.Execute(CallCtx(TEXT("manage_asset"), Del, EKismetAIEntryPoint::Panel, true, TEXT("s")));
		TestEqual(TEXT("asked again for the second call"), Asked, 2);
	}

	// Replaying the old answer for a new request creates nothing.
	{
		FKismetAIToolExecutor Executor(Invoker);
		const FString Replayed = LastRequestId;
		Executor.OnApprovalAnswer = [Replayed](const FPolicyVerdict& V)
		{
			FKismetAIApprovalAnswer A;
			A.RequestId = Replayed;
			A.Decision = EKismetAIApprovalDecision::AllowSession;
			return A;
		};
		const int32 Before = Invoker.HandlerRuns;
		const FToolCallOutcome Out = Executor.Execute(CallCtx(TEXT("manage_asset"), Del, EKismetAIEntryPoint::Panel, true, TEXT("s")));
		TestEqual(TEXT("replayed answer refused"), Out.Typed.ErrorCode, FString(TEXT("APPROVAL_INVALID")));
		TestEqual(TEXT("handler not run"), Invoker.HandlerRuns, Before);
		TestFalse(TEXT("no session authority appeared"),
			Policy.GetStore().Check(KismetAIAuthority::DescribeOperation(TEXT("manage_asset"), Del), Caller(EKismetAIEntryPoint::Panel), TEXT("s"), false).bAllowed);
	}

	// Declined.
	{
		FKismetAIToolExecutor Executor(Invoker);
		Executor.OnApprovalAnswer = [](const FPolicyVerdict& V)
		{
			FKismetAIApprovalAnswer A;
			A.RequestId = V.Approval->RequestId;
			A.Decision = EKismetAIApprovalDecision::Deny;
			return A;
		};
		const int32 Before = Invoker.HandlerRuns;
		TestEqual(TEXT("declined"), Executor.Execute(CallCtx(TEXT("manage_asset"), Del, EKismetAIEntryPoint::Panel, true, TEXT("s"))).Typed.ErrorCode,
			FString(TEXT("APPROVAL_DENIED")));
		TestEqual(TEXT("handler not run"), Invoker.HandlerRuns, Before);
	}

	// Approved, then every grant revoked while the call is queued: admission
	// refuses and the handler never runs.
	{
		FKismetAIToolExecutor Executor(Invoker);
		Executor.OnApprovalAnswer = [](const FPolicyVerdict& V)
		{
			FKismetAIApprovalAnswer A;
			A.RequestId = V.Approval->RequestId;
			A.Decision = EKismetAIApprovalDecision::AllowSession;
			return A;
		};
		Invoker.BeforeAdmission = [&Policy]() { Policy.GetStore().RevokeAll(TEXT("test")); };
		const int32 Before = Invoker.HandlerRuns;
		const FToolCallOutcome Out = Executor.Execute(CallCtx(TEXT("manage_asset"), Del, EKismetAIEntryPoint::Panel, true, TEXT("s")));
		TestEqual(TEXT("revoked while queued"), Out.Typed.ErrorCode, FString(TEXT("AUTHORITY_DENIED")));
		TestTrue(TEXT("reported as a denial"), Out.bWasDenied);
		TestEqual(TEXT("handler not run"), Invoker.HandlerRuns, Before);
		Invoker.BeforeAdmission = nullptr;
	}

	// A standing grant: no prompt at all for ordinary edits in scope.
	{
		AddGrant(Policy.GetStore(), EKismetAIEntryPoint::Panel, EKismetAIAuthorityOp::Edit, { TEXT("/Game/A") });
		FKismetAIToolExecutor Executor(Invoker);
		int32 Asked = 0;
		Executor.OnApprovalAnswer = [&Asked](const FPolicyVerdict& V) { ++Asked; return FKismetAIApprovalAnswer(); };
		for (int32 i = 0; i < 3; ++i)
		{
			Executor.Execute(CallCtx(TEXT("compile_blueprint"), Compile(TEXT("/Game/A/BP_X")), EKismetAIEntryPoint::Panel, true, TEXT("s")));
		}
		TestEqual(TEXT("ordinary edits in scope never prompt"), Asked, 0);
	}
	return true;
}
