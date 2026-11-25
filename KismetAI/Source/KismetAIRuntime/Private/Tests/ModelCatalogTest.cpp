// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIModelCatalog.h"
#include "KismetAIAgent.h"
#include "KismetAILLMProvider.h"
#include "KismetAIAnthropicProvider.h"
#include "KismetAIOpenAIProvider.h"
#include "KismetAIMinimaxProvider.h"
#include "KismetAIProviderProfile.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

constexpr auto ModelCatalogTestFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	FKismetAIAgentConfig MakeCatalogTestConfig(const FString& Model)
	{
		FKismetAIAgentConfig Cfg;
		Cfg.Model = Model;
		Cfg.ApiKey = TEXT("sk-ant-test");
		Cfg.OpenAIApiKey = TEXT("sk-openai-test");
		Cfg.MinimaxApiKey = TEXT("sk-minimax-test");
		Cfg.MaxTokens = 16000;
		Cfg.ThinkingMode = EThinkingMode::Adaptive;
		Cfg.EffortLevel = EEffortLevel::High;
		return Cfg;
	}

	TSharedPtr<FJsonObject> BuildBody(const ILLMProvider& Provider, const FKismetAIAgentConfig& Cfg, FLLMRequestShape& Shape)
	{
		TArray<TSharedPtr<FJsonValue>> Messages;
		TSharedPtr<FJsonObject> User = MakeShared<FJsonObject>();
		User->SetStringField(TEXT("role"), TEXT("user"));
		User->SetStringField(TEXT("content"), TEXT("hello"));
		Messages.Add(MakeShared<FJsonValueObject>(User));
		TArray<TSharedPtr<FJsonValue>> Tools;
		return Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("system"), Shape);
	}

	FString JoinedHeaders(const ILLMProvider& Provider, const FKismetAIAgentConfig& Cfg, const FLLMRequestShape& Shape)
	{
		return FString::Join(Provider.GetExtraCurlHeaders(Cfg, Shape), TEXT("\n"));
	}
}

// ── The shipped file ─────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCatalog_ShippedFileLoads,
	"KismetAI.ModelCatalog.ShippedFileLoads", ModelCatalogTestFlags)
bool FModelCatalog_ShippedFileLoads::RunTest(const FString& Parameters)
{
	const FKismetAIModelCatalog& Catalog = FKismetAIModelCatalog::Get();
	TestTrue(TEXT("Config/models.json loads and validates"), Catalog.IsLoaded());
	TestTrue(TEXT("catalog has picker rows"), Catalog.GetPickerModels().Num() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCatalog_PickerRowsAreComplete,
	"KismetAI.ModelCatalog.PickerRowsAreComplete", ModelCatalogTestFlags)
bool FModelCatalog_PickerRowsAreComplete::RunTest(const FString& Parameters)
{
	static const TSet<FString> KnownTiers = { TEXT("premium"), TEXT("standard"), TEXT("fast") };
	for (const FKismetAIModelEntry* Entry : FKismetAIModelCatalog::Get().GetPickerModels())
	{
		const FString Name = Entry->Id;
		TestFalse(Name + TEXT(": has an alias"), Entry->Alias.IsEmpty());
		TestFalse(Name + TEXT(": has a display name"), Entry->DisplayName.IsEmpty());
		TestFalse(Name + TEXT(": has a provider"), Entry->Provider.IsEmpty());
		TestTrue(Name + TEXT(": tier is known"), KnownTiers.Contains(Entry->Tier));
		TestTrue(Name + TEXT(": has a context window"), Entry->ContextWindow > 0);
		TestTrue(Name + TEXT(": has a price"), Entry->Price.Input > 0.0 && Entry->Price.Output > 0.0);
		TestEqual(Name + TEXT(": alias resolves to this model"),
			FKismetAIAgent::ResolveModelAlias(Entry->Alias), Entry->Id);
		if (!Entry->RecommendedEffort.IsEmpty())
		{
			TestTrue(Name + TEXT(": recommended effort is one the model accepts"),
				Entry->Caps.EffortLevels.Contains(Entry->RecommendedEffort)
				|| Entry->Caps.bReasoning);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCatalog_NewLineupIsListed,
	"KismetAI.ModelCatalog.NewLineupIsListed", ModelCatalogTestFlags)
bool FModelCatalog_NewLineupIsListed::RunTest(const FString& Parameters)
{
	TSet<FString> Listed;
	for (const FKismetAIModelEntry* Entry : FKismetAIModelCatalog::Get().GetPickerModels())
	{
		Listed.Add(Entry->Id);
	}
	for (const TCHAR* Id : { TEXT("claude-opus-5-5"), TEXT("claude-fable-5-2"), TEXT("claude-sonnet-5-5"),
		TEXT("claude-haiku-5-5"), TEXT("gpt-6-astra"), TEXT("gpt-6.1-sol"), TEXT("gpt-6.1-terra"), TEXT("gpt-6-luna") })
	{
		TestTrue(FString::Printf(TEXT("%s is in the picker"), Id), Listed.Contains(Id));
	}
	for (const TCHAR* Id : { TEXT("claude-fable-5-1"), TEXT("claude-opus-4-8"), TEXT("claude-sonnet-4-6"), TEXT("claude-haiku-4-5-20251001"), TEXT("gpt-5.5") })
	{
		TestFalse(FString::Printf(TEXT("%s is no longer a picker row"), Id), Listed.Contains(Id));
		TestEqual(FString::Printf(TEXT("%s still resolves for saved settings"), Id),
			FKismetAIAgent::ResolveModelAlias(Id), FString(Id));
	}
	TestEqual(TEXT("opus -> Opus 5.5"), FKismetAIAgent::ResolveModelAlias(TEXT("opus")), FString(TEXT("claude-opus-5-5")));
	TestEqual(TEXT("sonnet -> Sonnet 5.5"), FKismetAIAgent::ResolveModelAlias(TEXT("sonnet")), FString(TEXT("claude-sonnet-5-5")));
	TestEqual(TEXT("haiku -> Haiku 5.5"), FKismetAIAgent::ResolveModelAlias(TEXT("haiku")), FString(TEXT("claude-haiku-5-5")));
	TestEqual(TEXT("default model is Opus 5.5"), FKismetAIAgentConfig().Model, FString(TEXT("claude-opus-5-5")));
	return true;
}

// ── Parser rules ─────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCatalog_RejectsDuplicateNames,
	"KismetAI.ModelCatalog.RejectsDuplicateNames", ModelCatalogTestFlags)
bool FModelCatalog_RejectsDuplicateNames::RunTest(const FString& Parameters)
{
	FKismetAIModelCatalog Catalog;
	FString Error;
	const bool bOk = Catalog.LoadFromJson(TEXT(R"({"schema_version":1,"models":[
		{"id":"a","alias":"x"},
		{"id":"b","aliases":["X"]}]})"), Error);
	TestFalse(TEXT("an alias used twice is rejected"), bOk);
	TestTrue(TEXT("error names the alias"), Error.Contains(TEXT("X")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCatalog_RejectsInconsistentEntries,
	"KismetAI.ModelCatalog.RejectsInconsistentEntries", ModelCatalogTestFlags)
bool FModelCatalog_RejectsInconsistentEntries::RunTest(const FString& Parameters)
{
	FString Error;
	{
		FKismetAIModelCatalog Catalog;
		TestFalse(TEXT("fast price without a fast wire"), Catalog.LoadFromJson(TEXT(R"({"schema_version":1,"models":[
			{"id":"a","fast_pricing":{"input":1,"output":1}}]})"), Error));
	}
	{
		FKismetAIModelCatalog Catalog;
		TestFalse(TEXT("unknown effort level"), Catalog.LoadFromJson(TEXT(R"({"schema_version":1,"models":[
			{"id":"a","capabilities":{"effort":["turbo"]}}]})"), Error));
	}
	{
		FKismetAIModelCatalog Catalog;
		TestFalse(TEXT("pattern entry in the picker"), Catalog.LoadFromJson(TEXT(R"({"schema_version":1,"models":[
			{"pattern_only":true,"match":["x"],"picker":true}]})"), Error));
	}
	{
		FKismetAIModelCatalog Catalog;
		TestFalse(TEXT("unknown schema version"), Catalog.LoadFromJson(TEXT(R"({"schema_version":2,"models":[]})"), Error));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCatalog_LookupOrder,
	"KismetAI.ModelCatalog.LookupOrder", ModelCatalogTestFlags)
bool FModelCatalog_LookupOrder::RunTest(const FString& Parameters)
{
	FKismetAIModelCatalog Catalog;
	FString Error;
	const bool bOk = Catalog.LoadFromJson(TEXT(R"({"schema_version":1,
		"defaults":{"context_window":111},
		"models":[
			{"id":"fam-1","context_window":1},
			{"id":"fam-1-big","ids":["fam-1-big-latest"],"aliases":["big"],"context_window":2},
			{"pattern_only":true,"match":["^fam-"],"context_window":3}
		]})"), Error);
	TestTrue(TEXT("test catalog loads: ") + Error, bOk);
	TestEqual(TEXT("exact id"), Catalog.FindOrDefaults(TEXT("fam-1")).ContextWindow, 1);
	TestEqual(TEXT("alias"), Catalog.FindOrDefaults(TEXT("BIG")).ContextWindow, 2);
	TestEqual(TEXT("longest pattern wins over shorter id"), Catalog.FindOrDefaults(TEXT("fam-1-big-2027")).ContextWindow, 2);
	TestEqual(TEXT("substring of a platform-prefixed id"), Catalog.FindOrDefaults(TEXT("vendor.fam-1-big")).ContextWindow, 2);
	TestEqual(TEXT("family pattern"), Catalog.FindOrDefaults(TEXT("fam-9")).ContextWindow, 3);
	TestEqual(TEXT("defaults for unknown"), Catalog.FindOrDefaults(TEXT("other")).ContextWindow, 111);
	TestEqual(TEXT("ids entry resolves as written"), Catalog.Resolve(TEXT("FAM-1-BIG-LATEST")), FString(TEXT("fam-1-big-latest")));
	TestEqual(TEXT("pattern matches do not resolve"), Catalog.Resolve(TEXT("fam-9")), FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCatalog_ClampEffort,
	"KismetAI.ModelCatalog.ClampEffort", ModelCatalogTestFlags)
bool FModelCatalog_ClampEffort::RunTest(const FString& Parameters)
{
	const TArray<FString> ThreeLevels = { TEXT("low"), TEXT("medium"), TEXT("high") };
	TestEqual(TEXT("max clamps to high"), FKismetAIModelCatalog::ClampEffort(ThreeLevels, TEXT("max")), FString(TEXT("high")));
	TestEqual(TEXT("medium stays"), FKismetAIModelCatalog::ClampEffort(ThreeLevels, TEXT("medium")), FString(TEXT("medium")));
	TestEqual(TEXT("below every level -> lowest"), FKismetAIModelCatalog::ClampEffort({ TEXT("high"), TEXT("max") }, TEXT("low")), FString(TEXT("high")));
	TestEqual(TEXT("no levels -> empty"), FKismetAIModelCatalog::ClampEffort({}, TEXT("high")), FString());
	return true;
}

// ── Pricing ──────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCatalog_CurrentPrices,
	"KismetAI.ModelCatalog.CurrentPrices", ModelCatalogTestFlags)
bool FModelCatalog_CurrentPrices::RunTest(const FString& Parameters)
{
	// platform.claude.com/docs/en/about-claude/pricing and
	// developers.openai.com/api/docs/pricing, read 2026-11-06.
	FModelCost Opus = FKismetAIAgent::GetModelCost(TEXT("claude-opus-5-5"));
	TestEqual(TEXT("Opus 5.5 input"), Opus.InputPerMillion, 4.0);
	TestEqual(TEXT("Opus 5.5 output"), Opus.OutputPerMillion, 20.0);
	TestEqual(TEXT("Opus 5.5 cache read"), Opus.CacheReadPerMillion, 0.2);
	TestEqual(TEXT("Opus 5.5 cache write"), Opus.CacheWritePerMillion, 5.0);
	FModelCost OpusFast = FKismetAIAgent::GetModelCost(TEXT("claude-opus-5-5"), true);
	TestEqual(TEXT("Opus 5.5 fast input"), OpusFast.InputPerMillion, 8.0);
	TestEqual(TEXT("Opus 5.5 fast output"), OpusFast.OutputPerMillion, 40.0);

	TestEqual(TEXT("Fable 5.1 output"), FKismetAIAgent::GetModelCost(TEXT("claude-fable-5-1")).OutputPerMillion, 50.0);
	TestEqual(TEXT("Sonnet 5.5 input"), FKismetAIAgent::GetModelCost(TEXT("claude-sonnet-5-5")).InputPerMillion, 2.0);

	TestEqual(TEXT("Haiku 5.5 short prompt"), FKismetAIAgent::GetModelCost(TEXT("claude-haiku-5-5"), false, 50000).InputPerMillion, 0.1);
	TestEqual(TEXT("Haiku 5.5 long prompt"), FKismetAIAgent::GetModelCost(TEXT("claude-haiku-5-5"), false, 150000).InputPerMillion, 0.5);
	TestEqual(TEXT("Haiku 5.5 long prompt output"), FKismetAIAgent::GetModelCost(TEXT("claude-haiku-5-5"), false, 150000).OutputPerMillion, 2.5);

	TestEqual(TEXT("GPT-6.1 Sol input"), FKismetAIAgent::GetModelCost(TEXT("gpt-6.1-sol")).InputPerMillion, 2.0);
	TestEqual(TEXT("GPT-6.1 Sol >272K input"), FKismetAIAgent::GetModelCost(TEXT("gpt-6.1-sol"), false, 300000).InputPerMillion, 4.0);
	TestEqual(TEXT("GPT-6 Astra fast output"), FKismetAIAgent::GetModelCost(TEXT("gpt-6-astra"), true).OutputPerMillion, 100.0);
	TestEqual(TEXT("OpenAI has no cache-write charge"), FKismetAIAgent::GetModelCost(TEXT("gpt-6-luna")).CacheWritePerMillion, 0.0);
	return true;
}

// ── Request shapes for the new lineup ────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCatalog_Claude5RequestShape,
	"KismetAI.ModelCatalog.Claude5RequestShape", ModelCatalogTestFlags)
bool FModelCatalog_Claude5RequestShape::RunTest(const FString& Parameters)
{
	FAnthropicProvider Provider;
	for (const TCHAR* Model : { TEXT("claude-opus-5-5"), TEXT("claude-fable-5-2"), TEXT("claude-fable-5-1"), TEXT("claude-sonnet-5-5"),
		TEXT("claude-haiku-5-5"), TEXT("claude-opus-5-5-20270101") })
	{
		for (EThinkingMode Mode : { EThinkingMode::Adaptive, EThinkingMode::Enabled, EThinkingMode::Disabled })
		{
			FKismetAIAgentConfig Cfg = MakeCatalogTestConfig(Model);
			Cfg.ThinkingMode = Mode;
			Cfg.EffortLevel = EEffortLevel::Max;
			FLLMRequestShape Shape;
			TSharedPtr<FJsonObject> Body = BuildBody(Provider, Cfg, Shape);
			const FString Label = FString::Printf(TEXT("%s mode %d: "), Model, static_cast<int32>(Mode));

			// The API rejects budget_tokens, thinking:disabled and sampling params on these models.
			const TSharedPtr<FJsonObject>* Thinking = nullptr;
			TestTrue(Label + TEXT("thinking is always sent"), Body->TryGetObjectField(TEXT("thinking"), Thinking) && Thinking);
			if (Thinking)
			{
				TestEqual(Label + TEXT("thinking is adaptive"), (*Thinking)->GetStringField(TEXT("type")), FString(TEXT("adaptive")));
				TestFalse(Label + TEXT("no budget_tokens"), (*Thinking)->HasField(TEXT("budget_tokens")));
				const TSharedPtr<FJsonObject>* Binding = nullptr;
				TestTrue(Label + TEXT("block_binding present"), (*Thinking)->TryGetObjectField(TEXT("block_binding"), Binding) && Binding);
				if (Binding)
				{
					TestEqual(Label + TEXT("drop_block"), (*Binding)->GetStringField(TEXT("prefix_mismatch_behavior")), FString(TEXT("drop_block")));
				}
			}
			TestFalse(Label + TEXT("no temperature"), Body->HasField(TEXT("temperature")));

			const TSharedPtr<FJsonObject>* OutputConfig = nullptr;
			TestTrue(Label + TEXT("effort sent"), Body->TryGetObjectField(TEXT("output_config"), OutputConfig) && OutputConfig);
			if (OutputConfig)
			{
				TestEqual(Label + TEXT("max passes through"), (*OutputConfig)->GetStringField(TEXT("effort")), FString(TEXT("max")));
			}

			const FString Headers = JoinedHeaders(Provider, Cfg, Shape);
			TestTrue(Label + TEXT("block-binding beta header"), Headers.Contains(TEXT("thinking-binding-controls-2026-08-01")));
			TestFalse(Label + TEXT("no interleaved-thinking beta"), Headers.Contains(TEXT("interleaved-thinking")));
			TestFalse(Label + TEXT("no effort beta"), Headers.Contains(TEXT("effort-2025-11-24")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCatalog_AnthropicFastModeOnTheWire,
	"KismetAI.ModelCatalog.AnthropicFastModeOnTheWire", ModelCatalogTestFlags)
bool FModelCatalog_AnthropicFastModeOnTheWire::RunTest(const FString& Parameters)
{
	FAnthropicProvider Provider;
	{
		FKismetAIAgentConfig Cfg = MakeCatalogTestConfig(TEXT("claude-opus-5-5"));
		Cfg.bFastMode = true;
		FLLMRequestShape Shape;
		TSharedPtr<FJsonObject> Body = BuildBody(Provider, Cfg, Shape);
		TestEqual(TEXT("Opus 5.5 fast sends speed:fast"), Body->GetStringField(TEXT("speed")), FString(TEXT("fast")));
		TestFalse(TEXT("Opus 5.5 fast needs no beta"), JoinedHeaders(Provider, Cfg, Shape).Contains(TEXT("fast-mode-2026-02-01")));
	}
	{
		FKismetAIAgentConfig Cfg = MakeCatalogTestConfig(TEXT("claude-opus-4-8"));
		Cfg.bFastMode = true;
		FLLMRequestShape Shape;
		TSharedPtr<FJsonObject> Body = BuildBody(Provider, Cfg, Shape);
		// The research preview ended 2026-11-30: the toggle is ignored for Opus 4.8.
		TestFalse(TEXT("Opus 4.8 sends no speed field"), Body->HasField(TEXT("speed")));
		TestFalse(TEXT("Opus 4.8 sends no fast-mode beta"), JoinedHeaders(Provider, Cfg, Shape).Contains(TEXT("fast-mode-2026-02-01")));
	}
	{
		FKismetAIAgentConfig Cfg = MakeCatalogTestConfig(TEXT("claude-sonnet-5-5"));
		Cfg.bFastMode = true;
		FLLMRequestShape Shape;
		TSharedPtr<FJsonObject> Body = BuildBody(Provider, Cfg, Shape);
		TestFalse(TEXT("Sonnet 5.5 has no fast tier: speed omitted"), Body->HasField(TEXT("speed")));
	}
	{
		FKismetAIAgentConfig Cfg = MakeCatalogTestConfig(TEXT("claude-opus-5-5"));
		FLLMRequestShape Shape;
		TSharedPtr<FJsonObject> Body = BuildBody(Provider, Cfg, Shape);
		TestFalse(TEXT("fast mode off: speed omitted"), Body->HasField(TEXT("speed")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCatalog_GPT6RequestShape,
	"KismetAI.ModelCatalog.GPT6RequestShape", ModelCatalogTestFlags)
bool FModelCatalog_GPT6RequestShape::RunTest(const FString& Parameters)
{
	FOpenAIProvider Provider;
	for (const TCHAR* Model : { TEXT("gpt-6-astra"), TEXT("gpt-6.1-sol"), TEXT("gpt-6.1-terra"), TEXT("gpt-6-luna") })
	{
		FKismetAIAgentConfig Cfg = MakeCatalogTestConfig(Model);
		Cfg.EffortLevel = EEffortLevel::XHigh;
		Cfg.bFastMode = true;
		FLLMRequestShape Shape;
		TSharedPtr<FJsonObject> Body = BuildBody(Provider, Cfg, Shape);
		const FString Label = FString(Model) + TEXT(": ");

		// Before the catalog, gpt-6 ids failed the gpt-5/o-series prefix test and
		// were sent temperature with no reasoning effort.
		TestFalse(Label + TEXT("no temperature"), Body->HasField(TEXT("temperature")));
		const TSharedPtr<FJsonObject>* Reasoning = nullptr;
		TestTrue(Label + TEXT("reasoning sent"), Body->TryGetObjectField(TEXT("reasoning"), Reasoning) && Reasoning);
		if (Reasoning)
		{
			TestEqual(Label + TEXT("xhigh passes through"), (*Reasoning)->GetStringField(TEXT("effort")), FString(TEXT("xhigh")));
		}
		TestEqual(Label + TEXT("fast mode uses the priority/fast tier"), Body->GetStringField(TEXT("service_tier")), FString(TEXT("priority")));
	}
	{
		FKismetAIAgentConfig Cfg = MakeCatalogTestConfig(TEXT("gpt-5.4-nano"));
		Cfg.EffortLevel = EEffortLevel::Max;
		Cfg.bFastMode = true;
		FLLMRequestShape Shape;
		TSharedPtr<FJsonObject> Body = BuildBody(Provider, Cfg, Shape);
		const TSharedPtr<FJsonObject>* Reasoning = nullptr;
		if (TestTrue(TEXT("gpt-5.4-nano reasoning sent"), Body->TryGetObjectField(TEXT("reasoning"), Reasoning) && Reasoning))
		{
			TestEqual(TEXT("GPT-5.x clamps max to high"), (*Reasoning)->GetStringField(TEXT("effort")), FString(TEXT("high")));
		}
		TestFalse(TEXT("gpt-5.4-nano has no fast tier"), Body->HasField(TEXT("service_tier")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCatalog_MinimaxFastSwap,
	"KismetAI.ModelCatalog.MinimaxFastSwap", ModelCatalogTestFlags)
bool FModelCatalog_MinimaxFastSwap::RunTest(const FString& Parameters)
{
	FMinimaxProvider Provider;
	FKismetAIAgentConfig Cfg = MakeCatalogTestConfig(TEXT("MiniMax-M2.7"));
	Cfg.bFastMode = true;
	FLLMRequestShape Shape;
	TestEqual(TEXT("fast M2.7 sends the highspeed id"),
		BuildBody(Provider, Cfg, Shape)->GetStringField(TEXT("model")), FString(TEXT("MiniMax-M2.7-highspeed")));
	Cfg.Model = TEXT("MiniMax-M3");
	TestEqual(TEXT("M3 has no swap"),
		BuildBody(Provider, Cfg, Shape)->GetStringField(TEXT("model")), FString(TEXT("MiniMax-M3")));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCatalog_UnknownCapabilitiesAreConservative,
	"KismetAI.ModelCatalog.UnknownCapabilitiesAreConservative", ModelCatalogTestFlags)
bool FModelCatalog_UnknownCapabilitiesAreConservative::RunTest(const FString& Parameters)
{
	FKismetAIModelCatalog Catalog;
	FString Error;
	TestTrue(TEXT("fixture loads"), Catalog.LoadFromJson(TEXT(R"({"schema_version":1,
		"defaults":{"context_window":200000,"capabilities":{"thinking":true,"budget_thinking":true,"legacy_thinking_beta":true}},
		"models":[{"id":"known-legacy","context_window":200000}]})"), Error));
	const FKismetAIModelEntry& Unknown = Catalog.FindOrDefaults(TEXT("unlisted-local-model"));
	TestFalse(TEXT("unknown does not invent thinking support"), Unknown.Caps.bThinking);
	TestFalse(TEXT("unknown does not invent budget thinking support"), Unknown.Caps.bBudgetThinking);
	TestFalse(TEXT("unknown does not invent legacy beta support"), Unknown.Caps.bLegacyThinkingBeta);
	TestFalse(TEXT("unknown does not invent reasoning support"), Unknown.Caps.bReasoning);
	TestTrue(TEXT("unknown has no effort levels"), Unknown.Caps.EffortLevels.IsEmpty());
	const FKismetAIModelEntry& Known = Catalog.FindOrDefaults(TEXT("known-legacy"));
	TestTrue(TEXT("known inherits declared thinking"), Known.Caps.bThinking);
	TestTrue(TEXT("known inherits declared budget thinking"), Known.Caps.bBudgetThinking);
	TestTrue(TEXT("known inherits declared beta"), Known.Caps.bLegacyThinkingBeta);
	TestEqual(TEXT("known context unchanged"), Known.ContextWindow, 200000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCatalog_UnknownRequestOmitsThinking,
	"KismetAI.ModelCatalog.UnknownRequestOmitsThinking", ModelCatalogTestFlags)
bool FModelCatalog_UnknownRequestOmitsThinking::RunTest(const FString& Parameters)
{
	FAnthropicProvider Provider;
	FKismetAIAgentConfig Config = MakeCatalogTestConfig(TEXT("unlisted-local-model"));
	FLLMRequestShape Shape;
	const TSharedPtr<FJsonObject> Body = BuildBody(Provider, Config, Shape);
	TestFalse(TEXT("no assumed thinking field reaches the request"), Body->HasField(TEXT("thinking")));
	TestFalse(TEXT("request shape records thinking absent"), Shape.bThinkingEnabled);
	TestFalse(TEXT("no unverified beta reaches headers"), JoinedHeaders(Provider, Config, Shape).Contains(TEXT("interleaved-thinking")));
	return true;
}

// ── Model list parity with Eden (2026-11-25) ─────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCatalog_Fable52,
	"KismetAI.ModelCatalog.Fable52", ModelCatalogTestFlags)
bool FModelCatalog_Fable52::RunTest(const FString& Parameters)
{
	const FKismetAIModelCatalog& Catalog = FKismetAIModelCatalog::Get();
	for (const TCHAR* Name : { TEXT("claude-fable-5-2"), TEXT("fable"), TEXT("fable-5-2"), TEXT("fable-5.2") })
	{
		TestEqual(FString::Printf(TEXT("%s -> Fable 5.2"), Name), Catalog.Resolve(Name), FString(TEXT("claude-fable-5-2")));
	}
	const FKismetAIModelEntry* Fable = Catalog.Find(TEXT("fable"));
	if (!TestNotNull(TEXT("Fable 5.2 found"), Fable)) return false;
	TestTrue(TEXT("in the picker"), Fable->bInPicker);
	TestEqual(TEXT("1M context"), Fable->ContextWindow, 1000000);
	TestEqual(TEXT("128k output"), Fable->MaxOutputTokens, 128000);
	TestEqual(TEXT("input $8"), Fable->Price.Input, 8.0);
	TestEqual(TEXT("output $40"), Fable->Price.Output, 40.0);
	TestEqual(TEXT("cache read $0.20"), Fable->Price.CacheRead, 0.2);
	TestEqual(TEXT("cache write $10"), Fable->Price.CacheWrite, 10.0);
	TestTrue(TEXT("adaptive thinking always on"), Fable->Caps.bAdaptiveThinking && Fable->Caps.bThinkingAlwaysOn && !Fable->Caps.bBudgetThinking);
	TestEqual(TEXT("effort low..max"), FString::Join(Fable->Caps.EffortLevels, TEXT(",")), FString(TEXT("low,medium,high,xhigh,max")));
	TestTrue(TEXT("same request shape as 5.1: block binding"), Fable->Caps.bThinkingBlockBinding);

	const FKismetAIModelEntry* Old = Catalog.Find(TEXT("fable-5.1"));
	if (!TestNotNull(TEXT("Fable 5.1 still resolves"), Old)) return false;
	TestEqual(TEXT("5.1 id"), Old->Id, FString(TEXT("claude-fable-5-1")));
	TestFalse(TEXT("5.1 left the picker"), Old->bInPicker);
	TestEqual(TEXT("5.1 keeps its own price"), Old->Price.Input, 10.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCatalog_KimiModels,
	"KismetAI.ModelCatalog.KimiModels", ModelCatalogTestFlags)
bool FModelCatalog_KimiModels::RunTest(const FString& Parameters)
{
	const FKismetAIModelCatalog& Catalog = FKismetAIModelCatalog::Get();
	struct FCase { const TCHAR* Name; const TCHAR* Id; double Input; double Output; };
	for (const FCase& Case : {
		FCase{ TEXT("kimi"), TEXT("kimi-k3.1-pro"), 2.0, 10.0 },
		FCase{ TEXT("kimi-highspeed"), TEXT("kimi-k3.1-pro-highspeed"), 4.0, 20.0 },
		FCase{ TEXT("kimi-k3"), TEXT("kimi-k3"), 3.0, 15.0 } })
	{
		const FKismetAIModelEntry* Entry = Catalog.Find(Case.Name);
		if (!TestNotNull(Case.Name, Entry)) continue;
		TestEqual(FString(Case.Name) + TEXT(": id"), Entry->Id, FString(Case.Id));
		TestEqual(FString(Case.Name) + TEXT(": moonshot"), Entry->Provider, FString(TEXT("moonshot")));
		TestTrue(FString(Case.Name) + TEXT(": in the picker"), Entry->bInPicker);
		TestEqual(FString(Case.Name) + TEXT(": context"), Entry->ContextWindow, 1048576);
		TestEqual(FString(Case.Name) + TEXT(": max output"), Entry->MaxOutputTokens, 131072);
		TestEqual(FString(Case.Name) + TEXT(": input"), Entry->Price.Input, Case.Input);
		TestEqual(FString(Case.Name) + TEXT(": output"), Entry->Price.Output, Case.Output);
		TestEqual(FString(Case.Name) + TEXT(": effort low/high/max"), FString::Join(Entry->Caps.EffortLevels, TEXT(",")), FString(TEXT("low,high,max")));
		TestFalse(FString(Case.Name) + TEXT(": no thinking field (K3 always reasons)"), Entry->Caps.bThinking);
	}

	// Fast mode on K3.1 Pro sends the high-speed variant.
	FKismetAIAgentConfig Cfg = MakeCatalogTestConfig(TEXT("kimi-k3.1-pro"));
	Cfg.ProviderKeys.Add(TEXT("moonshot"), TEXT("sk-moon"));
	const FKismetAIProviderProfile& Moonshot = KismetAIProviders::ResolveProfile(Cfg);
	TestEqual(TEXT("standard id"), KismetAIProviders::WireModelId(Cfg, Moonshot), FString(TEXT("kimi-k3.1-pro")));
	Cfg.bFastMode = true;
	TestEqual(TEXT("fast swaps the model"), KismetAIProviders::WireModelId(Cfg, Moonshot), FString(TEXT("kimi-k3.1-pro-highspeed")));
	TestEqual(TEXT("fast price"), FKismetAIAgent::GetModelCost(TEXT("kimi-k3.1-pro"), true).InputPerMillion, 4.0);
	FLLMRequestShape Shape;
	const TSharedPtr<FJsonObject> Body = BuildBody(*MakeProviderForConfig(Cfg), Cfg, Shape);
	TestEqual(TEXT("request carries the high-speed id"), Body->GetStringField(TEXT("model")), FString(TEXT("kimi-k3.1-pro-highspeed")));
	const TSharedPtr<FJsonObject>* OutputConfig = nullptr;
	TestTrue(TEXT("effort sent as output_config"), Body->TryGetObjectField(TEXT("output_config"), OutputConfig));
	TestFalse(TEXT("no thinking field"), Body->HasField(TEXT("thinking")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCatalog_VendorReasoning,
	"KismetAI.ModelCatalog.VendorReasoning", ModelCatalogTestFlags)
bool FModelCatalog_VendorReasoning::RunTest(const FString& Parameters)
{
	const FKismetAIModelCatalog& Catalog = FKismetAIModelCatalog::Get();
	for (const TCHAR* Id : { TEXT("deepseek-v4-pro"), TEXT("deepseek-flash") })
	{
		const FKismetAIModelCapabilities& Caps = Catalog.FindOrDefaults(Id).Caps;
		TestTrue(FString(Id) + TEXT(": thinking"), Caps.bThinking && Caps.bAdaptiveThinking && !Caps.bBudgetThinking);
		TestEqual(FString(Id) + TEXT(": effort"), FString::Join(Caps.EffortLevels, TEXT(",")), FString(TEXT("low,high,max")));
	}
	const FKismetAIModelCapabilities& Flash = Catalog.FindOrDefaults(TEXT("minimax-flash")).Caps;
	TestTrue(TEXT("M3.1 Flash always thinks"), Flash.bThinking && Flash.bThinkingAlwaysOn);
	TestEqual(TEXT("M3.1 Flash effort"), Flash.EffortLevels.Num(), 5);
	TestEqual(TEXT("M3 has no effort (rejects reasoning_effort)"), Catalog.FindOrDefaults(TEXT("MiniMax-M3")).Caps.EffortLevels.Num(), 0);
	TestFalse(TEXT("GLM-5.3 takes no images"), Catalog.FindOrDefaults(TEXT("glm-5.3")).Caps.bImageInput);

	FKismetAIAgentConfig Cfg = MakeCatalogTestConfig(TEXT("MiniMax-M3.1-Flash"));
	FLLMRequestShape Shape;
	const TSharedPtr<FJsonObject> Body = BuildBody(*MakeProviderForConfig(Cfg), Cfg, Shape);
	const TSharedPtr<FJsonObject>* Thinking = nullptr;
	TestTrue(TEXT("M3.1 Flash: adaptive thinking sent"), Body->TryGetObjectField(TEXT("thinking"), Thinking)
		&& (*Thinking)->GetStringField(TEXT("type")) == TEXT("adaptive"));
	TestTrue(TEXT("M3.1 Flash: effort sent"), Body->HasField(TEXT("output_config")));

	for (const TCHAR* Id : { TEXT("gemini-4-argon"), TEXT("gemini-3.8-flash") })
	{
		TestEqual(FString(Id) + TEXT(": max output"), Catalog.FindOrDefaults(Id).MaxOutputTokens, 65536);
	}
	for (const TCHAR* Id : { TEXT("deepseek-v4-pro"), TEXT("deepseek-flash"), TEXT("glm-5.3"), TEXT("glm-5.3-flash"),
		TEXT("kimi-k3.1-pro"), TEXT("MiniMax-M3"), TEXT("MiniMax-M3.1-Flash") })
	{
		TestEqual(FString(Id) + TEXT(": max output"), Catalog.FindOrDefaults(Id).MaxOutputTokens, 131072);
	}
	TestEqual(TEXT("gpt-5.4-nano name"), FKismetAIAgent::GetModelDisplayName(TEXT("gpt-5.4-nano")), FString(TEXT("GPT-5.4 Nano")));
	TestEqual(TEXT("gpt-5-nano name"), FKismetAIAgent::GetModelDisplayName(TEXT("gpt-5-nano")), FString(TEXT("GPT-5 Nano")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCatalog_OpenRouterRows,
	"KismetAI.ModelCatalog.OpenRouterRows", ModelCatalogTestFlags)
bool FModelCatalog_OpenRouterRows::RunTest(const FString& Parameters)
{
	const FKismetAIModelCatalog& Catalog = FKismetAIModelCatalog::Get();
	struct FRow { const TCHAR* Id; const TCHAR* Name; const TCHAR* Base; };
	const TArray<FRow> Expected = {
		{ TEXT("anthropic/claude-opus-5.5"), TEXT("Claude Opus 5.5"), TEXT("claude-opus-5-5") },
		{ TEXT("anthropic/claude-fable-5.2"), TEXT("Claude Fable 5.2"), TEXT("claude-fable-5-2") },
		{ TEXT("anthropic/claude-sonnet-5.5"), TEXT("Claude Sonnet 5.5"), TEXT("claude-sonnet-5-5") },
		{ TEXT("anthropic/claude-haiku-5.5"), TEXT("Claude Haiku 5.5"), TEXT("claude-haiku-5-5") },
		{ TEXT("openai/gpt-6-astra"), TEXT("GPT-6 Astra"), TEXT("gpt-6-astra") },
		{ TEXT("openai/gpt-6.1-sol"), TEXT("GPT-6.1 Sol"), TEXT("gpt-6.1-sol") },
		{ TEXT("openai/gpt-6.1-terra"), TEXT("GPT-6.1 Terra"), TEXT("gpt-6.1-terra") },
		{ TEXT("openai/gpt-6-luna"), TEXT("GPT-6 Luna"), TEXT("gpt-6-luna") },
		{ TEXT("google/gemini-4-argon"), TEXT("Gemini 4 Argon"), TEXT("gemini-4-argon") },
		{ TEXT("google/gemini-3.8-flash"), TEXT("Gemini 3.8 Flash"), TEXT("gemini-3.8-flash") },
		{ TEXT("deepseek/deepseek-v4-pro"), TEXT("DeepSeek V4 Pro"), TEXT("deepseek-v4-pro") },
		{ TEXT("deepseek/deepseek-v4.1-flash"), TEXT("DeepSeek V4.1 Flash"), TEXT("deepseek-flash") },
		{ TEXT("z-ai/glm-5.3"), TEXT("GLM 5.3"), TEXT("glm-5.3") },
		{ TEXT("moonshotai/kimi-k3.1-pro"), TEXT("Kimi K3.1 Pro"), TEXT("kimi-k3.1-pro") },
		{ TEXT("minimax/minimax-m3.1-flash"), TEXT("MiniMax M3.1 Flash"), TEXT("MiniMax-M3.1-Flash") },
		{ TEXT("x-ai/grok-4.7"), TEXT("Grok 4.7"), nullptr },
		{ TEXT("qwen/qwen3.8-max-prime"), TEXT("Qwen 3.8 Max Prime"), nullptr },
	};

	// The OpenRouter group is contiguous: the direct rows' order, then OpenRouter-only models.
	TArray<FString> Group;
	bool bGroupEnded = false;
	bool bSplit = false;
	for (const FKismetAIModelEntry* Entry : Catalog.GetPickerModels())
	{
		if (Entry->Provider == TEXT("openrouter"))
		{
			bSplit |= bGroupEnded;
			Group.Add(Entry->Id);
		}
		else if (Group.Num() > 0)
		{
			bGroupEnded = true;
		}
	}
	TestFalse(TEXT("OpenRouter rows are one group"), bSplit);
	TArray<FString> ExpectedIds;
	for (const FRow& Row : Expected) ExpectedIds.Add(Row.Id);
	TestEqual(TEXT("OpenRouter picker rows"), FString::Join(Group, TEXT(", ")), FString::Join(ExpectedIds, TEXT(", ")));

	for (const FRow& Row : Expected)
	{
		const FKismetAIModelEntry* Entry = Catalog.Find(Row.Id);
		if (!TestNotNull(Row.Id, Entry)) continue;
		TestEqual(FString(Row.Id) + TEXT(": exact entry"), Entry->Id, FString(Row.Id));
		TestEqual(FString(Row.Id) + TEXT(": name"), Entry->DisplayName, FString(Row.Name));
		TestEqual(FString(Row.Id) + TEXT(": routes to OpenRouter"), KismetAIProviders::ProviderIdForModel(Row.Id), FString(TEXT("openrouter")));
		TestEqual(FString(Row.Id) + TEXT(": the alias resolves"), Catalog.Resolve(Entry->Alias), FString(Row.Id));
		TestTrue(FString(Row.Id) + TEXT(": no fast tier through OpenRouter"), Entry->Caps.FastModeWire == EKismetAIFastModeWire::None);
		TestFalse(FString(Row.Id) + TEXT(": no Anthropic thinking field"), Entry->Caps.bThinking);
		if (Row.Base)
		{
			const FKismetAIModelEntry* Base = Catalog.Find(Row.Base);
			if (!TestNotNull(Row.Base, Base)) continue;
			TestEqual(FString(Row.Id) + TEXT(": derived from its base"), Entry->DerivedFrom, Base->Id);
			TestEqual(FString(Row.Id) + TEXT(": base context"), Entry->ContextWindow, Base->ContextWindow);
			TestEqual(FString(Row.Id) + TEXT(": base max output"), Entry->MaxOutputTokens, Base->MaxOutputTokens);
			TestEqual(FString(Row.Id) + TEXT(": base image input"), Entry->Caps.bImageInput, Base->Caps.bImageInput);
			if (FCString::Strcmp(Row.Base, TEXT("kimi-k3.1-pro")) != 0)
			{
				TestEqual(FString(Row.Id) + TEXT(": base price"), Entry->Price.Input, Base->Price.Input);
			}
		}
	}

	// Reasoning through OpenRouter: reasoning_effort in the levels OpenRouter normalises.
	const FKismetAIModelEntry* Opus = Catalog.Find(TEXT("anthropic/claude-opus-5.5"));
	TestTrue(TEXT("Claude via OpenRouter sends reasoning_effort"), Opus && Opus->Caps.bReasoning);
	TestEqual(TEXT("Claude via OpenRouter effort levels"), Opus ? FString::Join(Opus->Caps.EffortLevels, TEXT(",")) : FString(), FString(TEXT("low,medium,high")));
	const FKismetAIModelEntry* Glm = Catalog.Find(TEXT("z-ai/glm-5.3"));
	TestTrue(TEXT("GLM via OpenRouter sends effort"), Glm && Glm->Caps.bReasoning && Glm->Caps.EffortLevels.Num() == 2);
	TestFalse(TEXT("GLM via OpenRouter takes no images"), Glm && Glm->Caps.bImageInput);
	const FKismetAIModelEntry* Grok = Catalog.Find(TEXT("grok"));
	TestTrue(TEXT("grok alias"), Grok && Grok->Id == TEXT("x-ai/grok-4.7") && Grok->ContextWindow == 500000 && Grok->MaxOutputTokens == 131072);
	const FKismetAIModelEntry* Qwen = Catalog.Find(TEXT("qwen"));
	TestTrue(TEXT("qwen alias"), Qwen && Qwen->Id == TEXT("qwen/qwen3.8-max-prime") && Qwen->ContextWindow == 1000000);

	// Typing an id still works on OpenRouter.
	TestTrue(TEXT("OpenRouter keeps custom models"), FKismetAIProviderRegistry::Get().Find(TEXT("openrouter"))->bCustomModels);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCatalog_OpenRouterDerivationRules,
	"KismetAI.ModelCatalog.OpenRouterDerivationRules", ModelCatalogTestFlags)
bool FModelCatalog_OpenRouterDerivationRules::RunTest(const FString& Parameters)
{
	FKismetAIModelCatalog Catalog;
	FString Error;
	TestFalse(TEXT("openrouter_id needs a vendor prefix"), Catalog.LoadFromJson(TEXT(R"({"schema_version":1,"models":[
		{"id":"m","alias":"m","display_name":"M","provider":"x","picker":true,"openrouter_id":"no-slash"}]})"), Error));
	TestFalse(TEXT("a derived id may not collide"), Catalog.LoadFromJson(TEXT(R"({"schema_version":1,"models":[
		{"id":"a","display_name":"A","provider":"x","picker":true,"openrouter_id":"v/a"},
		{"id":"v/a","display_name":"B","provider":"openrouter"}]})"), Error));
	TestTrue(TEXT("derivation loads"), Catalog.LoadFromJson(TEXT(R"({"schema_version":1,"models":[
		{"id":"a","alias":"a","display_name":"A","provider":"x","picker":true,"openrouter_id":"v/a","openrouter_picker":false,
		 "pricing":{"input":1,"output":2},"openrouter_pricing":{"input":3,"output":4}}]})"), Error));
	const FKismetAIModelEntry* Derived = Catalog.Find(TEXT("v/a"));
	TestTrue(TEXT("openrouter_picker false hides the row"), Derived && !Derived->bInPicker);
	TestTrue(TEXT("openrouter_pricing overrides"), Derived && Derived->Price.Input == 3.0);
	TestEqual(TEXT("base keeps its price"), Catalog.Find(TEXT("a"))->Price.Input, 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCatalog_RuntimeOverlay,
	"KismetAI.ModelCatalog.RuntimeOverlay", ModelCatalogTestFlags)
bool FModelCatalog_RuntimeOverlay::RunTest(const FString& Parameters)
{
	FKismetAIModelCatalog Catalog;
	FString Error;
	TestTrue(TEXT("loads"), Catalog.LoadFromJson(TEXT(R"({"schema_version":1,"models":[
		{"id":"a","alias":"a","display_name":"A","provider":"x","picker":true,"context_window":1000}]})"), Error));
	FKismetAIModelEntry Live = *Catalog.Find(TEXT("a"));
	Live.ContextWindow = 2000;
	FKismetAIModelEntry Local;
	Local.Id = TEXT("local:1b");
	Local.ContextWindow = 4096;
	Catalog.SetRuntimeEntries(TEXT("x"), { Live, Local });
	const FKismetAIModelEntry* Before = Catalog.Find(TEXT("a"));
	TestEqual(TEXT("overlay wins by exact id"), Before->ContextWindow, 2000);
	TestEqual(TEXT("alias resolves to the overlaid id"), Catalog.Resolve(TEXT("a")), FString(TEXT("a")));
	TestEqual(TEXT("overlay-only id resolves"), Catalog.Resolve(TEXT("LOCAL:1B")), FString(TEXT("local:1b")));
	TestEqual(TEXT("overlay rows belong to their owner"), Catalog.Find(TEXT("local:1b"))->Provider, FString(TEXT("x")));
	Catalog.ClearRuntimeEntries(TEXT("x"));
	TestEqual(TEXT("cleared: shipped entry again"), Catalog.Find(TEXT("a"))->ContextWindow, 1000);
	TestTrue(TEXT("cleared: overlay-only id gone"), Catalog.Resolve(TEXT("local:1b")).IsEmpty());
	TestEqual(TEXT("a pointer handed out earlier stays valid"), Before->ContextWindow, 2000);
	return true;
}
