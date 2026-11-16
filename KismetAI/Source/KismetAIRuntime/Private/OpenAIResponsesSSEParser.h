// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "KismetAILLMProvider.h"
#include "KismetAILog.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Log category defined in KismetAIOpenAIProvider.cpp (the sole non-test includer)
DECLARE_LOG_CATEGORY_EXTERN(LogKismetAIOpenAIStream, Log, All);

// ─────────────────────────────────────────────────────────────────────────────
// OpenAI Responses API SSE event-name constants.
//
// The text / function-call event names are confirmed from the OpenAI docs.
//
// *** UNVERIFIED: The reasoning-summary event names below are inferred
//     from the "summary: auto" parameter. They have NOT been confirmed via live
//     capture. If reasoning-summary events are not handled, update:
//       OpenAI_Evt_ReasoningSummaryDelta
//       OpenAI_Evt_ReasoningSummaryDone
//     One-line change here fixes it everywhere. ***
// ─────────────────────────────────────────────────────────────────────────────

// Text streaming
static const TCHAR* OpenAI_Evt_OutputTextDelta  = TEXT("response.output_text.delta");
static const TCHAR* OpenAI_Evt_OutputTextDone   = TEXT("response.output_text.done");

// Function-call streaming
static const TCHAR* OpenAI_Evt_OutputItemAdded  = TEXT("response.output_item.added");
static const TCHAR* OpenAI_Evt_FuncArgsDelta    = TEXT("response.function_call_arguments.delta");
static const TCHAR* OpenAI_Evt_FuncArgsDone     = TEXT("response.function_call_arguments.done");
static const TCHAR* OpenAI_Evt_OutputItemDone   = TEXT("response.output_item.done");

// Terminal events
static const TCHAR* OpenAI_Evt_Completed        = TEXT("response.completed");
static const TCHAR* OpenAI_Evt_Incomplete       = TEXT("response.incomplete");
static const TCHAR* OpenAI_Evt_Failed           = TEXT("response.failed");
static const TCHAR* OpenAI_Evt_Error            = TEXT("error");

// Reasoning summary (UNVERIFIED event names, may need correction after live capture)
static const TCHAR* OpenAI_Evt_ReasoningSummaryDelta = TEXT("response.reasoning_summary_text.delta");
static const TCHAR* OpenAI_Evt_ReasoningSummaryDone  = TEXT("response.reasoning_summary_text.done");

// ─────────────────────────────────────────────────────────────────────────────
// FOpenAIStreamState — ILLMStreamState implementation for OpenAI Responses API.
//
// Accumulates SSE events and builds the canonical Anthropic-shaped response
// JSON that the shared agent loop expects:
//   {
//     "content": [ {"type":"text","text":"..."} | {"type":"tool_use","id":"...","name":"...","input":{...}} | {"type":"thinking","thinking":"..."} ],
//     "stop_reason": "end_turn" | "tool_use" | "max_tokens",
//     "usage": {"input_tokens":N,"output_tokens":N,"cache_read_input_tokens":N,"cache_creation_input_tokens":0}
//   }
//
// Implementation notes:
//   - call_id (NOT item id) is used as the canonical tool_use.id (RK2).
//   - output_item.done is authoritative for arguments (beats delta accumulation).
//   - Malformed arguments reject the entire response before tool dispatch.
//   - Malformed events are logged and skipped (no silent fallback fabrication).
// ─────────────────────────────────────────────────────────────────────────────
class FOpenAIStreamState : public ILLMStreamState
{
public:
	FOpenAIStreamState() = default;

	// ILLMStreamState
	virtual bool ProcessEvent(
		const FString& EventType,
		const TSharedPtr<FJsonObject>& Data,
		FString& OutDelta) override;

	virtual TSharedPtr<FJsonObject> BuildResponseJson() const override;

	virtual bool HasStreamError(FString& OutMessage, FString& OutCode) const override
	{
		if (!bStreamError)
		{
			return false;
		}
		OutMessage = StreamErrorMessage;
		OutCode = StreamErrorCode;
		return true;
	}

private:
	// ── Per-item accumulation ─────────────────────────────────────────────────

	// The currently accumulating text delta (keyed by item_id, but we only
	// handle one item at a time; a more robust impl would key by item_id).
	FString CurrentTextItemId;
	FString AccumulatedText;

	// The currently accumulating function call (by item_id from output_item.added)
	FString CurrentFuncItemId;
	FString CurrentFuncCallId;
	FString CurrentFuncName;
	FString AccumulatedFuncArgs;

	// Reasoning summary accumulation (event names may need correction)
	FString AccumulatedReasoningText;

	// ── Finalized content blocks (canonical) ─────────────────────────────────
	TArray<TSharedPtr<FJsonValue>> ContentBlocks;

	// Track whether any function_call was finalized (drives stop_reason)
	bool bHasFunctionCall = false;

	// ── Response-level state ──────────────────────────────────────────────────
	FString StopReason;             // "end_turn", "tool_use", or "max_tokens"

	int32 InputTokens = 0;
	int32 OutputTokens = 0;
	int32 CacheReadTokens = 0;
	bool bComplete = false;
	bool bHasInputUsage = false;
	bool bHasOutputUsage = false;

	// ── In-stream error state (response.failed / error events on a 200 stream) ─
	bool bStreamError = false;
	FString StreamErrorMessage;
	FString StreamErrorCode;

	// Extract the upstream error message/code from a terminal failure event.
	// response.failed nests it under response.error.{code,message}; the bare
	// `error` event carries {code,message} at the root (or under "error").
	void ExtractStreamError(const TSharedPtr<FJsonObject>& EventData)
	{
		auto ReadFrom = [this](const TSharedPtr<FJsonObject>& Obj) -> bool
		{
			if (!Obj.IsValid())
			{
				return false;
			}
			FString Msg;
			FString Code;
			const bool bHasMsg = Obj->TryGetStringField(TEXT("message"), Msg);
			Obj->TryGetStringField(TEXT("code"), Code);
			if (bHasMsg && !Msg.IsEmpty())
			{
				StreamErrorMessage = Msg;
			}
			if (!Code.IsEmpty())
			{
				StreamErrorCode = Code;
			}
			return bHasMsg || !Code.IsEmpty();
		};

		// response.failed -> response.error
		const TSharedPtr<FJsonObject>* ResponseObjPtr = nullptr;
		if (EventData->TryGetObjectField(TEXT("response"), ResponseObjPtr))
		{
			const TSharedPtr<FJsonObject>* ErrObjPtr = nullptr;
			if ((*ResponseObjPtr)->TryGetObjectField(TEXT("error"), ErrObjPtr))
			{
				ReadFrom(*ErrObjPtr);
			}
		}

		// error event -> nested "error" object, then root fields
		const TSharedPtr<FJsonObject>* TopErrPtr = nullptr;
		if (StreamErrorMessage.IsEmpty() && EventData->TryGetObjectField(TEXT("error"), TopErrPtr))
		{
			ReadFrom(*TopErrPtr);
		}
		if (StreamErrorMessage.IsEmpty())
		{
			ReadFrom(EventData);
		}
	}

	// ── Helpers ───────────────────────────────────────────────────────────────

	// Parse the usage sub-object from a response.completed / response.incomplete event.
	// Expected shape: response.usage.{input_tokens, output_tokens, input_tokens_details.cached_tokens}
	void ExtractUsage(const TSharedPtr<FJsonObject>& EventData)
	{
		const TSharedPtr<FJsonObject>* ResponseObjPtr;
		if (!EventData->TryGetObjectField(TEXT("response"), ResponseObjPtr))
		{
			return;
		}
		const TSharedPtr<FJsonObject>& ResponseObj = *ResponseObjPtr;

		const TSharedPtr<FJsonObject>* UsageObjPtr;
		if (!ResponseObj->TryGetObjectField(TEXT("usage"), UsageObjPtr))
		{
			return;
		}
		const TSharedPtr<FJsonObject>& UsageObj = *UsageObjPtr;

		bHasInputUsage = UsageObj->TryGetNumberField(TEXT("input_tokens"), InputTokens);
		bHasOutputUsage = UsageObj->TryGetNumberField(TEXT("output_tokens"), OutputTokens);

		const TSharedPtr<FJsonObject>* DetailsPtr;
		if (UsageObj->TryGetObjectField(TEXT("input_tokens_details"), DetailsPtr))
		{
			(*DetailsPtr)->TryGetNumberField(TEXT("cached_tokens"), CacheReadTokens);
		}
	}

	// Finalize a text block into ContentBlocks.
	// Called on response.output_item.done for message items.
	void FinalizeTextBlock()
	{
		if (AccumulatedText.IsEmpty())
		{
			// Nothing to finalize
			AccumulatedText.Reset();
			CurrentTextItemId.Reset();
			return;
		}

		TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
		Block->SetStringField(TEXT("type"), TEXT("text"));
		Block->SetStringField(TEXT("text"), AccumulatedText);
		ContentBlocks.Add(MakeShared<FJsonValueObject>(Block));

		AccumulatedText.Reset();
		CurrentTextItemId.Reset();
	}

	// Finalize a function_call block into ContentBlocks using the authoritative
	// arguments string from output_item.done (or the accumulated delta string
	// if done event carries empty arguments).
	void FinalizeFunctionCallBlock(
		const FString& CallId,
		const FString& FuncName,
		const FString& ArgumentsStr)
	{
		TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
		Block->SetStringField(TEXT("type"), TEXT("tool_use"));
		Block->SetStringField(TEXT("id"), CallId);
		Block->SetStringField(TEXT("name"), FuncName);

		// Parse the arguments JSON string into an object
		TSharedPtr<FJsonObject> InputObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsStr);
		if (FJsonSerializer::Deserialize(Reader, InputObj) && InputObj.IsValid())
		{
			Block->SetObjectField(TEXT("input"), InputObj);
		}
		else
		{
			bStreamError = true;
			StreamErrorCode = TEXT("malformed_tool_arguments");
			StreamErrorMessage = TEXT("The model returned incomplete or invalid tool arguments. No tools from this response were executed. Retry with a smaller task.");
			return;
		}

		ContentBlocks.Add(MakeShared<FJsonValueObject>(Block));
		bHasFunctionCall = true;

		// Reset per-item state
		CurrentFuncItemId.Reset();
		CurrentFuncCallId.Reset();
		CurrentFuncName.Reset();
		AccumulatedFuncArgs.Reset();
	}
};

// ─────────────────────────────────────────────────────────────────────────────
// ProcessEvent implementation
// ─────────────────────────────────────────────────────────────────────────────
inline bool FOpenAIStreamState::ProcessEvent(
	const FString& EventType,
	const TSharedPtr<FJsonObject>& Data,
	FString& OutDelta)
{
	OutDelta.Reset();

	if (!Data.IsValid())
	{
		KISMETAI_LOG(LogKismetAIOpenAIStream, Warning,
			TEXT("FOpenAIStreamState::ProcessEvent: null Data for event '%s' — skipping"), *EventType);
		return false;
	}

	// ── Text delta (streaming) ────────────────────────────────────────────────
	if (EventType == OpenAI_Evt_OutputTextDelta)
	{
		FString Delta;
		if (!Data->TryGetStringField(TEXT("delta"), Delta))
		{
			KISMETAI_LOG(LogKismetAIOpenAIStream, Warning,
				TEXT("FOpenAIStreamState: response.output_text.delta missing 'delta' field — skipping"));
			return false;
		}

		// Track which item is accumulating
		FString ItemId;
		Data->TryGetStringField(TEXT("item_id"), ItemId);
		if (CurrentTextItemId.IsEmpty())
		{
			CurrentTextItemId = ItemId;
		}

		AccumulatedText.Append(Delta);
		OutDelta = Delta;
		return true;  // UI-visible text delta
	}

	// ── Text done (finalize text via this event or output_item.done) ──────────
	if (EventType == OpenAI_Evt_OutputTextDone)
	{
		// output_text.done fires before output_item.done. We finalize here only
		// if we have accumulated text. output_item.done for message items will
		// also call FinalizeTextBlock but that's idempotent (empty guard).
		FinalizeTextBlock();
		return false;
	}

	// ── Function call begins ──────────────────────────────────────────────────
	if (EventType == OpenAI_Evt_OutputItemAdded)
	{
		const TSharedPtr<FJsonObject>* ItemPtr;
		if (!Data->TryGetObjectField(TEXT("item"), ItemPtr))
		{
			KISMETAI_LOG(LogKismetAIOpenAIStream, Warning,
				TEXT("FOpenAIStreamState: response.output_item.added missing 'item' — skipping"));
			return false;
		}
		const TSharedPtr<FJsonObject>& Item = *ItemPtr;

		FString ItemType;
		Item->TryGetStringField(TEXT("type"), ItemType);
		if (ItemType != TEXT("function_call"))
		{
			// Not a function call — could be a message item starting; no action needed here
			return false;
		}

		// Capture call_id and name (RK2: call_id is authoritative, NOT item.id)
		Item->TryGetStringField(TEXT("id"), CurrentFuncItemId);
		Item->TryGetStringField(TEXT("call_id"), CurrentFuncCallId);
		Item->TryGetStringField(TEXT("name"), CurrentFuncName);
		AccumulatedFuncArgs.Reset();

		return false;
	}

	// ── Function call arguments delta ─────────────────────────────────────────
	if (EventType == OpenAI_Evt_FuncArgsDelta)
	{
		FString Delta;
		if (!Data->TryGetStringField(TEXT("delta"), Delta))
		{
			KISMETAI_LOG(LogKismetAIOpenAIStream, Warning,
				TEXT("FOpenAIStreamState: function_call_arguments.delta missing 'delta' — skipping"));
			return false;
		}
		AccumulatedFuncArgs.Append(Delta);
		return false;
	}

	// ── Function call arguments done ──────────────────────────────────────────
	if (EventType == OpenAI_Evt_FuncArgsDone)
	{
		// The .done event carries the complete arguments string. Update
		// AccumulatedFuncArgs so output_item.done (if it doesn't carry args) can
		// fall back to it. However, output_item.done is authoritative (per plan),
		// so we just update the accumulation here.
		FString FinalArgs;
		if (Data->TryGetStringField(TEXT("arguments"), FinalArgs))
		{
			AccumulatedFuncArgs = FinalArgs;
		}
		return false;
	}

	// ── Item done (finalize) ──────────────────────────────────────────────────
	if (EventType == OpenAI_Evt_OutputItemDone)
	{
		const TSharedPtr<FJsonObject>* ItemPtr;
		if (!Data->TryGetObjectField(TEXT("item"), ItemPtr))
		{
			KISMETAI_LOG(LogKismetAIOpenAIStream, Warning,
				TEXT("FOpenAIStreamState: response.output_item.done missing 'item' — skipping"));
			return false;
		}
		const TSharedPtr<FJsonObject>& Item = *ItemPtr;

		FString ItemType;
		Item->TryGetStringField(TEXT("type"), ItemType);
		const TSharedPtr<FJsonObject> Metadata = KismetAIProviderCodec::CopyWithout(Item,
			ItemType == TEXT("function_call")
				? TArray<FString>{ TEXT("type"), TEXT("id"), TEXT("call_id"), TEXT("name"), TEXT("arguments"), TEXT("status") }
				: ItemType == TEXT("message")
					? TArray<FString>{ TEXT("type"), TEXT("id"), TEXT("role"), TEXT("content"), TEXT("status") }
					: TArray<FString>{});
		if (ItemType == TEXT("message"))
		{
			// Preserve annotations and extensions without duplicating streamed text.
			const auto ItemCopy = KismetAIProviderCodec::CopyWithout(Item, {});
			const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
			if (ItemCopy->TryGetArrayField(TEXT("content"), Content))
			{
				bool bHasMetadata = false;
				for (const auto& Value : *Content)
				{
					const auto Block = Value->Type == EJson::Object ? Value->AsObject() : nullptr;
					FString BlockType;
					if (Block.IsValid() && Block->TryGetStringField(TEXT("type"), BlockType) && BlockType == TEXT("output_text"))
					{
						Block->RemoveField(TEXT("text"));
						bHasMetadata |= Block->Values.Num() > 1;
					}
					else { bHasMetadata = true; }
				}
				if (bHasMetadata) { Metadata->SetArrayField(TEXT("content"), *Content); }
			}
		}
		if (!Metadata->Values.IsEmpty())
		{
			FString ItemId;
			if (Item->TryGetStringField(TEXT("id"), ItemId)) Metadata->SetStringField(TEXT("id"), ItemId);
			if (Item->TryGetStringField(TEXT("call_id"), ItemId)) Metadata->SetStringField(TEXT("call_id"), ItemId);
			ContentBlocks.Add(KismetAIProviderCodec::Metadata(TEXT("openai_responses"), Metadata));
		}

		if (ItemType == TEXT("function_call"))
		{
			// Use call_id, name, and arguments from the done item (authoritative per plan)
			FString CallId;
			FString FuncName;
			FString Arguments;

			Item->TryGetStringField(TEXT("call_id"), CallId);
			Item->TryGetStringField(TEXT("name"), FuncName);
			Item->TryGetStringField(TEXT("arguments"), Arguments);

			// Validate we have the minimum required fields
			if (CallId.IsEmpty())
			{
				KISMETAI_LOG(LogKismetAIOpenAIStream, Warning,
					TEXT("FOpenAIStreamState: output_item.done function_call missing call_id — skipping"));
				return false;
			}
			if (FuncName.IsEmpty())
			{
				KISMETAI_LOG(LogKismetAIOpenAIStream, Warning,
					TEXT("FOpenAIStreamState: output_item.done function_call missing name — skipping"));
				return false;
			}

			// If arguments is empty in done event, fall back to delta-accumulated args
			if (Arguments.IsEmpty() && !AccumulatedFuncArgs.IsEmpty())
			{
				Arguments = AccumulatedFuncArgs;
			}

			FinalizeFunctionCallBlock(CallId, FuncName, Arguments);
		}
		else if (ItemType == TEXT("message"))
		{
			// Finalize the accumulated text block
			FinalizeTextBlock();
		}
		// Other item types (e.g. reasoning) — log and skip
		// (reasoning is handled by the reasoning summary events, not output_item)

		return false;
	}

	// ── Terminal: completed ───────────────────────────────────────────────────
	if (EventType == OpenAI_Evt_Completed)
	{
		bComplete = true;
		ExtractUsage(Data);
		// stop_reason set in BuildResponseJson based on bHasFunctionCall
		// Leave StopReason empty here; BuildResponseJson derives it
		return false;
	}

	// ── Terminal: incomplete (token cap) ─────────────────────────────────────
	if (EventType == OpenAI_Evt_Incomplete)
	{
		bComplete = true;
		ExtractUsage(Data);
		StopReason = TEXT("max_tokens");
		return false;
	}

	// ── Terminal: failed / error ──────────────────────────────────────────────
	if (EventType == OpenAI_Evt_Failed || EventType == OpenAI_Evt_Error)
	{
		ExtractUsage(Data);
		// These arrive on an HTTP-200 stream (the dispatcher only calls
		// BuildResponseJson on 200). Capture the upstream detail and flag the
		// stream as failed so the agent reclassifies the call as retryable
		// instead of surfacing an empty end_turn. Do NOT fabricate tool calls.
		bStreamError = true;
		ExtractStreamError(Data);
		StopReason = TEXT("end_turn");  // safe terminal default if used anyway
		KISMETAI_LOG(LogKismetAIOpenAIStream, Warning,
			TEXT("FOpenAIStreamState: terminal error event '%s' (code='%s'): %s"),
			*EventType,
			*StreamErrorCode,
			StreamErrorMessage.IsEmpty() ? TEXT("<no message>") : *StreamErrorMessage.Left(300));
		return false;
	}

	// ── Reasoning summary (event names unverified) ─────────────────────
	if (EventType == OpenAI_Evt_ReasoningSummaryDelta)
	{
		FString Delta;
		if (Data->TryGetStringField(TEXT("delta"), Delta))
		{
			AccumulatedReasoningText.Append(Delta);
			// Stream incremental reasoning to the UI (mirrors Anthropic thinking_delta).
			if (OnThinkingDelta && !Delta.IsEmpty())
			{
				OnThinkingDelta(Delta);
			}
		}
		return false;
	}

	if (EventType == OpenAI_Evt_ReasoningSummaryDone)
	{
		const TSharedPtr<FJsonObject> Metadata = KismetAIProviderCodec::CopyWithout(Data,
			{ TEXT("type"), TEXT("text"), TEXT("item_id"), TEXT("output_index"), TEXT("summary_index"), TEXT("sequence_number") });
		if (!Metadata->Values.IsEmpty())
		{
			ContentBlocks.Add(KismetAIProviderCodec::Metadata(TEXT("openai_responses"), Metadata));
		}
		// Optionally override with the full text from the done event
		FString FullText;
		if (Data->TryGetStringField(TEXT("text"), FullText))
		{
			AccumulatedReasoningText = FullText;
		}
		// Fire OnThinkingComplete callback (mirrors AnthropicStreamState behavior)
		if (OnThinkingComplete && !AccumulatedReasoningText.IsEmpty())
		{
			OnThinkingComplete(AccumulatedReasoningText);
		}

		// Add a thinking block to content[]
		TSharedPtr<FJsonObject> ThinkingBlock = MakeShared<FJsonObject>();
		ThinkingBlock->SetStringField(TEXT("type"), TEXT("thinking"));
		ThinkingBlock->SetStringField(TEXT("thinking"), AccumulatedReasoningText);
		ContentBlocks.Add(MakeShared<FJsonValueObject>(ThinkingBlock));

		AccumulatedReasoningText.Reset();
		return false;
	}

	// Unknown event — log at dev level and skip (no fabrication)
	KISMETAI_DEV_LOG(LogKismetAIOpenAIStream, Log,
		TEXT("FOpenAIStreamState: unhandled event type '%s' — skipping"), *EventType);
	return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// BuildResponseJson — produce canonical Anthropic-shaped response
// ─────────────────────────────────────────────────────────────────────────────
inline TSharedPtr<FJsonObject> FOpenAIStreamState::BuildResponseJson() const
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();

	Response->SetArrayField(TEXT("content"), ContentBlocks);

	// Derive stop_reason: incomplete event sets it explicitly to "max_tokens";
	// failed/error sets it to "end_turn"; completed derives from function call presence.
	FString FinalStopReason = StopReason;
	if (FinalStopReason.IsEmpty())
	{
		FinalStopReason = bHasFunctionCall ? TEXT("tool_use") : TEXT("end_turn");
	}
	Response->SetStringField(TEXT("stop_reason"), FinalStopReason);
	Response->SetBoolField(TEXT("stream_complete"), bComplete);
	Response->SetBoolField(TEXT("usage_complete"), bComplete && bHasInputUsage && bHasOutputUsage);
	if (!bHasInputUsage && !bHasOutputUsage) return Response;

	TSharedPtr<FJsonObject> Usage = MakeShared<FJsonObject>();
	Usage->SetNumberField(TEXT("input_tokens"), InputTokens);
	Usage->SetNumberField(TEXT("output_tokens"), OutputTokens);
	Usage->SetNumberField(TEXT("cache_read_input_tokens"), CacheReadTokens);
	Usage->SetNumberField(TEXT("cache_creation_input_tokens"), 0);
	Response->SetObjectField(TEXT("usage"), Usage);

	return Response;
}
