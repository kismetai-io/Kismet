// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// The set of tools we advertise to the model and the set we can actually
// run must be the same set.
//
// They were not. layout_graph had a schema in tools_extended.json, an entry in
// the discover_tools category map, argument marshalling, a validator and an HTTP
// route — but no branch in ExecuteTool's 26-way if/else chain, so every call the
// model made returned UNKNOWN_TOOL. Nothing reported it; the run just degraded
// in a way that looked like model failure.
//
// These tests close that hole in both directions.

#include "KismetAIAgent.h"
#include "KismetAIModuleToolInvoker.h"
#include "KismetAIEmbeddedConfig.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

constexpr auto ManifestTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	// Tools the agent answers itself, without a module handler: they touch run
	// state (plan mode, the ask_user channel, the todo list, tool discovery)
	// rather than the editor. Spelled out here on purpose — a new in-process
	// tool has to be added to this list deliberately, which is the point.
	const TCHAR* const InProcessTools[] = {
		TEXT("enter_plan_mode"),
		TEXT("exit_plan_mode"),
		TEXT("ask_user"),
		TEXT("update_todos"),
		TEXT("discover_tools"),
	};

	const TCHAR* const ToolConfigFiles[] = {
		TEXT("tools_core.json"),
		TEXT("tools_extended.json"),
		TEXT("tools_project.json"),
		TEXT("tools_material.json"),
		TEXT("tools_widget.json"),
		TEXT("tools_discovery.json"),
	};

	FString PluginConfigDir()
	{
		FString PluginDir = FPaths::GetPath(FModuleManager::Get().GetModuleFilename(TEXT("KismetAI")));
		PluginDir = FPaths::GetPath(FPaths::GetPath(PluginDir));  // up from Binaries/<Platform>/
		return PluginDir / TEXT("Config");
	}

	// Every tool name advertised to the model across all six schema files.
	bool CollectAdvertisedToolNames(TSet<FString>& Out, FString& OutError)
	{
		const FString ConfigDir = PluginConfigDir();
		for (const TCHAR* FileName : ToolConfigFiles)
		{
			FString Json;
			if (!KismetAIEmbeddedConfig::LoadConfigFile(FileName, ConfigDir / FileName, Json))
			{
				OutError = FString::Printf(TEXT("could not load %s"), FileName);
				return false;
			}

			TArray<TSharedPtr<FJsonValue>> Tools;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
			if (!FJsonSerializer::Deserialize(Reader, Tools))
			{
				OutError = FString::Printf(TEXT("could not parse %s"), FileName);
				return false;
			}

			for (const TSharedPtr<FJsonValue>& Val : Tools)
			{
				const TSharedPtr<FJsonObject> Obj = Val->AsObject();
				FString Name;
				if (Obj.IsValid() && Obj->TryGetStringField(TEXT("name"), Name))
				{
					Out.Add(Name);
				}
			}
		}
		return true;
	}

	TSet<FString> InProcessSet()
	{
		TSet<FString> S;
		for (const TCHAR* N : InProcessTools) { S.Add(FString(N)); }
		return S;
	}
}

// Forward: nothing may be advertised that cannot be run. This is the test that
// would have caught layout_graph.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManifest_EveryAdvertisedToolIsDispatchable, "KismetAI.ToolManifest.EveryAdvertisedToolIsDispatchable", ManifestTestFlags)
bool FManifest_EveryAdvertisedToolIsDispatchable::RunTest(const FString& Parameters)
{
	TSet<FString> Advertised;
	FString Error;
	if (!CollectAdvertisedToolNames(Advertised, Error))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("found the advertised tools"), Advertised.Num() >= 30);

	const TSet<FString> Dispatchable = FKismetAIModuleToolInvoker::DispatchableToolNames();
	const TSet<FString> InProcess = InProcessSet();

	for (const FString& Tool : Advertised)
	{
		if (!Dispatchable.Contains(Tool) && !InProcess.Contains(Tool))
		{
			AddError(FString::Printf(
				TEXT("Tool '%s' is advertised to the model but has no handler and is not in-process. ")
				TEXT("It will return UNKNOWN_TOOL at runtime. Add it to the dispatch table in ")
				TEXT("KismetAIModuleToolInvoker.cpp, or to InProcessTools in this test if the agent handles it."),
				*Tool));
		}
	}
	return true;
}

// Reverse: nothing may be wired that no schema mentions — a handler bound to a
// name the model never sees is dead weight and usually a rename that half-landed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManifest_EveryDispatchableToolIsAdvertised, "KismetAI.ToolManifest.EveryDispatchableToolIsAdvertised", ManifestTestFlags)
bool FManifest_EveryDispatchableToolIsAdvertised::RunTest(const FString& Parameters)
{
	TSet<FString> Advertised;
	FString Error;
	if (!CollectAdvertisedToolNames(Advertised, Error))
	{
		AddError(Error);
		return false;
	}

	for (const FString& Tool : FKismetAIModuleToolInvoker::DispatchableToolNames())
	{
		if (!Advertised.Contains(Tool))
		{
			AddError(FString::Printf(
				TEXT("Tool '%s' has a handler but appears in no Config/tools_*.json, so the model ")
				TEXT("can never call it."), *Tool));
		}
	}
	return true;
}

// The regression this task exists for.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManifest_LayoutGraphIsDispatchable, "KismetAI.ToolManifest.LayoutGraphIsDispatchable", ManifestTestFlags)
bool FManifest_LayoutGraphIsDispatchable::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("layout_graph is dispatchable"),
		FKismetAIModuleToolInvoker::DispatchableToolNames().Contains(TEXT("layout_graph")));
	TestEqual(TEXT("layout_graph routes to DoLayoutGraph"),
		FKismetAIModuleToolInvoker::HandlerNameFor(TEXT("layout_graph"), FString()),
		FString(TEXT("DoLayoutGraph")));
	return true;
}

// Mode routing: the three read tools pick a handler from their "mode" argument.
// Absent or unrecognised modes must fall back to the documented default.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManifest_ReadBlueprintModeRouting, "KismetAI.ToolManifest.ReadBlueprintModeRouting", ManifestTestFlags)
bool FManifest_ReadBlueprintModeRouting::RunTest(const FString& Parameters)
{
	auto Resolve = [](const TCHAR* Mode) {
		return FKismetAIModuleToolInvoker::HandlerNameFor(TEXT("read_blueprint"), Mode);
	};
	TestEqual(TEXT("t3d"),        Resolve(TEXT("t3d")),      FString(TEXT("DoExportT3D")));
	TestEqual(TEXT("overview"),   Resolve(TEXT("overview")), FString(TEXT("DoBlueprintOverview")));
	TestEqual(TEXT("describe"),   Resolve(TEXT("describe")), FString(TEXT("DoDescribeGraph")));
	TestEqual(TEXT("absent"),     Resolve(TEXT("")),         FString(TEXT("DoDescribeGraph")));
	TestEqual(TEXT("unknown"),    Resolve(TEXT("nonsense")), FString(TEXT("DoDescribeGraph")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManifest_ReadMaterialModeRouting, "KismetAI.ToolManifest.ReadMaterialModeRouting", ManifestTestFlags)
bool FManifest_ReadMaterialModeRouting::RunTest(const FString& Parameters)
{
	auto Resolve = [](const TCHAR* Mode) {
		return FKismetAIModuleToolInvoker::HandlerNameFor(TEXT("read_material"), Mode);
	};
	TestEqual(TEXT("overview"), Resolve(TEXT("overview")), FString(TEXT("DoMaterialOverview")));
	TestEqual(TEXT("describe"), Resolve(TEXT("describe")), FString(TEXT("DoDescribeMaterial")));
	TestEqual(TEXT("absent"),   Resolve(TEXT("")),         FString(TEXT("DoDescribeMaterial")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManifest_ReadWidgetModeRouting, "KismetAI.ToolManifest.ReadWidgetModeRouting", ManifestTestFlags)
bool FManifest_ReadWidgetModeRouting::RunTest(const FString& Parameters)
{
	auto Resolve = [](const TCHAR* Mode) {
		return FKismetAIModuleToolInvoker::HandlerNameFor(TEXT("read_widget"), Mode);
	};
	TestEqual(TEXT("overview"), Resolve(TEXT("overview")), FString(TEXT("FKismetAIWidgetService::Overview")));
	// read_widget's documented default mode is "tree", not "describe".
	TestEqual(TEXT("tree"),     Resolve(TEXT("tree")),     FString(TEXT("FKismetAIWidgetService::Describe")));
	TestEqual(TEXT("absent"),   Resolve(TEXT("")),         FString(TEXT("FKismetAIWidgetService::Describe")));
	return true;
}

// An unknown tool must still resolve to nothing, so ExecuteTool keeps returning
// UNKNOWN_TOOL rather than dispatching something arbitrary.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManifest_UnknownToolResolvesToNothing, "KismetAI.ToolManifest.UnknownToolResolvesToNothing", ManifestTestFlags)
bool FManifest_UnknownToolResolvesToNothing::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("unknown tool has no handler"),
		FKismetAIModuleToolInvoker::HandlerNameFor(TEXT("no_such_tool"), FString()).IsEmpty());
	return true;
}
