// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Streamed events are attempt-aware and serialized safely.
//
// Every scenario runs the production model-client code — stream decoder,
// attempt classifier, retry loop (Fakes/FixtureTransport.h replaces only curl)
// — under the real RunTask loop, and reads the journal back from disk.
//
// The defects these reproduce:
//   - a retry after partial text continued the same text_start..text_end
//     segment, so the panel showed "use A" + "use B" as one answer while the
//     journal kept only "use B";
//   - thinking was escaped by hand without tabs (and every hand-escaped event
//     missed the other control characters), so events were not valid JSON;
//   - the tool_call event spliced the model-chosen tool name in unescaped;
//   - Feed decoded each byte chunk on its own, so a UTF-8 sequence split
//     across reads became garbage in the streamed and the committed text.

#include "Fakes/FixtureTransport.h"
#include "Fakes/ScriptedAgentSeams.h"
#include "KismetAISessionStorage.h"
#include "Misc/AutomationTest.h"

using namespace KismetAITestFakes;
using namespace KismetAIFixtureTransport;

constexpr auto StreamAttemptFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	FKismetAIAgentConfig MakeAttemptConfig(int32 MaxRetries)
	{
		FKismetAIAgentConfig C;
		C.ApiKey = TEXT("test-key-unused");
		C.Model = TEXT("claude-sonnet-4-6");
		C.MaxSteps = 4;
		C.MaxRetries = MaxRetries;
		C.RetryBaseDelayMs = 1.0;
		C.RetryMaxDelayMs = 1.0;
		C.RetryJitterFactor = 0.0;
		C.ThinkingMode = EThinkingMode::Disabled;
		return C;
	}

	struct FAttemptSessionCleanup
	{
		FString SessionId;
		~FAttemptSessionCleanup()
		{
			if (!SessionId.IsEmpty()) { FKismetAISessionStorage::DeleteSession(SessionId); }
		}
	};

	// Valid JSON in the strict sense every consumer needs: it parses, and no raw
	// control character survives inside it (Python's json.loads, JSON.parse and
	// the SSE framing all reject or split on those).
	TSharedPtr<FJsonObject> SaParseStrict(FAutomationTestBase& Test, const FString& Type, const FString& Json)
	{
		for (int32 i = 0; i < Json.Len(); ++i)
		{
			if (Json[i] < 0x20)
			{
				Test.AddError(FString::Printf(TEXT("%s event carries raw control character U+%04X at %d: %s"),
					*Type, (int32)Json[i], i, *Json.Left(200)));
				return nullptr;
			}
		}
		TSharedPtr<FJsonObject> Obj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
		{
			Test.AddError(FString::Printf(TEXT("%s event is not valid JSON: %s"), *Type, *Json.Left(200)));
			return nullptr;
		}
		return Obj;
	}

	bool SaAllEventsStrict(FAutomationTestBase& Test, const FEventRecorder& Rec)
	{
		bool bAll = true;
		for (const TPair<FString, FString>& E : Rec.Events)
		{
			bAll &= SaParseStrict(Test, E.Key, E.Value).IsValid();
		}
		return bAll;
	}

	// Text the panel would show, as text_start..text_end segments.
	struct FSaTextSegment
	{
		FString AttemptId;
		FString Text;
		bool bClosed = false;
	};

	TArray<FSaTextSegment> SaTextSegments(const FEventRecorder& Rec)
	{
		TArray<FSaTextSegment> Out;
		for (const TPair<FString, FString>& E : Rec.Events)
		{
			TSharedPtr<FJsonObject> Obj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(E.Value);
			FJsonSerializer::Deserialize(Reader, Obj);
			if (E.Key == TEXT("text_start"))
			{
				FSaTextSegment& S = Out.AddDefaulted_GetRef();
				if (Obj.IsValid()) { Obj->TryGetStringField(TEXT("attempt_id"), S.AttemptId); }
			}
			else if (E.Key == TEXT("text_delta") && Out.Num() > 0 && Obj.IsValid())
			{
				Out.Last().Text += Obj->GetStringField(TEXT("text"));
			}
			else if (E.Key == TEXT("text_end") && Out.Num() > 0)
			{
				Out.Last().bClosed = true;
			}
		}
		return Out;
	}

	TArray<TSharedPtr<FJsonObject>> SaEventsOfType(const FEventRecorder& Rec, const FString& Type)
	{
		TArray<TSharedPtr<FJsonObject>> Out;
		for (const TPair<FString, FString>& E : Rec.Events)
		{
			if (E.Key != Type) { continue; }
			TSharedPtr<FJsonObject> Obj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(E.Value);
			if (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid()) { Out.Add(Obj); }
		}
		return Out;
	}

	TArray<TSharedPtr<FJsonObject>> SaEntriesOfType(const TArray<TSharedPtr<FJsonObject>>& Entries, const FString& Type)
	{
		TArray<TSharedPtr<FJsonObject>> Out;
		for (const TSharedPtr<FJsonObject>& E : Entries)
		{
			FString T;
			if (E.IsValid() && E->TryGetStringField(TEXT("type"), T) && T == Type) { Out.Add(E); }
		}
		return Out;
	}

	FString SaCommittedText(const TSharedPtr<FJsonObject>& AssistantEntry)
	{
		FString Out;
		const TArray<TSharedPtr<FJsonValue>>* Content;
		if (AssistantEntry.IsValid() && AssistantEntry->TryGetArrayField(TEXT("content"), Content))
		{
			for (const TSharedPtr<FJsonValue>& V : *Content)
			{
				const TSharedPtr<FJsonObject> B = V->AsObject();
				if (B.IsValid() && B->GetStringField(TEXT("type")) == TEXT("text")) { Out += B->GetStringField(TEXT("text")); }
			}
		}
		return Out;
	}

	FString SaStr(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		FString V;
		if (Obj.IsValid()) { Obj->TryGetStringField(Field, V); }
		return V;
	}

	// Terminal run outcomes: done, or an error that ends the run.
	int32 SaTerminalCount(const FEventRecorder& Rec)
	{
		return Rec.Count(TEXT("done")) + Rec.Count(TEXT("error"));
	}

	// "use A" streamed, then a tool call whose argument object is cut off, then
	// the stream closes without message_stop: the attempt is incomplete.
	FScriptedAttempt SaInterruptedUseA()
	{
		FScriptedAttempt A;
		A.Bytes = FAnthropicStreamBuilder()
			.MessageStart()
			.BlockStart(TEXT("text")).Text(TEXT("The graph is correct. ")).Text(TEXT("Use A.")).BlockStop()
			.BlockStart(TEXT("tool_use"), TEXT("toolu_partial"), TEXT("edit_blueprint"))
			.ToolJson(TEXT("{\"blueprint\":\"/Game/P110/BP_A\",\"operat"))
			.Bytes();
		return A;
	}

	FScriptedAttempt SaCompleteUseB()
	{
		FScriptedAttempt B;
		B.Bytes = FAnthropicStreamBuilder()
			.MessageStart()
			.BlockStart(TEXT("text")).Text(TEXT("The graph contains an error. Use B.")).BlockStop()
			.MessageEnd(TEXT("end_turn"))
			.Bytes();
		return B;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// R5 reproduction: "use A", disconnect, retry "use B".
// ─────────────────────────────────────────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamAttempt_RetriedTextIsNotMerged,
	"KismetAI.StreamAttempts.RetriedTextIsNotMerged", StreamAttemptFlags)
bool FStreamAttempt_RetriedTextIsNotMerged::RunTest(const FString& Parameters)
{
	FFixtureTransportModelClient Model({ SaInterruptedUseA(), SaCompleteUseB() });
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;
	FKismetAIAgent Agent(Tools, Model, MakeAttemptConfig(/*MaxRetries=*/1));
	FAttemptSessionCleanup Cleanup;

	Agent.RunTask(TEXT("is the graph right?"), Rec.MakeCallback());
	Cleanup.SessionId = Agent.GetSessionId();

	TestEqual(TEXT("both scripted attempts were used"), Model.AttemptsUsed(), 2);
	TestEqual(TEXT("the incomplete tool call never executed"), Tools.CallLog.Num(), 0);
	TestEqual(TEXT("exactly one terminal outcome"), SaTerminalCount(Rec), 1);
	TestTrue(TEXT("run completed"), Rec.HasType(TEXT("done")));
	SaAllEventsStrict(*this, Rec);

	// The panel's view: no single text segment carries both answers.
	const TArray<FSaTextSegment> Segments = SaTextSegments(Rec);
	for (const FSaTextSegment& S : Segments)
	{
		TestFalse(*FString::Printf(TEXT("segment '%s' does not merge the failed and the successful attempt"), *S.Text),
			S.Text.Contains(TEXT("Use A")) && S.Text.Contains(TEXT("Use B")));
		TestTrue(TEXT("every segment is closed"), S.bClosed);
	}
	TestEqual(TEXT("two text segments, one per attempt"), Segments.Num(), 2);
	if (Segments.Num() != 2) { return false; }
	TestNotEqual(TEXT("segments belong to different attempts"), Segments[0].AttemptId, Segments[1].AttemptId);

	// Attempt transitions are explicit and carry the identities.
	const TArray<TSharedPtr<FJsonObject>> Starts = SaEventsOfType(Rec, TEXT("attempt_start"));
	const TArray<TSharedPtr<FJsonObject>> Ends = SaEventsOfType(Rec, TEXT("attempt_end"));
	TestEqual(TEXT("two attempt_start"), Starts.Num(), 2);
	TestEqual(TEXT("two attempt_end"), Ends.Num(), 2);
	if (Starts.Num() != 2 || Ends.Num() != 2) { return false; }
	TestEqual(TEXT("first attempt interrupted"), SaStr(Ends[0], TEXT("status")), FString(TEXT("interrupted")));
	TestTrue(TEXT("first attempt will be retried"), Ends[0]->GetBoolField(TEXT("will_retry")));
	TestEqual(TEXT("interrupted attempt names its text"), SaStr(Ends[0], TEXT("text")), FString(TEXT("The graph is correct. Use A.")));
	TestEqual(TEXT("second attempt completed"), SaStr(Ends[1], TEXT("status")), FString(TEXT("completed")));
	TestEqual(TEXT("retry replaces the interrupted attempt"), SaStr(Starts[1], TEXT("replaces")), SaStr(Ends[0], TEXT("attempt_id")));
	TestEqual(TEXT("first segment is the interrupted attempt"), Segments[0].AttemptId, SaStr(Ends[0], TEXT("attempt_id")));
	TestEqual(TEXT("one message id across both attempts"), SaStr(Starts[0], TEXT("message_id")), SaStr(Starts[1], TEXT("message_id")));
	TestEqual(TEXT("one request id across both attempts"), SaStr(Starts[0], TEXT("request_id")), SaStr(Starts[1], TEXT("request_id")));
	TestFalse(TEXT("run id present"), SaStr(Starts[0], TEXT("run_id")).IsEmpty());
	TestTrue(TEXT("turn id is derived from the run id"), SaStr(Starts[0], TEXT("turn_id")).StartsWith(SaStr(Starts[0], TEXT("run_id"))));

	// The journal agrees: the interrupted attempt is recorded as such, and the
	// committed message is the successful attempt only.
	const TArray<TSharedPtr<FJsonObject>> Entries = FKismetAISessionStorage::LoadAllEntries(Cleanup.SessionId);
	const TArray<TSharedPtr<FJsonObject>> Attempts = SaEntriesOfType(Entries, TEXT("model_attempt"));
	const TArray<TSharedPtr<FJsonObject>> Assistant = SaEntriesOfType(Entries, TEXT("assistant_message"));
	TestEqual(TEXT("one model_attempt record (the interrupted one)"), Attempts.Num(), 1);
	TestEqual(TEXT("one committed assistant message"), Assistant.Num(), 1);
	if (Attempts.Num() != 1 || Assistant.Num() != 1) { return false; }
	TestEqual(TEXT("journal attempt status matches the event"), SaStr(Attempts[0], TEXT("status")), SaStr(Ends[0], TEXT("status")));
	TestEqual(TEXT("journal attempt id matches the event"), SaStr(Attempts[0], TEXT("attempt_id")), SaStr(Ends[0], TEXT("attempt_id")));
	TestEqual(TEXT("journal keeps the interrupted text"), SaStr(Attempts[0], TEXT("text")), FString(TEXT("The graph is correct. Use A.")));
	TestEqual(TEXT("committed text is the retry only"), SaCommittedText(Assistant[0]), FString(TEXT("The graph contains an error. Use B.")));
	TestEqual(TEXT("committed message names its attempt"), SaStr(Assistant[0], TEXT("attempt_id")), SaStr(Ends[1], TEXT("attempt_id")));
	TestEqual(TEXT("committed message names its message id"), SaStr(Assistant[0], TEXT("message_id")), SaStr(Starts[1], TEXT("message_id")));

	// Resume never sees the interrupted text.
	const TArray<TSharedPtr<FJsonValue>> Resumed = FKismetAISessionStorage::LoadMessages(Cleanup.SessionId);
	for (const TSharedPtr<FJsonValue>& M : Resumed)
	{
		FString Json;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Json);
		FJsonSerializer::Serialize(M->AsObject().ToSharedRef(), W);
		TestFalse(TEXT("resumed history has no interrupted text"), Json.Contains(TEXT("Use A")));
	}
	return true;
}

// An in-stream provider error after partial text (the common overload case)
// is retried the same way and leaves the same record.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamAttempt_InStreamErrorRetry,
	"KismetAI.StreamAttempts.InStreamErrorRetry", StreamAttemptFlags)
bool FStreamAttempt_InStreamErrorRetry::RunTest(const FString& Parameters)
{
	FScriptedAttempt Overloaded;
	Overloaded.Bytes = FAnthropicStreamBuilder()
		.MessageStart()
		.BlockStart(TEXT("text")).Text(TEXT("Use A")).Raw(
			TEXT("event: error\ndata: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}\n\n"))
		.Bytes();

	FFixtureTransportModelClient Model({ Overloaded, SaCompleteUseB() });
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;
	FKismetAIAgent Agent(Tools, Model, MakeAttemptConfig(/*MaxRetries=*/1));
	FAttemptSessionCleanup Cleanup;
	Agent.RunTask(TEXT("is the graph right?"), Rec.MakeCallback());
	Cleanup.SessionId = Agent.GetSessionId();

	SaAllEventsStrict(*this, Rec);
	const TArray<TSharedPtr<FJsonObject>> Ends = SaEventsOfType(Rec, TEXT("attempt_end"));
	TestEqual(TEXT("two attempts"), Ends.Num(), 2);
	if (Ends.Num() != 2) { return false; }
	TestEqual(TEXT("first interrupted"), SaStr(Ends[0], TEXT("status")), FString(TEXT("interrupted")));
	TestEqual(TEXT("error code is the provider's"), SaStr(Ends[0], TEXT("error_code")), FString(TEXT("overloaded_error")));
	TestEqual(TEXT("one terminal outcome"), SaTerminalCount(Rec), 1);
	for (const FSaTextSegment& S : SaTextSegments(Rec))
	{
		TestFalse(TEXT("no merged segment"), S.Text.Contains(TEXT("Use A")) && S.Text.Contains(TEXT("Use B")));
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Disconnect inside a tool argument object with no retry left: zero tools run,
// the attempt is reported failed, and the run ends exactly once.
// ─────────────────────────────────────────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamAttempt_IncompleteToolCallNeverExecutes,
	"KismetAI.StreamAttempts.IncompleteToolCallNeverExecutes", StreamAttemptFlags)
bool FStreamAttempt_IncompleteToolCallNeverExecutes::RunTest(const FString& Parameters)
{
	// Cut by the network (curl exit 56) after HTTP 200 and after the text.
	FScriptedAttempt Cut = SaInterruptedUseA();
	Cut.ExitCode = CurlRecvError;

	FFixtureTransportModelClient Model({ Cut });
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;
	FKismetAIAgent Agent(Tools, Model, MakeAttemptConfig(/*MaxRetries=*/0));
	FAttemptSessionCleanup Cleanup;
	AddExpectedError(TEXT("LLM API returned status 200 .curl exit 56"), EAutomationExpectedErrorFlags::Contains, 1);
	Agent.RunTask(TEXT("edit it"), Rec.MakeCallback());
	Cleanup.SessionId = Agent.GetSessionId();

	TestEqual(TEXT("no tool executed from the incomplete call"), Tools.CallLog.Num(), 0);
	TestFalse(TEXT("no tool_call event for the incomplete call"), Rec.HasType(TEXT("tool_call")));
	TestEqual(TEXT("exactly one terminal outcome"), SaTerminalCount(Rec), 1);
	TestTrue(TEXT("the run failed"), Rec.HasType(TEXT("error")));
	SaAllEventsStrict(*this, Rec);

	const TArray<TSharedPtr<FJsonObject>> Ends = SaEventsOfType(Rec, TEXT("attempt_end"));
	TestEqual(TEXT("one attempt"), Ends.Num(), 1);
	if (Ends.Num() != 1) { return false; }
	TestEqual(TEXT("attempt failed"), SaStr(Ends[0], TEXT("status")), FString(TEXT("failed")));
	TestFalse(TEXT("no retry"), Ends[0]->GetBoolField(TEXT("will_retry")));

	const TArray<TSharedPtr<FJsonObject>> Entries = FKismetAISessionStorage::LoadAllEntries(Cleanup.SessionId);
	TestEqual(TEXT("nothing committed as an assistant message"), SaEntriesOfType(Entries, TEXT("assistant_message")).Num(), 0);
	const TArray<TSharedPtr<FJsonObject>> Attempts = SaEntriesOfType(Entries, TEXT("model_attempt"));
	TestEqual(TEXT("the failed attempt is recorded"), Attempts.Num(), 1);
	if (Attempts.Num() == 1)
	{
		TestEqual(TEXT("recorded as failed"), SaStr(Attempts[0], TEXT("status")), FString(TEXT("failed")));
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Abort races the final retry.
// ─────────────────────────────────────────────────────────────────────────────

// Stop lands after transport classification but before the loop accepts it:
// no assistant intent is committed, no tool runs, and the run ends once as aborted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamAttempt_AbortAsFinalRetrySucceeds,
	"KismetAI.StreamAttempts.AbortAsFinalRetrySucceeds", StreamAttemptFlags)
bool FStreamAttempt_AbortAsFinalRetrySucceeds::RunTest(const FString& Parameters)
{
	FScriptedAttempt Final;
	Final.Bytes = FAnthropicStreamBuilder()
		.MessageStart()
		.BlockStart(TEXT("text")).Text(TEXT("Use B.")).BlockStop()
		.BlockStart(TEXT("tool_use"), TEXT("toolu_B"), TEXT("search_assets"))
		.ToolJson(TEXT("{\"query\":\"BP_B\"}")).BlockStop()
		.MessageEnd(TEXT("tool_use"))
		.Bytes();

	FKismetAIAgent* AgentPtr = nullptr;
	Final.OnClassified = [&AgentPtr]() { if (AgentPtr) { AgentPtr->Abort(); } };

	FFixtureTransportModelClient Model({ SaInterruptedUseA(), Final });
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;
	FKismetAIAgent Agent(Tools, Model, MakeAttemptConfig(/*MaxRetries=*/1));
	AgentPtr = &Agent;
	FAttemptSessionCleanup Cleanup;
	Agent.RunTask(TEXT("find B"), Rec.MakeCallback());
	Cleanup.SessionId = Agent.GetSessionId();

	TestEqual(TEXT("exactly one terminal outcome"), SaTerminalCount(Rec), 1);
	const FString* Done = Rec.FirstJsonOfType(TEXT("done"));
	TestTrue(TEXT("run ends aborted"), Done && Done->Contains(TEXT("\"status\":\"aborted\"")));
	TestEqual(TEXT("no tool ran after the stop"), Tools.CallLog.Num(), 0);
	SaAllEventsStrict(*this, Rec);

	const TArray<TSharedPtr<FJsonObject>> Entries = FKismetAISessionStorage::LoadAllEntries(Cleanup.SessionId);
	TestEqual(TEXT("cancelled response is not committed"), SaEntriesOfType(Entries, TEXT("assistant_message")).Num(), 0);
	const TArray<TSharedPtr<FJsonObject>> Ends = SaEventsOfType(Rec, TEXT("attempt_end"));
	TestEqual(TEXT("two attempts closed"), Ends.Num(), 2);
	if (Ends.Num() == 2)
	{
		TestEqual(TEXT("final attempt is aborted before acceptance"), SaStr(Ends[1], TEXT("status")), FString(TEXT("aborted")));
	}
	return true;
}

// Stop lands while the final attempt is still being read (the transport sees
// it, as curl's wait loop does): nothing is committed, the shown text is marked
// aborted in the stream and the journal, and the run ends once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamAttempt_AbortDuringFinalRetry,
	"KismetAI.StreamAttempts.AbortDuringFinalRetry", StreamAttemptFlags)
bool FStreamAttempt_AbortDuringFinalRetry::RunTest(const FString& Parameters)
{
	FScriptedAttempt Final = SaCompleteUseB();
	FKismetAIAgent* AgentPtr = nullptr;
	Final.OnDelivered = [&AgentPtr]() { if (AgentPtr) { AgentPtr->Abort(); } };

	FFixtureTransportModelClient Model({ SaInterruptedUseA(), Final });
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;
	FKismetAIAgent Agent(Tools, Model, MakeAttemptConfig(/*MaxRetries=*/1));
	AgentPtr = &Agent;
	FAttemptSessionCleanup Cleanup;
	// The transport reports a stopped call the way curl's does.
	AddExpectedError(TEXT("LLM API returned status 200 .curl exit 0"), EAutomationExpectedErrorFlags::Contains, 1);
	Agent.RunTask(TEXT("is the graph right?"), Rec.MakeCallback());
	Cleanup.SessionId = Agent.GetSessionId();

	TestEqual(TEXT("exactly one terminal outcome"), SaTerminalCount(Rec), 1);
	const FString* Done = Rec.FirstJsonOfType(TEXT("done"));
	TestTrue(TEXT("run ends aborted"), Done && Done->Contains(TEXT("\"status\":\"aborted\"")));
	SaAllEventsStrict(*this, Rec);

	const TArray<TSharedPtr<FJsonObject>> Ends = SaEventsOfType(Rec, TEXT("attempt_end"));
	TestEqual(TEXT("two attempts closed"), Ends.Num(), 2);
	if (Ends.Num() == 2)
	{
		TestEqual(TEXT("final attempt aborted"), SaStr(Ends[1], TEXT("status")), FString(TEXT("aborted")));
	}
	const TArray<TSharedPtr<FJsonObject>> Entries = FKismetAISessionStorage::LoadAllEntries(Cleanup.SessionId);
	TestEqual(TEXT("nothing committed"), SaEntriesOfType(Entries, TEXT("assistant_message")).Num(), 0);
	const TArray<TSharedPtr<FJsonObject>> Attempts = SaEntriesOfType(Entries, TEXT("model_attempt"));
	TestEqual(TEXT("both shown attempts are recorded"), Attempts.Num(), 2);
	if (Attempts.Num() == 2)
	{
		TestEqual(TEXT("journal says aborted"), SaStr(Attempts[1], TEXT("status")), FString(TEXT("aborted")));
		TestEqual(TEXT("journal keeps the aborted text"), SaStr(Attempts[1], TEXT("text")), FString(TEXT("The graph contains an error. Use B.")));
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Serialization: control characters, quotes, Unicode, split chunks.
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
	// Tab, newline, CR, quote, backslash, a C0 control, U+2028, Latin-1, CJK,
	// and an astral-plane emoji (a surrogate pair in UTF-16, four bytes in UTF-8).
	const FString SaNasty = FString(TEXT("a\tb\nc\r\"q\" \\ \x01 \x1b[0m   naïve 漢字 ")) + FString(TEXT("\U0001F600 end"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamAttempt_EventsAreStrictJson,
	"KismetAI.StreamAttempts.EventsAreStrictJson", StreamAttemptFlags)
bool FStreamAttempt_EventsAreStrictJson::RunTest(const FString& Parameters)
{
	for (int32 ChunkSize : { 1, 2, 3, 5, 4096 })
	{
		FScriptedAttempt A;
		A.ChunkSize = ChunkSize;
		A.Bytes = FAnthropicStreamBuilder()
			.MessageStart()
			.BlockStart(TEXT("thinking")).Thinking(SaNasty).Signature(TEXT("sig")).BlockStop()
			.BlockStart(TEXT("text")).Text(SaNasty).BlockStop()
			.MessageEnd(TEXT("end_turn"))
			.Bytes();

		FFixtureTransportModelClient Model({ A });
		FScriptedToolInvoker Tools;
		FEventRecorder Rec;
		FKismetAIAgentConfig Config = MakeAttemptConfig(0);
		FKismetAIAgent Agent(Tools, Model, Config);
		FAttemptSessionCleanup Cleanup;
		Agent.RunTask(TEXT("hi"), Rec.MakeCallback());
		Cleanup.SessionId = Agent.GetSessionId();

		const FString Ctx = FString::Printf(TEXT("[chunk %d] "), ChunkSize);
		TestTrue(Ctx + TEXT("every event is strict JSON"), SaAllEventsStrict(*this, Rec));

		FString Streamed;
		for (const TSharedPtr<FJsonObject>& D : SaEventsOfType(Rec, TEXT("text_delta"))) { Streamed += SaStr(D, TEXT("text")); }
		TestEqual(Ctx + TEXT("streamed text round-trips exactly"), Streamed, SaNasty);

		FString ThinkingStreamed;
		for (const TSharedPtr<FJsonObject>& D : SaEventsOfType(Rec, TEXT("thinking_delta"))) { ThinkingStreamed += SaStr(D, TEXT("text")); }
		TestEqual(Ctx + TEXT("streamed thinking round-trips exactly"), ThinkingStreamed, SaNasty);

		const TArray<TSharedPtr<FJsonObject>> Thinking = SaEventsOfType(Rec, TEXT("thinking"));
		TestEqual(Ctx + TEXT("one complete thinking event"), Thinking.Num(), 1);
		if (Thinking.Num() == 1)
		{
			TestEqual(Ctx + TEXT("complete thinking round-trips exactly"), SaStr(Thinking[0], TEXT("text")), SaNasty);
		}

		const TArray<TSharedPtr<FJsonObject>> Entries = FKismetAISessionStorage::LoadAllEntries(Cleanup.SessionId);
		const TArray<TSharedPtr<FJsonObject>> Assistant = SaEntriesOfType(Entries, TEXT("assistant_message"));
		TestEqual(Ctx + TEXT("one committed message"), Assistant.Num(), 1);
		if (Assistant.Num() == 1)
		{
			TestEqual(Ctx + TEXT("committed text equals streamed text"), SaCommittedText(Assistant[0]), SaNasty);
		}
	}
	return true;
}

// The tool name is the model's choice: a quote in it must not break the event.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamAttempt_ToolEventsAreStrictJson,
	"KismetAI.StreamAttempts.ToolEventsAreStrictJson", StreamAttemptFlags)
bool FStreamAttempt_ToolEventsAreStrictJson::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("query"), SaNasty);
	FScriptedModelClient Model({
		MakeResponse({ MakeToolUseBlock(TEXT("toolu_q"), TEXT("search\"\t_assets"), Args) }, TEXT("tool_use")),
		MakeTextTurn(TEXT("done")) });
	FScriptedToolInvoker Tools;
	// A result that is not JSON exercises the string branch of tool_result.
	Tools.SetResult(TEXT("search\"\t_assets"), FString(TEXT("plain\ttext \x01 result")));
	FEventRecorder Rec;
	FKismetAIAgent Agent(Tools, Model, MakeAttemptConfig(0));
	FAttemptSessionCleanup Cleanup;
	Agent.RunTask(TEXT("search"), Rec.MakeCallback());
	Cleanup.SessionId = Agent.GetSessionId();

	TestTrue(TEXT("every event is strict JSON"), SaAllEventsStrict(*this, Rec));
	const TArray<TSharedPtr<FJsonObject>> Calls = SaEventsOfType(Rec, TEXT("tool_call"));
	TestEqual(TEXT("one tool_call event"), Calls.Num(), 1);
	if (Calls.Num() == 1)
	{
		TestEqual(TEXT("tool name round-trips"), SaStr(Calls[0], TEXT("tool")), FString(TEXT("search\"\t_assets")));
	}
	return true;
}

// The decoder hands the stream state whole UTF-8 sequences whatever the read
// size: a character split across two reads is not decoded from half of it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStreamAttempt_SplitUtf8AcrossChunks,
	"KismetAI.StreamAttempts.SplitUtf8AcrossChunks", StreamAttemptFlags)
bool FStreamAttempt_SplitUtf8AcrossChunks::RunTest(const FString& Parameters)
{
	FScriptedAttempt A;
	A.Bytes = FAnthropicStreamBuilder()
		.MessageStart()
		.BlockStart(TEXT("text")).Text(SaNasty).BlockStop()
		.MessageEnd(TEXT("end_turn"))
		.Bytes();

	for (int32 ChunkSize : { 1, 2, 3, 4, 5, 7, 4096 })
	{
		A.ChunkSize = ChunkSize;
		FString Streamed;
		FModelCallbacks Callbacks;
		Callbacks.OnTextDelta = [&Streamed](const FString& D) { Streamed += D; };
		const FModelCallOutcome Outcome = RunAttempt(A, Callbacks, []() { return false; });
		const FString Ctx = FString::Printf(TEXT("[chunk %d] "), ChunkSize);
		TestEqual(Ctx + TEXT("attempt succeeds"), (int32)Outcome.Result, (int32)EModelCallResult::Success);
		TestEqual(Ctx + TEXT("streamed text intact"), Streamed, SaNasty);
		const FString Committed = Outcome.AssistantMessage.GetText();
		TestEqual(Ctx + TEXT("response text intact"), Committed, SaNasty);
	}
	return true;
}
