// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIAgent.h"
#include "Misc/AutomationTest.h"

constexpr auto PlanModeTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanMode_ReadOnlyToolsAllowed,
	"KismetAI.PlanMode.ReadOnlyToolsAllowed",
	PlanModeTestFlags)

bool FPlanMode_ReadOnlyToolsAllowed::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("read_blueprint is allowed"), FKismetAIAgent::IsToolAllowedInPlanMode(TEXT("read_blueprint")));
	TestTrue(TEXT("search_assets is allowed"), FKismetAIAgent::IsToolAllowedInPlanMode(TEXT("search_assets")));
	TestTrue(TEXT("get_references is allowed"), FKismetAIAgent::IsToolAllowedInPlanMode(TEXT("get_references")));
	TestTrue(TEXT("inspect_class is allowed"), FKismetAIAgent::IsToolAllowedInPlanMode(TEXT("inspect_class")));
	TestTrue(TEXT("search_nodes is allowed"), FKismetAIAgent::IsToolAllowedInPlanMode(TEXT("search_nodes")));
	TestTrue(TEXT("read_source is allowed"), FKismetAIAgent::IsToolAllowedInPlanMode(TEXT("read_source")));
	TestTrue(TEXT("discover_tools is allowed"), FKismetAIAgent::IsToolAllowedInPlanMode(TEXT("discover_tools")));
	TestTrue(TEXT("enter_plan_mode is allowed"), FKismetAIAgent::IsToolAllowedInPlanMode(TEXT("enter_plan_mode")));
	TestTrue(TEXT("exit_plan_mode is allowed"), FKismetAIAgent::IsToolAllowedInPlanMode(TEXT("exit_plan_mode")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanMode_MutatingToolsBlocked,
	"KismetAI.PlanMode.MutatingToolsBlocked",
	PlanModeTestFlags)

bool FPlanMode_MutatingToolsBlocked::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("write_blueprint is blocked"), FKismetAIAgent::IsToolAllowedInPlanMode(TEXT("write_blueprint")));
	TestFalse(TEXT("edit_blueprint is blocked"), FKismetAIAgent::IsToolAllowedInPlanMode(TEXT("edit_blueprint")));
	TestFalse(TEXT("compile_blueprint is blocked"), FKismetAIAgent::IsToolAllowedInPlanMode(TEXT("compile_blueprint")));
	TestFalse(TEXT("manage_blueprint is blocked"), FKismetAIAgent::IsToolAllowedInPlanMode(TEXT("manage_blueprint")));
	TestFalse(TEXT("manage_components is blocked"), FKismetAIAgent::IsToolAllowedInPlanMode(TEXT("manage_components")));
	TestFalse(TEXT("create_asset is blocked"), FKismetAIAgent::IsToolAllowedInPlanMode(TEXT("create_asset")));
	TestFalse(TEXT("manage_asset is blocked"), FKismetAIAgent::IsToolAllowedInPlanMode(TEXT("manage_asset")));
	TestFalse(TEXT("unknown tools are blocked"), FKismetAIAgent::IsToolAllowedInPlanMode(TEXT("unknown_tool")));

	return true;
}
