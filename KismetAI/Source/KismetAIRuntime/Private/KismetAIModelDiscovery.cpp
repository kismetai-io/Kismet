// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIModelDiscovery.h"
#include "KismetAIAgent.h"
#include "KismetAIModelClient.h"
#include "KismetAIProviderProfile.h"
#include "KismetAITransportScratch.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Named (not anonymous) so unity builds cannot collide with another .cpp's helpers.
namespace KismetAIModelDiscoveryDetail
{
	TSharedPtr<FJsonObject> ParseObject(const FString& Body)
	{
		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return nullptr;
		}
		return Root;
	}

	const TArray<TSharedPtr<FJsonValue>>* GetArray(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		return Obj.IsValid() && Obj->TryGetArrayField(Field, Values) ? Values : nullptr;
	}

	TSharedPtr<FJsonObject> GetObject(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Child = nullptr;
		return Obj.IsValid() && Obj->TryGetObjectField(Field, Child) && Child ? *Child : nullptr;
	}

	// A positive integer field; JSON null, strings and non-positive numbers read as 0.
	int32 PositiveInt(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		double Value = 0.0;
		if (!Obj.IsValid() || !Obj->TryGetNumberField(Field, Value) || Value <= 0.0)
		{
			return 0;
		}
		return Value >= static_cast<double>(MAX_int32) ? MAX_int32 : static_cast<int32>(Value);
	}

	bool StringArrayContains(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, const TCHAR* Wanted, bool& bOutHasField)
	{
		bOutHasField = false;
		const TArray<TSharedPtr<FJsonValue>>* Values = GetArray(Obj, Field);
		if (!Values)
		{
			return false;
		}
		bOutHasField = true;
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Str;
			if (Value.IsValid() && Value->TryGetString(Str) && Str.Equals(Wanted, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	int64 ParseIsoTime(const FString& Text)
	{
		if (Text.IsEmpty())
		{
			return 0;
		}
		FDateTime Time;
		// Ollama writes nanoseconds, which ParseIso8601 rejects; seconds are enough.
		FString Trimmed = Text;
		int32 Dot = INDEX_NONE;
		if (Trimmed.FindChar(TEXT('.'), Dot))
		{
			int32 End = Dot + 1;
			while (End < Trimmed.Len() && FChar::IsDigit(Trimmed[End]))
			{
				++End;
			}
			Trimmed = Trimmed.Left(Dot) + Trimmed.Mid(End);
		}
		return FDateTime::ParseIso8601(*Trimmed, Time) ? Time.ToUnixTimestamp() : 0;
	}

	// OpenRouter prices are $ per token as strings ("0.000004").
	double PerTokenToPerMillion(const TSharedPtr<FJsonObject>& Pricing, const TCHAR* Field, bool& bOutFound)
	{
		FString Text;
		double Number = 0.0;
		if (Pricing.IsValid() && Pricing->TryGetStringField(Field, Text) && !Text.IsEmpty())
		{
			bOutFound = true;
			return FCString::Atod(*Text) * 1000000.0;
		}
		if (Pricing.IsValid() && Pricing->TryGetNumberField(Field, Number))
		{
			bOutFound = true;
			return Number * 1000000.0;
		}
		return 0.0;
	}

	// "Anthropic: Claude Opus 5.5" -> "Claude Opus 5.5".
	FString StripVendorPrefix(const FString& Name)
	{
		int32 Colon = INDEX_NONE;
		if (Name.FindChar(TEXT(':'), Colon) && Colon + 2 <= Name.Len())
		{
			return Name.Mid(Colon + 1).TrimStartAndEnd();
		}
		return Name;
	}

	bool ParseOpenRouter(const TSharedPtr<FJsonObject>& Root, TArray<FKismetAILiveModel>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Data = GetArray(Root, TEXT("data"));
		if (!Data)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Data)
		{
			const TSharedPtr<FJsonObject>* ItemPtr = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(ItemPtr) || !ItemPtr)
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& Item = *ItemPtr;
			FKismetAILiveModel M;
			Item->TryGetStringField(TEXT("id"), M.Id);
			if (M.Id.IsEmpty())
			{
				continue;
			}
			FString Name;
			Item->TryGetStringField(TEXT("name"), Name);
			M.DisplayName = StripVendorPrefix(Name);
			double Created = 0.0;
			Item->TryGetNumberField(TEXT("created"), Created);
			M.Created = static_cast<int64>(Created);
			M.ContextWindow = PositiveInt(Item, TEXT("context_length"));
			const TSharedPtr<FJsonObject> Top = GetObject(Item, TEXT("top_provider"));
			M.MaxOutputTokens = PositiveInt(Top, TEXT("max_completion_tokens"));
			if (M.ContextWindow == 0)
			{
				M.ContextWindow = PositiveInt(Top, TEXT("context_length"));
			}

			bool bHasModalities = false;
			const bool bImage = StringArrayContains(GetObject(Item, TEXT("architecture")), TEXT("input_modalities"), TEXT("image"), bHasModalities);
			if (bHasModalities)
			{
				M.bImageInput = bImage;
			}
			bool bHasParams = false;
			const bool bTools = StringArrayContains(Item, TEXT("supported_parameters"), TEXT("tools"), bHasParams);
			if (bHasParams)
			{
				bool bUnused = false;
				M.bToolCalls = bTools;
				M.bReasoning = StringArrayContains(Item, TEXT("supported_parameters"), TEXT("reasoning"), bUnused)
					|| StringArrayContains(Item, TEXT("supported_parameters"), TEXT("reasoning_effort"), bUnused);
			}

			const TSharedPtr<FJsonObject> Pricing = GetObject(Item, TEXT("pricing"));
			bool bInput = false;
			bool bOutput = false;
			bool bUnused = false;
			FKismetAIModelPrice Price;
			Price.Input = PerTokenToPerMillion(Pricing, TEXT("prompt"), bInput);
			Price.Output = PerTokenToPerMillion(Pricing, TEXT("completion"), bOutput);
			Price.CacheRead = PerTokenToPerMillion(Pricing, TEXT("input_cache_read"), bUnused);
			Price.CacheWrite = PerTokenToPerMillion(Pricing, TEXT("input_cache_write"), bUnused);
			// "-1" marks a router with variable pricing; never show a negative price.
			if (bInput && bOutput && Price.Input >= 0.0 && Price.Output >= 0.0)
			{
				M.Price = Price;
			}
			Out.Add(MoveTemp(M));
		}
		return true;
	}

	bool ParseAnthropic(const TSharedPtr<FJsonObject>& Root, TArray<FKismetAILiveModel>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Data = GetArray(Root, TEXT("data"));
		if (!Data)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Data)
		{
			const TSharedPtr<FJsonObject>* ItemPtr = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(ItemPtr) || !ItemPtr)
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& Item = *ItemPtr;
			FKismetAILiveModel M;
			Item->TryGetStringField(TEXT("id"), M.Id);
			if (M.Id.IsEmpty())
			{
				continue;
			}
			Item->TryGetStringField(TEXT("display_name"), M.DisplayName);
			FString CreatedAt;
			Item->TryGetStringField(TEXT("created_at"), CreatedAt);
			M.Created = ParseIsoTime(CreatedAt);
			M.ContextWindow = PositiveInt(Item, TEXT("max_input_tokens"));
			M.MaxOutputTokens = PositiveInt(Item, TEXT("max_tokens"));
			const TSharedPtr<FJsonObject> Caps = GetObject(Item, TEXT("capabilities"));
			if (Caps.IsValid())
			{
				bool bImage = false;
				if (Caps->TryGetBoolField(TEXT("image_input"), bImage))
				{
					M.bImageInput = bImage;
				}
				else if (const TSharedPtr<FJsonObject> Image = GetObject(Caps, TEXT("image_input")))
				{
					if (Image->TryGetBoolField(TEXT("supported"), bImage))
					{
						M.bImageInput = bImage;
					}
				}
			}
			Out.Add(MoveTemp(M));
		}
		return true;
	}

	// The OpenAI shape and the fields compatible servers add to it.
	bool ParseOpenAI(const TSharedPtr<FJsonObject>& Root, TArray<FKismetAILiveModel>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Data = GetArray(Root, TEXT("data"));
		if (!Data)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Data)
		{
			const TSharedPtr<FJsonObject>* ItemPtr = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(ItemPtr) || !ItemPtr)
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& Item = *ItemPtr;
			FKismetAILiveModel M;
			Item->TryGetStringField(TEXT("id"), M.Id);
			if (M.Id.IsEmpty())
			{
				continue;
			}
			Item->TryGetStringField(TEXT("name"), M.DisplayName);
			double Created = 0.0;
			Item->TryGetNumberField(TEXT("created"), Created);
			M.Created = static_cast<int64>(Created);
			// Moonshot: context_length; DeepSeek: context_window; vLLM: max_model_len;
			// llama.cpp: meta.n_ctx_train.
			for (const TCHAR* Field : { TEXT("context_length"), TEXT("context_window"), TEXT("max_model_len") })
			{
				if (M.ContextWindow == 0)
				{
					M.ContextWindow = PositiveInt(Item, Field);
				}
			}
			if (M.ContextWindow == 0)
			{
				M.ContextWindow = PositiveInt(GetObject(Item, TEXT("meta")), TEXT("n_ctx_train"));
			}
			M.MaxOutputTokens = PositiveInt(Item, TEXT("max_output_tokens"));
			bool bImage = false;
			if (Item->TryGetBoolField(TEXT("supports_image_in"), bImage))
			{
				M.bImageInput = bImage;
			}
			else
			{
				bool bHasModalities = false;
				bImage = StringArrayContains(Item, TEXT("input_modalities"), TEXT("image"), bHasModalities);
				if (bHasModalities)
				{
					M.bImageInput = bImage;
				}
			}
			bool bReasoning = false;
			if (Item->TryGetBoolField(TEXT("supports_reasoning"), bReasoning))
			{
				M.bReasoning = bReasoning;
			}
			Out.Add(MoveTemp(M));
		}
		return true;
	}

	bool ParseGoogle(const TSharedPtr<FJsonObject>& Root, TArray<FKismetAILiveModel>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Models = GetArray(Root, TEXT("models"));
		if (!Models)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Models)
		{
			const TSharedPtr<FJsonObject>* ItemPtr = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(ItemPtr) || !ItemPtr)
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& Item = *ItemPtr;
			bool bHasMethods = false;
			if (!StringArrayContains(Item, TEXT("supportedGenerationMethods"), TEXT("generateContent"), bHasMethods) && bHasMethods)
			{
				continue;  // embeddings, AQA and other non-chat models
			}
			FKismetAILiveModel M;
			Item->TryGetStringField(TEXT("name"), M.Id);
			M.Id.RemoveFromStart(TEXT("models/"));
			if (M.Id.IsEmpty())
			{
				continue;
			}
			Item->TryGetStringField(TEXT("displayName"), M.DisplayName);
			M.ContextWindow = PositiveInt(Item, TEXT("inputTokenLimit"));
			M.MaxOutputTokens = PositiveInt(Item, TEXT("outputTokenLimit"));
			Out.Add(MoveTemp(M));
		}
		return true;
	}

	bool ParseOllamaTags(const TSharedPtr<FJsonObject>& Root, TArray<FKismetAILiveModel>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Models = GetArray(Root, TEXT("models"));
		if (!Models)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Models)
		{
			const TSharedPtr<FJsonObject>* ItemPtr = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(ItemPtr) || !ItemPtr)
			{
				continue;
			}
			FKismetAILiveModel M;
			(*ItemPtr)->TryGetStringField(TEXT("name"), M.Id);
			if (M.Id.IsEmpty())
			{
				(*ItemPtr)->TryGetStringField(TEXT("model"), M.Id);
			}
			if (M.Id.IsEmpty())
			{
				continue;
			}
			M.DisplayName = M.Id;
			FString Modified;
			(*ItemPtr)->TryGetStringField(TEXT("modified_at"), Modified);
			M.Created = ParseIsoTime(Modified);
			Out.Add(MoveTemp(M));
		}
		return true;
	}

	bool ParseLMStudio(const TSharedPtr<FJsonObject>& Root, TArray<FKismetAILiveModel>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Models = GetArray(Root, TEXT("models"));
		if (!Models)
		{
			// Older builds and the OpenAI-compatible route list {data:[...]}.
			return ParseOpenAI(Root, Out);
		}
		for (const TSharedPtr<FJsonValue>& Value : *Models)
		{
			const TSharedPtr<FJsonObject>* ItemPtr = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(ItemPtr) || !ItemPtr)
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& Item = *ItemPtr;
			FString Type;
			if (Item->TryGetStringField(TEXT("type"), Type) && !Type.Equals(TEXT("llm"), ESearchCase::IgnoreCase))
			{
				continue;  // embedding models cannot chat
			}
			FKismetAILiveModel M;
			Item->TryGetStringField(TEXT("key"), M.Id);
			if (M.Id.IsEmpty())
			{
				Item->TryGetStringField(TEXT("id"), M.Id);
			}
			if (M.Id.IsEmpty())
			{
				continue;
			}
			Item->TryGetStringField(TEXT("display_name"), M.DisplayName);
			M.ContextWindow = PositiveInt(Item, TEXT("max_context_length"));
			// A loaded instance runs with its own (usually smaller) context: that is
			// the window a request actually gets.
			if (const TArray<TSharedPtr<FJsonValue>>* Loaded = GetArray(Item, TEXT("loaded_instances")))
			{
				for (const TSharedPtr<FJsonValue>& Instance : *Loaded)
				{
					const TSharedPtr<FJsonObject>* InstancePtr = nullptr;
					if (Instance.IsValid() && Instance->TryGetObject(InstancePtr) && InstancePtr)
					{
						M.bLoaded = true;
						const int32 LoadedContext = PositiveInt(GetObject(*InstancePtr, TEXT("config")), TEXT("context_length"));
						if (LoadedContext > 0)
						{
							M.ContextWindow = LoadedContext;
						}
						break;
					}
				}
			}
			if (const TSharedPtr<FJsonObject> Caps = GetObject(Item, TEXT("capabilities")))
			{
				bool bVision = false;
				if (Caps->TryGetBoolField(TEXT("vision"), bVision))
				{
					M.bImageInput = bVision;
				}
				// trained_for_tool_use=false still accepts tools (LM Studio falls back to
				// a default tool prompt), so it is not a refusal signal.
			}
			Out.Add(MoveTemp(M));
		}
		return true;
	}

	TArray<FString> Tokens(const FString& Text)
	{
		TArray<FString> Out;
		FString Current;
		for (const TCHAR Char : Text)
		{
			if (Char == TEXT('-') || Char == TEXT('.') || Char == TEXT('_') || Char == TEXT(':') || Char == TEXT(' '))
			{
				if (!Current.IsEmpty())
				{
					Out.Add(Current);
					Current.Reset();
				}
			}
			else
			{
				Current.AppendChar(Char);
			}
		}
		if (!Current.IsEmpty())
		{
			Out.Add(Current);
		}
		return Out;
	}

	void ApplyLive(FKismetAIModelEntry& Entry, const FKismetAILiveModel& Live)
	{
		if (Live.ContextWindow > 0)
		{
			Entry.ContextWindow = Live.ContextWindow;
			Entry.ContextSource = TEXT("live");
		}
		if (Live.MaxOutputTokens > 0)
		{
			Entry.MaxOutputTokens = Live.MaxOutputTokens;
		}
		if (Live.Price.IsSet())
		{
			const double CacheWrite = Entry.Price.CacheWrite;
			Entry.Price = Live.Price.GetValue();
			if (Entry.Price.CacheWrite <= 0.0)
			{
				Entry.Price.CacheWrite = CacheWrite;
			}
		}
		if (Live.bImageInput.IsSet())
		{
			Entry.Caps.bImageInput = Live.bImageInput.GetValue();
		}
		if (Live.bToolCalls.IsSet())
		{
			Entry.Caps.bToolCalls = Live.bToolCalls.GetValue();
		}
		// Only an explicit "no reasoning" changes the request shape, and only for
		// rows that send reasoning_effort: never invent levels the catalog lacks.
		if (Live.bReasoning.IsSet() && !Live.bReasoning.GetValue() && Entry.Caps.bReasoning)
		{
			Entry.Caps.bReasoning = false;
			Entry.Caps.EffortLevels.Reset();
			Entry.RecommendedEffort.Reset();
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// curl fetcher
// ─────────────────────────────────────────────────────────────────────────────

FKismetAIHttpResponse FKismetAICurlHttpFetcher::Send(const FKismetAIHttpRequest& Request)
{
	FKismetAIHttpResponse Response;
	const FString Override = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMETAI_TRANSPORT_SCRATCH_ROOT"));
	const FString Root = FPaths::ConvertRelativePathToFull(Override.IsEmpty()
		? FString(FPlatformProcess::UserTempDir()) / TEXT("KismetAI") / TEXT("http") : Override);
	const FString Dir = Root / FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	FKismetAITransportScratch Scratch;
	if (!Scratch.Begin(Dir)
		|| !Scratch.WriteText(TEXT("curl.cfg"), FKismetAICurlModelClient::BuildCurlConfigFile(Request.Headers))
		|| (!Request.Body.IsEmpty() && !Scratch.WriteText(TEXT("body.json"), Request.Body))
		|| !Scratch.MarkLaunching())
	{
		return Response;
	}

	const int32 Timeout = FMath::Clamp(Request.TimeoutSeconds, 1, 60);
	FString Args = FString::Printf(TEXT("-sS --connect-timeout %d --max-time %d -X %s \"%s\" --config \"%s\" -o \"%s\" -w \"%%{http_code}\""),
		FMath::Min(Timeout, 5), Timeout, *Request.Verb, *Request.Url, *(Dir / TEXT("curl.cfg")), *(Dir / TEXT("response.sse")));
	if (!Request.Body.IsEmpty())
	{
		Args += FString::Printf(TEXT(" -d \"@%s\""), *(Dir / TEXT("body.json")));
	}
#if PLATFORM_WINDOWS
	const TCHAR* CurlExe = TEXT("curl.exe");
#else
	const TCHAR* CurlExe = TEXT("/usr/bin/curl");
#endif
	int32 ReturnCode = -1;
	FString StdOut;
	FString StdErr;
	FPlatformProcess::ExecProcess(CurlExe, *Args, &ReturnCode, &StdOut, &StdErr);
	Response.Status = FCString::Atoi(*StdOut.TrimStartAndEnd());
	if (Response.Status > 0)
	{
		FFileHelper::LoadFileToString(Response.Body, *(Dir / TEXT("response.sse")));
	}
	return Response;
}

// ─────────────────────────────────────────────────────────────────────────────
// Parsing
// ─────────────────────────────────────────────────────────────────────────────

namespace KismetAIModelsApi
{
	using namespace KismetAIModelDiscoveryDetail;

	bool ParseFormat(const FString& Name, EKismetAIModelsFormat& Out)
	{
		if (Name.IsEmpty() || Name == TEXT("none")) { Out = EKismetAIModelsFormat::None; return true; }
		if (Name == TEXT("openrouter")) { Out = EKismetAIModelsFormat::OpenRouter; return true; }
		if (Name == TEXT("anthropic"))  { Out = EKismetAIModelsFormat::Anthropic; return true; }
		if (Name == TEXT("openai"))     { Out = EKismetAIModelsFormat::OpenAI; return true; }
		if (Name == TEXT("google"))     { Out = EKismetAIModelsFormat::Google; return true; }
		if (Name == TEXT("ollama"))     { Out = EKismetAIModelsFormat::Ollama; return true; }
		if (Name == TEXT("lmstudio"))   { Out = EKismetAIModelsFormat::LMStudio; return true; }
		return false;
	}

	bool Parse(EKismetAIModelsFormat Format, const FString& Body, TArray<FKismetAILiveModel>& OutModels, FString& OutError)
	{
		OutModels.Reset();
		const TSharedPtr<FJsonObject> Root = ParseObject(Body);
		if (!Root.IsValid())
		{
			OutError = TEXT("the model list is not a JSON object");
			return false;
		}
		bool bOk = false;
		switch (Format)
		{
		case EKismetAIModelsFormat::OpenRouter: bOk = ParseOpenRouter(Root, OutModels); break;
		case EKismetAIModelsFormat::Anthropic:  bOk = ParseAnthropic(Root, OutModels); break;
		case EKismetAIModelsFormat::OpenAI:     bOk = ParseOpenAI(Root, OutModels); break;
		case EKismetAIModelsFormat::Google:     bOk = ParseGoogle(Root, OutModels); break;
		case EKismetAIModelsFormat::Ollama:     bOk = ParseOllamaTags(Root, OutModels); break;
		case EKismetAIModelsFormat::LMStudio:   bOk = ParseLMStudio(Root, OutModels); break;
		case EKismetAIModelsFormat::None:       break;
		}
		if (!bOk)
		{
			OutModels.Reset();
			OutError = TEXT("the model list has no models array");
		}
		return bOk;
	}

	bool ApplyOllamaShow(const FString& Body, FKismetAILiveModel& Model)
	{
		const TSharedPtr<FJsonObject> Root = ParseObject(Body);
		if (!Root.IsValid())
		{
			return false;
		}
		// The trained maximum, under "<arch>.context_length" (gemma4.context_length).
		if (const TSharedPtr<FJsonObject> Info = GetObject(Root, TEXT("model_info")))
		{
			for (const auto& Pair : Info->Values)
			{
				double Value = 0.0;
				if (FString(Pair.Key).EndsWith(TEXT(".context_length")) && Pair.Value.IsValid() && Pair.Value->TryGetNumber(Value) && Value > 0.0)
				{
					Model.ContextWindow = static_cast<int32>(FMath::Min(Value, static_cast<double>(MAX_int32)));
					break;
				}
			}
		}
		// A num_ctx in the Modelfile is what Ollama loads, and it wins over the trained maximum.
		FString Parameters;
		if (Root->TryGetStringField(TEXT("parameters"), Parameters))
		{
			TArray<FString> Lines;
			Parameters.ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				TArray<FString> Parts;
				Line.ParseIntoArrayWS(Parts);
				if (Parts.Num() == 2 && Parts[0] == TEXT("num_ctx") && FCString::Atoi(*Parts[1]) > 0)
				{
					Model.ContextWindow = FCString::Atoi(*Parts[1]);
				}
			}
		}
		bool bHasCaps = false;
		const bool bTools = StringArrayContains(Root, TEXT("capabilities"), TEXT("tools"), bHasCaps);
		if (bHasCaps)
		{
			bool bUnused = false;
			Model.bToolCalls = bTools;
			Model.bImageInput = StringArrayContains(Root, TEXT("capabilities"), TEXT("vision"), bUnused);
		}
		return true;
	}

	FString FamilyKey(const FString& Id)
	{
		FString Lower = Id.ToLower();
		FString Vendor;
		int32 Slash = INDEX_NONE;
		if (Lower.FindLastChar(TEXT('/'), Slash))
		{
			Vendor = Lower.Left(Slash + 1);
			Lower = Lower.Mid(Slash + 1);
		}
		TArray<FString> Parts;
		for (FString Token : Tokens(Lower))
		{
			FString Letters;
			for (const TCHAR Char : Token)
			{
				if (!FChar::IsDigit(Char))
				{
					Letters.AppendChar(Char);
				}
			}
			if (!Letters.IsEmpty())
			{
				Parts.Add(Letters);
			}
		}
		return Vendor + FString::Join(Parts, TEXT("-"));
	}

	bool IsSnapshotOrVariant(const FString& Id)
	{
		const FString Lower = Id.ToLower();
		if (Lower.Contains(TEXT(":")) && !Lower.StartsWith(TEXT("models/")))
		{
			// OpenRouter :free / :batch / :thinking variants. (Ollama tags never reach the merge.)
			return true;
		}
		for (const FString& Token : Tokens(Lower))
		{
			bool bAllDigits = Token.Len() > 0;
			for (const TCHAR Char : Token)
			{
				bAllDigits &= FChar::IsDigit(Char) != 0;
			}
			if ((bAllDigits && Token.Len() >= 4) || Token == TEXT("latest") || Token == TEXT("preview") || Token == TEXT("exp"))
			{
				return true;
			}
		}
		return false;
	}

	FMergeResult MergeCatalog(const TArray<const FKismetAIModelEntry*>& Curated, const TArray<FKismetAILiveModel>& Live)
	{
		FMergeResult Result;
		TMap<FString, const FKismetAILiveModel*> LiveById;
		for (const FKismetAILiveModel& Model : Live)
		{
			LiveById.Add(Model.Id.ToLower(), &Model);
		}
		auto FindLive = [&LiveById](const FKismetAIModelEntry& Entry) -> const FKismetAILiveModel*
		{
			if (const FKismetAILiveModel* const* Found = LiveById.Find(Entry.Id.ToLower()))
			{
				return *Found;
			}
			for (const FString& Other : Entry.Ids)
			{
				if (const FKismetAILiveModel* const* Found = LiveById.Find(Other.ToLower()))
				{
					return *Found;
				}
			}
			return nullptr;
		};

		struct FFamily
		{
			int64 NewestCreated = 0;
			const FKismetAIModelEntry* Representative = nullptr;
		};
		TMap<FString, FFamily> Families;
		TSet<FString> CuratedIds;
		for (const FKismetAIModelEntry* Entry : Curated)
		{
			CuratedIds.Add(Entry->Id.ToLower());
			for (const FString& Other : Entry->Ids)
			{
				CuratedIds.Add(Other.ToLower());
			}
			const FKismetAILiveModel* Match = FindLive(*Entry);
			if (!Match)
			{
				Result.Dropped.Add(Entry->Id);
				continue;
			}
			FKismetAIModelEntry Updated = *Entry;
			ApplyLive(Updated, *Match);
			Result.PickerRows.Add(Updated);
			Result.Overlay.Add(MoveTemp(Updated));

			FFamily& Family = Families.FindOrAdd(FamilyKey(Entry->Id));
			if (!Family.Representative || Match->Created > Family.NewestCreated)
			{
				Family.NewestCreated = Match->Created;
				Family.Representative = Entry;
			}
		}

		if (Result.PickerRows.Num() == 0 && Curated.Num() > 0)
		{
			// The list names none of the models we know: a wrong endpoint or a
			// proxy's own list, not a vendor that retired everything.
			Result = FMergeResult();
			for (const FKismetAIModelEntry* Entry : Curated)
			{
				Result.PickerRows.Add(*Entry);
			}
			return Result;
		}
		Result.bUsedLive = true;

		TArray<const FKismetAILiveModel*> Newer;
		for (const FKismetAILiveModel& Model : Live)
		{
			if (CuratedIds.Contains(Model.Id.ToLower()) || Model.Created <= 0 || IsSnapshotOrVariant(Model.Id))
			{
				continue;
			}
			const FFamily* Family = Families.Find(FamilyKey(Model.Id));
			if (Family && Family->Representative && Family->NewestCreated > 0 && Model.Created > Family->NewestCreated)
			{
				Newer.Add(&Model);
			}
		}
		Newer.Sort([](const FKismetAILiveModel& A, const FKismetAILiveModel& B) { return A.Created > B.Created; });
		for (const FKismetAILiveModel* Model : Newer)
		{
			const FKismetAIModelEntry& Rep = *Families.Find(FamilyKey(Model->Id))->Representative;
			FKismetAIModelEntry Added = Rep;
			Added.Id = Model->Id;
			Added.Ids.Reset();
			Added.Alias = Model->Id;
			Added.Aliases.Reset();
			Added.MatchPatterns = { Model->Id.ToLower() };
			Added.DisplayName = Model->DisplayName.IsEmpty() ? Model->Id : Model->DisplayName;
			Added.OpenRouterId.Empty();
			Added.DerivedFrom.Empty();
			Added.bInPicker = true;
			// A newer model's fast tier, if any, is not known; never send the old one's.
			Added.FastPrice.Reset();
			Added.Caps.FastModeWire = EKismetAIFastModeWire::None;
			Added.Caps.FastModeModelId.Empty();
			ApplyLive(Added, *Model);
			Result.Added.Add(Added.Id);
			Result.PickerRows.Add(Added);
			Result.Overlay.Add(MoveTemp(Added));
		}
		return Result;
	}

	FKismetAIModelEntry MakeTypedLocalEntry(const FString& ProviderId, const FString& ModelId, const TMap<FString, int32>& UserContext)
	{
		FKismetAILiveModel Model;
		Model.Id = ModelId;
		TArray<FKismetAIModelEntry> Rows = MakeLocalEntries(ProviderId, { Model }, UserContext);
		return Rows.Num() > 0 ? Rows[0] : FKismetAIModelEntry();
	}

	TArray<FKismetAIModelEntry> MakeLocalEntries(const FString& ProviderId, const TArray<FKismetAILiveModel>& Models,
		const TMap<FString, int32>& UserContext)
	{
		// The conservative unknown-model request shape: no thinking field, no
		// sampling params. A local model is not in the catalog, so nothing more is known.
		const FKismetAIModelEntry& Unknown = FKismetAIModelCatalog::Get().FindOrDefaults(FString());
		TArray<FKismetAIModelEntry> Rows;
		for (const FKismetAILiveModel& Model : Models)
		{
			if (Model.Id.IsEmpty())
			{
				continue;
			}
			FKismetAIModelEntry Row;
			Row.Caps = Unknown.Caps;
			Row.Id = Model.Id;
			Row.Alias = Model.Id;
			Row.MatchPatterns = { Model.Id.ToLower() };
			Row.DisplayName = Model.DisplayName.IsEmpty() ? Model.Id : Model.DisplayName;
			Row.Provider = ProviderId;
			Row.bInPicker = true;
			Row.bPatternOnly = false;
			Row.Price = FKismetAIModelPrice();
			Row.MaxOutputTokens = Model.MaxOutputTokens;
			if (const int32* User = UserContext.Find(Model.Id.ToLower()); User && *User > 0)
			{
				Row.ContextWindow = *User;
				Row.ContextSource = TEXT("user");
			}
			else if (Model.ContextWindow > 0)
			{
				Row.ContextWindow = Model.ContextWindow;
				Row.ContextSource = TEXT("reported");
			}
			else
			{
				Row.ContextWindow = LocalDefaultContext;
				Row.ContextSource = TEXT("assumed");
			}
			if (Model.bToolCalls.IsSet())
			{
				Row.Caps.bToolCalls = Model.bToolCalls.GetValue();
			}
			if (Model.bImageInput.IsSet())
			{
				Row.Caps.bImageInput = Model.bImageInput.GetValue();
			}
			Rows.Add(MoveTemp(Row));
		}
		return Rows;
	}

	FString ExpandUrl(const FString& Template, const FString& BaseUrl)
	{
		FString Base = BaseUrl;
		Base.RemoveFromEnd(TEXT("/"));
		FString Origin = Base;
		const int32 Scheme = Base.Find(TEXT("://"));
		if (Scheme != INDEX_NONE)
		{
			const int32 PathStart = Base.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Scheme + 3);
			if (PathStart != INDEX_NONE)
			{
				Origin = Base.Left(PathStart);
			}
		}
		return Template.Replace(TEXT("{base}"), *Base).Replace(TEXT("{origin}"), *Origin);
	}

	FString HealthUrl(const FKismetAIProviderProfile& Profile, const FString& BaseUrl)
	{
		if (Profile.Id == TEXT("ollama"))
		{
			return ExpandUrl(TEXT("{base}/api/version"), BaseUrl);
		}
		if (!Profile.ModelsUrl.IsEmpty())
		{
			return ExpandUrl(Profile.ModelsUrl, BaseUrl);
		}
		return BaseUrl;
	}

	FString NotRunningMessage(const FKismetAIProviderProfile& Profile, const FString& BaseUrl)
	{
		if (Profile.Id == TEXT("ollama"))
		{
			return FString::Printf(TEXT("Ollama not running at %s. Start it (open the Ollama app or run `ollama serve`), or point KismetAI at it with /provider ollama <model> <url>."), *BaseUrl);
		}
		if (Profile.Id == TEXT("lmstudio"))
		{
			return FString::Printf(TEXT("LM Studio server not running at %s. Start the local server in LM Studio (Developer tab, or `lms server start`), or set its address with /provider lmstudio <model> <url>."), *BaseUrl);
		}
		if (BaseUrl.IsEmpty())
		{
			return FString::Printf(TEXT("%s has no base URL. Set one with /provider %s <model> <url>."), *Profile.DisplayName, *Profile.Id);
		}
		return FString::Printf(TEXT("No server answering at %s (%s). Check that it is running and that the base URL is right (/provider %s <model> <url>)."),
			*BaseUrl, *Profile.DisplayName, *Profile.Id);
	}

	bool IsToolsUnsupportedMessage(const FString& Message)
	{
		const FString Lower = Message.ToLower();
		return Lower.Contains(TEXT("does not support tool"))
			|| Lower.Contains(TEXT("tools are not supported"))
			|| Lower.Contains(TEXT("tool use is not supported"))
			|| Lower.Contains(TEXT("tool calling is not supported"))
			|| Lower.Contains(TEXT("function calling is not supported"))
			|| Lower.Contains(TEXT("tool_choice\" is not supported"))
			|| Lower.Contains(TEXT("\"auto\" tool choice requires"));
	}

	FString ToolsUnsupportedMessage(const FString& ProviderName, const FString& ModelId)
	{
		return FString::Printf(TEXT("%s on %s does not support tool calls, which KismetAI needs to read and edit Blueprints. Pick a model with tool support (in Ollama, the model's library page lists \"tools\"; for example qwen3 or llama3.1), or enable tool calling on the server."),
			*ModelId, *ProviderName);
	}

	bool PreflightLocal(const FKismetAIAgentConfig& Config, const FKismetAIProviderProfile& Profile,
		IKismetAIHttpFetcher& Fetcher, FString& OutErrorType, FString& OutMessage, const FKismetAIModelCatalog& Catalog)
	{
		if (!KismetAIProviders::IsSelfHosted(Profile))
		{
			return true;
		}
		const FString BaseUrl = KismetAIProviders::GetBaseUrl(Config, Profile);
		if (BaseUrl.IsEmpty())
		{
			OutErrorType = TEXT("local_server_unreachable");
			OutMessage = NotRunningMessage(Profile, BaseUrl);
			return false;
		}
		FKismetAIHttpRequest Probe;
		Probe.Url = HealthUrl(Profile, BaseUrl);
		Probe.Headers = KismetAIProviders::BuildModelsHeaders(Config, Profile);
		Probe.TimeoutSeconds = 3;
		if (!Fetcher.Send(Probe).Answered())
		{
			OutErrorType = TEXT("local_server_unreachable");
			OutMessage = NotRunningMessage(Profile, BaseUrl);
			return false;
		}
		const FKismetAIModelEntry* Entry = Catalog.Find(Config.Model);
		if (Entry && Entry->bRuntime && !Entry->Caps.bToolCalls)
		{
			OutErrorType = TEXT("tools_unsupported");
			OutMessage = ToolsUnsupportedMessage(Profile.DisplayName, Config.Model);
			return false;
		}
		return true;
	}
}
