// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAISourceConfinement.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#if PLATFORM_MAC
#include <sys/param.h>
#endif
#elif PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

FKismetAISourceRoots FKismetAISourceRoots::FromEngine()
{
	FKismetAISourceRoots Roots;
	Roots.ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	Roots.EngineDir = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
	Roots.bIncludeDiscoveredPlugins = true;
	return Roots;
}

namespace KismetAISourceConfinement
{
namespace
{
	TOptional<FKismetAISourceRoots> GRootsOverride;

	const TCHAR* const AllowedExtensions[] = {
		TEXT(".h"), TEXT(".cpp"), TEXT(".inl"),
		TEXT(".cs") // .Build.cs, .Target.cs
	};

	FString StripTrailingSlash(FString Path)
	{
		while (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
		{
			Path.LeftChopInline(1, EAllowShrinking::No);
		}
		return Path;
	}

#if PLATFORM_WINDOWS
	// "\\?\C:\x" -> "C:/x", "\\?\UNC\srv\share\x" -> "//srv/share/x".
	FString FinalPathFromHandle(HANDLE Handle)
	{
		TCHAR Stack[1024];
		const DWORD Len = ::GetFinalPathNameByHandleW(Handle, Stack, static_cast<DWORD>(UE_ARRAY_COUNT(Stack)), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
		FString Result;
		if (Len == 0)
		{
			return FString();
		}
		if (Len < UE_ARRAY_COUNT(Stack))
		{
			Result = FString::ConstructFromPtrSize(Stack, static_cast<int32>(Len));
		}
		else
		{
			TArray<TCHAR> Heap;
			Heap.SetNumZeroed(Len + 1);
			const DWORD Len2 = ::GetFinalPathNameByHandleW(Handle, Heap.GetData(), Len + 1, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
			if (Len2 == 0 || Len2 > Len)
			{
				return FString();
			}
			Result = FString::ConstructFromPtrSize(Heap.GetData(), static_cast<int32>(Len2));
		}
		if (Result.StartsWith(TEXT("\\\\?\\UNC\\")))
		{
			Result = TEXT("\\\\") + Result.RightChop(8);
		}
		else if (Result.StartsWith(TEXT("\\\\?\\")))
		{
			Result = Result.RightChop(4);
		}
		Result.ReplaceCharInline(TEXT('\\'), TEXT('/'));
		return StripTrailingSlash(Result);
	}
#endif
}

bool TryParseScope(const FString& ScopeName, EKismetAISourceScope& OutScope)
{
	if (ScopeName == TEXT("project")) { OutScope = EKismetAISourceScope::Project; return true; }
	if (ScopeName == TEXT("engine"))  { OutScope = EKismetAISourceScope::Engine;  return true; }
	if (ScopeName == TEXT("plugins")) { OutScope = EKismetAISourceScope::Plugins; return true; }
	return false;
}

FKismetAISourceRoots GetActiveRoots()
{
	return GRootsOverride.IsSet() ? GRootsOverride.GetValue() : FKismetAISourceRoots::FromEngine();
}

FString CanonicalizeExisting(const FString& Path)
{
	const FString Full = FPaths::ConvertRelativePathToFull(Path);
#if PLATFORM_MAC || PLATFORM_LINUX
	char* Resolved = ::realpath(TCHAR_TO_UTF8(*Full), nullptr);
	if (Resolved == nullptr)
	{
		return FString();
	}
	const FString Result = StripTrailingSlash(FString(UTF8_TO_TCHAR(Resolved)));
	::free(Resolved);
	return Result;
#elif PLATFORM_WINDOWS
	// FILE_FLAG_BACKUP_SEMANTICS lets the same call open directories.
	HANDLE Handle = ::CreateFileW(*Full, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (Handle == INVALID_HANDLE_VALUE)
	{
		return FString();
	}
	const FString Result = FinalPathFromHandle(Handle);
	::CloseHandle(Handle);
	return Result;
#else
	return FString(); // unsupported platform: fail closed
#endif
}

bool IsWithinRoot(const FString& CanonicalPath, const FString& CanonicalRoot)
{
	if (CanonicalPath.IsEmpty() || CanonicalRoot.IsEmpty())
	{
		return false;
	}
	const FString Root = StripTrailingSlash(CanonicalRoot);
#if PLATFORM_WINDOWS
	const ESearchCase::Type Case = ESearchCase::IgnoreCase;
#else
	const ESearchCase::Type Case = ESearchCase::CaseSensitive;
#endif
	if (CanonicalPath.Len() <= Root.Len())
	{
		return false; // the root itself is a directory, never a readable source file
	}
	if (!CanonicalPath.StartsWith(Root, Case))
	{
		return false;
	}
	// Component boundary: "/p/Source-private" must not match root "/p/Source".
	return Root.EndsWith(TEXT("/")) || CanonicalPath[Root.Len()] == TEXT('/');
}

bool HasAllowedSourceExtension(const FString& Path)
{
	for (const TCHAR* Ext : AllowedExtensions)
	{
		if (Path.EndsWith(Ext, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

TArray<FString> GetPermittedRoots(const FKismetAISourceRoots& SourceRoots, EKismetAISourceScope Scope)
{
	TArray<FString> Lexical;
	const bool bAny = Scope == EKismetAISourceScope::Any;
	if (bAny || Scope == EKismetAISourceScope::Project)
	{
		Lexical.Add(SourceRoots.ProjectDir / TEXT("Source"));
	}
	if (bAny || Scope == EKismetAISourceScope::Engine)
	{
		Lexical.Add(SourceRoots.EngineDir / TEXT("Source"));
	}
	const bool bPlugins = bAny || Scope == EKismetAISourceScope::Plugins;
	if (bPlugins)
	{
		Lexical.Add(SourceRoots.EngineDir / TEXT("Plugins"));
		Lexical.Add(SourceRoots.ProjectDir / TEXT("Plugins"));
	}

	TArray<FString> Canonical;
	for (const FString& Root : Lexical)
	{
		const FString Resolved = CanonicalizeExisting(Root);
		if (!Resolved.IsEmpty())
		{
			Canonical.AddUnique(Resolved);
		}
	}

	// A plugin the engine discovered under Engine/Plugins or <Project>/Plugins is a
	// trusted root at its canonical location, so a symlinked plugin directory (the dev
	// project links KismetAI in) stays readable. Arbitrary symlinks are not admitted:
	// only directories UE itself loaded a .uplugin from.
	if (bPlugins && SourceRoots.bIncludeDiscoveredPlugins )
	{
		const FString EnginePlugins = FPaths::ConvertRelativePathToFull(SourceRoots.EngineDir / TEXT("Plugins"));
		const FString ProjectPlugins = FPaths::ConvertRelativePathToFull(SourceRoots.ProjectDir / TEXT("Plugins"));
		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
		{
			const FString BaseDir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
			if (!FPaths::IsUnderDirectory(BaseDir, EnginePlugins) && !FPaths::IsUnderDirectory(BaseDir, ProjectPlugins))
			{
				continue;
			}
			const FString Resolved = CanonicalizeExisting(BaseDir);
			if (Resolved.IsEmpty())
			{
				continue;
			}
			bool bCovered = false;
			for (const FString& Existing : Canonical)
			{
				if (Existing == Resolved || IsWithinRoot(Resolved, Existing))
				{
					bCovered = true;
					break;
				}
			}
			if (!bCovered)
			{
				Canonical.Add(Resolved);
			}
		}
	}
	return Canonical;
}

FString CanonicalPathForHandle(UPTRINT NativeHandle)
{
#if PLATFORM_MAC
	char PathBuf[MAXPATHLEN];
	if (::fcntl(static_cast<int>(NativeHandle), F_GETPATH, PathBuf) == -1) return FString();
	return StripTrailingSlash(FString(UTF8_TO_TCHAR(PathBuf)));
#elif PLATFORM_LINUX
	char ProcLink[64];
	::snprintf(ProcLink, sizeof(ProcLink), "/proc/self/fd/%d", static_cast<int>(NativeHandle));
	char PathBuf[4096];
	const ssize_t Length = ::readlink(ProcLink, PathBuf, sizeof(PathBuf) - 1);
	if (Length <= 0 || Length >= static_cast<ssize_t>(sizeof(PathBuf) - 1)) return FString();
	PathBuf[Length] = '\0';
	return StripTrailingSlash(FString(UTF8_TO_TCHAR(PathBuf)));
#elif PLATFORM_WINDOWS
	return FinalPathFromHandle(reinterpret_cast<HANDLE>(NativeHandle));
#else
	return FString();
#endif
}

static EKismetAISourceReadStatus ReadOrProbe(const FString& Candidate,
	const TArray<FString>& PermittedRoots, TArray<uint8>& OutBytes,
	FString& OutCanonicalPath, const FKismetAISourceReadOptions& Options,
	FKismetAISourceReadMetrics* Metrics, int64* ProbeSize)
{
	OutBytes.Reset();
	OutCanonicalPath.Reset();
	const double Started = FPlatformTime::Seconds();
	if (Metrics) *Metrics = FKismetAISourceReadMetrics();
	ON_SCOPE_EXIT { if (Metrics) Metrics->ElapsedSeconds = FPlatformTime::Seconds() - Started; };
	auto CheckWork = [&Options]()
	{
		if (Options.Cancellation && Options.Cancellation->IsCancelled()) return EKismetAISourceReadStatus::Cancelled;
		if (Options.DeadlineSeconds > 0 && FPlatformTime::Seconds() >= Options.DeadlineSeconds) return EKismetAISourceReadStatus::Deadline;
		return EKismetAISourceReadStatus::Ok;
	};
	if (const auto Status = CheckWork(); Status != EKismetAISourceReadStatus::Ok) return Status;
	const FString Full = FPaths::ConvertRelativePathToFull(Candidate);
	int64 FileSize = 0;
#if PLATFORM_MAC || PLATFORM_LINUX
	const int Handle = ::open(TCHAR_TO_UTF8(*Full), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (Handle < 0) return EKismetAISourceReadStatus::NotFound;
	ON_SCOPE_EXIT { ::close(Handle); };
	struct stat Info;
	if (::fstat(Handle, &Info) != 0 || !S_ISREG(Info.st_mode)) return EKismetAISourceReadStatus::NotRegularFile;
	FileSize = Info.st_size;
	const FString Final = CanonicalPathForHandle(static_cast<UPTRINT>(Handle));
#elif PLATFORM_WINDOWS
	HANDLE Handle = ::CreateFileW(*Full, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (Handle == INVALID_HANDLE_VALUE) return EKismetAISourceReadStatus::NotFound;
	ON_SCOPE_EXIT { ::CloseHandle(Handle); };
	BY_HANDLE_FILE_INFORMATION Info;
	if (::GetFileType(Handle) != FILE_TYPE_DISK || !::GetFileInformationByHandle(Handle, &Info)
		|| (Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return EKismetAISourceReadStatus::NotRegularFile;
	LARGE_INTEGER Size;
	if (!::GetFileSizeEx(Handle, &Size)) return EKismetAISourceReadStatus::ReadFailed;
	FileSize = Size.QuadPart;
	const FString Final = CanonicalPathForHandle(reinterpret_cast<UPTRINT>(Handle));
#else
	return EKismetAISourceReadStatus::OutsideRoots;
#endif
#if PLATFORM_MAC || PLATFORM_LINUX || PLATFORM_WINDOWS
	bool bInside = false;
	for (const FString& Root : PermittedRoots) bInside |= IsWithinRoot(Final, Root);
	if (!bInside) return EKismetAISourceReadStatus::OutsideRoots;
	if (!HasAllowedSourceExtension(Final)) return EKismetAISourceReadStatus::BadExtension;
	OutCanonicalPath = Final;
	if (ProbeSize)
	{
		*ProbeSize = FileSize;
		return CheckWork();
	}
	const int64 Cap = FMath::Clamp<int64>(Options.MaxBytes, 0, KismetAISourceContract::MaxReadBytes);
	if (FileSize > Cap) return EKismetAISourceReadStatus::TooLarge;
	uint8 Chunk[64 * 1024];
	// Read the attested file's initial length. A concurrent append is outside this read,
	// while premature EOF reports a changed file instead of claiming a complete read.
	while (OutBytes.Num() < FileSize)
	{
		if (const auto Status = CheckWork(); Status != EKismetAISourceReadStatus::Ok)
		{
			OutBytes.Reset();
			return Status;
		}
		const int32 Wanted = static_cast<int32>(FMath::Min<int64>(sizeof(Chunk), FileSize - OutBytes.Num()));
#if PLATFORM_MAC || PLATFORM_LINUX
		const ssize_t Got = ::read(Handle, Chunk, Wanted);
		if (Got < 0)
		{
			if (errno == EINTR) continue;
			OutBytes.Reset();
			return EKismetAISourceReadStatus::ReadFailed;
		}
#else
		DWORD Got = 0;
		if (!::ReadFile(Handle, Chunk, Wanted, &Got, nullptr))
		{
			OutBytes.Reset();
			return EKismetAISourceReadStatus::ReadFailed;
		}
#endif
		if (Got == 0)
		{
			OutBytes.Reset();
			return EKismetAISourceReadStatus::Changed;
		}
		OutBytes.Append(Chunk, static_cast<int32>(Got));
		if (Metrics) Metrics->BytesRead += Got;
		if (Options.OnChunkForTests) Options.OnChunkForTests(OutBytes.Num());
	}
	if (const auto Status = CheckWork(); Status != EKismetAISourceReadStatus::Ok)
	{
		OutBytes.Reset();
		return Status;
	}
	return EKismetAISourceReadStatus::Ok;
#endif
}

EKismetAISourceReadStatus ReadConfined(const FString& Candidate,
	const TArray<FString>& PermittedRoots, TArray<uint8>& OutBytes, FString& OutCanonicalPath,
	const FKismetAISourceReadOptions& Options, FKismetAISourceReadMetrics* Metrics)
{
	return ReadOrProbe(Candidate, PermittedRoots, OutBytes, OutCanonicalPath, Options, Metrics, nullptr);
}

EKismetAISourceReadStatus ProbeConfined(const FString& Candidate,
	const TArray<FString>& PermittedRoots, FString& OutCanonicalPath, int64& OutSizeBytes)
{
	TArray<uint8> Unused;
	OutSizeBytes = 0;
	return ReadOrProbe(Candidate, PermittedRoots, Unused, OutCanonicalPath, FKismetAISourceReadOptions(), nullptr, &OutSizeBytes);
}

void BytesToLines(const TArray<uint8>& Bytes, TArray<FString>& OutLines)
{
	OutLines.Reset();
	const int32 Num = Bytes.Num();

	// UTF-16 BOM: convert the whole buffer, then split (mirrors LoadFileToStringArray).
	if (Num >= 2 && !(Num & 1) &&
		((Bytes[0] == 0xFF && Bytes[1] == 0xFE) || (Bytes[0] == 0xFE && Bytes[1] == 0xFF)))
	{
		FString Buffer;
		FFileHelper::BufferToString(Buffer, Bytes.GetData(), Num);
		for (const TCHAR* Pos = *Buffer; *Pos != 0; )
		{
			const TCHAR* LineStart = Pos;
			while (*Pos != 0 && *Pos != TEXT('\r') && *Pos != TEXT('\n'))
			{
				Pos++;
			}
			OutLines.Add(FString::ConstructFromPtrSize(LineStart, UE_PTRDIFF_TO_INT32(Pos - LineStart)));
			if (*Pos == TEXT('\r')) { Pos++; }
			if (*Pos == TEXT('\n')) { Pos++; }
		}
		return;
	}

	// UTF-8/ANSI: convert per line; BufferToString strips a UTF-8 BOM on the first line.
	const uint8* Pos = Bytes.GetData();
	int32 Remaining = Num;
	while (Remaining > 0)
	{
		const uint8* LineStart = Pos;
		while (Remaining > 0 && *Pos != '\r' && *Pos != '\n')
		{
			Pos++;
			Remaining--;
		}
		FString Line;
		FFileHelper::BufferToString(Line, LineStart, UE_PTRDIFF_TO_INT32(Pos - LineStart));
		OutLines.Add(MoveTemp(Line));
		if (Remaining > 0 && *Pos == '\r') { Pos++; Remaining--; }
		if (Remaining > 0 && *Pos == '\n') { Pos++; Remaining--; }
	}
}

FScopedRootsOverride::FScopedRootsOverride(const FKismetAISourceRoots& InRoots)
	: Previous(GRootsOverride)
{
	check(IsInGameThread());
	GRootsOverride = InRoots;
}

FScopedRootsOverride::~FScopedRootsOverride()
{
	check(IsInGameThread());
	GRootsOverride = Previous;
}
}
