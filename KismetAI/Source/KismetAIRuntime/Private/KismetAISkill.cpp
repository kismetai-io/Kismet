// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAISkill.h"
#include "KismetAIPaths.h"
#include "KismetAIToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "HAL/FileManager.h"
#include "Internationalization/Regex.h"
#include "Runtime/Launch/Resources/Version.h"
THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END
#if PLATFORM_MAC || PLATFORM_LINUX
#include <sys/stat.h>
#include <errno.h>
#elif PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace
{
	bool Matches(const FString& Value, const TCHAR* Pattern)
	{
		return FRegexMatcher(FRegexPattern(Pattern), Value).FindNext();
	}
	bool Slug(const FString& Value)
	{
		return Value.Len() <= 64 && Matches(Value, TEXT("^[a-z0-9]+(-[a-z0-9]+)*$"));
	}
	bool Version(const FString& Value, int32 Parts, uint64& Out)
	{
		TArray<FString> Values;
		Value.ParseIntoArray(Values, TEXT("."), false);
		Out = 0;
		if (Values.Num() != Parts) return false;
		for (const FString& Part : Values)
		{
			if (Part.Len() > 5 || !Matches(Part, TEXT("^(0|[1-9][0-9]*)$"))) return false;
			const int32 Number = FCString::Atoi(*Part);
			if (Number > 65535) return false;
			Out = (Out << 16) | Number;
		}
		return true;
	}
	bool SafeId(const FString& Id)
	{
		TArray<FString> Parts;
		Id.ParseIntoArray(Parts, TEXT("/"), false);
		uint64 Number;
		return Parts.Num() == 3 && Slug(Parts[0]) && Slug(Parts[1]) && Version(Parts[2], 3, Number);
	}
	// UE's Apple/Unix IsSymlink uses stat(), which follows the link. Inspect
	// each entry itself instead. Root ancestors are trusted configuration;
	// the store root and every package entry beneath it must be ordinary paths.
	bool PlainStoreEntry(const FString& Path, bool bDirectory)
	{
		FString FullPath = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeDirectoryName(FullPath);
#if PLATFORM_MAC || PLATFORM_LINUX
		struct stat Entry;
		const FTCHARToUTF8 PathUtf8(*FullPath);
		if (::lstat(PathUtf8.Get(), &Entry) != 0) return errno == ENOENT;
		return bDirectory ? S_ISDIR(Entry.st_mode) : S_ISREG(Entry.st_mode);
#elif PLATFORM_WINDOWS
		const uint32 Attributes = ::GetFileAttributesW(*FullPath);
		if (Attributes == INVALID_FILE_ATTRIBUTES)
		{
			const uint32 Error = ::GetLastError();
			return Error == ERROR_FILE_NOT_FOUND || Error == ERROR_PATH_NOT_FOUND;
		}
		if (Attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) return false;
		return ((Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == bDirectory;
#else
		return false;
#endif
	}
	bool StorePath(const FString& Root, const FString& Id, FString& OutPath, FString& Error)
	{
		if (!SafeId(Id)) { Error = TEXT("Invalid publisher/name/version identity."); return false; }
		FString FullRoot = FPaths::ConvertRelativePathToFull(Root);
		FPaths::NormalizeDirectoryName(FullRoot);
		OutPath = FullRoot / (Id + TEXT(".json"));
		const FString SkillDirectory = FPaths::GetPath(OutPath);
		if (!PlainStoreEntry(FullRoot, true) || !PlainStoreEntry(FPaths::GetPath(SkillDirectory), true) ||
			!PlainStoreEntry(SkillDirectory, true) || !PlainStoreEntry(OutPath, false))
		{
			Error = TEXT("Skill store paths must be ordinary directories and files. Symbolic links and reparse points are refused.");
			return false;
		}
		return true;
	}
	bool TextValue(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, int32 MaxBytes, FString& Out)
	{
		const auto Value = Object->TryGetField(Key);
		if (!Value.IsValid() || Value->Type != EJson::String || !Object->TryGetStringField(Key, Out) || Out.TrimStartAndEnd().IsEmpty()) return false;
		for (TCHAR Char : Out) { if (Char == 0) return false; }
		return FTCHARToUTF8(*Out).Length() <= MaxBytes;
	}
	bool SafeResource(const FString& Path)
	{
		if (Path.Len() > 180) return false;
		TArray<FString> Parts;
		Path.ParseIntoArray(Parts, TEXT("/"), false);
		for (const FString& Part : Parts)
		{
			if (!Matches(Part, TEXT("^[a-zA-Z0-9][a-zA-Z0-9._-]*$")) || Part.EndsWith(TEXT(".")) ||
				Matches(Part.ToLower(), TEXT("^(con|prn|aux|nul|com[0-9]|lpt[0-9])(\\.|$)"))) return false;
		}
		const TSet<FString> Extensions = {TEXT("md"), TEXT("txt"), TEXT("json"), TEXT("yaml"), TEXT("yml"), TEXT("csv")};
		return Extensions.Contains(FPaths::GetExtension(Path).ToLower());
	}
	bool UniqueKeys(const FString& Json)
	{
		TArray<TSet<FString>> Keys;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		EJsonNotation Token;
		while (Reader->ReadNext(Token))
		{
			if (Token == EJsonNotation::Error) return false;
			if (Token == EJsonNotation::ObjectEnd || Token == EJsonNotation::ArrayEnd)
			{
				if (Keys.IsEmpty()) return false;
				Keys.Pop();
				continue;
			}
			const FString Key = Reader->GetIdentifier();
			if (!Keys.IsEmpty() && !Key.IsEmpty())
			{
				if (Keys.Last().Contains(Key)) return false;
				Keys.Last().Add(Key);
			}
			if (Token == EJsonNotation::ObjectStart || Token == EJsonNotation::ArrayStart) Keys.AddDefaulted();
		}
		return Keys.IsEmpty();
	}
	FString JsonString(const TSharedPtr<FJsonObject>& Object)
	{
		FString Result;
		FJsonSerializer::Serialize(Object.ToSharedRef(), TJsonWriterFactory<>::Create(&Result));
		return Result;
	}
}

bool KismetAISkills::Parse(const FString& Json, const FString& Plugin, const FString& Engine, FKismetAISkill& Out, FString& Error)
{
	Out = FKismetAISkill();
	Error = TEXT("Invalid skill package: check schema, size, tools, resource paths and compatibility.");
	const FTCHARToUTF8 Bytes(*Json);
	if (Bytes.Length() > 262144 || !UniqueKeys(Json)) return false;
	TSharedPtr<FJsonObject> Object;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object) || !Object.IsValid()) return false;
	const TSet<FString> Fields = {TEXT("schema_version"), TEXT("publisher"), TEXT("name"), TEXT("version"), TEXT("description"), TEXT("license"), TEXT("kismet_min_version"), TEXT("engine_versions"), TEXT("required_tools"), TEXT("instructions"), TEXT("resources")};
	if (Object->Values.Num() != Fields.Num()) return false;
	for (const auto& Pair : Object->Values) { if (!Fields.Contains(FString(*Pair.Key))) return false; }
	double Schema = 0;
	FString License, Minimum;
	if (Object->GetField<EJson::None>(TEXT("schema_version"))->Type != EJson::Number || !Object->TryGetNumberField(TEXT("schema_version"), Schema) || Schema != 1 ||
		!TextValue(Object, TEXT("publisher"), 64, Out.Publisher) || !Slug(Out.Publisher) ||
		!TextValue(Object, TEXT("name"), 64, Out.Name) || !Slug(Out.Name) ||
		!TextValue(Object, TEXT("version"), 17, Out.Version) ||
		!TextValue(Object, TEXT("description"), 500, Out.Description) ||
		!TextValue(Object, TEXT("license"), 128, License) ||
		!TextValue(Object, TEXT("instructions"), 65536, Out.Instructions) ||
		!TextValue(Object, TEXT("kismet_min_version"), 17, Minimum)) return false;
	uint64 VersionNumber, MinimumNumber, PluginNumber;
	if (!Version(Out.Version, 3, VersionNumber) || !Version(Minimum, 3, MinimumNumber) || !Version(Plugin, 3, PluginNumber) || MinimumNumber > PluginNumber) return false;
	const TArray<TSharedPtr<FJsonValue>>* Engines;
	const TArray<TSharedPtr<FJsonValue>>* Tools;
	const TSharedPtr<FJsonObject>* Resources;
	if (!Object->TryGetArrayField(TEXT("engine_versions"), Engines) || Engines->Num() < 1 || Engines->Num() > 32 ||
		!Object->TryGetArrayField(TEXT("required_tools"), Tools) ||
		!Object->TryGetObjectField(TEXT("resources"), Resources) || (*Resources)->Values.Num() > 32) return false;
	TSet<FString> SeenEngines;
	for (const auto& Item : *Engines)
	{
		FString Value;
		if (Item->Type != EJson::String || !Item->TryGetString(Value) || !Version(Value, 2, VersionNumber) || SeenEngines.Contains(Value)) return false;
		SeenEngines.Add(Value);
	}
	if (!SeenEngines.Contains(Engine)) return false;
	for (const auto& Item : *Tools)
	{
		FString Name;
		if (Item->Type != EJson::String || !Item->TryGetString(Name) || !FKismetAIToolRegistry::Find(Name) || Out.RequiredTools.Contains(Name)) return false;
		Out.RequiredTools.Add(Name);
	}
	TSet<FString> SeenPaths;
	for (const auto& Pair : (*Resources)->Values)
	{
		FString Content;
		const FString Path(*Pair.Key);
		if (!SafeResource(Path) || SeenPaths.Contains(Path.ToLower()) || Pair.Value->Type != EJson::String || !Pair.Value->TryGetString(Content)) return false;
		for (TCHAR Char : Content) { if (Char == 0) return false; }
		SeenPaths.Add(Path.ToLower());
		Out.Instructions += TEXT("\n\nResource: ") + Path + TEXT("\n") + Content;
	}
	uint8 Digest[SHA256_DIGEST_LENGTH];
	SHA256(reinterpret_cast<const unsigned char*>(Bytes.Get()), Bytes.Length(), Digest);
	Out.Sha256 = BytesToHex(Digest, SHA256_DIGEST_LENGTH).ToLower();
	Out.Json = Json;
	Error.Empty();
	return true;
}

FString KismetAISkills::DefaultRoot() { return KismetAIPaths::GetConfigDir() / TEXT("skills"); }
FString KismetAISkills::PluginVersion()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("KismetAI"));
	return Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : TEXT("0.9.1");
}
FString KismetAISkills::EngineVersion() { return FString::Printf(TEXT("%d.%d"), ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION); }

bool KismetAISkills::Load(const FString& Root, const FString& Id, FKismetAISkill& Out, FString& Error)
{
	Error = TEXT("Installed skill is missing, incompatible, or has changed. Import a verified version again.");
	FString Path;
	if (!StorePath(Root, Id, Path, Error)) return false;
	if (IFileManager::Get().FileSize(*Path) > 1048576) return false;
	FString Stored;
	TSharedPtr<FJsonObject> Object;
	FString Artifact, Hash;
	if (!FFileHelper::LoadFileToString(Stored, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Stored), Object) || !Object.IsValid() ||
		!Object->TryGetStringField(TEXT("artifact"), Artifact) || !Object->TryGetStringField(TEXT("sha256"), Hash)) return false;
	if (!Parse(Artifact, PluginVersion(), EngineVersion(), Out, Error)) return false;
	if (Out.Id() != Id || Out.Sha256 != Hash) { Error = TEXT("Installed skill identity or SHA256 mismatch."); return false; }
	return true;
}

bool KismetAISkills::Install(const FString& Root, const FKismetAISkill& Skill, FString& Error)
{
	FKismetAISkill Checked;
	if (!Parse(Skill.Json, PluginVersion(), EngineVersion(), Checked, Error)) return false;
	FString Path;
	if (!StorePath(Root, Checked.Id(), Path, Error)) return false;
	if (IFileManager::Get().FileExists(*Path))
	{
		FKismetAISkill Existing;
		if (!Load(Root, Checked.Id(), Existing, Error)) return false;
		if (Existing.Sha256 == Checked.Sha256) return true;
		Error = TEXT("This version already exists with different content. Increase the version.");
		return false;
	}
	TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
	Envelope->SetStringField(TEXT("artifact"), Checked.Json);
	Envelope->SetStringField(TEXT("sha256"), Checked.Sha256);
	if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true) || !StorePath(Root, Checked.Id(), Path, Error))
	{
		if (Error.IsEmpty()) Error = TEXT("Could not create the skill store directory.");
		return false;
	}
	const FString Temporary = Path + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
	const bool bSaved = FFileHelper::SaveStringToFile(JsonString(Envelope), *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	if (bSaved && IFileManager::Get().Move(*Path, *Temporary, false, false, false, true)) return true;
	IFileManager::Get().Delete(*Temporary);
	Error = TEXT("Could not atomically save the skill package.");
	return false;
}

TArray<FKismetAISkill> KismetAISkills::List(const FString& Root)
{
	TArray<FKismetAISkill> Result;
	if (!PlainStoreEntry(Root, true)) return Result;
	TArray<FString> Publishers;
	IFileManager::Get().FindFiles(Publishers, *(Root / TEXT("*")), false, true);
	for (const FString& Publisher : Publishers)
	{
		const FString PublisherPath = Root / Publisher;
		if (!Slug(Publisher) || !PlainStoreEntry(PublisherPath, true)) continue;
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *(PublisherPath / TEXT("*")), false, true);
		for (const FString& Name : Names)
		{
			const FString SkillPath = PublisherPath / Name;
			if (!Slug(Name) || !PlainStoreEntry(SkillPath, true)) continue;
			TArray<FString> Versions;
			IFileManager::Get().FindFiles(Versions, *(SkillPath / TEXT("*.json")), true, false);
			for (FString Entry : Versions)
			{
				Entry.RemoveFromEnd(TEXT(".json"));
				FKismetAISkill Skill;
				FString Error;
				if (Load(Root, Publisher / Name / Entry, Skill, Error)) Result.Add(MoveTemp(Skill));
			}
		}
	}
	Result.Sort([](const FKismetAISkill& A, const FKismetAISkill& B) { return A.Id() < B.Id(); });
	return Result;
}

bool KismetAISkills::Remove(const FString& Root, const FString& Id, FString& Error)
{
	Error = TEXT("Choose an installed publisher/name/version to remove.");
	FString Path;
	if (!StorePath(Root, Id, Path, Error)) return false;
	if (!IFileManager::Get().FileExists(*Path) || !IFileManager::Get().Delete(*Path, true, false, true))
	{
		Error = TEXT("The selected skill version is missing or could not be removed.");
		return false;
	}
	Error.Empty();
	return true;
}
