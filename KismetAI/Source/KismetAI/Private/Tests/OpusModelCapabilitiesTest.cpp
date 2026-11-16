// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIAgent.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

constexpr auto OpusCapabilityTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// ModelSupportsExtendedThinking — legacy {type:"enabled", budget_tokens:N} path
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_ExtendedThinking_Opus47False,
	"KismetAI.OpusModelCapabilities.ExtendedThinking.Opus47ReturnsFalse",
	OpusCapabilityTestFlags)
bool FOpusCaps_ExtendedThinking_Opus47False::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("Opus 4.7 does NOT support legacy extended thinking"),
		FKismetAIAgent::ModelSupportsExtendedThinking(TEXT("claude-opus-4-7")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_ExtendedThinking_Opus46True,
	"KismetAI.OpusModelCapabilities.ExtendedThinking.Opus46ReturnsTrue",
	OpusCapabilityTestFlags)
bool FOpusCaps_ExtendedThinking_Opus46True::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Opus 4.6 supports legacy extended thinking"),
		FKismetAIAgent::ModelSupportsExtendedThinking(TEXT("claude-opus-4-6")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_ExtendedThinking_Sonnet46True,
	"KismetAI.OpusModelCapabilities.ExtendedThinking.Sonnet46ReturnsTrue",
	OpusCapabilityTestFlags)
bool FOpusCaps_ExtendedThinking_Sonnet46True::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Sonnet 4.6 supports legacy extended thinking"),
		FKismetAIAgent::ModelSupportsExtendedThinking(TEXT("claude-sonnet-4-6")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_ExtendedThinking_Haiku45True,
	"KismetAI.OpusModelCapabilities.ExtendedThinking.Haiku45ReturnsTrue",
	OpusCapabilityTestFlags)
bool FOpusCaps_ExtendedThinking_Haiku45True::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Haiku 4.5 supports legacy extended thinking"),
		FKismetAIAgent::ModelSupportsExtendedThinking(TEXT("claude-haiku-4-5-20251001")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ModelAcceptsSamplingParams — temperature/top_p/top_k
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_SamplingParams_Opus47False,
	"KismetAI.OpusModelCapabilities.SamplingParams.Opus47ReturnsFalse",
	OpusCapabilityTestFlags)
bool FOpusCaps_SamplingParams_Opus47False::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("Opus 4.7 does NOT accept sampling params"),
		FKismetAIAgent::ModelAcceptsSamplingParams(TEXT("claude-opus-4-7")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_SamplingParams_Opus46True,
	"KismetAI.OpusModelCapabilities.SamplingParams.Opus46ReturnsTrue",
	OpusCapabilityTestFlags)
bool FOpusCaps_SamplingParams_Opus46True::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Opus 4.6 accepts sampling params"),
		FKismetAIAgent::ModelAcceptsSamplingParams(TEXT("claude-opus-4-6")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_SamplingParams_Sonnet46True,
	"KismetAI.OpusModelCapabilities.SamplingParams.Sonnet46ReturnsTrue",
	OpusCapabilityTestFlags)
bool FOpusCaps_SamplingParams_Sonnet46True::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Sonnet 4.6 accepts sampling params"),
		FKismetAIAgent::ModelAcceptsSamplingParams(TEXT("claude-sonnet-4-6")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_SamplingParams_Haiku45True,
	"KismetAI.OpusModelCapabilities.SamplingParams.Haiku45ReturnsTrue",
	OpusCapabilityTestFlags)
bool FOpusCaps_SamplingParams_Haiku45True::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Haiku 4.5 accepts sampling params"),
		FKismetAIAgent::ModelAcceptsSamplingParams(TEXT("claude-haiku-4-5-20251001")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ModelNeedsLegacyThinkingBetas — interleaved-thinking-2025-05-14
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_LegacyBetas_Opus47False,
	"KismetAI.OpusModelCapabilities.LegacyThinkingBetas.Opus47ReturnsFalse",
	OpusCapabilityTestFlags)
bool FOpusCaps_LegacyBetas_Opus47False::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("Opus 4.7 does NOT need legacy thinking betas"),
		FKismetAIAgent::ModelNeedsLegacyThinkingBetas(TEXT("claude-opus-4-7")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_LegacyBetas_Opus46True,
	"KismetAI.OpusModelCapabilities.LegacyThinkingBetas.Opus46ReturnsTrue",
	OpusCapabilityTestFlags)
bool FOpusCaps_LegacyBetas_Opus46True::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Opus 4.6 needs legacy thinking betas"),
		FKismetAIAgent::ModelNeedsLegacyThinkingBetas(TEXT("claude-opus-4-6")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_LegacyBetas_Sonnet46True,
	"KismetAI.OpusModelCapabilities.LegacyThinkingBetas.Sonnet46ReturnsTrue",
	OpusCapabilityTestFlags)
bool FOpusCaps_LegacyBetas_Sonnet46True::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Sonnet 4.6 needs legacy thinking betas"),
		FKismetAIAgent::ModelNeedsLegacyThinkingBetas(TEXT("claude-sonnet-4-6")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_LegacyBetas_Haiku45True,
	"KismetAI.OpusModelCapabilities.LegacyThinkingBetas.Haiku45ReturnsTrue",
	OpusCapabilityTestFlags)
bool FOpusCaps_LegacyBetas_Haiku45True::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Haiku 4.5 needs legacy thinking betas"),
		FKismetAIAgent::ModelNeedsLegacyThinkingBetas(TEXT("claude-haiku-4-5-20251001")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ModelSupportsExtendedEffortLevels — xhigh/max
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_ExtendedEffort_Opus47True,
	"KismetAI.OpusModelCapabilities.ExtendedEffortLevels.Opus47ReturnsTrue",
	OpusCapabilityTestFlags)
bool FOpusCaps_ExtendedEffort_Opus47True::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Opus 4.7 supports extended effort levels"),
		FKismetAIAgent::ModelSupportsExtendedEffortLevels(TEXT("claude-opus-4-7")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_ExtendedEffort_Opus46False,
	"KismetAI.OpusModelCapabilities.ExtendedEffortLevels.Opus46ReturnsFalse",
	OpusCapabilityTestFlags)
bool FOpusCaps_ExtendedEffort_Opus46False::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("Opus 4.6 does NOT support extended effort levels"),
		FKismetAIAgent::ModelSupportsExtendedEffortLevels(TEXT("claude-opus-4-6")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_ExtendedEffort_Sonnet46False,
	"KismetAI.OpusModelCapabilities.ExtendedEffortLevels.Sonnet46ReturnsFalse",
	OpusCapabilityTestFlags)
bool FOpusCaps_ExtendedEffort_Sonnet46False::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("Sonnet 4.6 does NOT support extended effort levels"),
		FKismetAIAgent::ModelSupportsExtendedEffortLevels(TEXT("claude-sonnet-4-6")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_ExtendedEffort_Haiku45False,
	"KismetAI.OpusModelCapabilities.ExtendedEffortLevels.Haiku45ReturnsFalse",
	OpusCapabilityTestFlags)
bool FOpusCaps_ExtendedEffort_Haiku45False::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("Haiku 4.5 does NOT support extended effort levels"),
		FKismetAIAgent::ModelSupportsExtendedEffortLevels(TEXT("claude-haiku-4-5-20251001")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// EffortLevelToString / StringToEffortLevel round-trip — all five values
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_EffortRoundTrip_Low,
	"KismetAI.OpusModelCapabilities.EffortRoundTrip.Low",
	OpusCapabilityTestFlags)
bool FOpusCaps_EffortRoundTrip_Low::RunTest(const FString& Parameters)
{
	FString Str = FKismetAIAgent::EffortLevelToString(EEffortLevel::Low);
	TestEqual(TEXT("Low serializes to 'low'"), Str, FString(TEXT("low")));
	TOptional<EEffortLevel> Back = FKismetAIAgent::StringToEffortLevel(Str);
	TestTrue(TEXT("'low' deserializes"), Back.IsSet());
	TestTrue(TEXT("Low round-trips"), Back.IsSet() && Back.GetValue() == EEffortLevel::Low);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_EffortRoundTrip_Medium,
	"KismetAI.OpusModelCapabilities.EffortRoundTrip.Medium",
	OpusCapabilityTestFlags)
bool FOpusCaps_EffortRoundTrip_Medium::RunTest(const FString& Parameters)
{
	FString Str = FKismetAIAgent::EffortLevelToString(EEffortLevel::Medium);
	TestEqual(TEXT("Medium serializes to 'medium'"), Str, FString(TEXT("medium")));
	TOptional<EEffortLevel> Back = FKismetAIAgent::StringToEffortLevel(Str);
	TestTrue(TEXT("'medium' deserializes"), Back.IsSet());
	TestTrue(TEXT("Medium round-trips"), Back.IsSet() && Back.GetValue() == EEffortLevel::Medium);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_EffortRoundTrip_High,
	"KismetAI.OpusModelCapabilities.EffortRoundTrip.High",
	OpusCapabilityTestFlags)
bool FOpusCaps_EffortRoundTrip_High::RunTest(const FString& Parameters)
{
	FString Str = FKismetAIAgent::EffortLevelToString(EEffortLevel::High);
	TestEqual(TEXT("High serializes to 'high'"), Str, FString(TEXT("high")));
	TOptional<EEffortLevel> Back = FKismetAIAgent::StringToEffortLevel(Str);
	TestTrue(TEXT("'high' deserializes"), Back.IsSet());
	TestTrue(TEXT("High round-trips"), Back.IsSet() && Back.GetValue() == EEffortLevel::High);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_EffortRoundTrip_XHigh,
	"KismetAI.OpusModelCapabilities.EffortRoundTrip.XHigh",
	OpusCapabilityTestFlags)
bool FOpusCaps_EffortRoundTrip_XHigh::RunTest(const FString& Parameters)
{
	FString Str = FKismetAIAgent::EffortLevelToString(EEffortLevel::XHigh);
	TestEqual(TEXT("XHigh serializes to 'xhigh'"), Str, FString(TEXT("xhigh")));
	TOptional<EEffortLevel> Back = FKismetAIAgent::StringToEffortLevel(Str);
	TestTrue(TEXT("'xhigh' deserializes"), Back.IsSet());
	TestTrue(TEXT("XHigh round-trips"), Back.IsSet() && Back.GetValue() == EEffortLevel::XHigh);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_EffortRoundTrip_Max,
	"KismetAI.OpusModelCapabilities.EffortRoundTrip.Max",
	OpusCapabilityTestFlags)
bool FOpusCaps_EffortRoundTrip_Max::RunTest(const FString& Parameters)
{
	FString Str = FKismetAIAgent::EffortLevelToString(EEffortLevel::Max);
	TestEqual(TEXT("Max serializes to 'max'"), Str, FString(TEXT("max")));
	TOptional<EEffortLevel> Back = FKismetAIAgent::StringToEffortLevel(Str);
	TestTrue(TEXT("'max' deserializes"), Back.IsSet());
	TestTrue(TEXT("Max round-trips"), Back.IsSet() && Back.GetValue() == EEffortLevel::Max);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// GetEffortLevelDisplayName — XHigh and Max
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_DisplayName_XHigh,
	"KismetAI.OpusModelCapabilities.DisplayName.XHigh",
	OpusCapabilityTestFlags)
bool FOpusCaps_DisplayName_XHigh::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("XHigh display name"),
		FKismetAIAgent::GetEffortLevelDisplayName(EEffortLevel::XHigh),
		FString(TEXT("XHigh")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_DisplayName_Max,
	"KismetAI.OpusModelCapabilities.DisplayName.Max",
	OpusCapabilityTestFlags)
bool FOpusCaps_DisplayName_Max::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Max display name"),
		FKismetAIAgent::GetEffortLevelDisplayName(EEffortLevel::Max),
		FString(TEXT("Max")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Mythos 5 — adaptive-only thinking, native effort, no legacy betas.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_ExtendedThinking_Mythos5False,
	"KismetAI.OpusModelCapabilities.ExtendedThinking.Mythos5ReturnsFalse",
	OpusCapabilityTestFlags)
bool FOpusCaps_ExtendedThinking_Mythos5False::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("Mythos 5 does NOT support legacy extended thinking"),
		FKismetAIAgent::ModelSupportsExtendedThinking(TEXT("claude-mythos-5")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_AdaptiveThinking_Mythos5True,
	"KismetAI.OpusModelCapabilities.AdaptiveThinking.Mythos5ReturnsTrue",
	OpusCapabilityTestFlags)
bool FOpusCaps_AdaptiveThinking_Mythos5True::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Mythos 5 supports adaptive thinking"),
		FKismetAIAgent::ModelSupportsAdaptiveThinking(TEXT("claude-mythos-5")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_SamplingParams_Mythos5False,
	"KismetAI.OpusModelCapabilities.SamplingParams.Mythos5ReturnsFalse",
	OpusCapabilityTestFlags)
bool FOpusCaps_SamplingParams_Mythos5False::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("Mythos 5 does NOT accept sampling params"),
		FKismetAIAgent::ModelAcceptsSamplingParams(TEXT("claude-mythos-5")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_LegacyBetas_Mythos5False,
	"KismetAI.OpusModelCapabilities.LegacyThinkingBetas.Mythos5ReturnsFalse",
	OpusCapabilityTestFlags)
bool FOpusCaps_LegacyBetas_Mythos5False::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("Mythos 5 does NOT need legacy thinking betas"),
		FKismetAIAgent::ModelNeedsLegacyThinkingBetas(TEXT("claude-mythos-5")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_ExtendedEffort_Mythos5True,
	"KismetAI.OpusModelCapabilities.ExtendedEffortLevels.Mythos5ReturnsTrue",
	OpusCapabilityTestFlags)
bool FOpusCaps_ExtendedEffort_Mythos5True::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Mythos 5 supports extended effort levels"),
		FKismetAIAgent::ModelSupportsExtendedEffortLevels(TEXT("claude-mythos-5")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Opus 4.8 — same request surface as 4.7: adaptive-only thinking, no sampling
// params, no legacy betas, native extended effort levels.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_ExtendedThinking_Opus48False,
	"KismetAI.OpusModelCapabilities.ExtendedThinking.Opus48ReturnsFalse",
	OpusCapabilityTestFlags)
bool FOpusCaps_ExtendedThinking_Opus48False::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("Opus 4.8 does NOT support legacy extended thinking"),
		FKismetAIAgent::ModelSupportsExtendedThinking(TEXT("claude-opus-4-8")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_SamplingParams_Opus48False,
	"KismetAI.OpusModelCapabilities.SamplingParams.Opus48ReturnsFalse",
	OpusCapabilityTestFlags)
bool FOpusCaps_SamplingParams_Opus48False::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("Opus 4.8 does NOT accept sampling params"),
		FKismetAIAgent::ModelAcceptsSamplingParams(TEXT("claude-opus-4-8")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_LegacyBetas_Opus48False,
	"KismetAI.OpusModelCapabilities.LegacyThinkingBetas.Opus48ReturnsFalse",
	OpusCapabilityTestFlags)
bool FOpusCaps_LegacyBetas_Opus48False::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("Opus 4.8 does NOT need legacy thinking betas"),
		FKismetAIAgent::ModelNeedsLegacyThinkingBetas(TEXT("claude-opus-4-8")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_ExtendedEffort_Opus48True,
	"KismetAI.OpusModelCapabilities.ExtendedEffortLevels.Opus48ReturnsTrue",
	OpusCapabilityTestFlags)
bool FOpusCaps_ExtendedEffort_Opus48True::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Opus 4.8 supports extended effort levels"),
		FKismetAIAgent::ModelSupportsExtendedEffortLevels(TEXT("claude-opus-4-8")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ModelSupportsMidConversationSystem — Opus 4.8 only
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_MidConvoSystem_Opus48True,
	"KismetAI.OpusModelCapabilities.MidConversationSystem.Opus48ReturnsTrue",
	OpusCapabilityTestFlags)
bool FOpusCaps_MidConvoSystem_Opus48True::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Opus 4.8 supports mid-conversation system messages"),
		FKismetAIAgent::ModelSupportsMidConversationSystem(TEXT("claude-opus-4-8")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_MidConvoSystem_Opus47False,
	"KismetAI.OpusModelCapabilities.MidConversationSystem.Opus47ReturnsFalse",
	OpusCapabilityTestFlags)
bool FOpusCaps_MidConvoSystem_Opus47False::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("Opus 4.7 does NOT support mid-conversation system messages"),
		FKismetAIAgent::ModelSupportsMidConversationSystem(TEXT("claude-opus-4-7")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_MidConvoSystem_Sonnet46False,
	"KismetAI.OpusModelCapabilities.MidConversationSystem.Sonnet46ReturnsFalse",
	OpusCapabilityTestFlags)
bool FOpusCaps_MidConvoSystem_Sonnet46False::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("Sonnet 4.6 does NOT support mid-conversation system messages"),
		FKismetAIAgent::ModelSupportsMidConversationSystem(TEXT("claude-sonnet-4-6")));
	return true;
}

// MakeSystemMessage emits {"role":"system","content":"..."} — the API peer-turn shape.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_MakeSystemMessage_Shape,
	"KismetAI.OpusModelCapabilities.MidConversationSystem.MakeSystemMessageShape",
	OpusCapabilityTestFlags)
bool FOpusCaps_MakeSystemMessage_Shape::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonValue> Val = FKismetAIAgent::MakeSystemMessage(TEXT("Always include type annotations."));
	if (!TestTrue(TEXT("MakeSystemMessage returns a value"), Val.IsValid())) return false;

	TSharedPtr<FJsonObject> Obj = Val->AsObject();
	if (!TestTrue(TEXT("value is a JSON object"), Obj.IsValid())) return false;

	TestEqual(TEXT("role == system"), Obj->GetStringField(TEXT("role")), FString(TEXT("system")));
	TestEqual(TEXT("content carries the instruction text"),
		Obj->GetStringField(TEXT("content")), FString(TEXT("Always include type annotations.")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FModelAliasInfo struct has Tier and RecommendedEffort fields (compile check)
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpusCaps_ModelAliasInfo_HasTierField,
	"KismetAI.OpusModelCapabilities.ModelAliasInfo.HasTierAndRecommendedEffortFields",
	OpusCapabilityTestFlags)
bool FOpusCaps_ModelAliasInfo_HasTierField::RunTest(const FString& Parameters)
{
	FKismetAIAgent::FModelAliasInfo Info;
	Info.Alias = TEXT("test");
	Info.ModelId = TEXT("claude-test-1-0");
	Info.DisplayName = TEXT("Test 1.0");
	Info.ContextWindow = 200000;
	Info.Tier = TEXT("standard");
	Info.RecommendedEffort = TEXT("");

	TestEqual(TEXT("Tier field accessible"), Info.Tier, FString(TEXT("standard")));
	TestTrue(TEXT("RecommendedEffort field accessible"), Info.RecommendedEffort.IsEmpty());
	return true;
}
