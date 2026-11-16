// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIAgent.h"
#include "Fakes/ScriptedAgentSeams.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

constexpr auto ContextTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// Helper: create a minimal agent with no compact prompt loaded (no summary can
// be produced, so a TrimConversation attempt fails and changes nothing).
static FKismetAIAgent CreateTestAgent()
{
	FKismetAIAgentConfig Config;
	return FKismetAIAgent(MakeUnique<KismetAITestFakes::FScriptedToolInvoker>(), Config);
}

// --- GetModelContextLimit ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContext_ModelLimit_KnownModel, "KismetAI.Context.ModelLimit.KnownModel", ContextTestFlags)
bool FContext_ModelLimit_KnownModel::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Haiku 4.5 has 200K"),
		FKismetAIAgent::GetModelContextLimit(TEXT("claude-haiku-4-5-20251001")), 200000);
	TestEqual(TEXT("Sonnet 4 has 200K"),
		FKismetAIAgent::GetModelContextLimit(TEXT("claude-sonnet-4-20250514")), 200000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContext_ModelLimit_1MModel, "KismetAI.Context.ModelLimit.1MModel", ContextTestFlags)
bool FContext_ModelLimit_1MModel::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Sonnet 4 suffix does not override catalog capacity"),
		FKismetAIAgent::GetModelContextLimit(TEXT("claude-sonnet-4-20250514-1m")), 200000);
	TestEqual(TEXT("Sonnet 4.6 retains documented 1M capacity"),
		FKismetAIAgent::GetModelContextLimit(TEXT("claude-sonnet-4-6-1m")), 1000000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContext_ModelLimit_Unknown, "KismetAI.Context.ModelLimit.Unknown", ContextTestFlags)
bool FContext_ModelLimit_Unknown::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Unknown model defaults to 200K"),
		FKismetAIAgent::GetModelContextLimit(TEXT("some-future-model-v9")), 200000);
	return true;
}

// --- TrimConversation ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContext_Trim_Basic, "KismetAI.Context.TrimConversation.Basic", ContextTestFlags)
bool FContext_Trim_Basic::RunTest(const FString& Parameters)
{
	// Build a conversation with 6 messages
	TArray<TSharedPtr<FJsonValue>> Messages;

	// Message 0: original user prompt
	auto Msg0 = MakeShared<FJsonObject>();
	Msg0->SetStringField(TEXT("role"), TEXT("user"));
	Msg0->SetStringField(TEXT("content"), TEXT("Add PrintString to BP_Test"));
	Messages.Add(MakeShared<FJsonValueObject>(Msg0));

	// Messages 1-3: assistant + tool results (middle turns to be trimmed)
	for (int32 i = 1; i <= 3; i++)
	{
		auto Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), (i % 2 == 1) ? TEXT("assistant") : TEXT("user"));
		Msg->SetStringField(TEXT("content"), FString::Printf(TEXT("Turn %d content"), i));
		Messages.Add(MakeShared<FJsonValueObject>(Msg));
	}

	// Messages 4-5: most recent assistant + tool result
	auto Msg4 = MakeShared<FJsonObject>();
	Msg4->SetStringField(TEXT("role"), TEXT("assistant"));
	Msg4->SetStringField(TEXT("content"), TEXT("Most recent assistant response"));
	Messages.Add(MakeShared<FJsonValueObject>(Msg4));

	auto Msg5 = MakeShared<FJsonObject>();
	Msg5->SetStringField(TEXT("role"), TEXT("user"));
	Msg5->SetStringField(TEXT("content"), TEXT("Most recent tool result"));
	Messages.Add(MakeShared<FJsonValueObject>(Msg5));

	// A trim needs a real summary. (It used to fall back to a generic
	// marker with no compact prompt or API key, and drop the middle anyway.)
	KismetAITestFakes::FScriptedModelClient Model({ KismetAITestFakes::MakeTextTurn(TEXT("<summary>Earlier turns.</summary>")) });
	KismetAITestFakes::FScriptedToolInvoker Tools;
	FKismetAIAgentConfig Config;
	FKismetAIAgent Agent(Tools, Model, Config);
	Agent.SetCompactPromptForTests(TEXT("Summarize."));
	const FKismetAICompactionResult Result = Agent.TrimConversation(Messages);

	TestTrue(TEXT("Compaction committed"), Result.IsCommitted());
	TestEqual(TEXT("One round dropped"), Result.DroppedGroups, 1);

	// First message is still the original user prompt
	TSharedPtr<FJsonObject> First = Messages[0]->AsObject();
	TestEqual(TEXT("First message is original prompt"),
		First->GetStringField(TEXT("content")), TEXT("Add PrintString to BP_Test"));

	// Last message is the most recent tool result
	TSharedPtr<FJsonObject> Last = Messages.Last()->AsObject();
	TestEqual(TEXT("Last message preserved"),
		Last->GetStringField(TEXT("content")), TEXT("Most recent tool result"));

	// The summary replaces the dropped round...
	TSharedPtr<FJsonObject> Summary = Messages[1]->AsObject();
	TestTrue(TEXT("Summary message is marked as compaction"),
		Summary->GetStringField(TEXT("content")).Contains(TEXT("[Context compaction]")));
	TestTrue(TEXT("Summary text is included"),
		Summary->GetStringField(TEXT("content")).Contains(TEXT("Earlier turns.")));

	// ...but the user's message from that round ("Turn 2", string content, so a
	// genuine user message) is kept verbatim; the assistant turn is not.
	TestEqual(TEXT("User message from the dropped round kept verbatim"),
		Messages[2]->AsObject()->GetStringField(TEXT("content")), FString(TEXT("Turn 2 content")));
	bool bAssistantTurnGone = true;
	for (const TSharedPtr<FJsonValue>& M : Messages)
	{
		FString Content;
		if (M->AsObject()->TryGetStringField(TEXT("content"), Content) && Content == TEXT("Turn 1 content"))
		{
			bAssistantTurnGone = false;
		}
	}
	TestTrue(TEXT("Dropped assistant turn is gone"), bAssistantTurnGone);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContext_Trim_TooShort, "KismetAI.Context.TrimConversation.TooShort", ContextTestFlags)
bool FContext_Trim_TooShort::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FJsonValue>> Messages;
	auto Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("role"), TEXT("user"));
	Msg->SetStringField(TEXT("content"), TEXT("Hello"));
	Messages.Add(MakeShared<FJsonValueObject>(Msg));

	int32 Before = Messages.Num();
	FKismetAIAgent Agent = CreateTestAgent();
	Agent.TrimConversation(Messages);
	TestEqual(TEXT("No trimming for short conversation"), Messages.Num(), Before);
	return true;
}

// Test: TrimConversation preserves tool_use/tool_result pairing
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContext_Trim_PreservesToolPairing, "KismetAI.Context.TrimConversation.PreservesToolPairing", ContextTestFlags)
bool FContext_Trim_PreservesToolPairing::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FJsonValue>> Messages;

	// Message 0: user prompt
	auto Msg0 = MakeShared<FJsonObject>();
	Msg0->SetStringField(TEXT("role"), TEXT("user"));
	Msg0->SetStringField(TEXT("content"), TEXT("Add PrintString"));
	Messages.Add(MakeShared<FJsonValueObject>(Msg0));

	// Build 4 complete API rounds (assistant with tool_use + user with tool_result)
	for (int32 Round = 0; Round < 4; Round++)
	{
		// Assistant message with tool_use block
		auto AssistantMsg = MakeShared<FJsonObject>();
		AssistantMsg->SetStringField(TEXT("role"), TEXT("assistant"));
		TArray<TSharedPtr<FJsonValue>> AssistantContent;
		auto ToolUseBlock = MakeShared<FJsonObject>();
		ToolUseBlock->SetStringField(TEXT("type"), TEXT("tool_use"));
		ToolUseBlock->SetStringField(TEXT("id"), FString::Printf(TEXT("toolu_%d"), Round));
		ToolUseBlock->SetStringField(TEXT("name"), TEXT("read_blueprint"));
		AssistantContent.Add(MakeShared<FJsonValueObject>(ToolUseBlock));
		AssistantMsg->SetArrayField(TEXT("content"), AssistantContent);
		Messages.Add(MakeShared<FJsonValueObject>(AssistantMsg));

		// User message with matching tool_result
		auto UserMsg = MakeShared<FJsonObject>();
		UserMsg->SetStringField(TEXT("role"), TEXT("user"));
		TArray<TSharedPtr<FJsonValue>> UserContent;
		auto ToolResultBlock = MakeShared<FJsonObject>();
		ToolResultBlock->SetStringField(TEXT("type"), TEXT("tool_result"));
		ToolResultBlock->SetStringField(TEXT("tool_use_id"), FString::Printf(TEXT("toolu_%d"), Round));
		ToolResultBlock->SetStringField(TEXT("content"), TEXT("result data"));
		UserContent.Add(MakeShared<FJsonValueObject>(ToolResultBlock));
		UserMsg->SetArrayField(TEXT("content"), UserContent);
		Messages.Add(MakeShared<FJsonValueObject>(UserMsg));
	}

	KismetAITestFakes::FScriptedModelClient Model({ KismetAITestFakes::MakeTextTurn(TEXT("<summary>Read twice.</summary>")) });
	KismetAITestFakes::FScriptedToolInvoker Tools;
	FKismetAIAgentConfig Config;
	FKismetAIAgent Agent(Tools, Model, Config);
	Agent.SetCompactPromptForTests(TEXT("Summarize."));
	const int32 Before = Messages.Num();
	TestTrue(TEXT("Compaction committed"), Agent.TrimConversation(Messages).IsCommitted());
	TestTrue(TEXT("History shrank"), Messages.Num() < Before);

	// Verify every tool_use has a matching tool_result in the next message
	for (int32 i = 0; i < Messages.Num(); i++)
	{
		TSharedPtr<FJsonObject> MsgObj = Messages[i]->AsObject();
		if (!MsgObj.IsValid()) continue;
		const TArray<TSharedPtr<FJsonValue>>* ContentArray;
		if (!MsgObj->TryGetArrayField(TEXT("content"), ContentArray)) continue;

		for (const auto& Block : *ContentArray)
		{
			TSharedPtr<FJsonObject> BlockObj = Block->AsObject();
			if (!BlockObj.IsValid()) continue;
			if (BlockObj->GetStringField(TEXT("type")) != TEXT("tool_use")) continue;

			FString ToolUseId = BlockObj->GetStringField(TEXT("id"));
			TestTrue(FString::Printf(TEXT("Has next message after tool_use %s"), *ToolUseId),
				i + 1 < Messages.Num());
			if (i + 1 < Messages.Num())
			{
				TSharedPtr<FJsonObject> NextMsg = Messages[i + 1]->AsObject();
				const TArray<TSharedPtr<FJsonValue>>* NextContentArray;
				bool bFoundMatch = false;
				if (NextMsg->TryGetArrayField(TEXT("content"), NextContentArray))
				{
					for (const auto& NextBlock : *NextContentArray)
					{
						TSharedPtr<FJsonObject> NB = NextBlock->AsObject();
						if (NB.IsValid() && NB->GetStringField(TEXT("type")) == TEXT("tool_result")
							&& NB->GetStringField(TEXT("tool_use_id")) == ToolUseId)
						{
							bFoundMatch = true;
							break;
						}
					}
				}
				TestTrue(FString::Printf(TEXT("tool_result matches tool_use %s"), *ToolUseId), bFoundMatch);
			}
		}
	}
	return true;
}

// Test: Microcompaction clears old tool results but keeps recent ones
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContext_Microcompact_Basic, "KismetAI.Context.Microcompact.Basic", ContextTestFlags)
bool FContext_Microcompact_Basic::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FJsonValue>> Messages;

	// Build 4 user messages each containing a tool_result
	for (int32 i = 0; i < 4; i++)
	{
		auto Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), TEXT("user"));
		TArray<TSharedPtr<FJsonValue>> Content;
		auto Block = MakeShared<FJsonObject>();
		Block->SetStringField(TEXT("type"), TEXT("tool_result"));
		Block->SetStringField(TEXT("tool_use_id"), FString::Printf(TEXT("toolu_%d"), i));
		Block->SetStringField(TEXT("content"), FString::Printf(TEXT("Large result data %d with lots of content"), i));
		Content.Add(MakeShared<FJsonValueObject>(Block));
		Msg->SetArrayField(TEXT("content"), Content);
		Messages.Add(MakeShared<FJsonValueObject>(Msg));
	}

	FKismetAIAgent::MicrocompactToolResults(Messages, 2);

	// First 2 tool results should be cleared
	for (int32 i = 0; i < 2; i++)
	{
		TSharedPtr<FJsonObject> MsgObj = Messages[i]->AsObject();
		const TArray<TSharedPtr<FJsonValue>>* ContentArray;
		MsgObj->TryGetArrayField(TEXT("content"), ContentArray);
		TSharedPtr<FJsonObject> Block = (*ContentArray)[0]->AsObject();
		TestTrue(FString::Printf(TEXT("Result %d is cleared"), i),
			Block->GetStringField(TEXT("content")).Contains(TEXT("[tool result cleared")));
	}

	// Last 2 should be intact
	for (int32 i = 2; i < 4; i++)
	{
		TSharedPtr<FJsonObject> MsgObj = Messages[i]->AsObject();
		const TArray<TSharedPtr<FJsonValue>>* ContentArray;
		MsgObj->TryGetArrayField(TEXT("content"), ContentArray);
		TSharedPtr<FJsonObject> Block = (*ContentArray)[0]->AsObject();
		TestTrue(FString::Printf(TEXT("Result %d is intact"), i),
			Block->GetStringField(TEXT("content")).Contains(TEXT("Large result data")));
	}
	return true;
}

// Test: GetEffectiveContextLimit
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContext_EffectiveLimit, "KismetAI.Context.EffectiveContextLimit", ContextTestFlags)
bool FContext_EffectiveLimit::RunTest(const FString& Parameters)
{
	int32 Limit = FKismetAIAgent::GetModelContextLimit(TEXT("claude-sonnet-4-20250514"));
	TestEqual(TEXT("Sonnet is 200K"), Limit, 200000);

	int32 Limit1M = FKismetAIAgent::GetModelContextLimit(TEXT("claude-opus-4-6"));
	TestEqual(TEXT("Opus 4.6 is 1M"), Limit1M, 1000000);
	return true;
}

// --- ValidateToolPairing duplicate tool_use id guard ---

// Test: a duplicated assistant turn (same tool_use id) is dropped so the
// request never carries two tool_use blocks with the same id. Anthropic rejects
// non-unique tool_use ids ("tool_use ids must be unique").
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContext_Pairing_DropsDuplicateToolUse, "KismetAI.Context.ValidateToolPairing.DropsDuplicateToolUse", ContextTestFlags)
bool FContext_Pairing_DropsDuplicateToolUse::RunTest(const FString& Parameters)
{
	// Helper: append one complete round (assistant tool_use + user tool_result).
	auto AppendRound = [](TArray<TSharedPtr<FJsonValue>>& Out, const FString& Id)
	{
		auto AssistantMsg = MakeShared<FJsonObject>();
		AssistantMsg->SetStringField(TEXT("role"), TEXT("assistant"));
		TArray<TSharedPtr<FJsonValue>> AC;
		auto TU = MakeShared<FJsonObject>();
		TU->SetStringField(TEXT("type"), TEXT("tool_use"));
		TU->SetStringField(TEXT("id"), Id);
		TU->SetStringField(TEXT("name"), TEXT("read_blueprint"));
		AC.Add(MakeShared<FJsonValueObject>(TU));
		AssistantMsg->SetArrayField(TEXT("content"), AC);
		Out.Add(MakeShared<FJsonValueObject>(AssistantMsg));

		auto UserMsg = MakeShared<FJsonObject>();
		UserMsg->SetStringField(TEXT("role"), TEXT("user"));
		TArray<TSharedPtr<FJsonValue>> UC;
		auto TR = MakeShared<FJsonObject>();
		TR->SetStringField(TEXT("type"), TEXT("tool_result"));
		TR->SetStringField(TEXT("tool_use_id"), Id);
		TR->SetStringField(TEXT("content"), TEXT("ok"));
		UC.Add(MakeShared<FJsonValueObject>(TR));
		UserMsg->SetArrayField(TEXT("content"), UC);
		Out.Add(MakeShared<FJsonValueObject>(UserMsg));
	};

	TArray<TSharedPtr<FJsonValue>> Messages;

	auto Prompt = MakeShared<FJsonObject>();
	Prompt->SetStringField(TEXT("role"), TEXT("user"));
	Prompt->SetStringField(TEXT("content"), TEXT("go"));
	Messages.Add(MakeShared<FJsonValueObject>(Prompt));

	AppendRound(Messages, TEXT("toolu_dup"));   // original
	AppendRound(Messages, TEXT("toolu_dup"));   // duplicate — must be dropped
	AppendRound(Messages, TEXT("toolu_uniq"));  // distinct — must survive

	FKismetAIAgent::ValidateToolPairing(Messages);

	// Collect surviving tool_use ids and the result ids that reference them.
	TArray<FString> ToolUseIds;
	TSet<FString> ResultIds;
	for (const TSharedPtr<FJsonValue>& MsgVal : Messages)
	{
		TSharedPtr<FJsonObject> Msg = MsgVal->AsObject();
		if (!Msg.IsValid()) continue;
		const TArray<TSharedPtr<FJsonValue>>* Content;
		if (!Msg->TryGetArrayField(TEXT("content"), Content)) continue;
		for (const TSharedPtr<FJsonValue>& BlockVal : *Content)
		{
			TSharedPtr<FJsonObject> B = BlockVal->AsObject();
			if (!B.IsValid()) continue;
			const FString Type = B->GetStringField(TEXT("type"));
			if (Type == TEXT("tool_use")) ToolUseIds.Add(B->GetStringField(TEXT("id")));
			else if (Type == TEXT("tool_result")) ResultIds.Add(B->GetStringField(TEXT("tool_use_id")));
		}
	}

	TSet<FString> UniqueIds(ToolUseIds);
	TestEqual(TEXT("no duplicate tool_use ids survive"), UniqueIds.Num(), ToolUseIds.Num());
	TestTrue(TEXT("original tool_use kept once"), UniqueIds.Contains(TEXT("toolu_dup")));
	TestTrue(TEXT("distinct tool_use kept"), UniqueIds.Contains(TEXT("toolu_uniq")));
	// The orphaned duplicate tool_result must be gone, leaving exactly one per id.
	TestEqual(TEXT("no orphaned tool_result ids"), ResultIds.Num(), UniqueIds.Num());
	return true;
}
