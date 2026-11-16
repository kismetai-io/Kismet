// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAILLMProvider.h"
#include "KismetAIAgentTypes.h"
#include "KismetAIAnthropicProvider.h"
#include "KismetAIMinimaxProvider.h"
#include "KismetAIOpenAIProvider.h"
#include "KismetAIOpenAIChatProvider.h"
#include "Fakes/FixtureTransport.h"
#include "Fakes/ScriptedAgentSeams.h"
#include "KismetAISessionStorage.h"
#include "KismetAIJournalReducer.h"
#include "Misc/AutomationTest.h"

constexpr auto TypedProviderFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTypedProvider_CanonicalRequest, "KismetAI.Provider.TypedCanonicalRequest", TypedProviderFlags)
bool FTypedProvider_CanonicalRequest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Thinking = MakeShared<FJsonObject>();
	Thinking->SetStringField(TEXT("type"), TEXT("thinking"));
	Thinking->SetStringField(TEXT("thinking"), TEXT("private-reasoning"));
	Thinking->SetStringField(TEXT("signature"), TEXT("opaque-signature"));
	Thinking->SetStringField(TEXT("provider_extension"), TEXT("opaque-extension"));
	FAgentMessage Assistant;
	Assistant.Role = EAgentRole::Assistant;
	Assistant.Parts = KismetAIAgentCodec::FromParts({ MakeShared<FJsonValueObject>(Thinking) });
	Assistant.Parts.Add(FAgentContentPart::MakeText(TEXT("Running.")));
	Assistant.Parts.Add(FAgentContentPart::MakeToolUse(TEXT("first-call"), TEXT("read_blueprint"), MakeShared<FJsonObject>()));
	Assistant.Parts.Add(FAgentContentPart::MakeToolUse(TEXT("second-call"), TEXT("compile_blueprint"), MakeShared<FJsonObject>()));
	FAgentMessage Results;
	Results.Parts.Add(FAgentContentPart::MakeToolResult(TEXT("first-call"), TEXT("read-result"), false));
	Results.Parts.Add(FAgentContentPart::MakeToolResult(TEXT("second-call"), TEXT("compile-result"), true));
	const TArray<FAgentMessage> Messages{ Assistant, Results };
	const FString Before = KismetAIFixtureTransport::Condensed(KismetAIAgentCodec::ToAnthropicJson(Messages[0]));
	AddInfo(TEXT("Canonical source: ") + Before);

	for (const TCHAR* Id : { TEXT("anthropic"), TEXT("minimax"), TEXT("openai"), TEXT("openrouter") })
	{
		FKismetAIAgentConfig Config;
		Config.ProviderId = Id;
		Config.Model = TEXT("fixture-model");
		TUniquePtr<ILLMProvider> Provider = MakeProviderForConfig(Config);
		if (!TestTrue(TEXT("fixture provider exists"), Provider.IsValid())) return false;
		FLLMRequestShape Shape;
		const TSharedPtr<FJsonObject> Body = Provider->BuildRequestBody(Config, Messages, {}, TEXT("system"), Shape);
		const FString Wire = KismetAIFixtureTransport::Condensed(Body);
		AddInfo(FString::Printf(TEXT("Provider %s request: %s"), Id, *Wire));
		TestTrue(TEXT("first identity encoded"), Wire.Contains(TEXT("first-call")));
		TestTrue(TEXT("second identity encoded"), Wire.Contains(TEXT("second-call")));
		TestTrue(TEXT("first result encoded"), Wire.Contains(TEXT("read-result")));
		TestTrue(TEXT("second result encoded"), Wire.Contains(TEXT("compile-result")));
		const bool bAnthropicWire = FString(Id) == TEXT("anthropic") || FString(Id) == TEXT("minimax");
		TestEqual(TEXT("thinking wire policy preserved"), Wire.Contains(TEXT("private-reasoning")), bAnthropicWire);
		TestEqual(TEXT("opaque compatible field policy preserved"), Wire.Contains(TEXT("opaque-extension")), bAnthropicWire);
		TestEqual(TEXT("adapter did not mutate canonical history"),
			KismetAIFixtureTransport::Condensed(KismetAIAgentCodec::ToAnthropicJson(Messages[0])), Before);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTypedProvider_CompleteStream, "KismetAI.Provider.TypedCompleteStream", TypedProviderFlags)
bool FTypedProvider_CompleteStream::RunTest(const FString& Parameters)
{
	using namespace KismetAIFixtureTransport;
	FAnthropicStreamBuilder Stream;
	Stream.MessageStart().BlockStart(TEXT("thinking")).Thinking(TEXT("reasoning")).Signature(TEXT("signature-value")).BlockStop()
		.BlockStart(TEXT("text")).Text(TEXT("Running.")).BlockStop()
		.BlockStart(TEXT("tool_use"), TEXT("a"), TEXT("read_blueprint")).ToolJson(TEXT("{}" )).BlockStop()
		.BlockStart(TEXT("tool_use"), TEXT("b"), TEXT("compile_blueprint")).ToolJson(TEXT("{}" )).BlockStop()
		.MessageEnd(TEXT("tool_use"));
	for (const TCHAR* Model : { TEXT("claude-sonnet-4-6"), TEXT("MiniMax-M3") })
	{
		FScriptedAttempt Attempt;
		Attempt.Bytes = Stream.Bytes();
		const FModelCallOutcome Outcome = RunAttempt(Attempt, {}, []() { return false; }, Model);
		TestTrue(TEXT("complete stream succeeds"), Outcome.Result == EModelCallResult::Success);
		TestEqual(TEXT("typed response has all parts"), Outcome.AssistantMessage.Parts.Num(), 4);
		TestEqual(TEXT("thinking is not visible text"), Outcome.AssistantMessage.GetText(), FString(TEXT("Running.")));
		const FString Json = Condensed(KismetAIAgentCodec::ToAnthropicJson(Outcome.AssistantMessage));
		TestTrue(TEXT("thinking signature survives production decoder"), Json.Contains(TEXT("signature-value")));
		TestTrue(TEXT("first call identity survives"), Json.Contains(TEXT("\"id\":\"a\"")));
		TestTrue(TEXT("second call identity survives"), Json.Contains(TEXT("\"id\":\"b\"")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTypedProvider_OpaqueStream, "KismetAI.Provider.TypedOpaqueStream", TypedProviderFlags)
bool FTypedProvider_OpaqueStream::RunTest(const FString& Parameters)
{
	using namespace KismetAIFixtureTransport;
	FAnthropicStreamBuilder Stream;
	Stream.MessageStart().Raw(TEXT("event: content_block_start\ndata: {\"content_block\":{\"type\":\"provider_extension\",\"payload\":{\"opaque\":17}},\"index\":0}\n\n"))
		.BlockStop().MessageEnd(TEXT("end_turn"));
	FScriptedAttempt Attempt;
	Attempt.Bytes = Stream.Bytes();
	const FModelCallOutcome Outcome = RunAttempt(Attempt, {}, []() { return false; });
	TestTrue(TEXT("opaque complete turn succeeds"), Outcome.Result == EModelCallResult::Success);
	TestTrue(TEXT("opaque block survives the production SSE adapter"),
		Condensed(KismetAIAgentCodec::ToAnthropicJson(Outcome.AssistantMessage)).Contains(TEXT("\"opaque\":17")));
	TestTrue(TEXT("opaque block does not become user-visible text"), Outcome.AssistantMessage.GetText().IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTypedProvider_InvalidInitialArguments, "KismetAI.Provider.TypedInvalidInitialArguments", TypedProviderFlags)
bool FTypedProvider_InvalidInitialArguments::RunTest(const FString& Parameters)
{
	using namespace KismetAIFixtureTransport;
	FAnthropicStreamBuilder Stream;
	Stream.MessageStart().Raw(TEXT("event: content_block_start\ndata: {\"content_block\":{\"type\":\"tool_use\",\"id\":\"bad\",\"name\":\"enter_plan_mode\",\"input\":\"invalid\"},\"index\":0}\n\n"))
		.BlockStop().MessageEnd(TEXT("tool_use"));
	FScriptedAttempt Attempt;
	Attempt.Bytes = Stream.Bytes();
	const FModelCallOutcome Outcome = RunAttempt(Attempt, {}, []() { return false; });
	TestTrue(TEXT("invalid initial argument shape is refused"), Outcome.Result == EModelCallResult::FatalError);
	TestEqual(TEXT("explicit malformed arguments error"), Outcome.ErrorType, FString(TEXT("malformed_tool_arguments")));
	TestTrue(TEXT("invalid attempt has no committed assistant tools"), !Outcome.AssistantMessage.HasToolUse());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTypedProvider_OpenAIStreams, "KismetAI.Provider.TypedOpenAIStreams", TypedProviderFlags)
bool FTypedProvider_OpenAIStreams::RunTest(const FString& Parameters)
{
	for (const bool bChat : { false, true })
	{
		FKismetAIAgentConfig Config;
		Config.ProviderId = bChat ? TEXT("openrouter") : TEXT("openai");
		Config.Model = TEXT("fixture-model");
		TUniquePtr<ILLMProvider> Provider = MakeProviderForConfig(Config);
		if (!TestTrue(TEXT("provider exists"), Provider.IsValid())) return false;
		const FString Wire = bChat
			? FString(TEXT("data: {\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"reasoning\"},\"finish_reason\":null}]}\n\n")
				TEXT("data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Running.\",\"tool_calls\":[{\"index\":0,\"id\":\"a\",\"function\":{\"name\":\"read_blueprint\",\"arguments\":\"{}\"}},{\"index\":1,\"id\":\"b\",\"function\":{\"name\":\"compile_blueprint\",\"arguments\":\"{}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n"))
			: FString(TEXT("event: response.reasoning_summary_text.delta\ndata: {\"delta\":\"reasoning\"}\n\n")
				TEXT("event: response.reasoning_summary_text.done\ndata: {}\n\n")
				TEXT("event: response.output_text.delta\ndata: {\"delta\":\"Running.\"}\n\n")
				TEXT("event: response.output_text.done\ndata: {}\n\n")
				TEXT("event: response.output_item.done\ndata: {\"item\":{\"type\":\"function_call\",\"call_id\":\"a\",\"name\":\"read_blueprint\",\"arguments\":\"{}\"}}\n\n")
				TEXT("event: response.output_item.done\ndata: {\"item\":{\"type\":\"function_call\",\"call_id\":\"b\",\"name\":\"compile_blueprint\",\"arguments\":\"{}\"}}\n\n")
				TEXT("event: response.completed\ndata: {\"response\":{}}\n\n"));
		TSharedPtr<ILLMStreamState> State = Provider->CreateStreamState();
		const FModelCallbacks Callbacks;
		FKismetAIStreamDecoder Decoder(*State, Callbacks);
		FTCHARToUTF8 Bytes(*Wire);
		Decoder.Feed(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
		Decoder.Finish();
		const FModelCallOutcome Outcome = FKismetAICurlModelClient::ClassifyAttempt(*Provider, *State, Wire, 0, 200, true, false);
		TestTrue(TEXT("complete provider stream succeeds"), Outcome.Result == EModelCallResult::Success);
		TestEqual(TEXT("visible text excludes thinking"), Outcome.AssistantMessage.GetText(), FString(TEXT("Running.")));
		TestEqual(TEXT("thinking/text/two calls all committed as typed parts"), Outcome.AssistantMessage.Parts.Num(), 4);
		const FString Json = KismetAIFixtureTransport::Condensed(KismetAIAgentCodec::ToAnthropicJson(Outcome.AssistantMessage));
		TestTrue(TEXT("thinking retained canonically"), Json.Contains(TEXT("reasoning")));
		TestTrue(TEXT("first call retained"), Json.Contains(TEXT("\"id\":\"a\"")));
		TestTrue(TEXT("second call retained"), Json.Contains(TEXT("\"id\":\"b\"")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTypedProvider_NativeMetadata, "KismetAI.Provider.TypedNativeMetadata", TypedProviderFlags)
bool FTypedProvider_NativeMetadata::RunTest(const FString& Parameters)
{
	for (const bool bChat : { false, true })
	{
		FKismetAIAgentConfig Config;
		Config.ProviderId = bChat ? TEXT("openrouter") : TEXT("openai");
		Config.Model = TEXT("fixture-model");
		TUniquePtr<ILLMProvider> Provider = MakeProviderForConfig(Config);
		if (!TestTrue(TEXT("provider exists"), Provider.IsValid())) return false;
		const FString Wire = bChat
			? FString(TEXT("data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Visible.\",\"reasoning_details\":[{\"signature\":\"native-signature\"}],\"x_native\":\"native-extension\",\"tool_calls\":[{\"index\":0,\"id\":\"a\",\"x_call\":\"call-extension\",\"function\":{\"name\":\"read_blueprint\",\"arguments\":\"{}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n"))
			: FString(TEXT("event: response.output_text.delta\ndata: {\"delta\":\"Visible.\"}\n\n")
				TEXT("event: response.output_text.done\ndata: {}\n\n")
				TEXT("event: response.output_item.done\ndata: {\"item\":{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"Visible.\",\"annotations\":[{\"type\":\"url_citation\",\"url\":\"https://fixture.invalid/citation\"}],\"x_nested\":\"nested-native-extension\"}]}}\n\n")
				TEXT("event: response.output_item.done\ndata: {\"item\":{\"type\":\"reasoning\",\"encrypted_content\":\"native-signature\",\"x_native\":\"native-extension\"}}\n\n")
				TEXT("event: response.output_item.done\ndata: {\"item\":{\"type\":\"function_call\",\"call_id\":\"a\",\"name\":\"read_blueprint\",\"arguments\":\"{}\",\"x_call\":\"call-extension\"}}\n\n")
				TEXT("event: response.completed\ndata: {\"response\":{}}\n\n"));
		TSharedPtr<ILLMStreamState> State = Provider->CreateStreamState();
		const FModelCallbacks Callbacks;
		FKismetAIStreamDecoder Decoder(*State, Callbacks);
		FTCHARToUTF8 Bytes(*Wire);
		Decoder.Feed(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
		Decoder.Finish();
		const FModelCallOutcome Outcome = FKismetAICurlModelClient::ClassifyAttempt(*Provider, *State, Wire, 0, 200, true, false);
		TestTrue(TEXT("native metadata turn succeeds"), Outcome.Result == EModelCallResult::Success);
		const FString Canonical = KismetAIFixtureTransport::Condensed(KismetAIAgentCodec::ToAnthropicJson(Outcome.AssistantMessage));
		TestTrue(TEXT("native thinking metadata retained canonically"), Canonical.Contains(TEXT("native-signature")));
		TestTrue(TEXT("unknown native field retained canonically"), Canonical.Contains(TEXT("native-extension")));
		TestTrue(TEXT("tool extension retained canonically"), Canonical.Contains(TEXT("call-extension")));
		if (!bChat)
		{
			TestTrue(TEXT("nested message extension retained"), Canonical.Contains(TEXT("nested-native-extension")));
			TestTrue(TEXT("text annotations retained"), Canonical.Contains(TEXT("https://fixture.invalid/citation")));
		}
		TestEqual(TEXT("native metadata never becomes text"), Outcome.AssistantMessage.GetText(), FString(TEXT("Visible.")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTypedProvider_MetadataNeverEchoed, "KismetAI.Provider.InternalMetadataNeverEchoed", TypedProviderFlags)
bool FTypedProvider_MetadataNeverEchoed::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Raw = MakeShared<FJsonObject>();
	Raw->SetStringField(TEXT("type"), TEXT("_kismet_provider_metadata"));
	Raw->SetStringField(TEXT("protocol"), TEXT("openai_responses"));
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("opaque"), FString(TEXT("private-native-metadata")) + FString::ChrN(100000, TEXT('x')));
	Raw->SetObjectField(TEXT("payload"), Payload);
	FAgentMessage Assistant;
	Assistant.Role = EAgentRole::Assistant;
	Assistant.Parts = KismetAIAgentCodec::FromParts({ MakeShared<FJsonValueObject>(Raw) });
	const FAgentMessage MetadataOnly = Assistant;
	Assistant.Parts.Add(FAgentContentPart::MakeText(TEXT("Visible.")));
	for (const TCHAR* Id : { TEXT("anthropic"), TEXT("minimax"), TEXT("openai"), TEXT("openrouter") })
	{
		FKismetAIAgentConfig Config;
		Config.ProviderId = Id;
		Config.Model = TEXT("fixture-model");
		TUniquePtr<ILLMProvider> Provider = MakeProviderForConfig(Config);
		if (!TestTrue(TEXT("provider exists"), Provider.IsValid())) return false;
		FLLMRequestShape Shape;
		const FString Wire = KismetAIFixtureTransport::Condensed(Provider->BuildRequestBody(Config, { MetadataOnly, Assistant }, {}, FString(), Shape));
		TestFalse(TEXT("internal native metadata never sent even after provider switch"), Wire.Contains(TEXT("private-native-metadata")));
		TestTrue(TEXT("ordinary content retained"), Wire.Contains(TEXT("Visible.")));
		const TSharedPtr<FJsonObject> Body = Provider->BuildRequestBody(Config, { MetadataOnly, Assistant }, {}, FString(), Shape);
		TestEqual(TEXT("metadata-only assistant is not emitted as an empty message"),
			Body->GetArrayField(FString(Id) == TEXT("openai") ? TEXT("input") : TEXT("messages")).Num(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTypedProvider_ComposedFlow, "KismetAI.Provider.TypedComposedFlow", TypedProviderFlags)
bool FTypedProvider_ComposedFlow::RunTest(const FString& Parameters)
{
	using namespace KismetAIFixtureTransport;
	using namespace KismetAITestFakes;
	FAnthropicStreamBuilder First;
	First.MessageStart().BlockStart(TEXT("thinking")).Thinking(TEXT("hidden-plan")).Signature(TEXT("composed-signature")).BlockStop()
		.Raw(TEXT("event: content_block_start\ndata: {\"content_block\":{\"type\":\"provider_extension\",\"opaque\":\"composed-opaque\"},\"index\":1}\n\n"))
		.BlockStop().BlockStart(TEXT("text")).Text(TEXT("Running.")).BlockStop()
		.BlockStart(TEXT("tool_use"), TEXT("read-id"), TEXT("read_blueprint")).ToolJson(TEXT("{\"blueprint\":\"/Game/BP_Fixture\"}")).BlockStop()
		.BlockStart(TEXT("tool_use"), TEXT("compile-id"), TEXT("compile_blueprint")).ToolJson(TEXT("{\"blueprint\":\"/Game/BP_Fixture\"}")).BlockStop()
		.MessageEnd(TEXT("tool_use"));
	FAnthropicStreamBuilder Second;
	Second.MessageStart().BlockStart(TEXT("text")).Text(TEXT("Finished.")).BlockStop().MessageEnd(TEXT("end_turn"));
	FScriptedAttempt FirstAttempt;
	FirstAttempt.Bytes = First.Bytes();
	FScriptedAttempt SecondAttempt;
	SecondAttempt.Bytes = Second.Bytes();
	FFixtureTransportModelClient Model({ FirstAttempt, SecondAttempt });
	FScriptedToolInvoker Tools;
	Tools.SetTypedResult(TEXT("read_blueprint"), FToolExecutionResult::Success());
	Tools.SetTypedResult(TEXT("compile_blueprint"), FToolExecutionResult::Error(400, TEXT("fixture-error"), TEXT("Expected fixture error")));
	FEventRecorder Events;
	FKismetAIAgentConfig Config;
	Config.Model = TEXT("claude-sonnet-4-6");
	Config.ApiKey = TEXT("unused-fixture-key");
	Config.MaxRetries = 0;
	Config.MaxSteps = 4;
	Config.ThinkingMode = EThinkingMode::Adaptive;
	FKismetAIAgent Agent(Tools, Model, Config);
	Agent.RunTask(TEXT("Read and compile the fixture."), Events.MakeCallback());
	struct FSessionCleanup
	{
		FString Id;
		~FSessionCleanup() { FKismetAISessionStorage::DeleteSession(Id); }
	} Cleanup{ Agent.GetSessionId() };
	TestEqual(TEXT("two calls ran through executor"), Tools.CallLog.Num(), 2);
	if (!TestEqual(TEXT("next typed request captured"), Model.CapturedMessages.Num(), 2)) return false;
	const FSessionSnapshot Snapshot = FKismetAISessionStorage::LoadSession(Cleanup.Id);
	TestTrue(TEXT("journal is readable"), Snapshot.CanResume());
	const TArray<const TArray<FAgentMessage>*> Histories{ &Model.CapturedMessages[1], &Snapshot.Messages };
	for (const TArray<FAgentMessage>* History : Histories)
	{
		bool bAssistant = false;
		bool bReadResult = false;
		bool bCompileResult = false;
		for (const FAgentMessage& Message : *History)
		{
			if (Message.HasToolUse())
			{
				bAssistant = true;
				const FString Json = Condensed(KismetAIAgentCodec::ToAnthropicJson(Message));
				TestTrue(TEXT("opaque survives live request and journal replay"), Json.Contains(TEXT("composed-opaque")));
				TestTrue(TEXT("thinking signature survives live request and journal replay"), Json.Contains(TEXT("composed-signature")));
				TestEqual(TEXT("hidden parts never become assistant text"), Message.GetText(), FString(TEXT("Running.")));
			}
			for (const FAgentContentPart& Part : Message.Parts)
			{
				if (Part.Type != EAgentPartType::ToolResult) continue;
				if (Part.ToolUseId == TEXT("read-id")) { bReadResult = true; TestFalse(TEXT("typed success retained"), Part.bIsError); }
				if (Part.ToolUseId == TEXT("compile-id")) { bCompileResult = true; TestTrue(TEXT("typed failure retained"), Part.bIsError); }
			}
		}
		TestTrue(TEXT("assistant turn retained"), bAssistant);
		TestTrue(TEXT("read result paired"), bReadResult);
		TestTrue(TEXT("compile result paired"), bCompileResult);
	}
	int32 ResultEvents = 0;
	for (const auto& Event : Events.Events)
	{
		if (Event.Key != TEXT("tool_result")) continue;
		TSharedPtr<FJsonObject> Payload;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Event.Value), Payload);
		if (!TestTrue(TEXT("result event is valid JSON"), Payload.IsValid())) return false;
		const FString Id = Payload->GetStringField(TEXT("tool_use_id"));
		TestTrue(TEXT("event retains call identity"), Id == TEXT("read-id") || Id == TEXT("compile-id"));
		TestEqual(TEXT("event error flag matches typed outcome"), Payload->GetBoolField(TEXT("is_error")), Id == TEXT("compile-id"));
		++ResultEvents;
	}
	TestEqual(TEXT("two result events delivered"), ResultEvents, 2);
	FAnthropicProvider Provider;
	FLLMRequestShape Shape;
	const FString Wire = Condensed(Provider.BuildRequestBody(Config, Model.CapturedMessages[1], {}, TEXT("system"), Shape));
	TestTrue(TEXT("next real provider request keeps signature"), Wire.Contains(TEXT("composed-signature")));
	TestTrue(TEXT("next real provider request keeps opaque extension"), Wire.Contains(TEXT("composed-opaque")));
	AddInfo(TEXT("Composed next provider request: ") + Wire);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTypedProvider_InitialPayload, "KismetAI.Provider.TypedInitialPayload", TypedProviderFlags)
bool FTypedProvider_InitialPayload::RunTest(const FString& Parameters)
{
	using namespace KismetAIFixtureTransport;
	FAnthropicStreamBuilder Stream;
	Stream.MessageStart()
		.Raw(TEXT("event: content_block_start\ndata: {\"content_block\":{\"type\":\"thinking\",\"thinking\":\"initial-thought\",\"signature\":\"initial-signature\"},\"index\":0}\n\n")).BlockStop()
		.Raw(TEXT("event: content_block_start\ndata: {\"content_block\":{\"type\":\"redacted_thinking\",\"data\":\"redacted-payload\"},\"index\":1}\n\n")).BlockStop()
		.Raw(TEXT("event: content_block_start\ndata: {\"content_block\":{\"type\":\"text\",\"text\":\"Initial.\"},\"index\":2}\n\n")).BlockStop()
		.Raw(TEXT("event: content_block_start\ndata: {\"content_block\":{\"type\":\"tool_use\",\"id\":\"initial-call\",\"name\":\"read_blueprint\",\"input\":{\"blueprint\":\"/Game/Initial\"}},\"index\":3}\n\n")).BlockStop()
		.MessageEnd(TEXT("tool_use"));
	FScriptedAttempt Attempt;
	Attempt.Bytes = Stream.Bytes();
	const FModelCallOutcome Outcome = RunAttempt(Attempt, {}, []() { return false; });
	TestTrue(TEXT("valid initial arguments succeed"), Outcome.Result == EModelCallResult::Success);
	const FString Json = Condensed(KismetAIAgentCodec::ToAnthropicJson(Outcome.AssistantMessage));
	for (const TCHAR* Payload : { TEXT("initial-thought"), TEXT("initial-signature"), TEXT("redacted-payload"), TEXT("/Game/Initial") })
	{
		TestTrue(TEXT("initial payload is not overwritten by absent deltas"), Json.Contains(Payload));
	}
	TestEqual(TEXT("initial visible text retained"), Outcome.AssistantMessage.GetText(), FString(TEXT("Initial.")));
	TestFalse(TEXT("successful outcome retains no duplicate JSON content"), Outcome.Response->HasField(TEXT("content")));
	return true;
}
