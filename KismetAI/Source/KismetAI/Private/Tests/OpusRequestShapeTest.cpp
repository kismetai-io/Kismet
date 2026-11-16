// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIAgent.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

constexpr auto ReqShapeTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// Helper: build a minimal FKismetAIAgentConfig for a given model
// ─────────────────────────────────────────────────────────────────────────────

static FKismetAIAgentConfig MakeConfig(
	const FString& ModelId,
	EThinkingMode ThinkingMode,
	EEffortLevel EffortLevel,
	float Temperature = 0.5f,
	int32 ThinkingBudgetTokens = 10000,
	int32 MaxTokens = 4096)
{
	FKismetAIAgentConfig Cfg;
	Cfg.Model = ModelId;
	Cfg.ThinkingMode = ThinkingMode;
	Cfg.EffortLevel = EffortLevel;
	Cfg.Temperature = Temperature;
	Cfg.ThinkingBudgetTokens = ThinkingBudgetTokens;
	Cfg.MaxTokens = MaxTokens;
	return Cfg;
}

// ─────────────────────────────────────────────────────────────────────────────
// Temperature/top_p/top_k never in body for opus-4-7
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_IV1_Opus47_AdaptiveNoTemperature,
	"KismetAI.OpusRequestShape.Opus47AdaptiveNoTemperature",
	ReqShapeTestFlags)
bool FReqShape_IV1_Opus47_AdaptiveNoTemperature::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-opus-4-7"), EThinkingMode::Adaptive, EEffortLevel::XHigh);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	bool bThinkingEnabled = false;
	bool bEffortEnabled = false;

	FKismetAIAgent::ApplyModelRequestShape(Body, Cfg, bThinkingEnabled, bEffortEnabled);

	TestFalse(TEXT("Opus 4.7 + Adaptive — no temperature field"),
		Body->HasField(TEXT("temperature")));
	TestFalse(TEXT("Opus 4.7 + Adaptive — no top_p field"),
		Body->HasField(TEXT("top_p")));
	TestFalse(TEXT("Opus 4.7 + Adaptive — no top_k field"),
		Body->HasField(TEXT("top_k")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_IV1_Opus47_DisabledNoTemperature,
	"KismetAI.OpusRequestShape.Opus47DisabledNoTemperature",
	ReqShapeTestFlags)
bool FReqShape_IV1_Opus47_DisabledNoTemperature::RunTest(const FString& Parameters)
{
	// Opus 4.7 with thinking disabled — still no temperature (ModelAcceptsSamplingParams returns false)
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-opus-4-7"), EThinkingMode::Disabled, EEffortLevel::XHigh);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	bool bThinkingEnabled = false;
	bool bEffortEnabled = false;

	FKismetAIAgent::ApplyModelRequestShape(Body, Cfg, bThinkingEnabled, bEffortEnabled);

	TestFalse(TEXT("Opus 4.7 + Disabled — no temperature field even when thinking off"),
		Body->HasField(TEXT("temperature")));
	TestFalse(TEXT("Opus 4.7 + Disabled — no top_p"),
		Body->HasField(TEXT("top_p")));
	TestFalse(TEXT("Opus 4.7 + Disabled — no top_k"),
		Body->HasField(TEXT("top_k")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_IV1_Opus46_DisabledHasTemperature,
	"KismetAI.OpusRequestShape.Opus46DisabledHasTemperature",
	ReqShapeTestFlags)
bool FReqShape_IV1_Opus46_DisabledHasTemperature::RunTest(const FString& Parameters)
{
	// Opus 4.6 with thinking disabled — temperature IS sent
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-opus-4-6"), EThinkingMode::Disabled, EEffortLevel::Medium, 0.5f);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	bool bThinkingEnabled = false;
	bool bEffortEnabled = false;

	FKismetAIAgent::ApplyModelRequestShape(Body, Cfg, bThinkingEnabled, bEffortEnabled);

	TestTrue(TEXT("Opus 4.6 + Disabled — temperature IS emitted"),
		Body->HasField(TEXT("temperature")));

	double Temp = 0.0;
	Body->TryGetNumberField(TEXT("temperature"), Temp);
	TestTrue(TEXT("Opus 4.6 temperature value correct"),
		FMath::IsNearlyEqual(Temp, 0.5, 0.0001));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_IV1_Opus46_AdaptiveNoTemperature,
	"KismetAI.OpusRequestShape.Opus46AdaptiveNoTemperature",
	ReqShapeTestFlags)
bool FReqShape_IV1_Opus46_AdaptiveNoTemperature::RunTest(const FString& Parameters)
{
	// Opus 4.6 with thinking enabled — temperature NOT sent (rule: temp must be 1 when thinking enabled)
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-opus-4-6"), EThinkingMode::Adaptive, EEffortLevel::Medium);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	bool bThinkingEnabled = false;
	bool bEffortEnabled = false;

	FKismetAIAgent::ApplyModelRequestShape(Body, Cfg, bThinkingEnabled, bEffortEnabled);

	TestFalse(TEXT("Opus 4.6 + Adaptive — no temperature when thinking enabled"),
		Body->HasField(TEXT("temperature")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_IV1_Sonnet46_DisabledHasTemperature,
	"KismetAI.OpusRequestShape.Sonnet46DisabledHasTemperature",
	ReqShapeTestFlags)
bool FReqShape_IV1_Sonnet46_DisabledHasTemperature::RunTest(const FString& Parameters)
{
	// Sonnet 4.6 with thinking disabled — temperature IS sent
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-sonnet-4-6"), EThinkingMode::Disabled, EEffortLevel::Medium, 0.5f);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	bool bThinkingEnabled = false;
	bool bEffortEnabled = false;

	FKismetAIAgent::ApplyModelRequestShape(Body, Cfg, bThinkingEnabled, bEffortEnabled);

	TestTrue(TEXT("Sonnet 4.6 + Disabled — temperature IS emitted"),
		Body->HasField(TEXT("temperature")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_IV1_Sonnet46_AdaptiveNoTemperature,
	"KismetAI.OpusRequestShape.Sonnet46AdaptiveNoTemperature",
	ReqShapeTestFlags)
bool FReqShape_IV1_Sonnet46_AdaptiveNoTemperature::RunTest(const FString& Parameters)
{
	// Sonnet 4.6 with thinking enabled — temperature NOT sent
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-sonnet-4-6"), EThinkingMode::Adaptive, EEffortLevel::Medium);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	bool bThinkingEnabled = false;
	bool bEffortEnabled = false;

	FKismetAIAgent::ApplyModelRequestShape(Body, Cfg, bThinkingEnabled, bEffortEnabled);

	TestFalse(TEXT("Sonnet 4.6 + Adaptive — no temperature when thinking enabled"),
		Body->HasField(TEXT("temperature")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Thinking field shape for opus-4-7 (adaptive+summarized, no budget_tokens)
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_IV2_Opus47_AdaptiveSummarized,
	"KismetAI.OpusRequestShape.Opus47AdaptiveHasSummarizedDisplay",
	ReqShapeTestFlags)
bool FReqShape_IV2_Opus47_AdaptiveSummarized::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-opus-4-7"), EThinkingMode::Adaptive, EEffortLevel::XHigh);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	bool bThinkingEnabled = false;
	bool bEffortEnabled = false;

	FKismetAIAgent::ApplyModelRequestShape(Body, Cfg, bThinkingEnabled, bEffortEnabled);

	TestTrue(TEXT("Opus 4.7 + Adaptive — bThinkingEnabled set true"),
		bThinkingEnabled);

	// Must have thinking field
	TestTrue(TEXT("Opus 4.7 + Adaptive — thinking field present"),
		Body->HasField(TEXT("thinking")));

	const TSharedPtr<FJsonObject>* ThinkingObj = nullptr;
	Body->TryGetObjectField(TEXT("thinking"), ThinkingObj);
	if (!ThinkingObj)
	{
		AddError(TEXT("Thinking field is not an object"));
		return false;
	}

	FString Type;
	(*ThinkingObj)->TryGetStringField(TEXT("type"), Type);
	TestEqual(TEXT("Opus 4.7 thinking.type == 'adaptive'"), Type, FString(TEXT("adaptive")));

	FString Display;
	(*ThinkingObj)->TryGetStringField(TEXT("display"), Display);
	TestEqual(TEXT("Opus 4.7 thinking.display == 'summarized'"), Display, FString(TEXT("summarized")));

	// Must NOT have budget_tokens
	TestFalse(TEXT("Opus 4.7 thinking must not have budget_tokens"),
		(*ThinkingObj)->HasField(TEXT("budget_tokens")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_IV2_Opus47_EnabledFallsBackToAdaptive,
	"KismetAI.OpusRequestShape.Opus47EnabledFallsBackToAdaptiveSummarized",
	ReqShapeTestFlags)
bool FReqShape_IV2_Opus47_EnabledFallsBackToAdaptive::RunTest(const FString& Parameters)
{
	// Opus 4.7 with Enabled mode — should fall back to adaptive+summarized (no extended thinking support)
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-opus-4-7"), EThinkingMode::Enabled, EEffortLevel::XHigh);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	bool bThinkingEnabled = false;
	bool bEffortEnabled = false;

	FKismetAIAgent::ApplyModelRequestShape(Body, Cfg, bThinkingEnabled, bEffortEnabled);

	TestTrue(TEXT("Opus 4.7 + Enabled — bThinkingEnabled set true"),
		bThinkingEnabled);

	const TSharedPtr<FJsonObject>* ThinkingObj = nullptr;
	Body->TryGetObjectField(TEXT("thinking"), ThinkingObj);
	if (!ThinkingObj)
	{
		AddError(TEXT("Thinking field is not an object"));
		return false;
	}

	FString Type;
	(*ThinkingObj)->TryGetStringField(TEXT("type"), Type);
	TestEqual(TEXT("Opus 4.7 + Enabled falls back to 'adaptive'"), Type, FString(TEXT("adaptive")));

	// Must carry display:"summarized" — requires it on opus-4-7 whenever thinking is emitted.
	FString Display;
	(*ThinkingObj)->TryGetStringField(TEXT("display"), Display);
	TestEqual(TEXT("Opus 4.7 + Enabled fallback emits display:'summarized'"),
		Display, FString(TEXT("summarized")));

	// Must NOT have budget_tokens
	TestFalse(TEXT("Opus 4.7 + Enabled fallback must not have budget_tokens"),
		(*ThinkingObj)->HasField(TEXT("budget_tokens")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_IV2_Opus46_AdaptiveType,
	"KismetAI.OpusRequestShape.Opus46AdaptiveType",
	ReqShapeTestFlags)
bool FReqShape_IV2_Opus46_AdaptiveType::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-opus-4-6"), EThinkingMode::Adaptive, EEffortLevel::Medium);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	bool bThinkingEnabled = false;
	bool bEffortEnabled = false;

	FKismetAIAgent::ApplyModelRequestShape(Body, Cfg, bThinkingEnabled, bEffortEnabled);

	TestTrue(TEXT("Opus 4.6 + Adaptive — bThinkingEnabled set true"), bThinkingEnabled);

	const TSharedPtr<FJsonObject>* ThinkingObj = nullptr;
	Body->TryGetObjectField(TEXT("thinking"), ThinkingObj);
	if (!ThinkingObj)
	{
		AddError(TEXT("Thinking field is not an object"));
		return false;
	}

	FString Type;
	(*ThinkingObj)->TryGetStringField(TEXT("type"), Type);
	TestEqual(TEXT("Opus 4.6 + Adaptive — thinking.type == 'adaptive'"), Type, FString(TEXT("adaptive")));

	// Opus 4.6 supports extended thinking, so no display:"summarized"
	FString Display;
	bool bHasDisplay = (*ThinkingObj)->TryGetStringField(TEXT("display"), Display);
	TestFalse(TEXT("Opus 4.6 + Adaptive — no display:summarized override"),
		bHasDisplay && Display == TEXT("summarized"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_IV2_Opus46_EnabledWithBudget,
	"KismetAI.OpusRequestShape.Opus46EnabledWithBudgetTokens",
	ReqShapeTestFlags)
bool FReqShape_IV2_Opus46_EnabledWithBudget::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-opus-4-6"), EThinkingMode::Enabled, EEffortLevel::Medium, 1.0f, 10000, 4096);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	bool bThinkingEnabled = false;
	bool bEffortEnabled = false;

	FKismetAIAgent::ApplyModelRequestShape(Body, Cfg, bThinkingEnabled, bEffortEnabled);

	TestTrue(TEXT("Opus 4.6 + Enabled — bThinkingEnabled set true"), bThinkingEnabled);

	const TSharedPtr<FJsonObject>* ThinkingObj = nullptr;
	Body->TryGetObjectField(TEXT("thinking"), ThinkingObj);
	if (!ThinkingObj)
	{
		AddError(TEXT("Thinking field is not an object"));
		return false;
	}

	FString Type;
	(*ThinkingObj)->TryGetStringField(TEXT("type"), Type);
	TestEqual(TEXT("Opus 4.6 + Enabled — thinking.type == 'enabled'"), Type, FString(TEXT("enabled")));

	// budget_tokens must be min(10000, 4096-1) = 4095
	double Budget = 0.0;
	TestTrue(TEXT("Opus 4.6 + Enabled — budget_tokens present"),
		(*ThinkingObj)->TryGetNumberField(TEXT("budget_tokens"), Budget));
	TestEqual(TEXT("Opus 4.6 + Enabled — budget_tokens == min(10000, 4096-1) = 4095"),
		(int32)Budget, 4095);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_IV2_Sonnet46_AdaptiveType,
	"KismetAI.OpusRequestShape.Sonnet46AdaptiveType",
	ReqShapeTestFlags)
bool FReqShape_IV2_Sonnet46_AdaptiveType::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-sonnet-4-6"), EThinkingMode::Adaptive, EEffortLevel::Medium);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	bool bThinkingEnabled = false;
	bool bEffortEnabled = false;

	FKismetAIAgent::ApplyModelRequestShape(Body, Cfg, bThinkingEnabled, bEffortEnabled);

	TestTrue(TEXT("Sonnet 4.6 + Adaptive — bThinkingEnabled set true"), bThinkingEnabled);

	const TSharedPtr<FJsonObject>* ThinkingObj = nullptr;
	Body->TryGetObjectField(TEXT("thinking"), ThinkingObj);
	if (!ThinkingObj)
	{
		AddError(TEXT("Thinking field is not an object"));
		return false;
	}

	FString Type;
	(*ThinkingObj)->TryGetStringField(TEXT("type"), Type);
	TestEqual(TEXT("Sonnet 4.6 + Adaptive — thinking.type == 'adaptive'"), Type, FString(TEXT("adaptive")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Effort downgrade: xhigh/max -> high for non-opus-4-7 models
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_IV6_Opus47_XHighPassthrough,
	"KismetAI.OpusRequestShape.Opus47XHighPassthrough",
	ReqShapeTestFlags)
bool FReqShape_IV6_Opus47_XHighPassthrough::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-opus-4-7"), EThinkingMode::Adaptive, EEffortLevel::XHigh);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	bool bThinkingEnabled = false;
	bool bEffortEnabled = false;

	FKismetAIAgent::ApplyModelRequestShape(Body, Cfg, bThinkingEnabled, bEffortEnabled);

	TestTrue(TEXT("Opus 4.7 + XHigh — bEffortEnabled set true"), bEffortEnabled);

	const TSharedPtr<FJsonObject>* OutputConfig = nullptr;
	Body->TryGetObjectField(TEXT("output_config"), OutputConfig);
	if (!OutputConfig)
	{
		AddError(TEXT("output_config field is not an object"));
		return false;
	}

	FString Effort;
	(*OutputConfig)->TryGetStringField(TEXT("effort"), Effort);
	TestEqual(TEXT("Opus 4.7 XHigh passes through as 'xhigh'"), Effort, FString(TEXT("xhigh")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_IV6_Opus47_MaxPassthrough,
	"KismetAI.OpusRequestShape.Opus47MaxPassthrough",
	ReqShapeTestFlags)
bool FReqShape_IV6_Opus47_MaxPassthrough::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-opus-4-7"), EThinkingMode::Adaptive, EEffortLevel::Max);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	bool bThinkingEnabled = false;
	bool bEffortEnabled = false;

	FKismetAIAgent::ApplyModelRequestShape(Body, Cfg, bThinkingEnabled, bEffortEnabled);

	const TSharedPtr<FJsonObject>* OutputConfig = nullptr;
	Body->TryGetObjectField(TEXT("output_config"), OutputConfig);
	if (!OutputConfig)
	{
		AddError(TEXT("output_config field is not an object"));
		return false;
	}

	FString Effort;
	(*OutputConfig)->TryGetStringField(TEXT("effort"), Effort);
	TestEqual(TEXT("Opus 4.7 Max passes through as 'max'"), Effort, FString(TEXT("max")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_IV6_Opus46_XHighDowngradedToHigh,
	"KismetAI.OpusRequestShape.Opus46XHighDowngradedToHigh",
	ReqShapeTestFlags)
bool FReqShape_IV6_Opus46_XHighDowngradedToHigh::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-opus-4-6"), EThinkingMode::Adaptive, EEffortLevel::XHigh);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	bool bThinkingEnabled = false;
	bool bEffortEnabled = false;

	FKismetAIAgent::ApplyModelRequestShape(Body, Cfg, bThinkingEnabled, bEffortEnabled);

	TestTrue(TEXT("Opus 4.6 + XHigh — bEffortEnabled set true"), bEffortEnabled);

	const TSharedPtr<FJsonObject>* OutputConfig = nullptr;
	Body->TryGetObjectField(TEXT("output_config"), OutputConfig);
	if (!OutputConfig)
	{
		AddError(TEXT("output_config field is not an object"));
		return false;
	}

	FString Effort;
	(*OutputConfig)->TryGetStringField(TEXT("effort"), Effort);
	TestEqual(TEXT("Opus 4.6 XHigh downgraded to 'high'"), Effort, FString(TEXT("high")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_IV6_Opus46_MaxDowngradedToHigh,
	"KismetAI.OpusRequestShape.Opus46MaxDowngradedToHigh",
	ReqShapeTestFlags)
bool FReqShape_IV6_Opus46_MaxDowngradedToHigh::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-opus-4-6"), EThinkingMode::Adaptive, EEffortLevel::Max);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	bool bThinkingEnabled = false;
	bool bEffortEnabled = false;

	FKismetAIAgent::ApplyModelRequestShape(Body, Cfg, bThinkingEnabled, bEffortEnabled);

	const TSharedPtr<FJsonObject>* OutputConfig = nullptr;
	Body->TryGetObjectField(TEXT("output_config"), OutputConfig);
	if (!OutputConfig)
	{
		AddError(TEXT("output_config field is not an object"));
		return false;
	}

	FString Effort;
	(*OutputConfig)->TryGetStringField(TEXT("effort"), Effort);
	TestEqual(TEXT("Opus 4.6 Max downgraded to 'high'"), Effort, FString(TEXT("high")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_IV6_Sonnet46_XHighDowngradedToHigh,
	"KismetAI.OpusRequestShape.Sonnet46XHighDowngradedToHigh",
	ReqShapeTestFlags)
bool FReqShape_IV6_Sonnet46_XHighDowngradedToHigh::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-sonnet-4-6"), EThinkingMode::Adaptive, EEffortLevel::XHigh);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	bool bThinkingEnabled = false;
	bool bEffortEnabled = false;

	FKismetAIAgent::ApplyModelRequestShape(Body, Cfg, bThinkingEnabled, bEffortEnabled);

	const TSharedPtr<FJsonObject>* OutputConfig = nullptr;
	Body->TryGetObjectField(TEXT("output_config"), OutputConfig);
	if (!OutputConfig)
	{
		AddError(TEXT("output_config field is not an object"));
		return false;
	}

	FString Effort;
	(*OutputConfig)->TryGetStringField(TEXT("effort"), Effort);
	TestEqual(TEXT("Sonnet 4.6 XHigh downgraded to 'high'"), Effort, FString(TEXT("high")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Beta header: opus-4-7 gets no interleaved-thinking or effort-2025-11-24
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_IV3_Opus47_NoBetaHeaders,
	"KismetAI.OpusRequestShape.Opus47NoBetaHeaders",
	ReqShapeTestFlags)
bool FReqShape_IV3_Opus47_NoBetaHeaders::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-opus-4-7"), EThinkingMode::Adaptive, EEffortLevel::XHigh);
	FString Header = FKismetAIAgent::BuildAnthropicBetaHeader(Cfg, /*bThinkingEnabled=*/true, /*bEffortEnabled=*/true);

	TestFalse(TEXT("Opus 4.7 — no interleaved-thinking in header"),
		Header.Contains(TEXT("interleaved-thinking-2025-05-14")));
	TestFalse(TEXT("Opus 4.7 — no effort-2025-11-24 in header"),
		Header.Contains(TEXT("effort-2025-11-24")));

	// For opus-4-7 with thinking+effort, header should be empty (no legacy betas needed)
	TestTrue(TEXT("Opus 4.7 returns empty header string"),
		Header.IsEmpty());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_IV3_Opus46_BothBetaHeaders,
	"KismetAI.OpusRequestShape.Opus46BothBetaHeaders",
	ReqShapeTestFlags)
bool FReqShape_IV3_Opus46_BothBetaHeaders::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-opus-4-6"), EThinkingMode::Adaptive, EEffortLevel::XHigh);
	FString Header = FKismetAIAgent::BuildAnthropicBetaHeader(Cfg, /*bThinkingEnabled=*/true, /*bEffortEnabled=*/true);

	TestTrue(TEXT("Opus 4.6 — contains interleaved-thinking-2025-05-14"),
		Header.Contains(TEXT("interleaved-thinking-2025-05-14")));
	TestTrue(TEXT("Opus 4.6 — contains effort-2025-11-24"),
		Header.Contains(TEXT("effort-2025-11-24")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_IV3_Sonnet46_BothBetaHeaders,
	"KismetAI.OpusRequestShape.Sonnet46BothBetaHeaders",
	ReqShapeTestFlags)
bool FReqShape_IV3_Sonnet46_BothBetaHeaders::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-sonnet-4-6"), EThinkingMode::Adaptive, EEffortLevel::XHigh);
	FString Header = FKismetAIAgent::BuildAnthropicBetaHeader(Cfg, /*bThinkingEnabled=*/true, /*bEffortEnabled=*/true);

	TestTrue(TEXT("Sonnet 4.6 — contains interleaved-thinking-2025-05-14"),
		Header.Contains(TEXT("interleaved-thinking-2025-05-14")));
	TestTrue(TEXT("Sonnet 4.6 — contains effort-2025-11-24"),
		Header.Contains(TEXT("effort-2025-11-24")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_IV3_ThinkingOnlyNoEffort,
	"KismetAI.OpusRequestShape.Opus46ThinkingOnlyNoEffortBeta",
	ReqShapeTestFlags)
bool FReqShape_IV3_ThinkingOnlyNoEffort::RunTest(const FString& Parameters)
{
	// When bEffortEnabled=false, effort-2025-11-24 should not be in header
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-opus-4-6"), EThinkingMode::Adaptive, EEffortLevel::Medium);
	FString Header = FKismetAIAgent::BuildAnthropicBetaHeader(Cfg, /*bThinkingEnabled=*/true, /*bEffortEnabled=*/false);

	TestTrue(TEXT("Thinking only — contains interleaved-thinking"),
		Header.Contains(TEXT("interleaved-thinking-2025-05-14")));
	TestFalse(TEXT("Thinking only — no effort-2025-11-24"),
		Header.Contains(TEXT("effort-2025-11-24")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_IV3_EffortOnlyNoThinking,
	"KismetAI.OpusRequestShape.Opus46EffortOnlyNoThinkingBeta",
	ReqShapeTestFlags)
bool FReqShape_IV3_EffortOnlyNoThinking::RunTest(const FString& Parameters)
{
	// When bThinkingEnabled=false, interleaved-thinking should not be in header
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-opus-4-6"), EThinkingMode::Disabled, EEffortLevel::XHigh);
	FString Header = FKismetAIAgent::BuildAnthropicBetaHeader(Cfg, /*bThinkingEnabled=*/false, /*bEffortEnabled=*/true);

	TestFalse(TEXT("Effort only — no interleaved-thinking"),
		Header.Contains(TEXT("interleaved-thinking-2025-05-14")));
	TestTrue(TEXT("Effort only — contains effort-2025-11-24"),
		Header.Contains(TEXT("effort-2025-11-24")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_IV3_NeitherEnabled_EmptyHeader,
	"KismetAI.OpusRequestShape.NeitherEnabledEmptyHeader",
	ReqShapeTestFlags)
bool FReqShape_IV3_NeitherEnabled_EmptyHeader::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-sonnet-4-6"), EThinkingMode::Disabled, EEffortLevel::Medium);
	FString Header = FKismetAIAgent::BuildAnthropicBetaHeader(Cfg, /*bThinkingEnabled=*/false, /*bEffortEnabled=*/false);

	TestTrue(TEXT("Neither enabled — empty header"), Header.IsEmpty());

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Mythos 5: always-on adaptive thinking, no sampling params, no legacy betas.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_Mythos5_DisabledStillAdaptive,
	"KismetAI.OpusRequestShape.Mythos5DisabledStillAdaptive",
	ReqShapeTestFlags)
bool FReqShape_Mythos5_DisabledStillAdaptive::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-mythos-5"), EThinkingMode::Disabled, EEffortLevel::XHigh);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	bool bThinkingEnabled = false;
	bool bEffortEnabled = false;

	FKismetAIAgent::ApplyModelRequestShape(Body, Cfg, bThinkingEnabled, bEffortEnabled);

	TestTrue(TEXT("Mythos 5 emits thinking even when persisted setting is disabled"), bThinkingEnabled);
	TestFalse(TEXT("Mythos 5: no temperature field"), Body->HasField(TEXT("temperature")));

	const TSharedPtr<FJsonObject>* ThinkingObj = nullptr;
	Body->TryGetObjectField(TEXT("thinking"), ThinkingObj);
	if (!ThinkingObj)
	{
		AddError(TEXT("Mythos 5: thinking field is not an object"));
		return false;
	}

	FString Type;
	(*ThinkingObj)->TryGetStringField(TEXT("type"), Type);
	TestEqual(TEXT("Mythos 5 thinking.type == adaptive"), Type, FString(TEXT("adaptive")));

	FString Display;
	(*ThinkingObj)->TryGetStringField(TEXT("display"), Display);
	TestEqual(TEXT("Mythos 5 thinking.display == summarized"), Display, FString(TEXT("summarized")));

	TestFalse(TEXT("Mythos 5 thinking must not have budget_tokens"),
		(*ThinkingObj)->HasField(TEXT("budget_tokens")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_Mythos5_MaxPassthrough,
	"KismetAI.OpusRequestShape.Mythos5MaxPassthrough",
	ReqShapeTestFlags)
bool FReqShape_Mythos5_MaxPassthrough::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-mythos-5"), EThinkingMode::Adaptive, EEffortLevel::Max);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	bool bThinkingEnabled = false;
	bool bEffortEnabled = false;

	FKismetAIAgent::ApplyModelRequestShape(Body, Cfg, bThinkingEnabled, bEffortEnabled);

	const TSharedPtr<FJsonObject>* OutputConfig = nullptr;
	Body->TryGetObjectField(TEXT("output_config"), OutputConfig);
	if (!OutputConfig)
	{
		AddError(TEXT("Mythos 5: output_config field is not an object"));
		return false;
	}

	FString Effort;
	(*OutputConfig)->TryGetStringField(TEXT("effort"), Effort);
	TestEqual(TEXT("Mythos 5 Max passes through as max"), Effort, FString(TEXT("max")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_Mythos5_NoBetaHeaders,
	"KismetAI.OpusRequestShape.Mythos5NoBetaHeaders",
	ReqShapeTestFlags)
bool FReqShape_Mythos5_NoBetaHeaders::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-mythos-5"), EThinkingMode::Adaptive, EEffortLevel::XHigh);
	FString Header = FKismetAIAgent::BuildAnthropicBetaHeader(Cfg, /*bThinkingEnabled=*/true, /*bEffortEnabled=*/true);

	TestFalse(TEXT("Mythos 5: no interleaved-thinking in header"),
		Header.Contains(TEXT("interleaved-thinking-2025-05-14")));
	TestFalse(TEXT("Mythos 5: no effort-2025-11-24 in header"),
		Header.Contains(TEXT("effort-2025-11-24")));
	TestTrue(TEXT("Mythos 5 returns empty header string"), Header.IsEmpty());
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Opus 4.8 — request shape parity with 4.7: no sampling params, adaptive+summarized
// thinking, native xhigh/max passthrough, and no beta headers.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_Opus48_AdaptiveNoTemperature,
	"KismetAI.OpusRequestShape.Opus48AdaptiveNoTemperature",
	ReqShapeTestFlags)
bool FReqShape_Opus48_AdaptiveNoTemperature::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-opus-4-8"), EThinkingMode::Adaptive, EEffortLevel::XHigh);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	bool bThinkingEnabled = false;
	bool bEffortEnabled = false;

	FKismetAIAgent::ApplyModelRequestShape(Body, Cfg, bThinkingEnabled, bEffortEnabled);

	TestFalse(TEXT("Opus 4.8 — no temperature field"), Body->HasField(TEXT("temperature")));
	TestFalse(TEXT("Opus 4.8 — no top_p field"), Body->HasField(TEXT("top_p")));
	TestFalse(TEXT("Opus 4.8 — no top_k field"), Body->HasField(TEXT("top_k")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_Opus48_AdaptiveSummarized,
	"KismetAI.OpusRequestShape.Opus48AdaptiveHasSummarizedDisplay",
	ReqShapeTestFlags)
bool FReqShape_Opus48_AdaptiveSummarized::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-opus-4-8"), EThinkingMode::Adaptive, EEffortLevel::XHigh);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	bool bThinkingEnabled = false;
	bool bEffortEnabled = false;

	FKismetAIAgent::ApplyModelRequestShape(Body, Cfg, bThinkingEnabled, bEffortEnabled);

	TestTrue(TEXT("Opus 4.8 + Adaptive — bThinkingEnabled set true"), bThinkingEnabled);

	const TSharedPtr<FJsonObject>* ThinkingObj = nullptr;
	Body->TryGetObjectField(TEXT("thinking"), ThinkingObj);
	if (!ThinkingObj)
	{
		AddError(TEXT("Opus 4.8: thinking field is not an object"));
		return false;
	}

	FString Type;
	(*ThinkingObj)->TryGetStringField(TEXT("type"), Type);
	TestEqual(TEXT("Opus 4.8 thinking.type == 'adaptive'"), Type, FString(TEXT("adaptive")));

	FString Display;
	(*ThinkingObj)->TryGetStringField(TEXT("display"), Display);
	TestEqual(TEXT("Opus 4.8 thinking.display == 'summarized'"), Display, FString(TEXT("summarized")));

	TestFalse(TEXT("Opus 4.8 thinking must not have budget_tokens"),
		(*ThinkingObj)->HasField(TEXT("budget_tokens")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_Opus48_XHighPassthrough,
	"KismetAI.OpusRequestShape.Opus48XHighPassthrough",
	ReqShapeTestFlags)
bool FReqShape_Opus48_XHighPassthrough::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-opus-4-8"), EThinkingMode::Adaptive, EEffortLevel::Max);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	bool bThinkingEnabled = false;
	bool bEffortEnabled = false;

	FKismetAIAgent::ApplyModelRequestShape(Body, Cfg, bThinkingEnabled, bEffortEnabled);

	const TSharedPtr<FJsonObject>* OutputConfig = nullptr;
	Body->TryGetObjectField(TEXT("output_config"), OutputConfig);
	if (!OutputConfig)
	{
		AddError(TEXT("Opus 4.8: output_config field is not an object"));
		return false;
	}

	FString Effort;
	(*OutputConfig)->TryGetStringField(TEXT("effort"), Effort);
	TestEqual(TEXT("Opus 4.8 Max passes through as 'max' (not downgraded)"), Effort, FString(TEXT("max")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReqShape_Opus48_NoBetaHeaders,
	"KismetAI.OpusRequestShape.Opus48NoBetaHeaders",
	ReqShapeTestFlags)
bool FReqShape_Opus48_NoBetaHeaders::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("claude-opus-4-8"), EThinkingMode::Adaptive, EEffortLevel::XHigh);
	FString Header = FKismetAIAgent::BuildAnthropicBetaHeader(Cfg, /*bThinkingEnabled=*/true, /*bEffortEnabled=*/true);

	TestFalse(TEXT("Opus 4.8 — no interleaved-thinking in header"),
		Header.Contains(TEXT("interleaved-thinking-2025-05-14")));
	TestFalse(TEXT("Opus 4.8 — no effort-2025-11-24 in header"),
		Header.Contains(TEXT("effort-2025-11-24")));
	TestTrue(TEXT("Opus 4.8 returns empty header string"), Header.IsEmpty());
	return true;
}
