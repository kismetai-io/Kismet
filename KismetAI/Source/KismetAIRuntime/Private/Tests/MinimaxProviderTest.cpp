// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIAgent.h"
#include "KismetAILLMProvider.h"
#include "KismetAIMinimaxProvider.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

constexpr auto MinimaxProviderTestFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

static FKismetAIAgentConfig MakeMinimaxConfig()
{
	FKismetAIAgentConfig Cfg;
	Cfg.Model = TEXT("MiniMax-M3");
	Cfg.ApiKey = TEXT("sk-anthropic-should-not-be-used");
	Cfg.OpenAIApiKey = TEXT("sk-openai-should-not-be-used");
	Cfg.MinimaxApiKey = TEXT("sk-minimax-test");
	Cfg.MaxTokens = 4096;
	Cfg.Temperature = 1.0f;
	Cfg.ThinkingMode = EThinkingMode::Adaptive;
	Cfg.EffortLevel = EEffortLevel::Medium;
	return Cfg;
}

static TSharedPtr<FJsonValue> MakeMinimaxDummyTool(const FString& ToolName)
{
	TSharedPtr<FJsonObject> Tool = MakeShared<FJsonObject>();
	Tool->SetStringField(TEXT("name"), ToolName);
	Tool->SetStringField(TEXT("description"), TEXT("A test tool"));
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));
	Tool->SetObjectField(TEXT("input_schema"), Schema);
	return MakeShared<FJsonValueObject>(Tool);
}

static TSharedPtr<FJsonValue> MakeMinimaxDummyMessage(const FString& Role, const FString& Text)
{
	TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("role"), Role);
	Msg->SetStringField(TEXT("content"), Text);
	return MakeShared<FJsonValueObject>(Msg);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMinimaxProvider_BodyShape_TopLevelFields,
	"KismetAI.Provider.MinimaxBodyShape.TopLevelFields",
	MinimaxProviderTestFlags)
bool FMinimaxProvider_BodyShape_TopLevelFields::RunTest(const FString& Parameters)
{
	FMinimaxProvider Provider;
	FKismetAIAgentConfig Cfg = MakeMinimaxConfig();

	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(MakeMinimaxDummyMessage(TEXT("user"), TEXT("Hello")));

	TArray<TSharedPtr<FJsonValue>> Tools;
	Tools.Add(MakeMinimaxDummyTool(TEXT("read_blueprint")));

	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(
		Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("System prompt."), Shape);

	TestTrue(TEXT("Body is valid"), Body.IsValid());
	TestEqual(TEXT("model == MiniMax-M3"), Body->GetStringField(TEXT("model")), FString(TEXT("MiniMax-M3")));
	TestEqual(TEXT("max_tokens == config max_tokens"), (int32)Body->GetNumberField(TEXT("max_tokens")), Cfg.MaxTokens);
	TestTrue(TEXT("stream == true"), Body->GetBoolField(TEXT("stream")));
	TestEqual(TEXT("system is plain string"), Body->GetStringField(TEXT("system")), FString(TEXT("System prompt.")));
	const TArray<TSharedPtr<FJsonValue>>* ToolsArray = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* MessagesArray = nullptr;
	TestTrue(TEXT("tools field present"), Body->TryGetArrayField(TEXT("tools"), ToolsArray));
	TestTrue(TEXT("messages field present"), Body->TryGetArrayField(TEXT("messages"), MessagesArray));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMinimaxProvider_BodyShape_M3AdaptiveThinking,
	"KismetAI.Provider.MinimaxBodyShape.M3AdaptiveThinking",
	MinimaxProviderTestFlags)
bool FMinimaxProvider_BodyShape_M3AdaptiveThinking::RunTest(const FString& Parameters)
{
	FMinimaxProvider Provider;
	FKismetAIAgentConfig Cfg = MakeMinimaxConfig();

	TArray<TSharedPtr<FJsonValue>> Messages;
	TArray<TSharedPtr<FJsonValue>> Tools;
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(
		Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("System prompt."), Shape);

	const TSharedPtr<FJsonObject>* Thinking;
	TestTrue(TEXT("thinking object is present for MiniMax-M3 adaptive mode"),
		Body->TryGetObjectField(TEXT("thinking"), Thinking));
	TestEqual(TEXT("thinking.type == adaptive"),
		(*Thinking)->GetStringField(TEXT("type")), FString(TEXT("adaptive")));
	TestFalse(TEXT("MiniMax-M3 thinking shape does not include Claude display field"),
		(*Thinking)->HasField(TEXT("display")));
	TestTrue(TEXT("Shape marks thinking enabled"), Shape.bThinkingEnabled);
	TestFalse(TEXT("MiniMax does not emit Claude effort output_config"), Shape.bEffortEnabled);
	TestFalse(TEXT("output_config omitted"), Body->HasField(TEXT("output_config")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMinimaxProvider_BodyShape_M2OmitsThinking,
	"KismetAI.Provider.MinimaxBodyShape.M2OmitsThinking",
	MinimaxProviderTestFlags)
bool FMinimaxProvider_BodyShape_M2OmitsThinking::RunTest(const FString& Parameters)
{
	FMinimaxProvider Provider;
	FKismetAIAgentConfig Cfg = MakeMinimaxConfig();
	Cfg.Model = TEXT("MiniMax-M2.7");

	TArray<TSharedPtr<FJsonValue>> Messages;
	TArray<TSharedPtr<FJsonValue>> Tools;
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(
		Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("System prompt."), Shape);

	TestFalse(TEXT("MiniMax M2.x request omits Anthropic legacy thinking field"),
		Body->HasField(TEXT("thinking")));
	TestFalse(TEXT("Shape marks thinking disabled"), Shape.bThinkingEnabled);
	TestTrue(TEXT("temperature remains available for MiniMax M2.x"),
		Body->HasField(TEXT("temperature")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMinimaxProvider_BodyShape_M27FastModeUsesHighspeed,
	"KismetAI.Provider.MinimaxBodyShape.M27FastModeUsesHighspeed",
	MinimaxProviderTestFlags)
bool FMinimaxProvider_BodyShape_M27FastModeUsesHighspeed::RunTest(const FString& Parameters)
{
	FMinimaxProvider Provider;
	FKismetAIAgentConfig Cfg = MakeMinimaxConfig();
	Cfg.Model = TEXT("MiniMax-M2.7");
	Cfg.bFastMode = true;

	TArray<TSharedPtr<FJsonValue>> Messages;
	TArray<TSharedPtr<FJsonValue>> Tools;
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(
		Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("System prompt."), Shape);

	TestEqual(TEXT("MiniMax-M2.7 fast mode sends highspeed model id"),
		Body->GetStringField(TEXT("model")), FString(TEXT("MiniMax-M2.7-highspeed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMinimaxProvider_EndpointUrl_Default,
	"KismetAI.Provider.MinimaxBodyShape.EndpointUrlDefault",
	MinimaxProviderTestFlags)
bool FMinimaxProvider_EndpointUrl_Default::RunTest(const FString& Parameters)
{
	FMinimaxProvider Provider;
	FKismetAIAgentConfig Cfg = MakeMinimaxConfig();

	const FString OldBaseUrl = FPlatformMisc::GetEnvironmentVariable(TEXT("MINIMAX_BASE_URL"));
	FPlatformMisc::SetEnvironmentVar(TEXT("MINIMAX_BASE_URL"), TEXT(""));
	FString Url = Provider.GetEndpointUrl(Cfg);
	FPlatformMisc::SetEnvironmentVar(TEXT("MINIMAX_BASE_URL"), OldBaseUrl.IsEmpty() ? TEXT("") : *OldBaseUrl);

	TestEqual(TEXT("Default MiniMax endpoint"),
		Url, FString(TEXT("https://api.minimax.io/anthropic/v1/messages")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMinimaxProvider_Headers_Auth,
	"KismetAI.Provider.MinimaxBodyShape.ExtraCurlHeadersAuth",
	MinimaxProviderTestFlags)
bool FMinimaxProvider_Headers_Auth::RunTest(const FString& Parameters)
{
	FMinimaxProvider Provider;
	FKismetAIAgentConfig Cfg = MakeMinimaxConfig();

	FLLMRequestShape Shape;
	TArray<FString> Headers = Provider.GetExtraCurlHeaders(Cfg, Shape);

	bool bHasAuth = false;
	bool bHasAnthropicKey = false;
	bool bHasOpenAIKey = false;
	bool bHasAnthropicVersion = false;
	for (const FString& H : Headers)
	{
		if (H.Contains(TEXT("Authorization: Bearer sk-minimax-test")))
		{
			bHasAuth = true;
		}
		bHasAnthropicKey |= H.Contains(TEXT("sk-anthropic-should-not-be-used")) || H.Contains(TEXT("x-api-key"));
		bHasOpenAIKey |= H.Contains(TEXT("sk-openai-should-not-be-used"));
		bHasAnthropicVersion |= H.Contains(TEXT("anthropic-version"));
	}

	TestTrue(TEXT("Authorization Bearer header uses MiniMax key"), bHasAuth);
	TestFalse(TEXT("Anthropic key/header not used"), bHasAnthropicKey);
	TestFalse(TEXT("OpenAI key not used"), bHasOpenAIKey);
	TestFalse(TEXT("Anthropic version header not emitted"), bHasAnthropicVersion);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMinimaxProvider_Factory_RoutesToMinimax,
	"KismetAI.Provider.MinimaxBodyShape.FactoryRoutesToMinimax",
	MinimaxProviderTestFlags)
bool FMinimaxProvider_Factory_RoutesToMinimax::RunTest(const FString& Parameters)
{
	TUniquePtr<ILLMProvider> Provider = MakeProviderForModel(TEXT("MiniMax-M3"));
	if (!TestTrue(TEXT("Provider is non-null"), Provider.IsValid())) return false;
	TestEqual(TEXT("MiniMax-M3 routes to minimax provider"),
		Provider->GetName(), FString(TEXT("minimax")));
	return true;
}
