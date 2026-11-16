// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "KismetAIJournalReducer.h"

class FJsonObject;
class FJsonValue;

struct FKismetAISessionInfo
{
	FString SessionId;
	FString Title;           // First user message (extracted from head)
	FString BlueprintPath;   // Last known blueprint
	FString Model;
	FString Status;          // How the session ended: completed|failed|aborted|max_steps (empty if unknown)
	FString StopReason;      // Finer-grained terminal reason (end_turn|error code|plan_ready|aborted|max_steps)
	FDateTime Created;       // From file stat
	FDateTime Modified;      // From file stat
	double CostUSD = 0.0;
	int32 TurnCount = 0;
	int32 TotalInputTokens = 0;
	int32 TotalOutputTokens = 0;
};

class KISMETAIRUNTIME_API FKismetAISessionStorage
{
public:
	struct FSweepResult
	{
		int32 Deleted = 0;
		int32 Kept = 0;
		int32 Rechmodded = 0;
	};

	// Sessions dir: {ProjectDir}/Saved/KismetAI/sessions/
	static FString GetSessionsDirectory();

	// Test seam: every journal read and write goes to Directory
	// instead. Empty restores the default. Set it only while no run is active;
	// automation tests install it per test (Tests/KismetAITestSandbox.cpp) so
	// they never write into the project's Saved folder.
	static void SetSessionsDirectoryOverrideForTests(const FString& Directory);

	// Generate UUID v4 session ID
	static FString CreateSessionId();

	// Append a single JSONL entry (JSON object + newline). Returns false when the
	// line could not be written (directory or file write failed, invalid entry).
	// If the file ends in an unterminated record (a crash mid-append), that
	// record is first terminated and a journal_seal record is written, so this
	// entry never fuses with the fragment; the fragment's bytes are kept.
	// True means the OS accepted the append; it is not an fsync — a power loss
	// can still lose the tail of the file.
	static bool AppendEntry(const FString& SessionId, TSharedPtr<FJsonObject> Entry);

	// List sessions — reads head + tail of each .jsonl for fast metadata
	// Sorted by modified time, newest first
	static TArray<FKismetAISessionInfo> ListSessions(int32 MaxCount = 50);

	// Load a session through the one journal reader (KismetAIJournalReducer):
	// the recovery projection (thinking and provider blocks kept), per-call
	// outcomes folded, pairing repaired, and an integrity report with
	// diagnostics. Read-only: the file is never rewritten or migrated.
	// Integrity is Missing when there is no file and Unreadable when it cannot
	// be read; CanResume() is false for Unreadable and UnsupportedVersion.
	static FSessionSnapshot LoadSession(const FString& SessionId);
	static FSessionSnapshot LoadSessionFile(const FString& FilePath);

	// The history a resumed request starts from: the effective context with
	// recorded compactions and synthetic messages replayed,
	// KismetAIJournalReducer::ProjectResumeRequest(LoadSession(SessionId).Context).
	static TArray<TSharedPtr<FJsonValue>> LoadMessages(const FString& SessionId);

	// Load all entries for UI replay, decoded by the same reader as resume
	// (lines that do not decode are left out; LoadSession reports them).
	static TArray<TSharedPtr<FJsonObject>> LoadAllEntries(const FString& SessionId);

	// Shared serializer; the run owns admission and checking of its terminal write.
	static TSharedPtr<FJsonObject> BuildMetadataEntry(const FKismetAISessionInfo& Info);

	// Re-append session_meta at EOF (call on session end)
	static void ReAppendMetadata(const FString& SessionId, const FKismetAISessionInfo& Info);

	// Delete a session file
	static bool DeleteSession(const FString& SessionId);

	// Get the file path for a session
	static FString GetSessionFilePath(const FString& SessionId);

	// Ensure a session file has POSIX mode 0600 (Mac/Linux only; no-op on Windows)
	static void EnsurePrivateMode(const FString& FilePath);

	// Delete expired session files and retroactively chmod kept files to 0600.
	// RetentionDays == 0 disables age-based deletion; chmod still runs.
	static FSweepResult SweepExpiredSessions(const FString& Dir, int32 RetentionDays);

	// Strip a leading "<references>...</references>" block (agent-prepended @-ref
	// context) from a user message in place, so derived titles show the prompt.
	// Public for direct unit testing.
	static void StripLeadingReferencesBlock(FString& Content);

private:
	// Read first N bytes of file
	static FString ReadHead(const FString& FilePath, int32 MaxBytes = 65536);

	// Read last N bytes of file
	static FString ReadTail(const FString& FilePath, int32 MaxBytes = 65536);

	// Extract first user message text from head content
	static FString ExtractFirstPrompt(const FString& HeadContent);

	// Extract latest session_meta fields from tail content
	static bool ExtractTailMetadata(const FString& TailContent, FKismetAISessionInfo& OutInfo);

	// Serialize a JSON object to a compact string
	static FString JsonToString(TSharedPtr<FJsonObject> Obj);
};
