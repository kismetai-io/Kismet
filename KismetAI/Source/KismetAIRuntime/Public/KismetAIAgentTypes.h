// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Canonical conversation values; provider and journal encodings live at boundaries.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Who produced a message.
enum class EAgentRole : uint8
{
	User,
	Assistant,
	System,
	Unknown,
};

// The kinds of content a message can carry. These mirror the Anthropic content
// block types the loop already handles; unknown kinds are preserved verbatim
// rather than dropped, so a session written by a newer build still loads.
enum class EAgentPartType : uint8
{
	Text,
	Thinking,
	RedactedThinking,
	Image,
	ToolUse,
	ToolResult,
	ProviderMetadata, // Native opaque evidence; never forwarded to a provider request.
	Unknown,
};

// One content block.
//
// Deliberately a single struct with optional fields rather than a class
// hierarchy: the set is small, closed in practice, and every consumer switches
// on Type anyway. RawJson carries anything the typed fields do not model —
// thinking-block signatures, image sources, provider extensions — so a round
// trip through this type is lossless.
struct KISMETAIRUNTIME_API FAgentContentPart
{
	EAgentPartType Type = EAgentPartType::Text;

	// Text / Thinking / RedactedThinking.
	FString Text;

	// ToolUse: the model's call.
	FString ToolUseId;
	FString ToolName;
	TSharedPtr<FJsonObject> ToolInput;

	// ToolResult: our answer to a ToolUse, matched by ToolUseId.
	FString ToolResultContent;
	bool bIsError = false;

	// Everything not modelled above, preserved so encoding is lossless. For an
	// Unknown part this is the whole block.
	TSharedPtr<FJsonObject> RawJson;
	TSharedPtr<FJsonValue> RawValue; // Non-object unknown blocks, never interpreted as text.

	static FAgentContentPart MakeText(const FString& InText)
	{
		FAgentContentPart P;
		P.Type = EAgentPartType::Text;
		P.Text = InText;
		return P;
	}

	static FAgentContentPart MakeToolUse(const FString& Id, const FString& Name, const TSharedPtr<FJsonObject>& Input)
	{
		FAgentContentPart P;
		P.Type = EAgentPartType::ToolUse;
		P.ToolUseId = Id;
		P.ToolName = Name;
		P.ToolInput = Input;
		return P;
	}

	static FAgentContentPart MakeToolResult(const FString& Id, const FString& Content, bool bError)
	{
		FAgentContentPart P;
		P.Type = EAgentPartType::ToolResult;
		P.ToolUseId = Id;
		P.ToolResultContent = Content;
		P.bIsError = bError;
		return P;
	}
};

// One turn's worth of content from one role.
struct KISMETAIRUNTIME_API FAgentMessage
{
	EAgentRole Role = EAgentRole::User;
	FString UnknownRole;
	TArray<FAgentContentPart> Parts;
	TSharedPtr<FJsonObject> RawJson; // Message-level extensions, excluding role/content.

	static FAgentMessage MakeText(EAgentRole InRole, const FString& Text)
	{
		FAgentMessage M; M.Role = InRole; M.bContentWasPlainString = true;
		M.Parts.Add(FAgentContentPart::MakeText(Text)); return M;
	}

	// True when the message was stored as a bare string rather than a block
	// array. The Anthropic API accepts both, the loop writes both, and the codec
	// must reproduce whichever form a session file used or resume changes the
	// bytes the model sees.
	bool bContentWasPlainString = false;

	bool HasToolUse() const
	{
		for (const FAgentContentPart& P : Parts)
		{
			if (P.Type == EAgentPartType::ToolUse) { return true; }
		}
		return false;
	}

	// Concatenated text of every Text part.
	FString GetText() const
	{
		FString Out;
		for (const FAgentContentPart& P : Parts)
		{
			if (P.Type == EAgentPartType::Text) { Out += P.Text; }
		}
		return Out;
	}
};

// Anthropic wire-shape codec.
//
// Provider/journal boundaries and temporary context-algorithm bridges use this
// codec. Preserve content values and ordering; JSON object key order is not semantic.
namespace KismetAIAgentCodec
{
	KISMETAIRUNTIME_API TArray<FAgentContentPart> FromParts(const TArray<TSharedPtr<FJsonValue>>& In);
	KISMETAIRUNTIME_API TArray<TSharedPtr<FJsonValue>> ToParts(const TArray<FAgentContentPart>& In);
	KISMETAIRUNTIME_API bool FromAnthropicJson(const TSharedPtr<FJsonObject>& In, FAgentMessage& Out);
	KISMETAIRUNTIME_API TSharedPtr<FJsonObject> ToAnthropicJson(const FAgentMessage& In);

	// Whole-array convenience for the resume path.
	KISMETAIRUNTIME_API TArray<FAgentMessage> FromAnthropicArray(const TArray<TSharedPtr<FJsonValue>>& In);
	KISMETAIRUNTIME_API TArray<TSharedPtr<FJsonValue>> ToAnthropicArray(const TArray<FAgentMessage>& In);

	KISMETAIRUNTIME_API FString RoleToString(EAgentRole Role);
	KISMETAIRUNTIME_API EAgentRole RoleFromString(const FString& Role);
	KISMETAIRUNTIME_API FString PartTypeToString(EAgentPartType Type);
	KISMETAIRUNTIME_API EAgentPartType PartTypeFromString(const FString& Type);
}
