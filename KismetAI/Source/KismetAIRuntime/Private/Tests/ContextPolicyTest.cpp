// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Context policy decisions, tested without a model or a transcript.
//
// These thresholds used to be inline in RunTask, reachable only by driving a
// real conversation past 90% of a 200k-token window. That is why they had no
// tests. As pure functions they are trivially checkable at the boundaries,
// which is where compaction bugs actually live.

#include "KismetAITurnContext.h"
#include "Misc/AutomationTest.h"

constexpr auto CtxPolicyFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCtxPolicy_CompactionThreshold, "KismetAI.ContextPolicy.CompactionThreshold", CtxPolicyFlags)
bool FCtxPolicy_CompactionThreshold::RunTest(const FString& Parameters)
{
	const int32 Limit = 100000;
	const float Threshold = 0.90f;
	auto Decide = [&](int32 Tokens)
	{
		return FKismetAIContextPolicy::DecideCompaction(Tokens, Limit, Threshold);
	};

	TestTrue(TEXT("well under the limit does nothing"), Decide(50000) == ECompactionAction::None);
	TestTrue(TEXT("just under the threshold does nothing"), Decide(90000) == ECompactionAction::None);
	TestTrue(TEXT("just over the threshold compacts"), Decide(90001) == ECompactionAction::Microcompact);
	TestTrue(TEXT("far over compacts"), Decide(99999) == ECompactionAction::Microcompact);

	// An unknown context limit must not trigger compaction on every turn.
	TestTrue(TEXT("a zero limit is not treated as exceeded"),
		FKismetAIContextPolicy::DecideCompaction(50000, 0, Threshold) == ECompactionAction::None);
	return true;
}

// The second phase is deliberately stricter: a full trim loses conversation, so
// it only runs when microcompaction demonstrably was not enough.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCtxPolicy_TrimIsSecondPhase, "KismetAI.ContextPolicy.TrimIsSecondPhase", CtxPolicyFlags)
bool FCtxPolicy_TrimIsSecondPhase::RunTest(const FString& Parameters)
{
	const int32 Limit = 100000;

	TestFalse(TEXT("at the compaction threshold, no trim yet"),
		FKismetAIContextPolicy::NeedsTrimAfterMicrocompact(91000, Limit));
	TestFalse(TEXT("just under 95% still no trim"),
		FKismetAIContextPolicy::NeedsTrimAfterMicrocompact(95000, Limit));
	TestTrue(TEXT("past 95% trims"),
		FKismetAIContextPolicy::NeedsTrimAfterMicrocompact(95001, Limit));

	// Trim is strictly rarer than compaction — otherwise the two phases collapse.
	int32 CompactCount = 0, TrimCount = 0;
	for (int32 T = 0; T <= Limit; T += 1000)
	{
		if (FKismetAIContextPolicy::DecideCompaction(T, Limit, 0.90f) != ECompactionAction::None) { ++CompactCount; }
		if (FKismetAIContextPolicy::NeedsTrimAfterMicrocompact(T, Limit)) { ++TrimCount; }
	}
	TestTrue(TEXT("trim fires strictly less often than compaction"), TrimCount < CompactCount);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCtxPolicy_NextTurnBudget, "KismetAI.ContextPolicy.NextTurnBudget", CtxPolicyFlags)
bool FCtxPolicy_NextTurnBudget::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig C;
	auto Decide = [&C](const TArray<FString>& Tools, bool bFirstNoTools = false, bool bThinking = false)
	{
		return FKismetAIContextPolicy::DecideNextMaxTokens(Tools, bFirstNoTools, bThinking, C);
	};

	TestEqual(TEXT("wrote T3D -> next turn verifies, small budget"),
		Decide({ TEXT("write_blueprint") }), C.MaxTokensLow);
	TestEqual(TEXT("read -> next turn likely writes, large budget"),
		Decide({ TEXT("read_blueprint") }), C.MaxTokensHigh);
	TestEqual(TEXT("compiled -> large budget"),
		Decide({ TEXT("compile_blueprint") }), C.MaxTokensHigh);
	TestEqual(TEXT("structural change -> large budget"),
		Decide({ TEXT("manage_blueprint") }), C.MaxTokensHigh);
	TestEqual(TEXT("edit alone -> default"),
		Decide({ TEXT("edit_blueprint") }), C.MaxTokensDefault);
	TestEqual(TEXT("planning turn with no tools -> small"),
		Decide({}, /*bFirstNoTools=*/true), C.MaxTokensLow);

	// A turn that both read and wrote is followed by verification, so Low wins.
	TestEqual(TEXT("mixed read+write -> Low, matching the pre-extraction order"),
		Decide({ TEXT("read_blueprint"), TEXT("write_blueprint") }), C.MaxTokensLow);
	return true;
}

// Reasoning tokens are billed against the output cap, so the tool heuristic
// must never starve a thinking turn into truncating before its tool_use.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCtxPolicy_ThinkingFloorWins, "KismetAI.ContextPolicy.ThinkingFloorWins", CtxPolicyFlags)
bool FCtxPolicy_ThinkingFloorWins::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig C;
	TestTrue(TEXT("the floor is above the low budget, or this test proves nothing"),
		C.MaxTokensThinkingFloor > C.MaxTokensLow);

	const int32 WithThinking = FKismetAIContextPolicy::DecideNextMaxTokens(
		{ TEXT("write_blueprint") }, false, /*bThinking=*/true, C);
	TestEqual(TEXT("a Low hint is raised to the thinking floor"), WithThinking, C.MaxTokensThinkingFloor);

	const int32 Without = FKismetAIContextPolicy::DecideNextMaxTokens(
		{ TEXT("write_blueprint") }, false, /*bThinking=*/false, C);
	TestEqual(TEXT("without thinking the Low hint stands"), Without, C.MaxTokensLow);
	return true;
}

// PrepareHistory must repair pairing AFTER stripping thinking blocks: stripping
// can leave an assistant message that no longer pairs with what follows it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCtxPolicy_PrepareHistoryOrder, "KismetAI.ContextPolicy.PrepareHistoryOrder", CtxPolicyFlags)
bool FCtxPolicy_PrepareHistoryOrder::RunTest(const FString& Parameters)
{
	// An assistant turn with a tool_use and no matching tool_result: pairing
	// repair must answer it, or the provider rejects the request.
	auto MakeMsg = [](const TCHAR* Role, TSharedPtr<FJsonObject> Block)
	{
		TSharedPtr<FJsonObject> M = MakeShared<FJsonObject>();
		M->SetStringField(TEXT("role"), Role);
		TArray<TSharedPtr<FJsonValue>> Content;
		Content.Add(MakeShared<FJsonValueObject>(Block));
		M->SetArrayField(TEXT("content"), Content);
		return MakeShared<FJsonValueObject>(M);
	};

	TSharedPtr<FJsonObject> Text = MakeShared<FJsonObject>();
	Text->SetStringField(TEXT("type"), TEXT("text"));
	Text->SetStringField(TEXT("text"), TEXT("hello"));

	TSharedPtr<FJsonObject> Orphan = MakeShared<FJsonObject>();
	Orphan->SetStringField(TEXT("type"), TEXT("tool_use"));
	Orphan->SetStringField(TEXT("id"), TEXT("t1"));
	Orphan->SetStringField(TEXT("name"), TEXT("read_blueprint"));
	Orphan->SetObjectField(TEXT("input"), MakeShared<FJsonObject>());

	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(MakeMsg(TEXT("user"), Text));
	Messages.Add(MakeMsg(TEXT("assistant"), Orphan));   // no tool_result follows

	FKismetAIContextPolicy::PrepareHistory(Messages, /*bThinkingEnabled=*/true);
	// The unanswered call is kept and answered honestly (an error saying
	// its outcome is unknown), rather than silently dropped.
	TestEqual(TEXT("the trailing tool_use turn is kept and answered"), Messages.Num(), 3);
	if (Messages.Num() == 3)
	{
		const TArray<TSharedPtr<FJsonValue>>& Answer = Messages[2]->AsObject()->GetArrayField(TEXT("content"));
		const TSharedPtr<FJsonObject> Result = Answer.Num() == 1 ? Answer[0]->AsObject() : nullptr;
		TestTrue(TEXT("answered by a tool_result for t1"), Result.IsValid()
			&& Result->GetStringField(TEXT("type")) == TEXT("tool_result")
			&& Result->GetStringField(TEXT("tool_use_id")) == TEXT("t1"));
		TestTrue(TEXT("which is an error, not a success"), Result.IsValid() && Result->GetBoolField(TEXT("is_error"))
			&& Result->GetStringField(TEXT("content")).Contains(TEXT("OUTCOME_UNRECORDED")));
	}

	// And the ordinary case is untouched: a conversation ending in assistant text
	// has nothing to repair.
	TArray<TSharedPtr<FJsonValue>> Normal;
	Normal.Add(MakeMsg(TEXT("user"), Text));
	Normal.Add(MakeMsg(TEXT("assistant"), Text));
	FKismetAIContextPolicy::PrepareHistory(Normal, /*bThinkingEnabled=*/true);
	TestEqual(TEXT("a normal trailing assistant message survives"), Normal.Num(), 2);
	return true;
}
