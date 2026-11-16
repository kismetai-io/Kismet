// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIProviderProfile.h"
#include "KismetAIRunSetup.h"
#include "KismetAIModelCatalog.h"
#include "KismetAIAgent.h"
#include "KismetAIKeyStore.h"
#include "KismetAILLMProvider.h"
#include "KismetAIAnthropicProvider.h"
#include "KismetAIOpenAIProvider.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

constexpr auto ProviderProfileTestFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	const FKismetAIProviderProfile& RequireProfile(FAutomationTestBase& Test, const TCHAR* Id)
	{
		const FKismetAIProviderProfile* Profile = FKismetAIProviderRegistry::Get().Find(Id);
		Test.TestNotNull(FString::Printf(TEXT("profile %s exists"), Id), Profile);
		static const FKismetAIProviderProfile Empty;
		return Profile ? *Profile : Empty;
	}

	FKismetAIAgentConfig ConfigFor(const FString& Model, const FString& ProviderId = FString())
	{
		FKismetAIAgentConfig Cfg;
		Cfg.Model = Model;
		Cfg.ProviderId = ProviderId;
		Cfg.ApiKey = TEXT("sk-ant");
		Cfg.OpenAIApiKey = TEXT("sk-oai");
		Cfg.MinimaxApiKey = TEXT("sk-mmx");
		Cfg.ProviderKeys.Add(TEXT("deepseek"), TEXT("sk-ds"));
		Cfg.ProviderKeys.Add(TEXT("openrouter"), TEXT("sk-or"));
		return Cfg;
	}

	TSharedPtr<FJsonObject> BodyFor(const ILLMProvider& Provider, const FKismetAIAgentConfig& Cfg)
	{
		TArray<TSharedPtr<FJsonValue>> Messages;
		TSharedPtr<FJsonObject> User = MakeShared<FJsonObject>();
		User->SetStringField(TEXT("role"), TEXT("user"));
		User->SetStringField(TEXT("content"), TEXT("hi"));
		Messages.Add(MakeShared<FJsonValueObject>(User));
		TArray<TSharedPtr<FJsonValue>> Tools;
		FLLMRequestShape Shape;
		return Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), Tools, TEXT("system prompt"), Shape);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderProfile_ShippedProfilesLoad,
	"KismetAI.ProviderProfile.ShippedProfilesLoad", ProviderProfileTestFlags)
bool FProviderProfile_ShippedProfilesLoad::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Config/providers.json loads and validates"), FKismetAIProviderRegistry::Get().IsLoaded());
	for (const TCHAR* Id : { TEXT("anthropic"), TEXT("openai"), TEXT("google"), TEXT("openrouter"), TEXT("deepseek"),
		TEXT("zai"), TEXT("moonshot"), TEXT("minimax"), TEXT("ollama"), TEXT("lmstudio"), TEXT("custom-anthropic"),
		TEXT("custom-openai") })
	{
		RequireProfile(*this, Id);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderProfile_EveryCatalogProviderExists,
	"KismetAI.ProviderProfile.EveryCatalogProviderExists", ProviderProfileTestFlags)
bool FProviderProfile_EveryCatalogProviderExists::RunTest(const FString& Parameters)
{
	// A catalog row naming a profile that does not exist would silently fall back
	// to Anthropic with the wrong key — catch it here instead.
	for (const FKismetAIModelEntry& Entry : FKismetAIModelCatalog::Get().GetEntries())
	{
		if (!Entry.Provider.IsEmpty())
		{
			TestNotNull(FString::Printf(TEXT("%s: provider '%s' is a profile"), *Entry.Id, *Entry.Provider),
				FKismetAIProviderRegistry::Get().Find(Entry.Provider));
		}
	}
	for (const FKismetAIModelEntry* Entry : FKismetAIModelCatalog::Get().GetPickerModels())
	{
		TestEqual(FString::Printf(TEXT("%s routes to its catalog provider"), *Entry->Id),
			KismetAIProviders::ProviderIdForModel(Entry->Id), Entry->Provider);
		const FKismetAIProviderProfile* Profile = FKismetAIProviderRegistry::Get().Find(Entry->Provider);
		TestNotNull(FString::Printf(TEXT("%s's profile exists"), *Entry->Id), Profile);
		TestTrue(FString::Printf(TEXT("%s has a provider implementation"), *Entry->Id),
			MakeProviderForModel(Entry->Id).IsValid());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderProfile_Routing,
	"KismetAI.ProviderProfile.Routing", ProviderProfileTestFlags)
bool FProviderProfile_Routing::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Opus 5.5 -> anthropic"), KismetAIProviders::ProviderIdForModel(TEXT("claude-opus-5-5")), FString(TEXT("anthropic")));
	TestEqual(TEXT("GPT-6.1 Sol -> openai"), KismetAIProviders::ProviderIdForModel(TEXT("gpt-6.1-sol")), FString(TEXT("openai")));
	TestEqual(TEXT("MiniMax -> minimax"), KismetAIProviders::ProviderIdForModel(TEXT("MiniMax-M3")), FString(TEXT("minimax")));
	TestEqual(TEXT("GLM -> zai"), KismetAIProviders::ProviderIdForModel(TEXT("glm-5.3")), FString(TEXT("zai")));
	TestEqual(TEXT("DeepSeek -> deepseek"), KismetAIProviders::ProviderIdForModel(TEXT("deepseek-v4-pro")), FString(TEXT("deepseek")));

	// Saved ids that predate the catalog still go where they always went.
	TestEqual(TEXT("unknown gpt- id -> openai"), KismetAIProviders::ProviderIdForModel(TEXT("gpt-9-preview")), FString(TEXT("openai")));
	TestEqual(TEXT("unknown id -> anthropic"), KismetAIProviders::ProviderIdForModel(TEXT("something-else")), FString(TEXT("anthropic")));

	// An explicit profile wins over the catalog.
	TestEqual(TEXT("explicit profile wins"),
		KismetAIProviders::ResolveProviderId(ConfigFor(TEXT("claude-opus-5-5"), TEXT("openrouter"))), FString(TEXT("openrouter")));
	TestEqual(TEXT("legacy helpers agree: IsOpenAIModel(gpt-6-astra)"), IsOpenAIModel(TEXT("gpt-6-astra")), true);
	TestEqual(TEXT("legacy helpers agree: IsOpenAIModel(claude-opus-5-5)"), IsOpenAIModel(TEXT("claude-opus-5-5")), false);
	TestEqual(TEXT("legacy helpers agree: IsMinimaxModel(MiniMax-M3.1-Flash)"), IsMinimaxModel(TEXT("MiniMax-M3.1-Flash")), true);
	// Aggregator ids carry a vendor prefix; capabilities still come from the model.
	TestTrue(TEXT("openai/gpt-6.1-sol is a reasoning model"),
		FKismetAIModelCatalog::Get().FindOrDefaults(TEXT("openai/gpt-6.1-sol")).Caps.bReasoning);
	TestEqual(TEXT("z-ai/glm-5.3 has GLM's context"),
		FKismetAIModelCatalog::Get().FindOrDefaults(TEXT("z-ai/glm-5.3")).ContextWindow, 1048576);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderProfile_KeysNeverCrossProfiles,
	"KismetAI.ProviderProfile.KeysNeverCrossProfiles", ProviderProfileTestFlags)
bool FProviderProfile_KeysNeverCrossProfiles::RunTest(const FString& Parameters)
{
	const FKismetAIAgentConfig Cfg = ConfigFor(TEXT("claude-opus-5-5"));
	TestEqual(TEXT("anthropic slot"), KismetAIProviders::GetApiKey(Cfg, RequireProfile(*this, TEXT("anthropic"))), FString(TEXT("sk-ant")));
	TestEqual(TEXT("openai slot"), KismetAIProviders::GetApiKey(Cfg, RequireProfile(*this, TEXT("openai"))), FString(TEXT("sk-oai")));
	TestEqual(TEXT("minimax slot"), KismetAIProviders::GetApiKey(Cfg, RequireProfile(*this, TEXT("minimax"))), FString(TEXT("sk-mmx")));
	TestEqual(TEXT("deepseek slot"), KismetAIProviders::GetApiKey(Cfg, RequireProfile(*this, TEXT("deepseek"))), FString(TEXT("sk-ds")));
	TestEqual(TEXT("zai has no key: empty, not another provider's"),
		KismetAIProviders::GetApiKey(Cfg, RequireProfile(*this, TEXT("zai"))), FString());
	TestFalse(TEXT("zai without a key is not usable"), KismetAIProviders::HasUsableKey(Cfg, RequireProfile(*this, TEXT("zai"))));
	TestTrue(TEXT("ollama needs no key"), KismetAIProviders::HasUsableKey(Cfg, RequireProfile(*this, TEXT("ollama"))));

	FKismetAIAgentConfig Mutable = Cfg;
	KismetAIProviders::SetApiKey(Mutable, TEXT("zai"), TEXT("sk-z"));
	TestEqual(TEXT("SetApiKey round trip"), KismetAIProviders::GetApiKey(Mutable, RequireProfile(*this, TEXT("zai"))), FString(TEXT("sk-z")));
	TestEqual(TEXT("SetApiKey leaves other slots alone"), Mutable.ApiKey, FString(TEXT("sk-ant")));
	KismetAIProviders::SetApiKey(Mutable, TEXT("anthropic"), TEXT("sk-ant-2"));
	TestEqual(TEXT("historical slot writes its field"), Mutable.ApiKey, FString(TEXT("sk-ant-2")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderProfile_Endpoints,
	"KismetAI.ProviderProfile.Endpoints", ProviderProfileTestFlags)
bool FProviderProfile_Endpoints::RunTest(const FString& Parameters)
{
	{
		FKismetAIAgentConfig Cfg = ConfigFor(TEXT("claude-opus-5-5"));
		TestEqual(TEXT("anthropic default"), MakeProviderForConfig(Cfg)->GetEndpointUrl(Cfg), FString(TEXT("https://api.anthropic.com/v1/messages")));
		Cfg.ApiBaseUrl = TEXT("http://127.0.0.1:9999/");
		TestEqual(TEXT("legacy ApiBaseUrl override still works"), MakeProviderForConfig(Cfg)->GetEndpointUrl(Cfg), FString(TEXT("http://127.0.0.1:9999/v1/messages")));
		Cfg.ProviderBaseUrls.Add(TEXT("anthropic"), TEXT("https://gateway.example/"));
		TestEqual(TEXT("settings override wins, trailing slash trimmed"), MakeProviderForConfig(Cfg)->GetEndpointUrl(Cfg), FString(TEXT("https://gateway.example/v1/messages")));
	}
	{
		const FKismetAIAgentConfig Cfg = ConfigFor(TEXT("deepseek-v4-pro"));
		TestEqual(TEXT("deepseek endpoint"), MakeProviderForConfig(Cfg)->GetEndpointUrl(Cfg), FString(TEXT("https://api.deepseek.com/anthropic/v1/messages")));
	}
	{
		const FKismetAIAgentConfig Cfg = ConfigFor(TEXT("qwen3.8:27b"), TEXT("ollama"));
		TestEqual(TEXT("ollama endpoint"), MakeProviderForConfig(Cfg)->GetEndpointUrl(Cfg), FString(TEXT("http://localhost:11434/v1/messages")));
	}
	{
		const FKismetAIAgentConfig Cfg = ConfigFor(TEXT("gemini-4-argon"));
		TestEqual(TEXT("gemini endpoint (Chat Completions)"), MakeProviderForConfig(Cfg)->GetEndpointUrl(Cfg),
			FString(TEXT("https://generativelanguage.googleapis.com/v1beta/openai/chat/completions")));
	}
	{
		const FKismetAIAgentConfig Cfg = ConfigFor(TEXT("gpt-6.1-sol"));
		TestEqual(TEXT("openai endpoint"), MakeProviderForConfig(Cfg)->GetEndpointUrl(Cfg), FString(TEXT("https://api.openai.com/v1/responses")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderProfile_AuthHeaders,
	"KismetAI.ProviderProfile.AuthHeaders", ProviderProfileTestFlags)
bool FProviderProfile_AuthHeaders::RunTest(const FString& Parameters)
{
	const FKismetAIAgentConfig Cfg = ConfigFor(TEXT("claude-opus-5-5"));
	const FString Anthropic = FString::Join(KismetAIProviders::BuildAuthHeaders(Cfg, RequireProfile(*this, TEXT("anthropic"))), TEXT("\n"));
	TestTrue(TEXT("anthropic x-api-key"), Anthropic.Contains(TEXT("x-api-key: sk-ant")));
	TestFalse(TEXT("anthropic sends no bearer"), Anthropic.Contains(TEXT("Authorization")));

	const FString DeepSeek = FString::Join(KismetAIProviders::BuildAuthHeaders(Cfg, RequireProfile(*this, TEXT("deepseek"))), TEXT("\n"));
	TestTrue(TEXT("deepseek x-api-key"), DeepSeek.Contains(TEXT("x-api-key: sk-ds")));
	TestTrue(TEXT("deepseek bearer"), DeepSeek.Contains(TEXT("Authorization: Bearer sk-ds")));
	TestFalse(TEXT("deepseek never sends the Anthropic key"), DeepSeek.Contains(TEXT("sk-ant")));

	TestEqual(TEXT("ollama sends no auth"), KismetAIProviders::BuildAuthHeaders(Cfg, RequireProfile(*this, TEXT("ollama"))).Num(), 0);

	const FString OpenRouter = FString::Join(KismetAIProviders::BuildAuthHeaders(Cfg, RequireProfile(*this, TEXT("openrouter"))), TEXT("\n"));
	TestTrue(TEXT("openrouter bearer"), OpenRouter.Contains(TEXT("Authorization: Bearer sk-or")));
	TestTrue(TEXT("openrouter extra header"), OpenRouter.Contains(TEXT("X-Title: KismetAI")));

	const FString MiniMax = FString::Join(KismetAIProviders::BuildAuthHeaders(Cfg, RequireProfile(*this, TEXT("minimax"))), TEXT("\n"));
	TestTrue(TEXT("minimax bearer (unchanged)"), MiniMax.Contains(TEXT("Authorization: Bearer sk-mmx")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderProfile_BodyFollowsProfile,
	"KismetAI.ProviderProfile.BodyFollowsProfile", ProviderProfileTestFlags)
bool FProviderProfile_BodyFollowsProfile::RunTest(const FString& Parameters)
{
	{
		const FKismetAIAgentConfig Cfg = ConfigFor(TEXT("claude-opus-5-5"));
		TSharedPtr<FJsonObject> Body = BodyFor(*MakeProviderForConfig(Cfg), Cfg);
		const TArray<TSharedPtr<FJsonValue>>* System = nullptr;
		TestTrue(TEXT("anthropic: system is a block array"), Body->TryGetArrayField(TEXT("system"), System) && System && System->Num() == 1);
		if (System && System->Num() == 1)
		{
			TestTrue(TEXT("anthropic: system carries cache_control"), (*System)[0]->AsObject()->HasField(TEXT("cache_control")));
		}
	}
	{
		const FKismetAIAgentConfig Cfg = ConfigFor(TEXT("qwen3.8:27b"), TEXT("ollama"));
		TSharedPtr<FJsonObject> Body = BodyFor(*MakeProviderForConfig(Cfg), Cfg);
		FString System;
		TestTrue(TEXT("ollama: system is a string"), Body->TryGetStringField(TEXT("system"), System));
		TestEqual(TEXT("ollama: model sent as typed"), Body->GetStringField(TEXT("model")), FString(TEXT("qwen3.8:27b")));
		// An unknown local model must not receive optional thinking, effort
		// or Claude beta headers without capability metadata.
		TestFalse(TEXT("ollama: unknown model gets no thinking field"), Body->HasField(TEXT("thinking")));
		TestFalse(TEXT("ollama: unknown model gets no effort"), Body->HasField(TEXT("output_config")));
		FLLMRequestShape Shape;
		TUniquePtr<ILLMProvider> Provider = MakeProviderForConfig(Cfg);
		TArray<TSharedPtr<FJsonValue>> NoTools;
		Provider->BuildRequestBody(Cfg, {}, NoTools, TEXT("s"), Shape);
		const FString Headers = FString::Join(Provider->GetExtraCurlHeaders(Cfg, Shape), TEXT("\n"));
		TestFalse(TEXT("ollama: no anthropic-beta header"), Headers.Contains(TEXT("anthropic-beta")));
	}
	{
		const FKismetAIAgentConfig Cfg = ConfigFor(TEXT("deepseek-v4-pro"));
		TSharedPtr<FJsonObject> Body = BodyFor(*MakeProviderForConfig(Cfg), Cfg);
		const TArray<TSharedPtr<FJsonValue>>* System = nullptr;
		if (Body->TryGetArrayField(TEXT("system"), System) && System && System->Num() == 1)
		{
			TestFalse(TEXT("deepseek: no Anthropic cache_control markers"), (*System)[0]->AsObject()->HasField(TEXT("cache_control")));
		}
		TestFalse(TEXT("deepseek: no thinking field (unconfirmed support)"), Body->HasField(TEXT("thinking")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderProfile_KeySlots,
	"KismetAI.ProviderProfile.KeySlots", ProviderProfileTestFlags)
bool FProviderProfile_KeySlots::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("deepseek is a valid slot"), KismetAIKeyStore::IsValidSlot(TEXT("deepseek")));
	TestTrue(TEXT("custom-openai is a valid slot"), KismetAIKeyStore::IsValidSlot(TEXT("custom-openai")));
	TestFalse(TEXT("path traversal is not a slot"), KismetAIKeyStore::IsValidSlot(TEXT("../x")));
	TestFalse(TEXT("uppercase is not a slot"), KismetAIKeyStore::IsValidSlot(TEXT("DeepSeek")));
	TestFalse(TEXT("empty is not a slot"), KismetAIKeyStore::IsValidSlot(FString()));
	TestEqual(TEXT("historical slot keeps its file"), KismetAIKeyStore::GetProviderKeyPath(TEXT("openai")), KismetAIKeyStore::GetOpenAIKeyPath());
	TestTrue(TEXT("new slot file name"), KismetAIKeyStore::GetProviderKeyPath(TEXT("deepseek")).EndsWith(TEXT("deepseek_key.enc")));
	TestEqual(TEXT("invalid slot has no path"), KismetAIKeyStore::GetProviderKeyPath(TEXT("../x")), FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderProfile_RegistryRejectsBadProfiles,
	"KismetAI.ProviderProfile.RegistryRejectsBadProfiles", ProviderProfileTestFlags)
bool FProviderProfile_RegistryRejectsBadProfiles::RunTest(const FString& Parameters)
{
	FString Error;
	{
		FKismetAIProviderRegistry Registry;
		TestFalse(TEXT("auth without a key slot"), Registry.LoadFromJson(TEXT(R"({"schema_version":1,"providers":[
			{"id":"a","protocol":"anthropic_messages","auth":"bearer"}]})"), Error));
	}
	{
		FKismetAIProviderRegistry Registry;
		TestFalse(TEXT("duplicate id"), Registry.LoadFromJson(TEXT(R"({"schema_version":1,"providers":[
			{"id":"a","protocol":"anthropic_messages","auth":"none"},
			{"id":"a","protocol":"openai_chat","auth":"none"}]})"), Error));
	}
	{
		FKismetAIProviderRegistry Registry;
		TestFalse(TEXT("unknown protocol"), Registry.LoadFromJson(TEXT(R"({"schema_version":1,"providers":[
			{"id":"a","protocol":"grpc","auth":"none"}]})"), Error));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderProfile_ExplicitSelectionRefused,
	"KismetAI.ProviderProfile.ExplicitSelectionRefused", ProviderProfileTestFlags)
bool FProviderProfile_ExplicitSelectionRefused::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Invalid = ConfigFor(TEXT("claude-opus-5-5"), TEXT("misspelled-provider"));
	FString Error;
	TestFalse(TEXT("unknown explicit provider is refused before admission"), KismetAIRunSetup::Prepare(Invalid, Error));
	TestTrue(TEXT("refusal identifies the selected provider"), Error.Contains(TEXT("misspelled-provider")));
	TestEqual(TEXT("refusal preserves selected identity"), Invalid.ProviderId, FString(TEXT("misspelled-provider")));
	FKismetAIAgentConfig Legacy = ConfigFor(TEXT("gpt-6.1-sol"));
	TestTrue(TEXT("absent provider preserves legacy derivation"), KismetAIRunSetup::Prepare(Legacy, Error));
	TestEqual(TEXT("legacy selected route remains OpenAI"), KismetAIProviders::ResolveProviderId(Legacy), FString(TEXT("openai")));
	FKismetAIAgentConfig Anthropic = ConfigFor(TEXT("claude-opus-5-5"), TEXT("anthropic"));
	TestTrue(TEXT("explicit Anthropic remains accepted"), KismetAIRunSetup::Prepare(Anthropic, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderProfile_MissingRegistrySelection,
	"KismetAI.ProviderProfile.MissingRegistrySelection", ProviderProfileTestFlags)
bool FProviderProfile_MissingRegistrySelection::RunTest(const FString& Parameters)
{
	const FKismetAIProviderRegistry MissingRegistry;
	FString Error;
	TestTrue(TEXT("explicit Anthropic has a built-in fallback"), KismetAIProviders::ValidateSelection(
		ConfigFor(TEXT("claude-opus-5-5"), TEXT("anthropic")), Error, MissingRegistry));
	TestTrue(TEXT("legacy derived Anthropic has a built-in fallback"), KismetAIProviders::ValidateSelection(
		ConfigFor(TEXT("some-model")), Error, MissingRegistry));
	TestFalse(TEXT("unavailable derived OpenAI never falls back to Anthropic"), KismetAIProviders::ValidateSelection(
		ConfigFor(TEXT("gpt-6.1-sol")), Error, MissingRegistry));
	TestFalse(TEXT("unknown explicit selection never falls back to Anthropic"), KismetAIProviders::ValidateSelection(
		ConfigFor(TEXT("claude-opus-5-5"), TEXT("missing")), Error, MissingRegistry));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderProfile_UnknownOptionalFields,
	"KismetAI.ProviderProfile.UnknownOptionalFieldsAcrossAdapters", ProviderProfileTestFlags)
bool FProviderProfile_UnknownOptionalFields::RunTest(const FString& Parameters)
{
	const FString UnknownModel = TEXT("p303-unlisted-capability-sentinel-zz9");
	TestNull(TEXT("sentinel has no exact or family catalog match"), FKismetAIModelCatalog::Get().Find(UnknownModel));
	const FKismetAIModelCapabilities& Caps = FKismetAIModelCatalog::Get().FindOrDefaults(UnknownModel).Caps;
	TestFalse(TEXT("unknown metadata does not promise sampling"), Caps.bSamplingParams);
	TestFalse(TEXT("unknown metadata does not promise thinking"), Caps.bThinking);
	TestFalse(TEXT("unknown metadata does not promise reasoning"), Caps.bReasoning);
	TestEqual(TEXT("unknown metadata does not promise effort levels"), Caps.EffortLevels.Num(), 0);
	for (const TCHAR* ProfileId : {TEXT("anthropic"), TEXT("openai"), TEXT("custom-openai"), TEXT("openrouter"), TEXT("minimax")})
	{
		FKismetAIAgentConfig Config = ConfigFor(UnknownModel, ProfileId);
		Config.ThinkingMode = EThinkingMode::Adaptive;
		Config.bFastMode = true;
		Config.EffortLevel = EEffortLevel::Max;
		TUniquePtr<ILLMProvider> Provider = MakeProviderForConfig(Config);
		if (!TestTrue(FString::Printf(TEXT("%s adapter exists"), ProfileId), Provider.IsValid())) continue;
		FLLMRequestShape Shape;
		const TSharedPtr<FJsonObject> Body = Provider->BuildRequestBody(Config, {}, {}, TEXT("fixed system"), Shape);
		if (!TestTrue(FString::Printf(TEXT("%s body exists"), ProfileId), Body.IsValid())) continue;
		for (const TCHAR* Field : {TEXT("temperature"), TEXT("top_p"), TEXT("top_k"), TEXT("thinking"), TEXT("reasoning"),
			TEXT("reasoning_effort"), TEXT("output_config"), TEXT("speed"), TEXT("service_tier")})
		{
			TestFalse(FString::Printf(TEXT("%s omits unsupported %s"), ProfileId, Field), Body->HasField(Field));
		}
		TestFalse(FString::Printf(TEXT("%s reports no thinking"), ProfileId), Shape.bThinkingEnabled);
		TestFalse(FString::Printf(TEXT("%s reports no effort"), ProfileId), Shape.bEffortEnabled);
		const FString Headers = FString::Join(Provider->GetExtraCurlHeaders(Config, Shape), TEXT("\n"));
		TestFalse(FString::Printf(TEXT("%s omits optional beta headers"), ProfileId), Headers.Contains(TEXT("beta"), ESearchCase::IgnoreCase));
		TestEqual(FString::Printf(TEXT("%s preserves selected model with fast preference"), ProfileId), Body->GetStringField(TEXT("model")), UnknownModel);
	}
	// Adapter contract only: this does not claim hosted Responses compatibility
	// for DeepSeek. Its catalogued sampling capability guards against blanket omission.
	const FString SamplingModel = TEXT("deepseek-v4-pro");
	const FKismetAIModelEntry* SamplingEntry = FKismetAIModelCatalog::Get().Find(SamplingModel);
	if (TestNotNull(TEXT("positive sampling model is catalogued"), SamplingEntry))
	{
		TestFalse(TEXT("positive sampling model is not a reasoning model"), SamplingEntry->Caps.bReasoning);
		TestTrue(TEXT("positive sampling model declares sampling"), SamplingEntry->Caps.bSamplingParams);
		FKismetAIAgentConfig SamplingConfig = ConfigFor(SamplingModel, TEXT("openai"));
		SamplingConfig.Temperature = 0.375f;
		TUniquePtr<ILLMProvider> Responses = MakeProviderForConfig(SamplingConfig);
		if (TestTrue(TEXT("positive case constructs Responses adapter"), Responses.IsValid()))
		{
			FLLMRequestShape Shape;
			const TSharedPtr<FJsonObject> Body = Responses->BuildRequestBody(SamplingConfig, {}, {}, TEXT("fixed system"), Shape);
			double Temperature = -1;
			TestTrue(TEXT("Responses emits catalogued sampling"), Body->TryGetNumberField(TEXT("temperature"), Temperature));
			TestEqual(TEXT("Responses preserves configured temperature exactly"), Temperature, 0.375);
			TestFalse(TEXT("sampling does not enable effort"), Shape.bEffortEnabled);
			TestFalse(TEXT("sampling does not emit reasoning"), Body->HasField(TEXT("reasoning")));
		}
	}

	return true;
}
