// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIModelCatalog.h"
#include "KismetAIAgent.h"
#include "KismetAILLMProvider.h"
#include "KismetAIAnthropicProvider.h"
#include "KismetAIOpenAIProvider.h"
#include "KismetAIMinimaxProvider.h"
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
	for (const TCHAR* Id : { TEXT("claude-opus-5-5"), TEXT("claude-fable-5-1"), TEXT("claude-sonnet-5-5"),
		TEXT("claude-haiku-5-5"), TEXT("gpt-6-astra"), TEXT("gpt-6.1-sol"), TEXT("gpt-6.1-terra"), TEXT("gpt-6-luna") })
	{
		TestTrue(FString::Printf(TEXT("%s is in the picker"), Id), Listed.Contains(Id));
	}
	for (const TCHAR* Id : { TEXT("claude-opus-4-8"), TEXT("claude-sonnet-4-6"), TEXT("claude-haiku-4-5-20251001"), TEXT("gpt-5.5") })
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
	for (const TCHAR* Model : { TEXT("claude-opus-5-5"), TEXT("claude-fable-5-1"), TEXT("claude-sonnet-5-5"),
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
