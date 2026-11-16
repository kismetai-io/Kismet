// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIAgentTypes.h"

namespace KismetAIAgentCodec
{

FString RoleToString(EAgentRole Role)
{
	if (Role == EAgentRole::Assistant) return TEXT("assistant");
	if (Role == EAgentRole::System) return TEXT("system");
	return Role == EAgentRole::User ? TEXT("user") : FString();
}

EAgentRole RoleFromString(const FString& Role)
{
	if (Role == TEXT("assistant")) return EAgentRole::Assistant;
	if (Role == TEXT("user")) return EAgentRole::User;
	if (Role == TEXT("system")) return EAgentRole::System;
	return EAgentRole::Unknown;
}

FString PartTypeToString(EAgentPartType Type)
{
	switch (Type)
	{
	case EAgentPartType::Text:             return TEXT("text");
	case EAgentPartType::Thinking:         return TEXT("thinking");
	case EAgentPartType::RedactedThinking:  return TEXT("redacted_thinking");
	case EAgentPartType::Image:            return TEXT("image");
	case EAgentPartType::ToolUse:          return TEXT("tool_use");
	case EAgentPartType::ToolResult:       return TEXT("tool_result");
	case EAgentPartType::ProviderMetadata: return TEXT("_kismet_provider_metadata");
	default:                               return FString();
	}
}

EAgentPartType PartTypeFromString(const FString& Type)
{
	if (Type == TEXT("text"))               return EAgentPartType::Text;
	if (Type == TEXT("thinking"))           return EAgentPartType::Thinking;
	if (Type == TEXT("redacted_thinking"))  return EAgentPartType::RedactedThinking;
	if (Type == TEXT("image"))              return EAgentPartType::Image;
	if (Type == TEXT("tool_use"))           return EAgentPartType::ToolUse;
	if (Type == TEXT("tool_result"))        return EAgentPartType::ToolResult;
	if (Type == TEXT("_kismet_provider_metadata")) return EAgentPartType::ProviderMetadata;
	return EAgentPartType::Unknown;
}

namespace
{
	// Deep-copies an object so a decoded part cannot alias — and later mutate —
	// the caller's JSON.
	TSharedPtr<FJsonObject> CloneObject(const TSharedPtr<FJsonObject>& In)
	{
		if (!In.IsValid()) { return nullptr; }
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		FJsonObject::Duplicate(In, Out);
		return Out;
	}

	TSharedPtr<FJsonValue> CloneValue(const TSharedPtr<FJsonValue>& In)
	{
		if (!In.IsValid()) return nullptr;
		auto Holder = MakeShared<FJsonObject>();
		Holder->SetField(TEXT("value"), In);
		return CloneObject(Holder)->Values.FindRef(TEXT("value"));
	}

	FAgentContentPart DecodePart(const TSharedPtr<FJsonObject>& Block)
	{
		FAgentContentPart Part;
		if (!Block.IsValid())
		{
			Part.Type = EAgentPartType::Unknown;
			return Part;
		}

		FString TypeStr;
		Block->TryGetStringField(TEXT("type"), TypeStr);
		Part.Type = PartTypeFromString(TypeStr);

		// Always keep the original block. Thinking blocks carry a `signature`
		// the API rejects the turn without, images carry a `source`, and
		// providers add fields we do not model — none of which may be lost.
		Part.RawJson = CloneObject(Block);

		switch (Part.Type)
		{
		// Each of these carries its payload under a different key: text blocks
		// use "text", thinking blocks use "thinking", redacted ones use "data".
		// Reading them all from "text" silently produced empty Text and — worse
		// — made the encoder inject a spurious "text":"" into every thinking
		// block, changing the bytes sent to the API.
		case EAgentPartType::Text:
			Block->TryGetStringField(TEXT("text"), Part.Text);
			break;

		case EAgentPartType::Thinking:
			Block->TryGetStringField(TEXT("thinking"), Part.Text);
			break;

		case EAgentPartType::RedactedThinking:
			Block->TryGetStringField(TEXT("data"), Part.Text);
			break;

		case EAgentPartType::ToolUse:
		{
			Block->TryGetStringField(TEXT("id"), Part.ToolUseId);
			Block->TryGetStringField(TEXT("name"), Part.ToolName);
			const TSharedPtr<FJsonObject>* Input = nullptr;
			if (Block->TryGetObjectField(TEXT("input"), Input))
			{
				Part.ToolInput = CloneObject(*Input);
			}
			break;
		}

		case EAgentPartType::ToolResult:
			Block->TryGetStringField(TEXT("tool_use_id"), Part.ToolUseId);
			Block->TryGetStringField(TEXT("content"), Part.ToolResultContent);
			Block->TryGetBoolField(TEXT("is_error"), Part.bIsError);
			break;

		default:
			break;
		}
		return Part;
	}

	TSharedPtr<FJsonObject> EncodePart(const FAgentContentPart& Part)
	{
		// Re-encode from RawJson so unmodelled fields survive, then overwrite the
		// fields we own. A part built in code has no RawJson and is encoded from
		// its typed fields alone.
		TSharedPtr<FJsonObject> Block = Part.RawJson.IsValid()
			? CloneObject(Part.RawJson)
			: MakeShared<FJsonObject>();

		if (Part.Type == EAgentPartType::Unknown)
		{
			return Block;
		}

		Block->SetStringField(TEXT("type"), PartTypeToString(Part.Type));

		switch (Part.Type)
		{
		case EAgentPartType::Text:
			Block->SetStringField(TEXT("text"), Part.Text);
			break;

		case EAgentPartType::Thinking:
			Block->SetStringField(TEXT("thinking"), Part.Text);
			break;

		case EAgentPartType::RedactedThinking:
			Block->SetStringField(TEXT("data"), Part.Text);
			break;

		case EAgentPartType::ToolUse:
			Block->SetStringField(TEXT("id"), Part.ToolUseId);
			Block->SetStringField(TEXT("name"), Part.ToolName);
			if (Part.ToolInput.IsValid())
			{
				Block->SetObjectField(TEXT("input"), CloneObject(Part.ToolInput));
			}
			break;

		case EAgentPartType::ToolResult:
			Block->SetStringField(TEXT("tool_use_id"), Part.ToolUseId);
			if (!Part.ToolResultContent.IsEmpty() || !Block->HasField(TEXT("content"))
				|| Block->HasTypedField<EJson::String>(TEXT("content")))
			{
				Block->SetStringField(TEXT("content"), Part.ToolResultContent);
			}
			// is_error is written only when true — the loop omits it otherwise,
			// and adding it would change the bytes for every non-error result.
			if (Part.bIsError)
			{
				Block->SetBoolField(TEXT("is_error"), true);
			}
			else
			{
				if (Block->HasField(TEXT("is_error"))) { Block->SetBoolField(TEXT("is_error"), false); }
			}
			break;

		default:
			break;
		}
		return Block;
	}
}

TArray<FAgentContentPart> FromParts(const TArray<TSharedPtr<FJsonValue>>& In)
{
	TArray<FAgentContentPart> Out;
	for (const auto& Value : In)
	{
		if (Value.IsValid() && Value->Type == EJson::Object) { Out.Add(DecodePart(Value->AsObject())); }
		else
		{
			FAgentContentPart Part; Part.Type = EAgentPartType::Unknown;
			Part.RawValue = CloneValue(Value); Out.Add(MoveTemp(Part));
		}
	}
	return Out;
}

TArray<TSharedPtr<FJsonValue>> ToParts(const TArray<FAgentContentPart>& In)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const auto& Part : In)
	{
		Out.Add(Part.RawValue.IsValid() ? CloneValue(Part.RawValue) : MakeShared<FJsonValueObject>(EncodePart(Part)));
	}
	return Out;
}

bool FromAnthropicJson(const TSharedPtr<FJsonObject>& In, FAgentMessage& Out)
{
	if (!In.IsValid())
	{
		return false;
	}

	FString RoleStr;
	if (!In->TryGetStringField(TEXT("role"), RoleStr))
	{
		return false;
	}
	Out.Role = RoleFromString(RoleStr);
	Out.UnknownRole = Out.Role == EAgentRole::Unknown ? RoleStr : FString();
	Out.RawJson = CloneObject(In);
	Out.RawJson->RemoveField(TEXT("role"));
	Out.RawJson->RemoveField(TEXT("content"));
	Out.Parts.Reset();

	// Content is either a bare string or an array of blocks; both are valid on
	// the wire and both appear in stored sessions.
	const TArray<TSharedPtr<FJsonValue>>* Blocks = nullptr;
	if (In->TryGetArrayField(TEXT("content"), Blocks))
	{
		Out.bContentWasPlainString = false;
		Out.Parts = FromParts(*Blocks);
		return true;
	}

	FString PlainText;
	if (In->TryGetStringField(TEXT("content"), PlainText))
	{
		Out.bContentWasPlainString = true;
		Out.Parts.Add(FAgentContentPart::MakeText(PlainText));
		return true;
	}

	// A role with no content at all: keep it rather than dropping the turn,
	// since dropping one breaks user/assistant alternation on resume.
	Out.bContentWasPlainString = false;
	return true;
}

TSharedPtr<FJsonObject> ToAnthropicJson(const FAgentMessage& In)
{
	TSharedPtr<FJsonObject> Msg = In.RawJson.IsValid() ? CloneObject(In.RawJson) : MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("role"), In.Role == EAgentRole::Unknown ? In.UnknownRole : RoleToString(In.Role));

	if (In.bContentWasPlainString)
	{
		// Reproduce the original bare-string form exactly.
		Msg->SetStringField(TEXT("content"), In.Parts.Num() > 0 ? In.Parts[0].Text : FString());
		return Msg;
	}

	Msg->SetArrayField(TEXT("content"), ToParts(In.Parts));
	return Msg;
}

TArray<FAgentMessage> FromAnthropicArray(const TArray<TSharedPtr<FJsonValue>>& In)
{
	TArray<FAgentMessage> Out;
	Out.Reserve(In.Num());
	for (const TSharedPtr<FJsonValue>& Val : In)
	{
		FAgentMessage Msg;
		if (FromAnthropicJson(Val->AsObject(), Msg))
		{
			Out.Add(MoveTemp(Msg));
		}
	}
	return Out;
}

TArray<TSharedPtr<FJsonValue>> ToAnthropicArray(const TArray<FAgentMessage>& In)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	Out.Reserve(In.Num());
	for (const FAgentMessage& Msg : In)
	{
		Out.Add(MakeShared<FJsonValueObject>(ToAnthropicJson(Msg)));
	}
	return Out;
}

}  // namespace KismetAIAgentCodec
