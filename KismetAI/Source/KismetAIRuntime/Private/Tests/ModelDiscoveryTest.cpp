// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIModelDiscovery.h"
#include "KismetAIModelRegistry.h"
#include "KismetAIModelCatalog.h"
#include "KismetAIProviderProfile.h"
#include "KismetAIAgent.h"
#include "KismetAIAnthropicProvider.h"
#include "KismetAIOpenAIChatProvider.h"
#include "Fakes/FixtureTransport.h"
#include "Fixtures/ModelListFixtures.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"

constexpr auto ModelDiscoveryTestFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	using namespace KismetAIModelListFixtures;
	using KismetAIFixtureTransport::FFixtureHttpFetcher;

	const FKismetAIProviderProfile& Profile(const TCHAR* Id)
	{
		return *FKismetAIProviderRegistry::Get().Find(Id);
	}

	FString Joined(const TArray<FString>& Ids)
	{
		return FString::Join(Ids, TEXT(", "));
	}

	// Local profiles at their default addresses, whatever OLLAMA_BASE_URL /
	// LMSTUDIO_BASE_URL this machine has set.
	FKismetAIAgentConfig LocalConfig()
	{
		FKismetAIAgentConfig Config;
		Config.ProviderBaseUrls.Add(TEXT("ollama"), TEXT("http://localhost:11434"));
		Config.ProviderBaseUrls.Add(TEXT("lmstudio"), TEXT("http://localhost:1234"));
		Config.bProviderEnvironmentResolved = true;
		return Config;
	}

	FKismetAIAgentConfig KeyedConfig()
	{
		FKismetAIAgentConfig Config;
		Config.ApiKey = TEXT("sk-ant-test");
		Config.ProviderKeys.Add(TEXT("openrouter"), TEXT("sk-or-test"));
		Config.ProviderKeys.Add(TEXT("moonshot"), TEXT("sk-moon-test"));
		Config.ProviderKeys.Add(TEXT("google"), TEXT("g-test"));
		return Config;
	}

	// A temporary cache folder of its own, removed when the test ends.
	struct FScopedCacheDir
	{
		FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()
			/ (TEXT("KismetAITest_ModelCache_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		~FScopedCacheDir() { IFileManager::Get().DeleteDirectory(*Path, false, true); }
	};

	struct FTestWorld
	{
		FKismetAIModelCatalog Catalog;
		TSharedRef<FFixtureHttpFetcher> Fetcher = MakeShared<FFixtureHttpFetcher>();
		FScopedCacheDir Cache;
		TUniquePtr<FKismetAIModelRegistry> Registry;

		explicit FTestWorld(const FString& CatalogJson = TestCatalog())
		{
			FString Error;
			check(Catalog.LoadFromJson(CatalogJson, Error));
			Registry = MakeUnique<FKismetAIModelRegistry>(Catalog, FKismetAIProviderRegistry::Get(), Fetcher, Cache.Path);
		}

		TArray<FString> RowIds(const FString& Provider) const
		{
			TArray<FString> Ids;
			for (const FKismetAIModelEntry& Row : Registry->GetPickerRows())
			{
				if (Row.Provider == Provider) Ids.Add(Row.Id);
			}
			return Ids;
		}
	};
}

// ── Parsers ──────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDiscovery_ParsesOpenRouter,
	"KismetAI.ModelDiscovery.ParsesOpenRouter", ModelDiscoveryTestFlags)
bool FModelDiscovery_ParsesOpenRouter::RunTest(const FString& Parameters)
{
	TArray<FKismetAILiveModel> Models;
	FString Error;
	TestTrue(TEXT("OpenRouter list parses"), KismetAIModelsApi::Parse(EKismetAIModelsFormat::OpenRouter, OpenRouterList(), Models, Error));
	TestEqual(TEXT("four models"), Models.Num(), 4);
	const FKismetAILiveModel& Opus = Models[0];
	TestEqual(TEXT("vendor prefix stripped from the name"), Opus.DisplayName, FString(TEXT("Claude Opus 5.5")));
	TestEqual(TEXT("context"), Opus.ContextWindow, 1000000);
	TestEqual(TEXT("max output from top_provider"), Opus.MaxOutputTokens, 128000);
	TestTrue(TEXT("price is reported"), Opus.Price.IsSet());
	TestEqual(TEXT("prompt $/token -> $/MTok"), Opus.Price->Input, 4.0, 1e-9);
	TestEqual(TEXT("completion $/token -> $/MTok"), Opus.Price->Output, 20.0, 1e-9);
	TestEqual(TEXT("cache read"), Opus.Price->CacheRead, 0.2, 1e-9);
	TestTrue(TEXT("tools from supported_parameters"), Opus.bToolCalls.Get(false));
	TestTrue(TEXT("image from input_modalities"), Opus.bImageInput.Get(false));
	TestFalse(TEXT("Grok here lists no tools"), Models[1].bToolCalls.Get(true));
	TestFalse(TEXT("a variable-price router gets no price"), Models[3].Price.IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDiscovery_ParsesVendorLists,
	"KismetAI.ModelDiscovery.ParsesVendorLists", ModelDiscoveryTestFlags)
bool FModelDiscovery_ParsesVendorLists::RunTest(const FString& Parameters)
{
	TArray<FKismetAILiveModel> Models;
	FString Error;
	TestTrue(TEXT("Anthropic"), KismetAIModelsApi::Parse(EKismetAIModelsFormat::Anthropic, AnthropicList(), Models, Error));
	TestEqual(TEXT("Anthropic max_input_tokens"), Models[1].ContextWindow, 900000);
	TestEqual(TEXT("Anthropic max_tokens"), Models[1].MaxOutputTokens, 64000);
	TestTrue(TEXT("Anthropic created_at"), Models[0].Created > Models[1].Created);

	TestTrue(TEXT("Moonshot"), KismetAIModelsApi::Parse(EKismetAIModelsFormat::OpenAI, MoonshotList(), Models, Error));
	TestEqual(TEXT("Moonshot context_length"), Models[0].ContextWindow, 1048576);
	TestTrue(TEXT("Moonshot supports_image_in"), Models[0].bImageInput.Get(false));

	TestTrue(TEXT("Google"), KismetAIModelsApi::Parse(EKismetAIModelsFormat::Google, GoogleList(), Models, Error));
	TestEqual(TEXT("embedding models skipped"), Models.Num(), 1);
	TestEqual(TEXT("models/ prefix stripped"), Models[0].Id, FString(TEXT("gemini-4-argon")));
	TestEqual(TEXT("outputTokenLimit"), Models[0].MaxOutputTokens, 65536);

	TestFalse(TEXT("a truncated body fails"), KismetAIModelsApi::Parse(EKismetAIModelsFormat::Anthropic, Malformed(), Models, Error));
	TestFalse(TEXT("an error body fails"), KismetAIModelsApi::Parse(EKismetAIModelsFormat::OpenRouter, WrongShape(), Models, Error));
	TestEqual(TEXT("nothing kept from a failed parse"), Models.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDiscovery_FamilyKeys,
	"KismetAI.ModelDiscovery.FamilyKeys", ModelDiscoveryTestFlags)
bool FModelDiscovery_FamilyKeys::RunTest(const FString& Parameters)
{
	using namespace KismetAIModelsApi;
	TestEqual(TEXT("Claude point releases share a family"), FamilyKey(TEXT("claude-opus-5-6")), FamilyKey(TEXT("claude-opus-5-5")));
	TestFalse(TEXT("Opus and Sonnet differ"), FamilyKey(TEXT("claude-opus-5-5")) == FamilyKey(TEXT("claude-sonnet-5-5")));
	TestEqual(TEXT("vendor prefix kept"), FamilyKey(TEXT("anthropic/claude-opus-5.6")), FString(TEXT("anthropic/claude-opus")));
	TestEqual(TEXT("GPT names"), FamilyKey(TEXT("gpt-6.1-sol")), FString(TEXT("gpt-sol")));
	TestTrue(TEXT("dated snapshot"), IsSnapshotOrVariant(TEXT("claude-opus-5-5-20261101")));
	TestTrue(TEXT("OpenRouter variant"), IsSnapshotOrVariant(TEXT("x-ai/grok-4.7:batch")));
	TestFalse(TEXT("a plain release"), IsSnapshotOrVariant(TEXT("claude-opus-5-6")));
	return true;
}

// ── Merge (Eden's merge_catalog rules) ───────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDiscovery_MergeUpdatesAddsDrops,
	"KismetAI.ModelDiscovery.MergeUpdatesAddsDrops", ModelDiscoveryTestFlags)
bool FModelDiscovery_MergeUpdatesAddsDrops::RunTest(const FString& Parameters)
{
	FTestWorld World;
	World.Fetcher->On(TEXT("https://api.anthropic.com/v1/models?limit=1000"), 200, AnthropicList());
	World.Registry->RefreshProvider(KeyedConfig(), Profile(TEXT("anthropic")));

	const TArray<FString> Ids = World.RowIds(TEXT("anthropic"));
	TestEqual(TEXT("curated model kept first, newer family member added after it"), Joined(Ids),
		FString(TEXT("claude-opus-5-5, claude-opus-5-6")));
	TestFalse(TEXT("unlisted curated model dropped from the picker"), Ids.Contains(TEXT("claude-test-old")));
	TestFalse(TEXT("dated snapshot not added"), Ids.Contains(TEXT("claude-opus-5-5-20261101")));
	TestFalse(TEXT("older family member not added"), Ids.Contains(TEXT("claude-opus-4-8")));

	const FKismetAIModelEntry* Opus = World.Catalog.Find(TEXT("claude-opus-5-5"));
	TestTrue(TEXT("curated model resolves"), Opus != nullptr);
	if (Opus)
	{
		TestEqual(TEXT("keeps its curated name"), Opus->DisplayName, FString(TEXT("Opus 5.5")));
		TestEqual(TEXT("takes the live context window"), Opus->ContextWindow, 900000);
		TestEqual(TEXT("takes the live max output"), Opus->MaxOutputTokens, 64000);
		TestEqual(TEXT("keeps its curated price (Anthropic lists none)"), Opus->Price.Input, 4.0, 1e-9);
	}
	TestEqual(TEXT("dropped model still resolves"), World.Catalog.Resolve(TEXT("old")), FString(TEXT("claude-test-old")));
	const FKismetAIModelEntry* Added = World.Catalog.Find(TEXT("claude-opus-5-6"));
	TestTrue(TEXT("added model resolves"), Added != nullptr && Added->bRuntime);
	if (Added)
	{
		TestEqual(TEXT("added model uses the live name"), Added->DisplayName, FString(TEXT("Claude Opus 5.6")));
		TestEqual(TEXT("added model has its family's request shape"), Added->Caps.EffortLevels.Num(), 5);
		TestTrue(TEXT("added model has no fast tier"), Added->Caps.FastModeWire == EKismetAIFastModeWire::None);
	}
	TestTrue(TEXT("source says live"), World.Registry->GetSourceLabel(TEXT("anthropic")).StartsWith(TEXT("live, ")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDiscovery_OpenRouterPricesAndCapabilities,
	"KismetAI.ModelDiscovery.OpenRouterPricesAndCapabilities", ModelDiscoveryTestFlags)
bool FModelDiscovery_OpenRouterPricesAndCapabilities::RunTest(const FString& Parameters)
{
	FTestWorld World;
	World.Fetcher->On(TEXT("https://openrouter.ai/api/v1/models"), 200, OpenRouterList());
	World.Registry->RefreshProvider(KeyedConfig(), Profile(TEXT("openrouter")));

	TestEqual(TEXT("derived and OpenRouter-only rows kept; variants and routers not added"), Joined(World.RowIds(TEXT("openrouter"))), Joined({ TEXT("anthropic/claude-opus-5.5"), TEXT("x-ai/grok-4.7") }));
	const FKismetAIModelEntry* Grok = World.Catalog.Find(TEXT("x-ai/grok-4.7"));
	TestTrue(TEXT("Grok resolves"), Grok != nullptr);
	if (Grok)
	{
		TestFalse(TEXT("OpenRouter says no tools"), Grok->Caps.bToolCalls);
		TestFalse(TEXT("no reasoning parameter: effort is not sent"), Grok->Caps.bReasoning);
		TestEqual(TEXT("live max output"), Grok->MaxOutputTokens, 450000);
	}
	const FKismetAIModelEntry* Opus = World.Catalog.Find(TEXT("anthropic/claude-opus-5.5"));
	TestTrue(TEXT("derived row resolves"), Opus != nullptr);
	if (Opus)
	{
		TestEqual(TEXT("OpenRouter price replaces the static one"), Opus->Price.Output, 20.0, 1e-9);
		TestEqual(TEXT("curated cache write kept when OpenRouter lists none"), Opus->Price.CacheWrite, 5.0, 1e-9);
		TestEqual(TEXT("curated name kept"), Opus->DisplayName, FString(TEXT("Claude Opus 5.5")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDiscovery_FailuresKeepCurated,
	"KismetAI.ModelDiscovery.FailuresKeepCurated", ModelDiscoveryTestFlags)
bool FModelDiscovery_FailuresKeepCurated::RunTest(const FString& Parameters)
{
	const TArray<FString> Curated = { TEXT("claude-opus-5-5"), TEXT("claude-test-old") };
	{
		FTestWorld World;
		World.Fetcher->On(TEXT("https://api.anthropic.com/v1/models?limit=1000"), 200, Malformed());
		World.Registry->RefreshProvider(KeyedConfig(), Profile(TEXT("anthropic")));
		TestEqual(TEXT("malformed body: curated rows"), Joined(World.RowIds(TEXT("anthropic"))), Joined(Curated));
		TestEqual(TEXT("malformed body: source stays curated"), World.Registry->GetSourceLabel(TEXT("anthropic")), FString(TEXT("curated")));
		TestEqual(TEXT("malformed body: curated limits"), World.Catalog.Find(TEXT("claude-opus-5-5"))->ContextWindow, 1000000);
	}
	{
		FTestWorld World;
		World.Fetcher->On(TEXT("https://api.anthropic.com/v1/models?limit=1000"), 401, WrongShape());
		World.Registry->RefreshProvider(KeyedConfig(), Profile(TEXT("anthropic")));
		TestEqual(TEXT("HTTP error: curated rows"), Joined(World.RowIds(TEXT("anthropic"))), Joined(Curated));
	}
	{
		FTestWorld World;  // nothing scripted: the network is down
		World.Registry->RefreshProvider(KeyedConfig(), Profile(TEXT("anthropic")));
		TestEqual(TEXT("no answer: curated rows"), Joined(World.RowIds(TEXT("anthropic"))), Joined(Curated));
	}
	{
		FTestWorld World;
		World.Fetcher->On(TEXT("https://api.anthropic.com/v1/models?limit=1000"), 200, TEXT(R"({"data":[{"id":"some-proxy-model","created_at":"2026-11-20T00:00:00Z"}]})"));
		World.Registry->RefreshProvider(KeyedConfig(), Profile(TEXT("anthropic")));
		TestEqual(TEXT("a list naming no curated model is not trusted"), Joined(World.RowIds(TEXT("anthropic"))), Joined(Curated));
	}
	{
		FTestWorld World;
		FKismetAIAgentConfig NoKey;
		World.Registry->RefreshProvider(NoKey, Profile(TEXT("anthropic")));
		TestEqual(TEXT("no key: no request at all"), World.Fetcher->Requests.Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDiscovery_DiskCache,
	"KismetAI.ModelDiscovery.DiskCache", ModelDiscoveryTestFlags)
bool FModelDiscovery_DiskCache::RunTest(const FString& Parameters)
{
	FTestWorld World;
	const FString Url = TEXT("https://api.anthropic.com/v1/models?limit=1000");
	World.Fetcher->On(Url, 200, AnthropicList());
	World.Registry->RefreshProvider(KeyedConfig(), Profile(TEXT("anthropic")));
	World.Registry->RefreshProvider(KeyedConfig(), Profile(TEXT("anthropic")));
	TestEqual(TEXT("second refresh read the cache"), World.Fetcher->CountRequests(Url), 1);
	World.Registry->RefreshProvider(KeyedConfig(), Profile(TEXT("anthropic")), /*bUseCache=*/false);
	TestEqual(TEXT("a forced refresh refetches"), World.Fetcher->CountRequests(Url), 2);
	TestTrue(TEXT("the key went in a header, never the URL"), !World.Fetcher->Requests[0].Url.Contains(TEXT("sk-ant-test"))
		&& World.Fetcher->Requests[0].Headers.Contains(TEXT("x-api-key: sk-ant-test")));
	TestTrue(TEXT("anthropic-version header sent"), World.Fetcher->Requests[0].Headers.Contains(TEXT("anthropic-version: 2023-06-01")));
	return true;
}

// ── Local discovery ──────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDiscovery_Ollama,
	"KismetAI.ModelDiscovery.Ollama", ModelDiscoveryTestFlags)
bool FModelDiscovery_Ollama::RunTest(const FString& Parameters)
{
	FTestWorld World;
	World.Fetcher->On(TEXT("http://localhost:11434/api/tags"), 200, OllamaTags());
	World.Fetcher->OnShow(TEXT("http://localhost:11434/api/show"), TEXT("qwen3:8b"), OllamaShowQwen());
	World.Fetcher->OnShow(TEXT("http://localhost:11434/api/show"), TEXT("gemma2:2b"), OllamaShowGemma());
	FKismetAIAgentConfig Config = LocalConfig();
	World.Registry->RefreshProvider(Config, Profile(TEXT("ollama")));

	TestEqual(TEXT("installed models listed under Ollama"), Joined(World.RowIds(TEXT("ollama"))), Joined({ TEXT("qwen3:8b"), TEXT("gemma2:2b"), TEXT("mystery:latest") }));
	const FKismetAIModelEntry* Qwen = World.Catalog.Find(TEXT("qwen3:8b"));
	TestTrue(TEXT("discovered model resolves"), Qwen != nullptr);
	if (!Qwen) return false;
	TestEqual(TEXT("num_ctx wins over the trained maximum"), Qwen->ContextWindow, 40960);
	TestEqual(TEXT("context source"), Qwen->ContextSource, FString(TEXT("reported")));
	TestEqual(TEXT("price 0"), Qwen->Price.Input + Qwen->Price.Output, 0.0, 1e-12);
	TestTrue(TEXT("tools reported"), Qwen->Caps.bToolCalls);
	TestEqual(TEXT("routes to Ollama"), Qwen->Provider, FString(TEXT("ollama")));

	const FKismetAIModelEntry* Gemma = World.Catalog.Find(TEXT("gemma2:2b"));
	TestEqual(TEXT("model_info context"), Gemma->ContextWindow, 8192);
	TestFalse(TEXT("no tools capability"), Gemma->Caps.bToolCalls);
	const FKismetAIModelEntry* Mystery = World.Catalog.Find(TEXT("mystery:latest"));
	TestEqual(TEXT("no /api/show: conservative 32k"), Mystery->ContextWindow, KismetAIModelsApi::LocalDefaultContext);
	TestEqual(TEXT("marked assumed"), Mystery->ContextSource, FString(TEXT("assumed")));

	// The user sets the window the server really uses.
	Config.ModelContextWindows.Add(TEXT("mystery:latest"), 65536);
	World.Registry->ApplyUserContext(Config, TEXT("ollama"), TEXT("mystery:latest"));
	TestEqual(TEXT("user context applied"), World.Catalog.Find(TEXT("mystery:latest"))->ContextWindow, 65536);
	TestEqual(TEXT("marked user"), World.Catalog.Find(TEXT("mystery:latest"))->ContextSource, FString(TEXT("user")));

	// First use of a model without tools is refused with a clear message.
	FKismetAIAgentConfig Run = LocalConfig();
	Run.Model = TEXT("gemma2:2b");
	Run.ProviderId = TEXT("ollama");
	World.Fetcher->On(TEXT("http://localhost:11434/api/version"), 200, TEXT(R"({"version":"0.13.0"})"));
	FString Type;
	FString Message;
	TestFalse(TEXT("no-tools model refused before the run"),
		KismetAIModelsApi::PreflightLocal(Run, Profile(TEXT("ollama")), *World.Fetcher, Type, Message, World.Catalog));
	TestEqual(TEXT("error type"), Type, FString(TEXT("tools_unsupported")));
	TestTrue(TEXT("names the model"), Message.Contains(TEXT("gemma2:2b")) && Message.Contains(TEXT("tool calls")));
	Run.Model = TEXT("qwen3:8b");
	TestTrue(TEXT("tool model passes"),
		KismetAIModelsApi::PreflightLocal(Run, Profile(TEXT("ollama")), *World.Fetcher, Type, Message, World.Catalog));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDiscovery_LMStudio,
	"KismetAI.ModelDiscovery.LMStudio", ModelDiscoveryTestFlags)
bool FModelDiscovery_LMStudio::RunTest(const FString& Parameters)
{
	FTestWorld World;
	World.Fetcher->On(TEXT("http://localhost:1234/api/v1/models"), 200, LMStudioList());
	World.Registry->RefreshProvider(LocalConfig(), Profile(TEXT("lmstudio")));
	TestEqual(TEXT("chat models only"), Joined(World.RowIds(TEXT("lmstudio"))), Joined({ TEXT("qwen/qwen3-coder-30b"), TEXT("google/gemma-3-12b") }));
	TestEqual(TEXT("loaded instance context wins"), World.Catalog.Find(TEXT("qwen/qwen3-coder-30b"))->ContextWindow, 32768);
	TestEqual(TEXT("max context when not loaded"), World.Catalog.Find(TEXT("google/gemma-3-12b"))->ContextWindow, 131072);
	TestTrue(TEXT("trained_for_tool_use=false does not refuse tools"), World.Catalog.Find(TEXT("google/gemma-3-12b"))->Caps.bToolCalls);
	TestEqual(TEXT("display name"), World.Catalog.Find(TEXT("google/gemma-3-12b"))->DisplayName, FString(TEXT("Gemma 3 12B")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDiscovery_GenericOpenAIServer,
	"KismetAI.ModelDiscovery.GenericOpenAIServer", ModelDiscoveryTestFlags)
bool FModelDiscovery_GenericOpenAIServer::RunTest(const FString& Parameters)
{
	FTestWorld World;
	FKismetAIAgentConfig Config = LocalConfig();
	Config.ProviderBaseUrls.Add(TEXT("custom-openai"), TEXT("http://127.0.0.1:8080/v1"));
	World.Fetcher->On(TEXT("http://127.0.0.1:8080/v1/models"), 200, GenericOpenAIList());
	World.Registry->RefreshProvider(Config, Profile(TEXT("custom-openai")));
	TestEqual(TEXT("vLLM max_model_len"), World.Catalog.Find(TEXT("Qwen/Qwen3-32B"))->ContextWindow, 65536);
	TestEqual(TEXT("llama.cpp n_ctx_train"), World.Catalog.Find(TEXT("local-gguf"))->ContextWindow, 16384);
	TestEqual(TEXT("unreported context assumed"), World.Catalog.Find(TEXT("no-context-reported"))->ContextSource, FString(TEXT("assumed")));

	// A server without /v1/models keeps the typed id.
	FTestWorld Bare;
	Bare.Fetcher->On(TEXT("http://127.0.0.1:8080/v1/models"), 404, TEXT("not found"));
	Bare.Registry->RefreshProvider(Config, Profile(TEXT("custom-openai")));
	Bare.Registry->RegisterTypedModel(Config, TEXT("custom-openai"), TEXT("my-finetune"));
	const FKismetAIModelEntry* Typed = Bare.Catalog.Find(TEXT("my-finetune"));
	TestTrue(TEXT("typed id resolves"), Typed != nullptr);
	if (Typed)
	{
		TestEqual(TEXT("typed id gets the conservative window"), Typed->ContextWindow, KismetAIModelsApi::LocalDefaultContext);
		TestEqual(TEXT("typed id is free"), Typed->Price.Input, 0.0, 1e-12);
	}
	TestFalse(TEXT("typed id is not offered as a discovered row"), Bare.RowIds(TEXT("custom-openai")).Contains(TEXT("my-finetune")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDiscovery_ServerDown,
	"KismetAI.ModelDiscovery.ServerDown", ModelDiscoveryTestFlags)
bool FModelDiscovery_ServerDown::RunTest(const FString& Parameters)
{
	FTestWorld World;  // nothing answers at localhost
	World.Registry->RefreshProvider(LocalConfig(), Profile(TEXT("ollama")));
	TestEqual(TEXT("no rows"), World.RowIds(TEXT("ollama")).Num(), 0);
	TestEqual(TEXT("source says not running"), World.Registry->GetSourceLabel(TEXT("ollama")), FString(TEXT("not running")));

	FKismetAIAgentConfig Run = LocalConfig();
	Run.Model = TEXT("qwen3:8b");
	Run.ProviderId = TEXT("ollama");
	FString Type;
	FString Message;
	TestFalse(TEXT("health check fails"), KismetAIModelsApi::PreflightLocal(Run, Profile(TEXT("ollama")), *World.Fetcher, Type, Message, World.Catalog));
	TestEqual(TEXT("error type"), Type, FString(TEXT("local_server_unreachable")));
	TestTrue(TEXT("names Ollama and the URL"), Message.StartsWith(TEXT("Ollama not running at http://localhost:11434")));
	TestTrue(TEXT("fatal, not retried"), FKismetAIAgent::ClassifyCallOutcome(0, 0, Type, Message, false) == EModelCallResult::FatalError);

	Run.ProviderId = TEXT("lmstudio");
	KismetAIModelsApi::PreflightLocal(Run, Profile(TEXT("lmstudio")), *World.Fetcher, Type, Message, World.Catalog);
	TestTrue(TEXT("LM Studio message"), Message.StartsWith(TEXT("LM Studio server not running at http://localhost:1234")));

	// Cloud profiles never probe.
	Run.ProviderId = TEXT("anthropic");
	const int32 Before = World.Fetcher->Requests.Num();
	TestTrue(TEXT("cloud passes"), KismetAIModelsApi::PreflightLocal(Run, Profile(TEXT("anthropic")), *World.Fetcher, Type, Message, World.Catalog));
	TestEqual(TEXT("no probe for cloud"), World.Fetcher->Requests.Num(), Before);
	return true;
}

// ── Errors and wire shape ───────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDiscovery_ToolsUnsupportedErrorIsClear,
	"KismetAI.ModelDiscovery.ToolsUnsupportedErrorIsClear", ModelDiscoveryTestFlags)
bool FModelDiscovery_ToolsUnsupportedErrorIsClear::RunTest(const FString& Parameters)
{
	// Ollama's Anthropic-compatible route, the first time a tool-less model gets tools.
	FAnthropicProvider Provider(Profile(TEXT("ollama")));
	TSharedPtr<FJsonObject> Body;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(
		TEXT(R"({"type":"error","error":{"type":"invalid_request_error","message":"registry.ollama.ai/library/gemma2:2b does not support tools"}})")), Body);
	FModelCallOutcome Outcome;
	Provider.ParseErrorBody(Body, Outcome);
	TestEqual(TEXT("type"), Outcome.ErrorType, FString(TEXT("tools_unsupported")));
	TestTrue(TEXT("explains"), Outcome.ErrorMessage.Contains(TEXT("does not support tool calls")));
	TestTrue(TEXT("fatal"), FKismetAIAgent::ClassifyCallOutcome(0, 400, Outcome.ErrorType, Outcome.ErrorMessage, false) == EModelCallResult::FatalError);
	Outcome.Result = EModelCallResult::FatalError;
	TestFalse(TEXT("no API-key advice"), FKismetAIAgent::BuildUserFacingError(Outcome).Suggestions.Contains(TEXT("Check API key")));

	FOpenAIChatProvider Chat(Profile(TEXT("custom-openai")));
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(
		TEXT(R"({"error":{"message":"\"auto\" tool choice requires --enable-auto-tool-choice and --tool-call-parser to be set","type":"BadRequestError","code":400}})")), Body);
	FModelCallOutcome VllmOutcome;
	Chat.ParseErrorBody(Body, VllmOutcome);
	TestEqual(TEXT("vLLM without tool parser"), VllmOutcome.ErrorType, FString(TEXT("tools_unsupported")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDiscovery_OpenRouterClaudeCacheMarkers,
	"KismetAI.ModelDiscovery.OpenRouterClaudeCacheMarkers", ModelDiscoveryTestFlags)
bool FModelDiscovery_OpenRouterClaudeCacheMarkers::RunTest(const FString& Parameters)
{
	FOpenAIChatProvider Provider(Profile(TEXT("openrouter")));
	FKismetAIAgentConfig Config;
	Config.ProviderId = TEXT("openrouter");
	Config.ProviderKeys.Add(TEXT("openrouter"), TEXT("sk-or-test"));
	TArray<TSharedPtr<FJsonValue>> Messages;
	TSharedPtr<FJsonObject> User = MakeShared<FJsonObject>();
	User->SetStringField(TEXT("role"), TEXT("user"));
	User->SetStringField(TEXT("content"), TEXT("hello"));
	Messages.Add(MakeShared<FJsonValueObject>(User));
	const TArray<FAgentMessage> Canonical = KismetAIAgentCodec::FromAnthropicArray(Messages);

	auto Body = [&](const TCHAR* Model)
	{
		Config.Model = Model;
		FLLMRequestShape Shape;
		return KismetAIFixtureTransport::Condensed(Provider.BuildRequestBody(Config, Canonical, {}, TEXT("system prompt"), Shape));
	};
	const FString Claude = Body(TEXT("anthropic/claude-opus-5.5"));
	TestTrue(TEXT("system prompt carries cache_control"),
		Claude.Contains(TEXT(R"("text":"system prompt","cache_control":{"type":"ephemeral"})")));
	TestTrue(TEXT("last user message carries cache_control"),
		Claude.Contains(TEXT(R"("text":"hello","cache_control":{"type":"ephemeral"})")));
	TestFalse(TEXT("other vendors keep plain strings"), Body(TEXT("openai/gpt-6.1-sol")).Contains(TEXT("cache_control")));
	return true;
}
