// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

// PH3 tests: FOpenAIProvider::BuildRequestBody, GetEndpointUrl, GetExtraCurlHeaders.
// All tests under "KismetAI.Provider.OpenAIBodyShape.*"

#include "KismetAIAgent.h"
#include "KismetAILLMProvider.h"
#include "KismetAIOpenAIProvider.h"
#include "KismetAIModelCatalog.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/Char.h"

constexpr auto OpenAIProviderTestFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static FKismetAIAgentConfig MakeOpenAIConfig(const FString& ModelId = TEXT("gpt-4o"), float Temperature = 1.0f)
{
	FKismetAIAgentConfig Cfg;
	Cfg.Model = ModelId;
	Cfg.OpenAIApiKey = TEXT("sk-openai-test");
	Cfg.ApiKey = TEXT("sk-anthropic-should-not-be-used");
	Cfg.ApiBaseUrl = TEXT("https://api.anthropic.com");  // should NOT be used by OpenAI provider
	Cfg.MaxTokens = 4096;
	Cfg.Temperature = Temperature;
	Cfg.EffortLevel = EEffortLevel::Medium;
	return Cfg;
}

// Build an Anthropic-shaped user text message (content as plain string)
static TSharedPtr<FJsonValue> MakeUserTextMessage(const FString& Text)
{
	TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("role"), TEXT("user"));
	Msg->SetStringField(TEXT("content"), Text);
	return MakeShared<FJsonValueObject>(Msg);
}

// Build an Anthropic-shaped user text message (content as array)
static TSharedPtr<FJsonValue> MakeUserTextMessageArray(const FString& Text)
{
	TSharedPtr<FJsonObject> TextBlock = MakeShared<FJsonObject>();
	TextBlock->SetStringField(TEXT("type"), TEXT("text"));
	TextBlock->SetStringField(TEXT("text"), Text);

	TArray<TSharedPtr<FJsonValue>> ContentArray;
	ContentArray.Add(MakeShared<FJsonValueObject>(TextBlock));

	TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("role"), TEXT("user"));
	Msg->SetArrayField(TEXT("content"), ContentArray);
	return MakeShared<FJsonValueObject>(Msg);
}

// Build an Anthropic-shaped tool-use block
static TSharedPtr<FJsonObject> MakeToolUseBlock(const FString& Id, const FString& Name, const FString& ArgumentJson)
{
	TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
	Block->SetStringField(TEXT("type"), TEXT("tool_use"));
	Block->SetStringField(TEXT("id"), Id);
	Block->SetStringField(TEXT("name"), Name);

	// Parse the argument JSON string into a FJsonObject for the "input" field
	TSharedPtr<FJsonObject> InputObj = MakeShared<FJsonObject>();
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentJson);
	FJsonSerializer::Deserialize(Reader, InputObj);
	Block->SetObjectField(TEXT("input"), InputObj);

	return Block;
}

// Build an Anthropic-shaped assistant message with text and/or tool_use blocks
static TSharedPtr<FJsonValue> MakeAssistantMessage(const TArray<TSharedPtr<FJsonObject>>& Blocks)
{
	TArray<TSharedPtr<FJsonValue>> ContentArray;
	for (const TSharedPtr<FJsonObject>& Block : Blocks)
	{
		ContentArray.Add(MakeShared<FJsonValueObject>(Block));
	}

	TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("role"), TEXT("assistant"));
	Msg->SetArrayField(TEXT("content"), ContentArray);
	return MakeShared<FJsonValueObject>(Msg);
}

// Build an Anthropic-shaped thinking block
static TSharedPtr<FJsonObject> MakeThinkingBlock(const FString& ThinkingText)
{
	TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
	Block->SetStringField(TEXT("type"), TEXT("thinking"));
	Block->SetStringField(TEXT("thinking"), ThinkingText);
	Block->SetStringField(TEXT("signature"), TEXT("sig_abc"));
	return Block;
}

// Build an Anthropic-shaped tool_result user message (single result)
static TSharedPtr<FJsonValue> MakeToolResultMessage(const FString& ToolUseId, const FString& Output)
{
	TSharedPtr<FJsonObject> ResultBlock = MakeShared<FJsonObject>();
	ResultBlock->SetStringField(TEXT("type"), TEXT("tool_result"));
	ResultBlock->SetStringField(TEXT("tool_use_id"), ToolUseId);
	ResultBlock->SetStringField(TEXT("content"), Output);

	TArray<TSharedPtr<FJsonValue>> ContentArray;
	ContentArray.Add(MakeShared<FJsonValueObject>(ResultBlock));

	TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("role"), TEXT("user"));
	Msg->SetArrayField(TEXT("content"), ContentArray);
	return MakeShared<FJsonValueObject>(Msg);
}

// Build an Anthropic-shaped tool definition (same shape as tools.json)
static TSharedPtr<FJsonValue> MakeTool(const FString& Name, const FString& Desc)
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));
	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetObjectField(TEXT("blueprint_name"), MakeShared<FJsonObject>());
	Schema->SetObjectField(TEXT("properties"), Props);

	TSharedPtr<FJsonObject> Tool = MakeShared<FJsonObject>();
	Tool->SetStringField(TEXT("name"), Name);
	Tool->SetStringField(TEXT("description"), Desc);
	Tool->SetObjectField(TEXT("input_schema"), Schema);
	return MakeShared<FJsonValueObject>(Tool);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.1 — Top-level body shape
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIProvider_BodyShape_TopLevel,
	"KismetAI.Provider.OpenAIBodyShape.TopLevelFields",
	OpenAIProviderTestFlags)
bool FOpenAIProvider_BodyShape_TopLevel::RunTest(const FString& Parameters)
{
	FOpenAIProvider Provider;
	FKismetAIAgentConfig Cfg = MakeOpenAIConfig();

	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(MakeUserTextMessage(TEXT("Hello")));

	TArray<TSharedPtr<FJsonValue>> Tools;
	Tools.Add(MakeTool(TEXT("read_blueprint"), TEXT("Reads a blueprint")));

	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools,
		TEXT("You are a helpful assistant."), Shape);

	TestTrue(TEXT("Body is valid"), Body.IsValid());

	// Required top-level fields
	TestTrue(TEXT("model field present"), Body->HasField(TEXT("model")));
	TestTrue(TEXT("stream field present"), Body->HasField(TEXT("stream")));
	TestTrue(TEXT("max_output_tokens present"), Body->HasField(TEXT("max_output_tokens")));
	TestTrue(TEXT("instructions present"), Body->HasField(TEXT("instructions")));
	TestTrue(TEXT("input array present"), Body->HasField(TEXT("input")));
	TestTrue(TEXT("tools array present"), Body->HasField(TEXT("tools")));
	TestTrue(TEXT("tool_choice present"), Body->HasField(TEXT("tool_choice")));

	// Must NOT use Anthropic field names
	TestFalse(TEXT("max_tokens NOT present (OpenAI uses max_output_tokens)"),
		Body->HasField(TEXT("max_tokens")));
	TestFalse(TEXT("messages NOT present (OpenAI uses input)"),
		Body->HasField(TEXT("messages")));
	TestFalse(TEXT("system NOT present (OpenAI uses instructions)"),
		Body->HasField(TEXT("system")));

	// Validate values
	FString ModelVal;
	Body->TryGetStringField(TEXT("model"), ModelVal);
	TestEqual(TEXT("model == config model"), ModelVal, Cfg.Model);

	bool bStream = false;
	Body->TryGetBoolField(TEXT("stream"), bStream);
	TestTrue(TEXT("stream == true"), bStream);

	double MaxOutputTokens = 0.0;
	Body->TryGetNumberField(TEXT("max_output_tokens"), MaxOutputTokens);
	TestEqual(TEXT("max_output_tokens == Config.MaxTokens"), (int32)MaxOutputTokens, Cfg.MaxTokens);

	FString Instructions;
	Body->TryGetStringField(TEXT("instructions"), Instructions);
	TestEqual(TEXT("instructions == SystemPrompt"), Instructions, TEXT("You are a helpful assistant."));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.2 — Tools transformation: input_schema -> parameters, no strict
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIProvider_BodyShape_ToolsTransform,
	"KismetAI.Provider.OpenAIBodyShape.ToolsTransform",
	OpenAIProviderTestFlags)
bool FOpenAIProvider_BodyShape_ToolsTransform::RunTest(const FString& Parameters)
{
	FOpenAIProvider Provider;
	FKismetAIAgentConfig Cfg = MakeOpenAIConfig();

	TArray<TSharedPtr<FJsonValue>> Messages;
	TArray<TSharedPtr<FJsonValue>> Tools;
	Tools.Add(MakeTool(TEXT("read_blueprint"), TEXT("Reads a blueprint")));
	Tools.Add(MakeTool(TEXT("write_blueprint"), TEXT("Writes a blueprint")));

	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("prompt"), Shape);

	const TArray<TSharedPtr<FJsonValue>>* ToolsArray;
	TestTrue(TEXT("tools is an array"), Body->TryGetArrayField(TEXT("tools"), ToolsArray));
	TestEqual(TEXT("tools has correct count"), ToolsArray->Num(), 2);

	// Each tool must be {type:function, name, description, parameters}
	// and must NOT have input_schema or strict
	const TSharedPtr<FJsonObject>* Tool0Ptr;
	TestTrue(TEXT("tools[0] is object"), (*ToolsArray)[0]->TryGetObject(Tool0Ptr));
	const TSharedPtr<FJsonObject>& Tool0 = *Tool0Ptr;

	FString ToolType;
	Tool0->TryGetStringField(TEXT("type"), ToolType);
	TestEqual(TEXT("tools[0].type == 'function'"), ToolType, FString(TEXT("function")));

	FString ToolName;
	Tool0->TryGetStringField(TEXT("name"), ToolName);
	TestEqual(TEXT("tools[0].name preserved"), ToolName, FString(TEXT("read_blueprint")));

	FString ToolDesc;
	Tool0->TryGetStringField(TEXT("description"), ToolDesc);
	TestEqual(TEXT("tools[0].description preserved"), ToolDesc, FString(TEXT("Reads a blueprint")));

	// input_schema renamed to parameters
	TestTrue(TEXT("tools[0].parameters present"), Tool0->HasField(TEXT("parameters")));
	TestFalse(TEXT("tools[0].input_schema absent"), Tool0->HasField(TEXT("input_schema")));
	TestFalse(TEXT("tools[0].strict absent"), Tool0->HasField(TEXT("strict")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.3 — input[] translation: user text message (string content)
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIProvider_Input_UserTextString,
	"KismetAI.Provider.OpenAIBodyShape.InputUserTextString",
	OpenAIProviderTestFlags)
bool FOpenAIProvider_Input_UserTextString::RunTest(const FString& Parameters)
{
	FOpenAIProvider Provider;
	FKismetAIAgentConfig Cfg = MakeOpenAIConfig();

	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(MakeUserTextMessage(TEXT("Hello world")));

	TArray<TSharedPtr<FJsonValue>> Tools;
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("prompt"), Shape);

	const TArray<TSharedPtr<FJsonValue>>* InputArray;
	TestTrue(TEXT("input is array"), Body->TryGetArrayField(TEXT("input"), InputArray));
	TestEqual(TEXT("input has 1 item"), InputArray->Num(), 1);

	const TSharedPtr<FJsonObject>* Item0Ptr;
	TestTrue(TEXT("input[0] is object"), (*InputArray)[0]->TryGetObject(Item0Ptr));
	const TSharedPtr<FJsonObject>& Item0 = *Item0Ptr;

	FString Role;
	Item0->TryGetStringField(TEXT("role"), Role);
	TestEqual(TEXT("input[0].role == 'user'"), Role, FString(TEXT("user")));

	FString Content;
	Item0->TryGetStringField(TEXT("content"), Content);
	TestEqual(TEXT("input[0].content == text"), Content, FString(TEXT("Hello world")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.4 — input[] translation: user text message (array content)
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIProvider_Input_UserTextArray,
	"KismetAI.Provider.OpenAIBodyShape.InputUserTextArray",
	OpenAIProviderTestFlags)
bool FOpenAIProvider_Input_UserTextArray::RunTest(const FString& Parameters)
{
	FOpenAIProvider Provider;
	FKismetAIAgentConfig Cfg = MakeOpenAIConfig();

	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(MakeUserTextMessageArray(TEXT("Array content")));

	TArray<TSharedPtr<FJsonValue>> Tools;
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("prompt"), Shape);

	const TArray<TSharedPtr<FJsonValue>>* InputArray;
	TestTrue(TEXT("input is array"), Body->TryGetArrayField(TEXT("input"), InputArray));
	TestEqual(TEXT("input has 1 item"), InputArray->Num(), 1);

	const TSharedPtr<FJsonObject>* Item0Ptr;
	TestTrue(TEXT("input[0] is object"), (*InputArray)[0]->TryGetObject(Item0Ptr));
	const TSharedPtr<FJsonObject>& Item0 = *Item0Ptr;

	FString Role;
	Item0->TryGetStringField(TEXT("role"), Role);
	TestEqual(TEXT("input[0].role == 'user'"), Role, FString(TEXT("user")));

	FString Content;
	Item0->TryGetStringField(TEXT("content"), Content);
	TestEqual(TEXT("input[0].content == text from array block"), Content, FString(TEXT("Array content")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.5 — input[] translation: assistant text block -> {role:assistant, content}
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIProvider_Input_AssistantText,
	"KismetAI.Provider.OpenAIBodyShape.InputAssistantText",
	OpenAIProviderTestFlags)
bool FOpenAIProvider_Input_AssistantText::RunTest(const FString& Parameters)
{
	FOpenAIProvider Provider;
	FKismetAIAgentConfig Cfg = MakeOpenAIConfig();

	TSharedPtr<FJsonObject> TextBlock = MakeShared<FJsonObject>();
	TextBlock->SetStringField(TEXT("type"), TEXT("text"));
	TextBlock->SetStringField(TEXT("text"), TEXT("I will help you"));

	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(MakeAssistantMessage({TextBlock}));

	TArray<TSharedPtr<FJsonValue>> Tools;
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("prompt"), Shape);

	const TArray<TSharedPtr<FJsonValue>>* InputArray;
	TestTrue(TEXT("input is array"), Body->TryGetArrayField(TEXT("input"), InputArray));
	TestEqual(TEXT("input has 1 item (text only)"), InputArray->Num(), 1);

	const TSharedPtr<FJsonObject>* Item0Ptr;
	TestTrue(TEXT("input[0] is object"), (*InputArray)[0]->TryGetObject(Item0Ptr));
	const TSharedPtr<FJsonObject>& Item0 = *Item0Ptr;

	FString Role;
	Item0->TryGetStringField(TEXT("role"), Role);
	TestEqual(TEXT("input[0].role == 'assistant'"), Role, FString(TEXT("assistant")));

	FString Content;
	Item0->TryGetStringField(TEXT("content"), Content);
	TestEqual(TEXT("input[0].content == text"), Content, FString(TEXT("I will help you")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.6 — input[] translation: tool_use block -> {type:function_call, ...}
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIProvider_Input_ToolUse,
	"KismetAI.Provider.OpenAIBodyShape.InputToolUse",
	OpenAIProviderTestFlags)
bool FOpenAIProvider_Input_ToolUse::RunTest(const FString& Parameters)
{
	FOpenAIProvider Provider;
	FKismetAIAgentConfig Cfg = MakeOpenAIConfig();

	TSharedPtr<FJsonObject> ToolUseBlock = MakeToolUseBlock(
		TEXT("call_abc123"),
		TEXT("read_blueprint"),
		TEXT("{\"blueprint_name\":\"BP_Test\"}"));

	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(MakeAssistantMessage({ToolUseBlock}));

	TArray<TSharedPtr<FJsonValue>> Tools;
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("prompt"), Shape);

	const TArray<TSharedPtr<FJsonValue>>* InputArray;
	TestTrue(TEXT("input is array"), Body->TryGetArrayField(TEXT("input"), InputArray));
	TestEqual(TEXT("input has 1 item (function_call)"), InputArray->Num(), 1);

	const TSharedPtr<FJsonObject>* Item0Ptr;
	TestTrue(TEXT("input[0] is object"), (*InputArray)[0]->TryGetObject(Item0Ptr));
	const TSharedPtr<FJsonObject>& Item0 = *Item0Ptr;

	FString ItemType;
	Item0->TryGetStringField(TEXT("type"), ItemType);
	TestEqual(TEXT("input[0].type == 'function_call'"), ItemType, FString(TEXT("function_call")));

	FString CallId;
	Item0->TryGetStringField(TEXT("call_id"), CallId);
	TestEqual(TEXT("input[0].call_id == tool_use.id"), CallId, FString(TEXT("call_abc123")));

	FString Name;
	Item0->TryGetStringField(TEXT("name"), Name);
	TestEqual(TEXT("input[0].name == tool name"), Name, FString(TEXT("read_blueprint")));

	// arguments must be a JSON-encoded string (not an object)
	FString ArgumentsStr;
	Item0->TryGetStringField(TEXT("arguments"), ArgumentsStr);
	TestFalse(TEXT("input[0].arguments is a non-empty string"), ArgumentsStr.IsEmpty());

	// The arguments string must be valid JSON containing blueprint_name
	TSharedPtr<FJsonObject> ParsedArgs;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgumentsStr);
	TestTrue(TEXT("arguments parses as JSON"), FJsonSerializer::Deserialize(Reader, ParsedArgs));
	FString BlueprintName;
	ParsedArgs->TryGetStringField(TEXT("blueprint_name"), BlueprintName);
	TestEqual(TEXT("arguments.blueprint_name preserved"), BlueprintName, FString(TEXT("BP_Test")));

	// Must NOT have role field (function_call items are not role-based)
	TestFalse(TEXT("function_call item has no role field"), Item0->HasField(TEXT("role")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.7 — input[] translation: tool_result -> {type:function_call_output, call_id, output}
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIProvider_Input_ToolResult,
	"KismetAI.Provider.OpenAIBodyShape.InputToolResult",
	OpenAIProviderTestFlags)
bool FOpenAIProvider_Input_ToolResult::RunTest(const FString& Parameters)
{
	FOpenAIProvider Provider;
	FKismetAIAgentConfig Cfg = MakeOpenAIConfig();

	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(MakeToolResultMessage(TEXT("call_abc123"), TEXT("Graph has 3 nodes")));

	TArray<TSharedPtr<FJsonValue>> Tools;
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("prompt"), Shape);

	const TArray<TSharedPtr<FJsonValue>>* InputArray;
	TestTrue(TEXT("input is array"), Body->TryGetArrayField(TEXT("input"), InputArray));
	TestEqual(TEXT("input has 1 item (function_call_output)"), InputArray->Num(), 1);

	const TSharedPtr<FJsonObject>* Item0Ptr;
	TestTrue(TEXT("input[0] is object"), (*InputArray)[0]->TryGetObject(Item0Ptr));
	const TSharedPtr<FJsonObject>& Item0 = *Item0Ptr;

	FString ItemType;
	Item0->TryGetStringField(TEXT("type"), ItemType);
	TestEqual(TEXT("input[0].type == 'function_call_output'"), ItemType,
		FString(TEXT("function_call_output")));

	FString CallId;
	Item0->TryGetStringField(TEXT("call_id"), CallId);
	TestEqual(TEXT("input[0].call_id == tool_use_id"), CallId, FString(TEXT("call_abc123")));

	FString Output;
	Item0->TryGetStringField(TEXT("output"), Output);
	TestEqual(TEXT("input[0].output == tool_result content"), Output,
		FString(TEXT("Graph has 3 nodes")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.8 — Round-trip: function_call followed by function_call_output (call_id matches)
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIProvider_Input_ToolRoundTrip,
	"KismetAI.Provider.OpenAIBodyShape.InputToolCallRoundTrip",
	OpenAIProviderTestFlags)
bool FOpenAIProvider_Input_ToolRoundTrip::RunTest(const FString& Parameters)
{
	FOpenAIProvider Provider;
	FKismetAIAgentConfig Cfg = MakeOpenAIConfig();

	// User message
	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(MakeUserTextMessage(TEXT("Read my blueprint")));

	// Assistant tool_use (no text block — only tool_use, must NOT emit empty text item)
	TSharedPtr<FJsonObject> ToolUseBlock = MakeToolUseBlock(
		TEXT("call_xyz789"),
		TEXT("read_blueprint"),
		TEXT("{\"blueprint_name\":\"BP_Enemy\",\"mode\":\"describe\"}"));
	Messages.Add(MakeAssistantMessage({ToolUseBlock}));

	// Tool result
	Messages.Add(MakeToolResultMessage(TEXT("call_xyz789"), TEXT("Blueprint has 5 nodes")));

	TArray<TSharedPtr<FJsonValue>> Tools;
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("prompt"), Shape);

	const TArray<TSharedPtr<FJsonValue>>* InputArray;
	TestTrue(TEXT("input is array"), Body->TryGetArrayField(TEXT("input"), InputArray));
	// Expected: user_text, function_call, function_call_output (3 items, no empty assistant text)
	TestEqual(TEXT("input has 3 items"), InputArray->Num(), 3);

	// Item 0: user
	const TSharedPtr<FJsonObject>* Item0Ptr;
	TestTrue(TEXT("input[0] is object"), (*InputArray)[0]->TryGetObject(Item0Ptr));
	FString Role0;
	(*Item0Ptr)->TryGetStringField(TEXT("role"), Role0);
	TestEqual(TEXT("input[0].role == user"), Role0, FString(TEXT("user")));

	// Item 1: function_call
	const TSharedPtr<FJsonObject>* Item1Ptr;
	TestTrue(TEXT("input[1] is object"), (*InputArray)[1]->TryGetObject(Item1Ptr));
	FString Type1;
	(*Item1Ptr)->TryGetStringField(TEXT("type"), Type1);
	TestEqual(TEXT("input[1].type == function_call"), Type1, FString(TEXT("function_call")));
	FString CallId1;
	(*Item1Ptr)->TryGetStringField(TEXT("call_id"), CallId1);
	TestEqual(TEXT("input[1].call_id"), CallId1, FString(TEXT("call_xyz789")));

	// Item 2: function_call_output
	const TSharedPtr<FJsonObject>* Item2Ptr;
	TestTrue(TEXT("input[2] is object"), (*InputArray)[2]->TryGetObject(Item2Ptr));
	FString Type2;
	(*Item2Ptr)->TryGetStringField(TEXT("type"), Type2);
	TestEqual(TEXT("input[2].type == function_call_output"), Type2,
		FString(TEXT("function_call_output")));
	FString CallId2;
	(*Item2Ptr)->TryGetStringField(TEXT("call_id"), CallId2);
	TestEqual(TEXT("call_id round-trips correctly"), CallId2, FString(TEXT("call_xyz789")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.9 — thinking blocks are dropped
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIProvider_Input_ThinkingDropped,
	"KismetAI.Provider.OpenAIBodyShape.InputThinkingBlocksDropped",
	OpenAIProviderTestFlags)
bool FOpenAIProvider_Input_ThinkingDropped::RunTest(const FString& Parameters)
{
	FOpenAIProvider Provider;
	FKismetAIAgentConfig Cfg = MakeOpenAIConfig();

	// Assistant message: thinking block + text block + tool_use block
	TSharedPtr<FJsonObject> ThinkingBlock = MakeThinkingBlock(TEXT("Let me analyze..."));
	TSharedPtr<FJsonObject> TextBlock = MakeShared<FJsonObject>();
	TextBlock->SetStringField(TEXT("type"), TEXT("text"));
	TextBlock->SetStringField(TEXT("text"), TEXT("I will read the blueprint"));
	TSharedPtr<FJsonObject> ToolUseBlock = MakeToolUseBlock(
		TEXT("call_think1"),
		TEXT("read_blueprint"),
		TEXT("{\"blueprint_name\":\"BP_Think\"}"));

	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(MakeAssistantMessage({ThinkingBlock, TextBlock, ToolUseBlock}));

	TArray<TSharedPtr<FJsonValue>> Tools;
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("prompt"), Shape);

	const TArray<TSharedPtr<FJsonValue>>* InputArray;
	TestTrue(TEXT("input is array"), Body->TryGetArrayField(TEXT("input"), InputArray));
	// thinking dropped: expect {role:assistant, content:text} + function_call = 2 items
	TestEqual(TEXT("input has 2 items (thinking dropped)"), InputArray->Num(), 2);

	// Check no item has type == "thinking"
	for (const TSharedPtr<FJsonValue>& Item : *InputArray)
	{
		const TSharedPtr<FJsonObject>* ItemObjPtr;
		if (Item->TryGetObject(ItemObjPtr))
		{
			FString ItemType;
			(*ItemObjPtr)->TryGetStringField(TEXT("type"), ItemType);
			TestFalse(TEXT("No thinking items in output"), ItemType == TEXT("thinking"));
		}
	}

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.10 — Text + tool_use in same assistant message: emit text THEN function_call
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIProvider_Input_TextThenToolUse,
	"KismetAI.Provider.OpenAIBodyShape.InputTextThenToolUse",
	OpenAIProviderTestFlags)
bool FOpenAIProvider_Input_TextThenToolUse::RunTest(const FString& Parameters)
{
	FOpenAIProvider Provider;
	FKismetAIAgentConfig Cfg = MakeOpenAIConfig();

	TSharedPtr<FJsonObject> TextBlock = MakeShared<FJsonObject>();
	TextBlock->SetStringField(TEXT("type"), TEXT("text"));
	TextBlock->SetStringField(TEXT("text"), TEXT("OK, reading now"));

	TSharedPtr<FJsonObject> ToolUseBlock = MakeToolUseBlock(
		TEXT("call_txt_tool"),
		TEXT("read_blueprint"),
		TEXT("{\"blueprint_name\":\"BP_Mix\"}"));

	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(MakeAssistantMessage({TextBlock, ToolUseBlock}));

	TArray<TSharedPtr<FJsonValue>> Tools;
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("prompt"), Shape);

	const TArray<TSharedPtr<FJsonValue>>* InputArray;
	TestTrue(TEXT("input is array"), Body->TryGetArrayField(TEXT("input"), InputArray));
	TestEqual(TEXT("input has 2 items"), InputArray->Num(), 2);

	// First: assistant text
	const TSharedPtr<FJsonObject>* Item0Ptr;
	TestTrue(TEXT("input[0] is object"), (*InputArray)[0]->TryGetObject(Item0Ptr));
	FString Role0;
	(*Item0Ptr)->TryGetStringField(TEXT("role"), Role0);
	TestEqual(TEXT("input[0].role == assistant"), Role0, FString(TEXT("assistant")));
	FString Content0;
	(*Item0Ptr)->TryGetStringField(TEXT("content"), Content0);
	TestEqual(TEXT("input[0].content == text"), Content0, FString(TEXT("OK, reading now")));

	// Second: function_call
	const TSharedPtr<FJsonObject>* Item1Ptr;
	TestTrue(TEXT("input[1] is object"), (*InputArray)[1]->TryGetObject(Item1Ptr));
	FString Type1;
	(*Item1Ptr)->TryGetStringField(TEXT("type"), Type1);
	TestEqual(TEXT("input[1].type == function_call"), Type1, FString(TEXT("function_call")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.11 — Parallel tool results: multiple tool_result blocks in one user message
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIProvider_Input_ParallelToolResults,
	"KismetAI.Provider.OpenAIBodyShape.InputParallelToolResults",
	OpenAIProviderTestFlags)
bool FOpenAIProvider_Input_ParallelToolResults::RunTest(const FString& Parameters)
{
	FOpenAIProvider Provider;
	FKismetAIAgentConfig Cfg = MakeOpenAIConfig();

	// User message with TWO tool_result blocks (parallel tool calls)
	TSharedPtr<FJsonObject> Result1 = MakeShared<FJsonObject>();
	Result1->SetStringField(TEXT("type"), TEXT("tool_result"));
	Result1->SetStringField(TEXT("tool_use_id"), TEXT("call_par_1"));
	Result1->SetStringField(TEXT("content"), TEXT("Result for tool 1"));

	TSharedPtr<FJsonObject> Result2 = MakeShared<FJsonObject>();
	Result2->SetStringField(TEXT("type"), TEXT("tool_result"));
	Result2->SetStringField(TEXT("tool_use_id"), TEXT("call_par_2"));
	Result2->SetStringField(TEXT("content"), TEXT("Result for tool 2"));

	TArray<TSharedPtr<FJsonValue>> ContentArray;
	ContentArray.Add(MakeShared<FJsonValueObject>(Result1));
	ContentArray.Add(MakeShared<FJsonValueObject>(Result2));

	TSharedPtr<FJsonObject> UserMsg = MakeShared<FJsonObject>();
	UserMsg->SetStringField(TEXT("role"), TEXT("user"));
	UserMsg->SetArrayField(TEXT("content"), ContentArray);

	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(MakeShared<FJsonValueObject>(UserMsg));

	TArray<TSharedPtr<FJsonValue>> Tools;
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("prompt"), Shape);

	const TArray<TSharedPtr<FJsonValue>>* InputArray;
	TestTrue(TEXT("input is array"), Body->TryGetArrayField(TEXT("input"), InputArray));
	// Expect 2 function_call_output items (one per tool_result block)
	TestEqual(TEXT("input has 2 items for parallel results"), InputArray->Num(), 2);

	const TSharedPtr<FJsonObject>* Item0Ptr;
	TestTrue(TEXT("input[0] is object"), (*InputArray)[0]->TryGetObject(Item0Ptr));
	FString Type0;
	(*Item0Ptr)->TryGetStringField(TEXT("type"), Type0);
	TestEqual(TEXT("input[0].type == function_call_output"), Type0,
		FString(TEXT("function_call_output")));
	FString CallId0;
	(*Item0Ptr)->TryGetStringField(TEXT("call_id"), CallId0);
	TestEqual(TEXT("input[0].call_id"), CallId0, FString(TEXT("call_par_1")));

	const TSharedPtr<FJsonObject>* Item1Ptr;
	TestTrue(TEXT("input[1] is object"), (*InputArray)[1]->TryGetObject(Item1Ptr));
	FString CallId1;
	(*Item1Ptr)->TryGetStringField(TEXT("call_id"), CallId1);
	TestEqual(TEXT("input[1].call_id"), CallId1, FString(TEXT("call_par_2")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.12 — Reasoning model: reasoning block present, temperature absent
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIProvider_ReasoningModel_ReasoningBlock,
	"KismetAI.Provider.OpenAIBodyShape.ReasoningModelHasReasoningBlock",
	OpenAIProviderTestFlags)
bool FOpenAIProvider_ReasoningModel_ReasoningBlock::RunTest(const FString& Parameters)
{
	FOpenAIProvider Provider;
	FKismetAIAgentConfig Cfg = MakeOpenAIConfig(TEXT("gpt-5-mini"));

	TArray<TSharedPtr<FJsonValue>> Messages;
	TArray<TSharedPtr<FJsonValue>> Tools;
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("prompt"), Shape);

	TestTrue(TEXT("reasoning block present for gpt-5-mini"),
		Body->HasField(TEXT("reasoning")));
	TestFalse(TEXT("temperature absent for gpt-5-mini (reasoning model)"),
		Body->HasField(TEXT("temperature")));
	TestTrue(TEXT("bEffortEnabled set for reasoning model"), Shape.bEffortEnabled);

	// Check reasoning shape
	const TSharedPtr<FJsonObject>* ReasoningPtr;
	TestTrue(TEXT("reasoning is object"), Body->TryGetObjectField(TEXT("reasoning"), ReasoningPtr));
	FString Summary;
	(*ReasoningPtr)->TryGetStringField(TEXT("summary"), Summary);
	TestEqual(TEXT("reasoning.summary == 'auto'"), Summary, FString(TEXT("auto")));
	FString Effort;
	(*ReasoningPtr)->TryGetStringField(TEXT("effort"), Effort);
	TestFalse(TEXT("reasoning.effort is non-empty"), Effort.IsEmpty());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIProvider_ReasoningModel_O1,
	"KismetAI.Provider.OpenAIBodyShape.ReasoningModelO1",
	OpenAIProviderTestFlags)
bool FOpenAIProvider_ReasoningModel_O1::RunTest(const FString& Parameters)
{
	FOpenAIProvider Provider;
	FKismetAIAgentConfig Cfg = MakeOpenAIConfig(TEXT("o1-preview"));

	TArray<TSharedPtr<FJsonValue>> Messages;
	TArray<TSharedPtr<FJsonValue>> Tools;
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("prompt"), Shape);

	TestTrue(TEXT("reasoning block for o1-preview"), Body->HasField(TEXT("reasoning")));
	TestFalse(TEXT("no temperature for o1-preview"), Body->HasField(TEXT("temperature")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIProvider_ReasoningModel_O3,
	"KismetAI.Provider.OpenAIBodyShape.ReasoningModelO3",
	OpenAIProviderTestFlags)
bool FOpenAIProvider_ReasoningModel_O3::RunTest(const FString& Parameters)
{
	FOpenAIProvider Provider;
	FKismetAIAgentConfig Cfg = MakeOpenAIConfig(TEXT("o3-mini"));

	TArray<TSharedPtr<FJsonValue>> Messages;
	TArray<TSharedPtr<FJsonValue>> Tools;
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("prompt"), Shape);

	TestTrue(TEXT("reasoning block for o3-mini"), Body->HasField(TEXT("reasoning")));
	TestFalse(TEXT("no temperature for o3-mini"), Body->HasField(TEXT("temperature")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.13 — Reasoning effort mapping: XHigh/Max -> "high"
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIProvider_ReasoningModel_EffortClamp,
	"KismetAI.Provider.OpenAIBodyShape.ReasoningEffortClamp",
	OpenAIProviderTestFlags)
bool FOpenAIProvider_ReasoningModel_EffortClamp::RunTest(const FString& Parameters)
{
	FOpenAIProvider Provider;

	// XHigh -> high
	{
		FKismetAIAgentConfig Cfg = MakeOpenAIConfig(TEXT("gpt-5-mini"));
		Cfg.EffortLevel = EEffortLevel::XHigh;
		TArray<TSharedPtr<FJsonValue>> Messages;
		TArray<TSharedPtr<FJsonValue>> Tools;
		FLLMRequestShape Shape;
		TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("p"), Shape);
		const TSharedPtr<FJsonObject>* ReasoningPtr;
		Body->TryGetObjectField(TEXT("reasoning"), ReasoningPtr);
		FString Effort;
		(*ReasoningPtr)->TryGetStringField(TEXT("effort"), Effort);
		TestEqual(TEXT("XHigh clamped to 'high'"), Effort, FString(TEXT("high")));
	}

	// Max -> high
	{
		FKismetAIAgentConfig Cfg = MakeOpenAIConfig(TEXT("o3-mini"));
		Cfg.EffortLevel = EEffortLevel::Max;
		TArray<TSharedPtr<FJsonValue>> Messages;
		TArray<TSharedPtr<FJsonValue>> Tools;
		FLLMRequestShape Shape;
		TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("p"), Shape);
		const TSharedPtr<FJsonObject>* ReasoningPtr;
		Body->TryGetObjectField(TEXT("reasoning"), ReasoningPtr);
		FString Effort;
		(*ReasoningPtr)->TryGetStringField(TEXT("effort"), Effort);
		TestEqual(TEXT("Max clamped to 'high'"), Effort, FString(TEXT("high")));
	}

	// Medium -> "medium"
	{
		FKismetAIAgentConfig Cfg = MakeOpenAIConfig(TEXT("gpt-5"));
		Cfg.EffortLevel = EEffortLevel::Medium;
		TArray<TSharedPtr<FJsonValue>> Messages;
		TArray<TSharedPtr<FJsonValue>> Tools;
		FLLMRequestShape Shape;
		TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("p"), Shape);
		const TSharedPtr<FJsonObject>* ReasoningPtr;
		Body->TryGetObjectField(TEXT("reasoning"), ReasoningPtr);
		FString Effort;
		(*ReasoningPtr)->TryGetStringField(TEXT("effort"), Effort);
		TestEqual(TEXT("Medium stays 'medium'"), Effort, FString(TEXT("medium")));
	}

	// Low -> "low"
	{
		FKismetAIAgentConfig Cfg = MakeOpenAIConfig(TEXT("o4-mini"));
		Cfg.EffortLevel = EEffortLevel::Low;
		TArray<TSharedPtr<FJsonValue>> Messages;
		TArray<TSharedPtr<FJsonValue>> Tools;
		FLLMRequestShape Shape;
		TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("p"), Shape);
		const TSharedPtr<FJsonObject>* ReasoningPtr;
		Body->TryGetObjectField(TEXT("reasoning"), ReasoningPtr);
		FString Effort;
		(*ReasoningPtr)->TryGetStringField(TEXT("effort"), Effort);
		TestEqual(TEXT("Low stays 'low'"), Effort, FString(TEXT("low")));
	}

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.14 — Uncatalogued legacy models omit unsupported optional fields
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIProvider_UnknownLegacy_NoSampling,
	"KismetAI.Provider.OpenAIBodyShape.UnknownLegacyOmitsSampling",
	OpenAIProviderTestFlags)
bool FOpenAIProvider_UnknownLegacy_NoSampling::RunTest(const FString& Parameters)
{
	FOpenAIProvider Provider;
	FKismetAIAgentConfig Cfg = MakeOpenAIConfig(TEXT("gpt-4o"), 0.7f);

	TArray<TSharedPtr<FJsonValue>> Messages;
	TArray<TSharedPtr<FJsonValue>> Tools;
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("prompt"), Shape);

	TestFalse(TEXT("no reasoning block for gpt-4o"), Body->HasField(TEXT("reasoning")));
	TestNull(TEXT("gpt-4o is not catalogued"), FKismetAIModelCatalog::Get().Find(Cfg.Model));
	TestFalse(TEXT("uncatalogued gpt-4o omits temperature"), Body->HasField(TEXT("temperature")));
	TestFalse(TEXT("bEffortEnabled not set for non-reasoning"), Shape.bEffortEnabled);

	TestFalse(TEXT("unknown legacy model does not enable thinking"), Shape.bThinkingEnabled);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIProvider_NonReasoning_Gpt4Turbo,
	"KismetAI.Provider.OpenAIBodyShape.NonReasoningGpt4Turbo",
	OpenAIProviderTestFlags)
bool FOpenAIProvider_NonReasoning_Gpt4Turbo::RunTest(const FString& Parameters)
{
	FOpenAIProvider Provider;
	FKismetAIAgentConfig Cfg = MakeOpenAIConfig(TEXT("gpt-4-turbo"), 1.0f);

	TArray<TSharedPtr<FJsonValue>> Messages;
	TArray<TSharedPtr<FJsonValue>> Tools;
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("prompt"), Shape);

	TestFalse(TEXT("no reasoning block for gpt-4-turbo"), Body->HasField(TEXT("reasoning")));
	TestNull(TEXT("gpt-4-turbo is not catalogued"), FKismetAIModelCatalog::Get().Find(Cfg.Model));
	TestFalse(TEXT("uncatalogued gpt-4-turbo omits temperature"), Body->HasField(TEXT("temperature")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.15 — GetEndpointUrl does not use Config.ApiBaseUrl (Anthropic base)
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIProvider_EndpointUrl,
	"KismetAI.Provider.OpenAIBodyShape.EndpointUrl",
	OpenAIProviderTestFlags)
bool FOpenAIProvider_EndpointUrl::RunTest(const FString& Parameters)
{
	FOpenAIProvider Provider;
	FKismetAIAgentConfig Cfg = MakeOpenAIConfig();
	// Config.ApiBaseUrl defaults to Anthropic base — must NOT be used
	Cfg.ApiBaseUrl = TEXT("https://api.anthropic.com");

	FString Url = Provider.GetEndpointUrl(Cfg);
	TestTrue(TEXT("URL ends with /v1/responses"), Url.EndsWith(TEXT("/v1/responses")));
	TestFalse(TEXT("URL does not contain anthropic.com"),
		Url.Contains(TEXT("anthropic.com")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.16 — GetExtraCurlHeaders: Authorization Bearer from OpenAIApiKey
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIProvider_Headers_Auth,
	"KismetAI.Provider.OpenAIBodyShape.ExtraCurlHeadersAuth",
	OpenAIProviderTestFlags)
bool FOpenAIProvider_Headers_Auth::RunTest(const FString& Parameters)
{
	FOpenAIProvider Provider;
	FKismetAIAgentConfig Cfg = MakeOpenAIConfig();
	Cfg.OpenAIApiKey = TEXT("sk-openai-specific-key");

	FLLMRequestShape Shape;
	TArray<FString> Headers = Provider.GetExtraCurlHeaders(Cfg, Shape);

	bool bHasAuth = false;
	bool bHasAnthropicKey = false;
	bool bHasAnthropicVersion = false;
	for (const FString& H : Headers)
	{
		if (H.Contains(TEXT("Authorization")) && H.Contains(TEXT("sk-openai-specific-key")))
		{
			bHasAuth = true;
		}
		if (H.Contains(TEXT("x-api-key")))
		{
			bHasAnthropicKey = true;
		}
		if (H.Contains(TEXT("anthropic-version")))
		{
			bHasAnthropicVersion = true;
		}
	}
	TestTrue(TEXT("Authorization Bearer header with OpenAI key"), bHasAuth);
	TestFalse(TEXT("No x-api-key (Anthropic) header"), bHasAnthropicKey);
	TestFalse(TEXT("No anthropic-version header"), bHasAnthropicVersion);

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3.17 — empty OpenAIApiKey still emits Authorization header (empty bearer)
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIProvider_Headers_EmptyKey,
	"KismetAI.Provider.OpenAIBodyShape.ExtraCurlHeadersEmptyKey",
	OpenAIProviderTestFlags)
bool FOpenAIProvider_Headers_EmptyKey::RunTest(const FString& Parameters)
{
	FOpenAIProvider Provider;
	FKismetAIAgentConfig Cfg = MakeOpenAIConfig();
	Cfg.OpenAIApiKey = TEXT("");  // empty — must still emit header, not substitute Anthropic key

	FLLMRequestShape Shape;
	TArray<FString> Headers = Provider.GetExtraCurlHeaders(Cfg, Shape);

	bool bHasAuthHeader = false;
	for (const FString& H : Headers)
	{
		if (H.Contains(TEXT("Authorization: Bearer")))
		{
			bHasAuthHeader = true;
			// Must NOT contain the Anthropic key
			TestFalse(TEXT("Anthropic key not used as fallback"),
				H.Contains(TEXT("sk-anthropic")));
		}
	}
	TestTrue(TEXT("Authorization header present even with empty key"), bHasAuthHeader);

	return true;
}
