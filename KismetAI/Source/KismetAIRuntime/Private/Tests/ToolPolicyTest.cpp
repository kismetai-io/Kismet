// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Authorization at the executor boundary.
//
// The point of these is that a policy decision is testable *without* a model, a
// prompt or an editor — which is exactly what the old arrangement could not do,
// because "is this allowed" was a name list consulted inside the dispatcher.

#include "KismetAIToolPolicy.h"
#include "Misc/AutomationTest.h"

constexpr auto PolicyFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	FPolicyContext Ctx(const TCHAR* Tool, bool bPlanMode = false, bool bInteractive = true)
	{
		FPolicyContext C;
		C.ToolName = Tool;
		C.bPlanModeActive = bPlanMode;
		C.bInteractiveAllowed = bInteractive;
		return C;
	}
}

// Rule 1: plan mode is read-only, derived from capability rather than a list.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPolicy_PlanModeIsReadOnly, "KismetAI.ToolPolicy.PlanModeIsReadOnly", PolicyFlags)
bool FPolicy_PlanModeIsReadOnly::RunTest(const FString& Parameters)
{
	FKismetAIDefaultToolPolicy P;

	TestTrue(TEXT("mutations refused in plan mode"),
		P.Evaluate(Ctx(TEXT("edit_blueprint"), /*plan=*/true)).Decision == EPolicyDecision::Deny);
	TestTrue(TEXT("reads allowed in plan mode"),
		P.Evaluate(Ctx(TEXT("read_blueprint"), true)).Decision == EPolicyDecision::Allow);
	TestTrue(TEXT("materials readable in plan mode"),
		P.Evaluate(Ctx(TEXT("read_material"), true)).Decision == EPolicyDecision::Allow);
	TestTrue(TEXT("widgets readable in plan mode"),
		P.Evaluate(Ctx(TEXT("read_widget"), true)).Decision == EPolicyDecision::Allow);

	// compile writes the generated class, and plan mode has always refused it.
	TestTrue(TEXT("compile refused in plan mode"),
		P.Evaluate(Ctx(TEXT("compile_blueprint"), true)).Decision == EPolicyDecision::Deny);

	// Outside plan mode the same calls are fine.
	TestTrue(TEXT("mutations allowed outside plan mode"),
		P.Evaluate(Ctx(TEXT("edit_blueprint"), false)).Decision == EPolicyDecision::Allow);

	// The denial has to tell the model what to do instead.
	const FPolicyVerdict Denied = P.Evaluate(Ctx(TEXT("edit_blueprint"), true));
	TestTrue(TEXT("denial names the tool"), Denied.Reason.Contains(TEXT("edit_blueprint")));
	TestTrue(TEXT("and says what to do next"), Denied.Reason.Contains(TEXT("exit_plan_mode")));
	return true;
}

// Shipping default: the mechanism is on, the prompting is off.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPolicy_AskIsOffByDefault, "KismetAI.ToolPolicy.AskIsOffByDefault", PolicyFlags)
bool FPolicy_AskIsOffByDefault::RunTest(const FString& Parameters)
{
	FKismetAIDefaultToolPolicy P;
	TestFalse(TEXT("asking is off"), P.bAskBeforeMutations);

	for (const TCHAR* Tool : { TEXT("edit_blueprint"), TEXT("write_blueprint"), TEXT("create_asset") })
	{
		TestTrue(*FString::Printf(TEXT("%s runs unattended"), Tool),
			P.Evaluate(Ctx(Tool)).Decision == EPolicyDecision::Allow);
	}
	return true;
}

// With asking on, mutations ask and reads do not.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPolicy_AsksOnlyForMutations, "KismetAI.ToolPolicy.AsksOnlyForMutations", PolicyFlags)
bool FPolicy_AsksOnlyForMutations::RunTest(const FString& Parameters)
{
	FKismetAIDefaultToolPolicy P;
	P.bAskBeforeMutations = true;

	TestTrue(TEXT("mutation asks"),
		P.Evaluate(Ctx(TEXT("edit_blueprint"))).Decision == EPolicyDecision::Ask);
	TestTrue(TEXT("read does not ask"),
		P.Evaluate(Ctx(TEXT("read_blueprint"))).Decision == EPolicyDecision::Allow);
	TestTrue(TEXT("agent-state tools do not ask"),
		P.Evaluate(Ctx(TEXT("update_todos"))).Decision == EPolicyDecision::Allow);
	return true;
}

// A headless run has nobody to ask. It must not block, and it must not
// proceed either: inability to ask is not permission. The refusal says why.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPolicy_HeadlessAskIsNotPermission, "KismetAI.ToolPolicy.HeadlessAskIsNotPermission", PolicyFlags)
bool FPolicy_HeadlessAskIsNotPermission::RunTest(const FString& Parameters)
{
	FKismetAIDefaultToolPolicy P;
	P.bAskBeforeMutations = true;

	const FPolicyVerdict Headless = P.Evaluate(Ctx(TEXT("edit_blueprint"), /*plan=*/false, /*interactive=*/false));
	TestTrue(TEXT("non-interactive run is refused, not silently allowed"), Headless.Decision == EPolicyDecision::Deny);
	TestEqual(TEXT("with a stable code"), Headless.ErrorCode, FString(TEXT("APPROVAL_UNAVAILABLE")));

	// Reads still run headless.
	TestTrue(TEXT("reads proceed headless"),
		P.Evaluate(Ctx(TEXT("read_blueprint"), false, false)).Decision == EPolicyDecision::Allow);

	// But plan mode still refuses — that is a rule, not a prompt.
	TestTrue(TEXT("plan mode still refuses headless"),
		P.Evaluate(Ctx(TEXT("edit_blueprint"), /*plan=*/true, /*interactive=*/false)).Decision
			== EPolicyDecision::Deny);
	return true;
}

// Rule 2: a session grant suppresses further asking for that capability.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPolicy_SessionGrantSuppressesAsk, "KismetAI.ToolPolicy.SessionGrantSuppressesAsk", PolicyFlags)
bool FPolicy_SessionGrantSuppressesAsk::RunTest(const FString& Parameters)
{
	FKismetAIDefaultToolPolicy P;
	P.bAskBeforeMutations = true;

	TestTrue(TEXT("asks the first time"),
		P.Evaluate(Ctx(TEXT("edit_blueprint"))).Decision == EPolicyDecision::Ask);

	P.GrantForSession(EToolCapability::AssetMutate);
	TestTrue(TEXT("granted capability stops asking"),
		P.Evaluate(Ctx(TEXT("edit_blueprint"))).Decision == EPolicyDecision::Allow);
	TestTrue(TEXT("and covers other tools with the same capability"),
		P.Evaluate(Ctx(TEXT("write_blueprint"))).Decision == EPolicyDecision::Allow);

	// A grant does NOT override plan mode — that is a different rule, checked first.
	TestTrue(TEXT("a grant cannot unlock plan mode"),
		P.Evaluate(Ctx(TEXT("edit_blueprint"), /*plan=*/true)).Decision == EPolicyDecision::Deny);

	P.ClearSessionGrants();
	TestTrue(TEXT("clearing restores asking"),
		P.Evaluate(Ctx(TEXT("edit_blueprint"))).Decision == EPolicyDecision::Ask);
	return true;
}

// An unknown tool is dispatch's problem, not the policy's.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPolicy_UnknownToolPasses, "KismetAI.ToolPolicy.UnknownToolPasses", PolicyFlags)
bool FPolicy_UnknownToolPasses::RunTest(const FString& Parameters)
{
	FKismetAIDefaultToolPolicy P;
	P.bAskBeforeMutations = true;
	TestTrue(TEXT("passes through to UNKNOWN_TOOL"),
		P.Evaluate(Ctx(TEXT("no_such_tool"))).Decision == EPolicyDecision::Allow);
	return true;
}
