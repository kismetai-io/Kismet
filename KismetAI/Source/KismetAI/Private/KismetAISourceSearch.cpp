// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAISourceSearch.h"
#include "KismetAISourceConfinement.h"
#include "HAL/PlatformTime.h"
#include "HAL/ThreadSafeBool.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#elif PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace KismetAISourceSearch
{
namespace
{
	constexpr int32 MaxPathCharacters = 4096;
	constexpr int32 MaxRoots = 128;

	bool IsPermittedDirectory(const FString& Path, const TArray<FString>& Roots)
	{
		if (Path.IsEmpty() || Path.Len() > MaxPathCharacters) return false;
		for (const FString& Root : Roots)
		{
			if (Path == Root || KismetAISourceConfinement::IsWithinRoot(Path, Root)) return true;
		}
		return false;
	}

	struct FDirectory
	{
		FString Path;
		bool bReadFailed = false;
#if PLATFORM_MAC || PLATFORM_LINUX
		DIR* Handle = nullptr;
#elif PLATFORM_WINDOWS
		HANDLE Handle = INVALID_HANDLE_VALUE;
		alignas(8) uint8 Buffer[4096];
		uint32 Offset = 0;
		bool bNeedBatch = true;
		bool bFirstBatch = true;
#endif
		~FDirectory()
		{
#if PLATFORM_MAC || PLATFORM_LINUX
			if (Handle) ::closedir(Handle);
#elif PLATFORM_WINDOWS
			if (Handle != INVALID_HANDLE_VALUE) ::CloseHandle(Handle);
#endif
		}
		bool Open(const FString& Candidate, const TArray<FString>& Roots)
		{
#if PLATFORM_MAC || PLATFORM_LINUX
			// Nested symlink directories are excluded; canonical discovered plugin roots
			// have already been resolved by the caller and remain supported.
			const int Fd = ::open(TCHAR_TO_UTF8(*Candidate), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
			if (Fd < 0) return false;
			Handle = ::fdopendir(Fd);
			if (!Handle) { ::close(Fd); return false; }
			Path = KismetAISourceConfinement::CanonicalPathForHandle(static_cast<UPTRINT>(Fd));
#elif PLATFORM_WINDOWS
			Handle = ::CreateFileW(*Candidate, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
			if (Handle == INVALID_HANDLE_VALUE) return false;
			BY_HANDLE_FILE_INFORMATION Info;
			if (!::GetFileInformationByHandle(Handle, &Info) || (Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
			Path = KismetAISourceConfinement::CanonicalPathForHandle(reinterpret_cast<UPTRINT>(Handle));
#else
			return false;
#endif
			return IsPermittedDirectory(Path, Roots);
		}
		bool Next(FString& Name, bool& bDirectory, bool& bLink)
		{
#if PLATFORM_MAC || PLATFORM_LINUX
			for (;;)
			{
				errno = 0;
				const dirent* Entry = ::readdir(Handle);
				if (!Entry) { bReadFailed = errno != 0; return false; }
				Name = UTF8_TO_TCHAR(Entry->d_name);
				if (Name == TEXT(".") || Name == TEXT("..")) continue;
				struct stat Info;
				if (::fstatat(::dirfd(Handle), Entry->d_name, &Info, AT_SYMLINK_NOFOLLOW) != 0)
				{
					bDirectory = false; bLink = true;
					return true;
				}
				bDirectory = S_ISDIR(Info.st_mode);
				bLink = S_ISLNK(Info.st_mode);
				return true;
			}
#elif PLATFORM_WINDOWS
			for (;;)
			{
				if (bNeedBatch)
				{
					const FILE_INFO_BY_HANDLE_CLASS Kind = bFirstBatch ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo;
					if (!::GetFileInformationByHandleEx(Handle, Kind, Buffer, sizeof(Buffer)))
					{
						bReadFailed = ::GetLastError() != ERROR_NO_MORE_FILES;
						return false;
					}
					bFirstBatch = false; bNeedBatch = false; Offset = 0;
				}
				const FILE_ID_BOTH_DIR_INFO* Entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(Buffer + Offset);
				Name = FString(static_cast<int32>(Entry->FileNameLength / sizeof(WCHAR)), Entry->FileName);
				bDirectory = (Entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
				bLink = (Entry->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
				if (Entry->NextEntryOffset) Offset += Entry->NextEntryOffset; else bNeedBatch = true;
				if (Name == TEXT(".") || Name == TEXT("..")) continue;
				return true;
			}
#else
			return false;
#endif
		}
		bool StillPermitted(const TArray<FString>& Roots)
		{
#if PLATFORM_MAC || PLATFORM_LINUX
			Path = KismetAISourceConfinement::CanonicalPathForHandle(static_cast<UPTRINT>(::dirfd(Handle)));
#elif PLATFORM_WINDOWS
			Path = KismetAISourceConfinement::CanonicalPathForHandle(reinterpret_cast<UPTRINT>(Handle));
#endif
			return IsPermittedDirectory(Path, Roots);
		}
	};

	struct FCursor
	{
		TArray<FString> Roots;
		FString Pattern;
		FString Scope;
		int32 Limit = 20;
		int32 NextRoot = 0;
		TArray<TUniquePtr<FDirectory>> Stack;
		FString PendingLine;
		bool bOmitted = false;
		bool bBusy = false;
		FThreadSafeBool bClosed = false;
		double LastUsed = 0;
		FKismetAICancellationToken OriginToken;
		FKismetAICancellationToken LastPageToken;
	};
	FCriticalSection CursorMutex;
	TMap<FString, TSharedPtr<FCursor, ESPMode::ThreadSafe>> Cursors;

	FToolExecutionResult Error(const TCHAR* Code, const TCHAR* Message, int32 Status = 400)
	{
		return FToolExecutionResult::Error(Status, Code, Message);
	}
	int32 Utf8Size(const FString& Text) { return FTCHARToUTF8(*Text).Length(); }
	int32 JsonTextBudget(const FString& Text)
	{
		int32 Bytes = Utf8Size(Text);
		for (const TCHAR Ch : Text)
		{
			if (Ch < 32) Bytes += 5; // Conservative allowance for \u00XX escaping.
			else if (Ch == TEXT('"') || Ch == TEXT('\\')) ++Bytes;
		}
		return Bytes;
	}
}

void Close()
{
	FScopeLock Lock(&CursorMutex);
	for (const auto& Pair : Cursors) Pair.Value->bClosed = true;
	Cursors.Empty();
}

void ReapExpired(double NowSeconds)
{
	const double Now = NowSeconds > 0 ? NowSeconds : FPlatformTime::Seconds();
	TArray<TSharedPtr<FCursor, ESPMode::ThreadSafe>> Retired;
	{
		FScopeLock Lock(&CursorMutex);
		for (auto It = Cursors.CreateIterator(); It; ++It)
		{
			const bool bCancelled = It.Value()->OriginToken.IsCancelled() || It.Value()->LastPageToken.IsCancelled() || It.Value()->bClosed;
			if (bCancelled) It.Value()->bClosed = true;
			if (!It.Value()->bBusy && (bCancelled || Now - It.Value()->LastUsed >= KismetAISourceContract::CursorIdleSeconds))
			{
				Retired.Add(It.Value());
				It.RemoveCurrent();
			}
		}
	}
	// Destruction closes at most MaxCursors * MaxDepth handles, outside the map lock.
}

FToolExecutionResult Find(const TSharedPtr<FJsonObject>& Args,
	const TArray<FString>& CanonicalRoots, const FKismetAICancellationToken& Token,
	double DeadlineSeconds, const TFunction<void(int32)>& OnEntryForTests)
{
	using namespace KismetAISourceContract;
	const double Started = FPlatformTime::Seconds();
	FString Pattern, CursorId, Scope;
	if (!Args.IsValid() || !Args->TryGetStringField(TEXT("pattern"), Pattern)) return Error(TEXT("UNSUPPORTED_PATTERN"), TEXT("A basename pattern is required."));
	if (!PatternError(Pattern).IsEmpty()) return Error(TEXT("UNSUPPORTED_PATTERN"), *PatternError(Pattern));
	Args->TryGetStringField(TEXT("cursor"), CursorId);
	Args->TryGetStringField(TEXT("scope"), Scope);
	if (CanonicalRoots.Num() > MaxRoots) return Error(TEXT("SOURCE_ROOT_LIMIT"), TEXT("Too many source roots; choose an explicit scope."));
	for (const FString& Root : CanonicalRoots)
	{
		if (Root.Len() > MaxPathCharacters) return Error(TEXT("SOURCE_ROOT_LIMIT"), TEXT("A source root exceeds the supported path length."));
	}
	double RequestedLimit = MaxResults;
	Args->TryGetNumberField(TEXT("limit"), RequestedLimit);
	if (!FMath::IsFinite(RequestedLimit)) return Error(TEXT("INVALID_ARGUMENTS"), TEXT("Limit must be finite."));
	const int32 Limit = static_cast<int32>(FMath::Clamp(RequestedLimit, 1.0, static_cast<double>(MaxResults)));
	ReapExpired(Started);
	TSharedPtr<FCursor, ESPMode::ThreadSafe> Cursor;
	{
		FScopeLock Lock(&CursorMutex);
		if (!CursorId.IsEmpty())
		{
			const auto* Found = Cursors.Find(CursorId);
			if (!Found) return Error(TEXT("INVALID_CURSOR"), TEXT("Source cursor expired, was cancelled, or was already consumed; restart the search."));
			Cursor = *Found;
			if (Cursor->bBusy) return Error(TEXT("CURSOR_BUSY"), TEXT("A page for this cursor is already running."), 409);
			if (Cursor->Pattern != Pattern || Cursor->Roots != CanonicalRoots || Cursor->Scope != Scope || Cursor->Limit != Limit)
				return Error(TEXT("CURSOR_MISMATCH"), TEXT("Continuation must use the original pattern, roots, scope and limit."));
		}
		else
		{
			if (Cursors.Num() >= MaxCursors) return Error(TEXT("SOURCE_BUSY"), TEXT("Source cursor capacity reached; finish a search or wait for idle expiry."), 429);
			Cursor = MakeShared<FCursor, ESPMode::ThreadSafe>();
			Cursor->Roots = CanonicalRoots; Cursor->Pattern = Pattern; Cursor->Scope = Scope; Cursor->Limit = Limit;
			Cursor->OriginToken = Token;
			CursorId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Cursors.Add(CursorId, Cursor);
		}
		Cursor->LastPageToken = Token;
		Cursor->bBusy = true;
	}
	int32 Entries = 0, Directories = 0, Files = 0, Count = 0, TextBytes = 0, EscapedTextBytes = 0;
	FString Output, StopReason;
	auto Cancelled = [&]() { return Token.IsCancelled() || Cursor->OriginToken.IsCancelled() || Cursor->LastPageToken.IsCancelled() || Cursor->bClosed; };
	while (true)
	{
		if (Cancelled()) { StopReason = TEXT("cancelled"); break; }
		if (FPlatformTime::Seconds() - Started >= PageSeconds || (DeadlineSeconds > 0 && FPlatformTime::Seconds() >= DeadlineSeconds)) { StopReason = TEXT("time_budget"); break; }
		if (Count >= Limit) { StopReason = TEXT("result_limit"); break; }
		if (!Cursor->PendingLine.IsEmpty())
		{
			const int32 Bytes = Utf8Size(Cursor->PendingLine);
			const int32 EscapedBytes = JsonTextBudget(Cursor->PendingLine);
			if (EscapedTextBytes + EscapedBytes > MaxOutputBytes) { StopReason = TEXT("output_budget"); break; }
			Output += Cursor->PendingLine; TextBytes += Bytes; EscapedTextBytes += EscapedBytes; ++Count; Cursor->PendingLine.Empty();
			continue;
		}
		if (Entries >= MaxEntries) { StopReason = TEXT("entry_budget"); break; }
		if (Directories >= MaxDirectories) { StopReason = TEXT("directory_budget"); break; }
		if (Cursor->Stack.IsEmpty())
		{
			if (Cursor->NextRoot >= Cursor->Roots.Num()) break;
			auto Directory = MakeUnique<FDirectory>();
			++Directories;
			if (Directory->Open(Cursor->Roots[Cursor->NextRoot++], Cursor->Roots)) Cursor->Stack.Add(MoveTemp(Directory));
			else Cursor->bOmitted = true;
			continue;
		}
		FDirectory& Directory = *Cursor->Stack.Last();
		if (!Directory.StillPermitted(Cursor->Roots))
		{
			Cursor->bOmitted = true; Cursor->Stack.Pop(); continue;
		}
		FString Name;
		bool bDirectory = false, bLink = false;
		if (!Directory.Next(Name, bDirectory, bLink))
		{
			Cursor->bOmitted |= Directory.bReadFailed;
			Cursor->Stack.Pop();
			continue;
		}
		++Entries;
		if (OnEntryForTests) OnEntryForTests(Entries);
		if (Cancelled()) continue;
		const FString Candidate = Directory.Path / Name;
		if (Candidate.Len() > MaxPathCharacters) { Cursor->bOmitted = true; continue; }
		if (bDirectory)
		{
			if (bLink || Cursor->Stack.Num() >= MaxDepth) { Cursor->bOmitted = true; continue; }
			auto Child = MakeUnique<FDirectory>();
			++Directories;
			if (Child->Open(Candidate, Cursor->Roots)) Cursor->Stack.Add(MoveTemp(Child));
			else Cursor->bOmitted = true;
			continue;
		}
		++Files;
		if (!KismetAISourceConfinement::HasAllowedSourceExtension(Name) || Name.Contains(TEXT(".generated.")) || !Matches(Name, Pattern)) continue;
		FString Final;
		int64 Size = 0;
		if (KismetAISourceConfinement::ProbeConfined(Candidate, Cursor->Roots, Final, Size) != EKismetAISourceReadStatus::Ok)
		{
			Cursor->bOmitted = true;
			continue;
		}
		Cursor->PendingLine = FString::Printf(TEXT("%s (%lld bytes)\n"), *Final, Size);
		if (JsonTextBudget(Cursor->PendingLine) > MaxOutputBytes) { Cursor->PendingLine.Empty(); Cursor->bOmitted = true; }
	}
	const bool bCancelled = Cancelled();
	const bool bMore = !Cursor->Stack.IsEmpty() || Cursor->NextRoot < Cursor->Roots.Num() || !Cursor->PendingLine.IsEmpty();
	FString NextCursor;
	{
		FScopeLock Lock(&CursorMutex);
		Cursors.Remove(CursorId);
		if (bMore && !bCancelled && !Cursor->bClosed)
		{
			NextCursor = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Cursor->bBusy = false; Cursor->LastUsed = FPlatformTime::Seconds();
			Cursors.Add(NextCursor, Cursor);
		}
	}
	if (bCancelled || Cancelled()) return Error(TEXT("CANCELLED"), TEXT("Source search was cancelled."), 409);
	auto Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("result"), Output);
	Data->SetNumberField(TEXT("count"), Count);
	Data->SetBoolField(TEXT("partial"), bMore || Cursor->bOmitted);
	Data->SetBoolField(TEXT("truncated"), bMore || Cursor->bOmitted);
	Data->SetStringField(TEXT("stop_reason"), bMore ? StopReason : Cursor->bOmitted ? TEXT("excluded_or_unreadable_entries") : TEXT("complete"));
	Data->SetStringField(TEXT("continuation_semantics"), TEXT("Live tree in native order; unchanged trees resume without replay. Changes may omit or duplicate entries. Nested directory symlinks are excluded."));
	if (!NextCursor.IsEmpty()) Data->SetStringField(TEXT("cursor"), NextCursor);
	auto Metrics = MakeShared<FJsonObject>();
	Metrics->SetNumberField(TEXT("directories_visited"), Directories);
	Metrics->SetNumberField(TEXT("entries_considered"), Entries);
	Metrics->SetNumberField(TEXT("files_considered"), Files);
	Metrics->SetNumberField(TEXT("bytes_read"), 0);
	Metrics->SetNumberField(TEXT("elapsed_ms"), (FPlatformTime::Seconds() - Started) * 1000);
	Metrics->SetNumberField(TEXT("output_text_bytes"), TextBytes);
	Metrics->SetNumberField(TEXT("escaped_text_budget_bytes"), EscapedTextBytes);
	Data->SetObjectField(TEXT("metrics"), Metrics);
	for (int32 Pass = 0; Pass < 3; ++Pass)
	{
		FString Serialized;
		FJsonSerializer::Serialize(Data, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialized));
		Metrics->SetNumberField(TEXT("serialized_data_bytes"), Utf8Size(Serialized));
	}
	return FToolExecutionResult::Success(Data);
}
}
