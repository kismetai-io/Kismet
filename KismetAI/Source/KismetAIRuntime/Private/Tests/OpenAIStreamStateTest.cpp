// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

// PH4 tests: FOpenAIStreamState SSE parser + canonical response JSON.
// Tests under "KismetAI.Provider.OpenAIStream.*"

#include "KismetAIAgent.h"
#include "KismetAILLMProvider.h"
#include "OpenAIResponsesSSEParser.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

constexpr auto OpenAIStreamTestFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

// Parse a compact JSON literal into a FJsonObject. Crashes the test on failure.
static TSharedPtr<FJsonObject> ParseJson(const FString& JsonStr)
{
	TSharedPtr<FJsonObject> Obj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
	FJsonSerializer::Deserialize(Reader, Obj);
	return Obj;
}

// Feed a sequence of (EventType, DataJson) pairs into a stream state.
// Returns the accumulated delta text produced.
static FString FeedEvents(FOpenAIStreamState& State,
	const TArray<TPair<FString, FString>>& Events)
{
	FString AllDeltas;
	for (const auto& Pair : Events)
	{
		TSharedPtr<FJsonObject> Data = ParseJson(Pair.Value);
		FString Delta;
		if (State.ProcessEvent(Pair.Key, Data, Delta))
		{
			AllDeltas += Delta;
		}
	}
	return AllDeltas;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test (a): text-only stream -> canonical text block + stop_reason=end_turn
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIStream_TextOnly,
	"KismetAI.Provider.OpenAIStream.TextOnly",
	OpenAIStreamTestFlags)
bool FOpenAIStream_TextOnly::RunTest(const FString& Parameters)
{
	FOpenAIStreamState State;

	// output_text.delta events (streaming text)
	TArray<TPair<FString, FString>> Events;
	Events.Add(MakeTuple(
		TEXT("response.output_text.delta"),
		TEXT("{\"type\":\"response.output_text.delta\",\"item_id\":\"msg_x\",\"delta\":\"The weather\"}")));
	Events.Add(MakeTuple(
		TEXT("response.output_text.delta"),
		TEXT("{\"type\":\"response.output_text.delta\",\"item_id\":\"msg_x\",\"delta\":\" is fine\"}")));
	// output_item.done finalizes the text block (item.type == "message")
	Events.Add(MakeTuple(
		TEXT("response.output_item.done"),
		TEXT("{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"msg_x\",\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"The weather is fine\"}]}}")));
	// response.completed with usage
	Events.Add(MakeTuple(
		TEXT("response.completed"),
		TEXT("{\"type\":\"response.completed\",\"response\":{\"id\":\"resp_x\",\"status\":\"completed\",\"usage\":{\"input_tokens\":85,\"output_tokens\":23,\"input_tokens_details\":{\"cached_tokens\":0}}}}")));

	FString AllDeltas = FeedEvents(State, Events);

	// Deltas must have streamed
	TestEqual(TEXT("Text deltas accumulated"), AllDeltas, FString(TEXT("The weather is fine")));

	// Build and check canonical response
	TSharedPtr<FJsonObject> Response = State.BuildResponseJson();
	TestTrue(TEXT("Response is valid"), Response.IsValid());

	// stop_reason == end_turn (no function calls)
	FString StopReason;
	Response->TryGetStringField(TEXT("stop_reason"), StopReason);
	TestEqual(TEXT("stop_reason == end_turn"), StopReason, FString(TEXT("end_turn")));

	// content[] has exactly one text block
	const TArray<TSharedPtr<FJsonValue>>* ContentArr;
	TestTrue(TEXT("content array present"), Response->TryGetArrayField(TEXT("content"), ContentArr));
	TestEqual(TEXT("content has 1 block"), ContentArr->Num(), 1);

	const TSharedPtr<FJsonObject>* Block0Ptr;
	TestTrue(TEXT("content[0] is object"), (*ContentArr)[0]->TryGetObject(Block0Ptr));
	FString BlockType;
	(*Block0Ptr)->TryGetStringField(TEXT("type"), BlockType);
	TestEqual(TEXT("content[0].type == text"), BlockType, FString(TEXT("text")));
	FString BlockText;
	(*Block0Ptr)->TryGetStringField(TEXT("text"), BlockText);
	TestFalse(TEXT("content[0].text is non-empty"), BlockText.IsEmpty());

	// usage fields
	const TSharedPtr<FJsonObject>* UsagePtr;
	TestTrue(TEXT("usage present"), Response->TryGetObjectField(TEXT("usage"), UsagePtr));
	double InputTokens = 0.0;
	(*UsagePtr)->TryGetNumberField(TEXT("input_tokens"), InputTokens);
	TestEqual(TEXT("usage.input_tokens == 85"), (int32)InputTokens, 85);
	double OutputTokens = 0.0;
	(*UsagePtr)->TryGetNumberField(TEXT("output_tokens"), OutputTokens);
	TestEqual(TEXT("usage.output_tokens == 23"), (int32)OutputTokens, 23);

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test (b): function-call stream -> tool_use{id,name,input} + stop_reason=tool_use + usage
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIStream_FunctionCall,
	"KismetAI.Provider.OpenAIStream.FunctionCall",
	OpenAIStreamTestFlags)
bool FOpenAIStream_FunctionCall::RunTest(const FString& Parameters)
{
	FOpenAIStreamState State;

	TArray<TPair<FString, FString>> Events;

	// Function call begins
	Events.Add(MakeTuple(
		TEXT("response.output_item.added"),
		TEXT("{\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"fc_x\",\"type\":\"function_call\",\"call_id\":\"call_123\",\"name\":\"get_weather\",\"arguments\":\"\"}}")));
	// Argument deltas streaming in
	Events.Add(MakeTuple(
		TEXT("response.function_call_arguments.delta"),
		TEXT("{\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"fc_x\",\"delta\":\"{\\\"loc\"}")));
	Events.Add(MakeTuple(
		TEXT("response.function_call_arguments.delta"),
		TEXT("{\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"fc_x\",\"delta\":\"ation\\\":\\\"Paris\\\"}\"}")));
	// Arguments done
	Events.Add(MakeTuple(
		TEXT("response.function_call_arguments.done"),
		TEXT("{\"type\":\"response.function_call_arguments.done\",\"item_id\":\"fc_x\",\"arguments\":\"{\\\"location\\\":\\\"Paris\\\"}\"}")));
	// output_item.done with full item (authoritative)
	Events.Add(MakeTuple(
		TEXT("response.output_item.done"),
		TEXT("{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"fc_x\",\"type\":\"function_call\",\"call_id\":\"call_123\",\"name\":\"get_weather\",\"arguments\":\"{\\\"location\\\":\\\"Paris\\\"}\"}}")));
	// response.completed with usage
	Events.Add(MakeTuple(
		TEXT("response.completed"),
		TEXT("{\"type\":\"response.completed\",\"response\":{\"id\":\"resp_y\",\"status\":\"completed\",\"usage\":{\"input_tokens\":100,\"output_tokens\":50,\"input_tokens_details\":{\"cached_tokens\":20}}}}")));

	FeedEvents(State, Events);

	TSharedPtr<FJsonObject> Response = State.BuildResponseJson();
	TestTrue(TEXT("Response is valid"), Response.IsValid());

	// stop_reason == tool_use
	FString StopReason;
	Response->TryGetStringField(TEXT("stop_reason"), StopReason);
	TestEqual(TEXT("stop_reason == tool_use"), StopReason, FString(TEXT("tool_use")));

	// content[] has exactly one tool_use block
	const TArray<TSharedPtr<FJsonValue>>* ContentArr;
	TestTrue(TEXT("content array present"), Response->TryGetArrayField(TEXT("content"), ContentArr));
	TestEqual(TEXT("content has 1 block"), ContentArr->Num(), 1);

	const TSharedPtr<FJsonObject>* Block0Ptr;
	TestTrue(TEXT("content[0] is object"), (*ContentArr)[0]->TryGetObject(Block0Ptr));
	FString BlockType;
	(*Block0Ptr)->TryGetStringField(TEXT("type"), BlockType);
	TestEqual(TEXT("content[0].type == tool_use"), BlockType, FString(TEXT("tool_use")));

	// RK2: call_id must round-trip as the tool_use id
	FString ToolUseId;
	(*Block0Ptr)->TryGetStringField(TEXT("id"), ToolUseId);
	TestEqual(TEXT("tool_use.id == call_123 (RK2)"), ToolUseId, FString(TEXT("call_123")));

	FString ToolName;
	(*Block0Ptr)->TryGetStringField(TEXT("name"), ToolName);
	TestEqual(TEXT("tool_use.name == get_weather"), ToolName, FString(TEXT("get_weather")));

	// input is parsed FJsonObject, not a string
	const TSharedPtr<FJsonObject>* InputPtr;
	TestTrue(TEXT("tool_use.input is object"), (*Block0Ptr)->TryGetObjectField(TEXT("input"), InputPtr));
	FString Location;
	(*InputPtr)->TryGetStringField(TEXT("location"), Location);
	TestEqual(TEXT("tool_use.input.location == Paris"), Location, FString(TEXT("Paris")));

	// usage
	const TSharedPtr<FJsonObject>* UsagePtr;
	TestTrue(TEXT("usage present"), Response->TryGetObjectField(TEXT("usage"), UsagePtr));
	double InputTokens = 0.0;
	(*UsagePtr)->TryGetNumberField(TEXT("input_tokens"), InputTokens);
	TestEqual(TEXT("usage.input_tokens == 100"), (int32)InputTokens, 100);
	double OutputTokens = 0.0;
	(*UsagePtr)->TryGetNumberField(TEXT("output_tokens"), OutputTokens);
	TestEqual(TEXT("usage.output_tokens == 50"), (int32)OutputTokens, 50);
	double CacheRead = 0.0;
	(*UsagePtr)->TryGetNumberField(TEXT("cache_read_input_tokens"), CacheRead);
	TestEqual(TEXT("usage.cache_read_input_tokens == 20"), (int32)CacheRead, 20);
	double CacheWrite = 0.0;
	(*UsagePtr)->TryGetNumberField(TEXT("cache_creation_input_tokens"), CacheWrite);
	TestEqual(TEXT("usage.cache_creation_input_tokens == 0"), (int32)CacheWrite, 0);

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test (c): response.incomplete -> stop_reason=max_tokens
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIStream_Incomplete,
	"KismetAI.Provider.OpenAIStream.Incomplete",
	OpenAIStreamTestFlags)
bool FOpenAIStream_Incomplete::RunTest(const FString& Parameters)
{
	FOpenAIStreamState State;

	TArray<TPair<FString, FString>> Events;
	Events.Add(MakeTuple(
		TEXT("response.output_text.delta"),
		TEXT("{\"type\":\"response.output_text.delta\",\"item_id\":\"msg_z\",\"delta\":\"Partial\"}")));
	Events.Add(MakeTuple(
		TEXT("response.incomplete"),
		TEXT("{\"type\":\"response.incomplete\",\"response\":{\"id\":\"resp_z\",\"status\":\"incomplete\",\"incomplete_details\":{\"reason\":\"max_output_tokens\"},\"usage\":{\"input_tokens\":200,\"output_tokens\":4096,\"input_tokens_details\":{\"cached_tokens\":50}}}}")));

	FeedEvents(State, Events);

	TSharedPtr<FJsonObject> Response = State.BuildResponseJson();
	TestTrue(TEXT("Response is valid"), Response.IsValid());

	FString StopReason;
	Response->TryGetStringField(TEXT("stop_reason"), StopReason);
	TestEqual(TEXT("stop_reason == max_tokens"), StopReason, FString(TEXT("max_tokens")));

	// usage from incomplete event
	const TSharedPtr<FJsonObject>* UsagePtr;
	TestTrue(TEXT("usage present"), Response->TryGetObjectField(TEXT("usage"), UsagePtr));
	double InputTokens = 0.0;
	(*UsagePtr)->TryGetNumberField(TEXT("input_tokens"), InputTokens);
	TestEqual(TEXT("usage.input_tokens == 200"), (int32)InputTokens, 200);

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test (c2): response.failed on a 200 stream -> HasStreamError surfaces the
// upstream message/code (must NOT be silently treated as end_turn).
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIStream_ResponseFailed,
	"KismetAI.Provider.OpenAIStream.ResponseFailed",
	OpenAIStreamTestFlags)
bool FOpenAIStream_ResponseFailed::RunTest(const FString& Parameters)
{
	FOpenAIStreamState State;

	TArray<TPair<FString, FString>> Events;
	Events.Add(MakeTuple(
		TEXT("response.created"),
		TEXT("{\"type\":\"response.created\",\"response\":{\"id\":\"resp_f\",\"status\":\"in_progress\"}}")));
	Events.Add(MakeTuple(
		TEXT("response.failed"),
		TEXT("{\"type\":\"response.failed\",\"response\":{\"id\":\"resp_f\",\"status\":\"failed\",\"error\":{\"code\":\"server_error\",\"message\":\"The model produced an internal error\"}}}")));

	FeedEvents(State, Events);

	FString Msg;
	FString Code;
	TestTrue(TEXT("HasStreamError reports the in-stream failure"), State.HasStreamError(Msg, Code));
	TestEqual(TEXT("error code extracted"), Code, FString(TEXT("server_error")));
	TestEqual(TEXT("error message extracted"), Msg, FString(TEXT("The model produced an internal error")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test (c3): bare `error` event (root-level message/code) -> HasStreamError.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIStream_ErrorEvent,
	"KismetAI.Provider.OpenAIStream.ErrorEvent",
	OpenAIStreamTestFlags)
bool FOpenAIStream_ErrorEvent::RunTest(const FString& Parameters)
{
	FOpenAIStreamState State;

	TArray<TPair<FString, FString>> Events;
	Events.Add(MakeTuple(
		TEXT("error"),
		TEXT("{\"type\":\"error\",\"code\":\"rate_limit_exceeded\",\"message\":\"Too many requests\"}")));

	FeedEvents(State, Events);

	FString Msg;
	FString Code;
	TestTrue(TEXT("HasStreamError reports the bare error event"), State.HasStreamError(Msg, Code));
	TestEqual(TEXT("error code extracted from root"), Code, FString(TEXT("rate_limit_exceeded")));
	TestEqual(TEXT("error message extracted from root"), Msg, FString(TEXT("Too many requests")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test (c4): a clean completed stream reports NO stream error.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIStream_NoErrorOnSuccess,
	"KismetAI.Provider.OpenAIStream.NoErrorOnSuccess",
	OpenAIStreamTestFlags)
bool FOpenAIStream_NoErrorOnSuccess::RunTest(const FString& Parameters)
{
	FOpenAIStreamState State;

	TArray<TPair<FString, FString>> Events;
	Events.Add(MakeTuple(
		TEXT("response.output_text.delta"),
		TEXT("{\"type\":\"response.output_text.delta\",\"item_id\":\"msg_ok\",\"delta\":\"hi\"}")));
	Events.Add(MakeTuple(
		TEXT("response.completed"),
		TEXT("{\"type\":\"response.completed\",\"response\":{\"id\":\"resp_ok\",\"status\":\"completed\",\"usage\":{\"input_tokens\":1,\"output_tokens\":1,\"input_tokens_details\":{\"cached_tokens\":0}}}}")));

	FeedEvents(State, Events);

	FString Msg;
	FString Code;
	TestFalse(TEXT("clean stream has no error"), State.HasStreamError(Msg, Code));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test (d): call_id round-trips byte-identical (RK2)
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIStream_CallIdRoundTrip,
	"KismetAI.Provider.OpenAIStream.CallIdRoundTrip",
	OpenAIStreamTestFlags)
bool FOpenAIStream_CallIdRoundTrip::RunTest(const FString& Parameters)
{
	// Unusual call_id with special chars to test byte-identical round-trip
	const FString ExpectedCallId = TEXT("call_AbCdEf_123-456");

	FOpenAIStreamState State;

	TArray<TPair<FString, FString>> Events;
	// output_item.done carries the authoritative call_id
	Events.Add(MakeTuple(
		TEXT("response.output_item.done"),
		FString::Printf(
			TEXT("{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"fc_abc\",\"type\":\"function_call\",\"call_id\":\"%s\",\"name\":\"read_blueprint\",\"arguments\":\"{\\\"blueprint_name\\\":\\\"BP_Test\\\"}\"}}"),
			*ExpectedCallId)));
	Events.Add(MakeTuple(
		TEXT("response.completed"),
		TEXT("{\"type\":\"response.completed\",\"response\":{\"id\":\"resp_rt\",\"status\":\"completed\",\"usage\":{\"input_tokens\":10,\"output_tokens\":5,\"input_tokens_details\":{\"cached_tokens\":0}}}}")));

	FeedEvents(State, Events);

	TSharedPtr<FJsonObject> Response = State.BuildResponseJson();
	TestTrue(TEXT("Response is valid"), Response.IsValid());

	const TArray<TSharedPtr<FJsonValue>>* ContentArr;
	TestTrue(TEXT("content array present"), Response->TryGetArrayField(TEXT("content"), ContentArr));
	TestEqual(TEXT("content has 1 block"), ContentArr->Num(), 1);

	const TSharedPtr<FJsonObject>* Block0Ptr;
	(*ContentArr)[0]->TryGetObject(Block0Ptr);
	FString ToolUseId;
	(*Block0Ptr)->TryGetStringField(TEXT("id"), ToolUseId);
	TestEqual(TEXT("call_id round-trips byte-identical (RK2)"), ToolUseId, ExpectedCallId);

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test (e): malformed arguments reject the turn; never substitute executable {}.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIStream_MalformedArgs,
	"KismetAI.Provider.OpenAIStream.MalformedArguments",
	OpenAIStreamTestFlags)
bool FOpenAIStream_MalformedArgs::RunTest(const FString& Parameters)
{
	FOpenAIStreamState State;

	TArray<TPair<FString, FString>> Events;
	// output_item.done with invalid JSON in arguments
	Events.Add(MakeTuple(
		TEXT("response.output_item.done"),
		TEXT("{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"fc_bad\",\"type\":\"function_call\",\"call_id\":\"call_bad\",\"name\":\"read_blueprint\",\"arguments\":\"NOT VALID JSON\"}}")));
	Events.Add(MakeTuple(
		TEXT("response.completed"),
		TEXT("{\"type\":\"response.completed\",\"response\":{\"id\":\"resp_bad\",\"status\":\"completed\",\"usage\":{\"input_tokens\":5,\"output_tokens\":5,\"input_tokens_details\":{\"cached_tokens\":0}}}}")));

	FeedEvents(State, Events);

	TSharedPtr<FJsonObject> Response = State.BuildResponseJson();
	TestTrue(TEXT("usage remains readable after malformed args"), Response.IsValid());

	const TArray<TSharedPtr<FJsonValue>>* ContentArr;
	TestTrue(TEXT("content array present"), Response->TryGetArrayField(TEXT("content"), ContentArr));
	TestEqual(TEXT("malformed tool is not exposed for execution"), ContentArr->Num(), 0);
	FString Message, Code;
	TestTrue(TEXT("transport can reject the whole response"), State.HasStreamError(Message, Code));
	TestEqual(TEXT("explicit malformed-arguments error"), Code, FString(TEXT("malformed_tool_arguments")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test (f): ParseErrorBody — context_length_exceeded + regex for N/M tokens
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIStream_ParseErrorBody_ContextLength,
	"KismetAI.Provider.OpenAIStream.ParseErrorBodyContextLength",
	OpenAIStreamTestFlags)
bool FOpenAIStream_ParseErrorBody_ContextLength::RunTest(const FString& Parameters)
{
	// This tests ParseErrorBody via a round-trip through FModelCallOutcome.
	// We import FOpenAIProvider here to test ParseErrorBody.
	// Include path: KismetAIOpenAIProvider.h is in the Private directory but
	// reachable as a module-private include.
	//
	// We hand-build the error JSON in the OpenAI error shape:
	// {"error":{"message":"...","type":"context_length_exceeded","code":"context_length_exceeded"}}

	// Cannot include FOpenAIProvider directly in test without exposing it.
	// The plan drives this via ClassifyCallOutcome + ParseErrorBody.
	// We only test ClassifyCallOutcome here for the OpenAI error types.

	// Test ClassifyCallOutcome recognizes context_length_exceeded -> MaxTokensOverflow
	{
		EModelCallResult Result = FKismetAIAgent::ClassifyCallOutcome(
			0, 400,
			TEXT("context_length_exceeded"),
			TEXT("This model's maximum context length is 128000 tokens. However, your messages resulted in 130000 tokens."),
			false);
		TestTrue(TEXT("context_length_exceeded -> MaxTokensOverflow"),
			Result == EModelCallResult::MaxTokensOverflow);
	}

	// "maximum context length" in message also triggers MaxTokensOverflow
	{
		EModelCallResult Result = FKismetAIAgent::ClassifyCallOutcome(
			0, 400,
			TEXT("invalid_request_error"),
			TEXT("This model's maximum context length is 128000 tokens."),
			false);
		TestTrue(TEXT("'maximum context length' in message -> MaxTokensOverflow"),
			Result == EModelCallResult::MaxTokensOverflow);
	}

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test (g): ClassifyCallOutcome — OpenAI error types
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIStream_ClassifyOpenAIErrors,
	"KismetAI.Provider.OpenAIStream.ClassifyOpenAIErrorTypes",
	OpenAIStreamTestFlags)
bool FOpenAIStream_ClassifyOpenAIErrors::RunTest(const FString& Parameters)
{
	// rate_limit_exceeded -> RetryableError
	{
		EModelCallResult Result = FKismetAIAgent::ClassifyCallOutcome(
			0, 429,
			TEXT("rate_limit_exceeded"),
			TEXT("You exceeded your current quota, please check your plan and billing details."),
			false);
		TestTrue(TEXT("rate_limit_exceeded -> RetryableError"),
			Result == EModelCallResult::RetryableError);
	}

	// insufficient_quota -> FatalError
	{
		EModelCallResult Result = FKismetAIAgent::ClassifyCallOutcome(
			0, 429,
			TEXT("insufficient_quota"),
			TEXT("You have exceeded your quota for this organization."),
			false);
		TestTrue(TEXT("insufficient_quota -> FatalError"),
			Result == EModelCallResult::FatalError);
	}

	return true;
}
