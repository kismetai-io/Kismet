// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIAssetState.h"
#include "KismetAIEngineCompat.h"
#include "DiffUtils.h"
#include "BlueprintActionDatabase.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialFunctionInterface.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"
#include "UObject/LinkerLoad.h"

DEFINE_LOG_CATEGORY_STATIC(LogKismetAIAssetState, Log, All);

namespace KismetAIAssetState
{
	namespace
	{
		bool IsVolatileProperty(const UObject* Object, const FProperty* Prop)
		{
			static const FName ErrorType(TEXT("ErrorType"));
			static const FName ErrorMsg(TEXT("ErrorMsg"));
			static const FName HasCompilerMessage(TEXT("bHasCompilerMessage"));
			static const FName StateId(TEXT("StateId"));
			static const FName LightingGuid(TEXT("LightingGuid"));
			// Derived from the opacity mask and recomputed on PostEditChange in UE 5.7.
			static const FName CanMaskedBeAssumedOpaque(TEXT("bCanMaskedBeAssumedOpaque"));
			const FName Name = Prop->GetFName();
			if (Object->IsA<UEdGraphNode>() && (Name == ErrorType || Name == ErrorMsg || Name == HasCompilerMessage))
			{
				return true;
			}
			if (Object->IsA<UMaterialInterface>() && (Name == StateId || Name == LightingGuid || Name == CanMaskedBeAssumedOpaque))
			{
				return true;
			}
			return false;
		}

		FString Normalize(const FString& Value, const FString& From, const FString& To)
		{
			return From.IsEmpty() || From == To ? Value : Value.Replace(*From, *To, ESearchCase::CaseSensitive);
		}

		void AddProperties(TArray<FString>& Lines, UObject* Object, const FString& From, const FString& To)
		{
			for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
			{
				const FProperty* Prop = *It;
				if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated) || IsVolatileProperty(Object, Prop))
				{
					continue;
				}
				for (int32 Index = 0; Index < Prop->ArrayDim; ++Index)
				{
					FString Value;
					Prop->ExportText_InContainer(Index, Value, Object, nullptr, Object, PPF_None);
					Lines.Add(FString::Printf(TEXT("prop/%s[%d]=%s"), *Prop->GetName(), Index, *Normalize(Value, From, To)));
				}
			}
		}

		void AddPins(TArray<FString>& Lines, const UEdGraphNode* Node, const FString& From, const FString& To)
		{
			TArray<FString> Order;
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin)
				{
					Order.Add(TEXT("null"));
					continue;
				}
				const FString Key = FString::Printf(TEXT("pin/%s"), *Pin->PinId.ToString());
				Order.Add(Pin->PinId.ToString());
				Lines.Add(Key + TEXT("/name=") + Pin->PinName.ToString());
				Lines.Add(Key + TEXT("/direction=") + (Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out")));
				Lines.Add(Key + FString::Printf(TEXT("/type=%s|%s|%s|%d|%d"),
					*Pin->PinType.PinCategory.ToString(), *Pin->PinType.PinSubCategory.ToString(),
					*Normalize(GetPathNameSafe(Pin->PinType.PinSubCategoryObject.Get()), From, To),
					(int32)Pin->PinType.ContainerType, Pin->PinType.bIsReference ? 1 : 0));
				Lines.Add(Key + TEXT("/default=") + Pin->DefaultValue);
				Lines.Add(Key + TEXT("/default_object=") + Normalize(GetPathNameSafe(Pin->DefaultObject), From, To));
				Lines.Add(Key + TEXT("/default_text=") + Pin->DefaultTextValue.ToString());
				Lines.Add(Key + FString::Printf(TEXT("/flags=hidden:%d,orphaned:%d"), Pin->bHidden ? 1 : 0, Pin->bOrphanedPin ? 1 : 0));
				TArray<FString> Links;
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					const UEdGraphNode* Owner = Linked ? Linked->GetOwningNodeUnchecked() : nullptr;
					Links.Add(FString::Printf(TEXT("%s:%s"), Owner ? *Owner->NodeGuid.ToString() : TEXT("?"),
						Linked ? *Linked->PinId.ToString() : TEXT("null")));
				}
				Links.Sort();
				Lines.Add(Key + TEXT("/links=") + FString::Join(Links, TEXT(",")));
			}
			Lines.Add(TEXT("pins=") + FString::Join(Order, TEXT(",")));
		}

		FString Sha1Hex(const FString& Text)
		{
			FTCHARToUTF8 Utf8(*Text);
			FSHA1 Sha;
			Sha.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
			Sha.Final();
			uint8 Hash[FSHA1::DigestSize];
			Sha.GetHash(Hash);
			return BytesToHex(Hash, FSHA1::DigestSize).ToLower();
		}

		void FillFile(FKismetAIAssetFingerprint& Out, const FString& PackagePath)
		{
			const FString File = PackageFilename(PackagePath);
			Out.bFileExists = !File.IsEmpty() && IFileManager::Get().FileExists(*File);
			if (Out.bFileExists && !HashFile(File, Out.FileSha1, Out.FileSize))
			{
				Out.FileSha1.Reset();
				Out.FileSize = -1;
			}
		}

		// A node removed from its graph (UEdGraph::RemoveNode) stays in the
		// package until it is collected, but nothing refers to it and a save
		// does not write it. It is not part of the asset's state; nor is
		// anything inside it.
		bool IsDetachedGraphObject(const UObject* Object)
		{
			for (const UObject* It = Object; It; It = It->GetOuter())
			{
				if (const UEdGraphNode* Node = Cast<UEdGraphNode>(It))
				{
					const UEdGraph* Graph = Cast<UEdGraph>(Node->GetOuter());
					if (!Graph || !Graph->Nodes.Contains(Node))
					{
						return true;
					}
				}
			}
			return false;
		}

		UObject* FindAsset(UPackage* Package)
		{
			return Package ? Package->FindAssetInPackage() : nullptr;
		}
	}

	TArray<FString> DescribeObject(UObject* Object, const FString& NormalizeFrom, const FString& NormalizeTo)
	{
		TArray<FString> Lines;
		if (!Object)
		{
			return Lines;
		}
		Lines.Add(TEXT("class=") + Normalize(Object->GetClass()->GetPathName(), NormalizeFrom, NormalizeTo));
		AddProperties(Lines, Object, NormalizeFrom, NormalizeTo);
		if (const UEdGraphNode* Node = Cast<UEdGraphNode>(Object))
		{
			AddPins(Lines, Node, NormalizeFrom, NormalizeTo);
		}
		Lines.Sort();
		return Lines;
	}

	FString UnsupportedReason(const UObject* Asset)
	{
		if (!Asset)
		{
			return TEXT("the package holds no asset");
		}
		if (Asset->IsA<UWorld>())
		{
			return TEXT("map packages are not supported (their actors live in other packages)");
		}
		if (Asset->IsA<UBlueprint>() || Asset->IsA<UMaterialInterface>() || Asset->IsA<UMaterialFunctionInterface>())
		{
			return FString();
		}
		return FString::Printf(TEXT("asset class %s is not described by this build"), *Asset->GetClass()->GetName());
	}

	bool HashFile(const FString& FilePath, FString& OutSha1, int64& OutSize)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *FilePath, FILEREAD_Silent))
		{
			return false;
		}
		FSHA1 Sha;
		Sha.Update(Bytes.GetData(), Bytes.Num());
		Sha.Final();
		uint8 Hash[FSHA1::DigestSize];
		Sha.GetHash(Hash);
		OutSha1 = BytesToHex(Hash, FSHA1::DigestSize).ToLower();
		OutSize = Bytes.Num();
		return true;
	}

	FString PackageFilename(const FString& PackagePath)
	{
		FString Existing;
		if (FPackageName::DoesPackageExist(PackagePath, &Existing) && !Existing.IsEmpty())
		{
			return FPaths::ConvertRelativePathToFull(Existing);
		}
		FString Filename;
		if (FPackageName::TryConvertLongPackageNameToFilename(PackagePath, Filename, FPackageName::GetAssetPackageExtension()))
		{
			return FPaths::ConvertRelativePathToFull(Filename);
		}
		return FString();
	}

	FKismetAIAssetFingerprint CaptureLoadedPackage(UPackage* Package, const FString& AssetPath, const FString& NormalizeFrom)
	{
		FKismetAIAssetFingerprint Out;
		Out.AssetPath = AssetPath;
		Out.bCaptured = true;
		UObject* Asset = FindAsset(Package);
		Out.bExists = Asset != nullptr;
		if (!Asset)
		{
			Out.UnsupportedReason = UnsupportedReason(nullptr);
			return Out;
		}
		Out.AssetClass = Asset->GetClass()->GetName();
		Out.bPackageDirty = Package->IsDirty();
		Out.UnsupportedReason = UnsupportedReason(Asset);
		Out.bSupported = Out.UnsupportedReason.IsEmpty();
		if (!Out.bSupported)
		{
			return Out;
		}

		TArray<FString> Summary;
		ForEachObjectWithPackage(Package, [&](UObject* Object)
		{
			if (!Object || Object->HasAnyFlags(RF_Transient) || Object->IsA<UField>() || !IsValid(Object)
				|| IsDetachedGraphObject(Object))
			{
				return true;
			}
			const FString Key = Normalize(Object->GetPathName(Package), NormalizeFrom, AssetPath);
			const TArray<FString> Lines = DescribeObject(Object, NormalizeFrom, AssetPath);
			const FString ObjectDigest = Sha1Hex(FString::Join(Lines, TEXT("\n")));
			Out.ObjectDigests.Add(Key, ObjectDigest.Left(16));
			Summary.Add(Key + TEXT("=") + ObjectDigest);
			return true;
		}, KISMETAI_INCLUDE_NESTED_OBJECTS);
		Summary.Sort();
		Out.Digest = Sha1Hex(FString::Join(Summary, TEXT("\n")));
		return Out;
	}

	FKismetAIAssetFingerprint Capture(const FString& PackagePath)
	{
		FKismetAIAssetFingerprint Out;
		Out.AssetPath = PackagePath;
		if (!FPackageName::IsValidLongPackageName(PackagePath))
		{
			Out.UnsupportedReason = TEXT("not a long package name");
			return Out;
		}
		FillFile(Out, PackagePath);

		UPackage* Package = FindPackage(nullptr, *PackagePath);
		if (!FindAsset(Package) && Out.bFileExists)
		{
			Package = LoadPackage(nullptr, *PackagePath, LOAD_NoWarn | LOAD_Quiet);
		}
		if (!FindAsset(Package))
		{
			// Neither loaded nor on disk: the asset does not exist. That is
			// evidence too (a create that did not happen, a delete that did).
			Out.bCaptured = true;
			Out.bExists = false;
			return Out;
		}
		FKismetAIAssetFingerprint State = CaptureLoadedPackage(Package, PackagePath);
		State.bFileExists = Out.bFileExists;
		State.FileSha1 = Out.FileSha1;
		State.FileSize = Out.FileSize;
		return State;
	}

	FKismetAIAssetFingerprint CaptureFile(const FString& FilePath, const FString& AssetPath)
	{
		FKismetAIAssetFingerprint Out;
		Out.AssetPath = AssetPath;
		Out.bFileExists = HashFile(FilePath, Out.FileSha1, Out.FileSize);
		if (!Out.bFileExists)
		{
			Out.UnsupportedReason = TEXT("the file could not be read");
			return Out;
		}
		const FString TempFile = FPaths::CreateTempFilename(*FPaths::DiffDir(), TEXT("KismetCheckpoint_"),
			*(TEXT(".") + FPaths::GetExtension(FilePath)));
		if (IFileManager::Get().Copy(*TempFile, *FilePath) != COPY_OK)
		{
			Out.UnsupportedReason = TEXT("the file could not be copied for inspection");
			return Out;
		}
		const FString OriginalFile = PackageFilename(AssetPath);
		UPackage* Copy = DiffUtils::LoadPackageForDiff(FPackagePath::FromLocalPath(TempFile),
			OriginalFile.IsEmpty() ? FPackagePath::FromLocalPath(TempFile) : FPackagePath::FromLocalPath(OriginalFile));
		if (!Copy)
		{
			IFileManager::Get().Delete(*TempFile, false, true, true);
			Out.UnsupportedReason = TEXT("the file could not be loaded (corrupt, or written by an incompatible engine version)");
			return Out;
		}
		FKismetAIAssetFingerprint State = CaptureLoadedPackage(Copy, AssetPath, Copy->GetName());
		State.bPackageDirty = false;
		State.bFileExists = true;
		State.FileSha1 = Out.FileSha1;
		State.FileSize = Out.FileSize;

		// Unload the copy: detach its loader so the temp file can go, and let GC
		// take the objects. The live package is never involved. A Blueprint
		// copy leaves the action database first (its spawners would outlive it).
		if (UBlueprint* CopyBlueprint = Cast<UBlueprint>(Copy->FindAssetInPackage()))
		{
			FBlueprintActionDatabase::Get().ClearAssetActions(CopyBlueprint);
		}
		ResetLoaders(Copy);
		ForEachObjectWithPackage(Copy, [](UObject* Object)
		{
			Object->ClearFlags(RF_Public | RF_Standalone);
			Object->MarkAsGarbage();
			return true;
		}, KISMETAI_INCLUDE_NESTED_OBJECTS);
		Copy->MarkAsGarbage();
		IFileManager::Get().Delete(*TempFile, false, true, true);
		return State;
	}
}
