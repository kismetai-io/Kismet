// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

// FOpenAIChatProvider (Chat Completions wire protocol): request shape, endpoint
// and auth from a hand-built profile, and stream decoding from recorded-style
// SSE fixtures fed through the same parser + state wiring FCurlModelClient uses.
// All tests under "KismetAI.OpenAIChat.*".

#include "KismetAIOpenAIChatProvider.h"
#include "KismetAIAgent.h"
#include "KismetAILLMProvider.h"
#include "KismetAIModelCatalog.h"
#include "KismetAIProviderProfile.h"
#include "KismetAISSEParser.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

constexpr auto OpenAIChatTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// Named namespace: unity builds put several test .cpp files in one TU, and the
// other provider tests already define static helpers with generic names.
namespace OpenAIChatTestUtil
{
	FKismetAIProviderProfile MakeOpenRouterProfile()
	{
		FKismetAIProviderProfile P;
		P.Id = TEXT("openrouter");
		P.DisplayName = TEXT("OpenRouter");
		P.Protocol = EKismetAIWireProtocol::OpenAIChat;
		P.BaseUrl = TEXT("https://openrouter.ai/api/v1");
		P.KeySlot = TEXT("openrouter");
		P.Auth = EKismetAIAuthStyle::Bearer;
		P.ExtraHeaders.Add(TEXT("X-Title"), TEXT("KismetAI"));
		P.bCustomModels = true;
		return P;
	}

	FKismetAIProviderProfile MakeLocalProfile()
	{
		FKismetAIProviderProfile P;
		P.Id = TEXT("llamacpp-test");
		P.Protocol = EKismetAIWireProtocol::OpenAIChat;
		P.BaseUrl = TEXT("http://localhost:8080/v1");
		P.Auth = EKismetAIAuthStyle::None;
		P.bLocal = true;
		return P;
	}

	FKismetAIAgentConfig MakeConfig(const FString& Model = TEXT("some-org/custom-chat-model"))
	{
		FKismetAIAgentConfig Cfg;
		Cfg.Model = Model;
		Cfg.ApiKey = TEXT("sk-ant-must-not-leak");
		Cfg.OpenAIApiKey = TEXT("sk-openai-must-not-leak");
		Cfg.ProviderKeys.Add(TEXT("openrouter"), TEXT("sk-or-test"));
		Cfg.MaxTokens = 4096;
		Cfg.Temperature = 0.25f;
		Cfg.EffortLevel = EEffortLevel::Medium;
		return Cfg;
	}

	FKismetAIModelCapabilities PlainCaps()
	{
		FKismetAIModelCapabilities Caps;
		Caps.bReasoning = false;
		Caps.bSamplingParams = true;
		return Caps;
	}

	FKismetAIModelCapabilities ReasoningCaps(const TArray<FString>& Levels)
	{
		FKismetAIModelCapabilities Caps;
		Caps.bReasoning = true;
		Caps.bSamplingParams = false;
		Caps.EffortLevels = Levels;
		return Caps;
	}

	TSharedPtr<FJsonObject> ParseObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Obj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Obj);
		return Obj;
	}

	FString Serialize(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Out;
	}

	TSharedPtr<FJsonValue> Msg(const FString& Json)
	{
		return MakeShared<FJsonValueObject>(ParseObject(Json));
	}

	TArray<TSharedPtr<FJsonValue>> OneTool()
	{
		TArray<TSharedPtr<FJsonValue>> Tools;
		Tools.Add(Msg(TEXT("{\"name\":\"read_blueprint\",\"description\":\"Reads a blueprint\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"blueprint_name\":{\"type\":\"string\"}}}}")));
		return Tools;
	}

	TSharedPtr<FJsonObject> ObjectAt(const TArray<TSharedPtr<FJsonValue>>& Arr, int32 Index)
	{
		if (!Arr.IsValidIndex(Index) || !Arr[Index].IsValid())
		{
			return TSharedPtr<FJsonObject>();
		}
		return Arr[Index]->AsObject();
	}

	// Raw SSE bytes -> parser -> stream state, skipping events whose data is not
	// JSON exactly as FCurlModelClient does. Fed in small slices so events
	// straddle chunk boundaries the way they do on the wire.
	struct FChatHarness
	{
		FKismetAISSEParser Parser;
		TSharedPtr<ILLMStreamState> State;
		TArray<FString> TextDeltas;
		TArray<FString> ThinkingDeltas;
		TArray<FString> ThinkingCompletes;
		TArray<FString> Order;   // "think" / "thinkdone" / "text", in callback order

		FChatHarness()
		{
			FOpenAIChatProvider Provider(MakeOpenRouterProfile());
			State = Provider.CreateStreamState();
			State->OnThinkingDelta = [this](const FString& D) { ThinkingDeltas.Add(D); Order.Add(TEXT("think")); };
			State->OnThinkingComplete = [this](const FString& T) { ThinkingCompletes.Add(T); Order.Add(TEXT("thinkdone")); };
		}

		void FeedAll(const ANSICHAR* Bytes, int32 SliceSize = 7)
		{
			const int32 Total = FCStringAnsi::Strlen(Bytes);
			for (int32 Offset = 0; Offset < Total; Offset += SliceSize)
			{
				const int32 Count = FMath::Min(SliceSize, Total - Offset);
				Parser.Feed(reinterpret_cast<const uint8*>(Bytes + Offset), Count);
				for (const FKismetAISSEEvent& Event : Parser.DequeueEvents())
				{
					if (!Event.Data.IsValid()) { continue; }
					FString Delta;
					if (State->ProcessEvent(Event.EventType, Event.Data, Delta))
					{
						TextDeltas.Add(Delta);
						Order.Add(TEXT("text"));
					}
				}
			}
		}

		TSharedPtr<FJsonObject> Response() const { return State->BuildResponseJson(); }
		TArray<TSharedPtr<FJsonValue>> Content() const { return Response()->GetArrayField(TEXT("content")); }
		FString StopReason() const { return Response()->GetStringField(TEXT("stop_reason")); }
		bool StreamComplete() const
		{
			bool b = false;
			Response()->TryGetBoolField(TEXT("stream_complete"), b);
			return b;
		}
	};

	int32 UsageField(const TSharedPtr<FJsonObject>& Response, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Usage = nullptr;
		int32 Value = -1;
		if (Response->TryGetObjectField(TEXT("usage"), Usage))
		{
			(*Usage)->TryGetNumberField(Field, Value);
		}
		return Value;
	}
}

using namespace OpenAIChatTestUtil;

// ─────────────────────────────────────────────────────────────────────────────
// Request shape
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIChat_RequestTopLevel, "KismetAI.OpenAIChat.RequestTopLevelShape", OpenAIChatTestFlags)
bool FOpenAIChat_RequestTopLevel::RunTest(const FString& Parameters)
{
	FOpenAIChatProvider Provider(MakeOpenRouterProfile());
	FKismetAIAgentConfig Cfg = MakeConfig();
	TArray<TSharedPtr<FJsonValue>> Messages = { Msg(TEXT("{\"role\":\"user\",\"content\":\"Hello\"}")) };

	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBodyWithCaps(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), OneTool(),
		TEXT("You are KismetAI."), PlainCaps(), Shape);

	TestEqual(TEXT("model is the wire id"), Body->GetStringField(TEXT("model")), Cfg.Model);
	TestTrue(TEXT("stream true"), Body->GetBoolField(TEXT("stream")));
	TestTrue(TEXT("stream_options.include_usage"), Body->GetObjectField(TEXT("stream_options"))->GetBoolField(TEXT("include_usage")));
	TestEqual(TEXT("max_tokens"), (int32)Body->GetNumberField(TEXT("max_tokens")), 4096);
	TestEqual(TEXT("tool_choice auto"), Body->GetStringField(TEXT("tool_choice")), FString(TEXT("auto")));

	const TArray<TSharedPtr<FJsonValue>>& ChatMessages = Body->GetArrayField(TEXT("messages"));
	TestEqual(TEXT("system + user"), ChatMessages.Num(), 2);
	TestEqual(TEXT("system first"), ObjectAt(ChatMessages, 0)->GetStringField(TEXT("role")), FString(TEXT("system")));
	TestEqual(TEXT("system text"), ObjectAt(ChatMessages, 0)->GetStringField(TEXT("content")), FString(TEXT("You are KismetAI.")));
	TestEqual(TEXT("user second"), ObjectAt(ChatMessages, 1)->GetStringField(TEXT("content")), FString(TEXT("Hello")));

	const TArray<TSharedPtr<FJsonValue>>& Tools = Body->GetArrayField(TEXT("tools"));
	TestEqual(TEXT("one tool"), Tools.Num(), 1);
	TSharedPtr<FJsonObject> Tool = ObjectAt(Tools, 0);
	TestEqual(TEXT("tool type function"), Tool->GetStringField(TEXT("type")), FString(TEXT("function")));
	TSharedPtr<FJsonObject> Function = Tool->GetObjectField(TEXT("function"));
	TestEqual(TEXT("function.name"), Function->GetStringField(TEXT("name")), FString(TEXT("read_blueprint")));
	TestEqual(TEXT("function.description"), Function->GetStringField(TEXT("description")), FString(TEXT("Reads a blueprint")));
	TestTrue(TEXT("input_schema -> parameters"), Function->GetObjectField(TEXT("parameters"))->HasField(TEXT("properties")));
	TestFalse(TEXT("no input_schema"), Function->HasField(TEXT("input_schema")));

	// No Anthropic-only or Responses-only fields.
	for (const TCHAR* Field : { TEXT("system"), TEXT("thinking"), TEXT("output_config"), TEXT("input"),
		TEXT("instructions"), TEXT("max_output_tokens"), TEXT("reasoning"), TEXT("speed") })
	{
		TestFalse(FString::Printf(TEXT("no top-level '%s'"), Field), Body->HasField(Field));
	}
	TestFalse(TEXT("no cache_control anywhere"), Serialize(Body).Contains(TEXT("cache_control")));
	TestFalse(TEXT("thinking not enabled"), Shape.bThinkingEnabled);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIChat_NoToolsNoToolChoice, "KismetAI.OpenAIChat.NoToolsOmitsToolChoice", OpenAIChatTestFlags)
bool FOpenAIChat_NoToolsNoToolChoice::RunTest(const FString& Parameters)
{
	FOpenAIChatProvider Provider(MakeOpenRouterProfile());
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBodyWithCaps(MakeConfig(),
		KismetAIAgentCodec::FromAnthropicArray({ Msg(TEXT("{\"role\":\"user\",\"content\":\"Hi\"}")) }), {}, FString(), PlainCaps(), Shape);
	TestFalse(TEXT("no tools"), Body->HasField(TEXT("tools")));
	TestFalse(TEXT("no tool_choice without tools"), Body->HasField(TEXT("tool_choice")));
	TestEqual(TEXT("empty system prompt is not sent"), Body->GetArrayField(TEXT("messages")).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIChat_ToolRoundTrip, "KismetAI.OpenAIChat.ToolUseAndResultRoundTrip", OpenAIChatTestFlags)
bool FOpenAIChat_ToolRoundTrip::RunTest(const FString& Parameters)
{
	FOpenAIChatProvider Provider(MakeOpenRouterProfile());
	TArray<TSharedPtr<FJsonValue>> Messages = {
		Msg(TEXT("{\"role\":\"user\",\"content\":\"Fix BP_Door\"}")),
		Msg(TEXT("{\"role\":\"assistant\",\"content\":["
			"{\"type\":\"thinking\",\"thinking\":\"plan it\",\"signature\":\"sig\"},"
			"{\"type\":\"text\",\"text\":\"Reading both.\"},"
			"{\"type\":\"tool_use\",\"id\":\"call_a\",\"name\":\"read_blueprint\",\"input\":{\"blueprint_name\":\"BP_Door\"}},"
			"{\"type\":\"tool_use\",\"id\":\"call_b\",\"name\":\"compile_blueprint\",\"input\":{}}]}")),
		Msg(TEXT("{\"role\":\"user\",\"content\":["
			"{\"type\":\"tool_result\",\"tool_use_id\":\"call_a\",\"content\":\"graph ok\"},"
			"{\"type\":\"tool_result\",\"tool_use_id\":\"call_b\",\"content\":\"compile failed\",\"is_error\":true},"
			"{\"type\":\"text\",\"text\":\"[reminder] keep going\"}]}"))
	};

	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBodyWithCaps(MakeConfig(), KismetAIAgentCodec::FromAnthropicArray(Messages), OneTool(),
		TEXT("sys"), PlainCaps(), Shape);
	const TArray<TSharedPtr<FJsonValue>>& M = Body->GetArrayField(TEXT("messages"));

	// system, user, assistant, tool, tool, user
	TestEqual(TEXT("message count"), M.Num(), 6);
	TSharedPtr<FJsonObject> Assistant = ObjectAt(M, 2);
	TestEqual(TEXT("assistant role"), Assistant->GetStringField(TEXT("role")), FString(TEXT("assistant")));
	TestEqual(TEXT("assistant text only (thinking dropped)"), Assistant->GetStringField(TEXT("content")), FString(TEXT("Reading both.")));
	const TArray<TSharedPtr<FJsonValue>>& Calls = Assistant->GetArrayField(TEXT("tool_calls"));
	TestEqual(TEXT("two tool_calls"), Calls.Num(), 2);
	TSharedPtr<FJsonObject> Call0 = ObjectAt(Calls, 0);
	TestEqual(TEXT("call id"), Call0->GetStringField(TEXT("id")), FString(TEXT("call_a")));
	TestEqual(TEXT("call type"), Call0->GetStringField(TEXT("type")), FString(TEXT("function")));
	TestEqual(TEXT("call name"), Call0->GetObjectField(TEXT("function"))->GetStringField(TEXT("name")), FString(TEXT("read_blueprint")));
	TSharedPtr<FJsonObject> Args = ParseObject(Call0->GetObjectField(TEXT("function"))->GetStringField(TEXT("arguments")));
	TestTrue(TEXT("arguments is a JSON string of the input"), Args.IsValid() && Args->GetStringField(TEXT("blueprint_name")) == TEXT("BP_Door"));
	TestEqual(TEXT("empty input -> {}"), ObjectAt(Calls, 1)->GetObjectField(TEXT("function"))->GetStringField(TEXT("arguments")), FString(TEXT("{}")));

	TSharedPtr<FJsonObject> Tool0 = ObjectAt(M, 3);
	TSharedPtr<FJsonObject> Tool1 = ObjectAt(M, 4);
	TestEqual(TEXT("tool role"), Tool0->GetStringField(TEXT("role")), FString(TEXT("tool")));
	TestEqual(TEXT("first result first"), Tool0->GetStringField(TEXT("tool_call_id")), FString(TEXT("call_a")));
	TestEqual(TEXT("ok result unchanged"), Tool0->GetStringField(TEXT("content")), FString(TEXT("graph ok")));
	TestEqual(TEXT("second result second"), Tool1->GetStringField(TEXT("tool_call_id")), FString(TEXT("call_b")));
	TestEqual(TEXT("error result prefixed"), Tool1->GetStringField(TEXT("content")),
		FString(FOpenAIChatProvider::ToolErrorPrefix) + TEXT("compile failed"));
	TSharedPtr<FJsonObject> Trailing = ObjectAt(M, 5);
	TestEqual(TEXT("text after the tool messages"), Trailing->GetStringField(TEXT("role")), FString(TEXT("user")));
	TestEqual(TEXT("trailing text"), Trailing->GetStringField(TEXT("content")), FString(TEXT("[reminder] keep going")));

	const FString Json = Serialize(Body);
	TestFalse(TEXT("no thinking text leaks"), Json.Contains(TEXT("plan it")));
	TestFalse(TEXT("no signature leaks"), Json.Contains(TEXT("signature")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIChat_ToolOnlyAssistant, "KismetAI.OpenAIChat.ToolOnlyAssistantHasNullContent", OpenAIChatTestFlags)
bool FOpenAIChat_ToolOnlyAssistant::RunTest(const FString& Parameters)
{
	FOpenAIChatProvider Provider(MakeOpenRouterProfile());
	TArray<TSharedPtr<FJsonValue>> Messages = {
		Msg(TEXT("{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"c1\",\"name\":\"compile_blueprint\",\"input\":{}}]}")),
		Msg(TEXT("{\"role\":\"assistant\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"only thinking\"}]}"))
	};
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBodyWithCaps(MakeConfig(), KismetAIAgentCodec::FromAnthropicArray(Messages), {}, FString(), PlainCaps(), Shape);
	const TArray<TSharedPtr<FJsonValue>>& M = Body->GetArrayField(TEXT("messages"));
	TestEqual(TEXT("thinking-only turn dropped"), M.Num(), 1);
	TSharedPtr<FJsonValue> Content = ObjectAt(M, 0)->TryGetField(TEXT("content"));
	TestTrue(TEXT("content is JSON null alongside tool_calls"), Content.IsValid() && Content->Type == EJson::Null);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIChat_Images, "KismetAI.OpenAIChat.UserImagesBecomeDataUris", OpenAIChatTestFlags)
bool FOpenAIChat_Images::RunTest(const FString& Parameters)
{
	FOpenAIChatProvider Provider(MakeOpenRouterProfile());
	TArray<TSharedPtr<FJsonValue>> Messages = {
		Msg(TEXT("{\"role\":\"user\",\"content\":["
			"{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":\"image/jpeg\",\"data\":\"QUJD\"}},"
			"{\"type\":\"text\",\"text\":\"What is this?\"}]}"))
	};
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBodyWithCaps(MakeConfig(), KismetAIAgentCodec::FromAnthropicArray(Messages), {}, FString(), PlainCaps(), Shape);
	TSharedPtr<FJsonObject> User = ObjectAt(Body->GetArrayField(TEXT("messages")), 0);
	const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
	if (!TestTrue(TEXT("content is a parts array"), User->TryGetArrayField(TEXT("content"), Parts) && Parts && Parts->Num() == 2))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Image = ObjectAt(*Parts, 0);
	TestEqual(TEXT("image part type"), Image->GetStringField(TEXT("type")), FString(TEXT("image_url")));
	TestEqual(TEXT("data URI"), Image->GetObjectField(TEXT("image_url"))->GetStringField(TEXT("url")),
		FString(TEXT("data:image/jpeg;base64,QUJD")));
	TSharedPtr<FJsonObject> Text = ObjectAt(*Parts, 1);
	TestEqual(TEXT("text part type"), Text->GetStringField(TEXT("type")), FString(TEXT("text")));
	TestEqual(TEXT("text part"), Text->GetStringField(TEXT("text")), FString(TEXT("What is this?")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIChat_ReasoningVsTemperature, "KismetAI.OpenAIChat.ReasoningEffortVersusTemperature", OpenAIChatTestFlags)
bool FOpenAIChat_ReasoningVsTemperature::RunTest(const FString& Parameters)
{
	FOpenAIChatProvider Provider(MakeOpenRouterProfile());
	TArray<TSharedPtr<FJsonValue>> Messages = { Msg(TEXT("{\"role\":\"user\",\"content\":\"Hi\"}")) };

	{
		FLLMRequestShape Shape;
		TSharedPtr<FJsonObject> Body = Provider.BuildRequestBodyWithCaps(MakeConfig(), KismetAIAgentCodec::FromAnthropicArray(Messages), {}, FString(),
			ReasoningCaps({ TEXT("low"), TEXT("medium"), TEXT("high") }), Shape);
		TestEqual(TEXT("reasoning_effort sent"), Body->GetStringField(TEXT("reasoning_effort")), FString(TEXT("medium")));
		TestFalse(TEXT("reasoning model omits temperature"), Body->HasField(TEXT("temperature")));
		TestTrue(TEXT("shape records effort"), Shape.bEffortEnabled);
	}
	{
		FLLMRequestShape Shape;
		TSharedPtr<FJsonObject> Body = Provider.BuildRequestBodyWithCaps(MakeConfig(), KismetAIAgentCodec::FromAnthropicArray(Messages), {}, FString(), PlainCaps(), Shape);
		TestFalse(TEXT("plain model has no reasoning_effort"), Body->HasField(TEXT("reasoning_effort")));
		TestEqual(TEXT("temperature from config"), (float)Body->GetNumberField(TEXT("temperature")), 0.25f);
		TestFalse(TEXT("no effort in shape"), Shape.bEffortEnabled);
	}
	{
		FKismetAIModelCapabilities NoSampling = PlainCaps();
		NoSampling.bSamplingParams = false;
		FLLMRequestShape Shape;
		TSharedPtr<FJsonObject> Body = Provider.BuildRequestBodyWithCaps(MakeConfig(), KismetAIAgentCodec::FromAnthropicArray(Messages), {}, FString(), NoSampling, Shape);
		TestFalse(TEXT("no temperature when sampling params are unsupported"), Body->HasField(TEXT("temperature")));
		TestFalse(TEXT("and no reasoning_effort"), Body->HasField(TEXT("reasoning_effort")));
	}
	{
		// The catalog path uses conservative unknown capabilities, independently
		// of the defaults inherited by known entries.
		FLLMRequestShape Shape;
		TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(MakeConfig(TEXT("acme/unknown-model-zz9")), KismetAIAgentCodec::FromAnthropicArray(Messages), {}, FString(), Shape);
		const FKismetAIModelCapabilities& Defaults = FKismetAIModelCatalog::Get().FindOrDefaults(TEXT("acme/unknown-model-zz9")).Caps;
		TestTrue(TEXT("catalog path follows the defaults (temperature)"), Body->HasField(TEXT("temperature")) == (!Defaults.bReasoning && Defaults.bSamplingParams));
		TestTrue(TEXT("catalog path follows the defaults (effort)"), Body->HasField(TEXT("reasoning_effort")) == Defaults.bReasoning);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIChat_EffortClamp, "KismetAI.OpenAIChat.ReasoningEffortIsClamped", OpenAIChatTestFlags)
bool FOpenAIChat_EffortClamp::RunTest(const FString& Parameters)
{
	FOpenAIChatProvider Provider(MakeOpenRouterProfile());
	TArray<TSharedPtr<FJsonValue>> Messages = { Msg(TEXT("{\"role\":\"user\",\"content\":\"Hi\"}")) };

	auto EffortFor = [&](EEffortLevel Level, const TArray<FString>& Levels)
	{
		FKismetAIAgentConfig Cfg = MakeConfig();
		Cfg.EffortLevel = Level;
		FLLMRequestShape Shape;
		return Provider.BuildRequestBodyWithCaps(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), {}, FString(), ReasoningCaps(Levels), Shape)
			->GetStringField(TEXT("reasoning_effort"));
	};

	TestEqual(TEXT("max clamps down to high"), EffortFor(EEffortLevel::Max, { TEXT("low"), TEXT("medium"), TEXT("high") }), FString(TEXT("high")));
	TestEqual(TEXT("no list = low/medium/high"), EffortFor(EEffortLevel::XHigh, {}), FString(TEXT("high")));
	TestEqual(TEXT("supported level passes through"), EffortFor(EEffortLevel::XHigh,
		{ TEXT("low"), TEXT("medium"), TEXT("high"), TEXT("xhigh"), TEXT("max") }), FString(TEXT("xhigh")));
	TestEqual(TEXT("below the floor rises to the lowest supported"), EffortFor(EEffortLevel::Low,
		{ TEXT("medium"), TEXT("high") }), FString(TEXT("medium")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIChat_EndpointAndHeaders, "KismetAI.OpenAIChat.EndpointAndAuthFromProfile", OpenAIChatTestFlags)
bool FOpenAIChat_EndpointAndHeaders::RunTest(const FString& Parameters)
{
	FKismetAIProviderProfile Profile = MakeOpenRouterProfile();
	Profile.StripModelSuffix = TEXT(":route");
	FOpenAIChatProvider Provider(Profile);
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("deepseek/deepseek-r2:route"));

	TestEqual(TEXT("endpoint"), Provider.GetEndpointUrl(Cfg), FString(TEXT("https://openrouter.ai/api/v1/chat/completions")));

	FLLMRequestShape Shape;
	TestEqual(TEXT("wire model id from the profile"),
		Provider.BuildRequestBodyWithCaps(Cfg, {}, {}, FString(), PlainCaps(), Shape)->GetStringField(TEXT("model")),
		FString(TEXT("deepseek/deepseek-r2")));

	const TArray<FString> Headers = Provider.GetExtraCurlHeaders(Cfg, Shape);
	TestTrue(TEXT("bearer with this profile's key"), Headers.Contains(TEXT("Authorization: Bearer sk-or-test")));
	TestTrue(TEXT("extra header"), Headers.Contains(TEXT("X-Title: KismetAI")));
	for (const FString& Header : Headers)
	{
		TestFalse(TEXT("no Anthropic key"), Header.Contains(Cfg.ApiKey));
		TestFalse(TEXT("no OpenAI key"), Header.Contains(Cfg.OpenAIApiKey));
		TestFalse(TEXT("no x-api-key for a bearer profile"), Header.StartsWith(TEXT("x-api-key")));
	}

	// A settings override wins and its trailing slash is dropped.
	Cfg.ProviderBaseUrls.Add(TEXT("openrouter"), TEXT("http://127.0.0.1:9999/v1/"));
	TestEqual(TEXT("override endpoint"), Provider.GetEndpointUrl(Cfg), FString(TEXT("http://127.0.0.1:9999/v1/chat/completions")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIChat_AuthNone, "KismetAI.OpenAIChat.LocalProfileSendsNoAuth", OpenAIChatTestFlags)
bool FOpenAIChat_AuthNone::RunTest(const FString& Parameters)
{
	FOpenAIChatProvider Provider(MakeLocalProfile());
	FKismetAIAgentConfig Cfg = MakeConfig(TEXT("qwen3-coder"));
	FLLMRequestShape Shape;
	TestEqual(TEXT("local endpoint"), Provider.GetEndpointUrl(Cfg), FString(TEXT("http://localhost:8080/v1/chat/completions")));
	TestEqual(TEXT("no headers at all"), Provider.GetExtraCurlHeaders(Cfg, Shape).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIChat_Name, "KismetAI.OpenAIChat.NameIsProfileIdNeverOpenAI", OpenAIChatTestFlags)
bool FOpenAIChat_Name::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("profile id"), FOpenAIChatProvider(MakeOpenRouterProfile()).GetName(), FString(TEXT("openrouter")));

	FKismetAIProviderProfile OpenAIish = MakeOpenRouterProfile();
	OpenAIish.Id = TEXT("OpenAI");
	const FString Name = FOpenAIChatProvider(OpenAIish).GetName();
	TestFalse(TEXT("never reports openai (inclusive-usage semantics)"), Name.Equals(TEXT("openai"), ESearchCase::IgnoreCase));

	FKismetAIProviderProfile Empty;
	TestFalse(TEXT("empty id still has a name"), FOpenAIChatProvider(Empty).GetName().IsEmpty());

	TestEqual(TEXT("cache TTL"), FOpenAIChatProvider(MakeOpenRouterProfile()).CacheTtlSeconds(), 300);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIChat_ErrorBody, "KismetAI.OpenAIChat.ParseErrorBody", OpenAIChatTestFlags)
bool FOpenAIChat_ErrorBody::RunTest(const FString& Parameters)
{
	FOpenAIChatProvider Provider(MakeOpenRouterProfile());
	{
		FModelCallOutcome Outcome;
		Provider.ParseErrorBody(ParseObject(TEXT("{\"error\":{\"message\":\"You exceeded your quota\",\"type\":\"insufficient_quota\",\"code\":\"insufficient_quota\"}}")), Outcome);
		TestEqual(TEXT("string code is the type"), Outcome.ErrorType, FString(TEXT("insufficient_quota")));
		TestEqual(TEXT("message"), Outcome.ErrorMessage, FString(TEXT("You exceeded your quota")));
	}
	{
		FModelCallOutcome Outcome;
		Provider.ParseErrorBody(ParseObject(TEXT("{\"error\":{\"message\":\"No auth credentials found\",\"code\":401}}")), Outcome);
		TestEqual(TEXT("message from numeric-code body"), Outcome.ErrorMessage, FString(TEXT("No auth credentials found")));
		TestEqual(TEXT("numeric code is not used as the type"), Outcome.ErrorType, FString());
	}
	{
		FModelCallOutcome Outcome;
		Provider.ParseErrorBody(ParseObject(TEXT("{\"object\":\"error\",\"message\":\"This model's maximum context length is 32768 tokens. However, you requested 40000 tokens (35000 in the messages, 5000 in the completion).\",\"type\":\"BadRequestError\",\"code\":400}")), Outcome);
		TestEqual(TEXT("flat body overflow -> context_length_exceeded"), Outcome.ErrorType, FString(TEXT("context_length_exceeded")));
		TestEqual(TEXT("context limit"), Outcome.ContextLimit, 32768);
		TestEqual(TEXT("input tokens"), Outcome.InputTokens, 35000);
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Stream decoding
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIChat_StreamText, "KismetAI.OpenAIChat.StreamPlainText", OpenAIChatTestFlags)
bool FOpenAIChat_StreamText::RunTest(const FString& Parameters)
{
	FChatHarness H;
	H.FeedAll(
		"data: {\"id\":\"gen-1\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"\"},\"finish_reason\":null}]}\n\n"
		": OPENROUTER PROCESSING\n\n"
		"data: {\"id\":\"gen-1\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"The graph \"},\"finish_reason\":null}]}\n\n"
		"data: {\"id\":\"gen-1\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"is fine.\"},\"finish_reason\":null}]}\n\n"
		"data: {\"id\":\"gen-1\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
		"data: {\"id\":\"gen-1\",\"object\":\"chat.completion.chunk\",\"choices\":[],\"usage\":{\"prompt_tokens\":120,\"completion_tokens\":9,\"total_tokens\":129}}\n\n"
		"data: [DONE]\n\n");

	TestEqual(TEXT("two text deltas, no empty one"), H.TextDeltas.Num(), 2);
	TestEqual(TEXT("joined"), FString::Join(H.TextDeltas, TEXT("")), FString(TEXT("The graph is fine.")));
	const TArray<TSharedPtr<FJsonValue>> Content = H.Content();
	TestEqual(TEXT("one block"), Content.Num(), 1);
	TestEqual(TEXT("text block"), ObjectAt(Content, 0)->GetStringField(TEXT("text")), FString(TEXT("The graph is fine.")));
	TestEqual(TEXT("stop -> end_turn"), H.StopReason(), FString(TEXT("end_turn")));
	TestTrue(TEXT("complete"), H.StreamComplete());
	TestTrue(TEXT("usage complete"), H.Response()->GetBoolField(TEXT("usage_complete")));
	FString Message, Code;
	TestFalse(TEXT("no stream error"), H.State->HasStreamError(Message, Code));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIChat_StreamSplitArgs, "KismetAI.OpenAIChat.StreamSplitToolArguments", OpenAIChatTestFlags)
bool FOpenAIChat_StreamSplitArgs::RunTest(const FString& Parameters)
{
	FChatHarness H;
	H.FeedAll(
		"data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"index\":0,\"id\":\"call_x1\",\"type\":\"function\",\"function\":{\"name\":\"read_blueprint\",\"arguments\":\"\"}}]},\"finish_reason\":null}]}\n\n"
		"data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{\\\"bluep\"}}]},\"finish_reason\":null}]}\n\n"
		"data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"rint_name\\\":\\\"BP_\"}}]},\"finish_reason\":null}]}\n\n"
		"data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"Door\\\"}\"}}]},\"finish_reason\":null}]}\n\n"
		"data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
		"data: [DONE]\n\n");

	TestEqual(TEXT("no text deltas"), H.TextDeltas.Num(), 0);
	const TArray<TSharedPtr<FJsonValue>> Content = H.Content();
	if (!TestEqual(TEXT("one tool_use"), Content.Num(), 1)) { return false; }
	TSharedPtr<FJsonObject> Tool = ObjectAt(Content, 0);
	TestEqual(TEXT("type"), Tool->GetStringField(TEXT("type")), FString(TEXT("tool_use")));
	TestEqual(TEXT("id"), Tool->GetStringField(TEXT("id")), FString(TEXT("call_x1")));
	TestEqual(TEXT("name"), Tool->GetStringField(TEXT("name")), FString(TEXT("read_blueprint")));
	TestEqual(TEXT("reassembled input"), Tool->GetObjectField(TEXT("input"))->GetStringField(TEXT("blueprint_name")), FString(TEXT("BP_Door")));
	TestEqual(TEXT("tool_calls -> tool_use"), H.StopReason(), FString(TEXT("tool_use")));
	TestTrue(TEXT("complete"), H.StreamComplete());
	TestFalse(TEXT("no usage reported -> usage incomplete"), H.Response()->GetBoolField(TEXT("usage_complete")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIChat_StreamInterleaved, "KismetAI.OpenAIChat.StreamInterleavedToolCalls", OpenAIChatTestFlags)
bool FOpenAIChat_StreamInterleaved::RunTest(const FString& Parameters)
{
	FChatHarness H;
	H.FeedAll(
		"data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Checking.\"},\"finish_reason\":null}]}\n\n"
		"data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_a\",\"type\":\"function\",\"function\":{\"name\":\"read_blueprint\",\"arguments\":\"{\\\"blueprint_name\\\":\"}}]},\"finish_reason\":null}]}\n\n"
		"data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":1,\"id\":\"call_b\",\"type\":\"function\",\"function\":{\"name\":\"search_assets\",\"arguments\":\"{\\\"query\\\":\"}}]},\"finish_reason\":null}]}\n\n"
		"data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"\\\"BP_A\\\"}\"}},{\"index\":1,\"function\":{\"arguments\":\"\\\"door\\\"}\"}}]},\"finish_reason\":null}]}\n\n"
		"data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":50,\"completion_tokens\":20}}\n\n");

	const TArray<TSharedPtr<FJsonValue>> Content = H.Content();
	if (!TestEqual(TEXT("text + two tools"), Content.Num(), 3)) { return false; }
	TestEqual(TEXT("text first"), ObjectAt(Content, 0)->GetStringField(TEXT("type")), FString(TEXT("text")));
	TSharedPtr<FJsonObject> A = ObjectAt(Content, 1);
	TSharedPtr<FJsonObject> B = ObjectAt(Content, 2);
	TestEqual(TEXT("index 0 first"), A->GetStringField(TEXT("id")), FString(TEXT("call_a")));
	TestEqual(TEXT("index 0 args"), A->GetObjectField(TEXT("input"))->GetStringField(TEXT("blueprint_name")), FString(TEXT("BP_A")));
	TestEqual(TEXT("index 1 second"), B->GetStringField(TEXT("id")), FString(TEXT("call_b")));
	TestEqual(TEXT("index 1 name"), B->GetStringField(TEXT("name")), FString(TEXT("search_assets")));
	TestEqual(TEXT("index 1 args"), B->GetObjectField(TEXT("input"))->GetStringField(TEXT("query")), FString(TEXT("door")));
	TestEqual(TEXT("tool_use"), H.StopReason(), FString(TEXT("tool_use")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIChat_StreamReasoning, "KismetAI.OpenAIChat.StreamReasoningThenText", OpenAIChatTestFlags)
bool FOpenAIChat_StreamReasoning::RunTest(const FString& Parameters)
{
	// DeepSeek / vLLM spelling.
	{
		FChatHarness H;
		H.FeedAll(
			"data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"reasoning_content\":\"Check the \"},\"finish_reason\":null}]}\n\n"
			"data: {\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"pins.\"},\"finish_reason\":null}]}\n\n"
			"data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Done.\"},\"finish_reason\":null}]}\n\n"
			"data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n");

		TestEqual(TEXT("thinking deltas streamed"), H.ThinkingDeltas.Num(), 2);
		TestEqual(TEXT("thinking completed once"), H.ThinkingCompletes.Num(), 1);
		TestEqual(TEXT("complete text"), H.ThinkingCompletes.IsEmpty() ? FString() : H.ThinkingCompletes[0], FString(TEXT("Check the pins.")));
		TestEqual(TEXT("order: think, think, thinkdone, text"), FString::Join(H.Order, TEXT(",")), FString(TEXT("think,think,thinkdone,text")));
		const TArray<TSharedPtr<FJsonValue>> Content = H.Content();
		if (!TestEqual(TEXT("thinking + text"), Content.Num(), 2)) { return false; }
		TestEqual(TEXT("thinking block first"), ObjectAt(Content, 0)->GetStringField(TEXT("type")), FString(TEXT("thinking")));
		TestEqual(TEXT("thinking text"), ObjectAt(Content, 0)->GetStringField(TEXT("thinking")), FString(TEXT("Check the pins.")));
		TestEqual(TEXT("then text"), ObjectAt(Content, 1)->GetStringField(TEXT("text")), FString(TEXT("Done.")));
		TestEqual(TEXT("reasoning is not a text delta"), FString::Join(H.TextDeltas, TEXT("")), FString(TEXT("Done.")));
	}
	// OpenRouter spelling, reasoning straight into a tool call.
	{
		FChatHarness H;
		H.FeedAll(
			"data: {\"choices\":[{\"index\":0,\"delta\":{\"reasoning\":\"Need the graph.\"},\"finish_reason\":null}]}\n\n"
			"data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c\",\"function\":{\"name\":\"read_blueprint\",\"arguments\":\"{}\"}}]},\"finish_reason\":null}]}\n\n"
			"data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n");
		TestEqual(TEXT("thinking finished before the tool"), H.ThinkingCompletes.Num(), 1);
		const TArray<TSharedPtr<FJsonValue>> Content = H.Content();
		TestEqual(TEXT("thinking + tool_use"), Content.Num(), 2);
		TestEqual(TEXT("thinking first"), ObjectAt(Content, 0)->GetStringField(TEXT("type")), FString(TEXT("thinking")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIChat_StreamLength, "KismetAI.OpenAIChat.StreamLengthIsMaxTokens", OpenAIChatTestFlags)
bool FOpenAIChat_StreamLength::RunTest(const FString& Parameters)
{
	FChatHarness H;
	H.FeedAll(
		"data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"A long answer that\"},\"finish_reason\":null}]}\n\n"
		"data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"length\"}]}\n\n"
		"data: {\"choices\":[],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":4096}}\n\n"
		"data: [DONE]\n\n");
	TestEqual(TEXT("length -> max_tokens"), H.StopReason(), FString(TEXT("max_tokens")));
	TestTrue(TEXT("complete"), H.StreamComplete());
	TestEqual(TEXT("output tokens"), UsageField(H.Response(), TEXT("output_tokens")), 4096);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIChat_StreamUsage, "KismetAI.OpenAIChat.StreamUsageIsDisjoint", OpenAIChatTestFlags)
bool FOpenAIChat_StreamUsage::RunTest(const FString& Parameters)
{
	const ANSICHAR* Finish = "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}\n\n";
	{
		FChatHarness H;
		H.FeedAll(Finish);
		H.FeedAll("data: {\"choices\":[],\"usage\":{\"prompt_tokens\":1000,\"completion_tokens\":50,\"prompt_tokens_details\":{\"cached_tokens\":800}}}\n\n");
		const TSharedPtr<FJsonObject> R = H.Response();
		TestEqual(TEXT("input excludes cached"), UsageField(R, TEXT("input_tokens")), 200);
		TestEqual(TEXT("cache read"), UsageField(R, TEXT("cache_read_input_tokens")), 800);
		TestEqual(TEXT("output"), UsageField(R, TEXT("output_tokens")), 50);
		TestEqual(TEXT("no cache writes"), UsageField(R, TEXT("cache_creation_input_tokens")), 0);
		TestTrue(TEXT("usage complete"), R->GetBoolField(TEXT("usage_complete")));
	}
	{
		FChatHarness H;
		H.FeedAll(Finish);
		H.FeedAll("data: {\"choices\":[],\"usage\":{\"prompt_tokens\":1000,\"completion_tokens\":50,\"cached_tokens\":300}}\n\n");
		TestEqual(TEXT("flat cached_tokens"), UsageField(H.Response(), TEXT("input_tokens")), 700);
		TestEqual(TEXT("flat cache read"), UsageField(H.Response(), TEXT("cache_read_input_tokens")), 300);
	}
	{
		FChatHarness H;
		H.FeedAll(Finish);
		H.FeedAll("data: {\"choices\":[],\"usage\":{\"prompt_tokens\":1000,\"completion_tokens\":50}}\n\n");
		TestEqual(TEXT("no cache info: all input"), UsageField(H.Response(), TEXT("input_tokens")), 1000);
		TestEqual(TEXT("no cache info: zero read"), UsageField(H.Response(), TEXT("cache_read_input_tokens")), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIChat_StreamDone, "KismetAI.OpenAIChat.DataOnlyEventsAndDoneSentinel", OpenAIChatTestFlags)
bool FOpenAIChat_StreamDone::RunTest(const FString& Parameters)
{
	// What the shared parser actually produces for a data-only stream.
	FKismetAISSEParser Parser;
	const ANSICHAR* Bytes =
		"data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"x\"},\"finish_reason\":\"stop\"}]}\n\n"
		"data: [DONE]\n\n";
	Parser.Feed(reinterpret_cast<const uint8*>(Bytes), FCStringAnsi::Strlen(Bytes));
	const TArray<FKismetAISSEEvent> Events = Parser.DequeueEvents();
	if (!TestEqual(TEXT("two events"), Events.Num(), 2)) { return false; }
	TestTrue(TEXT("data-only chunk has an empty event type"), Events[0].EventType.IsEmpty());
	TestTrue(TEXT("chunk parsed"), Events[0].Data.IsValid());
	TestTrue(TEXT("[DONE] has an empty event type"), Events[1].EventType.IsEmpty());
	TestFalse(TEXT("[DONE] is not JSON, so Data is null (the client drops it)"), Events[1].Data.IsValid());

	// And a state handed a null Data (should a caller not filter it) ignores it.
	FOpenAIChatProvider Provider(MakeOpenRouterProfile());
	TSharedPtr<ILLMStreamState> State = Provider.CreateStreamState();
	FString Delta;
	TestFalse(TEXT("null data is a no-op"), State->ProcessEvent(FString(), nullptr, Delta));
	TestFalse(TEXT("[DONE] alone does not complete a stream"), State->BuildResponseJson()->GetBoolField(TEXT("stream_complete")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIChat_StreamError, "KismetAI.OpenAIChat.StreamInStreamError", OpenAIChatTestFlags)
bool FOpenAIChat_StreamError::RunTest(const FString& Parameters)
{
	{
		FChatHarness H;
		H.FeedAll(
			"data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Partial\"},\"finish_reason\":null}]}\n\n"
			"data: {\"id\":\"gen-2\",\"object\":\"chat.completion.chunk\",\"error\":{\"code\":502,\"message\":\"Upstream provider overloaded\"},\"choices\":[{\"index\":0,\"delta\":{\"content\":\"\"},\"finish_reason\":\"error\"}]}\n\n");
		FString Message, Code;
		TestTrue(TEXT("error chunk -> stream error"), H.State->HasStreamError(Message, Code));
		TestEqual(TEXT("message"), Message, FString(TEXT("Upstream provider overloaded")));
		TestEqual(TEXT("numeric code as string"), Code, FString(TEXT("502")));
	}
	{
		FChatHarness H;
		H.FeedAll("data: {\"error\":{\"message\":\"Rate limit\",\"type\":\"rate_limit_error\",\"code\":\"rate_limit_exceeded\"}}\n\n");
		FString Message, Code;
		TestTrue(TEXT("bare error chunk"), H.State->HasStreamError(Message, Code));
		TestEqual(TEXT("string code"), Code, FString(TEXT("rate_limit_exceeded")));
	}
	{
		FChatHarness H;
		H.FeedAll("data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"error\"}]}\n\n");
		FString Message, Code;
		TestTrue(TEXT("finish_reason error alone is an error"), H.State->HasStreamError(Message, Code));
		TestFalse(TEXT("with a message"), Message.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIChat_StreamMalformed, "KismetAI.OpenAIChat.StreamMalformedToolJson", OpenAIChatTestFlags)
bool FOpenAIChat_StreamMalformed::RunTest(const FString& Parameters)
{
	{
		FChatHarness H;
		H.FeedAll(
			"data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"ok\",\"function\":{\"name\":\"compile_blueprint\",\"arguments\":\"{}\"}}]},\"finish_reason\":null}]}\n\n"
			"data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":1,\"id\":\"bad\",\"function\":{\"name\":\"enter_plan_mode\",\"arguments\":\"{\\\"unfinished\\\":\"}}]},\"finish_reason\":null}]}\n\n"
			"data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n");
		FString Message, Code;
		TestTrue(TEXT("malformed arguments reject the whole turn"), H.State->HasStreamError(Message, Code));
		TestEqual(TEXT("same code as the Responses provider"), Code, FString(TEXT("malformed_tool_arguments")));
		TestFalse(TEXT("explains recovery"), Message.IsEmpty());
		for (const TSharedPtr<FJsonValue>& Block : H.Content())
		{
			TestNotEqual(TEXT("no tool_use survives"), Block->AsObject()->GetStringField(TEXT("type")), FString(TEXT("tool_use")));
		}
	}
	{
		// Empty arguments mean "no arguments" for a zero-parameter tool.
		FChatHarness H;
		H.FeedAll(
			"data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"z\",\"function\":{\"name\":\"exit_plan_mode\",\"arguments\":\"\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n");
		FString Message, Code;
		TestFalse(TEXT("empty arguments are valid"), H.State->HasStreamError(Message, Code));
		const TArray<TSharedPtr<FJsonValue>> Content = H.Content();
		TestTrue(TEXT("tool present with {} input"), Content.Num() == 1 && ObjectAt(Content, 0)->GetObjectField(TEXT("input"))->Values.Num() == 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIChat_StreamCutOff, "KismetAI.OpenAIChat.StreamCutOffBeforeFinishReason", OpenAIChatTestFlags)
bool FOpenAIChat_StreamCutOff::RunTest(const FString& Parameters)
{
	FChatHarness H;
	H.FeedAll(
		"data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Let me \"},\"finish_reason\":null}]}\n\n"
		"data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c\",\"function\":{\"name\":\"read_blueprint\",\"arguments\":\"{\\\"blueprint_name\\\":\\\"BP\\\"}\"}}]},\"finish_reason\":null}]}\n\n"
		"data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"partial");   // connection dropped mid-event

	TestFalse(TEXT("not complete -> the client retries as incomplete_stream"), H.StreamComplete());
	TestFalse(TEXT("usage not complete"), H.Response()->GetBoolField(TEXT("usage_complete")));
	for (const TSharedPtr<FJsonValue>& Block : H.Content())
	{
		TestNotEqual(TEXT("a cut-off stream never yields a tool call"), Block->AsObject()->GetStringField(TEXT("type")), FString(TEXT("tool_use")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenAIChat_StreamStopWithTools, "KismetAI.OpenAIChat.StreamStopWithToolCallsIsToolUse", OpenAIChatTestFlags)
bool FOpenAIChat_StreamStopWithTools::RunTest(const FString& Parameters)
{
	// Some hosts (llama.cpp builds, Gemini) report "stop" even when they called a
	// tool, and some omit the tool-call index and id.
	FChatHarness H;
	H.FeedAll(
		"data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"function\":{\"name\":\"compile_blueprint\",\"arguments\":\"{\\\"blueprint_name\\\":\\\"BP\\\"}\"}}]},\"finish_reason\":\"stop\"}]}\n\n");
	TestEqual(TEXT("stop + tool call -> tool_use"), H.StopReason(), FString(TEXT("tool_use")));
	const TArray<TSharedPtr<FJsonValue>> Content = H.Content();
	if (!TestEqual(TEXT("one tool"), Content.Num(), 1)) { return false; }
	TestFalse(TEXT("an id is synthesised when the host omits it"), ObjectAt(Content, 0)->GetStringField(TEXT("id")).IsEmpty());
	return true;
}
