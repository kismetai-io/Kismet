// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIProviderProfile.h"
#include "KismetAIAgent.h"
#include "KismetAIModelCatalog.h"
#include "KismetAIEmbeddedConfig.h"
#include "KismetAILog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformMisc.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogKismetAIProviders, Log, All);

namespace
{
	const TCHAR* const ProvidersFileName = TEXT("providers.json");

	bool ParseProtocol(const FString& Str, EKismetAIWireProtocol& Out)
	{
		if (Str == TEXT("anthropic_messages")) { Out = EKismetAIWireProtocol::AnthropicMessages; return true; }
		if (Str == TEXT("openai_responses"))   { Out = EKismetAIWireProtocol::OpenAIResponses; return true; }
		if (Str == TEXT("openai_chat"))        { Out = EKismetAIWireProtocol::OpenAIChat; return true; }
		return false;
	}

	bool ParseAuth(const FString& Str, EKismetAIAuthStyle& Out)
	{
		if (Str == TEXT("none"))             { Out = EKismetAIAuthStyle::None; return true; }
		if (Str == TEXT("x-api-key"))        { Out = EKismetAIAuthStyle::XApiKey; return true; }
		if (Str == TEXT("bearer"))           { Out = EKismetAIAuthStyle::Bearer; return true; }
		if (Str == TEXT("x-api-key+bearer")) { Out = EKismetAIAuthStyle::XApiKeyAndBearer; return true; }
		return false;
	}

	// Used when providers.json cannot be loaded, so a broken install still
	// behaves exactly like the pre-profile Anthropic default.
	const FKismetAIProviderProfile& BuiltInAnthropic()
	{
		static const FKismetAIProviderProfile Profile = []()
		{
			FKismetAIProviderProfile P;
			P.Id = TEXT("anthropic");
			P.DisplayName = TEXT("Anthropic");
			P.Protocol = EKismetAIWireProtocol::AnthropicMessages;
			P.BaseUrl = TEXT("https://api.anthropic.com");
			P.KeySlot = TEXT("anthropic");
			P.KeyEnvVar = TEXT("ANTHROPIC_API_KEY");
			P.Auth = EKismetAIAuthStyle::XApiKey;
			P.bPromptCacheMarkers = true;
			return P;
		}();
		return Profile;
	}

	// The legacy routing rule, kept only for saved model ids that are not in the
	// catalog: the behaviour IsOpenAIModel / IsMinimaxModel had.
	FString LegacyProviderForModel(const FString& Model)
	{
		const FString Lower = Model.ToLower();
		if (Lower.StartsWith(TEXT("gpt-")) || Lower.StartsWith(TEXT("gpt5")) || Lower.StartsWith(TEXT("o3"))
			|| Lower.StartsWith(TEXT("o4")))
		{
			return TEXT("openai");
		}
		if (Lower.StartsWith(TEXT("minimax-")))
		{
			return TEXT("minimax");
		}
		return TEXT("anthropic");
	}
}

bool FKismetAIProviderRegistry::LoadFromJson(const FString& Json, FString& OutError)
{
	Profiles.Reset();
	bLoaded = false;

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = TEXT("providers.json is not valid JSON");
		return false;
	}
	int32 SchemaVersion = 0;
	Root->TryGetNumberField(TEXT("schema_version"), SchemaVersion);
	if (SchemaVersion != 1)
	{
		OutError = FString::Printf(TEXT("unsupported schema_version %d"), SchemaVersion);
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!Root->TryGetArrayField(TEXT("providers"), Items) || !Items)
	{
		OutError = TEXT("providers.json has no providers array");
		return false;
	}

	TSet<FString> SeenIds;
	TArray<FKismetAIProviderProfile> Parsed;
	for (int32 Index = 0; Index < Items->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
		if (!(*Items)[Index]->TryGetObject(ObjPtr) || !ObjPtr || !ObjPtr->IsValid())
		{
			OutError = FString::Printf(TEXT("providers[%d] is not an object"), Index);
			return false;
		}
		const FJsonObject& Obj = **ObjPtr;
		FKismetAIProviderProfile P;
		Obj.TryGetStringField(TEXT("id"), P.Id);
		if (P.Id.IsEmpty())
		{
			OutError = FString::Printf(TEXT("providers[%d] has no id"), Index);
			return false;
		}
		bool bDuplicate = false;
		SeenIds.Add(P.Id, &bDuplicate);
		if (bDuplicate)
		{
			OutError = FString::Printf(TEXT("provider id '%s' is used twice"), *P.Id);
			return false;
		}

		Obj.TryGetStringField(TEXT("display_name"), P.DisplayName);
		FString Protocol;
		Obj.TryGetStringField(TEXT("protocol"), Protocol);
		if (!ParseProtocol(Protocol, P.Protocol))
		{
			OutError = FString::Printf(TEXT("provider '%s': unknown protocol '%s'"), *P.Id, *Protocol);
			return false;
		}
		FString Auth;
		Obj.TryGetStringField(TEXT("auth"), Auth);
		if (!ParseAuth(Auth, P.Auth))
		{
			OutError = FString::Printf(TEXT("provider '%s': unknown auth '%s'"), *P.Id, *Auth);
			return false;
		}
		Obj.TryGetStringField(TEXT("base_url"), P.BaseUrl);
		Obj.TryGetStringField(TEXT("base_url_env"), P.BaseUrlEnvVar);
		Obj.TryGetStringField(TEXT("key_slot"), P.KeySlot);
		Obj.TryGetStringField(TEXT("key_env"), P.KeyEnvVar);
		Obj.TryGetBoolField(TEXT("prompt_cache_markers"), P.bPromptCacheMarkers);
		Obj.TryGetBoolField(TEXT("system_as_string"), P.bSystemAsString);
		Obj.TryGetBoolField(TEXT("local"), P.bLocal);
		Obj.TryGetBoolField(TEXT("custom_models"), P.bCustomModels);
		Obj.TryGetStringField(TEXT("strip_model_suffix"), P.StripModelSuffix);
		Obj.TryGetStringField(TEXT("key_url"), P.KeyUrl);
		Obj.TryGetStringField(TEXT("models_url"), P.ModelsUrl);
		Obj.TryGetStringField(TEXT("models_format"), P.ModelsFormat);
		Obj.TryGetStringField(TEXT("models_auth"), P.ModelsAuth);
		static const TSet<FString> KnownFormats = { TEXT(""), TEXT("openrouter"), TEXT("anthropic"), TEXT("openai"), TEXT("google"), TEXT("ollama"), TEXT("lmstudio") };
		if (!KnownFormats.Contains(P.ModelsFormat) || P.ModelsUrl.IsEmpty() != P.ModelsFormat.IsEmpty())
		{
			OutError = FString::Printf(TEXT("provider '%s': models_url and a known models_format go together"), *P.Id);
			return false;
		}
		if (!P.ModelsAuth.IsEmpty() && P.ModelsAuth != TEXT("x-goog-api-key"))
		{
			OutError = FString::Printf(TEXT("provider '%s': unknown models_auth '%s'"), *P.Id, *P.ModelsAuth);
			return false;
		}
		const TSharedPtr<FJsonObject>* ModelsHeaders = nullptr;
		if (Obj.TryGetObjectField(TEXT("models_headers"), ModelsHeaders) && ModelsHeaders && ModelsHeaders->IsValid())
		{
			for (const auto& Pair : (*ModelsHeaders)->Values)
			{
				FString Value;
				if (Pair.Value.IsValid() && Pair.Value->TryGetString(Value))
				{
					P.ModelsHeaders.Add(FString(Pair.Key), Value);
				}
			}
		}
		if (P.Auth != EKismetAIAuthStyle::None && P.KeySlot.IsEmpty())
		{
			OutError = FString::Printf(TEXT("provider '%s' authenticates but has no key_slot"), *P.Id);
			return false;
		}
		const TSharedPtr<FJsonObject>* Headers = nullptr;
		if (Obj.TryGetObjectField(TEXT("extra_headers"), Headers) && Headers && Headers->IsValid())
		{
			for (const auto& Pair : (*Headers)->Values)
			{
				FString Value;
				if (Pair.Value.IsValid() && Pair.Value->TryGetString(Value))
				{
					P.ExtraHeaders.Add(FString(Pair.Key), Value);
				}
			}
		}
		P.BaseUrl.RemoveFromEnd(TEXT("/"));
		Parsed.Add(MoveTemp(P));
	}

	Profiles = MoveTemp(Parsed);
	bLoaded = true;
	return true;
}

const FKismetAIProviderRegistry& FKismetAIProviderRegistry::Get()
{
	static const FKismetAIProviderRegistry Registry = []()
	{
		FKismetAIProviderRegistry Loaded;
		FString DiskPath;
		if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("KismetAI")))
		{
			DiskPath = Plugin->GetBaseDir() / TEXT("Config") / ProvidersFileName;
		}
		FString Json;
		FString Error;
		if (!KismetAIEmbeddedConfig::LoadConfigFile(ProvidersFileName, DiskPath, Json))
		{
			KISMETAI_LOG(LogKismetAIProviders, Error,
				TEXT("Provider profiles not found (%s). Only the built-in Anthropic profile is available."), *DiskPath);
		}
		else if (!Loaded.LoadFromJson(Json, Error))
		{
			KISMETAI_LOG(LogKismetAIProviders, Error,
				TEXT("Provider profiles are invalid: %s. Only the built-in Anthropic profile is available."), *Error);
		}
		return Loaded;
	}();
	return Registry;
}

const FKismetAIProviderProfile* FKismetAIProviderRegistry::Find(const FString& ProviderId) const
{
	for (const FKismetAIProviderProfile& Profile : Profiles)
	{
		if (Profile.Id.Equals(ProviderId, ESearchCase::IgnoreCase))
		{
			return &Profile;
		}
	}
	return nullptr;
}

namespace KismetAIProviders
{
	FString ProviderIdForModel(const FString& Model)
	{
		if (const FKismetAIModelEntry* Entry = FKismetAIModelCatalog::Get().Find(Model))
		{
			if (!Entry->Provider.IsEmpty())
			{
				return Entry->Provider;
			}
		}
		return LegacyProviderForModel(Model);
	}

	FString ResolveProviderId(const FKismetAIAgentConfig& Config)
	{
		return Config.ProviderId.IsEmpty() ? ProviderIdForModel(Config.Model) : Config.ProviderId;
	}

	bool ValidateSelection(const FKismetAIAgentConfig& Config, FString& OutError,
		const FKismetAIProviderRegistry& Registry)
	{
		const FString Id = ResolveProviderId(Config);
		if (Id.Equals(TEXT("anthropic"), ESearchCase::IgnoreCase) || Registry.Find(Id))
		{
			OutError.Reset();
			return true;
		}
		OutError = FString::Printf(TEXT("Provider '%s' is unavailable. Select a configured provider before running; no fallback provider was selected."), *Id);
		return false;
	}

	const FKismetAIProviderProfile& ResolveProfile(const FKismetAIAgentConfig& Config)
	{
		const FString Id = ResolveProviderId(Config);
		if (const FKismetAIProviderProfile* Profile = FKismetAIProviderRegistry::Get().Find(Id))
		{
			return *Profile;
		}
		if (!Id.Equals(TEXT("anthropic"), ESearchCase::IgnoreCase))
		{
			KISMETAI_LOG(LogKismetAIProviders, Error,
				TEXT("Unknown provider '%s' for model '%s'; using the built-in Anthropic profile."), *Id, *Config.Model);
		}
		return BuiltInAnthropic();
	}

	FString GetBaseUrl(const FKismetAIAgentConfig& Config, const FKismetAIProviderProfile& Profile)
	{
		FString Url;
		if (const FString* Override = Config.ProviderBaseUrls.Find(Profile.Id))
		{
			Url = *Override;
			if (Config.bProviderEnvironmentResolved) return Url;
		}
		if (Url.IsEmpty() && !Profile.BaseUrlEnvVar.IsEmpty())
		{
			Url = FPlatformMisc::GetEnvironmentVariable(*Profile.BaseUrlEnvVar);
		}
		// The anthropic profile predates profiles: Config.ApiBaseUrl was its
		// only override (tests and evals point it at local servers).
		if (Url.IsEmpty() && Profile.Id == TEXT("anthropic")
			&& !Config.ApiBaseUrl.IsEmpty() && Config.ApiBaseUrl != TEXT("https://api.anthropic.com"))
		{
			Url = Config.ApiBaseUrl;
		}
		if (Url.IsEmpty())
		{
			Url = Profile.BaseUrl;
		}
		Url.TrimStartAndEndInline();
		Url.RemoveFromEnd(TEXT("/"));
		return Url;
	}

	FString GetApiKey(const FKismetAIAgentConfig& Config, const FKismetAIProviderProfile& Profile)
	{
		const FString& Slot = Profile.KeySlot;
		if (Slot.IsEmpty())           return FString();
		if (Slot == TEXT("anthropic")) return Config.ApiKey;
		if (Slot == TEXT("openai"))    return Config.OpenAIApiKey;
		if (Slot == TEXT("minimax"))   return Config.MinimaxApiKey;
		if (const FString* Key = Config.ProviderKeys.Find(Slot))
		{
			return *Key;
		}
		return FString();
	}

	void SetApiKey(FKismetAIAgentConfig& Config, const FString& Slot, const FString& Key)
	{
		if (Slot.IsEmpty())                 return;
		if (Slot == TEXT("anthropic"))      { Config.ApiKey = Key; return; }
		if (Slot == TEXT("openai"))         { Config.OpenAIApiKey = Key; return; }
		if (Slot == TEXT("minimax"))        { Config.MinimaxApiKey = Key; return; }
		if (Key.IsEmpty())
		{
			Config.ProviderKeys.Remove(Slot);
		}
		else
		{
			Config.ProviderKeys.Add(Slot, Key);
		}
	}

	bool IsSelfHosted(const FKismetAIProviderProfile& Profile)
	{
		return Profile.bLocal || (Profile.bCustomModels && Profile.BaseUrl.IsEmpty());
	}

	bool HasUsableKey(const FKismetAIAgentConfig& Config, const FKismetAIProviderProfile& Profile)
	{
		return Profile.Auth == EKismetAIAuthStyle::None || !GetApiKey(Config, Profile).IsEmpty();
	}

	TArray<FString> BuildAuthHeaders(const FKismetAIAgentConfig& Config, const FKismetAIProviderProfile& Profile)
	{
		TArray<FString> Headers;
		const FString Key = GetApiKey(Config, Profile);
		// Emit the header even when the key is empty: the upstream answers 401,
		// which the no-key gate and error mapping turn into setup guidance.
		switch (Profile.Auth)
		{
		case EKismetAIAuthStyle::None:
			break;
		case EKismetAIAuthStyle::XApiKey:
			Headers.Add(FString::Printf(TEXT("x-api-key: %s"), *Key));
			break;
		case EKismetAIAuthStyle::Bearer:
			Headers.Add(FString::Printf(TEXT("Authorization: Bearer %s"), *Key));
			break;
		case EKismetAIAuthStyle::XApiKeyAndBearer:
			Headers.Add(FString::Printf(TEXT("x-api-key: %s"), *Key));
			Headers.Add(FString::Printf(TEXT("Authorization: Bearer %s"), *Key));
			break;
		}
		for (const TPair<FString, FString>& Pair : Profile.ExtraHeaders)
		{
			Headers.Add(FString::Printf(TEXT("%s: %s"), *Pair.Key, *Pair.Value));
		}
		return Headers;
	}

	TArray<FString> BuildModelsHeaders(const FKismetAIAgentConfig& Config, const FKismetAIProviderProfile& Profile)
	{
		TArray<FString> Headers;
		if (Profile.ModelsAuth == TEXT("x-goog-api-key"))
		{
			Headers.Add(FString::Printf(TEXT("x-goog-api-key: %s"), *GetApiKey(Config, Profile)));
		}
		else
		{
			FKismetAIProviderProfile AuthOnly = Profile;
			AuthOnly.ExtraHeaders.Reset();
			Headers = BuildAuthHeaders(Config, AuthOnly);
		}
		for (const TPair<FString, FString>& Pair : Profile.ModelsHeaders)
		{
			Headers.Add(FString::Printf(TEXT("%s: %s"), *Pair.Key, *Pair.Value));
		}
		return Headers;
	}

	FString WireModelId(const FKismetAIAgentConfig& Config, const FKismetAIProviderProfile& Profile)
	{
		FString Wire = Config.Model;
		// A fast tier that is a separate model (Kimi K3.1 Pro -> its high-speed
		// variant) is asked for by sending that model's id.
		if (Config.bFastMode)
		{
			const FKismetAIModelCapabilities& Caps = FKismetAIModelCatalog::Get().FindOrDefaults(Config.Model).Caps;
			if (Caps.FastModeWire == EKismetAIFastModeWire::ModelSwap && !Caps.FastModeModelId.IsEmpty())
			{
				Wire = Caps.FastModeModelId;
			}
		}
		if (!Profile.StripModelSuffix.IsEmpty())
		{
			Wire.RemoveFromEnd(Profile.StripModelSuffix);
		}
		return Wire;
	}
}
