// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIModelCatalog.h"
#include "KismetAIEmbeddedConfig.h"
#include "KismetAILog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogKismetAIModelCatalog, Log, All);

namespace
{
	const TCHAR* const CatalogFileName = TEXT("models.json");

	FKismetAIModelEntry UnknownEntry(const FKismetAIModelEntry& Defaults)
	{
		FKismetAIModelEntry Entry = Defaults;
		Entry.Caps = FKismetAIModelCapabilities();
		Entry.Caps.bThinking = false;
		Entry.Caps.bBudgetThinking = false;
		Entry.Caps.bSummarizedThinkingDisplay = false;
		Entry.Caps.bSamplingParams = false;
		Entry.Caps.bLegacyThinkingBeta = false;
		return Entry;
	}


	int32 EffortRank(const FString& Level)
	{
		static const TCHAR* const Order[] = { TEXT("low"), TEXT("medium"), TEXT("high"), TEXT("xhigh"), TEXT("max") };
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Order); ++Index)
		{
			if (Level.Equals(Order[Index], ESearchCase::IgnoreCase))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	bool ReadPrice(const FJsonObject& Obj, const TCHAR* Field, FKismetAIModelPrice& Out)
	{
		const TSharedPtr<FJsonObject>* PriceObj = nullptr;
		if (!Obj.TryGetObjectField(Field, PriceObj) || !PriceObj || !PriceObj->IsValid())
		{
			return false;
		}
		(*PriceObj)->TryGetNumberField(TEXT("input"), Out.Input);
		(*PriceObj)->TryGetNumberField(TEXT("output"), Out.Output);
		(*PriceObj)->TryGetNumberField(TEXT("cache_read"), Out.CacheRead);
		(*PriceObj)->TryGetNumberField(TEXT("cache_write"), Out.CacheWrite);
		return true;
	}

	void ReadStringArray(const FJsonObject& Obj, const TCHAR* Field, TArray<FString>& Out, bool bLowercase)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Obj.TryGetArrayField(Field, Values) || !Values)
		{
			return;
		}
		Out.Reset();
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Str;
			if (Value.IsValid() && Value->TryGetString(Str) && !Str.IsEmpty())
			{
				Out.Add(bLowercase ? Str.ToLower() : Str);
			}
		}
	}

	bool ParseFastModeWire(const FString& Str, EKismetAIFastModeWire& Out)
	{
		if (Str == TEXT("none"))                 { Out = EKismetAIFastModeWire::None; return true; }
		if (Str == TEXT("anthropic_speed"))      { Out = EKismetAIFastModeWire::AnthropicSpeed; return true; }
		if (Str == TEXT("openai_priority"))      { Out = EKismetAIFastModeWire::OpenAIPriority; return true; }
		if (Str == TEXT("model_swap"))           { Out = EKismetAIFastModeWire::ModelSwap; return true; }
		return false;
	}

	bool ReadCapabilities(const FJsonObject& Obj, FKismetAIModelCapabilities& Caps, FString& OutError)
	{
		const TSharedPtr<FJsonObject>* CapsObjPtr = nullptr;
		if (!Obj.TryGetObjectField(TEXT("capabilities"), CapsObjPtr) || !CapsObjPtr || !CapsObjPtr->IsValid())
		{
			return true;
		}
		const FJsonObject& C = **CapsObjPtr;
		C.TryGetBoolField(TEXT("thinking"), Caps.bThinking);
		C.TryGetBoolField(TEXT("adaptive_thinking"), Caps.bAdaptiveThinking);
		C.TryGetBoolField(TEXT("budget_thinking"), Caps.bBudgetThinking);
		C.TryGetBoolField(TEXT("thinking_always_on"), Caps.bThinkingAlwaysOn);
		C.TryGetBoolField(TEXT("summarized_thinking_display"), Caps.bSummarizedThinkingDisplay);
		C.TryGetBoolField(TEXT("sampling_params"), Caps.bSamplingParams);
		C.TryGetBoolField(TEXT("legacy_thinking_beta"), Caps.bLegacyThinkingBeta);
		C.TryGetBoolField(TEXT("effort_beta"), Caps.bEffortBeta);
		C.TryGetBoolField(TEXT("mid_conversation_system"), Caps.bMidConversationSystem);
		C.TryGetBoolField(TEXT("thinking_block_binding"), Caps.bThinkingBlockBinding);
		C.TryGetBoolField(TEXT("reasoning"), Caps.bReasoning);
		C.TryGetBoolField(TEXT("image_input"), Caps.bImageInput);
		C.TryGetBoolField(TEXT("tool_calls"), Caps.bToolCalls);
		C.TryGetStringField(TEXT("fast_mode_model"), Caps.FastModeModelId);

		if (C.HasField(TEXT("effort")))
		{
			ReadStringArray(C, TEXT("effort"), Caps.EffortLevels, /*bLowercase=*/true);
			for (const FString& Level : Caps.EffortLevels)
			{
				if (EffortRank(Level) == INDEX_NONE)
				{
					OutError = FString::Printf(TEXT("unknown effort level '%s'"), *Level);
					return false;
				}
			}
		}

		FString FastWire;
		if (C.TryGetStringField(TEXT("fast_mode"), FastWire) && !ParseFastModeWire(FastWire, Caps.FastModeWire))
		{
			OutError = FString::Printf(TEXT("unknown fast_mode '%s'"), *FastWire);
			return false;
		}
		if (Caps.FastModeWire == EKismetAIFastModeWire::ModelSwap && Caps.FastModeModelId.IsEmpty())
		{
			OutError = TEXT("fast_mode 'model_swap' needs fast_mode_model");
			return false;
		}
		return true;
	}

	bool PatternMatches(const FString& Pattern, const FString& LowerName)
	{
		if (Pattern.StartsWith(TEXT("^")))
		{
			return LowerName.StartsWith(Pattern.RightChop(1));
		}
		return LowerName.Contains(Pattern);
	}

	int32 PatternLength(const FString& Pattern)
	{
		return Pattern.StartsWith(TEXT("^")) ? Pattern.Len() - 1 : Pattern.Len();
	}
}

FKismetAIModelCatalog::FKismetAIModelCatalog()
	: UnknownDefaults(UnknownEntry(Defaults))
{
}

bool FKismetAIModelCatalog::ParseEntry(const FJsonObject& Obj, const FKismetAIModelEntry& Base, FKismetAIModelEntry& Out, FString& OutError)
{
	Out = Base;  // capabilities, context and price fall back to the defaults
	Out.Id.Empty();
	Out.Ids.Empty();
	Out.Alias.Empty();
	Out.Aliases.Empty();
	Out.MatchPatterns.Empty();
	Out.DisplayName.Empty();
	Out.Tier.Empty();
	Out.RecommendedEffort.Empty();
	Out.FastPrice.Reset();
	Out.LongContextPrice.Reset();
	Out.LongContextThreshold = 0;
	Out.Provider.Empty();
	Out.MaxOutputTokens = 0;
	Out.bPatternOnly = false;
	Out.OpenRouterId.Empty();
	Out.DerivedFrom.Empty();
	Out.bRuntime = false;
	Out.ContextSource = TEXT("catalog");

	Obj.TryGetBoolField(TEXT("pattern_only"), Out.bPatternOnly);
	Obj.TryGetStringField(TEXT("id"), Out.Id);
	if (!Out.bPatternOnly && Out.Id.IsEmpty())
	{
		OutError = TEXT("entry has no id and is not pattern_only");
		return false;
	}

	ReadStringArray(Obj, TEXT("ids"), Out.Ids, /*bLowercase=*/false);
	Obj.TryGetStringField(TEXT("alias"), Out.Alias);
	ReadStringArray(Obj, TEXT("aliases"), Out.Aliases, /*bLowercase=*/false);
	ReadStringArray(Obj, TEXT("match"), Out.MatchPatterns, /*bLowercase=*/true);
	if (Out.bPatternOnly && Out.MatchPatterns.Num() == 0)
	{
		OutError = TEXT("pattern_only entry has no match patterns");
		return false;
	}
	// A real model always matches its own ids as substrings, so dated snapshots
	// and platform-prefixed ids (anthropic.claude-...) reach it.
	if (!Out.bPatternOnly)
	{
		Out.MatchPatterns.AddUnique(Out.Id.ToLower());
		for (const FString& Other : Out.Ids)
		{
			Out.MatchPatterns.AddUnique(Other.ToLower());
		}
	}

	Obj.TryGetStringField(TEXT("display_name"), Out.DisplayName);
	Obj.TryGetStringField(TEXT("provider"), Out.Provider);
	Obj.TryGetStringField(TEXT("tier"), Out.Tier);
	Obj.TryGetStringField(TEXT("recommended_effort"), Out.RecommendedEffort);
	Out.bInPicker = false;
	Obj.TryGetBoolField(TEXT("picker"), Out.bInPicker);
	if (Out.bPatternOnly && Out.bInPicker)
	{
		OutError = TEXT("pattern_only entry cannot be in the picker");
		return false;
	}
	Obj.TryGetNumberField(TEXT("context_window"), Out.ContextWindow);
	Obj.TryGetNumberField(TEXT("max_output_tokens"), Out.MaxOutputTokens);

	ReadPrice(Obj, TEXT("pricing"), Out.Price);
	FKismetAIModelPrice Fast;
	if (ReadPrice(Obj, TEXT("fast_pricing"), Fast))
	{
		Out.FastPrice = Fast;
	}
	const TSharedPtr<FJsonObject>* LongObj = nullptr;
	if (Obj.TryGetObjectField(TEXT("long_context"), LongObj) && LongObj && LongObj->IsValid())
	{
		FKismetAIModelPrice Long;
		(*LongObj)->TryGetNumberField(TEXT("threshold_tokens"), Out.LongContextThreshold);
		(*LongObj)->TryGetNumberField(TEXT("input"), Long.Input);
		(*LongObj)->TryGetNumberField(TEXT("output"), Long.Output);
		(*LongObj)->TryGetNumberField(TEXT("cache_read"), Long.CacheRead);
		(*LongObj)->TryGetNumberField(TEXT("cache_write"), Long.CacheWrite);
		if (Out.LongContextThreshold <= 0)
		{
			OutError = TEXT("long_context needs a positive threshold_tokens");
			return false;
		}
		Out.LongContextPrice = Long;
	}

	if (!ReadCapabilities(Obj, Out.Caps, OutError))
	{
		return false;
	}
	Obj.TryGetStringField(TEXT("openrouter_id"), Out.OpenRouterId);
	if (!Out.OpenRouterId.IsEmpty() && (Out.bPatternOnly || !Out.OpenRouterId.Contains(TEXT("/"))))
	{
		OutError = TEXT("openrouter_id must be a vendor/model id on a real model");
		return false;
	}
	if (Out.FastPrice.IsSet() != (Out.Caps.FastModeWire != EKismetAIFastModeWire::None))
	{
		OutError = TEXT("fast_pricing and capabilities.fast_mode must be set together");
		return false;
	}
	return true;
}

bool FKismetAIModelCatalog::DeriveOpenRouterEntry(const FJsonObject& Obj, const FKismetAIModelEntry& BaseEntry, FKismetAIModelEntry& Out, FString& OutError)
{
	// The same model through OpenRouter's Chat Completions endpoint: one key
	// reaches every vendor. Prices and limits are the base model's (OpenRouter
	// passes vendor prices through; the live list replaces them when it loads).
	// The request shape is OpenAI Chat's, so the vendor's own wire features do
	// not carry over: no fast tier, no Anthropic thinking fields, and effort is
	// sent as reasoning_effort in the levels OpenRouter normalises.
	Out = BaseEntry;
	Out.Id = BaseEntry.OpenRouterId;
	Out.Ids.Reset();
	Out.Alias = Out.Id;          // the picker selects rows by alias
	Out.Aliases.Reset();
	Out.MatchPatterns = { Out.Id.ToLower() };
	Out.Provider = TEXT("openrouter");
	Out.DerivedFrom = BaseEntry.Id;
	Out.OpenRouterId.Empty();
	Out.FastPrice.Reset();
	Out.bInPicker = BaseEntry.bInPicker;
	Obj.TryGetBoolField(TEXT("openrouter_picker"), Out.bInPicker);
	Obj.TryGetStringField(TEXT("openrouter_name"), Out.DisplayName);
	ReadPrice(Obj, TEXT("openrouter_pricing"), Out.Price);

	FKismetAIModelCapabilities& Caps = Out.Caps;
	const bool bHadEffort = Caps.EffortLevels.Num() > 0 || Caps.bReasoning;
	TArray<FString> Levels;
	for (const FString& Level : Caps.EffortLevels)
	{
		if (Level == TEXT("low") || Level == TEXT("medium") || Level == TEXT("high"))
		{
			Levels.Add(Level);
		}
	}
	if (bHadEffort && Levels.Num() == 0)
	{
		Levels = { TEXT("low"), TEXT("medium"), TEXT("high") };
	}
	Caps.bReasoning = bHadEffort;
	Caps.EffortLevels = Levels;
	Caps.bEffortBeta = false;
	Caps.bThinking = false;
	Caps.bAdaptiveThinking = false;
	Caps.bBudgetThinking = false;
	Caps.bThinkingAlwaysOn = false;
	Caps.bSummarizedThinkingDisplay = false;
	Caps.bLegacyThinkingBeta = false;
	Caps.bMidConversationSystem = false;
	Caps.bThinkingBlockBinding = false;
	Caps.FastModeWire = EKismetAIFastModeWire::None;
	Caps.FastModeModelId.Empty();
	// "openrouter_capabilities" overrides the derived shape where OpenRouter
	// accepts more than the vendor's own endpoint documents (GLM effort).
	const TSharedPtr<FJsonObject>* OverrideCaps = nullptr;
	if (Obj.TryGetObjectField(TEXT("openrouter_capabilities"), OverrideCaps) && OverrideCaps && OverrideCaps->IsValid())
	{
		TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
		Wrapper->SetObjectField(TEXT("capabilities"), *OverrideCaps);
		if (!ReadCapabilities(*Wrapper, Caps, OutError))
		{
			OutError = TEXT("openrouter_capabilities: ") + OutError;
			return false;
		}
		Levels = Caps.EffortLevels;
	}
	if (Out.RecommendedEffort.Len() > 0 && !Levels.Contains(Out.RecommendedEffort))
	{
		Out.RecommendedEffort = Levels.Num() > 0 ? Levels.Last() : FString();
	}
	if (Out.DisplayName.IsEmpty())
	{
		OutError = TEXT("derived OpenRouter row has no display name");
		return false;
	}
	return true;
}

bool FKismetAIModelCatalog::LoadFromJson(const FString& Json, FString& OutError)
{
	Entries.Reset();
	Defaults = FKismetAIModelEntry();
	UnknownDefaults = UnknownEntry(Defaults);
	bLoaded = false;

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = TEXT("models.json is not valid JSON");
		return false;
	}

	int32 SchemaVersion = 0;
	Root->TryGetNumberField(TEXT("schema_version"), SchemaVersion);
	if (SchemaVersion != 1)
	{
		OutError = FString::Printf(TEXT("unsupported schema_version %d"), SchemaVersion);
		return false;
	}

	const TSharedPtr<FJsonObject>* DefaultsObj = nullptr;
	if (Root->TryGetObjectField(TEXT("defaults"), DefaultsObj) && DefaultsObj && DefaultsObj->IsValid())
	{
		Defaults.bPatternOnly = true;
		(*DefaultsObj)->TryGetNumberField(TEXT("context_window"), Defaults.ContextWindow);
		ReadPrice(**DefaultsObj, TEXT("pricing"), Defaults.Price);
		if (!ReadCapabilities(**DefaultsObj, Defaults.Caps, OutError))
		{
			OutError = TEXT("defaults: ") + OutError;
			return false;
		}
	}

	UnknownDefaults = UnknownEntry(Defaults);

	const TArray<TSharedPtr<FJsonValue>>* Models = nullptr;
	if (!Root->TryGetArrayField(TEXT("models"), Models) || !Models)
	{
		OutError = TEXT("models.json has no models array");
		return false;
	}

	TSet<FString> SeenNames;
	TArray<FKismetAIModelEntry> Parsed;
	TArray<FKismetAIModelEntry> Derived;
	for (int32 Index = 0; Index < Models->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject>* ModelObj = nullptr;
		if (!(*Models)[Index]->TryGetObject(ModelObj) || !ModelObj || !ModelObj->IsValid())
		{
			OutError = FString::Printf(TEXT("models[%d] is not an object"), Index);
			return false;
		}
		FKismetAIModelEntry Entry;
		FString EntryError;
		if (!ParseEntry(**ModelObj, Defaults, Entry, EntryError))
		{
			OutError = FString::Printf(TEXT("models[%d] (%s): %s"), Index, *Entry.Id, *EntryError);
			return false;
		}

		TArray<FKismetAIModelEntry*> ToCheck = { &Entry };
		FKismetAIModelEntry DerivedEntry;
		if (!Entry.OpenRouterId.IsEmpty())
		{
			if (!DeriveOpenRouterEntry(**ModelObj, Entry, DerivedEntry, EntryError))
			{
				OutError = FString::Printf(TEXT("models[%d] (%s): %s"), Index, *Entry.Id, *EntryError);
				return false;
			}
			ToCheck.Add(&DerivedEntry);
		}

		// Every id and alias names exactly one model, or lookups would depend on order.
		for (FKismetAIModelEntry* Checked : ToCheck)
		{
			if (Checked->bPatternOnly)
			{
				continue;
			}
			TArray<FString> Names = Checked->Ids;
			Names.Add(Checked->Id);
			Names.Append(Checked->Aliases);
			if (!Checked->Alias.IsEmpty())
			{
				Names.Add(Checked->Alias);
			}
			// An alias that repeats the entry's own id (MiniMax-M3 / minimax-m3) is fine.
			TSet<FString> EntryNames;
			for (const FString& Name : Names)
			{
				EntryNames.Add(Name.ToLower());
			}
			for (const FString& Name : EntryNames)
			{
				bool bAlreadySeen = false;
				SeenNames.Add(Name, &bAlreadySeen);
				if (bAlreadySeen)
				{
					OutError = FString::Printf(TEXT("models[%d]: '%s' is already used by another model"), Index, *Name);
					return false;
				}
			}
		}
		Parsed.Add(MoveTemp(Entry));
		if (!DerivedEntry.Id.IsEmpty())
		{
			Derived.Add(MoveTemp(DerivedEntry));
		}
	}

	// Derived OpenRouter rows open the OpenRouter picker group, so the group
	// stays contiguous and OpenRouter-only models (Grok, Qwen) follow them.
	if (Derived.Num() > 0)
	{
		int32 InsertAt = INDEX_NONE;
		int32 LastPicker = INDEX_NONE;
		for (int32 Index = 0; Index < Parsed.Num(); ++Index)
		{
			if (Parsed[Index].bInPicker)
			{
				LastPicker = Index;
				if (InsertAt == INDEX_NONE && Parsed[Index].Provider == TEXT("openrouter"))
				{
					InsertAt = Index;
				}
			}
		}
		if (InsertAt == INDEX_NONE)
		{
			InsertAt = LastPicker == INDEX_NONE ? Parsed.Num() : LastPicker + 1;
		}
		Parsed.Insert(MoveTemp(Derived), InsertAt);
	}

	Entries = MoveTemp(Parsed);
	bLoaded = true;
	return true;
}

const FKismetAIModelCatalog& FKismetAIModelCatalog::Get()
{
	return GetMutable();
}

FKismetAIModelCatalog& FKismetAIModelCatalog::GetMutable()
{
	// Built in place: the runtime overlay's lock makes the catalog immovable.
	static FKismetAIModelCatalog Catalog;
	static const bool bLoadedOnce = [](FKismetAIModelCatalog& Loaded)
	{

		FString DiskPath;
		if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("KismetAI")))
		{
			DiskPath = Plugin->GetBaseDir() / TEXT("Config") / CatalogFileName;
		}

		FString Json;
		FString Error;
		if (!KismetAIEmbeddedConfig::LoadConfigFile(CatalogFileName, DiskPath, Json))
		{
			KISMETAI_LOG(LogKismetAIModelCatalog, Error,
				TEXT("Model catalog not found (%s). Every model uses conservative unknown capabilities and assumed defaults."), *DiskPath);
		}
		else if (!Loaded.LoadFromJson(Json, Error))
		{
			KISMETAI_LOG(LogKismetAIModelCatalog, Error,
				TEXT("Model catalog is invalid: %s. Every model uses conservative unknown capabilities and assumed defaults."), *Error);
		}
		return true;
	}(Catalog);
	(void)bLoadedOnce;
	return Catalog;
}

const FKismetAIModelEntry* FKismetAIModelCatalog::FindRuntime(const FString& LowerId) const
{
	FReadScopeLock Lock(RuntimeLock);
	const FKismetAIModelEntry* const* Found = RuntimeById.Find(LowerId);
	return Found ? *Found : nullptr;
}

bool FKismetAIModelCatalog::HasRuntimeEntry(const FString& ModelId) const
{
	return FindRuntime(ModelId.TrimStartAndEnd().ToLower()) != nullptr;
}

void FKismetAIModelCatalog::SetRuntimeEntries(const FString& OwnerProvider, const TArray<FKismetAIModelEntry>& InEntries)
{
	FWriteScopeLock Lock(RuntimeLock);
	if (TArray<FString>* Previous = RuntimeIdsByOwner.Find(OwnerProvider))
	{
		for (const FString& Id : *Previous)
		{
			const FKismetAIModelEntry* const* Current = RuntimeById.Find(Id);
			if (Current && (*Current)->Provider == OwnerProvider)
			{
				RuntimeById.Remove(Id);
			}
		}
	}
	TArray<FString>& Owned = RuntimeIdsByOwner.FindOrAdd(OwnerProvider);
	Owned.Reset();
	for (const FKismetAIModelEntry& In : InEntries)
	{
		if (In.Id.IsEmpty() || In.bPatternOnly)
		{
			continue;
		}
		TUniquePtr<FKismetAIModelEntry> Stored = MakeUnique<FKismetAIModelEntry>(In);
		Stored->bRuntime = true;
		Stored->Provider = OwnerProvider;
		const FString Lower = Stored->Id.ToLower();
		RuntimeById.Add(Lower, Stored.Get());
		Owned.AddUnique(Lower);
		RuntimeStorage.Add(MoveTemp(Stored));
	}
}

void FKismetAIModelCatalog::ClearRuntimeEntries(const FString& OwnerProvider)
{
	SetRuntimeEntries(OwnerProvider, TArray<FKismetAIModelEntry>());
}

const FKismetAIModelEntry* FKismetAIModelCatalog::Find(const FString& ModelName) const
{
	const FString Lower = ModelName.TrimStartAndEnd().ToLower();
	if (Lower.IsEmpty())
	{
		return nullptr;
	}
	if (const FKismetAIModelEntry* Runtime = FindRuntime(Lower))
	{
		return Runtime;
	}

	for (const FKismetAIModelEntry& Entry : Entries)
	{
		if (Entry.bPatternOnly)
		{
			continue;
		}
		if (Entry.Id.ToLower() == Lower || Entry.Alias.ToLower() == Lower)
		{
			return &Entry;
		}
		for (const FString& Other : Entry.Ids)
		{
			if (Other.ToLower() == Lower) return &Entry;
		}
		for (const FString& Other : Entry.Aliases)
		{
			if (Other.ToLower() == Lower) return &Entry;
		}
	}

	const FKismetAIModelEntry* Best = nullptr;
	int32 BestLength = 0;
	for (const FKismetAIModelEntry& Entry : Entries)
	{
		for (const FString& Pattern : Entry.MatchPatterns)
		{
			const int32 Length = PatternLength(Pattern);
			if (Length > BestLength && PatternMatches(Pattern, Lower))
			{
				Best = &Entry;
				BestLength = Length;
			}
		}
	}
	return Best;
}

const FKismetAIModelEntry& FKismetAIModelCatalog::FindOrDefaults(const FString& ModelName) const
{
	const FKismetAIModelEntry* Found = Find(ModelName);
	// Aggregators prefix the vendor ("openai/gpt-6.1-sol", "z-ai/glm-5.3").
	// The capabilities belong to the model, so retry with the bare id.
	int32 Slash = INDEX_NONE;
	if (!Found && ModelName.FindLastChar(TEXT('/'), Slash) && Slash + 1 < ModelName.Len())
	{
		Found = Find(ModelName.RightChop(Slash + 1));
	}
	return Found ? *Found : UnknownDefaults;
}

bool FKismetAIModelCatalog::IsKnownContextSuffix(const FString& ModelName) const
{
	if (!ModelName.EndsWith(TEXT("-1m"))) return false;
	const FString BaseId = Resolve(ModelName.LeftChop(3));
	const FKismetAIModelEntry* Base = BaseId.IsEmpty() ? nullptr : Find(BaseId);
	return Base && !Base->bPatternOnly && Base->ContextWindow >= 1000000;
}

FString FKismetAIModelCatalog::Resolve(const FString& AliasOrId) const
{
	const FString Lower = AliasOrId.TrimStartAndEnd().ToLower();
	if (Lower.IsEmpty())
	{
		return FString();
	}
	for (const FKismetAIModelEntry& Entry : Entries)
	{
		if (Entry.bPatternOnly)
		{
			continue;
		}
		if (Entry.Id.ToLower() == Lower || Entry.Alias.ToLower() == Lower)
		{
			return Entry.Id;
		}
		for (const FString& Other : Entry.Aliases)
		{
			if (Other.ToLower() == Lower) return Entry.Id;
		}
		// A full id in "ids" is sent as written: claude-haiku-4-5 stays claude-haiku-4-5.
		for (const FString& Other : Entry.Ids)
		{
			if (Other.ToLower() == Lower) return Other;
		}
	}
	// A live or discovered model resolves to itself once its provider listed it.
	if (const FKismetAIModelEntry* Runtime = FindRuntime(Lower))
	{
		return Runtime->Id;
	}
	return FString();
}

TArray<const FKismetAIModelEntry*> FKismetAIModelCatalog::GetPickerModels() const
{
	TArray<const FKismetAIModelEntry*> Result;
	for (const FKismetAIModelEntry& Entry : Entries)
	{
		if (Entry.bInPicker)
		{
			Result.Add(&Entry);
		}
	}
	return Result;
}

FString FKismetAIModelCatalog::ClampEffort(const TArray<FString>& Supported, const FString& Requested)
{
	if (Supported.Num() == 0)
	{
		return FString();
	}
	const int32 RequestedRank = EffortRank(Requested);
	FString Best;
	int32 BestRank = INDEX_NONE;
	FString Lowest;
	int32 LowestRank = MAX_int32;
	for (const FString& Level : Supported)
	{
		const int32 Rank = EffortRank(Level);
		if (Rank == INDEX_NONE)
		{
			continue;
		}
		if (Rank <= RequestedRank && Rank > BestRank)
		{
			Best = Level;
			BestRank = Rank;
		}
		if (Rank < LowestRank)
		{
			Lowest = Level;
			LowestRank = Rank;
		}
	}
	return BestRank != INDEX_NONE ? Best : Lowest;
}

FKismetAIModelPrice FKismetAIModelCatalog::PriceFor(const FKismetAIModelEntry& Entry, bool bFastMode, int32 PromptTokens)
{
	if (bFastMode && Entry.FastPrice.IsSet())
	{
		return Entry.FastPrice.GetValue();
	}
	if (Entry.LongContextPrice.IsSet() && Entry.LongContextThreshold > 0 && PromptTokens > Entry.LongContextThreshold)
	{
		return Entry.LongContextPrice.GetValue();
	}
	return Entry.Price;
}
