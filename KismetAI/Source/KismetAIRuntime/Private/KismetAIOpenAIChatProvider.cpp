// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIOpenAIChatProvider.h"
#include "KismetAIAgent.h"
#include "KismetAILog.h"
#include "KismetAIModelCatalog.h"
#include "KismetAIModelDiscovery.h"
#include "KismetAIProviderProfile.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Regex.h"
#include "Misc/Guid.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogKismetAIOpenAIChat, Log, All);

// Named (not anonymous) so unity builds cannot collide with another .cpp's helpers.
namespace KismetAIOpenAIChat
{
	typedef TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>        FCompactWriter;
	typedef TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>> FCompactWriterFactory;

	const TCHAR* const MalformedToolCode = TEXT("malformed_tool_arguments");
	const TCHAR* const MalformedToolMessage = TEXT("The model returned incomplete or invalid tool arguments. No tools from this response were executed. Retry with a smaller task.");

	FString SerializeCompact(const TSharedPtr<FJsonObject>& Obj)
	{
		if (!Obj.IsValid())
		{
			return TEXT("{}");
		}
		FString Output;
		TSharedRef<FCompactWriter> Writer = FCompactWriterFactory::Create(&Output);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Output;
	}

	// A string field, or a number field rendered as an integer string. Hosts
	// disagree on whether error.code is "rate_limit_exceeded" or 429.
	bool ReadScalar(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, FString& Out, bool* bOutWasNumber = nullptr)
	{
		if (bOutWasNumber)
		{
			*bOutWasNumber = false;
		}
		if (!Obj.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Field);
		if (!Value.IsValid())
		{
			return false;
		}
		if (Value->Type == EJson::String)
		{
			Out = Value->AsString();
			return true;
		}
		if (Value->Type == EJson::Number)
		{
			Out = FString::Printf(TEXT("%lld"), static_cast<int64>(Value->AsNumber()));
			if (bOutWasNumber)
			{
				*bOutWasNumber = true;
			}
			return true;
		}
		return false;
	}

	// The text of a canonical content value: a plain string, or the joined
	// text blocks of an array.
	FString ContentText(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, const TCHAR* Separator)
	{
		FString Str;
		if (Obj->TryGetStringField(Field, Str))
		{
			return Str;
		}
		const TArray<TSharedPtr<FJsonValue>>* Blocks = nullptr;
		if (!Obj->TryGetArrayField(Field, Blocks) || !Blocks)
		{
			return FString();
		}
		TArray<FString> Parts;
		for (const TSharedPtr<FJsonValue>& BlockValue : *Blocks)
		{
			const TSharedPtr<FJsonObject>* BlockPtr = nullptr;
			if (!BlockValue.IsValid() || !BlockValue->TryGetObject(BlockPtr) || !BlockPtr)
			{
				continue;
			}
			FString Type;
			(*BlockPtr)->TryGetStringField(TEXT("type"), Type);
			FString Text;
			if (Type == TEXT("text") && (*BlockPtr)->TryGetStringField(TEXT("text"), Text))
			{
				Parts.Add(Text);
			}
		}
		return FString::Join(Parts, Separator);
	}

	// Canonical image block -> {"type":"image_url","image_url":{"url":...}}.
	TSharedPtr<FJsonObject> TranslateImage(const TSharedPtr<FJsonObject>& Block)
	{
		const TSharedPtr<FJsonObject>* SourcePtr = nullptr;
		if (!Block->TryGetObjectField(TEXT("source"), SourcePtr) || !SourcePtr || !SourcePtr->IsValid())
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject>& Source = *SourcePtr;
		FString SourceType;
		Source->TryGetStringField(TEXT("type"), SourceType);

		FString Url;
		if (SourceType == TEXT("url"))
		{
			Source->TryGetStringField(TEXT("url"), Url);
		}
		else
		{
			FString MediaType;
			FString Data;
			Source->TryGetStringField(TEXT("media_type"), MediaType);
			Source->TryGetStringField(TEXT("data"), Data);
			if (!Data.IsEmpty())
			{
				Url = FString::Printf(TEXT("data:%s;base64,%s"),
					MediaType.IsEmpty() ? TEXT("image/png") : *MediaType, *Data);
			}
		}
		if (Url.IsEmpty())
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> ImageUrl = MakeShared<FJsonObject>();
		ImageUrl->SetStringField(TEXT("url"), Url);
		TSharedPtr<FJsonObject> Part = MakeShared<FJsonObject>();
		Part->SetStringField(TEXT("type"), TEXT("image_url"));
		Part->SetObjectField(TEXT("image_url"), ImageUrl);
		return Part;
	}

	// Put an ephemeral cache_control on a message's last text part, turning a
	// plain string content into a one-part array first.
	bool MarkLastTextPart(const TSharedPtr<FJsonObject>& Msg)
	{
		TSharedPtr<FJsonObject> CacheControl = MakeShared<FJsonObject>();
		CacheControl->SetStringField(TEXT("type"), TEXT("ephemeral"));
		FString Plain;
		if (Msg->TryGetStringField(TEXT("content"), Plain))
		{
			if (Plain.IsEmpty())
			{
				return false;
			}
			TSharedPtr<FJsonObject> Part = MakeShared<FJsonObject>();
			Part->SetStringField(TEXT("type"), TEXT("text"));
			Part->SetStringField(TEXT("text"), Plain);
			Part->SetObjectField(TEXT("cache_control"), CacheControl);
			Msg->SetArrayField(TEXT("content"), { MakeShared<FJsonValueObject>(Part) });
			return true;
		}
		const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
		if (!Msg->TryGetArrayField(TEXT("content"), Parts) || !Parts)
		{
			return false;
		}
		for (int32 Index = Parts->Num() - 1; Index >= 0; --Index)
		{
			const TSharedPtr<FJsonObject>* PartPtr = nullptr;
			FString Type;
			if ((*Parts)[Index].IsValid() && (*Parts)[Index]->TryGetObject(PartPtr) && PartPtr
				&& (*PartPtr)->TryGetStringField(TEXT("type"), Type) && Type == TEXT("text"))
			{
				(*PartPtr)->SetObjectField(TEXT("cache_control"), CacheControl);
				return true;
			}
		}
		return false;
	}

	// Two breakpoints: the system prompt (stable for the whole run) and the
	// last user message (the growing prefix). Anthropic allows four.
	void AddCacheMarkers(TArray<TSharedPtr<FJsonValue>>& ChatMessages)
	{
		for (const TSharedPtr<FJsonValue>& Value : ChatMessages)
		{
			const TSharedPtr<FJsonObject>* MsgPtr = nullptr;
			FString Role;
			if (Value.IsValid() && Value->TryGetObject(MsgPtr) && MsgPtr
				&& (*MsgPtr)->TryGetStringField(TEXT("role"), Role) && Role == TEXT("system"))
			{
				MarkLastTextPart(*MsgPtr);
				break;
			}
		}
		for (int32 Index = ChatMessages.Num() - 1; Index >= 0; --Index)
		{
			const TSharedPtr<FJsonObject>* MsgPtr = nullptr;
			FString Role;
			if (ChatMessages[Index].IsValid() && ChatMessages[Index]->TryGetObject(MsgPtr) && MsgPtr
				&& (*MsgPtr)->TryGetStringField(TEXT("role"), Role) && Role == TEXT("user"))
			{
				MarkLastTextPart(*MsgPtr);
				break;
			}
		}
	}

	TSharedPtr<FJsonValue> MakeMessage(const FString& Role, const FString& Content)
	{
		TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), Role);
		Msg->SetStringField(TEXT("content"), Content);
		return MakeShared<FJsonValueObject>(Msg);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// FOpenAIChatProvider
// ─────────────────────────────────────────────────────────────────────────────

const TCHAR* FOpenAIChatProvider::ToolErrorPrefix = TEXT("[tool error] ");

FOpenAIChatProvider::FOpenAIChatProvider(const FKismetAIProviderProfile& InProfile)
	: Profile(InProfile)
{
}

FString FOpenAIChatProvider::GetName() const
{
	// The agent prices and sizes context with OpenAI's inclusive usage
	// semantics only when the name is exactly "openai" (case-insensitive).
	// This provider normalises usage to disjoint, so it must never claim it.
	if (Profile.Id.IsEmpty() || Profile.Id.Equals(TEXT("openai"), ESearchCase::IgnoreCase))
	{
		return TEXT("openai-chat");
	}
	return Profile.Id;
}

int32 FOpenAIChatProvider::CacheTtlSeconds() const
{
	// Chat Completions hosts cache automatically (when at all) with no TTL
	// control; 300s is the conservative common floor.
	return 300;
}

FString FOpenAIChatProvider::GetEndpointUrl(const FKismetAIAgentConfig& Config) const
{
	const FString BaseUrl = KismetAIProviders::GetBaseUrl(Config, Profile);
	if (BaseUrl.IsEmpty())
	{
		KISMETAI_LOG(LogKismetAIOpenAIChat, Warning,
			TEXT("Provider '%s' has no base URL configured; set one in settings."), *Profile.Id);
	}
	return BaseUrl + TEXT("/chat/completions");
}

TArray<FString> FOpenAIChatProvider::GetExtraCurlHeaders(
	const FKismetAIAgentConfig& Config,
	const FLLMRequestShape& /*Shape*/) const
{
	// Auth style and ExtraHeaders come from the profile; the key comes from the
	// profile's own slot only.
	return KismetAIProviders::BuildAuthHeaders(Config, Profile);
}

TSharedPtr<ILLMStreamState> FOpenAIChatProvider::CreateStreamState() const
{
	return MakeShared<FOpenAIChatStreamState>();
}

TSharedPtr<FJsonObject> FOpenAIChatProvider::BuildRequestBody(
	const FKismetAIAgentConfig& Config,
	const TArray<FAgentMessage>& CanonicalMessages,
	const TArray<TSharedPtr<FJsonValue>>& Tools,
	const FString& SystemPrompt,
	FLLMRequestShape& OutShape) const
{
	return BuildRequestBodyWithCaps(Config, CanonicalMessages, Tools, SystemPrompt,
		FKismetAIModelCatalog::Get().FindOrDefaults(Config.Model).Caps, OutShape);
}

TSharedPtr<FJsonObject> FOpenAIChatProvider::BuildRequestBodyWithCaps(
	const FKismetAIAgentConfig& Config,
	const TArray<FAgentMessage>& CanonicalMessages,
	const TArray<TSharedPtr<FJsonValue>>& Tools,
	const FString& SystemPrompt,
	const FKismetAIModelCapabilities& Caps,
	FLLMRequestShape& OutShape) const
{
	const TArray<TSharedPtr<FJsonValue>> Messages = KismetAIProviderCodec::EncodeMessages(CanonicalMessages);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();

	Body->SetStringField(TEXT("model"), KismetAIProviders::WireModelId(Config, Profile));
	Body->SetBoolField(TEXT("stream"), true);
	{
		TSharedPtr<FJsonObject> StreamOptions = MakeShared<FJsonObject>();
		StreamOptions->SetBoolField(TEXT("include_usage"), true);
		Body->SetObjectField(TEXT("stream_options"), StreamOptions);
	}
	Body->SetNumberField(TEXT("max_tokens"), Config.MaxTokens);

	// ── Reasoning vs sampling ─────────────────────────────────────────────
	OutShape.bThinkingEnabled = false;
	OutShape.bEffortEnabled = false;
	if (Caps.bReasoning)
	{
		TArray<FString> Supported = Caps.EffortLevels;
		if (Supported.Num() == 0)
		{
			Supported = { TEXT("low"), TEXT("medium"), TEXT("high") };
		}
		FString Effort = FKismetAIModelCatalog::ClampEffort(Supported,
			FKismetAIAgent::EffortLevelToString(Config.EffortLevel));
		if (Effort.IsEmpty())
		{
			Effort = TEXT("medium");
		}
		Body->SetStringField(TEXT("reasoning_effort"), Effort);
		OutShape.bEffortEnabled = true;
		// Reasoning models reject temperature; omit it.
	}
	else if (Caps.bSamplingParams)
	{
		Body->SetNumberField(TEXT("temperature"), static_cast<double>(Config.Temperature));
	}

	// ── Tools ─────────────────────────────────────────────────────────────
	TArray<TSharedPtr<FJsonValue>> ChatTools;
	for (const TSharedPtr<FJsonValue>& ToolValue : Tools)
	{
		const TSharedPtr<FJsonObject>* ToolPtr = nullptr;
		if (!ToolValue.IsValid() || !ToolValue->TryGetObject(ToolPtr) || !ToolPtr || !ToolPtr->IsValid())
		{
			continue;
		}
		const TSharedPtr<FJsonObject>& Tool = *ToolPtr;
		FString Name;
		FString Description;
		Tool->TryGetStringField(TEXT("name"), Name);
		Tool->TryGetStringField(TEXT("description"), Description);
		if (Name.IsEmpty())
		{
			continue;
		}
		const TSharedPtr<FJsonObject>* SchemaPtr = nullptr;
		TSharedPtr<FJsonObject> Schema;
		if (Tool->TryGetObjectField(TEXT("input_schema"), SchemaPtr) && SchemaPtr && SchemaPtr->IsValid())
		{
			Schema = *SchemaPtr;
		}
		else
		{
			Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("object"));
		}

		TSharedPtr<FJsonObject> Function = MakeShared<FJsonObject>();
		Function->SetStringField(TEXT("name"), Name);
		Function->SetStringField(TEXT("description"), Description);
		Function->SetObjectField(TEXT("parameters"), Schema);

		TSharedPtr<FJsonObject> ChatTool = MakeShared<FJsonObject>();
		ChatTool->SetStringField(TEXT("type"), TEXT("function"));
		ChatTool->SetObjectField(TEXT("function"), Function);
		ChatTools.Add(MakeShared<FJsonValueObject>(ChatTool));
	}
	// tool_choice without tools is a 400 on OpenAI and several compatible
	// hosts, so both are sent together or not at all.
	if (ChatTools.Num() > 0)
	{
		Body->SetArrayField(TEXT("tools"), ChatTools);
		Body->SetStringField(TEXT("tool_choice"), TEXT("auto"));
	}

	// ── Messages ──────────────────────────────────────────────────────────
	TArray<TSharedPtr<FJsonValue>> ChatMessages;
	if (!SystemPrompt.IsEmpty())
	{
		ChatMessages.Add(KismetAIOpenAIChat::MakeMessage(TEXT("system"), SystemPrompt));
	}
	for (const TSharedPtr<FJsonValue>& MsgValue : Messages)
	{
		const TSharedPtr<FJsonObject>* MsgPtr = nullptr;
		if (!MsgValue.IsValid() || !MsgValue->TryGetObject(MsgPtr) || !MsgPtr)
		{
			KISMETAI_LOG(LogKismetAIOpenAIChat, Warning,
				TEXT("FOpenAIChatProvider::BuildRequestBody: skipping non-object message value"));
			continue;
		}
		TranslateMessage(*MsgPtr, ChatMessages);
	}
	// Claude through OpenRouter caches only at explicit cache_control
	// breakpoints, as on Anthropic's own API (Eden chatCompletionsProtocol.h).
	if (UsesAnthropicCacheMarkers(KismetAIProviders::WireModelId(Config, Profile)))
	{
		KismetAIOpenAIChat::AddCacheMarkers(ChatMessages);
	}
	Body->SetArrayField(TEXT("messages"), ChatMessages);

	return Body;
}

bool FOpenAIChatProvider::UsesAnthropicCacheMarkers(const FString& WireModel)
{
	return WireModel.StartsWith(TEXT("anthropic/"), ESearchCase::IgnoreCase);
}

void FOpenAIChatProvider::TranslateMessage(
	const TSharedPtr<FJsonObject>& CanonicalMsg,
	TArray<TSharedPtr<FJsonValue>>& OutMessages)
{
	using namespace KismetAIOpenAIChat;

	if (!CanonicalMsg.IsValid())
	{
		return;
	}
	FString Role;
	CanonicalMsg->TryGetStringField(TEXT("role"), Role);

	// ── System (mid-conversation reminders) ───────────────────────────────
	if (Role == TEXT("system"))
	{
		const FString Text = ContentText(CanonicalMsg, TEXT("content"), TEXT("\n\n"));
		if (!Text.IsEmpty())
		{
			OutMessages.Add(MakeMessage(TEXT("system"), Text));
		}
		return;
	}

	// ── User ──────────────────────────────────────────────────────────────
	if (Role == TEXT("user"))
	{
		FString PlainContent;
		if (CanonicalMsg->TryGetStringField(TEXT("content"), PlainContent))
		{
			OutMessages.Add(MakeMessage(TEXT("user"), PlainContent));
			return;
		}
		const TArray<TSharedPtr<FJsonValue>>* Blocks = nullptr;
		if (!CanonicalMsg->TryGetArrayField(TEXT("content"), Blocks) || !Blocks)
		{
			return;
		}

		// Tool results become role:"tool" messages, emitted first and in order:
		// Chat Completions requires them to follow the assistant's tool_calls
		// directly, so any text or image in the same canonical turn goes into
		// one user message after them.
		TArray<FString> Texts;
		TArray<TSharedPtr<FJsonValue>> ImageParts;
		for (const TSharedPtr<FJsonValue>& BlockValue : *Blocks)
		{
			const TSharedPtr<FJsonObject>* BlockPtr = nullptr;
			if (!BlockValue.IsValid() || !BlockValue->TryGetObject(BlockPtr) || !BlockPtr || !BlockPtr->IsValid())
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& Block = *BlockPtr;
			FString Type;
			Block->TryGetStringField(TEXT("type"), Type);

			if (Type == TEXT("tool_result"))
			{
				FString ToolUseId;
				Block->TryGetStringField(TEXT("tool_use_id"), ToolUseId);
				FString Output = ContentText(Block, TEXT("content"), TEXT("\n"));
				bool bIsError = false;
				Block->TryGetBoolField(TEXT("is_error"), bIsError);
				if (bIsError)
				{
					Output = FString(ToolErrorPrefix) + Output;
				}
				TSharedPtr<FJsonObject> ToolMsg = MakeShared<FJsonObject>();
				ToolMsg->SetStringField(TEXT("role"), TEXT("tool"));
				ToolMsg->SetStringField(TEXT("tool_call_id"), ToolUseId);
				ToolMsg->SetStringField(TEXT("content"), Output);
				OutMessages.Add(MakeShared<FJsonValueObject>(ToolMsg));
			}
			else if (Type == TEXT("text"))
			{
				FString Text;
				if (Block->TryGetStringField(TEXT("text"), Text) && !Text.IsEmpty())
				{
					Texts.Add(Text);
				}
			}
			else if (Type == TEXT("image"))
			{
				if (TSharedPtr<FJsonObject> Part = TranslateImage(Block))
				{
					ImageParts.Add(MakeShared<FJsonValueObject>(Part));
				}
			}
		}

		if (ImageParts.Num() > 0)
		{
			// Multimodal: an array of parts, images first as the canonical turn has them.
			TArray<TSharedPtr<FJsonValue>> Parts = ImageParts;
			for (const FString& Text : Texts)
			{
				TSharedPtr<FJsonObject> TextPart = MakeShared<FJsonObject>();
				TextPart->SetStringField(TEXT("type"), TEXT("text"));
				TextPart->SetStringField(TEXT("text"), Text);
				Parts.Add(MakeShared<FJsonValueObject>(TextPart));
			}
			TSharedPtr<FJsonObject> UserMsg = MakeShared<FJsonObject>();
			UserMsg->SetStringField(TEXT("role"), TEXT("user"));
			UserMsg->SetArrayField(TEXT("content"), Parts);
			OutMessages.Add(MakeShared<FJsonValueObject>(UserMsg));
		}
		else if (Texts.Num() > 0)
		{
			// Text only: a plain string, which every compatible server accepts.
			OutMessages.Add(MakeMessage(TEXT("user"), FString::Join(Texts, TEXT("\n\n"))));
		}
		return;
	}

	// ── Assistant ─────────────────────────────────────────────────────────
	if (Role == TEXT("assistant"))
	{
		FString PlainContent;
		if (CanonicalMsg->TryGetStringField(TEXT("content"), PlainContent))
		{
			if (!PlainContent.IsEmpty())
			{
				OutMessages.Add(MakeMessage(TEXT("assistant"), PlainContent));
			}
			return;
		}
		const TArray<TSharedPtr<FJsonValue>>* Blocks = nullptr;
		if (!CanonicalMsg->TryGetArrayField(TEXT("content"), Blocks) || !Blocks)
		{
			return;
		}

		TArray<FString> Texts;
		TArray<TSharedPtr<FJsonValue>> ToolCalls;
		for (const TSharedPtr<FJsonValue>& BlockValue : *Blocks)
		{
			const TSharedPtr<FJsonObject>* BlockPtr = nullptr;
			if (!BlockValue.IsValid() || !BlockValue->TryGetObject(BlockPtr) || !BlockPtr || !BlockPtr->IsValid())
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& Block = *BlockPtr;
			FString Type;
			Block->TryGetStringField(TEXT("type"), Type);

			if (Type == TEXT("text"))
			{
				FString Text;
				if (Block->TryGetStringField(TEXT("text"), Text) && !Text.IsEmpty())
				{
					Texts.Add(Text);
				}
			}
			else if (Type == TEXT("tool_use"))
			{
				FString Id;
				FString Name;
				Block->TryGetStringField(TEXT("id"), Id);
				Block->TryGetStringField(TEXT("name"), Name);
				if (Id.IsEmpty() || Name.IsEmpty())
				{
					KISMETAI_LOG(LogKismetAIOpenAIChat, Error,
						TEXT("FOpenAIChatProvider: tool_use block missing id or name; skipping (cannot round-trip)"));
					continue;
				}
				const TSharedPtr<FJsonObject>* InputPtr = nullptr;
				TSharedPtr<FJsonObject> Input;
				if (Block->TryGetObjectField(TEXT("input"), InputPtr) && InputPtr)
				{
					Input = *InputPtr;
				}

				TSharedPtr<FJsonObject> Function = MakeShared<FJsonObject>();
				Function->SetStringField(TEXT("name"), Name);
				Function->SetStringField(TEXT("arguments"), SerializeCompact(Input));

				TSharedPtr<FJsonObject> Call = MakeShared<FJsonObject>();
				Call->SetStringField(TEXT("id"), Id);
				Call->SetStringField(TEXT("type"), TEXT("function"));
				Call->SetObjectField(TEXT("function"), Function);
				ToolCalls.Add(MakeShared<FJsonValueObject>(Call));
			}
			// thinking / redacted_thinking: dropped. Chat Completions has no way
			// to replay them, and hosts that return reasoning do not accept it back.
		}

		if (Texts.Num() == 0 && ToolCalls.Num() == 0)
		{
			return;  // nothing replayable (e.g. a thinking-only turn)
		}

		TSharedPtr<FJsonObject> AssistantMsg = MakeShared<FJsonObject>();
		AssistantMsg->SetStringField(TEXT("role"), TEXT("assistant"));
		if (Texts.Num() > 0)
		{
			AssistantMsg->SetStringField(TEXT("content"), FString::Join(Texts, TEXT("\n")));
		}
		else
		{
			AssistantMsg->SetField(TEXT("content"), MakeShared<FJsonValueNull>());
		}
		if (ToolCalls.Num() > 0)
		{
			AssistantMsg->SetArrayField(TEXT("tool_calls"), ToolCalls);
		}
		OutMessages.Add(MakeShared<FJsonValueObject>(AssistantMsg));
		return;
	}

	KISMETAI_LOG(LogKismetAIOpenAIChat, Warning,
		TEXT("FOpenAIChatProvider::TranslateMessage: unknown role '%s'; skipping"), *Role);
}

void FOpenAIChatProvider::ParseErrorBody(
	const TSharedPtr<FJsonObject>& Body,
	FModelCallOutcome& Outcome) const
{
	using namespace KismetAIOpenAIChat;

	if (!Body.IsValid())
	{
		return;
	}

	// {"error":{"message","type","code"}} (OpenAI, OpenRouter, Groq, ...),
	// {"error":"text"}, or a flat {"message","type","code"} (vLLM, some proxies).
	TSharedPtr<FJsonObject> ErrorObj = Body;
	const TSharedPtr<FJsonObject>* NestedPtr = nullptr;
	FString ErrorString;
	if (Body->TryGetObjectField(TEXT("error"), NestedPtr) && NestedPtr && NestedPtr->IsValid())
	{
		ErrorObj = *NestedPtr;
	}
	else if (Body->TryGetStringField(TEXT("error"), ErrorString))
	{
		Outcome.ErrorMessage = ErrorString;
	}

	FString Message;
	FString Type;
	FString Code;
	bool bCodeIsNumber = false;
	ErrorObj->TryGetStringField(TEXT("message"), Message);
	ErrorObj->TryGetStringField(TEXT("type"), Type);
	ReadScalar(ErrorObj, TEXT("code"), Code, &bCodeIsNumber);
	if (Message.IsEmpty())
	{
		Body->TryGetStringField(TEXT("detail"), Message);
	}

	if (!Message.IsEmpty())
	{
		Outcome.ErrorMessage = Message;
	}

	// ClassifyCallOutcome keys off ErrorType. A string code is the most specific
	// ("context_length_exceeded", "insufficient_quota"); a numeric code is just
	// the HTTP status again, which the classifier already has.
	if (!Code.IsEmpty() && !bCodeIsNumber)
	{
		Outcome.ErrorType = Code;
	}
	else if (!Type.IsEmpty())
	{
		Outcome.ErrorType = Type;
	}

	if (KismetAIModelsApi::IsToolsUnsupportedMessage(Outcome.ErrorMessage))
	{
		Outcome.ErrorType = TEXT("tools_unsupported");
		Outcome.ErrorMessage = KismetAIModelsApi::ToolsUnsupportedMessage(Profile.DisplayName, TEXT("This model"))
			+ TEXT(" (server: ") + Outcome.ErrorMessage + TEXT(")");
		return;
	}

	// Context overflow. Wording differs per server:
	//   OpenAI: "maximum context length is N tokens. However, your messages resulted in M tokens"
	//   vLLM:   "maximum context length is N tokens. However, you requested T tokens (M in the messages, K in the completion)"
	//   llama.cpp: type "exceed_context_size_error"
	const FString Lower = Outcome.ErrorMessage.ToLower();
	const bool bOverflow = Outcome.ErrorType == TEXT("context_length_exceeded")
		|| Outcome.ErrorType == TEXT("exceed_context_size_error")
		|| Lower.Contains(TEXT("maximum context length"))
		|| (Lower.Contains(TEXT("context")) && Lower.Contains(TEXT("exceed")));
	if (bOverflow)
	{
		Outcome.ErrorType = TEXT("context_length_exceeded");

		FRegexPattern LimitPattern(TEXT("maximum context length is (\\d+)"));
		FRegexMatcher LimitMatcher(LimitPattern, Outcome.ErrorMessage);
		if (LimitMatcher.FindNext())
		{
			Outcome.ContextLimit = FCString::Atoi(*LimitMatcher.GetCaptureGroup(1));
		}

		FRegexPattern UsedPattern(TEXT("resulted in (\\d+) tokens"));
		FRegexMatcher UsedMatcher(UsedPattern, Outcome.ErrorMessage);
		if (UsedMatcher.FindNext())
		{
			Outcome.InputTokens = FCString::Atoi(*UsedMatcher.GetCaptureGroup(1));
		}
		else
		{
			FRegexPattern MessagesPattern(TEXT("(\\d+) in the messages"));
			FRegexMatcher MessagesMatcher(MessagesPattern, Outcome.ErrorMessage);
			if (MessagesMatcher.FindNext())
			{
				Outcome.InputTokens = FCString::Atoi(*MessagesMatcher.GetCaptureGroup(1));
			}
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// FOpenAIChatStreamState
// ─────────────────────────────────────────────────────────────────────────────

bool FOpenAIChatStreamState::ProcessEvent(
	const FString& EventType,
	const TSharedPtr<FJsonObject>& Data,
	FString& OutDelta)
{
	OutDelta.Reset();

	// `data: [DONE]` is not JSON: it arrives (if at all) with a null Data.
	if (!Data.IsValid())
	{
		return false;
	}

	// ── In-stream error on an HTTP-200 connection ─────────────────────────
	const TSharedPtr<FJsonObject>* ErrorPtr = nullptr;
	FString ErrorString;
	if (Data->TryGetObjectField(TEXT("error"), ErrorPtr) && ErrorPtr && ErrorPtr->IsValid())
	{
		RecordStreamError(*ErrorPtr, FString());
	}
	else if (Data->TryGetStringField(TEXT("error"), ErrorString))
	{
		RecordStreamError(nullptr, ErrorString);
	}
	else if (EventType == TEXT("error"))
	{
		RecordStreamError(Data, FString());
	}

	// ── Usage (final chunk with include_usage; Groq nests it under x_groq) ─
	const TSharedPtr<FJsonObject>* UsagePtr = nullptr;
	if (Data->TryGetObjectField(TEXT("usage"), UsagePtr) && UsagePtr && UsagePtr->IsValid())
	{
		ExtractUsage(*UsagePtr);
	}
	else
	{
		const TSharedPtr<FJsonObject>* GroqPtr = nullptr;
		if (Data->TryGetObjectField(TEXT("x_groq"), GroqPtr) && GroqPtr && GroqPtr->IsValid()
			&& (*GroqPtr)->TryGetObjectField(TEXT("usage"), UsagePtr) && UsagePtr && UsagePtr->IsValid())
		{
			ExtractUsage(*UsagePtr);
		}
	}

	// ── choices[0] ────────────────────────────────────────────────────────
	const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
	if (!Data->TryGetArrayField(TEXT("choices"), Choices) || !Choices || Choices->Num() == 0)
	{
		return false;
	}
	TSharedPtr<FJsonObject> Choice;
	for (const TSharedPtr<FJsonValue>& ChoiceValue : *Choices)
	{
		const TSharedPtr<FJsonObject>* ChoicePtr = nullptr;
		if (!ChoiceValue.IsValid() || !ChoiceValue->TryGetObject(ChoicePtr) || !ChoicePtr || !ChoicePtr->IsValid())
		{
			continue;
		}
		int32 ChoiceIndex = 0;
		(*ChoicePtr)->TryGetNumberField(TEXT("index"), ChoiceIndex);
		if (ChoiceIndex == 0)
		{
			Choice = *ChoicePtr;
			break;
		}
	}
	if (!Choice.IsValid())
	{
		return false;
	}

	bool bTextDelta = false;
	const TSharedPtr<FJsonObject>* DeltaPtr = nullptr;
	if (Choice->TryGetObjectField(TEXT("delta"), DeltaPtr) && DeltaPtr && DeltaPtr->IsValid())
	{
		const TSharedPtr<FJsonObject>& Delta = *DeltaPtr;
		const TSharedPtr<FJsonObject> Metadata = KismetAIProviderCodec::CopyWithout(Delta,
			{ TEXT("role"), TEXT("content"), TEXT("reasoning"), TEXT("reasoning_content"), TEXT("tool_calls") });
		if (!Metadata->Values.IsEmpty())
		{
			NativeMetadataBlocks.Add(KismetAIProviderCodec::Metadata(TEXT("openai_chat"), Metadata));
		}

		// Reasoning: DeepSeek / vLLM spell it reasoning_content, OpenRouter
		// reasoning. Some hosts send both with the same text, so take one.
		FString Reasoning;
		if (!Delta->TryGetStringField(TEXT("reasoning_content"), Reasoning) || Reasoning.IsEmpty())
		{
			Delta->TryGetStringField(TEXT("reasoning"), Reasoning);
		}
		if (!Reasoning.IsEmpty())
		{
			AppendReasoning(Reasoning);
		}

		FString Content;
		if (Delta->TryGetStringField(TEXT("content"), Content) && !Content.IsEmpty())
		{
			CloseThinkingSegment();
			Text.Append(Content);
			OutDelta = Content;
			bTextDelta = true;
		}

		const TArray<TSharedPtr<FJsonValue>>* ToolCallValues = nullptr;
		if (Delta->TryGetArrayField(TEXT("tool_calls"), ToolCallValues) && ToolCallValues && ToolCallValues->Num() > 0)
		{
			CloseThinkingSegment();
			AccumulateToolCalls(*ToolCallValues);
		}
	}

	FString Finish;
	if (Choice->TryGetStringField(TEXT("finish_reason"), Finish) && !Finish.IsEmpty())
	{
		ApplyFinishReason(Finish);
	}

	return bTextDelta;
}

void FOpenAIChatStreamState::AppendReasoning(const FString& Delta)
{
	OpenThinking.Append(Delta);
	if (OnThinkingDelta)
	{
		OnThinkingDelta(Delta);
	}
}

void FOpenAIChatStreamState::CloseThinkingSegment()
{
	if (OpenThinking.IsEmpty())
	{
		return;
	}
	if (OnThinkingComplete)
	{
		OnThinkingComplete(OpenThinking);
	}
	ThinkingSegments.Add(MoveTemp(OpenThinking));
	OpenThinking.Reset();
}

void FOpenAIChatStreamState::AccumulateToolCalls(const TArray<TSharedPtr<FJsonValue>>& ToolCallValues)
{
	for (const TSharedPtr<FJsonValue>& CallValue : ToolCallValues)
	{
		const TSharedPtr<FJsonObject>* CallPtr = nullptr;
		if (!CallValue.IsValid() || !CallValue->TryGetObject(CallPtr) || !CallPtr || !CallPtr->IsValid())
		{
			continue;
		}
		const TSharedPtr<FJsonObject>& Call = *CallPtr;
		const TSharedPtr<FJsonObject> Metadata = KismetAIProviderCodec::CopyWithout(Call,
			{ TEXT("index"), TEXT("id"), TEXT("type"), TEXT("function") });
		const TSharedPtr<FJsonObject>* NativeFunction = nullptr;
		if (Call->TryGetObjectField(TEXT("function"), NativeFunction))
		{
			const TSharedPtr<FJsonObject> FunctionMetadata = KismetAIProviderCodec::CopyWithout(*NativeFunction,
				{ TEXT("name"), TEXT("arguments") });
			if (!FunctionMetadata->Values.IsEmpty()) Metadata->SetObjectField(TEXT("function"), FunctionMetadata);
		}
		if (!Metadata->Values.IsEmpty())
		{
			if (Call->HasField(TEXT("id"))) Metadata->SetField(TEXT("id"), Call->TryGetField(TEXT("id")));
			if (Call->HasField(TEXT("index"))) Metadata->SetField(TEXT("index"), Call->TryGetField(TEXT("index")));
			NativeMetadataBlocks.Add(KismetAIProviderCodec::Metadata(TEXT("openai_chat"), Metadata));
		}

		int32 Index = 0;
		const bool bHasIndex = Call->TryGetNumberField(TEXT("index"), Index);
		FString Id;
		Call->TryGetStringField(TEXT("id"), Id);

		// Find the accumulator: by index (the spec), else by id, else the last
		// one (hosts that omit index send one call per fragment run).
		FToolCallAcc* Acc = nullptr;
		if (bHasIndex)
		{
			Acc = ToolCalls.FindByPredicate([Index](const FToolCallAcc& A) { return A.Index == Index; });
		}
		else if (!Id.IsEmpty())
		{
			Acc = ToolCalls.FindByPredicate([&Id](const FToolCallAcc& A) { return A.Id == Id; });
		}
		else if (ToolCalls.Num() > 0)
		{
			Acc = &ToolCalls.Last();
		}
		if (!Acc)
		{
			FToolCallAcc& New = ToolCalls.AddDefaulted_GetRef();
			New.Index = bHasIndex ? Index : ToolCalls.Num() - 1;
			Acc = &New;
		}

		if (!Id.IsEmpty() && Acc->Id.IsEmpty())
		{
			Acc->Id = Id;
		}

		const TSharedPtr<FJsonObject>* FunctionPtr = nullptr;
		if (Call->TryGetObjectField(TEXT("function"), FunctionPtr) && FunctionPtr && FunctionPtr->IsValid())
		{
			const TSharedPtr<FJsonObject>& Function = *FunctionPtr;
			FString Name;
			if (Function->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty() && Acc->Name.IsEmpty())
			{
				Acc->Name = Name;
			}
			const TSharedPtr<FJsonValue> Arguments = Function->TryGetField(TEXT("arguments"));
			if (Arguments.IsValid())
			{
				if (Arguments->Type == EJson::String)
				{
					Acc->Arguments.Append(Arguments->AsString());
				}
				else if (Arguments->Type == EJson::Object)
				{
					// Non-conforming hosts send the object itself.
					Acc->Arguments.Append(KismetAIOpenAIChat::SerializeCompact(Arguments->AsObject()));
				}
			}
		}
	}
}

void FOpenAIChatStreamState::ApplyFinishReason(const FString& Reason)
{
	FinishReason = Reason;
	bComplete = true;
	if (Reason == TEXT("error") && !bStreamError)
	{
		// OpenRouter ends a failed upstream with finish_reason "error".
		RecordStreamError(nullptr, TEXT("The model provider reported an error mid-stream"));
	}
	Finalize();
}

void FOpenAIChatStreamState::Finalize()
{
	if (bFinalized)
	{
		return;
	}
	bFinalized = true;
	CloseThinkingSegment();

	for (const FToolCallAcc& Acc : ToolCalls)
	{
		if (Acc.Name.IsEmpty())
		{
			bStreamError = true;
			StreamErrorCode = KismetAIOpenAIChat::MalformedToolCode;
			StreamErrorMessage = KismetAIOpenAIChat::MalformedToolMessage;
			KISMETAI_LOG(LogKismetAIOpenAIChat, Warning,
				TEXT("FOpenAIChatStreamState: tool call at index %d has no function name; rejecting the turn"), Acc.Index);
			ToolUseBlocks.Reset();
			return;
		}

		TSharedPtr<FJsonObject> Input;
		const FString Trimmed = Acc.Arguments.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			Input = MakeShared<FJsonObject>();
		}
		else
		{
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
			if (!FJsonSerializer::Deserialize(Reader, Input) || !Input.IsValid())
			{
				// Same rule as the Responses provider: one broken call rejects the
				// whole turn so no tool from a damaged response runs.
				bStreamError = true;
				StreamErrorCode = KismetAIOpenAIChat::MalformedToolCode;
				StreamErrorMessage = KismetAIOpenAIChat::MalformedToolMessage;
				KISMETAI_LOG(LogKismetAIOpenAIChat, Warning,
					TEXT("FOpenAIChatStreamState: tool '%s' arguments are not a JSON object (%d chars); rejecting the turn"),
					*Acc.Name, Acc.Arguments.Len());
				ToolUseBlocks.Reset();
				return;
			}
		}

		// Some hosts omit the id; the agent still needs one to pair the result.
		const FString Id = Acc.Id.IsEmpty()
			? FString::Printf(TEXT("call_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower())
			: Acc.Id;

		TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
		Block->SetStringField(TEXT("type"), TEXT("tool_use"));
		Block->SetStringField(TEXT("id"), Id);
		Block->SetStringField(TEXT("name"), Acc.Name);
		Block->SetObjectField(TEXT("input"), Input);
		ToolUseBlocks.Add(MakeShared<FJsonValueObject>(Block));
	}
}

void FOpenAIChatStreamState::ExtractUsage(const TSharedPtr<FJsonObject>& UsageObj)
{
	int32 PromptTokens = 0;
	int32 CompletionTokens = 0;
	const bool bPrompt = UsageObj->TryGetNumberField(TEXT("prompt_tokens"), PromptTokens);
	const bool bCompletion = UsageObj->TryGetNumberField(TEXT("completion_tokens"), CompletionTokens);
	if (!bPrompt && !bCompletion)
	{
		return;
	}

	int32 Cached = 0;
	const TSharedPtr<FJsonObject>* DetailsPtr = nullptr;
	if (UsageObj->TryGetObjectField(TEXT("prompt_tokens_details"), DetailsPtr) && DetailsPtr && DetailsPtr->IsValid()
		&& (*DetailsPtr)->TryGetNumberField(TEXT("cached_tokens"), Cached))
	{
		// OpenAI / OpenRouter / Gemini shape
	}
	else if (UsageObj->TryGetNumberField(TEXT("cached_tokens"), Cached))
	{
		// flat shape (some hosts)
	}
	else
	{
		UsageObj->TryGetNumberField(TEXT("prompt_cache_hit_tokens"), Cached);  // DeepSeek
	}
	Cached = FMath::Clamp(Cached, 0, FMath::Max(0, PromptTokens));

	// Anthropic disjoint semantics: input excludes the cached part.
	InputTokens = FMath::Max(0, PromptTokens - Cached);
	CacheReadTokens = Cached;
	OutputTokens = CompletionTokens;
	bHasUsage = true;
}

void FOpenAIChatStreamState::RecordStreamError(const TSharedPtr<FJsonObject>& ErrorObj, const FString& FallbackMessage)
{
	bStreamError = true;
	FString Message;
	FString Code;
	if (ErrorObj.IsValid())
	{
		ErrorObj->TryGetStringField(TEXT("message"), Message);
		if (!KismetAIOpenAIChat::ReadScalar(ErrorObj, TEXT("code"), Code) || Code.IsEmpty())
		{
			ErrorObj->TryGetStringField(TEXT("type"), Code);
		}
	}
	if (Message.IsEmpty())
	{
		Message = FallbackMessage;
	}
	if (!Message.IsEmpty())
	{
		StreamErrorMessage = Message;
	}
	if (!Code.IsEmpty())
	{
		StreamErrorCode = Code;
	}
	KISMETAI_LOG(LogKismetAIOpenAIChat, Warning,
		TEXT("FOpenAIChatStreamState: in-stream error (code='%s'): %s"),
		*StreamErrorCode, StreamErrorMessage.IsEmpty() ? TEXT("<no message>") : *StreamErrorMessage.Left(300));
}

bool FOpenAIChatStreamState::HasStreamError(FString& OutMessage, FString& OutCode) const
{
	if (!bStreamError)
	{
		return false;
	}
	OutMessage = StreamErrorMessage;
	OutCode = StreamErrorCode;
	return true;
}

TSharedPtr<FJsonObject> FOpenAIChatStreamState::BuildResponseJson() const
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();

	// Order: thinking, text, tool_use — the order the agent replays and shows.
	TArray<TSharedPtr<FJsonValue>> Content;
	auto AddThinking = [&Content](const FString& Thinking)
	{
		TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
		Block->SetStringField(TEXT("type"), TEXT("thinking"));
		Block->SetStringField(TEXT("thinking"), Thinking);
		Content.Add(MakeShared<FJsonValueObject>(Block));
	};
	for (const FString& Segment : ThinkingSegments)
	{
		AddThinking(Segment);
	}
	if (!OpenThinking.IsEmpty())
	{
		AddThinking(OpenThinking);  // only on a stream that never finished
	}
	if (!Text.IsEmpty())
	{
		TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
		Block->SetStringField(TEXT("type"), TEXT("text"));
		Block->SetStringField(TEXT("text"), Text);
		Content.Add(MakeShared<FJsonValueObject>(Block));
	}
	// Tool calls exist only once finish_reason arrived and every one parsed;
	// a cut-off stream never yields a partial tool call.
	if (bFinalized && !bStreamError)
	{
		Content.Append(ToolUseBlocks);
	}
	Content.Append(NativeMetadataBlocks);
	Response->SetArrayField(TEXT("content"), Content);

	const bool bHasTools = bFinalized && !bStreamError && ToolUseBlocks.Num() > 0;
	FString StopReason;
	if (FinishReason == TEXT("length"))
	{
		StopReason = TEXT("max_tokens");
	}
	else if (bHasTools)
	{
		// "tool_calls" (spec), "function_call" (legacy), or "stop" from hosts
		// that report stop even when they called tools.
		StopReason = TEXT("tool_use");
	}
	else
	{
		// "stop", "end_turn", "eos", "content_filter", or an unfinished stream —
		// the last is caught by stream_complete=false, not by this value.
		StopReason = TEXT("end_turn");
	}
	Response->SetStringField(TEXT("stop_reason"), StopReason);
	Response->SetBoolField(TEXT("stream_complete"), bComplete);
	Response->SetBoolField(TEXT("usage_complete"), bComplete && bHasUsage);

	if (bHasUsage)
	{
		TSharedPtr<FJsonObject> Usage = MakeShared<FJsonObject>();
		Usage->SetNumberField(TEXT("input_tokens"), InputTokens);
		Usage->SetNumberField(TEXT("output_tokens"), OutputTokens);
		Usage->SetNumberField(TEXT("cache_read_input_tokens"), CacheReadTokens);
		Usage->SetNumberField(TEXT("cache_creation_input_tokens"), 0);
		Response->SetObjectField(TEXT("usage"), Usage);
	}
	return Response;
}
