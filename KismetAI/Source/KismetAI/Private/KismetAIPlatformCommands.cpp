// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIPlatformCommands.h"
#include "KismetAIModule.h"
#include "KismetAIAgent.h"
#include "KismetAISkill.h"
#include "KismetAIToolRegistry.h"
#include "KismetAIAuth.h"
#include "KismetAIPaths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"

namespace
{
	FString SerializePlatformJson(const TSharedPtr<FJsonObject>& Object)
	{
		FString Json;
		FJsonSerializer::Serialize(Object.ToSharedRef(), TJsonWriterFactory<>::Create(&Json));
		return Json;
	}
	TSharedPtr<FJsonObject> ReadObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
		return Object;
	}
	FString Failure(const FString& Error)
	{
		const auto Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("error"), Error);
		return SerializePlatformJson(Object);
	}
	TSharedPtr<FJsonObject> SkillInfo(const FKismetAISkill& Skill)
	{
		const auto Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("id"), Skill.Id());
		Object->SetStringField(TEXT("description"), Skill.Description);
		Object->SetStringField(TEXT("sha256"), Skill.Sha256);
		return Object;
	}
	bool RegistryId(const FString& Id, bool bVersion)
	{
		if (Id.Len() > 150) return false;
		const FString Pattern = TEXT("^[a-z0-9]+(-[a-z0-9]+)*/[a-z0-9]+(-[a-z0-9]+)*") +
			FString(bVersion ? TEXT("/[0-9]+\\.[0-9]+\\.[0-9]+$") : TEXT("$"));
		return FRegexMatcher(FRegexPattern(Pattern), Id).FindNext();
	}
}

FString KismetAIPlatformCommands::Local(const FString& Action, const FString& Payload)
{
	check(IsInGameThread());
	auto& Module = FKismetAIModule::Get();
	FKismetAIAgentConfig Config = Module.GetAgentConfig();
	if (Action == TEXT("state"))
	{
		const auto Object = MakeShared<FJsonObject>();
		Object->SetBoolField(TEXT("provider_confirmation_required"), Config.bProviderConfirmationRequired);
		Object->SetStringField(TEXT("selected"), Config.SelectedSkill);
		Object->SetStringField(TEXT("plugin_version"), KismetAISkills::PluginVersion());
		Object->SetStringField(TEXT("engine_version"), KismetAISkills::EngineVersion());
		Object->SetStringField(TEXT("backend"), KismetAIAuth::GetBackendUrl());
		TArray<TSharedPtr<FJsonValue>> Installed, Tools;
		for (const auto& Skill : KismetAISkills::List(KismetAISkills::DefaultRoot())) Installed.Add(MakeShared<FJsonValueObject>(SkillInfo(Skill)));
		TArray<FString> Names = FKismetAIToolRegistry::GetAllNames().Array();
		Names.Sort();
		for (const FString& Name : Names) Tools.Add(MakeShared<FJsonValueString>(Name));
		Object->SetArrayField(TEXT("installed"), Installed);
		Object->SetArrayField(TEXT("tools"), Tools);
		return SerializePlatformJson(Object);
	}
	if (Action == TEXT("route")) return Failure(TEXT("Kismet-hosted inference routing has been retired. Use your own provider connection."));
	FString Error;
	FKismetAISkill Skill;
	if (Action == TEXT("preview") || Action == TEXT("import"))
	{
		if (!KismetAISkills::Parse(Payload, KismetAISkills::PluginVersion(), KismetAISkills::EngineVersion(), Skill, Error)) return Failure(Error);
		if (Action == TEXT("import") && !KismetAISkills::Install(KismetAISkills::DefaultRoot(), Skill, Error)) return Failure(Error);
		const auto Object = SkillInfo(Skill);
		Object->SetStringField(TEXT("artifact"), Skill.Json);
		return SerializePlatformJson(Object);
	}
	if (Action == TEXT("read") || Action == TEXT("export"))
	{
		if (!KismetAISkills::Load(KismetAISkills::DefaultRoot(), Payload, Skill, Error)) return Failure(Error);
		const auto Object = SkillInfo(Skill);
		Object->SetStringField(TEXT("artifact"), Skill.Json);
		if (Action == TEXT("export"))
		{
			const FString Directory = KismetAIPaths::GetConfigDir() / TEXT("exports");
			IFileManager::Get().MakeDirectory(*Directory, true);
			const FString Path = Directory / (Skill.Publisher + TEXT("-") + Skill.Name + TEXT("-") + Skill.Version + TEXT(".json"));
			if (!FFileHelper::SaveStringToFile(Skill.Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return Failure(TEXT("Could not export the package."));
			Object->SetStringField(TEXT("path"), Path);
		}
		return SerializePlatformJson(Object);
	}
	if (Action == TEXT("select") || Action == TEXT("confirm_provider") || Action == TEXT("remove"))
	{
		if (Module.IsAgentRunning()) return Failure(TEXT("Stop the current run before changing skills or confirming a provider connection."));
		if (Action == TEXT("remove"))
		{
			if (!KismetAISkills::Remove(KismetAISkills::DefaultRoot(), Payload, Error)) return Failure(Error);
			if (Config.SelectedSkill == Payload) Config.SelectedSkill.Empty();
		}
		else if (Action == TEXT("select"))
		{
			if (!Payload.IsEmpty() && !KismetAISkills::Load(KismetAISkills::DefaultRoot(), Payload, Skill, Error)) return Failure(Error);
			Config.SelectedSkill = Payload;
		}
		else
		{
			Config.bProviderConfirmationRequired = false;
		}

		Module.SetAgentConfig(Config);
		Module.SaveSettings();
		Module.BroadcastJS(TEXT("window.onSkillsChanged && window.onSkillsChanged()"));
		return TEXT("{\"ok\":true}");
	}
	return Failure(TEXT("Unknown skill command."));
}

void KismetAIPlatformCommands::Registry(const FString& Action, const FString& Payload, TFunction<void(const FString&)> OnComplete)
{
	check(IsInGameThread());
	auto Finish = [Action, OnComplete](const FString& Json)
	{
		auto Object = ReadObject(Json);
		if (!Object.IsValid()) Object = ReadObject(Failure(TEXT("The registry returned invalid JSON.")));
		Object->SetStringField(TEXT("action"), Action);
		OnComplete(SerializePlatformJson(Object));
	};
	FString Url = KismetAIAuth::GetBackendUrl() + TEXT("/api/skills");
	FString Verb = TEXT("GET"), Body, Id, ExpectedHash;
	const bool bArtifact = Action == TEXT("preview") || Action == TEXT("install");
	const bool bAuthenticated = Action == TEXT("publish") || Action == TEXT("publisher");
	if (Action == TEXT("browse")) Url += TEXT("?q=") + FGenericPlatformHttp::UrlEncode(Payload.Left(200));
	else if (Action == TEXT("versions"))
	{
		if (!RegistryId(Payload, false)) { Finish(Failure(TEXT("Invalid publisher/skill identity."))); return; }
		Url += TEXT("/") + Payload;
	}
	else if (bArtifact)
	{
		const auto Object = ReadObject(Payload);
		if (!Object.IsValid() || !Object->TryGetStringField(TEXT("id"), Id) || !RegistryId(Id, true) ||
			!Object->TryGetStringField(TEXT("sha256"), ExpectedHash) ||
			!FRegexMatcher(FRegexPattern(TEXT("^[a-f0-9]{64}$")), ExpectedHash).FindNext())
		{ Finish(Failure(TEXT("Select a registry version and its SHA256 before downloading."))); return; }
		Url += TEXT("/") + Id;
	}
	else if (Action == TEXT("publish"))
	{
		FKismetAISkill Skill;
		FString Error;
		if (!KismetAISkills::Parse(Payload, KismetAISkills::PluginVersion(), KismetAISkills::EngineVersion(), Skill, Error)) { Finish(Failure(Error)); return; }
		Verb = TEXT("POST"); Body = Payload;
	}
	else if (Action == TEXT("publisher"))
	{
		if (Payload.Len() > 2048 || !ReadObject(Payload).IsValid()) { Finish(Failure(TEXT("Invalid publisher details."))); return; }
		Url += TEXT("/publisher"); Verb = TEXT("POST"); Body = Payload;
	}
	else if (Action == TEXT("quota"))
	{
		Finish(Failure(TEXT("Kismet-hosted inference and its quota endpoint have been retired.")));
		return;
	}
	else { Finish(Failure(TEXT("Unknown registry command."))); return; }

	const TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(Verb);
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetTimeout(30.0f);
	Request->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnGameThread);
	if (bAuthenticated)
	{
		FKismetAIAuthSession Session;
		if (!KismetAIAuth::LoadSession(Session) || !Session.IsValid()) { Finish(Failure(TEXT("Sign in to your Kismet account to publish skills or claim a publisher."))); return; }
		Request->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + Session.Token);
	}
	if (Verb == TEXT("POST")) Request->SetContentAsString(Body);
	const uint64 MaxBytes = bArtifact ? 262144 : 1048576;
	Request->OnRequestProgress64().BindLambda([MaxBytes](FHttpRequestPtr InRequest, uint64, uint64 Received)
	{
		if (Received > MaxBytes) InRequest->CancelRequest();
	});
	Request->OnProcessRequestComplete().BindLambda([Finish, Action, Id, ExpectedHash, bArtifact, MaxBytes](FHttpRequestPtr, FHttpResponsePtr Response, bool bOk)
	{
		if (!bOk || !Response.IsValid()) { Finish(Failure(TEXT("Kismet service unavailable or response too large. Installed skills remain available locally."))); return; }
		if (Response->GetContent().Num() > MaxBytes) { Finish(Failure(TEXT("Registry response exceeds the size limit."))); return; }
		if (Response->GetResponseCode() < 200 || Response->GetResponseCode() >= 300)
		{
			const auto Object = ReadObject(Response->GetContentAsString());
			FString Detail;
			if (Object.IsValid()) Object->TryGetStringField(TEXT("error"), Detail);
			Finish(Failure(FString::Printf(TEXT("Kismet service HTTP %d: %s"), Response->GetResponseCode(), *Detail.Left(500)))); return;
		}
		if (bArtifact)
		{
			FKismetAISkill Skill;
			FString Error;
			if (!KismetAISkills::Parse(Response->GetContentAsString(), KismetAISkills::PluginVersion(), KismetAISkills::EngineVersion(), Skill, Error)) { Finish(Failure(Error)); return; }
			if (Skill.Id() != Id || Skill.Sha256 != ExpectedHash || Response->GetHeader(TEXT("X-Kismet-Skill-SHA256")) != ExpectedHash)
			{ Finish(Failure(TEXT("The downloaded skill identity or SHA256 differs from the selected version."))); return; }
			if (Action == TEXT("install") && !KismetAISkills::Install(KismetAISkills::DefaultRoot(), Skill, Error)) { Finish(Failure(Error)); return; }
			const auto Object = SkillInfo(Skill);
			Object->SetStringField(TEXT("artifact"), Skill.Json);
			Finish(SerializePlatformJson(Object));
		}
		else Finish(Response->GetContentAsString());
	});
	if (!Request->ProcessRequest()) Finish(Failure(TEXT("Could not start the registry request.")));
}
