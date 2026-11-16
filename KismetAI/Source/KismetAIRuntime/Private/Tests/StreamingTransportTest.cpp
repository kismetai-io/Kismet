// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// The streaming path over the fixture corpus.
//
// The property that matters, and the one the whole task exists for: deltas are
// delivered WHILE the response is arriving, not after it has finished. Before
// The transport read the whole SSE body from disk after curl exited, so a
// long reasoning turn showed nothing for its entire duration and then everything
// at once.
//
// Asserting that here does not need a subprocess — only that feeding the parser
// half a stream produces callbacks before the other half exists.

#include "KismetAISSEParser.h"
#include "KismetAILLMProvider.h"
#include "Fixtures/SSEFixtures.h"
#include "Misc/AutomationTest.h"

constexpr auto StreamFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	// Feed bytes to a parser and route complete events into a provider stream
	// state, collecting text deltas — the same wiring FCurlModelClient uses.
	struct FStreamHarness
	{
		FKismetAISSEParser Parser;
		TSharedPtr<ILLMStreamState> State;
		TArray<FString> TextDeltas;

		FStreamHarness()
		{
			TUniquePtr<ILLMProvider> Provider = MakeProviderForModel(TEXT("claude-sonnet-4-6"));
			State = Provider->CreateStreamState();
		}

		void Feed(const ANSICHAR* Bytes, int32 Offset, int32 Count)
		{
			Parser.Feed(reinterpret_cast<const uint8*>(Bytes + Offset), Count);
			for (const FKismetAISSEEvent& Event : Parser.DequeueEvents())
			{
				if (!Event.Data.IsValid()) { continue; }
				FString Delta;
				if (State->ProcessEvent(Event.EventType, Event.Data, Delta))
				{
					TextDeltas.Add(Delta);
				}
			}
		}

		void FeedAll(const ANSICHAR* Bytes)
		{
			Feed(Bytes, 0, FCStringAnsi::Strlen(Bytes));
		}

		FString JoinedText() const { return FString::Join(TextDeltas, TEXT("")); }
	};
}

// THE test for this task: text is delivered before the stream is complete.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStream_DeltasArriveBeforeTheEnd, "KismetAI.Streaming.DeltasArriveBeforeTheEnd", StreamFlags)
bool FStream_DeltasArriveBeforeTheEnd::RunTest(const FString& Parameters)
{
	const ANSICHAR* Bytes = KismetAISSEFixtures::TextOnly();
	const int32 Total = FCStringAnsi::Strlen(Bytes);

	FStreamHarness H;

	// Feed only as far as the first text delta's terminating blank line.
	const ANSICHAR* FirstDeltaEnd = FCStringAnsi::Strstr(Bytes, "The graph ");
	if (!FirstDeltaEnd)
	{
		AddError(TEXT("fixture changed; first delta not found"));
		return false;
	}
	const int32 UpTo = static_cast<int32>(FirstDeltaEnd - Bytes) + 40;   // past the event's blank line
	H.Feed(Bytes, 0, FMath::Min(UpTo, Total));

	TestTrue(TEXT("text was delivered while the stream was still open"), H.TextDeltas.Num() > 0);
	TestTrue(TEXT("and it is the first chunk, not the whole answer"),
		H.JoinedText().Contains(TEXT("The graph")) && !H.JoinedText().Contains(TEXT("already has")));

	// Feed the rest; the remaining text arrives.
	H.Feed(Bytes, FMath::Min(UpTo, Total), Total - FMath::Min(UpTo, Total));
	TestEqual(TEXT("the full answer assembles"),
		H.JoinedText(), FString(TEXT("The graph already has that node.")));
	return true;
}

// Chunk size must not change the outcome — the transport reads whatever curl
// has written, which is an arbitrary boundary.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStream_ChunkSizeDoesNotChangeResult, "KismetAI.Streaming.ChunkSizeDoesNotChangeResult", StreamFlags)
bool FStream_ChunkSizeDoesNotChangeResult::RunTest(const FString& Parameters)
{
	auto Replay = [](const ANSICHAR* Bytes, int32 ChunkSize)
	{
		FStreamHarness H;
		const int32 Total = FCStringAnsi::Strlen(Bytes);
		for (int32 Off = 0; Off < Total; Off += ChunkSize)
		{
			H.Feed(Bytes, Off, FMath::Min(ChunkSize, Total - Off));
		}
		return H.JoinedText();
	};

	const ANSICHAR* Bytes = KismetAISSEFixtures::TextOnly();
	const FString Whole = Replay(Bytes, 1024 * 1024);
	TestEqual(TEXT("baseline"), Whole, FString(TEXT("The graph already has that node.")));

	for (int32 Size : { 1, 3, 17, 128 })
	{
		TestEqual(*FString::Printf(TEXT("same text at chunk size %d"), Size), Replay(Bytes, Size), Whole);
	}
	return true;
}

// Thinking signatures must survive the streaming assembly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStream_ThinkingSignatureSurvives, "KismetAI.Streaming.ThinkingSignatureSurvives", StreamFlags)
bool FStream_ThinkingSignatureSurvives::RunTest(const FString& Parameters)
{
	FStreamHarness H;
	H.FeedAll(KismetAISSEFixtures::WithThinking());

	const TSharedPtr<FJsonObject> Response = H.State->BuildResponseJson();
	if (!Response.IsValid())
	{
		AddError(TEXT("no response assembled"));
		return false;
	}

	FString Serialized;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
	FJsonSerializer::Serialize(Response.ToSharedRef(), Writer);

	TestTrue(TEXT("signature preserved"), Serialized.Contains(TEXT("SIGabc123")));
	TestTrue(TEXT("thinking text preserved"), Serialized.Contains(TEXT("check the pins")));
	TestTrue(TEXT("and the visible answer too"), Serialized.Contains(TEXT("Done.")));
	return true;
}

// tool_use input arrives as partial JSON across deltas and must reassemble.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStream_ToolUseInputReassembles, "KismetAI.Streaming.ToolUseInputReassembles", StreamFlags)
bool FStream_ToolUseInputReassembles::RunTest(const FString& Parameters)
{
	// Byte-at-a-time is the worst case for reassembling partial JSON.
	FStreamHarness H;
	const ANSICHAR* Bytes = KismetAISSEFixtures::WithToolUse();
	const int32 Total = FCStringAnsi::Strlen(Bytes);
	for (int32 i = 0; i < Total; ++i)
	{
		H.Feed(Bytes, i, 1);
	}

	const TSharedPtr<FJsonObject> Response = H.State->BuildResponseJson();
	if (!Response.IsValid())
	{
		AddError(TEXT("no response assembled"));
		return false;
	}

	FString Serialized;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
	FJsonSerializer::Serialize(Response.ToSharedRef(), Writer);

	TestTrue(TEXT("tool name survives"), Serialized.Contains(TEXT("read_blueprint")));
	TestTrue(TEXT("tool id survives"), Serialized.Contains(TEXT("toolu_9")));
	TestTrue(TEXT("the split JSON input reassembled"), Serialized.Contains(TEXT("/Game/BP_Player")));

	FString StopReason;
	Response->TryGetStringField(TEXT("stop_reason"), StopReason);
	TestEqual(TEXT("stop_reason is tool_use"), StopReason, FString(TEXT("tool_use")));
	return true;
}

// A non-SSE error body must produce no events rather than garbage — the error
// path reads the raw buffer instead.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStream_ErrorBodyYieldsNoEvents, "KismetAI.Streaming.ErrorBodyYieldsNoEvents", StreamFlags)
bool FStream_ErrorBodyYieldsNoEvents::RunTest(const FString& Parameters)
{
	FStreamHarness H;
	H.FeedAll(KismetAISSEFixtures::ErrorBody());

	TestEqual(TEXT("no text deltas from an error body"), H.TextDeltas.Num(), 0);
	TestTrue(TEXT("but the raw body is retained for classification"),
		H.Parser.GetRawBuffer().Contains(TEXT("overloaded_error")));
	return true;
}

// A stream cut mid-event must not emit a partial event or spin.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStream_TruncatedMidEvent, "KismetAI.Streaming.TruncatedMidEvent", StreamFlags)
bool FStream_TruncatedMidEvent::RunTest(const FString& Parameters)
{
	FStreamHarness H;
	H.FeedAll(KismetAISSEFixtures::TruncatedMidEvent());

	// message_start carries no text, and the cut delta never completes.
	TestEqual(TEXT("no text delivered from an incomplete delta"), H.TextDeltas.Num(), 0);
	return true;
}
