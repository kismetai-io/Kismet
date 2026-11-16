// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Reducing a stored session to a resumable conversation.
//
// This reduction used to live inside LoadMessages, tangled with file IO, so its
// four repairs could only be exercised by writing real files. As a pure function
// over entries, the shapes that actually break resume are directly testable —
// and 00-PLAN.md §6.4 (every shipped session file must keep loading) finally has
// a guard that is not "run the editor and see".

#include "KismetAIJournalReducer.h"
#include "Fixtures/SessionFixtures.h"
#include "Misc/AutomationTest.h"

constexpr auto JournalFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{

	FString RoleSequence(const FSessionSnapshot& S)
	{
		TArray<FString> Roles;
		for (const FAgentMessage& M : S.Messages) { Roles.Add(KismetAIAgentCodec::RoleToString(M.Role)); }
		return FString::Join(Roles, TEXT(","));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJournal_CompletedSession, "KismetAI.JournalReducer.CompletedSession", JournalFlags)
bool FJournal_CompletedSession::RunTest(const FString& Parameters)
{
	const FSessionSnapshot S = KismetAIJournalReducer::ReduceJsonl(KismetAISessionFixtures::Completed());

	TestEqual(TEXT("alternates user/assistant/user/assistant"),
		RoleSequence(S), FString(TEXT("user,assistant,user,assistant")));
	TestEqual(TEXT("terminal status is read"), S.Status, FString(TEXT("completed")));
	TestEqual(TEXT("stop reason is read"), S.StopReason, FString(TEXT("end_turn")));
	TestEqual(TEXT("model is read from session_start"), S.Model, FString(TEXT("claude-sonnet-4-6")));
	TestEqual(TEXT("title is the first prompt"), S.Title, FString(TEXT("add a print node")));
	TestEqual(TEXT("nothing was skipped"), S.SkippedEntryCount, 0);
	return true;
}

// The writer stores tool results under "results", not "content". Reading the
// wrong key yields an empty user message and breaks pairing for the turn before.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJournal_ToolResultsUseResultsKey, "KismetAI.JournalReducer.ToolResultsUseResultsKey", JournalFlags)
bool FJournal_ToolResultsUseResultsKey::RunTest(const FString& Parameters)
{
	const FSessionSnapshot S = KismetAIJournalReducer::ReduceJsonl(KismetAISessionFixtures::Completed());

	bool bFoundPairedResult = false;
	for (const FAgentMessage& Message : S.Messages)
	{
		for (const FAgentContentPart& Part : Message.Parts)
		{
			bFoundPairedResult |= Part.Type == EAgentPartType::ToolResult && Part.ToolUseId == TEXT("t1");
		}
	}
	TestTrue(TEXT("the tool_result survived and kept its id"), bFoundPairedResult);
	return true;
}

// Thinking signatures must survive: the API rejects the turn without them.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJournal_ThinkingSignatureSurvives, "KismetAI.JournalReducer.ThinkingSignatureSurvives", JournalFlags)
bool FJournal_ThinkingSignatureSurvives::RunTest(const FString& Parameters)
{
	const FSessionSnapshot S = KismetAIJournalReducer::ReduceJsonl(KismetAISessionFixtures::Completed());

	bool bFoundSignature = false;
	for (const FAgentMessage& Message : S.Messages)
	{
		for (const FAgentContentPart& Part : Message.Parts)
		{
			FString Signature;
			bFoundSignature |= Part.Type == EAgentPartType::Thinking && Part.RawJson.IsValid()
				&& Part.RawJson->TryGetStringField(TEXT("signature"), Signature) && Signature == TEXT("SIG123");
		}
	}
	TestTrue(TEXT("signature preserved through the reduction"), bFoundSignature);
	return true;
}

// An abort mid-tool leaves an unpaired tool_use. Resuming with it makes the
// provider reject the whole request, so the reduction has to repair it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJournal_AbortedMidToolIsRepaired, "KismetAI.JournalReducer.AbortedMidToolIsRepaired", JournalFlags)
bool FJournal_AbortedMidToolIsRepaired::RunTest(const FString& Parameters)
{
	const FSessionSnapshot S = KismetAIJournalReducer::ReduceJsonl(KismetAISessionFixtures::AbortedMidTool());

	TestEqual(TEXT("status is read"), S.Status, FString(TEXT("aborted")));

	// The interrupted call stays in history, answered by an error that
	// says its outcome is unknown — never dropped, never a success.
	TestEqual(TEXT("user, assistant, answer"), RoleSequence(S), FString(TEXT("user,assistant,user")));
	if (S.Messages.Num() == 3)
	{
		const TArray<FAgentContentPart>& Answer = S.Messages[2].Parts;
		TestEqual(TEXT("one result"), Answer.Num(), 1);
		if (Answer.Num() == 1)
		{
			TestTrue(TEXT("typed tool result"), Answer[0].Type == EAgentPartType::ToolResult);
			TestEqual(TEXT("for t9"), Answer[0].ToolUseId, FString(TEXT("t9")));
			TestTrue(TEXT("an error"), Answer[0].bIsError);
			TestTrue(TEXT("outcome unrecorded"), Answer[0].ToolResultContent.Contains(TEXT("OUTCOME_UNRECORDED")));
		}
	}
	return true;
}

// A failed run records the error but it is not conversation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJournal_FailedSession, "KismetAI.JournalReducer.FailedSession", JournalFlags)
bool FJournal_FailedSession::RunTest(const FString& Parameters)
{
	const FSessionSnapshot S = KismetAIJournalReducer::ReduceJsonl(KismetAISessionFixtures::Failed());

	TestEqual(TEXT("status is read"), S.Status, FString(TEXT("failed")));
	TestEqual(TEXT("stop reason carries the wire error code"), S.StopReason, FString(TEXT("authentication_error")));
	TestEqual(TEXT("the error entry is not a message"), RoleSequence(S), FString(TEXT("user")));
	return true;
}

// A crashed editor leaves a half-written final line. Losing the whole session
// over it would be a poor trade.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJournal_TruncatedTailTolerated, "KismetAI.JournalReducer.TruncatedTailTolerated", JournalFlags)
bool FJournal_TruncatedTailTolerated::RunTest(const FString& Parameters)
{
	const FSessionSnapshot S = KismetAIJournalReducer::ReduceJsonl(KismetAISessionFixtures::TruncatedTail());

	TestEqual(TEXT("the complete entries still load"), RoleSequence(S), FString(TEXT("user")));
	TestEqual(TEXT("and the bad line is counted, not fatal"), S.SkippedEntryCount, 1);
	return true;
}

// Forward compatibility: unknown entry types are ignored; a newer format version
// loads read-only.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJournal_UnknownEntriesIgnored, "KismetAI.JournalReducer.UnknownEntriesIgnored", JournalFlags)
bool FJournal_UnknownEntriesIgnored::RunTest(const FString& Parameters)
{
	const FSessionSnapshot S = KismetAIJournalReducer::ReduceJsonl(KismetAISessionFixtures::WithUnknownEntries());

	TestEqual(TEXT("known entries reduce normally"), RoleSequence(S), FString(TEXT("user,assistant")));
	TestEqual(TEXT("an unknown entry type is not a parse failure"), S.SkippedEntryCount, 0);
	// V2 is a newer journal format. The entries this build understands
	// still reduce for inspection, but the session is not resumable.
	TestEqual(TEXT("a newer format version is still readable"), S.Model, FString(TEXT("claude-opus-9")));
	TestTrue(TEXT("and reported as unsupported"), S.Integrity == EJournalIntegrity::UnsupportedVersion);
	TestFalse(TEXT("so it cannot be resumed"), S.CanResume());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJournal_EmptyInput, "KismetAI.JournalReducer.EmptyInput", JournalFlags)
bool FJournal_EmptyInput::RunTest(const FString& Parameters)
{
	const FSessionSnapshot S = KismetAIJournalReducer::ReduceJsonl(TEXT(""));
	TestEqual(TEXT("no messages"), S.Messages.Num(), 0);
	TestEqual(TEXT("nothing skipped"), S.SkippedEntryCount, 0);
	TestTrue(TEXT("no status"), S.Status.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJournal_TypedReplayKeepsOpaqueMetadata,
	"KismetAI.JournalReducer.TypedReplayKeepsOpaqueMetadata", JournalFlags)
bool FJournal_TypedReplayKeepsOpaqueMetadata::RunTest(const FString& Parameters)
{
	const FSessionSnapshot Snapshot = KismetAIJournalReducer::ReduceJsonl(
		TEXT("{\"type\":\"user_message\",\"content\":\"inspect\"}\n")
		TEXT("{\"type\":\"assistant_message\",\"uuid\":\"journal-only\",\"message_extensions\":{\"provider_trace\":\"opaque\"},\"content\":[")
		TEXT("{\"type\":\"thinking\",\"thinking\":\"private\",\"signature\":\"SIGNED\"},")
		TEXT("{\"type\":\"provider_future\",\"payload\":{\"value\":7}},")
		TEXT("{\"type\":\"text\",\"text\":\"Ready.\"}]}\n"));
	TestEqual(TEXT("typed transcript has both messages"), Snapshot.Messages.Num(), 2);
	if (Snapshot.Messages.Num() != 2) { return false; }
	const FAgentMessage& Assistant = Snapshot.Messages[1];
	TestEqual(TEXT("opaque and thinking are not visible prose"), Assistant.GetText(), FString(TEXT("Ready.")));
	TestEqual(TEXT("all typed parts survive"), Assistant.Parts.Num(), 3);
	const TSharedPtr<FJsonObject> Encoded = KismetAIAgentCodec::ToAnthropicJson(Assistant);
	TestEqual(TEXT("message extension survives journal replay"), Encoded->GetStringField(TEXT("provider_trace")), FString(TEXT("opaque")));
	TestFalse(TEXT("journal UUID is not provider metadata"), Encoded->HasField(TEXT("uuid")));
	TestFalse(TEXT("journal type is not provider metadata"), Encoded->HasField(TEXT("type")));
	const TArray<FAgentMessage> Request = KismetAIJournalReducer::ProjectResumeRequest(Snapshot.Context);
	TestEqual(TEXT("resume retains both messages"), Request.Num(), 2);
	if (Request.Num() == 2)
	{
		TestEqual(TEXT("resume strips only prior thinking"), Request[1].Parts.Num(), 2);
		if (Request[1].Parts.Num() != 2) { return false; }
		TestTrue(TEXT("unknown part is retained as opaque"), Request[1].Parts[0].Type == EAgentPartType::Unknown);
		TestEqual(TEXT("resume preserves message extension"), KismetAIAgentCodec::ToAnthropicJson(Request[1])->GetStringField(TEXT("provider_trace")), FString(TEXT("opaque")));
	}
	TestEqual(TEXT("resume did not change the transcript"), Assistant.Parts.Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJournal_TypedPartialBatchPreservesPairing,
	"KismetAI.JournalReducer.TypedPartialBatchPreservesPairing", JournalFlags)
bool FJournal_TypedPartialBatchPreservesPairing::RunTest(const FString& Parameters)
{
	const FSessionSnapshot Snapshot = KismetAIJournalReducer::ReduceJsonl(
		TEXT("{\"type\":\"user_message\",\"content\":\"inspect\"}\n")
		TEXT("{\"type\":\"assistant_message\",\"run_id\":\"r\",\"turn_id\":\"r/t0\",\"tool_call_count\":2,\"message_extensions\":{\"provider_trace\":\"opaque\"},\"content\":[{\"type\":\"thinking\",\"thinking\":\"private\",\"signature\":\"SIGNED\"},{\"type\":\"provider_future\",\"payload\":{\"value\":7}},{\"type\":\"tool_use\",\"id\":\"A\",\"name\":\"read_blueprint\",\"input\":{}},{\"type\":\"tool_use\",\"id\":\"B\",\"name\":\"read_blueprint\",\"input\":{}}]}\n")
		TEXT("{\"type\":\"tool_result_message\",\"record\":\"tool_call\",\"schema\":1,\"run_id\":\"r\",\"turn_id\":\"r/t0\",\"call_id\":\"r/t0/c0\",\"call_index\":0,\"call_count\":2,\"tool_use_id\":\"A\",\"tool_name\":\"read_blueprint\",\"status\":\"succeeded\",\"attempted\":true,\"results\":[{\"type\":\"tool_result\",\"tool_use_id\":\"A\",\"content\":\"COMMITTED_A\"}]}\n"));
	TestEqual(TEXT("one outcome is unrecorded"), Snapshot.UnrecordedCalls, 1);
	TestEqual(TEXT("user, assistant, paired results"), Snapshot.Messages.Num(), 3);
	if (Snapshot.Messages.Num() != 3) { return false; }
	const FAgentMessage& Assistant = Snapshot.Messages[1];
	TestEqual(TEXT("thinking and unknown block survive with both calls"), Assistant.Parts.Num(), 4);
	if (Assistant.Parts.Num() != 4 || !Assistant.Parts[0].RawJson.IsValid()) { return false; }
	TestTrue(TEXT("thinking remains typed thinking"), Assistant.Parts[0].Type == EAgentPartType::Thinking);
	TestEqual(TEXT("thinking signature retained"), Assistant.Parts[0].RawJson->GetStringField(TEXT("signature")), FString(TEXT("SIGNED")));
	TestTrue(TEXT("unknown provider part remains opaque"), Assistant.Parts[1].Type == EAgentPartType::Unknown);
	TestTrue(TEXT("opaque content does not become user text"), Assistant.GetText().IsEmpty());
	const TArray<FAgentContentPart>& Results = Snapshot.Messages[2].Parts;
	TestEqual(TEXT("one result per call"), Results.Num(), 2);
	if (Results.Num() != 2) { return false; }
	TestEqual(TEXT("first result pairs with A"), Results[0].ToolUseId, FString(TEXT("A")));
	TestEqual(TEXT("A retains committed content"), Results[0].ToolResultContent, FString(TEXT("COMMITTED_A")));
	TestFalse(TEXT("A remains successful"), Results[0].bIsError);
	TestEqual(TEXT("second result pairs with B"), Results[1].ToolUseId, FString(TEXT("B")));
	TestTrue(TEXT("B is uncertainty, never success"), Results[1].bIsError && Results[1].ToolResultContent.Contains(TEXT("OUTCOME_UNRECORDED")));
	const TArray<FAgentMessage> Request = KismetAIJournalReducer::ProjectResumeRequest(Snapshot.Context);
	TestEqual(TEXT("resume keeps paired batch"), Request.Num(), 3);
	if (Request.Num() == 3)
	{
		TestEqual(TEXT("prior thinking is removed, opaque block and calls remain"), Request[1].Parts.Num(), 3);
		if (Request[2].Parts.Num() != 2) { return false; }
		TestEqual(TEXT("B remains uncertain in request"), Request[2].Parts[1].ToolUseId, FString(TEXT("B")));
		TestTrue(TEXT("request still marks B error"), Request[2].Parts[1].bIsError);
	}
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJournal_CommittedCallsMatchAcceptedEvidence,
	"KismetAI.JournalReducer.CommittedCallsMatchAcceptedEvidence", JournalFlags)
bool FJournal_CommittedCallsMatchAcceptedEvidence::RunTest(const FString& Parameters)
{
	const FString Batch = TEXT("{\"type\":\"assistant_message\",\"run_id\":\"original\",\"turn_id\":\"original/t0\",\"tool_call_count\":3,\"content\":[{\"type\":\"tool_use\",\"id\":\"A\",\"name\":\"edit_blueprint\",\"input\":{\"blueprint\":\"/Game/A\"}},{\"type\":\"tool_use\",\"id\":\"B\",\"name\":\"compile_blueprint\",\"input\":{}},{\"type\":\"tool_use\",\"id\":\"C\",\"name\":\"edit_blueprint\",\"input\":{}}]}\n");
	auto Call = [](int32 Index, const TCHAR* Id, int32 Schema, const TCHAR* Save)
	{
		return FString::Printf(TEXT("{\"type\":\"tool_result_message\",\"record\":\"tool_call\",\"schema\":%d,\"run_id\":\"incorrect\",\"turn_id\":\"original/t0\",\"call_id\":\"incorrect\",\"call_index\":%d,\"tool_use_id\":\"%s\",\"tool_name\":\"incorrect\",\"status\":\"succeeded\",\"attempted\":true,\"mutation_outcome\":{\"save_status\":\"%s\"},\"results\":[{\"type\":\"tool_result\",\"tool_use_id\":\"%s\",\"content\":\"result\"}]}\n"), Schema, Index, Id, Save, Id);
	};
	const FSessionSnapshot Snapshot = KismetAIJournalReducer::ReduceJsonl(Batch
		+ Call(0, TEXT("A"), 1, TEXT("failed"))
		+ Call(0, TEXT("A"), 1, TEXT("saved"))
		+ Call(1, TEXT("B"), 999, TEXT("saved"))
		+ Call(2, TEXT("wrong-id"), 1, TEXT("saved")));
	if (!TestEqual(TEXT("only the accepted first indexed record is evidence"), Snapshot.CommittedCalls.Num(), 1)) { return false; }
	const TSharedPtr<FJsonObject>& Evidence = Snapshot.CommittedCalls[0];
	TestEqual(TEXT("original run identity comes from batch"), Evidence->GetStringField(TEXT("run_id")), FString(TEXT("original")));
	TestEqual(TEXT("call identity comes from batch"), Evidence->GetStringField(TEXT("call_id")), FString(TEXT("original/t0/c0")));
	TestEqual(TEXT("tool name comes from batch"), Evidence->GetStringField(TEXT("tool_name")), FString(TEXT("edit_blueprint")));
	TestEqual(TEXT("target input comes from batch"), Evidence->GetObjectField(TEXT("input"))->GetStringField(TEXT("blueprint")), FString(TEXT("/Game/A")));
	TestEqual(TEXT("later duplicate success cannot erase first failure"), Evidence->GetObjectField(TEXT("mutation_outcome"))->GetStringField(TEXT("save_status")), FString(TEXT("failed")));
	TestEqual(TEXT("future schema and unmatched calls stay unrecorded"), Snapshot.UnrecordedCalls, 2);
	return true;
}
