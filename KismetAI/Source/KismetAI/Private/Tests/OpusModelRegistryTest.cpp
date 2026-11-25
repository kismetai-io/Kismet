// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIAgent.h"
#include "KismetAIBridge.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

constexpr auto RegistryTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// ResolveModelAlias: opus → claude-opus-4-8 (flagship switched to 4.8)
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_ResolveAlias_OpusToOpus48,
	"KismetAI.OpusModelRegistry.ResolveAlias.OpusResolvesToOpus48",
	RegistryTestFlags)
bool FRegistry_ResolveAlias_OpusToOpus48::RunTest(const FString& Parameters)
{
	// The flagship moved to Opus 5.5 with the model catalog (Config/models.json).
	TestEqual(TEXT("opus alias resolves to claude-opus-5-5"),
		FKismetAIAgent::ResolveModelAlias(TEXT("opus")),
		FString(TEXT("claude-opus-5-5")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_ResolveAlias_Opus48Direct,
	"KismetAI.OpusModelRegistry.ResolveAlias.Opus48DirectAlias",
	RegistryTestFlags)
bool FRegistry_ResolveAlias_Opus48Direct::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("opus-4-8 alias resolves to claude-opus-4-8"),
		FKismetAIAgent::ResolveModelAlias(TEXT("opus-4-8")),
		FString(TEXT("claude-opus-4-8")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_ResolveAlias_MythosDirect,
	"KismetAI.OpusModelRegistry.ResolveAlias.MythosDirectAlias",
	RegistryTestFlags)
bool FRegistry_ResolveAlias_MythosDirect::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("mythos alias resolves to claude-mythos-5"),
		FKismetAIAgent::ResolveModelAlias(TEXT("mythos")),
		FString(TEXT("claude-mythos-5")));
	TestEqual(TEXT("mythos-5 alias resolves to claude-mythos-5"),
		FKismetAIAgent::ResolveModelAlias(TEXT("mythos-5")),
		FString(TEXT("claude-mythos-5")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_ResolveAlias_Opus47Direct,
	"KismetAI.OpusModelRegistry.ResolveAlias.Opus47DirectAlias",
	RegistryTestFlags)
bool FRegistry_ResolveAlias_Opus47Direct::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("opus-4-7 alias resolves to claude-opus-4-7"),
		FKismetAIAgent::ResolveModelAlias(TEXT("opus-4-7")),
		FString(TEXT("claude-opus-4-7")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_ResolveAlias_Minimax,
	"KismetAI.OpusModelRegistry.ResolveAlias.MinimaxAliases",
	RegistryTestFlags)
bool FRegistry_ResolveAlias_Minimax::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("minimax alias resolves to MiniMax-M3"),
		FKismetAIAgent::ResolveModelAlias(TEXT("minimax")),
		FString(TEXT("MiniMax-M3")));
	TestEqual(TEXT("m3 alias resolves to MiniMax-M3"),
		FKismetAIAgent::ResolveModelAlias(TEXT("m3")),
		FString(TEXT("MiniMax-M3")));
	TestEqual(TEXT("minimax-m2.7-highspeed alias resolves"),
		FKismetAIAgent::ResolveModelAlias(TEXT("minimax-m2.7-highspeed")),
		FString(TEXT("MiniMax-M2.7-highspeed")));
	TestTrue(TEXT("removed minimax-m2.5 alias does not resolve"),
		FKismetAIAgent::ResolveModelAlias(TEXT("minimax-m2.5")).IsEmpty());
	TestTrue(TEXT("removed minimax-m2.1 alias does not resolve"),
		FKismetAIAgent::ResolveModelAlias(TEXT("minimax-m2.1")).IsEmpty());
	TestTrue(TEXT("removed minimax-m2 alias does not resolve"),
		FKismetAIAgent::ResolveModelAlias(TEXT("minimax-m2")).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_ResolveAlias_Opus46Direct,
	"KismetAI.OpusModelRegistry.ResolveAlias.Opus46DirectAlias",
	RegistryTestFlags)
bool FRegistry_ResolveAlias_Opus46Direct::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("opus-4-6 alias resolves to claude-opus-4-6"),
		FKismetAIAgent::ResolveModelAlias(TEXT("opus-4-6")),
		FString(TEXT("claude-opus-4-6")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Sonnet, haiku, sonnet-1m, opus-1m mappings unchanged
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_IV5_SonnetUnchanged,
	"KismetAI.OpusModelRegistry.SonnetAliasUnchanged",
	RegistryTestFlags)
bool FRegistry_IV5_SonnetUnchanged::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("sonnet alias follows the current Sonnet"),
		FKismetAIAgent::ResolveModelAlias(TEXT("sonnet")),
		FString(TEXT("claude-sonnet-5-5")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_IV5_HaikuUnchanged,
	"KismetAI.OpusModelRegistry.HaikuAliasUnchanged",
	RegistryTestFlags)
bool FRegistry_IV5_HaikuUnchanged::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("haiku alias follows the current Haiku"),
		FKismetAIAgent::ResolveModelAlias(TEXT("haiku")),
		FString(TEXT("claude-haiku-5-5")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_IV5_Sonnet1mUnchanged,
	"KismetAI.OpusModelRegistry.Sonnet1mAliasUnchanged",
	RegistryTestFlags)
bool FRegistry_IV5_Sonnet1mUnchanged::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Sonnet-1m alias unchanged"),
		FKismetAIAgent::ResolveModelAlias(TEXT("sonnet-1m")),
		FString(TEXT("claude-sonnet-4-6")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_IV5_Opus1mUnchanged,
	"KismetAI.OpusModelRegistry.Opus1mAliasUnchanged",
	RegistryTestFlags)
bool FRegistry_IV5_Opus1mUnchanged::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Opus-1m alias still resolves to claude-opus-4-6"),
		FKismetAIAgent::ResolveModelAlias(TEXT("opus-1m")),
		FString(TEXT("claude-opus-4-6")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.1 — GetModelContextLimit for claude-opus-4-7
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_ContextLimit_Opus47,
	"KismetAI.OpusModelRegistry.ContextLimit.Opus47Is1M",
	RegistryTestFlags)
bool FRegistry_ContextLimit_Opus47::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("claude-opus-4-7 context limit is 1M"),
		FKismetAIAgent::GetModelContextLimit(TEXT("claude-opus-4-7")),
		1000000);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.2 — GetModelCost for claude-opus-4-7 (no fast mode, AS3)
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_Cost_Opus47,
	"KismetAI.OpusModelRegistry.ModelCost.Opus47Pricing",
	RegistryTestFlags)
bool FRegistry_Cost_Opus47::RunTest(const FString& Parameters)
{
	FModelCost Cost = FKismetAIAgent::GetModelCost(TEXT("claude-opus-4-7"));
	TestEqual(TEXT("Opus 4.7 input per million"), Cost.InputPerMillion, 5.0);
	TestEqual(TEXT("Opus 4.7 output per million"), Cost.OutputPerMillion, 25.0);
	TestEqual(TEXT("Opus 4.7 cache read per million"), Cost.CacheReadPerMillion, 0.5);
	TestEqual(TEXT("Opus 4.7 cache write per million"), Cost.CacheWritePerMillion, 6.25);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.6 — GetModelDisplayName for claude-opus-4-7
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_DisplayName_Opus47,
	"KismetAI.OpusModelRegistry.DisplayName.Opus47",
	RegistryTestFlags)
bool FRegistry_DisplayName_Opus47::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Opus 4.7 display name"),
		FKismetAIAgent::GetModelDisplayName(TEXT("claude-opus-4-7")),
		FString(TEXT("Opus 4.7")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.3 / 3.4 — ModelSupportsAdaptiveThinking / ModelSupportsEffort for opus-4-7
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_AdaptiveThinking_Opus47,
	"KismetAI.OpusModelRegistry.Capabilities.Opus47AdaptiveThinking",
	RegistryTestFlags)
bool FRegistry_AdaptiveThinking_Opus47::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Opus 4.7 supports adaptive thinking"),
		FKismetAIAgent::ModelSupportsAdaptiveThinking(TEXT("claude-opus-4-7")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_Effort_Opus47,
	"KismetAI.OpusModelRegistry.Capabilities.Opus47Effort",
	RegistryTestFlags)
bool FRegistry_Effort_Opus47::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Opus 4.7 supports effort"),
		FKismetAIAgent::ModelSupportsEffort(TEXT("claude-opus-4-7")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Opus 4.8 — context limit, pricing (no fast tier), display name, capabilities
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_ContextLimit_Opus48,
	"KismetAI.OpusModelRegistry.ContextLimit.Opus48Is1M",
	RegistryTestFlags)
bool FRegistry_ContextLimit_Opus48::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("claude-opus-4-8 context limit is 1M"),
		FKismetAIAgent::GetModelContextLimit(TEXT("claude-opus-4-8")),
		1000000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_ContextLimit_Mythos5,
	"KismetAI.OpusModelRegistry.ContextLimit.Mythos5Is1M",
	RegistryTestFlags)
bool FRegistry_ContextLimit_Mythos5::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("claude-mythos-5 context limit is 1M"),
		FKismetAIAgent::GetModelContextLimit(TEXT("claude-mythos-5")),
		1000000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_Cost_Opus48,
	"KismetAI.OpusModelRegistry.ModelCost.Opus48Pricing",
	RegistryTestFlags)
bool FRegistry_Cost_Opus48::RunTest(const FString& Parameters)
{
	// Default (no fast mode) — base pricing. The default argument guarantees fast
	// mode is never applied unless a caller explicitly opts in.
	FModelCost Cost = FKismetAIAgent::GetModelCost(TEXT("claude-opus-4-8"));
	TestEqual(TEXT("Opus 4.8 input per million"), Cost.InputPerMillion, 5.0);
	TestEqual(TEXT("Opus 4.8 output per million"), Cost.OutputPerMillion, 25.0);
	TestEqual(TEXT("Opus 4.8 cache read per million"), Cost.CacheReadPerMillion, 0.5);
	TestEqual(TEXT("Opus 4.8 cache write per million"), Cost.CacheWritePerMillion, 6.25);

	// Fast mode ended 2026-11-30 for Opus 4.8: the toggle keeps base pricing.
	FModelCost Fast = FKismetAIAgent::GetModelCost(TEXT("claude-opus-4-8"), /*bFastMode=*/true);
	TestEqual(TEXT("Opus 4.8 fast toggle keeps base input"), Fast.InputPerMillion, 5.0);
	TestEqual(TEXT("Opus 4.8 fast toggle keeps base output"), Fast.OutputPerMillion, 25.0);
	TestFalse(TEXT("Opus 4.8 has no fast mode"), FKismetAIAgent::ModelSupportsFastMode(TEXT("claude-opus-4-8")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_Cost_Mythos5,
	"KismetAI.OpusModelRegistry.ModelCost.Mythos5Pricing",
	RegistryTestFlags)
bool FRegistry_Cost_Mythos5::RunTest(const FString& Parameters)
{
	FModelCost Cost = FKismetAIAgent::GetModelCost(TEXT("claude-mythos-5"));
	TestEqual(TEXT("Mythos 5 input per million"), Cost.InputPerMillion, 10.0);
	TestEqual(TEXT("Mythos 5 output per million"), Cost.OutputPerMillion, 50.0);
	TestEqual(TEXT("Mythos 5 cache read per million"), Cost.CacheReadPerMillion, 1.0);
	TestEqual(TEXT("Mythos 5 cache write per million"), Cost.CacheWritePerMillion, 12.5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_DisplayName_Opus48,
	"KismetAI.OpusModelRegistry.DisplayName.Opus48",
	RegistryTestFlags)
bool FRegistry_DisplayName_Opus48::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Opus 4.8 display name"),
		FKismetAIAgent::GetModelDisplayName(TEXT("claude-opus-4-8")),
		FString(TEXT("Opus 4.8")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_DisplayName_Mythos5,
	"KismetAI.OpusModelRegistry.DisplayName.Mythos5",
	RegistryTestFlags)
bool FRegistry_DisplayName_Mythos5::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Mythos 5 display name"),
		FKismetAIAgent::GetModelDisplayName(TEXT("claude-mythos-5")),
		FString(TEXT("Mythos 5")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_AdaptiveThinking_Opus48,
	"KismetAI.OpusModelRegistry.Capabilities.Opus48AdaptiveThinking",
	RegistryTestFlags)
bool FRegistry_AdaptiveThinking_Opus48::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Opus 4.8 supports adaptive thinking"),
		FKismetAIAgent::ModelSupportsAdaptiveThinking(TEXT("claude-opus-4-8")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_Effort_Opus48,
	"KismetAI.OpusModelRegistry.Capabilities.Opus48Effort",
	RegistryTestFlags)
bool FRegistry_Effort_Opus48::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Opus 4.8 supports effort"),
		FKismetAIAgent::ModelSupportsEffort(TEXT("claude-opus-4-8")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.7 — GetAvailableModels: premium-first capability order with Tier / RecommendedEffort
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_AvailableModels_Count,
	"KismetAI.OpusModelRegistry.AvailableModels.FourRows",
	RegistryTestFlags)
bool FRegistry_AvailableModels_Count::RunTest(const FString& Parameters)
{
	TArray<FKismetAIAgent::FModelAliasInfo> Models = FKismetAIAgent::GetAvailableModels();
	TestTrue(TEXT("GetAvailableModels returns Claude, OpenAI, and MiniMax rows"), Models.Num() >= 9);
	return true;
}

// Mythos 5 is hidden from the picker: it must not appear in GetAvailableModels,
// but (like archived Opus 4.7) its alias still resolves for direct selection.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_AvailableModels_MythosHidden,
	"KismetAI.OpusModelRegistry.AvailableModels.Mythos5HiddenNotDisplayed",
	RegistryTestFlags)
bool FRegistry_AvailableModels_MythosHidden::RunTest(const FString& Parameters)
{
	TArray<FKismetAIAgent::FModelAliasInfo> Models = FKismetAIAgent::GetAvailableModels();
	for (const FKismetAIAgent::FModelAliasInfo& Model : Models)
	{
		if (Model.ModelId == TEXT("claude-mythos-5") || Model.Alias == TEXT("mythos"))
		{
			AddError(TEXT("Mythos 5 should be hidden and out of GetAvailableModels"));
			return false;
		}
	}
	TestEqual(TEXT("hidden mythos alias still resolves"),
		FKismetAIAgent::ResolveModelAlias(TEXT("mythos")),
		FString(TEXT("claude-mythos-5")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_AvailableModels_Row0,
	"KismetAI.OpusModelRegistry.AvailableModels.Row0IsOpus55Premium",
	RegistryTestFlags)
bool FRegistry_AvailableModels_Row0::RunTest(const FString& Parameters)
{
	TArray<FKismetAIAgent::FModelAliasInfo> Models = FKismetAIAgent::GetAvailableModels();
	if (!TestTrue(TEXT("at least 1 rows available"), Models.Num() > 0)) return false;

	TestEqual(TEXT("Row[0].Alias"),   Models[0].Alias,   FString(TEXT("opus")));
	TestEqual(TEXT("Row[0].ModelId"), Models[0].ModelId, FString(TEXT("claude-opus-5-5")));
	TestEqual(TEXT("Row[0].Tier"),    Models[0].Tier,    FString(TEXT("premium")));
	TestEqual(TEXT("Row[0].RecommendedEffort"), Models[0].RecommendedEffort, FString(TEXT("high")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_AvailableModels_Row1,
	"KismetAI.OpusModelRegistry.AvailableModels.Row1IsFable52Premium",
	RegistryTestFlags)
bool FRegistry_AvailableModels_Row1::RunTest(const FString& Parameters)
{
	TArray<FKismetAIAgent::FModelAliasInfo> Models = FKismetAIAgent::GetAvailableModels();
	if (!TestTrue(TEXT("at least 2 rows available"), Models.Num() > 1)) return false;

	TestEqual(TEXT("Row[1].Alias"),   Models[1].Alias,   FString(TEXT("fable")));
	TestEqual(TEXT("Row[1].ModelId"), Models[1].ModelId, FString(TEXT("claude-fable-5-2")));
	TestEqual(TEXT("Row[1].Tier"),    Models[1].Tier,    FString(TEXT("premium")));
	TestEqual(TEXT("Row[1].RecommendedEffort"), Models[1].RecommendedEffort, FString(TEXT("high")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_AvailableModels_Row2,
	"KismetAI.OpusModelRegistry.AvailableModels.Row2IsSonnet55Standard",
	RegistryTestFlags)
bool FRegistry_AvailableModels_Row2::RunTest(const FString& Parameters)
{
	TArray<FKismetAIAgent::FModelAliasInfo> Models = FKismetAIAgent::GetAvailableModels();
	if (!TestTrue(TEXT("at least 3 rows available"), Models.Num() > 2)) return false;

	TestEqual(TEXT("Row[2].Alias"),   Models[2].Alias,   FString(TEXT("sonnet")));
	TestEqual(TEXT("Row[2].ModelId"), Models[2].ModelId, FString(TEXT("claude-sonnet-5-5")));
	TestEqual(TEXT("Row[2].Tier"),    Models[2].Tier,    FString(TEXT("standard")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_AvailableModels_Row3,
	"KismetAI.OpusModelRegistry.AvailableModels.Row3IsHaiku55Fast",
	RegistryTestFlags)
bool FRegistry_AvailableModels_Row3::RunTest(const FString& Parameters)
{
	TArray<FKismetAIAgent::FModelAliasInfo> Models = FKismetAIAgent::GetAvailableModels();
	if (!TestTrue(TEXT("at least 4 rows available"), Models.Num() > 3)) return false;

	TestEqual(TEXT("Row[3].Alias"),   Models[3].Alias,   FString(TEXT("haiku")));
	TestEqual(TEXT("Row[3].ModelId"), Models[3].ModelId, FString(TEXT("claude-haiku-5-5")));
	TestEqual(TEXT("Row[3].Tier"),    Models[3].Tier,    FString(TEXT("fast")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_AvailableModels_Row4,
	"KismetAI.OpusModelRegistry.AvailableModels.Row4IsGpt6AstraPremium",
	RegistryTestFlags)
bool FRegistry_AvailableModels_Row4::RunTest(const FString& Parameters)
{
	TArray<FKismetAIAgent::FModelAliasInfo> Models = FKismetAIAgent::GetAvailableModels();
	if (!TestTrue(TEXT("at least 5 rows available"), Models.Num() > 4)) return false;

	TestEqual(TEXT("Row[4].Alias"),   Models[4].Alias,   FString(TEXT("astra")));
	TestEqual(TEXT("Row[4].ModelId"), Models[4].ModelId, FString(TEXT("gpt-6-astra")));
	TestEqual(TEXT("Row[4].Tier"),    Models[4].Tier,    FString(TEXT("premium")));
	TestEqual(TEXT("Row[4].RecommendedEffort"), Models[4].RecommendedEffort, FString(TEXT("high")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_AvailableModels_Row5,
	"KismetAI.OpusModelRegistry.AvailableModels.Row5IsGpt61SolStandard",
	RegistryTestFlags)
bool FRegistry_AvailableModels_Row5::RunTest(const FString& Parameters)
{
	TArray<FKismetAIAgent::FModelAliasInfo> Models = FKismetAIAgent::GetAvailableModels();
	if (!TestTrue(TEXT("at least 6 rows available"), Models.Num() > 5)) return false;

	TestEqual(TEXT("Row[5].Alias"),   Models[5].Alias,   FString(TEXT("sol")));
	TestEqual(TEXT("Row[5].ModelId"), Models[5].ModelId, FString(TEXT("gpt-6.1-sol")));
	TestEqual(TEXT("Row[5].Tier"),    Models[5].Tier,    FString(TEXT("standard")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_AvailableModels_Opus47Archived,
	"KismetAI.OpusModelRegistry.AvailableModels.Opus47ArchivedNotDisplayed",
	RegistryTestFlags)
bool FRegistry_AvailableModels_Opus47Archived::RunTest(const FString& Parameters)
{
	TArray<FKismetAIAgent::FModelAliasInfo> Models = FKismetAIAgent::GetAvailableModels();
	for (const FKismetAIAgent::FModelAliasInfo& Model : Models)
	{
		if (Model.ModelId == TEXT("claude-opus-4-7") || Model.Alias == TEXT("opus-4-7"))
		{
			AddError(TEXT("Opus 4.7 should stay archived and out of GetAvailableModels"));
			return false;
		}
	}
	TestEqual(TEXT("archived opus-4-7 alias still resolves"),
		FKismetAIAgent::ResolveModelAlias(TEXT("opus-4-7")),
		FString(TEXT("claude-opus-4-7")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_AvailableModels_MinimaxOnlySupportedRows,
	"KismetAI.OpusModelRegistry.AvailableModels.MinimaxOnlySupportedRows",
	RegistryTestFlags)
bool FRegistry_AvailableModels_MinimaxOnlySupportedRows::RunTest(const FString& Parameters)
{
	TArray<FKismetAIAgent::FModelAliasInfo> Models = FKismetAIAgent::GetAvailableModels();
	bool bHasM3 = false;
	bool bHasM27 = false;
	bool bHasVisibleM27Highspeed = false;

	for (const FKismetAIAgent::FModelAliasInfo& Model : Models)
	{
		bHasM3 |= Model.ModelId == TEXT("MiniMax-M3");
		bHasM27 |= Model.ModelId == TEXT("MiniMax-M2.7");
		bHasVisibleM27Highspeed |= Model.ModelId == TEXT("MiniMax-M2.7-highspeed");

		if (Model.ModelId.StartsWith(TEXT("MiniMax-M2.5"))
			|| Model.ModelId.StartsWith(TEXT("MiniMax-M2.1"))
			|| Model.ModelId == TEXT("MiniMax-M2"))
		{
			AddError(FString::Printf(TEXT("Unsupported MiniMax model should not display: %s"), *Model.ModelId));
			return false;
		}
	}

	TestTrue(TEXT("MiniMax-M3 displays"), bHasM3);
	// M2.7 is API-only and superseded by M3.1 Flash; it still resolves for saved settings.
	TestFalse(TEXT("MiniMax-M2.7 is no longer a picker row"), bHasM27);
	TestEqual(TEXT("MiniMax-M2.7 still resolves"), FKismetAIAgent::ResolveModelAlias(TEXT("m2.7")), FString(TEXT("MiniMax-M2.7")));
	TestFalse(TEXT("MiniMax-M2.7-highspeed is routed through fast mode, not displayed"), bHasVisibleM27Highspeed);
	return true;
}

// Ordering invariant: the panel draws one section per provider (Anthropic →
// OpenAI → Google → the other vendors) and inserts a header wherever the
// provider changes, so each provider's rows must be contiguous. Inside a
// provider, rows run from strongest to fastest (premium → standard → fast).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_AvailableModels_ProviderOrder,
	"KismetAI.OpusModelRegistry.AvailableModels.GroupedByProvider",
	RegistryTestFlags)
bool FRegistry_AvailableModels_ProviderOrder::RunTest(const FString& Parameters)
{
	auto GroupRank = [](const FString& Provider) -> int32
	{
		if (Provider == TEXT("anthropic")) return 0;
		if (Provider == TEXT("openai"))    return 1;
		if (Provider == TEXT("google"))    return 2;
		return 3; // every other vendor, each its own section
	};
	auto TierRank = [](const FString& Tier) -> int32
	{
		if (Tier == TEXT("premium"))  return 0;
		if (Tier == TEXT("standard")) return 1;
		if (Tier == TEXT("fast"))     return 2;
		return 3;
	};

	TArray<FKismetAIAgent::FModelAliasInfo> Models = FKismetAIAgent::GetAvailableModels();
	TSet<FString> ClosedProviders;
	for (int32 i = 1; i < Models.Num(); ++i)
	{
		const FKismetAIAgent::FModelAliasInfo& Prev = Models[i - 1];
		const FKismetAIAgent::FModelAliasInfo& Cur = Models[i];
		TestTrue(FString::Printf(TEXT("%s: provider group does not go back"), *Cur.ModelId),
			GroupRank(Cur.Provider) >= GroupRank(Prev.Provider));
		// An aggregator's group is sorted per vendor ("anthropic/...", "openai/...").
		auto Vendor = [](const FKismetAIAgent::FModelAliasInfo& Row)
		{
			int32 Slash = INDEX_NONE;
			return Row.ModelId.FindChar(TEXT('/'), Slash) ? Row.ModelId.Left(Slash) : FString();
		};
		if (Cur.Provider == Prev.Provider)
		{
			if (Vendor(Cur) == Vendor(Prev))
			{
				TestTrue(FString::Printf(TEXT("%s: tier does not go back within %s"), *Cur.ModelId, *Cur.Provider),
					TierRank(Cur.Tier) >= TierRank(Prev.Tier));
			}
		}
		else
		{
			ClosedProviders.Add(Prev.Provider);
			TestFalse(FString::Printf(TEXT("%s: %s rows are contiguous"), *Cur.ModelId, *Cur.Provider),
				ClosedProviders.Contains(Cur.Provider));
		}
	}
	TestEqual(TEXT("First row is Anthropic"), Models.Num() > 0 ? Models[0].Provider : FString(), FString(TEXT("anthropic")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.8 — UKismetAIBridge::GetAvailableModels JSON output (check)
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegistry_BridgeJson_Opus48HasTierAndEffort,
	"KismetAI.OpusModelRegistry.BridgeJson.FirstRowHasTierEffortAndCapabilities",
	RegistryTestFlags)
bool FRegistry_BridgeJson_Opus48HasTierAndEffort::RunTest(const FString& Parameters)
{
	// The real bridge output — the panel parses exactly this.
	const FString Json = NewObject<UKismetAIBridge>()->GetAvailableModels();
	TArray<TSharedPtr<FJsonValue>> Rows;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!TestTrue(TEXT("bridge JSON parses as an array"), FJsonSerializer::Deserialize(Reader, Rows) && Rows.Num() > 0)) return false;

	const TSharedPtr<FJsonObject> First = Rows[0]->AsObject();
	if (!TestTrue(TEXT("row 0 is an object"), First.IsValid())) return false;
	TestEqual(TEXT("alias"), First->GetStringField(TEXT("alias")), FString(TEXT("opus")));
	TestEqual(TEXT("model_id"), First->GetStringField(TEXT("model_id")), FString(TEXT("claude-opus-5-5")));
	TestEqual(TEXT("tier"), First->GetStringField(TEXT("tier")), FString(TEXT("premium")));
	TestEqual(TEXT("recommended_effort"), First->GetStringField(TEXT("recommended_effort")), FString(TEXT("high")));
	TestEqual(TEXT("context_window"), static_cast<int32>(First->GetNumberField(TEXT("context_window"))), 1000000);
	TestEqual(TEXT("provider"), First->GetStringField(TEXT("provider")), FString(TEXT("anthropic")));
	TestTrue(TEXT("supports_fast_mode"), First->GetBoolField(TEXT("supports_fast_mode")));
	TestEqual(TEXT("effort_levels has five levels"), First->GetArrayField(TEXT("effort_levels")).Num(), 5);
	return true;
}
