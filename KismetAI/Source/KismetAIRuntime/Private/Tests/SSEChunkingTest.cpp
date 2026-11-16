// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Does the SSE parser survive being fed in arbitrary chunks?
//
// CallLLMOnce bypasses FKismetAISSEParser::Feed with a hand-rolled line split,
// on the strength of a comment claiming Feed "has a while(true) loop that hangs
// on certain inputs". Real incremental streaming needs Feed to work, so the
// claim gets tested rather than inherited.
//
// The result: the loop does not hang — it always advances past the newline it
// found and returns when there is none. The genuine hazard was different: Feed
// ran FUTF8ToTCHAR over whatever byte range it was handed, so a multi-byte
// character split across a chunk boundary was decoded from an incomplete
// sequence. made Feed decode whole lines only; MultiByteAcrossBoundary
// pins that.

#include "KismetAISSEParser.h"
#include "Misc/AutomationTest.h"

constexpr auto SSEChunkFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	// A realistic Anthropic stream: message start, a text delta, a tool_use
	// block with partial JSON, and a stop. The text delta deliberately contains
	// a multi-byte character (an em dash) so chunk boundaries can land inside it.
	const ANSICHAR* const SampleStream =
		"event: message_start\n"
		"data: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":10,\"output_tokens\":1}}}\n"
		"\n"
		"event: content_block_start\n"
		"data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n"
		"\n"
		"event: content_block_delta\n"
		"data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Added the node \xE2\x80\x94 done.\"}}\n"
		"\n"
		"event: content_block_stop\n"
		"data: {\"type\":\"content_block_stop\",\"index\":0}\n"
		"\n"
		"event: message_delta\n"
		"data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n"
		"\n"
		"event: message_stop\n"
		"data: {\"type\":\"message_stop\"}\n"
		"\n";

	// Feed the stream in fixed-size byte chunks and collect every event.
	TArray<FKismetAISSEEvent> FeedInChunks(const ANSICHAR* Bytes, int32 ChunkSize)
	{
		FKismetAISSEParser Parser;
		TArray<FKismetAISSEEvent> All;

		const int32 Total = FCStringAnsi::Strlen(Bytes);
		for (int32 Offset = 0; Offset < Total; Offset += ChunkSize)
		{
			const int32 ThisChunk = FMath::Min(ChunkSize, Total - Offset);
			Parser.Feed(reinterpret_cast<const uint8*>(Bytes + Offset), ThisChunk);
			All.Append(Parser.DequeueEvents());
		}
		All.Append(Parser.DequeueEvents());
		return All;
	}

	FString EventTypeList(const TArray<FKismetAISSEEvent>& Events)
	{
		TArray<FString> Types;
		for (const FKismetAISSEEvent& E : Events) { Types.Add(E.EventType); }
		return FString::Join(Types, TEXT(","));
	}

	// The text carried by the first content_block_delta.
	FString FindDeltaText(const TArray<FKismetAISSEEvent>& Events)
	{
		for (const FKismetAISSEEvent& E : Events)
		{
			if (E.EventType != TEXT("content_block_delta") || !E.Data.IsValid()) { continue; }
			const TSharedPtr<FJsonObject>* Delta = nullptr;
			if (E.Data->TryGetObjectField(TEXT("delta"), Delta))
			{
				FString Text;
				if ((*Delta)->TryGetStringField(TEXT("text"), Text)) { return Text; }
			}
		}
		return FString();
	}
}

// The claim under test. If Feed hung, this would never return.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSSEChunk_FeedDoesNotHang, "KismetAI.SSEChunking.FeedDoesNotHang", SSEChunkFlags)
bool FSSEChunk_FeedDoesNotHang::RunTest(const FString& Parameters)
{
	const TArray<FKismetAISSEEvent> Whole = FeedInChunks(SampleStream, 1024 * 1024);
	TestEqual(TEXT("six events parsed from one chunk"), Whole.Num(), 6);
	TestEqual(TEXT("in stream order"), EventTypeList(Whole),
		FString(TEXT("message_start,content_block_start,content_block_delta,content_block_stop,message_delta,message_stop")));
	return true;
}

// Event framing must not depend on where chunk boundaries fall.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSSEChunk_FramingIsChunkIndependent, "KismetAI.SSEChunking.FramingIsChunkIndependent", SSEChunkFlags)
bool FSSEChunk_FramingIsChunkIndependent::RunTest(const FString& Parameters)
{
	const FString Expected = EventTypeList(FeedInChunks(SampleStream, 1024 * 1024));

	// 7 and 13 are chosen to be coprime with the line lengths, so boundaries land
	// mid-line, mid-JSON and mid-token across the stream.
	for (int32 ChunkSize : { 3, 7, 13, 64, 257 })
	{
		const TArray<FKismetAISSEEvent> Got = FeedInChunks(SampleStream, ChunkSize);
		TestEqual(*FString::Printf(TEXT("same events at chunk size %d"), ChunkSize),
			EventTypeList(Got), Expected);
	}
	return true;
}

// The real hazard: a multi-byte character straddling a read boundary. Feed used
// to decode each byte chunk on its own, so the character was decoded from an
// incomplete sequence; now it buffers bytes to whole lines first.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSSEChunk_MultiByteAcrossBoundary, "KismetAI.SSEChunking.MultiByteAcrossBoundary", SSEChunkFlags)
bool FSSEChunk_MultiByteAcrossBoundary::RunTest(const FString& Parameters)
{
	// Whole-stream decode is the reference: the em dash survives.
	const FString Reference = FindDeltaText(FeedInChunks(SampleStream, 1024 * 1024));
	TestEqual(TEXT("reference text is intact"), Reference, FString(TEXT("Added the node — done.")));

	// Byte-at-a-time is the worst case: every multi-byte sequence is split.
	for (int32 ChunkSize : { 1, 2, 3 })
	{
		TestEqual(*FString::Printf(TEXT("text intact at chunk size %d"), ChunkSize),
			FindDeltaText(FeedInChunks(SampleStream, ChunkSize)), Reference);
	}

	TestEqual(TEXT("event framing is unaffected"),
		EventTypeList(FeedInChunks(SampleStream, 1)),
		EventTypeList(FeedInChunks(SampleStream, 1024 * 1024)));
	return true;
}

// A stream cut off mid-event must not produce a bogus event or spin.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSSEChunk_TruncatedStream, "KismetAI.SSEChunking.TruncatedStream", SSEChunkFlags)
bool FSSEChunk_TruncatedStream::RunTest(const FString& Parameters)
{
	const ANSICHAR* Truncated =
		"event: message_start\n"
		"data: {\"type\":\"message_start\"}\n"
		"\n"
		"event: content_block_delta\n"
		"data: {\"type\":\"content_block_de";   // cut mid-JSON, no newline

	const TArray<FKismetAISSEEvent> Events = FeedInChunks(Truncated, 5);
	TestEqual(TEXT("only the complete event is dispatched"), Events.Num(), 1);
	TestEqual(TEXT("and it is the first one"), Events[0].EventType, FString(TEXT("message_start")));
	return true;
}

// Anthropic pads data lines with trailing whitespace and extra braces; the
// brace-counting extractor has to survive that under chunking too.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSSEChunk_PaddedDataLine, "KismetAI.SSEChunking.PaddedDataLine", SSEChunkFlags)
bool FSSEChunk_PaddedDataLine::RunTest(const FString& Parameters)
{
	const ANSICHAR* Padded =
		"event: message_delta\n"
		"data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}}      }\n"
		"\n";

	for (int32 ChunkSize : { 1, 5, 4096 })
	{
		const TArray<FKismetAISSEEvent> Events = FeedInChunks(Padded, ChunkSize);
		if (Events.Num() != 1 || !Events[0].Data.IsValid())
		{
			AddError(FString::Printf(TEXT("padded line failed to parse at chunk size %d"), ChunkSize));
			continue;
		}
		const TSharedPtr<FJsonObject>* Delta = nullptr;
		FString StopReason;
		if (Events[0].Data->TryGetObjectField(TEXT("delta"), Delta))
		{
			(*Delta)->TryGetStringField(TEXT("stop_reason"), StopReason);
		}
		TestEqual(*FString::Printf(TEXT("stop_reason survives padding at chunk %d"), ChunkSize),
			StopReason, FString(TEXT("end_turn")));
	}
	return true;
}
