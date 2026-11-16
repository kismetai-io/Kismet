// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// The registry must reproduce, exactly, the six scattered tables it
// replaces. These tests hold the old values literally so the comparison is
// against what shipped, not against the registry restating itself.
//
// The lists below were transcribed from KismetAIAgent.cpp before the tables were
// deleted:
//   MutationToolNames, ReadOnlyToolNames, IsToolAllowedInPlanMode,
//   the truncation if/else chain, the adaptive max_tokens classification,
//   and the discover_tools CategoryMap.

#include "KismetAIToolRegistry.h"
#include "KismetAIEmbeddedConfig.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

constexpr auto ToolRegTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	// --- the shipped tables, verbatim ------------------------------------

	const TCHAR* const OldMutationTools[] = {
		TEXT("write_blueprint"), TEXT("edit_blueprint"), TEXT("insert_node"),
		TEXT("manage_blueprint"), TEXT("manage_components"), TEXT("manage_state_machine"),
		TEXT("create_asset"), TEXT("manage_asset"), TEXT("manage_animation"),
		TEXT("edit_widget"),
	};

	const TCHAR* const OldReadOnlyTools[] = {
		TEXT("read_blueprint"), TEXT("read_material"), TEXT("read_widget"),
		TEXT("search_nodes"), TEXT("search_assets"),
		TEXT("read_source"), TEXT("inspect_class"), TEXT("get_references"),
		TEXT("discover_tools"),
	};

	// IsToolAllowedInPlanMode's full list. Note the last three never actually
	// reach the plan gate — enter_plan_mode, exit_plan_mode and update_todos are
	// handled and returned earlier in ExecuteTool — so only the first ten are
	// behaviourally load-bearing.
	const TCHAR* const OldPlanModeAllowed[] = {
		TEXT("read_blueprint"), TEXT("read_material"), TEXT("read_widget"),
		TEXT("search_assets"), TEXT("get_references"), TEXT("inspect_class"),
		TEXT("search_nodes"), TEXT("read_source"), TEXT("read_logs"),
		TEXT("discover_tools"),
		TEXT("enter_plan_mode"), TEXT("exit_plan_mode"), TEXT("update_todos"),
	};

	// Tools handled before the plan gate, so the gate's verdict on them is moot.
	const TCHAR* const GateBypassingTools[] = {
		TEXT("enter_plan_mode"), TEXT("exit_plan_mode"),
		TEXT("ask_user"), TEXT("update_todos"),
	};

	bool IsGateBypassing(const FString& Name)
	{
		for (const TCHAR* N : GateBypassingTools) { if (Name == N) { return true; } }
		return false;
	}

	bool InList(const TCHAR* const* List, int32 Num, const FString& Name)
	{
		for (int32 i = 0; i < Num; ++i) { if (Name == List[i]) { return true; } }
		return false;
	}

	TSet<FString> AdvertisedToolNames()
	{
		FString PluginDir = FPaths::GetPath(FModuleManager::Get().GetModuleFilename(TEXT("KismetAI")));
		PluginDir = FPaths::GetPath(FPaths::GetPath(PluginDir));
		const FString ConfigDir = PluginDir / TEXT("Config");

		const TCHAR* const Files[] = {
			TEXT("tools_core.json"), TEXT("tools_extended.json"), TEXT("tools_project.json"),
			TEXT("tools_material.json"), TEXT("tools_widget.json"), TEXT("tools_discovery.json"),
		};

		TSet<FString> Out;
		for (const TCHAR* File : Files)
		{
			FString Json;
			if (!KismetAIEmbeddedConfig::LoadConfigFile(File, ConfigDir / File, Json)) { continue; }
			TArray<TSharedPtr<FJsonValue>> Tools;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
			if (!FJsonSerializer::Deserialize(Reader, Tools)) { continue; }
			for (const TSharedPtr<FJsonValue>& V : Tools)
			{
				const TSharedPtr<FJsonObject> O = V->AsObject();
				FString Name;
				if (O.IsValid() && O->TryGetStringField(TEXT("name"), Name)) { Out.Add(Name); }
			}
		}
		return Out;
	}
}

// The registry and the shipped schemas must describe the same tools. This is the
// guard that makes "one place per tool" true rather than aspirational.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_CoversEveryAdvertisedTool, "KismetAI.ToolRegistry.CoversEveryAdvertisedTool", ToolRegTestFlags)
bool FRegistry_CoversEveryAdvertisedTool::RunTest(const FString& Parameters)
{
	const TSet<FString> Advertised = AdvertisedToolNames();
	TestTrue(TEXT("schemas loaded"), Advertised.Num() >= 30);

	const TSet<FString> Registered = FKismetAIToolRegistry::GetAllNames();

	for (const FString& Tool : Advertised)
	{
		if (!Registered.Contains(Tool))
		{
			AddError(FString::Printf(
				TEXT("Tool '%s' is advertised to the model but has no registry definition, so its ")
				TEXT("mutation/plan-mode/truncation/budget metadata silently falls back to defaults."),
				*Tool));
		}
	}
	for (const FString& Tool : Registered)
	{
		if (!Advertised.Contains(Tool))
		{
			AddError(FString::Printf(
				TEXT("Tool '%s' has a registry definition but appears in no Config/tools_*.json."), *Tool));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_MutationSetParity, "KismetAI.ToolRegistry.MutationSetParity", ToolRegTestFlags)
bool FRegistry_MutationSetParity::RunTest(const FString& Parameters)
{
	for (const FString& Tool : FKismetAIToolRegistry::GetAllNames())
	{
		const bool bOld = InList(OldMutationTools, UE_ARRAY_COUNT(OldMutationTools), Tool);
		TestEqual(*FString::Printf(TEXT("%s mutation flag"), *Tool),
			FKismetAIToolRegistry::IsMutation(Tool), bOld);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_ReadOnlySetParity, "KismetAI.ToolRegistry.ReadOnlySetParity", ToolRegTestFlags)
bool FRegistry_ReadOnlySetParity::RunTest(const FString& Parameters)
{
	for (const FString& Tool : FKismetAIToolRegistry::GetAllNames())
	{
		const bool bOld = InList(OldReadOnlyTools, UE_ARRAY_COUNT(OldReadOnlyTools), Tool);
		TestEqual(*FString::Printf(TEXT("%s read-only flag"), *Tool),
			FKismetAIToolRegistry::IsIdempotentRead(Tool), bOld);
	}
	return true;
}

// Plan mode is the one place where deriving from capability could quietly widen
// what the agent may do, so it gets an exact comparison for every tool the gate
// can actually see.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_PlanModeGateParity, "KismetAI.ToolRegistry.PlanModeGateParity", ToolRegTestFlags)
bool FRegistry_PlanModeGateParity::RunTest(const FString& Parameters)
{
	for (const FString& Tool : FKismetAIToolRegistry::GetAllNames())
	{
		if (IsGateBypassing(Tool))
		{
			continue;  // returned before the gate; its verdict is unobservable
		}
		const bool bOld = InList(OldPlanModeAllowed, UE_ARRAY_COUNT(OldPlanModeAllowed), Tool);
		TestEqual(*FString::Printf(TEXT("%s plan-mode verdict"), *Tool),
			FKismetAIToolRegistry::IsAllowedInPlanMode(Tool), bOld);
	}

	// Named explicitly because they are the ones most likely to be reclassified
	// by accident: compiling writes the generated class, and plan mode has always
	// refused it.
	TestFalse(TEXT("compile_blueprint stays refused in plan mode"),
		FKismetAIToolRegistry::IsAllowedInPlanMode(TEXT("compile_blueprint")));
	TestFalse(TEXT("compile_material stays refused in plan mode"),
		FKismetAIToolRegistry::IsAllowedInPlanMode(TEXT("compile_material")));
	TestFalse(TEXT("an unknown tool fails closed"),
		FKismetAIToolRegistry::IsAllowedInPlanMode(TEXT("no_such_tool")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_TruncationParity, "KismetAI.ToolRegistry.TruncationParity", ToolRegTestFlags)
bool FRegistry_TruncationParity::RunTest(const FString& Parameters)
{
	const int32 Default = 30000;
	auto Cap = [Default](const TCHAR* Tool, const TCHAR* Mode)
	{
		return FKismetAIToolRegistry::GetMaxResultChars(Tool, Mode, Default);
	};

	// read_blueprint is the only mode-sensitive one.
	TestEqual(TEXT("read_blueprint t3d"),      Cap(TEXT("read_blueprint"), TEXT("t3d")),      30000);
	TestEqual(TEXT("read_blueprint overview"), Cap(TEXT("read_blueprint"), TEXT("overview")), 20000);
	TestEqual(TEXT("read_blueprint describe"), Cap(TEXT("read_blueprint"), TEXT("describe")), 15000);
	TestEqual(TEXT("read_blueprint no mode"),  Cap(TEXT("read_blueprint"), TEXT("")),         15000);

	TestEqual(TEXT("write_blueprint"),      Cap(TEXT("write_blueprint"), TEXT("")),      15000);
	TestEqual(TEXT("edit_blueprint"),       Cap(TEXT("edit_blueprint"), TEXT("")),       10000);
	TestEqual(TEXT("compile_blueprint"),    Cap(TEXT("compile_blueprint"), TEXT("")),     5000);
	TestEqual(TEXT("discover_tools"),       Cap(TEXT("discover_tools"), TEXT("")),        5000);
	TestEqual(TEXT("read_source"),          Cap(TEXT("read_source"), TEXT("")),          32768);
	TestEqual(TEXT("read_logs"),            Cap(TEXT("read_logs"), TEXT("")),            15000);
	TestEqual(TEXT("manage_blueprint"),     Cap(TEXT("manage_blueprint"), TEXT("")),     15000);
	TestEqual(TEXT("manage_state_machine"), Cap(TEXT("manage_state_machine"), TEXT("")), 15000);
	TestEqual(TEXT("manage_components"),    Cap(TEXT("manage_components"), TEXT("")),    10000);

	// Anything without an explicit cap falls back to the config default.
	TestEqual(TEXT("search_assets falls back"), Cap(TEXT("search_assets"), TEXT("")), Default);
	TestEqual(TEXT("unknown tool falls back"),  Cap(TEXT("no_such_tool"), TEXT("")),   Default);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_BudgetHintParity, "KismetAI.ToolRegistry.BudgetHintParity", ToolRegTestFlags)
bool FRegistry_BudgetHintParity::RunTest(const FString& Parameters)
{
	// Wrote T3D -> next turn verifies, so a small budget.
	TestTrue(TEXT("write_blueprint -> Low"),
		FKismetAIToolRegistry::GetBudgetHint(TEXT("write_blueprint")) == ENextTurnBudget::Low);

	// Read or compiled -> next turn likely writes T3D, so a large budget.
	TestTrue(TEXT("read_blueprint -> High"),
		FKismetAIToolRegistry::GetBudgetHint(TEXT("read_blueprint")) == ENextTurnBudget::High);
	TestTrue(TEXT("compile_blueprint -> High"),
		FKismetAIToolRegistry::GetBudgetHint(TEXT("compile_blueprint")) == ENextTurnBudget::High);

	// Discovered tools or made structural changes -> next turn likely writes.
	TestTrue(TEXT("discover_tools -> High"),
		FKismetAIToolRegistry::GetBudgetHint(TEXT("discover_tools")) == ENextTurnBudget::High);
	TestTrue(TEXT("manage_blueprint -> High"),
		FKismetAIToolRegistry::GetBudgetHint(TEXT("manage_blueprint")) == ENextTurnBudget::High);
	TestTrue(TEXT("manage_components -> High"),
		FKismetAIToolRegistry::GetBudgetHint(TEXT("manage_components")) == ENextTurnBudget::High);
	TestTrue(TEXT("manage_state_machine -> High"),
		FKismetAIToolRegistry::GetBudgetHint(TEXT("manage_state_machine")) == ENextTurnBudget::High);

	// Everything else leaves the default budget alone.
	TestTrue(TEXT("edit_blueprint -> Default"),
		FKismetAIToolRegistry::GetBudgetHint(TEXT("edit_blueprint")) == ENextTurnBudget::Default);
	TestTrue(TEXT("unknown -> Default"),
		FKismetAIToolRegistry::GetBudgetHint(TEXT("no_such_tool")) == ENextTurnBudget::Default);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_DiscoveryCategoryParity, "KismetAI.ToolRegistry.DiscoveryCategoryParity", ToolRegTestFlags)
bool FRegistry_DiscoveryCategoryParity::RunTest(const FString& Parameters)
{
	auto CheckCategory = [this](const TCHAR* Category, const TArray<FString>& Expected)
	{
		TArray<FString> Got = FKismetAIToolRegistry::GetNamesInCategory(Category);
		TArray<FString> Want = Expected;
		Want.Sort();
		TestEqual(*FString::Printf(TEXT("%s membership"), Category), FString::Join(Got, TEXT(",")), FString::Join(Want, TEXT(",")));
	};

	CheckCategory(TEXT("blueprint_tools"),
		{ TEXT("manage_blueprint"), TEXT("manage_components"), TEXT("manage_state_machine"), TEXT("layout_graph") });
	CheckCategory(TEXT("project_tools"),
		{ TEXT("get_references"), TEXT("create_asset"), TEXT("manage_asset"), TEXT("manage_animation") });
	CheckCategory(TEXT("discovery_tools"),
		{ TEXT("search_nodes"), TEXT("inspect_class") });
	CheckCategory(TEXT("material_tools"),
		{ TEXT("read_material"), TEXT("create_material_expression"), TEXT("edit_material"),
		  TEXT("compile_material"), TEXT("create_material_instance"),
		  TEXT("set_material_instance_parameter"), TEXT("manage_material") });
	CheckCategory(TEXT("widget_tools"),
		{ TEXT("read_widget"), TEXT("edit_widget") });

	TestEqual(TEXT("no extra categories"), FKismetAIToolRegistry::GetCategoryNames().Num(), 5);

	// Core tools are always advertised, so they belong to no category.
	TestEqual(TEXT("core tools are uncategorised"),
		FKismetAIToolRegistry::GetNamesInCategory(TEXT("blueprint_tools")).Contains(TEXT("read_blueprint")), false);
	return true;
}

// A tool cannot be both a mutation and an idempotent read; the loop treats the
// two exclusively and would apply the wrong short-circuit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_FlagsAreExclusive, "KismetAI.ToolRegistry.FlagsAreExclusive", ToolRegTestFlags)
bool FRegistry_FlagsAreExclusive::RunTest(const FString& Parameters)
{
	for (const FString& Tool : FKismetAIToolRegistry::GetAllNames())
	{
		const FKismetAIToolDefinition* D = FKismetAIToolRegistry::Find(Tool);
		if (!D) { continue; }
		TestFalse(*FString::Printf(TEXT("%s is not both mutation and read"), *Tool),
			D->bIsMutation && D->bIsIdempotentRead);
		// A mutation must never be permitted in plan mode.
		TestFalse(*FString::Printf(TEXT("%s: mutation implies plan-mode refusal"), *Tool),
			D->bIsMutation && D->IsAllowedInPlanMode());
	}
	return true;
}

// F8b: what enter_plan_mode ADVERTISES to the model and what the plan gate
// ENFORCES must be the same set. They were not — the hand-written advertised
// list omitted read_material and read_widget, which the gate has always
// permitted, so the model planned blind to materials and widgets on every plan
// phase. Both now derive from the registry; this pins them together.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_PlanModeAdvertisedMatchesEnforced, "KismetAI.ToolRegistry.PlanModeAdvertisedMatchesEnforced", ToolRegTestFlags)
bool FRegistry_PlanModeAdvertisedMatchesEnforced::RunTest(const FString& Parameters)
{
	// What enter_plan_mode tells the model (it drops enter_plan_mode itself,
	// since the model is already in plan mode).
	TArray<FString> Advertised = FKismetAIToolRegistry::GetPlanModeAllowedNames();
	Advertised.Remove(TEXT("enter_plan_mode"));

	for (const FString& Tool : Advertised)
	{
		TestTrue(*FString::Printf(TEXT("advertised '%s' is actually permitted"), *Tool),
			FKismetAIToolRegistry::IsAllowedInPlanMode(Tool));
	}

	for (const FString& Tool : FKismetAIToolRegistry::GetAllNames())
	{
		if (Tool == TEXT("enter_plan_mode")) { continue; }
		if (FKismetAIToolRegistry::IsAllowedInPlanMode(Tool))
		{
			TestTrue(*FString::Printf(TEXT("permitted '%s' is actually advertised"), *Tool),
				Advertised.Contains(Tool));
		}
	}

	// The two the old hand-written list silently dropped.
	TestTrue(TEXT("read_material is advertised in plan mode"), Advertised.Contains(TEXT("read_material")));
	TestTrue(TEXT("read_widget is advertised in plan mode"), Advertised.Contains(TEXT("read_widget")));
	// And the things that must never appear.
	TestFalse(TEXT("write_blueprint is never advertised"), Advertised.Contains(TEXT("write_blueprint")));
	TestFalse(TEXT("compile_blueprint is never advertised"), Advertised.Contains(TEXT("compile_blueprint")));
	return true;
}
