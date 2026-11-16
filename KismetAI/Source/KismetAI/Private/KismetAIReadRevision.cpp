// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIReadRevision.h"
#include "KismetAIEngineCompat.h"
#include "KismetAIAssetState.h"
#include "KismetAIAuthority.h"
#include "KismetAIUtils.h"
#include "KismetAIToolRegistry.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

namespace KismetAIReadRevision
{
	namespace
	{
		TSet<FName> ActivePackages;
		TMap<TWeakObjectPtr<UBlueprint>, FGuid> Incarnations;

		FString PinTypeEvidence(UPackage* Package, const FKismetAIAssetFingerprint& State)
		{
			TArray<FString> Types;
			ForEachObjectWithPackage(Package, [&](UObject* Object)
			{
				const UEdGraphNode* Node = Cast<UEdGraphNode>(Object);
				if (!Node || !State.ObjectDigests.Contains(Node->GetPathName(Package))) return true;
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin) continue;
					FString Type;
					// Pins are native rather than reflected node properties. Export the
					// entire type, including map value/member/qualifier fields, as paths
					// and values; never change the durable fingerprint format.
					FEdGraphPinType::StaticStruct()->ExportText(Type, &Pin->PinType, nullptr, nullptr, PPF_None, nullptr);
					Types.Add(Node->GetPathName(Package) + TEXT("/") + Pin->PinId.ToString() + TEXT("=") + Type);
				}
				return true;
			}, KISMETAI_INCLUDE_NESTED_OBJECTS);
			Types.Sort();
			return FString::Join(Types, TEXT("\n"));
		}

		FString Revision(UBlueprint* Blueprint)
		{
			for (auto It = Incarnations.CreateIterator(); It; ++It)
			{
				if (!It.Key().IsValid()) It.RemoveCurrent();
			}
			const TWeakObjectPtr<UBlueprint> Key(Blueprint);
			FGuid* Identity = Incarnations.Find(Key);
			if (!Identity) Identity = &Incarnations.Add(Key, FGuid::NewGuid());
			const auto State = KismetAIAssetState::CaptureLoadedPackage(Blueprint->GetOutermost(), Blueprint->GetOutermost()->GetName());
			if (!State.bCaptured || !State.bSupported) return FString();
			const FString Evidence = Identity->ToString() + TEXT("\n") + Blueprint->GetPathName() + TEXT("\n") + State.Digest
				+ TEXT("\n") + PinTypeEvidence(Blueprint->GetOutermost(), State);
			FTCHARToUTF8 Utf8(*Evidence);
			uint8 Hash[FSHA1::DigestSize];
			FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Hash);
			return TEXT("bp1:") + BytesToHex(Hash, FSHA1::DigestSize).ToLower();
		}

		FToolExecutionResult Conflict()
		{
			return FToolExecutionResult::NotStartedError(409, TEXT("ASSET_REVISION_CONFLICT"),
				TEXT("The Blueprint changed or was replaced since the observed revision. This call was not applied."),
				{ TEXT("Read the asset again, preserve intervening changes, and plan a new operation using its asset_revision. Do not retry the old operation or omit its expectation.") });
		}
	}

	FToolExecutionResult Run(const FString& ToolName, const TSharedPtr<FJsonObject>& Body,
		const TMap<FString, FString>& ObservedRevisions, bool bRequireObservation,
		const TFunction<FToolExecutionResult(const TSharedPtr<FJsonObject>&)>& Work)
	{
		check(IsInGameThread());
		const TCHAR* Field = FKismetAIToolRegistry::RevisionTargetField(ToolName);
		FString Expected;
		const bool bExplicit = Body && Body->HasField(TEXT("expected_revision"));
		if (bExplicit && (!Body->HasTypedField<EJson::String>(TEXT("expected_revision")) || !Body->TryGetStringField(TEXT("expected_revision"), Expected) || Expected.IsEmpty()))
			return FToolExecutionResult::NotStartedError(400, TEXT("VALIDATION_ERROR"), TEXT("expected_revision must be a nonempty string."));
		if (bExplicit && !Field)
			return FToolExecutionResult::NotStartedError(400, TEXT("REVISION_UNSUPPORTED"), TEXT("This tool does not support Blueprint revision preconditions."));

		// Other asset mutators participate in exclusivity even when they do not
		// expose the Blueprint revision contract (e.g. rename/delete/save).
		TArray<FString> Packages;
		for (const auto& Target : KismetAIAuthority::DescribeOperation(ToolName, Body).Targets)
		{
			if (Target.IsResolved()) Packages.AddUnique(Target.PackagePath);
		}
		FString RawPath;
		if (Field && Body) Body->TryGetStringField(Field, RawPath);
		FString Normalized;
		if (!RawPath.IsEmpty() && KismetAIAuthority::NormalizeAssetReference(RawPath, Normalized)) Packages.AddUnique(Normalized);
		for (const FString& Package : Packages)
		{
			if (ActivePackages.Contains(FName(*Package)))
				return FToolExecutionResult::NotStartedError(409, TEXT("ASSET_BUSY"), TEXT("Another tool is executing on this asset. Read it again after that operation finishes."));
		}

		if (ToolName == TEXT("manage_asset") && Body)
		{
			FString Source;
			if (Body->TryGetStringField(TEXT("asset_path"), Source))
			{
				if (UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *Source))
					Packages.AddUnique(Asset->GetOutermost()->GetName());
			}
		}
		UBlueprint* Blueprint = Field && !RawPath.IsEmpty() ? KismetAIUtils::FindBlueprint(RawPath) : nullptr;
		TSharedPtr<FJsonObject> CanonicalBody = Body;
		if (Blueprint)
		{
			const FString CanonicalPath = Blueprint->GetPathName();
			Packages.AddUnique(Blueprint->GetOutermost()->GetName());
			CanonicalBody = MakeShared<FJsonObject>();
			CanonicalBody->Values = Body->Values;
			CanonicalBody->SetStringField(Field, CanonicalPath);
			if (!bExplicit)
			{
				const FString* Observed = ObservedRevisions.Find(CanonicalPath);
				if (!Observed) Observed = ObservedRevisions.Find(RawPath);
				if (Observed) Expected = *Observed;
			}
		}
		else if (!bExplicit)
		{
			if (const FString* Observed = ObservedRevisions.Find(RawPath)) Expected = *Observed;
		}
		const bool bRead = ToolName == TEXT("read_blueprint") || ToolName == TEXT("read_widget")
			|| !KismetAIAuthority::DescribeOperation(ToolName, CanonicalBody).RequiresAuthority();
		// Fresh reads must remain possible after a conflict. Mixed read-only
		// batches are classified by the same authority logic as admission.
		if (bRead && !bExplicit) Expected.Reset();
		for (const FString& Package : Packages)
		{
			if (ActivePackages.Contains(FName(*Package)))
				return FToolExecutionResult::NotStartedError(409, TEXT("ASSET_BUSY"), TEXT("Another tool is executing on this asset. Read it again after that operation finishes."));
		}
		for (const FString& Package : Packages) ActivePackages.Add(FName(*Package));
		ON_SCOPE_EXIT { for (const FString& Package : Packages) ActivePackages.Remove(FName(*Package)); };

		if (Field && !bRead && bRequireObservation && Expected.IsEmpty())
			return FToolExecutionResult::NotStartedError(409, TEXT("REVISION_REQUIRED"),
				TEXT("This resumed run has no current observation of the Blueprint. This call was not applied."),
				{ TEXT("Read the asset again and plan against that result, or supply a genuinely current expected_revision. Old or compacted history is not a current observation.") });
		if (!Expected.IsEmpty() && (!Blueprint || Expected != Revision(Blueprint))) return Conflict();
		FToolExecutionResult Result = Work(CanonicalBody);
		if (Result.bSuccess && Result.bOperationsSucceeded && Blueprint && IsValid(Blueprint))
		{
			const FString Token = Revision(Blueprint);
			if (!Token.IsEmpty())
			{
				if (!Result.Data) Result.Data = MakeShared<FJsonObject>();
				Result.Data->SetStringField(TEXT("asset_revision"), Token);
				Result.Data->SetStringField(TEXT("asset_revision_path"), Blueprint->GetPathName());
			}
		}
		return Result;
	}
}
