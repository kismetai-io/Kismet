// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIAgent.h"
#include "KismetAILLMProvider.h"
#include "Misc/AutomationTest.h"

constexpr auto OpenAIRegistryTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// 2.1 — GetModelContextLimit: OpenAI model IDs
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_ContextLimit_Gpt55,
	"KismetAI.Model.OpenAIRegistry.ContextLimit.Gpt55Is1050K",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_ContextLimit_Gpt55::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("gpt-5.5 context limit is 1050000"),
		FKismetAIAgent::GetModelContextLimit(TEXT("gpt-5.5")),
		1050000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_ContextLimit_Gpt54,
	"KismetAI.Model.OpenAIRegistry.ContextLimit.Gpt54Is1050K",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_ContextLimit_Gpt54::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("gpt-5.4 context limit is 1050000"),
		FKismetAIAgent::GetModelContextLimit(TEXT("gpt-5.4")),
		1050000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_ContextLimit_Gpt54Mini,
	"KismetAI.Model.OpenAIRegistry.ContextLimit.Gpt54MiniIs400K",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_ContextLimit_Gpt54Mini::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("gpt-5.4-mini context limit is 400000"),
		FKismetAIAgent::GetModelContextLimit(TEXT("gpt-5.4-mini")),
		400000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_ContextLimit_Gpt54Nano,
	"KismetAI.Model.OpenAIRegistry.ContextLimit.Gpt54NanoIs400K",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_ContextLimit_Gpt54Nano::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("gpt-5.4-nano context limit is 400000"),
		FKismetAIAgent::GetModelContextLimit(TEXT("gpt-5.4-nano")),
		400000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_ContextLimit_Gpt5,
	"KismetAI.Model.OpenAIRegistry.ContextLimit.Gpt5Is400K",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_ContextLimit_Gpt5::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("gpt-5 context limit is 400000"),
		FKismetAIAgent::GetModelContextLimit(TEXT("gpt-5")),
		400000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_ContextLimit_Gpt5Mini,
	"KismetAI.Model.OpenAIRegistry.ContextLimit.Gpt5MiniIs400K",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_ContextLimit_Gpt5Mini::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("gpt-5-mini context limit is 400000"),
		FKismetAIAgent::GetModelContextLimit(TEXT("gpt-5-mini")),
		400000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_ContextLimit_Gpt5Nano,
	"KismetAI.Model.OpenAIRegistry.ContextLimit.Gpt5NanoIs400K",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_ContextLimit_Gpt5Nano::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("gpt-5-nano context limit is 400000"),
		FKismetAIAgent::GetModelContextLimit(TEXT("gpt-5-nano")),
		400000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_ContextLimit_O3,
	"KismetAI.Model.OpenAIRegistry.ContextLimit.O3Is200K",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_ContextLimit_O3::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("o3 context limit is 200000"),
		FKismetAIAgent::GetModelContextLimit(TEXT("o3")),
		200000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_ContextLimit_O4Mini,
	"KismetAI.Model.OpenAIRegistry.ContextLimit.O4MiniIs200K",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_ContextLimit_O4Mini::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("o4-mini context limit is 200000"),
		FKismetAIAgent::GetModelContextLimit(TEXT("o4-mini")),
		200000);
	return true;
}

// Prefix-match: dated suffix e.g. "gpt-5.5-20260101" should hit gpt-5.5 prefix
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_ContextLimit_Gpt55DatedSuffix,
	"KismetAI.Model.OpenAIRegistry.ContextLimit.Gpt55DatedSuffixPrefixMatch",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_ContextLimit_Gpt55DatedSuffix::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("gpt-5.5-20260101 prefix-matches gpt-5.5"),
		FKismetAIAgent::GetModelContextLimit(TEXT("gpt-5.5-20260101")),
		1050000);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 2.1 — regression: Claude context limits unchanged
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_IV3_ClaudeOpus47Unchanged,
	"KismetAI.Model.OpenAIRegistry.ClaudeOpus47ContextUnchanged",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_IV3_ClaudeOpus47Unchanged::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("claude-opus-4-7 context limit still 1M"),
		FKismetAIAgent::GetModelContextLimit(TEXT("claude-opus-4-7")),
		1000000);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 2.1 — GetModelCost: OpenAI model IDs
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_Cost_Gpt55,
	"KismetAI.Model.OpenAIRegistry.ModelCost.Gpt55Pricing",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_Cost_Gpt55::RunTest(const FString& Parameters)
{
	FModelCost Cost = FKismetAIAgent::GetModelCost(TEXT("gpt-5.5"));
	TestEqual(TEXT("gpt-5.5 input per million"), Cost.InputPerMillion, 5.0);
	TestEqual(TEXT("gpt-5.5 output per million"), Cost.OutputPerMillion, 30.0);
	TestEqual(TEXT("gpt-5.5 cache write per million"), Cost.CacheWritePerMillion, 0.0);
	// cache read ≈ 0.1× input = 0.5; verify it's in the expected range
	TestTrue(TEXT("gpt-5.5 cache read ≈ 0.1x input"),
		FMath::Abs(Cost.CacheReadPerMillion - 0.5) < 0.01);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_Cost_Gpt54,
	"KismetAI.Model.OpenAIRegistry.ModelCost.Gpt54Pricing",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_Cost_Gpt54::RunTest(const FString& Parameters)
{
	FModelCost Cost = FKismetAIAgent::GetModelCost(TEXT("gpt-5.4"));
	TestEqual(TEXT("gpt-5.4 input per million"), Cost.InputPerMillion, 2.5);
	TestEqual(TEXT("gpt-5.4 output per million"), Cost.OutputPerMillion, 15.0);
	TestEqual(TEXT("gpt-5.4 cache write per million"), Cost.CacheWritePerMillion, 0.0);
	TestTrue(TEXT("gpt-5.4 cache read ≈ 0.1x input"),
		FMath::Abs(Cost.CacheReadPerMillion - 0.25) < 0.01);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_Cost_Gpt54Mini,
	"KismetAI.Model.OpenAIRegistry.ModelCost.Gpt54MiniPricing",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_Cost_Gpt54Mini::RunTest(const FString& Parameters)
{
	FModelCost Cost = FKismetAIAgent::GetModelCost(TEXT("gpt-5.4-mini"));
	TestEqual(TEXT("gpt-5.4-mini input per million"), Cost.InputPerMillion, 0.75);
	TestEqual(TEXT("gpt-5.4-mini output per million"), Cost.OutputPerMillion, 4.5);
	TestEqual(TEXT("gpt-5.4-mini cache write per million"), Cost.CacheWritePerMillion, 0.0);
	TestTrue(TEXT("gpt-5.4-mini cache read ≈ 0.1x input"),
		FMath::Abs(Cost.CacheReadPerMillion - 0.075) < 0.01);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_Cost_Gpt5,
	"KismetAI.Model.OpenAIRegistry.ModelCost.Gpt5Pricing",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_Cost_Gpt5::RunTest(const FString& Parameters)
{
	FModelCost Cost = FKismetAIAgent::GetModelCost(TEXT("gpt-5"));
	TestEqual(TEXT("gpt-5 input per million"), Cost.InputPerMillion, 1.25);
	TestEqual(TEXT("gpt-5 output per million"), Cost.OutputPerMillion, 10.0);
	TestEqual(TEXT("gpt-5 cache write per million"), Cost.CacheWritePerMillion, 0.0);
	TestTrue(TEXT("gpt-5 cache read ≈ 0.1x input"),
		FMath::Abs(Cost.CacheReadPerMillion - 0.125) < 0.01);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_Cost_Gpt5Mini,
	"KismetAI.Model.OpenAIRegistry.ModelCost.Gpt5MiniPricing",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_Cost_Gpt5Mini::RunTest(const FString& Parameters)
{
	FModelCost Cost = FKismetAIAgent::GetModelCost(TEXT("gpt-5-mini"));
	TestEqual(TEXT("gpt-5-mini input per million"), Cost.InputPerMillion, 0.25);
	TestEqual(TEXT("gpt-5-mini output per million"), Cost.OutputPerMillion, 2.0);
	TestEqual(TEXT("gpt-5-mini cache write per million"), Cost.CacheWritePerMillion, 0.0);
	TestTrue(TEXT("gpt-5-mini cache read ≈ 0.1x input"),
		FMath::Abs(Cost.CacheReadPerMillion - 0.025) < 0.01);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 2.1 — regression: Claude cost unchanged
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_IV3_SonnetCostUnchanged,
	"KismetAI.Model.OpenAIRegistry.SonnetCostUnchanged",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_IV3_SonnetCostUnchanged::RunTest(const FString& Parameters)
{
	FModelCost Cost = FKismetAIAgent::GetModelCost(TEXT("claude-sonnet-4-6"));
	TestEqual(TEXT("Sonnet 4.6 input unchanged"), Cost.InputPerMillion, 3.0);
	TestEqual(TEXT("Sonnet 4.6 output unchanged"), Cost.OutputPerMillion, 15.0);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 2.1 — ResolveModelAlias: OpenAI aliases
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_Alias_Gpt5,
	"KismetAI.Model.OpenAIRegistry.Alias.Gpt5AliasResolvesDirectly",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_Alias_Gpt5::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("gpt5 alias resolves to gpt-5"),
		FKismetAIAgent::ResolveModelAlias(TEXT("gpt5")),
		FString(TEXT("gpt-5")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_Alias_Gpt5Mini,
	"KismetAI.Model.OpenAIRegistry.Alias.Gpt5MiniAliasResolvesDirectly",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_Alias_Gpt5Mini::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("gpt5-mini alias resolves to gpt-5-mini"),
		FKismetAIAgent::ResolveModelAlias(TEXT("gpt5-mini")),
		FString(TEXT("gpt-5-mini")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_Alias_O4Mini,
	"KismetAI.Model.OpenAIRegistry.Alias.O4MiniAliasResolvesDirectly",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_Alias_O4Mini::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("o4-mini alias resolves to o4-mini"),
		FKismetAIAgent::ResolveModelAlias(TEXT("o4-mini")),
		FString(TEXT("o4-mini")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_Alias_FullIdPassthrough,
	"KismetAI.Model.OpenAIRegistry.Alias.FullGptIdPassthrough",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_Alias_FullIdPassthrough::RunTest(const FString& Parameters)
{
	// Full canonical model ID passed directly — should be returned as-is
	TestEqual(TEXT("gpt-5 full id passthrough"),
		FKismetAIAgent::ResolveModelAlias(TEXT("gpt-5")),
		FString(TEXT("gpt-5")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_Alias_O3Passthrough,
	"KismetAI.Model.OpenAIRegistry.Alias.O3FullIdPassthrough",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_Alias_O3Passthrough::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("o3 full id passthrough"),
		FKismetAIAgent::ResolveModelAlias(TEXT("o3")),
		FString(TEXT("o3")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 2.1 — regression: Claude aliases unchanged
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_IV3_OpusAliasUnchanged,
	"KismetAI.Model.OpenAIRegistry.OpusAliasUnchanged",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_IV3_OpusAliasUnchanged::RunTest(const FString& Parameters)
{
	// The opus alias still resolves to a Claude flagship (the OpenAI work never
	// repoints it at a GPT model). The flagship itself advanced to 4.8 with the
	// Opus 4.8 release — see OpusModelRegistry.ResolveAlias.OpusResolvesToOpus48.
	TestEqual(TEXT("Opus alias resolves to the current Claude flagship (claude-opus-5-5)"),
		FKismetAIAgent::ResolveModelAlias(TEXT("opus")),
		FString(TEXT("claude-opus-5-5")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 2.1 — GetModelDisplayName: OpenAI model IDs
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_DisplayName_Gpt55,
	"KismetAI.Model.OpenAIRegistry.DisplayName.Gpt55",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_DisplayName_Gpt55::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("gpt-5.5 display name"),
		FKismetAIAgent::GetModelDisplayName(TEXT("gpt-5.5")),
		FString(TEXT("GPT-5.5")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_DisplayName_Gpt54,
	"KismetAI.Model.OpenAIRegistry.DisplayName.Gpt54",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_DisplayName_Gpt54::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("gpt-5.4 display name"),
		FKismetAIAgent::GetModelDisplayName(TEXT("gpt-5.4")),
		FString(TEXT("GPT-5.4")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_DisplayName_Gpt54Mini,
	"KismetAI.Model.OpenAIRegistry.DisplayName.Gpt54Mini",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_DisplayName_Gpt54Mini::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("gpt-5.4-mini display name"),
		FKismetAIAgent::GetModelDisplayName(TEXT("gpt-5.4-mini")),
		FString(TEXT("GPT-5.4 Mini")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_DisplayName_Gpt5,
	"KismetAI.Model.OpenAIRegistry.DisplayName.Gpt5",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_DisplayName_Gpt5::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("gpt-5 display name"),
		FKismetAIAgent::GetModelDisplayName(TEXT("gpt-5")),
		FString(TEXT("GPT-5")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_DisplayName_Gpt5Mini,
	"KismetAI.Model.OpenAIRegistry.DisplayName.Gpt5Mini",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_DisplayName_Gpt5Mini::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("gpt-5-mini display name"),
		FKismetAIAgent::GetModelDisplayName(TEXT("gpt-5-mini")),
		FString(TEXT("GPT-5 Mini")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_DisplayName_O3,
	"KismetAI.Model.OpenAIRegistry.DisplayName.O3",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_DisplayName_O3::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("o3 display name"),
		FKismetAIAgent::GetModelDisplayName(TEXT("o3")),
		FString(TEXT("o3")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_DisplayName_O4Mini,
	"KismetAI.Model.OpenAIRegistry.DisplayName.O4Mini",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_DisplayName_O4Mini::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("o4-mini display name"),
		FKismetAIAgent::GetModelDisplayName(TEXT("o4-mini")),
		FString(TEXT("o4-mini")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 2.1 — regression: Claude display names unchanged
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_IV3_OpusDisplayNameUnchanged,
	"KismetAI.Model.OpenAIRegistry.OpusDisplayNameUnchanged",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_IV3_OpusDisplayNameUnchanged::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("claude-opus-4-7 display name unchanged"),
		FKismetAIAgent::GetModelDisplayName(TEXT("claude-opus-4-7")),
		FString(TEXT("Opus 4.7")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 2.4 — MakeProviderForModel factory routing
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_Factory_Gpt5RoutesToOpenAI,
	"KismetAI.Model.OpenAIRegistry.Factory.Gpt5RoutesToOpenAI",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_Factory_Gpt5RoutesToOpenAI::RunTest(const FString& Parameters)
{
	TUniquePtr<ILLMProvider> Provider = MakeProviderForModel(TEXT("gpt-5"));
	if (!TestTrue(TEXT("Provider is non-null"), Provider.IsValid())) return false;
	TestEqual(TEXT("gpt-5 routes to openai provider"),
		Provider->GetName(),
		FString(TEXT("openai")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_Factory_O4MiniRoutesToOpenAI,
	"KismetAI.Model.OpenAIRegistry.Factory.O4MiniRoutesToOpenAI",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_Factory_O4MiniRoutesToOpenAI::RunTest(const FString& Parameters)
{
	TUniquePtr<ILLMProvider> Provider = MakeProviderForModel(TEXT("o4-mini"));
	if (!TestTrue(TEXT("Provider is non-null"), Provider.IsValid())) return false;
	TestEqual(TEXT("o4-mini routes to openai provider"),
		Provider->GetName(),
		FString(TEXT("openai")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_Factory_O3RoutesToOpenAI,
	"KismetAI.Model.OpenAIRegistry.Factory.O3RoutesToOpenAI",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_Factory_O3RoutesToOpenAI::RunTest(const FString& Parameters)
{
	TUniquePtr<ILLMProvider> Provider = MakeProviderForModel(TEXT("o3"));
	if (!TestTrue(TEXT("Provider is non-null"), Provider.IsValid())) return false;
	TestEqual(TEXT("o3 routes to openai provider"),
		Provider->GetName(),
		FString(TEXT("openai")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_Factory_ClaudeRoutesToAnthropic,
	"KismetAI.Model.OpenAIRegistry.Factory.ClaudeRoutesToAnthropic",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_Factory_ClaudeRoutesToAnthropic::RunTest(const FString& Parameters)
{
	TUniquePtr<ILLMProvider> Provider = MakeProviderForModel(TEXT("claude-opus-4-7"));
	if (!TestTrue(TEXT("Provider is non-null"), Provider.IsValid())) return false;
	TestEqual(TEXT("claude-opus-4-7 routes to anthropic provider"),
		Provider->GetName(),
		FString(TEXT("anthropic")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_Factory_DefaultRoutesToAnthropic,
	"KismetAI.Model.OpenAIRegistry.Factory.EmptyModelRoutesToAnthropic",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_Factory_DefaultRoutesToAnthropic::RunTest(const FString& Parameters)
{
	// Unrecognized/empty model falls back to Anthropic
	TUniquePtr<ILLMProvider> Provider = MakeProviderForModel(TEXT(""));
	if (!TestTrue(TEXT("Provider is non-null"), Provider.IsValid())) return false;
	TestEqual(TEXT("empty model routes to anthropic (default)"),
		Provider->GetName(),
		FString(TEXT("anthropic")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Fast mode (OpenAI priority tier) — eligibility + pricing
// Priority-eligible: gpt-5.5, gpt-5.4, gpt-5.4-mini. NOT: gpt-5.4-nano, gpt-5(/mini),
// o-series. Off by default; fast pricing is 2× base (gpt-5.5 output is 2.5×).
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_FastMode_Eligibility,
	"KismetAI.Model.OpenAIRegistry.FastMode.Eligibility",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_FastMode_Eligibility::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("gpt-5.5 supports fast mode"),      FKismetAIAgent::ModelSupportsFastMode(TEXT("gpt-5.5")));
	TestTrue(TEXT("gpt-5.4 supports fast mode"),      FKismetAIAgent::ModelSupportsFastMode(TEXT("gpt-5.4")));
	TestTrue(TEXT("gpt-5.4-mini supports fast mode"), FKismetAIAgent::ModelSupportsFastMode(TEXT("gpt-5.4-mini")));

	TestFalse(TEXT("gpt-5.4-nano does NOT support fast mode"), FKismetAIAgent::ModelSupportsFastMode(TEXT("gpt-5.4-nano")));
	TestFalse(TEXT("gpt-5 does NOT support fast mode"),        FKismetAIAgent::ModelSupportsFastMode(TEXT("gpt-5")));

	TestFalse(TEXT("gpt-5-mini does NOT support fast mode"),   FKismetAIAgent::ModelSupportsFastMode(TEXT("gpt-5-mini")));
	TestFalse(TEXT("o3 does NOT support fast mode"),           FKismetAIAgent::ModelSupportsFastMode(TEXT("o3")));
	TestFalse(TEXT("o4-mini does NOT support fast mode"),      FKismetAIAgent::ModelSupportsFastMode(TEXT("o4-mini")));

	// Anthropic: the Opus 4.8 fast-mode research preview ended on 2026-11-30.
	TestFalse(TEXT("opus-4-8 does NOT support fast mode"), FKismetAIAgent::ModelSupportsFastMode(TEXT("claude-opus-4-8")));
	// Anthropic removed fast mode from Opus 4.6 (requests run at standard speed);
	// Opus 5.5 is the current fast-mode model.
	TestFalse(TEXT("opus-4-6 no longer has fast mode"), FKismetAIAgent::ModelSupportsFastMode(TEXT("claude-opus-4-6")));
	TestTrue(TEXT("opus-5-5 supports fast mode"), FKismetAIAgent::ModelSupportsFastMode(TEXT("claude-opus-5-5")));
	TestFalse(TEXT("opus-4-7 does NOT support fast mode"), FKismetAIAgent::ModelSupportsFastMode(TEXT("claude-opus-4-7")));
	TestFalse(TEXT("sonnet-4-6 does NOT support fast mode"), FKismetAIAgent::ModelSupportsFastMode(TEXT("claude-sonnet-4-6")));

	// MiniMax M2.7 routes to the highspeed sibling through the same toggle.
	TestTrue(TEXT("MiniMax-M2.7 supports fast mode"), FKismetAIAgent::ModelSupportsFastMode(TEXT("MiniMax-M2.7")));
	TestFalse(TEXT("MiniMax-M3 does NOT support fast mode"), FKismetAIAgent::ModelSupportsFastMode(TEXT("MiniMax-M3")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_FastMode_Gpt55Pricing,
	"KismetAI.Model.OpenAIRegistry.FastMode.Gpt55Pricing",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_FastMode_Gpt55Pricing::RunTest(const FString& Parameters)
{
	FModelCost Base = FKismetAIAgent::GetModelCost(TEXT("gpt-5.5"));
	TestEqual(TEXT("gpt-5.5 base input"), Base.InputPerMillion, 5.0);
	TestEqual(TEXT("gpt-5.5 base output"), Base.OutputPerMillion, 30.0);

	FModelCost Fast = FKismetAIAgent::GetModelCost(TEXT("gpt-5.5"), /*bFastMode=*/true);
	TestEqual(TEXT("gpt-5.5 fast input (2x)"), Fast.InputPerMillion, 12.5);
	TestEqual(TEXT("gpt-5.5 fast output (2.5x)"), Fast.OutputPerMillion, 75.0);
	TestEqual(TEXT("gpt-5.5 fast cache read"), Fast.CacheReadPerMillion, 1.25);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_FastMode_Gpt54Pricing,
	"KismetAI.Model.OpenAIRegistry.FastMode.Gpt54Pricing",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_FastMode_Gpt54Pricing::RunTest(const FString& Parameters)
{
	FModelCost Fast = FKismetAIAgent::GetModelCost(TEXT("gpt-5.4"), /*bFastMode=*/true);
	TestEqual(TEXT("gpt-5.4 fast input (2x)"), Fast.InputPerMillion, 5.0);
	TestEqual(TEXT("gpt-5.4 fast output (2x)"), Fast.OutputPerMillion, 30.0);

	FModelCost MiniFast = FKismetAIAgent::GetModelCost(TEXT("gpt-5.4-mini"), /*bFastMode=*/true);
	TestEqual(TEXT("gpt-5.4-mini fast input (2x)"), MiniFast.InputPerMillion, 1.5);
	TestEqual(TEXT("gpt-5.4-mini fast output (2x)"), MiniFast.OutputPerMillion, 9.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_FastMode_MinimaxM27Pricing,
	"KismetAI.Model.OpenAIRegistry.FastMode.MinimaxM27Pricing",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_FastMode_MinimaxM27Pricing::RunTest(const FString& Parameters)
{
	FModelCost Base = FKismetAIAgent::GetModelCost(TEXT("MiniMax-M2.7"));
	TestEqual(TEXT("MiniMax-M2.7 base input"), Base.InputPerMillion, 0.3);
	TestEqual(TEXT("MiniMax-M2.7 base output"), Base.OutputPerMillion, 1.2);

	FModelCost Fast = FKismetAIAgent::GetModelCost(TEXT("MiniMax-M2.7"), /*bFastMode=*/true);
	TestEqual(TEXT("MiniMax-M2.7 fast input"), Fast.InputPerMillion, 0.6);
	TestEqual(TEXT("MiniMax-M2.7 fast output"), Fast.OutputPerMillion, 2.4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIRegistry_FastMode_IneligibleUnchanged,
	"KismetAI.Model.OpenAIRegistry.FastMode.IneligibleModelsUnchanged",
	OpenAIRegistryTestFlags)
bool FOpenAIRegistry_FastMode_IneligibleUnchanged::RunTest(const FString& Parameters)
{
	// Ineligible models return identical pricing whether or not fast mode is requested.
	FModelCost NanoBase = FKismetAIAgent::GetModelCost(TEXT("gpt-5.4-nano"));
	FModelCost NanoFast = FKismetAIAgent::GetModelCost(TEXT("gpt-5.4-nano"), /*bFastMode=*/true);
	TestEqual(TEXT("gpt-5.4-nano ignores fast (input)"), NanoFast.InputPerMillion, NanoBase.InputPerMillion);
	TestEqual(TEXT("gpt-5.4-nano ignores fast (output)"), NanoFast.OutputPerMillion, NanoBase.OutputPerMillion);

	FModelCost Gpt5Base = FKismetAIAgent::GetModelCost(TEXT("gpt-5"));
	FModelCost Gpt5Fast = FKismetAIAgent::GetModelCost(TEXT("gpt-5"), /*bFastMode=*/true);
	TestEqual(TEXT("gpt-5 ignores fast (input)"), Gpt5Fast.InputPerMillion, Gpt5Base.InputPerMillion);
	TestEqual(TEXT("gpt-5 ignores fast (output)"), Gpt5Fast.OutputPerMillion, Gpt5Base.OutputPerMillion);
	return true;
}
