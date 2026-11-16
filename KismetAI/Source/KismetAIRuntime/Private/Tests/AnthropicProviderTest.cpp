// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

// PH1 parity test: FAnthropicProvider::BuildRequestBody must produce the same request
// body shape as the pre-refactor CallAnthropicOnce did.
// Guarded invariant: system array w/ cache_control, Anthropic tool shape, max_tokens.

#include "KismetAIAgent.h"
#include "KismetAILLMProvider.h"
#include "KismetAIAnthropicProvider.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

constexpr auto AnthropicProviderTestFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static FKismetAIAgentConfig MakeAnthropicConfig()
{
	FKismetAIAgentConfig Cfg;
	Cfg.Model = TEXT("claude-sonnet-4-6");
	Cfg.ApiKey = TEXT("test-key-abc");
	Cfg.ApiBaseUrl = TEXT("https://api.anthropic.com");
	Cfg.MaxTokens = 4096;
	Cfg.Temperature = 1.0f;
	Cfg.ThinkingMode = EThinkingMode::Adaptive;
	Cfg.EffortLevel = EEffortLevel::Medium;
	return Cfg;
}

// Build a minimal tool definition (Anthropic shape from tools.json)
static TSharedPtr<FJsonValue> MakeDummyTool(const FString& ToolName)
{
	TSharedPtr<FJsonObject> Tool = MakeShared<FJsonObject>();
	Tool->SetStringField(TEXT("name"), ToolName);
	Tool->SetStringField(TEXT("description"), TEXT("A test tool"));
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));
	Tool->SetObjectField(TEXT("input_schema"), Schema);
	return MakeShared<FJsonValueObject>(Tool);
}

// Build a minimal user message
static TSharedPtr<FJsonValue> MakeDummyMessage(const FString& Role, const FString& Text)
{
	TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("role"), Role);
	Msg->SetStringField(TEXT("content"), Text);
	return MakeShared<FJsonValueObject>(Msg);
}

// ─────────────────────────────────────────────────────────────────────────────
// Parity — body shape matches pre-refactor CallAnthropicOnce output
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnthropicProvider_BodyShape_TopLevelFields,
	"KismetAI.Provider.AnthropicBodyShape.TopLevelFields",
	AnthropicProviderTestFlags)
bool FAnthropicProvider_BodyShape_TopLevelFields::RunTest(const FString& Parameters)
{
	FAnthropicProvider Provider;
	FKismetAIAgentConfig Cfg = MakeAnthropicConfig();

	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(MakeDummyMessage(TEXT("user"), TEXT("Hello")));

	TArray<TSharedPtr<FJsonValue>> Tools;
	Tools.Add(MakeDummyTool(TEXT("read_blueprint")));

	FString SystemPrompt = TEXT("You are a helpful assistant.");
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, SystemPrompt, Shape);

	TestTrue(TEXT("Body is valid"), Body.IsValid());
	TestTrue(TEXT("model field present"), Body->HasField(TEXT("model")));
	TestTrue(TEXT("max_tokens field present"), Body->HasField(TEXT("max_tokens")));
	TestTrue(TEXT("stream field present"), Body->HasField(TEXT("stream")));
	TestTrue(TEXT("system field present"), Body->HasField(TEXT("system")));
	TestTrue(TEXT("tools field present"), Body->HasField(TEXT("tools")));
	TestTrue(TEXT("messages field present"), Body->HasField(TEXT("messages")));

	FString ModelVal;
	Body->TryGetStringField(TEXT("model"), ModelVal);
	TestEqual(TEXT("model == config model"), ModelVal, Cfg.Model);

	double MaxTokens = 0.0;
	Body->TryGetNumberField(TEXT("max_tokens"), MaxTokens);
	TestEqual(TEXT("max_tokens == config max_tokens"), (int32)MaxTokens, Cfg.MaxTokens);

	bool bStream = false;
	Body->TryGetBoolField(TEXT("stream"), bStream);
	TestTrue(TEXT("stream == true"), bStream);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnthropicProvider_BodyShape_SystemCacheControl,
	"KismetAI.Provider.AnthropicBodyShape.SystemArrayWithCacheControl",
	AnthropicProviderTestFlags)
bool FAnthropicProvider_BodyShape_SystemCacheControl::RunTest(const FString& Parameters)
{
	// System array must have exactly one block with type=text, text=<prompt>,
	// and cache_control.type=ephemeral — matching the pre-refactor body.
	FAnthropicProvider Provider;
	FKismetAIAgentConfig Cfg = MakeAnthropicConfig();

	TArray<TSharedPtr<FJsonValue>> Messages;
	TArray<TSharedPtr<FJsonValue>> Tools;
	FString SystemPrompt = TEXT("System prompt text here.");
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, SystemPrompt, Shape);

	TestTrue(TEXT("Body valid"), Body.IsValid());

	const TArray<TSharedPtr<FJsonValue>>* SystemArray;
	TestTrue(TEXT("system is an array"), Body->TryGetArrayField(TEXT("system"), SystemArray));
	TestEqual(TEXT("system array has exactly one block"), SystemArray->Num(), 1);

	const TSharedPtr<FJsonObject>* SysBlock;
	TestTrue(TEXT("system[0] is an object"), (*SystemArray)[0]->TryGetObject(SysBlock));

	FString SysType;
	(*SysBlock)->TryGetStringField(TEXT("type"), SysType);
	TestEqual(TEXT("system[0].type == 'text'"), SysType, FString(TEXT("text")));

	FString SysText;
	(*SysBlock)->TryGetStringField(TEXT("text"), SysText);
	TestEqual(TEXT("system[0].text == SystemPrompt"), SysText, SystemPrompt);

	// cache_control must be present and have type=ephemeral
	const TSharedPtr<FJsonObject>* CacheControl;
	TestTrue(TEXT("system[0] has cache_control"), (*SysBlock)->TryGetObjectField(TEXT("cache_control"), CacheControl));

	FString CacheType;
	(*CacheControl)->TryGetStringField(TEXT("type"), CacheType);
	TestEqual(TEXT("cache_control.type == 'ephemeral'"), CacheType, FString(TEXT("ephemeral")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnthropicProvider_BodyShape_ToolsPassthrough,
	"KismetAI.Provider.AnthropicBodyShape.ToolsArrayPassthrough",
	AnthropicProviderTestFlags)
bool FAnthropicProvider_BodyShape_ToolsPassthrough::RunTest(const FString& Parameters)
{
	// Tools array is passed through verbatim (Anthropic-native shape from tools.json)
	FAnthropicProvider Provider;
	FKismetAIAgentConfig Cfg = MakeAnthropicConfig();

	TArray<TSharedPtr<FJsonValue>> Messages;
	TArray<TSharedPtr<FJsonValue>> Tools;
	Tools.Add(MakeDummyTool(TEXT("read_blueprint")));
	Tools.Add(MakeDummyTool(TEXT("write_blueprint")));

	FString SystemPrompt = TEXT("prompt");
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, SystemPrompt, Shape);

	const TArray<TSharedPtr<FJsonValue>>* ToolsArray;
	TestTrue(TEXT("tools is an array"), Body->TryGetArrayField(TEXT("tools"), ToolsArray));
	TestEqual(TEXT("tools array has correct count"), ToolsArray->Num(), 2);

	// Verify tool names are preserved
	const TSharedPtr<FJsonObject>* Tool0;
	TestTrue(TEXT("tools[0] is object"), (*ToolsArray)[0]->TryGetObject(Tool0));
	FString ToolName;
	(*Tool0)->TryGetStringField(TEXT("name"), ToolName);
	TestEqual(TEXT("tools[0].name preserved"), ToolName, FString(TEXT("read_blueprint")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnthropicProvider_BodyShape_MessagesPassthrough,
	"KismetAI.Provider.AnthropicBodyShape.MessagesArrayPassthrough",
	AnthropicProviderTestFlags)
bool FAnthropicProvider_BodyShape_MessagesPassthrough::RunTest(const FString& Parameters)
{
	FAnthropicProvider Provider;
	FKismetAIAgentConfig Cfg = MakeAnthropicConfig();

	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(MakeDummyMessage(TEXT("user"), TEXT("Hello")));
	Messages.Add(MakeDummyMessage(TEXT("assistant"), TEXT("Hi there")));

	TArray<TSharedPtr<FJsonValue>> Tools;
	FString SystemPrompt = TEXT("prompt");
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, SystemPrompt, Shape);

	const TArray<TSharedPtr<FJsonValue>>* MsgArray;
	TestTrue(TEXT("messages is an array"), Body->TryGetArrayField(TEXT("messages"), MsgArray));
	TestEqual(TEXT("messages array has correct count"), MsgArray->Num(), 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnthropicProvider_BodyShape_MidConversationSystemPassthrough,
	"KismetAI.Provider.AnthropicBodyShape.MidConversationSystemPassthrough",
	AnthropicProviderTestFlags)
bool FAnthropicProvider_BodyShape_MidConversationSystemPassthrough::RunTest(const FString& Parameters)
{
	// Opus 4.8 mid-conversation system message: a {"role":"system"} entry appended to
	// the messages array must survive BuildRequestBody verbatim (the provider passes the
	// messages array through). The top-level `system` block is still the start-of-convo prompt.
	FAnthropicProvider Provider;
	FKismetAIAgentConfig Cfg = MakeAnthropicConfig();
	Cfg.Model = TEXT("claude-opus-4-8");

	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(MakeDummyMessage(TEXT("user"), TEXT("Review this graph.")));
	Messages.Add(MakeDummyMessage(TEXT("assistant"), TEXT("Done.")));
	Messages.Add(MakeDummyMessage(TEXT("user"), TEXT("Now wire the nodes.")));
	Messages.Add(FKismetAIAgent::MakeSystemMessage(TEXT("From now on, always compile after writing.")));

	TArray<TSharedPtr<FJsonValue>> Tools;
	FString SystemPrompt = TEXT("You are a Blueprint assistant.");
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, SystemPrompt, Shape);

	const TArray<TSharedPtr<FJsonValue>>* MsgArray;
	TestTrue(TEXT("messages is an array"), Body->TryGetArrayField(TEXT("messages"), MsgArray));
	TestEqual(TEXT("all four messages preserved"), MsgArray->Num(), 4);

	const TSharedPtr<FJsonObject>* LastMsg;
	TestTrue(TEXT("messages[3] is object"), (*MsgArray)[3]->TryGetObject(LastMsg));
	FString Role;
	(*LastMsg)->TryGetStringField(TEXT("role"), Role);
	TestEqual(TEXT("messages[3].role == 'system' (mid-conversation system survives)"),
		Role, FString(TEXT("system")));

	// Top-level system block remains the conversation-start prompt.
	const TArray<TSharedPtr<FJsonValue>>* SystemArray;
	TestTrue(TEXT("top-level system array present"), Body->TryGetArrayField(TEXT("system"), SystemArray));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnthropicProvider_BodyShape_ThinkingShape,
	"KismetAI.Provider.AnthropicBodyShape.ThinkingFieldShape",
	AnthropicProviderTestFlags)
bool FAnthropicProvider_BodyShape_ThinkingShape::RunTest(const FString& Parameters)
{
	// With Adaptive thinking + claude-sonnet-4-6, bThinkingEnabled must be set
	// and thinking field must be present (same as pre-refactor).
	FAnthropicProvider Provider;
	FKismetAIAgentConfig Cfg = MakeAnthropicConfig(); // Adaptive + Sonnet 4.6
	Cfg.ThinkingMode = EThinkingMode::Adaptive;

	TArray<TSharedPtr<FJsonValue>> Messages;
	TArray<TSharedPtr<FJsonValue>> Tools;
	FString SystemPrompt = TEXT("prompt");
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, SystemPrompt, Shape);

	TestTrue(TEXT("FLLMRequestShape.bThinkingEnabled set"), Shape.bThinkingEnabled);
	TestTrue(TEXT("thinking field present in body"), Body->HasField(TEXT("thinking")));

	const TSharedPtr<FJsonObject>* ThinkingObj;
	Body->TryGetObjectField(TEXT("thinking"), ThinkingObj);
	if (!ThinkingObj)
	{
		AddError(TEXT("thinking field is not an object"));
		return false;
	}
	FString ThinkingType;
	(*ThinkingObj)->TryGetStringField(TEXT("type"), ThinkingType);
	TestEqual(TEXT("thinking.type == 'adaptive'"), ThinkingType, FString(TEXT("adaptive")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnthropicProvider_EndpointUrl,
	"KismetAI.Provider.AnthropicBodyShape.EndpointUrl",
	AnthropicProviderTestFlags)
bool FAnthropicProvider_EndpointUrl::RunTest(const FString& Parameters)
{
	FAnthropicProvider Provider;
	FKismetAIAgentConfig Cfg = MakeAnthropicConfig();

	FString Url = Provider.GetEndpointUrl(Cfg);
	TestEqual(TEXT("Endpoint URL ends with /v1/messages"),
		Url, FString(TEXT("https://api.anthropic.com/v1/messages")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnthropicProvider_ExtraCurlHeaders,
	"KismetAI.Provider.AnthropicBodyShape.ExtraCurlHeaders",
	AnthropicProviderTestFlags)
bool FAnthropicProvider_ExtraCurlHeaders::RunTest(const FString& Parameters)
{
	// GetExtraCurlHeaders must include x-api-key and anthropic-version headers
	FAnthropicProvider Provider;
	FKismetAIAgentConfig Cfg = MakeAnthropicConfig();
	Cfg.ApiKey = TEXT("sk-test-1234");

	FLLMRequestShape Shape;
	Shape.bThinkingEnabled = false;
	Shape.bEffortEnabled = false;

	TArray<FString> Headers = Provider.GetExtraCurlHeaders(Cfg, Shape);

	bool bHasApiKey = false;
	bool bHasVersion = false;
	for (const FString& H : Headers)
	{
		if (H.Contains(TEXT("x-api-key")) && H.Contains(TEXT("sk-test-1234")))
		{
			bHasApiKey = true;
		}
		if (H.Contains(TEXT("anthropic-version")) && H.Contains(TEXT("2023-06-01")))
		{
			bHasVersion = true;
		}
	}
	TestTrue(TEXT("x-api-key header present with correct key"), bHasApiKey);
	TestTrue(TEXT("anthropic-version header present"), bHasVersion);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnthropicProvider_MakeProvider,
	"KismetAI.Provider.AnthropicBodyShape.MakeProviderForModel",
	AnthropicProviderTestFlags)
bool FAnthropicProvider_MakeProvider::RunTest(const FString& Parameters)
{
	// MakeProviderForModel must return a valid provider for Anthropic models
	TUniquePtr<ILLMProvider> Provider = MakeProviderForModel(TEXT("claude-sonnet-4-6"));
	TestTrue(TEXT("MakeProviderForModel returns valid provider for claude-sonnet-4-6"), Provider.IsValid());

	if (Provider.IsValid())
	{
		TestEqual(TEXT("Provider name is 'anthropic'"), Provider->GetName(), FString(TEXT("anthropic")));
	}

	TUniquePtr<ILLMProvider> Provider2 = MakeProviderForModel(TEXT("claude-opus-4-7"));
	TestTrue(TEXT("MakeProviderForModel returns valid provider for claude-opus-4-7"), Provider2.IsValid());

	// The new flagship still routes to the Anthropic provider (not OpenAI).
	TUniquePtr<ILLMProvider> Provider3 = MakeProviderForModel(TEXT("claude-opus-4-8"));
	TestTrue(TEXT("MakeProviderForModel returns valid provider for claude-opus-4-8"), Provider3.IsValid());
	if (Provider3.IsValid())
	{
		TestEqual(TEXT("claude-opus-4-8 routes to anthropic provider"),
			Provider3->GetName(), FString(TEXT("anthropic")));
	}

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Message-level prompt-cache breakpoint (transcript caching)
// ─────────────────────────────────────────────────────────────────────────────

// Count cache_control objects across all content blocks of all messages in a body.
static int32 CountMessageCacheBreakpoints(const TSharedPtr<FJsonObject>& Body)
{
	int32 Count = 0;
	const TArray<TSharedPtr<FJsonValue>>* MsgArray = nullptr;
	if (!Body->TryGetArrayField(TEXT("messages"), MsgArray) || !MsgArray)
	{
		return 0;
	}
	for (const TSharedPtr<FJsonValue>& MsgVal : *MsgArray)
	{
		TSharedPtr<FJsonObject> MsgObj = MsgVal.IsValid() ? MsgVal->AsObject() : nullptr;
		if (!MsgObj.IsValid()) continue;
		const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
		if (MsgObj->TryGetArrayField(TEXT("content"), Content) && Content)
		{
			for (const TSharedPtr<FJsonValue>& BlockVal : *Content)
			{
				TSharedPtr<FJsonObject> BlockObj = BlockVal.IsValid() ? BlockVal->AsObject() : nullptr;
				if (BlockObj.IsValid() && BlockObj->HasField(TEXT("cache_control")))
				{
					Count++;
				}
			}
		}
	}
	return Count;
}

// Build a message whose content is already an array of blocks (e.g. tool_result).
static TSharedPtr<FJsonValue> MakeArrayContentMessage(const FString& Role, const FString& Text)
{
	TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
	Block->SetStringField(TEXT("type"), TEXT("text"));
	Block->SetStringField(TEXT("text"), Text);

	TArray<TSharedPtr<FJsonValue>> Content;
	Content.Add(MakeShared<FJsonValueObject>(Block));

	TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("role"), Role);
	Msg->SetArrayField(TEXT("content"), Content);
	return MakeShared<FJsonValueObject>(Msg);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnthropicProvider_CacheBreakpoint_LastBlockOnly,
	"KismetAI.Provider.AnthropicBodyShape.CacheBreakpointLastBlockOnly",
	AnthropicProviderTestFlags)
bool FAnthropicProvider_CacheBreakpoint_LastBlockOnly::RunTest(const FString& Parameters)
{
	// Exactly one message-level cache breakpoint, on the last message, so the whole
	// transcript prefix is cached. A string-content last message is converted to a
	// single text block so the breakpoint has somewhere to attach.
	FAnthropicProvider Provider;
	FKismetAIAgentConfig Cfg = MakeAnthropicConfig();

	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(MakeDummyMessage(TEXT("user"), TEXT("Read the graph")));
	Messages.Add(MakeDummyMessage(TEXT("assistant"), TEXT("Reading...")));
	Messages.Add(MakeDummyMessage(TEXT("user"), TEXT("Now wire the nodes")));

	TArray<TSharedPtr<FJsonValue>> Tools;
	FString SystemPrompt = TEXT("prompt");
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, SystemPrompt, Shape);

	TestEqual(TEXT("exactly one message cache breakpoint"), CountMessageCacheBreakpoints(Body), 1);

	// The breakpoint must be on the LAST message, whose string content is now an array.
	const TArray<TSharedPtr<FJsonValue>>* MsgArray = nullptr;
	Body->TryGetArrayField(TEXT("messages"), MsgArray);
	if (!MsgArray || MsgArray->Num() != 3)
	{
		AddError(TEXT("expected 3 messages"));
		return false;
	}
	TSharedPtr<FJsonObject> LastMsg = (*MsgArray)[2]->AsObject();
	const TArray<TSharedPtr<FJsonValue>>* LastContent = nullptr;
	TestTrue(TEXT("last message content is an array"),
		LastMsg->TryGetArrayField(TEXT("content"), LastContent) && LastContent != nullptr);
	if (LastContent && LastContent->Num() > 0)
	{
		TSharedPtr<FJsonObject> LastBlock = LastContent->Last()->AsObject();
		const TSharedPtr<FJsonObject>* CC = nullptr;
		TestTrue(TEXT("last block has cache_control"), LastBlock->TryGetObjectField(TEXT("cache_control"), CC));
		if (CC)
		{
			FString CCType;
			(*CC)->TryGetStringField(TEXT("type"), CCType);
			TestEqual(TEXT("cache_control.type == ephemeral"), CCType, FString(TEXT("ephemeral")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnthropicProvider_CacheBreakpoint_Idempotent,
	"KismetAI.Provider.AnthropicBodyShape.CacheBreakpointIdempotent",
	AnthropicProviderTestFlags)
bool FAnthropicProvider_CacheBreakpoint_Idempotent::RunTest(const FString& Parameters)
{
	// Repeated BuildRequestBody calls on the same shared messages (the retry path)
	// must clean-slate prior breakpoints, never accumulating past one.
	FAnthropicProvider Provider;
	FKismetAIAgentConfig Cfg = MakeAnthropicConfig();

	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(MakeArrayContentMessage(TEXT("user"), TEXT("First")));
	Messages.Add(MakeArrayContentMessage(TEXT("assistant"), TEXT("Second")));
	Messages.Add(MakeArrayContentMessage(TEXT("user"), TEXT("Third")));

	TArray<TSharedPtr<FJsonValue>> Tools;
	FString SystemPrompt = TEXT("prompt");
	FLLMRequestShape Shape;

	TSharedPtr<FJsonObject> Body1 = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, SystemPrompt, Shape);
	TestEqual(TEXT("one breakpoint after first build"), CountMessageCacheBreakpoints(Body1), 1);

	TSharedPtr<FJsonObject> Body2 = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, SystemPrompt, Shape);
	TestEqual(TEXT("still one breakpoint after second build (clean-slate)"),
		CountMessageCacheBreakpoints(Body2), 1);

	return true;
}
