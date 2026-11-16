// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "KismetAICancellation.h"
#include "KismetAISourceContract.h"

// Path confinement for read_source.
//
// The reader opens the candidate file first, then asks the OS which file the open
// handle actually refers to (F_GETPATH on macOS, /proc/self/fd on Linux,
// GetFinalPathNameByHandleW on Windows), and only reads through that same handle
// if that final path lies inside a permitted root. Roots are canonicalized the
// same way, and containment is checked per path component, so:
//   - an adjacent-prefix sibling ("Source-private" next to "Source") is outside;
//   - ".." and mixed separators are collapsed before the check, and the OS final
//     path is authoritative anyway;
//   - a symlink anywhere in the path is judged by its destination, not its name;
//   - swapping a symlink between check and read cannot redirect the read, because
//     the check is made on the already-open handle that is then read.
//
// Residual assumptions (not an OS sandbox):
//   - a hard link inside a permitted root to a file outside it is indistinguishable
//     from a regular file there; creating one needs write access to that root;
//   - plugins discovered under Engine/Plugins or <Project>/Plugins are trusted
//     roots at their canonical location, even when the plugin directory itself is
//     a symlink (the dev project links KismetAI this way);
//   - case: comparison is exact after OS canonicalization on macOS/Linux (the OS
//     returns on-disk case), and case-insensitive on Windows;
//   - platforms other than Mac/Linux/Windows fail closed.

enum class EKismetAISourceScope : uint8
{
	Any,      // scope omitted: union of every permitted root
	Project,  // <Project>/Source
	Engine,   // Engine/Source
	Plugins,  // Engine/Plugins, <Project>/Plugins, and discovered plugins under them
};

enum class EKismetAISourceReadStatus : uint8
{
	Ok,
	NotFound,      // could not be opened
	NotRegularFile,
	OutsideRoots,  // resolved target is not inside a permitted root for the scope
	BadExtension,  // resolved target does not carry an allowed source extension
	ReadFailed,
	TooLarge,
	Cancelled,
	Deadline,
	Changed,
};

// Lexical roots the reader resolves relative paths against and confines to.
// Production values come from FPaths; tests install a disposable fixture.
struct FKismetAISourceRoots
{
	FString ProjectDir; // absolute, no trailing slash requirement
	FString EngineDir;
	bool bIncludeDiscoveredPlugins = true;

	static FKismetAISourceRoots FromEngine();
};

struct FKismetAISourceReadOptions
{
	int64 MaxBytes = KismetAISourceContract::MaxReadBytes;
	const FKismetAICancellationToken* Cancellation = nullptr;
	double DeadlineSeconds = 0;
	TFunction<void(int64)> OnChunkForTests;
};

struct FKismetAISourceReadMetrics
{
	int64 BytesRead = 0;
	double ElapsedSeconds = 0;
};

namespace KismetAISourceConfinement
{
	bool TryParseScope(const FString& ScopeName, EKismetAISourceScope& OutScope);

	// Active roots: the test override if one is installed, otherwise FromEngine().
	FKismetAISourceRoots GetActiveRoots();

	// Canonical permitted roots for a scope. Roots that do not exist are omitted.
	TArray<FString> GetPermittedRoots(const FKismetAISourceRoots& SourceRoots, EKismetAISourceScope Scope);

	// OS-canonical path of an existing file or directory; empty if it cannot be resolved.
	FString CanonicalizeExisting(const FString& Path);

	// Component-aware containment of two canonical paths (platform case rule).
	bool IsWithinRoot(const FString& CanonicalPath, const FString& CanonicalRoot);

	bool HasAllowedSourceExtension(const FString& Path);

	// Opens Candidate, verifies the open handle's final path against PermittedRoots and
	// the extension rule, and reads the file through that same handle.
	EKismetAISourceReadStatus ReadConfined(
		const FString& Candidate,
		const TArray<FString>& PermittedRoots,
		TArray<uint8>& OutBytes,
		FString& OutCanonicalPath,
		const FKismetAISourceReadOptions& Options = FKismetAISourceReadOptions(),
		FKismetAISourceReadMetrics* Metrics = nullptr);

	// Shared native-handle attestation for files and incremental directory enumeration.
	FString CanonicalPathForHandle(UPTRINT NativeHandle);
	EKismetAISourceReadStatus ProbeConfined(const FString& Candidate,
		const TArray<FString>& PermittedRoots, FString& OutCanonicalPath, int64& OutSizeBytes);

	// Splits raw bytes into lines exactly like FFileHelper::LoadFileToStringArray.
	void BytesToLines(const TArray<uint8>& Bytes, TArray<FString>& OutLines);

	// Test seam: while alive, GetActiveRoots() returns the given roots. Game thread only.
	class FScopedRootsOverride
	{
	public:
		explicit FScopedRootsOverride(const FKismetAISourceRoots& InRoots);
		~FScopedRootsOverride();
		FScopedRootsOverride(const FScopedRootsOverride&) = delete;
		FScopedRootsOverride& operator=(const FScopedRootsOverride&) = delete;
	private:
		TOptional<FKismetAISourceRoots> Previous;
	};
}
