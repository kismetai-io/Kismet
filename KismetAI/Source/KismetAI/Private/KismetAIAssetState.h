// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Asset-state fingerprints: what an asset looks like, as a digest, so
// two states can be compared without keeping either one.
//
// Described state, per object in the asset's package (key: the object's path
// inside the package, e.g. "BP_Hero:EventGraph.K2Node_CallFunction_3"):
//   - the class;
//   - every property that is not transient or deprecated, as exported text
//     (object references export as paths, so links between objects count);
//   - for a graph node, every pin: id, name, direction, type, default value /
//     object / text, flags, and its links as (peer node GUID, peer pin id).
// Left out because the editor rewrites them without anyone editing the asset:
//   - compiler messages on graph nodes (ErrorType, ErrorMsg, bHasCompilerMessage);
//   - material cache keys regenerated on every PostEditChange (StateId,
//     LightingGuid; see KismetAIBatchRecovery.cpp);
//   - generated code (UClass / UFunction objects) — it is rebuilt from the
//     described state by a compile.
// No object address is part of the state, so a fingerprint taken after a
// restart, a reload or from a copy of the file loaded under another name
// (normalized to the original package name) is comparable.
//
// Supported classes: Blueprints of every kind (including Widget and Animation
// Blueprints) and materials (UMaterialInterface, UMaterialFunctionInterface).
// Any other asset is described as unsupported, with the reason; a map (UWorld)
// package is unsupported because its actors live in other packages.
//
// Game thread only.

#pragma once

#include "CoreMinimal.h"
#include "KismetAIMutationIntent.h"

class UPackage;
class UObject;

namespace KismetAIAssetState
{
	// The asset at a long package path, now: loaded state (the asset is loaded
	// if it is not), dirty flag and package file. A path that is not a valid long
	// package name is captured as unsupported.
	FKismetAIAssetFingerprint Capture(const FString& PackagePath);

	// The described state of an already loaded package. NormalizeFrom (a package
	// name) is replaced with AssetPath in every value, for copies loaded under
	// another name. Does not touch the file fields.
	FKismetAIAssetFingerprint CaptureLoadedPackage(UPackage* Package, const FString& AssetPath, const FString& NormalizeFrom = FString());

	// The state a package FILE holds: the file is copied to a temp name and
	// loaded beside the live package the way the editor's diff tool does
	// (DiffUtils::LoadPackageForDiff), described, and unloaded. Never touches
	// the live package. bCaptured=false with a reason when it cannot be loaded.
	FKismetAIAssetFingerprint CaptureFile(const FString& FilePath, const FString& AssetPath);

	// SHA-1 (hex) and size of a file. False when it cannot be read.
	bool HashFile(const FString& FilePath, FString& OutSha1, int64& OutSize);

	// The package file a long package name maps to (.uasset), whether or not it exists.
	FString PackageFilename(const FString& PackagePath);

	// The described lines of one object (diagnostics: what a digest covers).
	TArray<FString> DescribeObject(UObject* Object, const FString& NormalizeFrom = FString(), const FString& NormalizeTo = FString());

	// Why an asset is not supported, or empty when it is.
	FString UnsupportedReason(const UObject* Asset);
}
