// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// A single parsed SSE event.
//
// Provider-neutral despite the historical name this file used to carry: this is
// the line parser for EVERY provider. OpenAI and MiniMax reach it through
// ILLMProvider::CreateStreamState the same way Anthropic does — only the
// per-event interpretation differs, which is what ILLMStreamState owns.
struct FKismetAISSEEvent
{
	FString EventType;              // "message_start", "content_block_delta", etc.
	TSharedPtr<FJsonObject> Data;   // Parsed JSON from the data: line
};

// ─────────────────────────────────────────────────────────────────────────────
// SSE line parser — converts raw bytes into FKismetAISSEEvent structs.
// Shared by all providers; see the note above.
// ─────────────────────────────────────────────────────────────────────────────
class FKismetAISSEParser
{
public:
	// Append raw bytes from the network. Parses complete SSE events internally.
	//
	// Bytes are buffered until a line is complete and only whole lines are
	// decoded. A read can end anywhere — inside a multi-byte UTF-8 sequence
	// included — and decoding each read on its own turned a character split
	// across two reads into garbage in both the streamed and the committed text
	//. '\n' never occurs inside a multi-byte sequence, so a line
	// boundary is always a character boundary.
	//
	// UTF-8 decoding must use the length-taking FString constructor:
	// FUTF8ToTCHAR built from an explicit source length does NOT null-terminate
	// its output, so FString(Converter.Get()) reads past the end of the
	// converted buffer. Covered by KismetAI.SSEChunking.FramingIsChunkIndependent.
	void Feed(const uint8* Data, int64 Length)
	{
		if (Length <= 0) { return; }
		RawBytes.Append(Data, Length);
		PendingBytes.Append(Data, Length);

		int32 LastNewline = INDEX_NONE;
		for (int32 i = PendingBytes.Num() - 1; i >= 0; --i)
		{
			if (PendingBytes[i] == '\n') { LastNewline = i; break; }
		}
		if (LastNewline == INDEX_NONE) { return; }

		Buffer.Append(DecodeUtf8(PendingBytes.GetData(), LastNewline + 1));
		PendingBytes.RemoveAt(0, LastNewline + 1, EAllowShrinking::No);
		ProcessLines();
	}

	// Return and clear all parsed events since last call.
	TArray<FKismetAISSEEvent> DequeueEvents()
	{
		TArray<FKismetAISSEEvent> Out = MoveTemp(PendingEvents);
		PendingEvents.Reset();
		return Out;
	}

	// Every byte received so far, decoded (for the error-response fallback when
	// status != 200; a body that is not SSE may never contain a newline).
	FString GetRawBuffer() const { return DecodeUtf8(RawBytes.GetData(), RawBytes.Num()); }

private:
	static FString DecodeUtf8(const uint8* Bytes, int32 Count)
	{
		FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Bytes), Count);
		return FString(Converter.Length(), Converter.Get());
	}

	TArray<uint8> PendingBytes;             // Bytes of the incomplete trailing line
	TArray<uint8> RawBytes;                 // All bytes received (for error fallback)
	FString Buffer;                         // Decoded, not yet split into lines
	TArray<FKismetAISSEEvent> PendingEvents;

	// Current SSE event being assembled (may span multiple lines)
	FString CurrentEventType;
	FString CurrentDataLine;

	void ProcessLines()
	{
		// SSE lines are delimited by \n. We process all complete lines
		// and leave any trailing partial line in Buffer.
		//
		// The newline scan is manual rather than FString::Find(..., SearchFrom).
		// Find clamps SearchFrom to Len()-1, so once every line has been consumed
		// and SearchFrom == Len(), it searches from the LAST character instead of
		// past the end. When the buffer ends in \n — which it does after any
		// complete SSE event — it re-finds that same trailing newline, SearchFrom
		// is set back to Len(), and the loop spins forever.
		//
		// That is the hang the comment in CallLLMOnce warned about, and it is why
		// the transport hand-rolled its own line split instead of using Feed.
		// Covered by KismetAI.SSEChunking.FeedDoesNotHang.
		auto FindNewlineFrom = [this](int32 From) -> int32
		{
			for (int32 i = From; i < Buffer.Len(); ++i)
			{
				if (Buffer[i] == TEXT('\n')) { return i; }
			}
			return INDEX_NONE;
		};

		int32 SearchFrom = 0;
		while (true)
		{
			const int32 NewlineIdx = FindNewlineFrom(SearchFrom);
			if (NewlineIdx == INDEX_NONE)
			{
				// No more complete lines — keep remainder
				if (SearchFrom > 0)
				{
					Buffer.RightChopInline(SearchFrom);
				}
				return;
			}

			// Extract line (without the \n)
			FString Line = Buffer.Mid(SearchFrom, NewlineIdx - SearchFrom);
			// Strip trailing \r if present (handles \r\n)
			if (Line.Len() > 0 && Line[Line.Len() - 1] == TEXT('\r'))
			{
				Line.LeftChopInline(1);
			}
			SearchFrom = NewlineIdx + 1;

			// Process the line per SSE spec
			if (Line.IsEmpty())
			{
				// Empty line = event dispatch boundary
				DispatchEvent();
			}
			else if (Line.StartsWith(TEXT("event: ")))
			{
				CurrentEventType = Line.Mid(7);
			}
			else if (Line.StartsWith(TEXT("data: ")))
			{
				// SSE spec: multiple data lines are joined with \n
				if (!CurrentDataLine.IsEmpty())
				{
					CurrentDataLine.Append(TEXT("\n"));
				}
				// Anthropic SSE pads data lines with trailing whitespace and
				// sometimes extra braces. Trim to get valid JSON.
				FString DataContent = Line.Mid(6).TrimEnd();
				CurrentDataLine.Append(DataContent);
			}
			else if (Line[0] == TEXT(':'))
			{
				// Comment line — ignore
			}
		}
	}

public:
	// Extract the first top-level JSON object from a data line.
	// Anthropic SSE pads lines with trailing spaces and extra braces
	// (e.g., '{"type":"message_start",...}}        }')
	// which makes the raw line invalid JSON. We find where the first
	// complete object ends by brace counting.
	static FString ExtractJsonObject(const FString& Raw)
	{
		int32 Depth = 0;
		bool bInString = false;
		bool bEscape = false;
		for (int32 i = 0; i < Raw.Len(); i++)
		{
			TCHAR C = Raw[i];
			if (bEscape) { bEscape = false; continue; }
			if (C == TEXT('\\') && bInString) { bEscape = true; continue; }
			if (C == TEXT('"')) { bInString = !bInString; continue; }
			if (bInString) continue;
			if (C == TEXT('{')) Depth++;
			else if (C == TEXT('}'))
			{
				Depth--;
				if (Depth == 0) return Raw.Left(i + 1);
			}
		}
		return Raw; // Fallback: return as-is
	}

	void DispatchEvent()
	{
		if (CurrentDataLine.IsEmpty() && CurrentEventType.IsEmpty())
		{
			return; // Spurious empty line
		}

		FKismetAISSEEvent Event;
		Event.EventType = CurrentEventType;

		// Parse JSON data
		if (!CurrentDataLine.IsEmpty())
		{
			FString CleanJson = ExtractJsonObject(CurrentDataLine);
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(CleanJson);
			TSharedPtr<FJsonObject> JsonObj;
			if (FJsonSerializer::Deserialize(Reader, JsonObj))
			{
				Event.Data = JsonObj;
			}
		}

		PendingEvents.Add(MoveTemp(Event));

		// Reset for next event
		CurrentEventType.Reset();
		CurrentDataLine.Reset();
	}
};

// ─────────────────────────────────────────────────────────────────────────────
// Stream state accumulator — processes SSE events into the equivalent of a
// non-streaming Messages API response.
// ─────────────────────────────────────────────────────────────────────────────
class FAnthropicStreamState
{
public:
	bool bComplete = false;
	bool bMalformedToolInput = false;

	// Callback for thinking deltas (separate from text deltas)
	using FOnThinkingDelta = TFunction<void(const FString& ThinkingText)>;
	FOnThinkingDelta OnThinkingDelta;

	// Callback when a complete thinking block is finalized (fires before text starts)
	using FOnThinkingComplete = TFunction<void(const FString& FullThinkingText)>;
	FOnThinkingComplete OnThinkingComplete;

	// Process a single SSE event. Returns true if this was a text_delta
	// (caller can use DeltaText to forward to UI).
	bool ProcessEvent(const FKismetAISSEEvent& Event, FString& OutDeltaText)
	{
		OutDeltaText.Reset();

		if (!Event.Data.IsValid()) return false;

		const FString& Type = Event.EventType;

		if (Type == TEXT("message_start"))
		{
			// Extract usage from message_start.message.usage
			const TSharedPtr<FJsonObject>* MsgObj;
			if (Event.Data->TryGetObjectField(TEXT("message"), MsgObj))
			{
				const TSharedPtr<FJsonObject>* UsageObj;
				if ((*MsgObj)->TryGetObjectField(TEXT("usage"), UsageObj))
				{
					bHasInputUsage = (*UsageObj)->TryGetNumberField(TEXT("input_tokens"), InputTokens);
					(*UsageObj)->TryGetNumberField(TEXT("output_tokens"), OutputTokens);
					(*UsageObj)->TryGetNumberField(TEXT("cache_read_input_tokens"), CacheReadTokens);
					(*UsageObj)->TryGetNumberField(TEXT("cache_creation_input_tokens"), CacheWriteTokens);
				}
			}
		}
		else if (Type == TEXT("content_block_start"))
		{
			InitialBlock.Reset();
			CurrentBlockType.Reset();
			AccumulatedText.Reset();
			AccumulatedToolJson.Reset();
			AccumulatedThinkingText.Reset();
			AccumulatedThinkingSignature.Reset();
			const TSharedPtr<FJsonObject>* BlockObj;
			if (Event.Data->TryGetObjectField(TEXT("content_block"), BlockObj))
			{
				InitialBlock = MakeShared<FJsonObject>();
				FJsonObject::Duplicate(*BlockObj, InitialBlock);
				CurrentBlockType = (*BlockObj)->GetStringField(TEXT("type"));
				(*BlockObj)->TryGetStringField(TEXT("text"), AccumulatedText);
				(*BlockObj)->TryGetStringField(TEXT("thinking"), AccumulatedThinkingText);
				(*BlockObj)->TryGetStringField(TEXT("signature"), AccumulatedThinkingSignature);
				if (CurrentBlockType == TEXT("tool_use"))
				{
					ToolUseId = (*BlockObj)->GetStringField(TEXT("id"));
					ToolUseName = (*BlockObj)->GetStringField(TEXT("name"));
				}
			}
		}
		else if (Type == TEXT("content_block_delta"))
		{
			const TSharedPtr<FJsonObject>* DeltaObj;
			if (Event.Data->TryGetObjectField(TEXT("delta"), DeltaObj))
			{
				FString DeltaType = (*DeltaObj)->GetStringField(TEXT("type"));
				if (DeltaType == TEXT("text_delta"))
				{
					FString Text = (*DeltaObj)->GetStringField(TEXT("text"));
					AccumulatedText.Append(Text);
					OutDeltaText = Text;
					return true;
				}
				else if (DeltaType == TEXT("input_json_delta"))
				{
					FString Partial = (*DeltaObj)->GetStringField(TEXT("partial_json"));
					AccumulatedToolJson.Append(Partial);
				}
				else if (DeltaType == TEXT("thinking_delta"))
				{
					FString ThinkingText = (*DeltaObj)->GetStringField(TEXT("thinking"));
					AccumulatedThinkingText.Append(ThinkingText);
					if (OnThinkingDelta)
					{
						OnThinkingDelta(ThinkingText);
					}
				}
				else if (DeltaType == TEXT("signature_delta"))
				{
					FString Sig = (*DeltaObj)->GetStringField(TEXT("signature"));
					AccumulatedThinkingSignature = Sig;
				}
			}
		}
		else if (Type == TEXT("content_block_stop"))
		{
			FinalizeCurrentBlock();
		}
		else if (Type == TEXT("message_delta"))
		{
			const TSharedPtr<FJsonObject>* DeltaObj;
			if (Event.Data->TryGetObjectField(TEXT("delta"), DeltaObj))
			{
				(*DeltaObj)->TryGetStringField(TEXT("stop_reason"), StopReason);
			}
			const TSharedPtr<FJsonObject>* UsageObj;
			if (Event.Data->TryGetObjectField(TEXT("usage"), UsageObj))
			{
				bHasFinalOutputUsage = (*UsageObj)->TryGetNumberField(TEXT("output_tokens"), OutputTokens);
			}
		}
		else if (Type == TEXT("message_stop"))
		{
			bComplete = true;
		}

		return false;
	}

	// Build a response JSON equivalent to the non-streaming API response
	TSharedPtr<FJsonObject> BuildResponseJson() const
	{
		TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetArrayField(TEXT("content"), ContentBlocks);
		Response->SetStringField(TEXT("stop_reason"), StopReason);
		Response->SetBoolField(TEXT("stream_complete"), bComplete);
		Response->SetBoolField(TEXT("usage_complete"), bComplete && bHasInputUsage && bHasFinalOutputUsage);
		if (!bHasInputUsage && !bHasFinalOutputUsage) return Response;

		TSharedPtr<FJsonObject> Usage = MakeShared<FJsonObject>();
		Usage->SetNumberField(TEXT("input_tokens"), InputTokens);
		Usage->SetNumberField(TEXT("output_tokens"), OutputTokens);
		Usage->SetNumberField(TEXT("cache_read_input_tokens"), CacheReadTokens);
		Usage->SetNumberField(TEXT("cache_creation_input_tokens"), CacheWriteTokens);
		Response->SetObjectField(TEXT("usage"), Usage);

		return Response;
	}

private:
	// Accumulated content blocks (equivalent to response.content array)
	TArray<TSharedPtr<FJsonValue>> ContentBlocks;

	// Per-block accumulation
	TSharedPtr<FJsonObject> InitialBlock;
	FString CurrentBlockType;
	FString AccumulatedText;
	FString AccumulatedToolJson;
	FString ToolUseId;
	FString ToolUseName;
	FString AccumulatedThinkingText;
	FString AccumulatedThinkingSignature;

	// Message-level state
	FString StopReason;
	int32 InputTokens = 0;
	int32 OutputTokens = 0;
	int32 CacheReadTokens = 0;
	int32 CacheWriteTokens = 0;
	bool bHasInputUsage = false;
	bool bHasFinalOutputUsage = false;

	void FinalizeCurrentBlock()
	{
		if (!InitialBlock.IsValid()) { return; }
		TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
		FJsonObject::Duplicate(InitialBlock, Block);

		if (CurrentBlockType == TEXT("text"))
		{
			Block->SetStringField(TEXT("type"), TEXT("text"));
			Block->SetStringField(TEXT("text"), AccumulatedText);
		}
		else if (CurrentBlockType == TEXT("tool_use"))
		{
			Block->SetStringField(TEXT("type"), TEXT("tool_use"));
			Block->SetStringField(TEXT("id"), ToolUseId);
			Block->SetStringField(TEXT("name"), ToolUseName);

			// Parse the accumulated JSON string into an object
			TSharedPtr<FJsonObject> InputObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(AccumulatedToolJson);
			if (FJsonSerializer::Deserialize(Reader, InputObj) && InputObj.IsValid())
			{
				Block->SetObjectField(TEXT("input"), InputObj);
			}
			else
			{
				if (!AccumulatedToolJson.IsEmpty())
				{
					bMalformedToolInput = true;
					return;
				}
				// No deltas: the initial object is authoritative, including nonempty
				// arguments. A malformed initial shape must not become an empty call.
				const TSharedPtr<FJsonObject>* InitialInput = nullptr;
				if (!Block->TryGetObjectField(TEXT("input"), InitialInput))
				{
					bMalformedToolInput = true;
					return;
				}
			}
		}
		else if (CurrentBlockType == TEXT("thinking"))
		{
			Block->SetStringField(TEXT("type"), TEXT("thinking"));
			Block->SetStringField(TEXT("thinking"), AccumulatedThinkingText);
			if (!AccumulatedThinkingSignature.IsEmpty())
			{
				Block->SetStringField(TEXT("signature"), AccumulatedThinkingSignature);
			}

			// Fire callback so the agent can emit the thinking event before text starts
			if (OnThinkingComplete)
			{
				OnThinkingComplete(AccumulatedThinkingText);
			}
		}

		ContentBlocks.Add(MakeShared<FJsonValueObject>(Block));

		// Reset per-block state
		InitialBlock.Reset();
		CurrentBlockType.Reset();
		AccumulatedText.Reset();
		AccumulatedToolJson.Reset();
		ToolUseId.Reset();
		ToolUseName.Reset();
		AccumulatedThinkingText.Reset();
		AccumulatedThinkingSignature.Reset();
	}
};
