// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAISessionStorage.h"
#include "KismetAIJournalReducer.h"
#include "KismetAILog.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include <sys/stat.h>
#include <errno.h>
#endif

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/ScopeLock.h"

DEFINE_LOG_CATEGORY_STATIC(LogKismetAISession, Log, All);

// ─────────────────────────────────────────────────────────────────────────────
// Path helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	// Read from worker threads (journal appends), written by the test sandbox
	// on the game thread between tests.
	FCriticalSection SessionsDirectoryOverrideLock;
	FString SessionsDirectoryOverride;
}

FString FKismetAISessionStorage::GetSessionsDirectory()
{
	{
		FScopeLock Lock(&SessionsDirectoryOverrideLock);
		if (!SessionsDirectoryOverride.IsEmpty())
		{
			return SessionsDirectoryOverride;
		}
	}
	return FPaths::ProjectSavedDir() / TEXT("KismetAI") / TEXT("sessions");
}

void FKismetAISessionStorage::SetSessionsDirectoryOverrideForTests(const FString& Directory)
{
	FScopeLock Lock(&SessionsDirectoryOverrideLock);
	SessionsDirectoryOverride = Directory;
}

FString FKismetAISessionStorage::GetSessionFilePath(const FString& SessionId)
{
	return GetSessionsDirectory() / SessionId + TEXT(".jsonl");
}

FString FKismetAISessionStorage::CreateSessionId()
{
	return FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
}

// ─────────────────────────────────────────────────────────────────────────────
// JSON helper
// ─────────────────────────────────────────────────────────────────────────────

FString FKismetAISessionStorage::JsonToString(TSharedPtr<FJsonObject> Obj)
{
	FString Output;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
	FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
	return Output;
}

// ─────────────────────────────────────────────────────────────────────────────
// Append entry
// ─────────────────────────────────────────────────────────────────────────────

bool FKismetAISessionStorage::AppendEntry(const FString& SessionId, TSharedPtr<FJsonObject> Entry)
{
	if (!Entry.IsValid() || SessionId.IsEmpty()) return false;

	FString Dir = GetSessionsDirectory();
	IFileManager::Get().MakeDirectory(*Dir, true);

	FString FilePath = GetSessionFilePath(SessionId);
	FString Line = JsonToString(Entry) + TEXT("\n");

	// A crash mid-append leaves a final record with no newline. Appending
	// straight after it would glue this record onto the fragment, and the glued
	// line fails to parse — losing this record (on resume: the user's new
	// instruction). Terminate the fragment and say so in a seal record; the
	// fragment's bytes stay exactly where they were, and the reader classifies
	// the sealed line as a recovered truncation rather than corruption.
	{
		int64 Size = 0;
		uint8 LastByte = '\n';
		{
			TUniquePtr<IFileHandle> Reader(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*FilePath));
			if (Reader)
			{
				Size = Reader->Size();
				if (Size > 0 && !(Reader->Seek(Size - 1) && Reader->Read(&LastByte, 1)))
				{
					LastByte = '\n';  // unreadable tail: the append below reports any real failure
				}
			}
		}
		if (Size > 0 && LastByte != '\n')
		{
			TSharedPtr<FJsonObject> Seal = MakeShared<FJsonObject>();
			Seal->SetStringField(TEXT("type"), KismetAIJournalReducer::SealEntryType);
			Seal->SetNumberField(TEXT("schema"), 1);
			Seal->SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());
			Seal->SetStringField(TEXT("reason"), TEXT("unterminated_final_record"));
			Seal->SetNumberField(TEXT("at_offset"), (double)Size);
			Line = TEXT("\n") + JsonToString(Seal) + TEXT("\n") + Line;
			KISMETAI_LOG(LogKismetAISession, Warning,
				TEXT("AppendEntry: session %s ended in an unterminated record at byte %lld; sealed it before appending"),
				*SessionId, Size);
		}
	}

	if (FFileHelper::SaveStringToFile(Line, *FilePath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(),
			FILEWRITE_Append))
	{
		EnsurePrivateMode(FilePath);
		return true;
	}

	KISMETAI_LOG(LogKismetAISession, Warning, TEXT("AppendEntry: failed to write to session %s"), *SessionId);
	return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Head / Tail reading (for fast session listing)
// ─────────────────────────────────────────────────────────────────────────────

FString FKismetAISessionStorage::ReadHead(const FString& FilePath, int32 MaxBytes)
{
	TUniquePtr<IFileHandle> File(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*FilePath));
	if (!File) return FString();

	int64 FileSize = File->Size();
	int32 ReadSize = FMath::Min((int32)FileSize, MaxBytes);

	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(ReadSize);
	if (!File->Read(Buffer.GetData(), ReadSize))
	{
		return FString();
	}

	// Convert UTF-8 to FString
	FUTF8ToTCHAR Converter((const ANSICHAR*)Buffer.GetData(), ReadSize);
	return FString(Converter.Length(), Converter.Get());
}

FString FKismetAISessionStorage::ReadTail(const FString& FilePath, int32 MaxBytes)
{
	TUniquePtr<IFileHandle> File(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*FilePath));
	if (!File) return FString();

	int64 FileSize = File->Size();
	if (FileSize <= MaxBytes)
	{
		// Small file — tail is the whole file
		return ReadHead(FilePath, MaxBytes);
	}

	int64 Offset = FileSize - MaxBytes;
	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(MaxBytes);

	if (!File->Seek(Offset) || !File->Read(Buffer.GetData(), MaxBytes))
	{
		return FString();
	}

	FUTF8ToTCHAR Converter((const ANSICHAR*)Buffer.GetData(), MaxBytes);
	FString Result(Converter.Length(), Converter.Get());

	// Skip partial first line (we might have landed mid-line)
	int32 FirstNewline;
	if (Result.FindChar(TEXT('\n'), FirstNewline))
	{
		Result.RightChopInline(FirstNewline + 1);
	}

	return Result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Metadata extraction
// ─────────────────────────────────────────────────────────────────────────────

// Remove a leading "<references>\n...\n</references>\n\n" block from a user
// message in place. Mirrors FormatReferencesBlock's output and the UI's render
// strip. Only a block at the very start is removed (that is the only place the
// agent prepends it); message bodies that merely mention the tag are untouched.
void FKismetAISessionStorage::StripLeadingReferencesBlock(FString& Content)
{
	const TCHAR* OpenTag = TEXT("<references>");
	if (!Content.StartsWith(OpenTag))
	{
		return;
	}

	int32 CloseIdx = Content.Find(TEXT("</references>"), ESearchCase::CaseSensitive, ESearchDir::FromStart);
	if (CloseIdx == INDEX_NONE)
	{
		return;
	}

	int32 CutFrom = CloseIdx + FCString::Strlen(TEXT("</references>"));
	Content.MidInline(CutFrom, MAX_int32, EAllowShrinking::No);
	// Drop the blank-line separator (and any leading whitespace) the agent adds
	// between the block and the real prompt.
	Content.TrimStartInline();
}

FString FKismetAISessionStorage::ExtractFirstPrompt(const FString& HeadContent)
{
	// Parse line by line, find first user_message entry
	TArray<FString> Lines;
	HeadContent.ParseIntoArrayLines(Lines);

	for (const FString& Line : Lines)
	{
		if (Line.IsEmpty()) continue;

		TSharedPtr<FJsonObject> Entry;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
		if (!FJsonSerializer::Deserialize(Reader, Entry) || !Entry.IsValid()) continue;

		FString Type;
		if (Entry->TryGetStringField(TEXT("type"), Type) && Type == TEXT("user_message"))
		{
			FString Content;
			if (Entry->TryGetStringField(TEXT("content"), Content))
			{
				// The agent prepends a "<references>\n...\n</references>\n\n" block
				// (built by FormatReferencesBlock) to the user message when @-refs
				// are present. It is machine context, not what the user typed, so
				// strip a leading block before deriving the display title — otherwise
				// the session list shows "<references> - ..." instead of the prompt.
				StripLeadingReferencesBlock(Content);

				// Truncate to 200 chars for display
				if (Content.Len() > 200)
				{
					Content = Content.Left(200) + TEXT("...");
				}
				return Content;
			}
		}
	}

	return FString();
}

bool FKismetAISessionStorage::ExtractTailMetadata(const FString& TailContent, FKismetAISessionInfo& OutInfo)
{
	// Parse lines in reverse order — last session_meta wins
	TArray<FString> Lines;
	TailContent.ParseIntoArrayLines(Lines);

	bool bFound = false;

	// Scan all lines — last occurrence of session_meta wins
	for (const FString& Line : Lines)
	{
		if (Line.IsEmpty()) continue;

		TSharedPtr<FJsonObject> Entry;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
		if (!FJsonSerializer::Deserialize(Reader, Entry) || !Entry.IsValid()) continue;

		FString Type;
		if (!Entry->TryGetStringField(TEXT("type"), Type)) continue;

		if ((Type == TEXT("error") || Type == TEXT("done")) && Entry->HasField(TEXT("outcome")))
		{
			Entry->TryGetStringField(TEXT("status"), OutInfo.Status);
			if (!Entry->TryGetStringField(TEXT("stop_reason"), OutInfo.StopReason)) { Entry->TryGetStringField(TEXT("error_code"), OutInfo.StopReason); }
			bFound = true;
			continue;
		}
		if (Type == TEXT("session_meta"))
		{
			Entry->TryGetStringField(TEXT("model"), OutInfo.Model);
			Entry->TryGetStringField(TEXT("blueprint"), OutInfo.BlueprintPath);
			Entry->TryGetStringField(TEXT("status"), OutInfo.Status);
			Entry->TryGetStringField(TEXT("stop_reason"), OutInfo.StopReason);
			Entry->TryGetNumberField(TEXT("cost_usd"), OutInfo.CostUSD);

			double TurnCount = 0, InputTokens = 0, OutputTokens = 0;
			Entry->TryGetNumberField(TEXT("turn_count"), TurnCount);
			Entry->TryGetNumberField(TEXT("total_input_tokens"), InputTokens);
			Entry->TryGetNumberField(TEXT("total_output_tokens"), OutputTokens);
			OutInfo.TurnCount = (int32)TurnCount;
			OutInfo.TotalInputTokens = (int32)InputTokens;
			OutInfo.TotalOutputTokens = (int32)OutputTokens;
			bFound = true;
		}
	}

	return bFound;
}

// ─────────────────────────────────────────────────────────────────────────────
// List sessions
// ─────────────────────────────────────────────────────────────────────────────

TArray<FKismetAISessionInfo> FKismetAISessionStorage::ListSessions(int32 MaxCount)
{
	TArray<FKismetAISessionInfo> Sessions;

	FString Dir = GetSessionsDirectory();
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.jsonl")), true, false);

	// Gather file info with timestamps
	struct FSessionFile
	{
		FString FileName;
		FDateTime Modified;
	};
	TArray<FSessionFile> SessionFiles;

	for (const FString& File : Files)
	{
		FString FullPath = Dir / File;
		FFileStatData Stat = IFileManager::Get().GetStatData(*FullPath);
		if (Stat.bIsValid)
		{
			SessionFiles.Add({File, Stat.ModificationTime});
		}
	}

	// Sort by modified time, newest first
	SessionFiles.Sort([](const FSessionFile& A, const FSessionFile& B)
	{
		return A.Modified > B.Modified;
	});

	// Limit count
	int32 Count = FMath::Min(SessionFiles.Num(), MaxCount);

	for (int32 i = 0; i < Count; i++)
	{
		const FSessionFile& SF = SessionFiles[i];
		FString FullPath = Dir / SF.FileName;

		FKismetAISessionInfo Info;
		Info.SessionId = FPaths::GetBaseFilename(SF.FileName);

		// File timestamps
		FFileStatData Stat = IFileManager::Get().GetStatData(*FullPath);
		Info.Created = Stat.CreationTime;
		Info.Modified = Stat.ModificationTime;

		// Read head for title (first user message)
		FString Head = ReadHead(FullPath);
		Info.Title = ExtractFirstPrompt(Head);
		if (Info.Title.IsEmpty())
		{
			Info.Title = TEXT("(empty session)");
		}

		// Read tail for metadata (model, tokens, cost)
		FString Tail = ReadTail(FullPath);
		ExtractTailMetadata(Tail, Info);

		Sessions.Add(MoveTemp(Info));
	}

	return Sessions;
}

// ─────────────────────────────────────────────────────────────────────────────
// Load session (the one reader: KismetAIJournalReducer)
// ─────────────────────────────────────────────────────────────────────────────

FSessionSnapshot FKismetAISessionStorage::LoadSession(const FString& SessionId)
{
	return LoadSessionFile(GetSessionFilePath(SessionId));
}

FSessionSnapshot FKismetAISessionStorage::LoadSessionFile(const FString& FilePath)
{
	if (!IFileManager::Get().FileExists(*FilePath))
	{
		FSessionSnapshot Missing;
		Missing.Integrity = EJournalIntegrity::Missing;
		return Missing;
	}

	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *FilePath))
	{
		KISMETAI_LOG(LogKismetAISession, Warning, TEXT("Session journal exists but could not be read: %s"), *FPaths::GetCleanFilename(FilePath));
		FSessionSnapshot Unreadable;
		Unreadable.Integrity = EJournalIntegrity::Unreadable;
		FJournalDiagnostic Diag;
		Diag.Issue = EJournalIssue::UnreadableFile;
		Diag.Detail = TEXT("The session journal could not be read");
		Unreadable.Diagnostics.Add(Diag);
		return Unreadable;
	}

	FSessionSnapshot Snapshot = KismetAIJournalReducer::ReduceJournalBytes(Bytes);
	if (Snapshot.Integrity != EJournalIntegrity::Intact)
	{
		KISMETAI_LOG(LogKismetAISession, Warning, TEXT("Session journal %s loaded as %s (%d diagnostic(s))"),
			*FPaths::GetCleanFilename(FilePath), FSessionSnapshot::IntegrityToString(Snapshot.Integrity), Snapshot.Diagnostics.Num());
	}
	KISMETAI_DEV_LOG(LogKismetAISession, Log, TEXT("Loaded %d messages from %s"), Snapshot.Messages.Num(), *FPaths::GetCleanFilename(FilePath));
	return Snapshot;
}

TArray<TSharedPtr<FJsonValue>> FKismetAISessionStorage::LoadMessages(const FString& SessionId)
{
	return KismetAIAgentCodec::ToAnthropicArray(KismetAIJournalReducer::ProjectResumeRequest(LoadSession(SessionId).Context));
}

// ─────────────────────────────────────────────────────────────────────────────
// Load all entries (for UI replay)
// ─────────────────────────────────────────────────────────────────────────────

TArray<TSharedPtr<FJsonObject>> FKismetAISessionStorage::LoadAllEntries(const FString& SessionId)
{
	// The same decoder as resume; a line that does not decode is left out here
	// and reported by LoadSession.
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *GetSessionFilePath(SessionId)))
	{
		return TArray<TSharedPtr<FJsonObject>>();
	}
	return KismetAIJournalReducer::ParseJournalBytes(Bytes).Entries();
}

// ─────────────────────────────────────────────────────────────────────────────
// Re-append metadata (for tail reads)
// ─────────────────────────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FKismetAISessionStorage::BuildMetadataEntry(const FKismetAISessionInfo& Info)
{
	TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
	Meta->SetStringField(TEXT("type"), TEXT("session_meta"));
	Meta->SetStringField(TEXT("session_id"), Info.SessionId);
	Meta->SetStringField(TEXT("title"), Info.Title);
	Meta->SetStringField(TEXT("model"), Info.Model);
	Meta->SetStringField(TEXT("blueprint"), Info.BlueprintPath);
	Meta->SetStringField(TEXT("status"), Info.Status);
	Meta->SetStringField(TEXT("stop_reason"), Info.StopReason);
	Meta->SetNumberField(TEXT("cost_usd"), Info.CostUSD);
	Meta->SetNumberField(TEXT("turn_count"), Info.TurnCount);
	Meta->SetNumberField(TEXT("total_input_tokens"), Info.TotalInputTokens);
	Meta->SetNumberField(TEXT("total_output_tokens"), Info.TotalOutputTokens);

	return Meta;
}

void FKismetAISessionStorage::ReAppendMetadata(const FString& SessionId, const FKismetAISessionInfo& Info)
{
	AppendEntry(SessionId, BuildMetadataEntry(Info));
}

// ─────────────────────────────────────────────────────────────────────────────
// Delete session
// ─────────────────────────────────────────────────────────────────────────────

bool FKismetAISessionStorage::DeleteSession(const FString& SessionId)
{
	FString FilePath = GetSessionFilePath(SessionId);
	if (IFileManager::Get().Delete(*FilePath))
	{
		KISMETAI_DEV_LOG(LogKismetAISession, Log, TEXT("Deleted session: %s"), *SessionId);
		return true;
	}
	KISMETAI_LOG(LogKismetAISession, Warning, TEXT("Failed to delete session: %s"), *SessionId);
	return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// EnsurePrivateMode
// ─────────────────────────────────────────────────────────────────────────────

void FKismetAISessionStorage::EnsurePrivateMode(const FString& FilePath)
{
#if PLATFORM_MAC || PLATFORM_LINUX
	if (chmod(TCHAR_TO_UTF8(*FilePath), S_IRUSR | S_IWUSR) != 0)
	{
		KISMETAI_LOG(LogKismetAISession, Warning,
			TEXT("chmod 0600 failed for %s (errno=%d)"), *FilePath, errno);
	}
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// SweepExpiredSessions
// ─────────────────────────────────────────────────────────────────────────────

FKismetAISessionStorage::FSweepResult FKismetAISessionStorage::SweepExpiredSessions(
	const FString& Dir, int32 RetentionDays)
{
	FSweepResult Result;

	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.jsonl")), true, false);

	FDateTime Now = FDateTime::UtcNow();

	for (const FString& FileName : Files)
	{
		FString FullPath = Dir / FileName;

		FFileStatData Stat = IFileManager::Get().GetStatData(*FullPath);
		if (!Stat.bIsValid)
		{
			continue;
		}

		bool bExpired = RetentionDays > 0 &&
			(Now - Stat.ModificationTime).GetTotalDays() > RetentionDays;

		if (bExpired)
		{
			if (IFileManager::Get().Delete(*FullPath))
			{
				++Result.Deleted;
			}
			else
			{
				KISMETAI_LOG(LogKismetAISession, Warning,
					TEXT("SweepExpiredSessions: failed to delete %s"), *FullPath);
				++Result.Kept;
			}
		}
		else
		{
			EnsurePrivateMode(FullPath);
			++Result.Kept;
#if PLATFORM_MAC || PLATFORM_LINUX
			++Result.Rechmodded;
#endif
		}
	}

	return Result;
}
