// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIOpenAIProvider.h"
#include "KismetAILog.h"
#include "KismetAIAgent.h"
#include "KismetAILLMProvider.h"
#include "KismetAIModelCatalog.h"
#include "OpenAIResponsesSSEParser.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "HAL/PlatformMisc.h"
#include "Internationalization/Regex.h"

DEFINE_LOG_CATEGORY_STATIC(LogKismetAIOpenAIProvider, Log, All);
DEFINE_LOG_CATEGORY(LogKismetAIOpenAIStream);

// ─────────────────────────────────────────────────────────────────────────────
// Local JSON serialization helpers
// ─────────────────────────────────────────────────────────────────────────────

// Compact writer type aliases (mirrors KismetAIAgent.cpp)
typedef TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>        FCondensedJsonWriter;
typedef TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>> FCondensedJsonWriterFactory;

// ─────────────────────────────────────────────────────────────────────────────
// FOpenAIProvider — static helpers
// ─────────────────────────────────────────────────────────────────────────────

bool FOpenAIProvider::IsReasoningModel(const FString& Model)
{
	return FKismetAIModelCatalog::Get().FindOrDefaults(Model).Caps.bReasoning;
}

FString FOpenAIProvider::MapEffortLevel(EEffortLevel Level, const FString& Model)
{
	// reasoning.effort support is model-specific. The catalog lists what each
	// model accepts; a model with no list gets low/medium/high, the subset every
	// reasoning model takes. Requests above the model's ceiling are clamped down.
	TArray<FString> Supported = FKismetAIModelCatalog::Get().FindOrDefaults(Model).Caps.EffortLevels;
	if (Supported.Num() == 0)
	{
		Supported = { TEXT("low"), TEXT("medium"), TEXT("high") };
	}
	const FString Clamped = FKismetAIModelCatalog::ClampEffort(Supported, FKismetAIAgent::EffortLevelToString(Level));
	return Clamped.IsEmpty() ? FString(TEXT("high")) : Clamped;
}

FString FOpenAIProvider::SerializeJsonObject(const TSharedPtr<FJsonObject>& Obj)
{
	if (!Obj.IsValid())
	{
		return TEXT("{}");
	}
	FString Output;
	TSharedRef<FCondensedJsonWriter> Writer = FCondensedJsonWriterFactory::Create(&Output);
	FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
	return Output;
}

void FOpenAIProvider::TranslateMessage(
	const TSharedPtr<FJsonObject>& CanonicalMsg,
	TArray<TSharedPtr<FJsonValue>>& OutItems)
{
	if (!CanonicalMsg.IsValid())
	{
		return;
	}

	FString Role;
	CanonicalMsg->TryGetStringField(TEXT("role"), Role);

	// ── User messages ──────────────────────────────────────────────────────
	if (Role == TEXT("user"))
	{
		// Content may be: plain string OR array of content blocks
		FString ContentStr;
		if (CanonicalMsg->TryGetStringField(TEXT("content"), ContentStr))
		{
			// Plain string — emit {role:user, content:<string>}
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("role"), TEXT("user"));
			Item->SetStringField(TEXT("content"), ContentStr);
			OutItems.Add(MakeShared<FJsonValueObject>(Item));
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* ContentArray = nullptr;
		if (!CanonicalMsg->TryGetArrayField(TEXT("content"), ContentArray))
		{
			// No content at all — skip
			return;
		}

		// Walk content array: may be text blocks OR tool_result blocks
		for (const TSharedPtr<FJsonValue>& BlockValue : *ContentArray)
		{
			const TSharedPtr<FJsonObject>* BlockPtr = nullptr;
			if (!BlockValue->TryGetObject(BlockPtr))
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& Block = *BlockPtr;

			FString BlockType;
			Block->TryGetStringField(TEXT("type"), BlockType);

			if (BlockType == TEXT("tool_result"))
			{
				// Emit function_call_output
				FString ToolUseId;
				Block->TryGetStringField(TEXT("tool_use_id"), ToolUseId);
				FString OutputContent;
				Block->TryGetStringField(TEXT("content"), OutputContent);

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("type"), TEXT("function_call_output"));
				Item->SetStringField(TEXT("call_id"), ToolUseId);
				Item->SetStringField(TEXT("output"), OutputContent);
				OutItems.Add(MakeShared<FJsonValueObject>(Item));
			}
			else if (BlockType == TEXT("text"))
			{
				// User text in content array
				FString Text;
				Block->TryGetStringField(TEXT("text"), Text);

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("role"), TEXT("user"));
				Item->SetStringField(TEXT("content"), Text);
				OutItems.Add(MakeShared<FJsonValueObject>(Item));
			}
			// Other user block types (image, etc.) — not currently supported; skip silently
		}
		return;
	}

	// ── Assistant messages ─────────────────────────────────────────────────
	if (Role == TEXT("assistant"))
	{
		const TArray<TSharedPtr<FJsonValue>>* ContentArray = nullptr;
		if (!CanonicalMsg->TryGetArrayField(TEXT("content"), ContentArray))
		{
			// No content array — skip
			return;
		}

		// First collect any text content (non-thinking, non-tool_use blocks)
		FString AccumulatedText;
		for (const TSharedPtr<FJsonValue>& BlockValue : *ContentArray)
		{
			const TSharedPtr<FJsonObject>* BlockPtr = nullptr;
			if (!BlockValue->TryGetObject(BlockPtr))
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& Block = *BlockPtr;

			FString BlockType;
			Block->TryGetStringField(TEXT("type"), BlockType);

			if (BlockType == TEXT("text"))
			{
				FString Text;
				Block->TryGetStringField(TEXT("text"), Text);
				if (!AccumulatedText.IsEmpty())
				{
					AccumulatedText += TEXT("\n");
				}
				AccumulatedText += Text;
			}
			// thinking / redacted_thinking — DROP (plan: can't replay, OpenAI manages server-side)
		}

		// Emit assistant text item only if we have non-empty text
		if (!AccumulatedText.IsEmpty())
		{
			TSharedPtr<FJsonObject> TextItem = MakeShared<FJsonObject>();
			TextItem->SetStringField(TEXT("role"), TEXT("assistant"));
			TextItem->SetStringField(TEXT("content"), AccumulatedText);
			OutItems.Add(MakeShared<FJsonValueObject>(TextItem));
		}

		// Then emit function_call items for each tool_use block
		for (const TSharedPtr<FJsonValue>& BlockValue : *ContentArray)
		{
			const TSharedPtr<FJsonObject>* BlockPtr = nullptr;
			if (!BlockValue->TryGetObject(BlockPtr))
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& Block = *BlockPtr;

			FString BlockType;
			Block->TryGetStringField(TEXT("type"), BlockType);

			if (BlockType == TEXT("tool_use"))
			{
				FString ToolId;
				if (!Block->TryGetStringField(TEXT("id"), ToolId) || ToolId.IsEmpty())
				{
					// Malformed: missing id — cannot round-trip. Log and skip (plan note 4).
					KISMETAI_LOG(LogKismetAIOpenAIProvider, Error,
						TEXT("FOpenAIProvider: tool_use block missing 'id' field — skipping (cannot round-trip)"));
					continue;
				}

				FString ToolName;
				if (!Block->TryGetStringField(TEXT("name"), ToolName) || ToolName.IsEmpty())
				{
					// Malformed: missing name. Log and skip.
					KISMETAI_LOG(LogKismetAIOpenAIProvider, Error,
						TEXT("FOpenAIProvider: tool_use block missing 'name' field — skipping"));
					continue;
				}

				// Serialize the input object to a JSON string (arguments must be a string)
				const TSharedPtr<FJsonObject>* InputObjPtr = nullptr;
				TSharedPtr<FJsonObject> InputObj;
				if (Block->TryGetObjectField(TEXT("input"), InputObjPtr) && InputObjPtr)
				{
					InputObj = *InputObjPtr;
				}
				else
				{
					InputObj = MakeShared<FJsonObject>();
				}
				FString ArgumentsStr = SerializeJsonObject(InputObj);

				TSharedPtr<FJsonObject> FuncCallItem = MakeShared<FJsonObject>();
				FuncCallItem->SetStringField(TEXT("type"), TEXT("function_call"));
				FuncCallItem->SetStringField(TEXT("call_id"), ToolId);
				FuncCallItem->SetStringField(TEXT("name"), ToolName);
				FuncCallItem->SetStringField(TEXT("arguments"), ArgumentsStr);
				OutItems.Add(MakeShared<FJsonValueObject>(FuncCallItem));
			}
		}
		return;
	}

	// Unknown role — skip with a log
	KISMETAI_LOG(LogKismetAIOpenAIProvider, Warning,
		TEXT("FOpenAIProvider::TranslateMessage: unknown role '%s' — skipping"), *Role);
}

// ─────────────────────────────────────────────────────────────────────────────
// ILLMProvider implementation
// ─────────────────────────────────────────────────────────────────────────────

FOpenAIProvider::FOpenAIProvider()
{
	FKismetAIAgentConfig Default;
	Default.ProviderId = TEXT("openai");
	Profile = KismetAIProviders::ResolveProfile(Default);
	bFollowResolvedProfile = true;
}

const FKismetAIProviderProfile& FOpenAIProvider::ProfileFor(const FKismetAIAgentConfig& Config) const
{
	if (bFollowResolvedProfile)
	{
		const FKismetAIProviderProfile& Resolved = KismetAIProviders::ResolveProfile(Config);
		if (Resolved.Protocol == EKismetAIWireProtocol::OpenAIResponses)
		{
			return Resolved;
		}
	}
	return Profile;
}

FOpenAIProvider::FOpenAIProvider(const FKismetAIProviderProfile& InProfile)
	: Profile(InProfile)
{
}

FString FOpenAIProvider::GetName() const
{
	return TEXT("openai");
}

int32 FOpenAIProvider::CacheTtlSeconds() const
{
	// 300s is conservative for OpenAI in_memory prompt caching.
	// Becomes 86400 if prompt_cache_retention:"24h" is ever set (AS2).
	return 300;
}

FString FOpenAIProvider::GetEndpointUrl(const FKismetAIAgentConfig& Config) const
{
	// The profile decides the host: api.openai.com (OPENAI_BASE_URL or a settings
	// override allowed). Config.ApiBaseUrl is the Anthropic base and is never
	// used here.
	return KismetAIProviders::GetBaseUrl(Config, ProfileFor(Config)) + TEXT("/v1/responses");
}

TSharedPtr<FJsonObject> FOpenAIProvider::BuildRequestBody(
	const FKismetAIAgentConfig& Config,
	const TArray<FAgentMessage>& CanonicalMessages,
	const TArray<TSharedPtr<FJsonValue>>& Tools,
	const FString& SystemPrompt,
	FLLMRequestShape& OutShape) const
{
	const TArray<TSharedPtr<FJsonValue>> Messages = KismetAIProviderCodec::EncodeMessages(CanonicalMessages);
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();

	// ── Mandatory top-level fields ─────────────────────────────────────────
	// A profile may strip a route suffix from the model id before it goes on the wire.
	const FString WireModel = KismetAIProviders::WireModelId(Config, ProfileFor(Config));
	Body->SetStringField(TEXT("model"), WireModel);
	Body->SetBoolField(TEXT("stream"), true);
	Body->SetNumberField(TEXT("max_output_tokens"), Config.MaxTokens);
	Body->SetStringField(TEXT("instructions"), SystemPrompt);

	// ── Reasoning vs. non-reasoning ───────────────────────────────────────
	const bool bReasoning = IsReasoningModel(Config.Model);
	if (bReasoning)
	{
		FString EffortStr = MapEffortLevel(Config.EffortLevel, Config.Model);

		TSharedPtr<FJsonObject> ReasoningObj = MakeShared<FJsonObject>();
		ReasoningObj->SetStringField(TEXT("effort"), EffortStr);
		ReasoningObj->SetStringField(TEXT("summary"), TEXT("auto"));
		Body->SetObjectField(TEXT("reasoning"), ReasoningObj);

		// temperature / top_p must be omitted for reasoning models
		OutShape.bEffortEnabled = true;
	}
	else
	{
		// Non-reasoning alone does not establish sampling support for unknown models.
		if (FKismetAIModelCatalog::Get().FindOrDefaults(Config.Model).Caps.bSamplingParams)
		{
			Body->SetNumberField(TEXT("temperature"), static_cast<double>(Config.Temperature));
		}
		OutShape.bEffortEnabled = false;
	}
	// Fast mode = OpenAI's fast tier ("priority" is accepted on every model and
	// resolves to the fast tier where a model has one). Without this the cost
	// estimate doubled while the request still ran on the default tier.
	if (Config.bFastMode
		&& FKismetAIModelCatalog::Get().FindOrDefaults(Config.Model).Caps.FastModeWire == EKismetAIFastModeWire::OpenAIPriority)
	{
		Body->SetStringField(TEXT("service_tier"), TEXT("priority"));
	}

	// bThinkingEnabled stays false — OpenAI has no separate thinking toggle
	OutShape.bThinkingEnabled = false;

	// ── Tools transform: Anthropic shape -> OpenAI Responses shape ─────────
	// input_schema -> parameters; add type:"function"; omit strict
	TArray<TSharedPtr<FJsonValue>> OpenAITools;
	for (const TSharedPtr<FJsonValue>& ToolValue : Tools)
	{
		const TSharedPtr<FJsonObject>* ToolObjPtr = nullptr;
		if (!ToolValue->TryGetObject(ToolObjPtr))
		{
			continue;
		}
		const TSharedPtr<FJsonObject>& AnthropicTool = *ToolObjPtr;

		FString ToolName;
		AnthropicTool->TryGetStringField(TEXT("name"), ToolName);
		FString ToolDesc;
		AnthropicTool->TryGetStringField(TEXT("description"), ToolDesc);

		const TSharedPtr<FJsonObject>* SchemaPtr = nullptr;
		TSharedPtr<FJsonObject> Schema;
		if (AnthropicTool->TryGetObjectField(TEXT("input_schema"), SchemaPtr) && SchemaPtr)
		{
			Schema = *SchemaPtr;
		}
		else
		{
			Schema = MakeShared<FJsonObject>();
		}

		TSharedPtr<FJsonObject> OpenAITool = MakeShared<FJsonObject>();
		OpenAITool->SetStringField(TEXT("type"), TEXT("function"));
		OpenAITool->SetStringField(TEXT("name"), ToolName);
		OpenAITool->SetStringField(TEXT("description"), ToolDesc);
		OpenAITool->SetObjectField(TEXT("parameters"), Schema);
		// "strict" is omitted

		OpenAITools.Add(MakeShared<FJsonValueObject>(OpenAITool));
	}
	// tool_choice without tools is rejected; a tool-less request (the compaction
	// summary) omits both.
	if (OpenAITools.Num() > 0)
	{
		Body->SetArrayField(TEXT("tools"), OpenAITools);
		Body->SetStringField(TEXT("tool_choice"), TEXT("auto"));
	}

	// ── input[] translation ────────────────────────────────────────────────
	// AS1: full message history resent every call (stateless multi-turn)
	TArray<TSharedPtr<FJsonValue>> InputItems;
	for (const TSharedPtr<FJsonValue>& MsgValue : Messages)
	{
		const TSharedPtr<FJsonObject>* MsgObjPtr = nullptr;
		if (!MsgValue->TryGetObject(MsgObjPtr))
		{
			KISMETAI_LOG(LogKismetAIOpenAIProvider, Warning,
				TEXT("FOpenAIProvider::BuildRequestBody: skipping non-object message value"));
			continue;
		}
		TranslateMessage(*MsgObjPtr, InputItems);
	}
	Body->SetArrayField(TEXT("input"), InputItems);

	return Body;
}

TArray<FString> FOpenAIProvider::GetExtraCurlHeaders(
	const FKismetAIAgentConfig& Config,
	const FLLMRequestShape& /*Shape*/) const
{
	// Use OpenAIApiKey, never fall back to Config.ApiKey (Anthropic).
	// If empty, still emit the header — the upstream will 401.
	const FKismetAIProviderProfile& Effective = ProfileFor(Config);
	TArray<FString> Headers = KismetAIProviders::BuildAuthHeaders(Config, Effective);
	if (Effective.Id != TEXT("openai"))
	{
		return Headers;  // organization/project headers are OpenAI-account specific
	}

	// Optional organization and project headers from environment
	const FString OrgId = Config.bProviderEnvironmentResolved ? Config.ProviderOrganization
		: FPlatformMisc::GetEnvironmentVariable(TEXT("OPENAI_ORG_ID"));
	if (!OrgId.IsEmpty())
	{
		Headers.Add(FString::Printf(TEXT("OpenAI-Organization: %s"), *OrgId));
	}

	const FString ProjectId = Config.bProviderEnvironmentResolved ? Config.ProviderProject
		: FPlatformMisc::GetEnvironmentVariable(TEXT("OPENAI_PROJECT_ID"));
	if (!ProjectId.IsEmpty())
	{
		Headers.Add(FString::Printf(TEXT("OpenAI-Project: %s"), *ProjectId));
	}

	return Headers;
}

TSharedPtr<ILLMStreamState> FOpenAIProvider::CreateStreamState() const
{
	return MakeShared<FOpenAIStreamState>();
}

void FOpenAIProvider::ParseErrorBody(
	const TSharedPtr<FJsonObject>& Body,
	FModelCallOutcome& Outcome) const
{
	if (!Body.IsValid())
	{
		return;
	}

	// OpenAI error shape: {"error":{"message":"...","type":"...","code":"..."}}
	const TSharedPtr<FJsonObject>* ErrorObjPtr;
	if (!Body->TryGetObjectField(TEXT("error"), ErrorObjPtr))
	{
		// Not an OpenAI-shaped error body — leave Outcome fields empty
		KISMETAI_DEV_LOG(LogKismetAIOpenAIProvider, Warning,
			TEXT("FOpenAIProvider::ParseErrorBody: no 'error' object in response body"));
		return;
	}
	const TSharedPtr<FJsonObject>& ErrorObj = *ErrorObjPtr;

	FString Message;
	FString Type;
	FString Code;

	ErrorObj->TryGetStringField(TEXT("message"), Message);
	ErrorObj->TryGetStringField(TEXT("type"), Type);
	ErrorObj->TryGetStringField(TEXT("code"), Code);

	Outcome.ErrorMessage = Message;

	// Use code as ErrorType if more specific (e.g. "context_length_exceeded", "insufficient_quota")
	// because ClassifyCallOutcome keys off ErrorType.
	// When code is set and non-empty, prefer it; otherwise fall back to type.
	if (!Code.IsEmpty())
	{
		Outcome.ErrorType = Code;
	}
	else if (!Type.IsEmpty())
	{
		Outcome.ErrorType = Type;
	}

	// Best-effort regex parse for context overflow messages.
	// OpenAI pattern: "This model's maximum context length is N tokens. However, your messages
	// resulted in M tokens." (exact wording varies; we capture N and M where possible).
	//
	// On parse failure, leave InputTokens and ContextLimit at 0 (do NOT fabricate values).
	if (Code == TEXT("context_length_exceeded")
		|| Message.ToLower().Contains(TEXT("maximum context length")))
	{
		// Pattern: "maximum context length is N tokens"
		{
			FRegexPattern LimitPattern(TEXT("maximum context length is (\\d+)"));
			FRegexMatcher LimitMatcher(LimitPattern, Message);
			if (LimitMatcher.FindNext())
			{
				Outcome.ContextLimit = FCString::Atoi(*LimitMatcher.GetCaptureGroup(1));
			}
		}

		// Pattern: "resulted in N tokens" or "your messages resulted in N tokens"
		{
			FRegexPattern UsedPattern(TEXT("resulted in (\\d+) tokens"));
			FRegexMatcher UsedMatcher(UsedPattern, Message);
			if (UsedMatcher.FindNext())
			{
				Outcome.InputTokens = FCString::Atoi(*UsedMatcher.GetCaptureGroup(1));
			}
		}
	}
}
