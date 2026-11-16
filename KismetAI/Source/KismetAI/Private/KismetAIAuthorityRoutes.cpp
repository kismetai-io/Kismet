// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Scoped authority in the editor: resolving targets without loading,
// the default panel grant, the /api/v1/authority routes and the
// KismetAI.Authority.* console commands. The model is in KismetAIAuthority.h.

#include "KismetAIModule.h"
#include "KismetAIAuthority.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/IConsoleManager.h"
#include "HttpServerRequest.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogKismetAIAuthorityEditor, Log, All);

// ─────────────────────────────────────────────────────────────────────────────
// Target resolution — what the handler would act on, found without loading
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	bool PackageKnown(IAssetRegistry* AssetRegistry, const FString& PackagePath)
	{
		if (FindPackage(nullptr, *PackagePath))
		{
			return true;
		}
		if (AssetRegistry)
		{
			TArray<FAssetData> Assets;
			AssetRegistry->GetAssetsByPackageName(FName(*PackagePath), Assets, /*bIncludeOnlyOnDiskAssets=*/false);
			if (Assets.Num() > 0)
			{
				return true;
			}
		}
		return FPackageName::IsValidLongPackageName(PackagePath) && FPackageName::DoesPackageExist(PackagePath);
	}
}

TArray<FString> FKismetAIModule::ResolveAuthorityTarget(const FKismetAIAuthorityTarget& Target)
{
	check(IsInGameThread());

	FAssetRegistryModule* ARModule = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry* AssetRegistry = ARModule ? &ARModule->Get() : nullptr;

	FString PackagePath;
	FString Leaf;
	const bool bAbsolute = KismetAIAuthority::NormalizeAssetReference(Target.Raw, PackagePath, &Leaf);

	// The handlers' lookup order (KismetAIUtils::FindBlueprint and friends):
	// the path as given, the path without a generated-class "_C", then a unique
	// asset with that leaf name anywhere.
	if (bAbsolute)
	{
		if (PackageKnown(AssetRegistry, PackagePath))
		{
			return { PackagePath };
		}
		if (PackagePath.EndsWith(TEXT("_C")))
		{
			const FString Stripped = PackagePath.LeftChop(2);
			if (PackageKnown(AssetRegistry, Stripped))
			{
				return { Stripped };
			}
			Leaf.RemoveFromEnd(TEXT("_C"));
		}
	}
	else
	{
		Leaf.RemoveFromEnd(TEXT("_C"));
	}

	if (Leaf.IsEmpty() || !AssetRegistry)
	{
		return {};
	}

	// Every asset with that leaf name. The handler filters by class and refuses
	// an ambiguous match; authority does not know which class it will accept, so
	// every candidate has to be covered.
	TArray<FString> Candidates;
	AssetRegistry->EnumerateAllAssets([&Candidates, &Leaf](const FAssetData& Asset)
	{
		if (Asset.AssetName.ToString().Equals(Leaf, ESearchCase::IgnoreCase))
		{
			Candidates.AddUnique(Asset.PackageName.ToString());
		}
		return true;
	});
	return Candidates;
}

void FKismetAIModule::InstallDefaultAuthority()
{
	FKismetAIAuthorityStore& Store = GetAuthorityStore();
	for (const FKismetAIAuthorityGrant& G : Store.GetGrants())
	{
		if (G.Label == PanelDefaultGrantLabel)
		{
			return;
		}
	}

	// The editor user drives the panel and watches it work, so ordinary edits
	// and new assets in the project's own content need no prompt. Deleting,
	// renaming or moving an asset, and anything outside /Game, asks.
	FKismetAIAuthorityGrant G;
	G.Label = PanelDefaultGrantLabel;
	G.CreatedBy = TEXT("default");
	G.EntryPoints = { EKismetAIEntryPoint::Panel };
	G.PathScopes = { TEXT("/Game") };
	G.Operations = EKismetAIAuthorityOp::Edit | EKismetAIAuthorityOp::Create;
	G.Lifetime = EKismetAIGrantLifetime::UntilRevoked;
	FString Error;
	if (Store.AddGrant(MoveTemp(G), Error).IsEmpty())
	{
		UE_LOG(LogKismetAIAuthorityEditor, Error, TEXT("Could not install the default panel grant: %s"), *Error);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// /api/v1/authority
// ─────────────────────────────────────────────────────────────────────────────

bool FKismetAIModule::HandleAuthorityList(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	SendSuccess(OnComplete, GetAuthorityStore().ToJson());
	return true;
}

bool FKismetAIModule::HandleAuthorityGrant(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	const TSharedPtr<FJsonObject> Body = ParseRequestBody(Request);
	if (!Body.IsValid())
	{
		SendError(OnComplete, 400, TEXT("INVALID_JSON"), TEXT("Request body must be a JSON object"));
		return true;
	}

	FKismetAIAuthorityGrant Grant;
	FString Error;
	if (!FKismetAIAuthorityGrant::FromJson(*Body, Grant, Error))
	{
		SendError(OnComplete, 400, TEXT("INVALID_GRANT"), Error,
			{ TEXT("Example: {\"entry_points\":[\"mcp\"],\"operations\":[\"edit\",\"create\"],\"scope\":[\"/Game/Sandbox\"]}") });
		return true;
	}
	// The panel's authority is the editor user's to give, and in-process code
	// needs none.
	for (EKismetAIEntryPoint E : Grant.EntryPoints)
	{
		if (E != EKismetAIEntryPoint::Rest && E != EKismetAIEntryPoint::Mcp && E != EKismetAIEntryPoint::HttpAgentRun)
		{
			SendError(OnComplete, 403, TEXT("ENTRY_POINT_NOT_GRANTABLE"),
				FString::Printf(TEXT("'%s' authority cannot be granted over REST; use KismetAI.Authority.Grant in the editor."), LexToString(E)));
			return true;
		}
	}
	if (Grant.Lifetime == EKismetAIGrantLifetime::Session && Grant.SessionId.IsEmpty())
	{
		SendError(OnComplete, 400, TEXT("INVALID_GRANT"), TEXT("A session grant needs 'session_id'."));
		return true;
	}
	Grant.CreatedBy = TEXT("x-kismet-token");

	const FString Id = GetAuthorityStore().AddGrant(Grant, Error);
	if (Id.IsEmpty())
	{
		SendError(OnComplete, 400, TEXT("INVALID_GRANT"), Error);
		return true;
	}
	TOptional<FKismetAIAuthorityGrant> Stored = GetAuthorityStore().FindGrant(Id);
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetObjectField(TEXT("grant"), Stored.IsSet() ? Stored->ToJson() : MakeShared<FJsonObject>());
	SendSuccess(OnComplete, Data);
	return true;
}

bool FKismetAIModule::HandleAuthorityRevoke(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	const TSharedPtr<FJsonObject> Body = ParseRequestBody(Request);
	if (!Body.IsValid())
	{
		SendError(OnComplete, 400, TEXT("INVALID_JSON"), TEXT("Request body must be a JSON object"));
		return true;
	}
	bool bAll = false;
	Body->TryGetBoolField(TEXT("all"), bAll);
	FString Id;
	Body->TryGetStringField(TEXT("grant_id"), Id);
	if (!bAll && Id.IsEmpty())
	{
		SendError(OnComplete, 400, TEXT("MISSING_FIELD"), TEXT("Pass 'grant_id' (an id or label) or {\"all\": true}."));
		return true;
	}
	const int32 Revoked = bAll
		? GetAuthorityStore().RevokeAll(TEXT("x-kismet-token"))
		: GetAuthorityStore().Revoke(Id, TEXT("x-kismet-token"));
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("revoked"), Revoked);
	SendSuccess(OnComplete, Data);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Console: the editor user's controls
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	FKismetAIModule* LoadedModule()
	{
		return FModuleManager::GetModulePtr<FKismetAIModule>(TEXT("KismetAI"));
	}

	void LogGrants(FKismetAIModule& Module)
	{
		const TArray<FKismetAIAuthorityGrant> Grants = Module.GetAuthorityStore().GetGrants();
		UE_LOG(LogKismetAIAuthorityEditor, Display, TEXT("%d active grant(s):"), Grants.Num());
		for (const FKismetAIAuthorityGrant& G : Grants)
		{
			FString Json;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
			FJsonSerializer::Serialize(G.ToJson(), Writer);
			UE_LOG(LogKismetAIAuthorityEditor, Display, TEXT("  %s"), *Json);
		}
	}

	FAutoConsoleCommand AuthorityListCommand(
		TEXT("KismetAI.Authority.List"),
		TEXT("List KismetAI's active editing grants (who may change which assets)."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			if (FKismetAIModule* Module = LoadedModule())
			{
				LogGrants(*Module);
			}
		}));

	FAutoConsoleCommand AuthorityGrantCommand(
		TEXT("KismetAI.Authority.Grant"),
		TEXT("KismetAI.Authority.Grant <entry_points> <operations> <scopes> [label]\n")
		TEXT("  entry_points: comma list of panel, mcp, rest, http_agent_run\n")
		TEXT("  operations:   comma list of edit, create, delete\n")
		TEXT("  scopes:       comma list of asset folders, e.g. /Game/Sandbox (\"/\" = every mount)\n")
		TEXT("Example: KismetAI.Authority.Grant mcp edit,create /Game/Sandbox"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			FKismetAIModule* Module = LoadedModule();
			if (!Module)
			{
				return;
			}
			if (Args.Num() < 3)
			{
				UE_LOG(LogKismetAIAuthorityEditor, Warning, TEXT("Usage: KismetAI.Authority.Grant <entry_points> <operations> <scopes> [label]"));
				return;
			}
			auto Split = [](const FString& S)
			{
				TArray<FString> Out;
				S.ParseIntoArray(Out, TEXT(","), true);
				return Out;
			};
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> EP, Ops, Scopes;
			for (const FString& S : Split(Args[0])) EP.Add(MakeShared<FJsonValueString>(S));
			for (const FString& S : Split(Args[1])) Ops.Add(MakeShared<FJsonValueString>(S));
			for (const FString& S : Split(Args[2])) Scopes.Add(MakeShared<FJsonValueString>(S));
			Json->SetArrayField(TEXT("entry_points"), EP);
			Json->SetArrayField(TEXT("operations"), Ops);
			Json->SetArrayField(TEXT("scope"), Scopes);
			if (Args.Num() > 3)
			{
				Json->SetStringField(TEXT("label"), Args[3]);
			}
			FKismetAIAuthorityGrant Grant;
			FString Error;
			if (!FKismetAIAuthorityGrant::FromJson(*Json, Grant, Error))
			{
				UE_LOG(LogKismetAIAuthorityEditor, Warning, TEXT("Grant refused: %s"), *Error);
				return;
			}
			Grant.CreatedBy = TEXT("editor-user");
			const FString Id = Module->GetAuthorityStore().AddGrant(Grant, Error);
			if (Id.IsEmpty())
			{
				UE_LOG(LogKismetAIAuthorityEditor, Warning, TEXT("Grant refused: %s"), *Error);
				return;
			}
			UE_LOG(LogKismetAIAuthorityEditor, Display, TEXT("Granted %s"), *Id);
		}));

	FAutoConsoleCommand AuthorityRevokeCommand(
		TEXT("KismetAI.Authority.Revoke"),
		TEXT("KismetAI.Authority.Revoke <grant id | label | all> — revoke KismetAI editing grants. Calls already queued are refused at admission."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			FKismetAIModule* Module = LoadedModule();
			if (!Module || Args.Num() < 1)
			{
				UE_LOG(LogKismetAIAuthorityEditor, Warning, TEXT("Usage: KismetAI.Authority.Revoke <grant id | label | all>"));
				return;
			}
			const int32 Revoked = Args[0] == TEXT("all")
				? Module->GetAuthorityStore().RevokeAll(TEXT("editor-user"))
				: Module->GetAuthorityStore().Revoke(Args[0], TEXT("editor-user"));
			UE_LOG(LogKismetAIAuthorityEditor, Display, TEXT("Revoked %d grant(s)."), Revoked);
		}));
}
