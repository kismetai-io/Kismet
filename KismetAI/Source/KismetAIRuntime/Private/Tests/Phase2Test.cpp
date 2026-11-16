// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIAgent.h"
#include "KismetAISSEParser.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

constexpr auto Phase2TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// Improvement 6: Adaptive max_tokens — GetModelCost
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCost_Opus46Base, "KismetAI.Phase2.ModelCost.Opus46Base", Phase2TestFlags)
bool FCost_Opus46Base::RunTest(const FString& Parameters)
{
	FModelCost Cost = FKismetAIAgent::GetModelCost(TEXT("claude-opus-4-6"));
	TestEqual(TEXT("Opus 4.6 base input"), Cost.InputPerMillion, 5.0);
	TestEqual(TEXT("Opus 4.6 base output"), Cost.OutputPerMillion, 25.0);
	TestEqual(TEXT("Opus 4.6 base cache read"), Cost.CacheReadPerMillion, 0.5);
	TestEqual(TEXT("Opus 4.6 base cache write"), Cost.CacheWritePerMillion, 6.25);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCost_Sonnet4, "KismetAI.Phase2.ModelCost.Sonnet4", Phase2TestFlags)
bool FCost_Sonnet4::RunTest(const FString& Parameters)
{
	FModelCost Cost = FKismetAIAgent::GetModelCost(TEXT("claude-sonnet-4-6"));
	TestEqual(TEXT("Sonnet input"), Cost.InputPerMillion, 3.0);
	TestEqual(TEXT("Sonnet output"), Cost.OutputPerMillion, 15.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCost_Haiku, "KismetAI.Phase2.ModelCost.Haiku", Phase2TestFlags)
bool FCost_Haiku::RunTest(const FString& Parameters)
{
	FModelCost Cost = FKismetAIAgent::GetModelCost(TEXT("claude-haiku-4-5-20251001"));
	TestEqual(TEXT("Haiku input"), Cost.InputPerMillion, 1.0);
	TestEqual(TEXT("Haiku output"), Cost.OutputPerMillion, 5.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCost_Opus4PreV6, "KismetAI.Phase2.ModelCost.Opus4PreV6", Phase2TestFlags)
bool FCost_Opus4PreV6::RunTest(const FString& Parameters)
{
	FModelCost Cost = FKismetAIAgent::GetModelCost(TEXT("claude-opus-4-20250514"));
	TestEqual(TEXT("Opus 4 input"), Cost.InputPerMillion, 15.0);
	TestEqual(TEXT("Opus 4 output"), Cost.OutputPerMillion, 75.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCost_UnknownModel, "KismetAI.Phase2.ModelCost.UnknownModel", Phase2TestFlags)
bool FCost_UnknownModel::RunTest(const FString& Parameters)
{
	FModelCost Cost = FKismetAIAgent::GetModelCost(TEXT("some-future-model"));
	TestEqual(TEXT("Unknown defaults to Sonnet pricing"), Cost.InputPerMillion, 3.0);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Improvement 7: Extended Thinking — Model support detection
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThinking_ModelSupport, "KismetAI.Phase2.Thinking.ModelSupport", Phase2TestFlags)
bool FThinking_ModelSupport::RunTest(const FString& Parameters)
{
	// Claude 3.x should NOT support thinking
	TestFalse(TEXT("Claude 3 Opus no thinking"),
		FKismetAIAgent::ModelSupportsThinking(TEXT("claude-3-opus-20240229")));
	TestFalse(TEXT("Claude 3.5 Sonnet no thinking"),
		FKismetAIAgent::ModelSupportsThinking(TEXT("claude-3-5-sonnet-20241022")));

	// Claude 4+ should support thinking
	TestTrue(TEXT("Haiku 4.5 supports thinking"),
		FKismetAIAgent::ModelSupportsThinking(TEXT("claude-haiku-4-5-20251001")));
	TestTrue(TEXT("Sonnet 4 supports thinking"),
		FKismetAIAgent::ModelSupportsThinking(TEXT("claude-sonnet-4-20250514")));
	TestTrue(TEXT("Opus 4.6 supports thinking"),
		FKismetAIAgent::ModelSupportsThinking(TEXT("claude-opus-4-6")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThinking_AdaptiveSupport, "KismetAI.Phase2.Thinking.AdaptiveSupport", Phase2TestFlags)
bool FThinking_AdaptiveSupport::RunTest(const FString& Parameters)
{
	// Only 4.6 models support adaptive
	TestTrue(TEXT("Opus 4.6 adaptive"),
		FKismetAIAgent::ModelSupportsAdaptiveThinking(TEXT("claude-opus-4-6")));
	TestTrue(TEXT("Sonnet 4.6 adaptive"),
		FKismetAIAgent::ModelSupportsAdaptiveThinking(TEXT("claude-sonnet-4-6")));

	// Pre-4.6 should NOT support adaptive
	TestFalse(TEXT("Sonnet 4 no adaptive"),
		FKismetAIAgent::ModelSupportsAdaptiveThinking(TEXT("claude-sonnet-4-20250514")));
	TestFalse(TEXT("Haiku 4.5 no adaptive"),
		FKismetAIAgent::ModelSupportsAdaptiveThinking(TEXT("claude-haiku-4-5-20251001")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Improvement 7: Extended Thinking — FilterThinkingFromHistory
// ─────────────────────────────────────────────────────────────────────────────

// Helper to build a content block
static TSharedPtr<FJsonValue> MakeBlock(const FString& Type, const FString& Key, const FString& Value)
{
	TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
	Block->SetStringField(TEXT("type"), Type);
	Block->SetStringField(Key, Value);
	return MakeShared<FJsonValueObject>(Block);
}

static TSharedPtr<FJsonValue> MakeMessage(const FString& Role, const TArray<TSharedPtr<FJsonValue>>& Content)
{
	TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("role"), Role);
	Msg->SetArrayField(TEXT("content"), Content);
	return MakeShared<FJsonValueObject>(Msg);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThinking_FilterTrailing, "KismetAI.Phase2.Thinking.FilterTrailingThinking", Phase2TestFlags)
bool FThinking_FilterTrailing::RunTest(const FString& Parameters)
{
	// Build: assistant message with [text, thinking] — trailing thinking should be stripped
	TArray<TSharedPtr<FJsonValue>> Messages;

	TArray<TSharedPtr<FJsonValue>> Content;
	Content.Add(MakeBlock(TEXT("text"), TEXT("text"), TEXT("Hello")));
	Content.Add(MakeBlock(TEXT("thinking"), TEXT("thinking"), TEXT("Let me think...")));
	Messages.Add(MakeMessage(TEXT("assistant"), Content));

	FKismetAIAgent::FilterThinkingFromHistory(Messages);

	// Should have 1 message with 1 block (text only)
	TestEqual(TEXT("Still 1 message"), Messages.Num(), 1);
	TSharedPtr<FJsonObject> Msg = Messages[0]->AsObject();
	const TArray<TSharedPtr<FJsonValue>>* ContentArray;
	TestTrue(TEXT("Has content"), Msg->TryGetArrayField(TEXT("content"), ContentArray));
	TestEqual(TEXT("1 block after filter"), ContentArray->Num(), 1);
	TestEqual(TEXT("Remaining block is text"),
		(*ContentArray)[0]->AsObject()->GetStringField(TEXT("type")), TEXT("text"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThinking_FilterAllThinking, "KismetAI.Phase2.Thinking.FilterAllThinkingPlaceholder", Phase2TestFlags)
bool FThinking_FilterAllThinking::RunTest(const FString& Parameters)
{
	// Assistant message with ONLY thinking blocks — should get placeholder
	TArray<TSharedPtr<FJsonValue>> Messages;

	TArray<TSharedPtr<FJsonValue>> Content;
	Content.Add(MakeBlock(TEXT("thinking"), TEXT("thinking"), TEXT("Reasoning...")));
	Content.Add(MakeBlock(TEXT("redacted_thinking"), TEXT("data"), TEXT("...")));
	Messages.Add(MakeMessage(TEXT("assistant"), Content));

	// Add a user message after so the assistant isn't the only message
	TArray<TSharedPtr<FJsonValue>> UserContent;
	UserContent.Add(MakeBlock(TEXT("text"), TEXT("text"), TEXT("user input")));
	Messages.Add(MakeMessage(TEXT("user"), UserContent));

	FKismetAIAgent::FilterThinkingFromHistory(Messages);

	// The all-thinking assistant message should be removed entirely (Rule 2)
	TestEqual(TEXT("Thinking-only message removed"), Messages.Num(), 1);
	TestEqual(TEXT("Remaining is user"),
		Messages[0]->AsObject()->GetStringField(TEXT("role")), TEXT("user"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThinking_FilterNoThinking, "KismetAI.Phase2.Thinking.FilterNoThinkingNoop", Phase2TestFlags)
bool FThinking_FilterNoThinking::RunTest(const FString& Parameters)
{
	// No thinking blocks — should be a no-op
	TArray<TSharedPtr<FJsonValue>> Messages;

	TArray<TSharedPtr<FJsonValue>> Content;
	Content.Add(MakeBlock(TEXT("text"), TEXT("text"), TEXT("Hello")));
	Content.Add(MakeBlock(TEXT("tool_use"), TEXT("name"), TEXT("read_blueprint")));
	Messages.Add(MakeMessage(TEXT("assistant"), Content));

	FKismetAIAgent::FilterThinkingFromHistory(Messages);

	TestEqual(TEXT("Still 1 message"), Messages.Num(), 1);
	const TArray<TSharedPtr<FJsonValue>>* ContentArray;
	Messages[0]->AsObject()->TryGetArrayField(TEXT("content"), ContentArray);
	TestEqual(TEXT("Still 2 blocks"), ContentArray->Num(), 2);

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Improvement 7: SSE Parser — thinking_delta and signature_delta
// ─────────────────────────────────────────────────────────────────────────────

static FKismetAISSEEvent MakeSSEEvent(const FString& EventType, const FString& JsonStr)
{
	FKismetAISSEEvent Event;
	Event.EventType = EventType;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
	FJsonSerializer::Deserialize(Reader, Event.Data);
	return Event;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSSE_ThinkingDelta, "KismetAI.Phase2.SSE.ThinkingDelta", Phase2TestFlags)
bool FSSE_ThinkingDelta::RunTest(const FString& Parameters)
{
	FAnthropicStreamState State;

	FString ThinkingAccum;
	State.OnThinkingDelta = [&ThinkingAccum](const FString& Text)
	{
		ThinkingAccum += Text;
	};

	FString DeltaText;

	// message_start
	State.ProcessEvent(MakeSSEEvent(TEXT("message_start"),
		TEXT("{\"message\":{\"usage\":{\"input_tokens\":100}}}")), DeltaText);

	// content_block_start (thinking)
	State.ProcessEvent(MakeSSEEvent(TEXT("content_block_start"),
		TEXT("{\"index\":0,\"content_block\":{\"type\":\"thinking\"}}")), DeltaText);

	// thinking_delta
	bool bResult1 = State.ProcessEvent(MakeSSEEvent(TEXT("content_block_delta"),
		TEXT("{\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"Let me \"}}")), DeltaText);
	TestFalse(TEXT("thinking_delta is not text_delta"), bResult1);

	bool bResult2 = State.ProcessEvent(MakeSSEEvent(TEXT("content_block_delta"),
		TEXT("{\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"think...\"}}")), DeltaText);
	TestFalse(TEXT("thinking_delta is not text_delta"), bResult2);

	// signature_delta
	State.ProcessEvent(MakeSSEEvent(TEXT("content_block_delta"),
		TEXT("{\"delta\":{\"type\":\"signature_delta\",\"signature\":\"abc123\"}}")), DeltaText);

	// content_block_stop
	State.ProcessEvent(MakeSSEEvent(TEXT("content_block_stop"), TEXT("{}")), DeltaText);

	// Verify thinking callback fired
	TestEqual(TEXT("Thinking callback accumulated"), ThinkingAccum, TEXT("Let me think..."));

	// Verify built response contains thinking block with signature
	TSharedPtr<FJsonObject> Response = State.BuildResponseJson();
	const TArray<TSharedPtr<FJsonValue>>* ContentArray;
	TestTrue(TEXT("Has content"), Response->TryGetArrayField(TEXT("content"), ContentArray));
	TestEqual(TEXT("1 content block"), ContentArray->Num(), 1);

	TSharedPtr<FJsonObject> Block = (*ContentArray)[0]->AsObject();
	TestEqual(TEXT("Block type is thinking"), Block->GetStringField(TEXT("type")), TEXT("thinking"));
	TestEqual(TEXT("Thinking text"), Block->GetStringField(TEXT("thinking")), TEXT("Let me think..."));
	TestEqual(TEXT("Signature"), Block->GetStringField(TEXT("signature")), TEXT("abc123"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSSE_MixedThinkingAndText, "KismetAI.Phase2.SSE.MixedThinkingAndText", Phase2TestFlags)
bool FSSE_MixedThinkingAndText::RunTest(const FString& Parameters)
{
	FAnthropicStreamState State;
	FString DeltaText;

	// message_start
	State.ProcessEvent(MakeSSEEvent(TEXT("message_start"),
		TEXT("{\"message\":{\"usage\":{\"input_tokens\":50}}}")), DeltaText);

	// thinking block
	State.ProcessEvent(MakeSSEEvent(TEXT("content_block_start"),
		TEXT("{\"index\":0,\"content_block\":{\"type\":\"thinking\"}}")), DeltaText);
	State.ProcessEvent(MakeSSEEvent(TEXT("content_block_delta"),
		TEXT("{\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"Planning...\"}}")), DeltaText);
	State.ProcessEvent(MakeSSEEvent(TEXT("content_block_stop"), TEXT("{}")), DeltaText);

	// text block
	State.ProcessEvent(MakeSSEEvent(TEXT("content_block_start"),
		TEXT("{\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}")), DeltaText);
	bool bIsText = State.ProcessEvent(MakeSSEEvent(TEXT("content_block_delta"),
		TEXT("{\"delta\":{\"type\":\"text_delta\",\"text\":\"I will read the blueprint.\"}}")), DeltaText);
	TestTrue(TEXT("text_delta returns true"), bIsText);
	TestEqual(TEXT("Delta text correct"), DeltaText, TEXT("I will read the blueprint."));
	State.ProcessEvent(MakeSSEEvent(TEXT("content_block_stop"), TEXT("{}")), DeltaText);

	// message_stop
	State.ProcessEvent(MakeSSEEvent(TEXT("message_stop"), TEXT("{}")), DeltaText);
	TestTrue(TEXT("Stream complete"), State.bComplete);

	// Verify 2 blocks in response
	TSharedPtr<FJsonObject> Response = State.BuildResponseJson();
	const TArray<TSharedPtr<FJsonValue>>* ContentArray;
	Response->TryGetArrayField(TEXT("content"), ContentArray);
	TestEqual(TEXT("2 content blocks"), ContentArray->Num(), 2);
	TestEqual(TEXT("First is thinking"),
		(*ContentArray)[0]->AsObject()->GetStringField(TEXT("type")), TEXT("thinking"));
	TestEqual(TEXT("Second is text"),
		(*ContentArray)[1]->AsObject()->GetStringField(TEXT("type")), TEXT("text"));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Improvement 10: Token stats struct
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTokenStats_CostCalc, "KismetAI.Phase2.TokenStats.CostCalculation", Phase2TestFlags)
bool FTokenStats_CostCalc::RunTest(const FString& Parameters)
{
	FModelCost Cost = FKismetAIAgent::GetModelCost(TEXT("claude-sonnet-4-6"));

	// Simulate 10K input, 1K output, 5K cache read, 2K cache write
	double EstimatedCost =
		(10000.0 / 1000000.0) * Cost.InputPerMillion +
		(1000.0 / 1000000.0) * Cost.OutputPerMillion +
		(5000.0 / 1000000.0) * Cost.CacheReadPerMillion +
		(2000.0 / 1000000.0) * Cost.CacheWritePerMillion;

	// Sonnet: 3/15/0.3/3.75 per million
	// Expected: 10K*3/1M + 1K*15/1M + 5K*0.3/1M + 2K*3.75/1M
	//         = 0.03 + 0.015 + 0.0015 + 0.0075 = 0.054
	double Expected = 0.054;
	TestTrue(TEXT("Cost calculation within tolerance"),
		FMath::Abs(EstimatedCost - Expected) < 0.0001);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTokenStats_ContextInput, "KismetAI.Phase2.TokenStats.ContextInput", Phase2TestFlags)
bool FTokenStats_ContextInput::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Anthropic cache tokens occupy context"),
		FKismetAIAgent::ComputeContextInputTokens(TEXT("anthropic"), 600, 50000, 3000),
		53600);

	TestEqual(TEXT("OpenAI cached_tokens are already included in input_tokens"),
		FKismetAIAgent::ComputeContextInputTokens(TEXT("openai"), 100, 20, 0),
		100);

	TestEqual(TEXT("Unknown providers use Anthropic-style separate cache fields"),
		FKismetAIAgent::ComputeContextInputTokens(TEXT(""), 100, 20, 5),
		125);

	return true;
}
