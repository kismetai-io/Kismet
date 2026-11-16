// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAISessionStorage.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include <sys/stat.h>
#endif

constexpr auto TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// Helper: unique temp dir per test
// ─────────────────────────────────────────────────────────────────────────────

static FString MakeTempDir()
{
	FString Dir = FPaths::EngineSavedDir() / TEXT("KismetAITests") / FGuid::NewGuid().ToString();
	IFileManager::Get().MakeDirectory(*Dir, true);
	return Dir;
}

// ─────────────────────────────────────────────────────────────────────────────
// FSessionStorageEnsurePrivateMode
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionStorageEnsurePrivateMode,
	"KismetAI.SessionStorage.EnsurePrivateMode", TestFlags)

bool FSessionStorageEnsurePrivateMode::RunTest(const FString& Parameters)
{
	FString TempDir = MakeTempDir();
	FString Path = TempDir / TEXT("test_mode.jsonl");

	// Write a file
	TestTrue(TEXT("Write temp file"),
		FFileHelper::SaveStringToFile(TEXT("{\"test\":1}\n"), *Path));

#if PLATFORM_MAC || PLATFORM_LINUX
	// Force 0644 via raw chmod
	int ChmodResult = chmod(TCHAR_TO_UTF8(*Path), 0644);
	TestEqual(TEXT("chmod 0644 succeeded"), ChmodResult, 0);

	// Now apply EnsurePrivateMode
	FKismetAISessionStorage::EnsurePrivateMode(Path);

	// Verify the mode is now 0600
	struct stat St;
	int StatResult = stat(TCHAR_TO_UTF8(*Path), &St);
	TestEqual(TEXT("stat succeeded"), StatResult, 0);
	TestEqual(TEXT("Mode is 0600"), (int)(St.st_mode & 0777), (int)0600);
#endif

	IFileManager::Get().DeleteDirectory(*TempDir, false, true);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FSessionStorageSweepDeletesByAge
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionStorageSweepDeletesByAge,
	"KismetAI.SessionStorage.SweepDeletesByAge", TestFlags)

bool FSessionStorageSweepDeletesByAge::RunTest(const FString& Parameters)
{
	FString TempDir = MakeTempDir();

	// Create three files
	FString Old1 = TempDir / TEXT("old30.jsonl");
	FString Old2 = TempDir / TEXT("old10.jsonl");
	FString New1 = TempDir / TEXT("new1.jsonl");

	const FString Content = TEXT("{\"type\":\"session_meta\"}\n");
	TestTrue(TEXT("Write old30"), FFileHelper::SaveStringToFile(Content, *Old1));
	TestTrue(TEXT("Write old10"), FFileHelper::SaveStringToFile(Content, *Old2));
	TestTrue(TEXT("Write new1"),  FFileHelper::SaveStringToFile(Content, *New1));

	// Backdate the files
	FDateTime Now = FDateTime::UtcNow();
	IFileManager::Get().SetTimeStamp(*Old1, Now - FTimespan::FromDays(30.0));
	IFileManager::Get().SetTimeStamp(*Old2, Now - FTimespan::FromDays(10.0));
	IFileManager::Get().SetTimeStamp(*New1, Now - FTimespan::FromDays(1.0));

	// Sweep with 7-day retention
	FKismetAISessionStorage::FSweepResult Result =
		FKismetAISessionStorage::SweepExpiredSessions(TempDir, 7);

	// The two old files should be gone, the new one should survive
	TestFalse(TEXT("old30.jsonl deleted"), IFileManager::Get().FileExists(*Old1));
	TestFalse(TEXT("old10.jsonl deleted"), IFileManager::Get().FileExists(*Old2));
	TestTrue(TEXT("new1.jsonl survives"), IFileManager::Get().FileExists(*New1));

	TestEqual(TEXT("Deleted == 2"), Result.Deleted, 2);
	TestEqual(TEXT("Kept == 1"), Result.Kept, 1);

#if PLATFORM_MAC || PLATFORM_LINUX
	TestEqual(TEXT("Rechmodded == 1"), Result.Rechmodded, 1);
#else
	TestEqual(TEXT("Rechmodded == 0 on non-POSIX"), Result.Rechmodded, 0);
#endif

	IFileManager::Get().DeleteDirectory(*TempDir, false, true);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FSessionStorageSweepRetroChmod
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionStorageSweepRetroChmod,
	"KismetAI.SessionStorage.SweepRetroChmod", TestFlags)

bool FSessionStorageSweepRetroChmod::RunTest(const FString& Parameters)
{
#if PLATFORM_MAC || PLATFORM_LINUX
	FString TempDir = MakeTempDir();
	FString Path = TempDir / TEXT("fresh.jsonl");

	const FString Content = TEXT("{\"type\":\"session_meta\"}\n");
	TestTrue(TEXT("Write file"), FFileHelper::SaveStringToFile(Content, *Path));

	// Force 0644
	int ChmodResult = chmod(TCHAR_TO_UTF8(*Path), 0644);
	TestEqual(TEXT("chmod 0644 succeeded"), ChmodResult, 0);

	// File has fresh mtime (no age-based deletion), sweep with 30-day retention
	FKismetAISessionStorage::FSweepResult Result =
		FKismetAISessionStorage::SweepExpiredSessions(TempDir, 30);

	// File must still exist
	TestTrue(TEXT("File still exists"), IFileManager::Get().FileExists(*Path));

	// Mode must now be 0600
	struct stat St;
	int StatResult = stat(TCHAR_TO_UTF8(*Path), &St);
	TestEqual(TEXT("stat succeeded"), StatResult, 0);
	TestEqual(TEXT("Mode is 0600 after sweep"), (int)(St.st_mode & 0777), (int)0600);

	TestEqual(TEXT("Rechmodded == 1"), Result.Rechmodded, 1);
	TestEqual(TEXT("Deleted == 0"), Result.Deleted, 0);

	IFileManager::Get().DeleteDirectory(*TempDir, false, true);
#endif
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FSessionStorageSweepZeroDisablesDelete
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionStorageSweepZeroDisablesDelete,
	"KismetAI.SessionStorage.SweepZeroDisablesDelete", TestFlags)

bool FSessionStorageSweepZeroDisablesDelete::RunTest(const FString& Parameters)
{
	FString TempDir = MakeTempDir();
	FString Path = TempDir / TEXT("ancient.jsonl");

	const FString Content = TEXT("{\"type\":\"session_meta\"}\n");
	TestTrue(TEXT("Write file"), FFileHelper::SaveStringToFile(Content, *Path));

	// Backdate 1000 days
	FDateTime Now = FDateTime::UtcNow();
	IFileManager::Get().SetTimeStamp(*Path, Now - FTimespan::FromDays(1000.0));

	// Sweep with RetentionDays=0 — must not delete
	FKismetAISessionStorage::FSweepResult Result =
		FKismetAISessionStorage::SweepExpiredSessions(TempDir, 0);

	TestTrue(TEXT("File still exists when RetentionDays==0"), IFileManager::Get().FileExists(*Path));
	TestEqual(TEXT("Deleted == 0"), Result.Deleted, 0);

	IFileManager::Get().DeleteDirectory(*TempDir, false, true);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FSessionStorageRoundTrip
//
// Guards the "collect everything for restore" contract: a session that contains
// a session_start header, both-sides messages (incl. thinking + tool_use),
// tool results, an error, and a status-bearing session_meta must survive a
// write→read round-trip. Verifies LoadAllEntries (full replay), LoadMessages
// (API resume — thinking filtered, non-message entries dropped), and tail
// metadata extraction (status/stop_reason).
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionStorageRoundTrip,
	"KismetAI.SessionStorage.RoundTrip", TestFlags)

bool FSessionStorageRoundTrip::RunTest(const FString& Parameters)
{
	const FString Sid = FKismetAISessionStorage::CreateSessionId();

	// session_start header (model + system prompt captured for reproduction)
	{
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("type"), TEXT("session_start"));
		E->SetStringField(TEXT("model"), TEXT("claude-test"));
		E->SetStringField(TEXT("system_prompt"), TEXT("SYS"));
		FKismetAISessionStorage::AppendEntry(Sid, E);
	}
	// user_message
	{
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("type"), TEXT("user_message"));
		E->SetStringField(TEXT("content"), TEXT("hello"));
		FKismetAISessionStorage::AppendEntry(Sid, E);
	}
	// assistant_message with thinking + text + tool_use blocks
	{
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("type"), TEXT("assistant_message"));
		TArray<TSharedPtr<FJsonValue>> Content;
		{ TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>(); B->SetStringField(TEXT("type"), TEXT("thinking")); B->SetStringField(TEXT("thinking"), TEXT("hmm")); Content.Add(MakeShared<FJsonValueObject>(B)); }
		{ TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>(); B->SetStringField(TEXT("type"), TEXT("text")); B->SetStringField(TEXT("text"), TEXT("doing it")); Content.Add(MakeShared<FJsonValueObject>(B)); }
		{ TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>(); B->SetStringField(TEXT("type"), TEXT("tool_use")); B->SetStringField(TEXT("id"), TEXT("tc1")); B->SetStringField(TEXT("name"), TEXT("read_blueprint")); Content.Add(MakeShared<FJsonValueObject>(B)); }
		E->SetArrayField(TEXT("content"), Content);
		FKismetAISessionStorage::AppendEntry(Sid, E);
	}
	// tool_result_message
	{
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("type"), TEXT("tool_result_message"));
		TArray<TSharedPtr<FJsonValue>> Results;
		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("type"), TEXT("tool_result"));
		R->SetStringField(TEXT("tool_use_id"), TEXT("tc1"));
		R->SetStringField(TEXT("content"), TEXT("ok"));
		Results.Add(MakeShared<FJsonValueObject>(R));
		E->SetArrayField(TEXT("results"), Results);
		FKismetAISessionStorage::AppendEntry(Sid, E);
	}
	// error (the previously-missing record for a failed turn)
	{
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("type"), TEXT("error"));
		E->SetStringField(TEXT("error_code"), TEXT("overloaded"));
		E->SetStringField(TEXT("message"), TEXT("boom"));
		FKismetAISessionStorage::AppendEntry(Sid, E);
	}
	// session_meta with terminal status
	{
		FKismetAISessionInfo Info;
		Info.SessionId = Sid;
		Info.Model = TEXT("claude-test");
		Info.Status = TEXT("failed");
		Info.StopReason = TEXT("overloaded");
		FKismetAISessionStorage::ReAppendMetadata(Sid, Info);
	}

	// LoadAllEntries → every entry present for full replay.
	TArray<TSharedPtr<FJsonObject>> Entries = FKismetAISessionStorage::LoadAllEntries(Sid);
	TestEqual(TEXT("LoadAllEntries returns all 6 entries"), Entries.Num(), 6);

	auto FindType = [&Entries](const FString& T) -> TSharedPtr<FJsonObject>
	{
		for (const TSharedPtr<FJsonObject>& E : Entries)
		{
			FString Ty;
			if (E->TryGetStringField(TEXT("type"), Ty) && Ty == T) return E;
		}
		return nullptr;
	};

	TestTrue(TEXT("session_start persisted"), FindType(TEXT("session_start")).IsValid());
	TestTrue(TEXT("error persisted"), FindType(TEXT("error")).IsValid());

	TSharedPtr<FJsonObject> Meta = FindType(TEXT("session_meta"));
	TestTrue(TEXT("session_meta persisted"), Meta.IsValid());
	if (Meta.IsValid())
	{
		FString S, SR;
		Meta->TryGetStringField(TEXT("status"), S);
		Meta->TryGetStringField(TEXT("stop_reason"), SR);
		TestEqual(TEXT("session_meta status round-trips"), S, FString(TEXT("failed")));
		TestEqual(TEXT("session_meta stop_reason round-trips"), SR, FString(TEXT("overloaded")));
	}

	// LoadMessages → 3 chat messages; thinking filtered; non-messages dropped.
	TArray<TSharedPtr<FJsonValue>> Messages = FKismetAISessionStorage::LoadMessages(Sid);
	TestEqual(TEXT("LoadMessages reconstructs 3 messages"), Messages.Num(), 3);
	if (Messages.Num() >= 2)
	{
		TSharedPtr<FJsonObject> Asst = Messages[1]->AsObject();
		TestEqual(TEXT("second message is assistant"), Asst->GetStringField(TEXT("role")), FString(TEXT("assistant")));
		const TArray<TSharedPtr<FJsonValue>>* C;
		if (Asst->TryGetArrayField(TEXT("content"), C))
		{
			bool bHasThinking = false;
			for (const TSharedPtr<FJsonValue>& B : *C)
			{
				if (B->AsObject()->GetStringField(TEXT("type")) == TEXT("thinking")) bHasThinking = true;
			}
			TestFalse(TEXT("thinking filtered from resumed assistant content"), bHasThinking);
			TestEqual(TEXT("assistant keeps text + tool_use (2 blocks)"), C->Num(), 2);
		}
	}

	// Tail metadata extraction (via ListSessions) reads status + title back.
	TArray<FKismetAISessionInfo> Sessions = FKismetAISessionStorage::ListSessions(50);
	const FKismetAISessionInfo* Found = nullptr;
	for (const FKismetAISessionInfo& I : Sessions)
	{
		if (I.SessionId == Sid) { Found = &I; break; }
	}
	TestNotNull(TEXT("session appears in ListSessions"), Found);
	if (Found)
	{
		TestEqual(TEXT("ListSessions extracts status"), Found->Status, FString(TEXT("failed")));
		TestEqual(TEXT("ListSessions title is first user message"), Found->Title, FString(TEXT("hello")));
	}

	FKismetAISessionStorage::DeleteSession(Sid);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FSessionStorageDuplicateToolUseDedup
//
// A corrupted or concurrently-written session can persist the same assistant
// turn (with its tool_use id) twice. LoadMessages must drop the duplicate turn
// and its now-orphaned tool_result so the resumed history never carries two
// tool_use blocks with the same id — Anthropic rejects that with
// "tool_use ids must be unique".
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionStorageDuplicateToolUseDedup,
	"KismetAI.SessionStorage.DuplicateToolUseDedup", TestFlags)

bool FSessionStorageDuplicateToolUseDedup::RunTest(const FString& Parameters)
{
	const FString Sid = FKismetAISessionStorage::CreateSessionId();

	// Helper: append one assistant turn (text + tool_use) and its tool_result,
	// simulating a turn that was persisted to the JSONL.
	auto AppendTurn = [&Sid](const FString& ToolUseId, const FString& Text)
	{
		{
			TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("type"), TEXT("assistant_message"));
			TArray<TSharedPtr<FJsonValue>> Content;
			{ TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>(); B->SetStringField(TEXT("type"), TEXT("text")); B->SetStringField(TEXT("text"), Text); Content.Add(MakeShared<FJsonValueObject>(B)); }
			{ TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>(); B->SetStringField(TEXT("type"), TEXT("tool_use")); B->SetStringField(TEXT("id"), ToolUseId); B->SetStringField(TEXT("name"), TEXT("read_blueprint")); Content.Add(MakeShared<FJsonValueObject>(B)); }
			E->SetArrayField(TEXT("content"), Content);
			FKismetAISessionStorage::AppendEntry(Sid, E);
		}
		{
			TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("type"), TEXT("tool_result_message"));
			TArray<TSharedPtr<FJsonValue>> Results;
			TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
			R->SetStringField(TEXT("type"), TEXT("tool_result"));
			R->SetStringField(TEXT("tool_use_id"), ToolUseId);
			R->SetStringField(TEXT("content"), TEXT("ok"));
			Results.Add(MakeShared<FJsonValueObject>(R));
			E->SetArrayField(TEXT("results"), Results);
			FKismetAISessionStorage::AppendEntry(Sid, E);
		}
	};

	// user_message
	{
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("type"), TEXT("user_message"));
		E->SetStringField(TEXT("content"), TEXT("go"));
		FKismetAISessionStorage::AppendEntry(Sid, E);
	}
	// First turn, then the SAME turn persisted again (the corruption), then a
	// distinct third turn that must survive untouched.
	AppendTurn(TEXT("dup1"), TEXT("first"));
	AppendTurn(TEXT("dup1"), TEXT("first"));   // duplicate — must be dropped
	AppendTurn(TEXT("uniq2"), TEXT("second")); // distinct — must be kept

	TArray<TSharedPtr<FJsonValue>> Messages = FKismetAISessionStorage::LoadMessages(Sid);

	// Count tool_use ids across the whole resumed history — none may repeat.
	TArray<FString> AllToolUseIds;
	int32 Dup1AssistantCount = 0;
	int32 Dup1ResultCount = 0;
	for (const TSharedPtr<FJsonValue>& MsgVal : Messages)
	{
		TSharedPtr<FJsonObject> Msg = MsgVal->AsObject();
		if (!Msg.IsValid()) continue;
		const TArray<TSharedPtr<FJsonValue>>* Content;
		if (!Msg->TryGetArrayField(TEXT("content"), Content)) continue;
		for (const TSharedPtr<FJsonValue>& BlockVal : *Content)
		{
			TSharedPtr<FJsonObject> B = BlockVal->AsObject();
			if (!B.IsValid()) continue;
			const FString Type = B->GetStringField(TEXT("type"));
			if (Type == TEXT("tool_use"))
			{
				const FString Id = B->GetStringField(TEXT("id"));
				AllToolUseIds.Add(Id);
				if (Id == TEXT("dup1")) Dup1AssistantCount++;
			}
			else if (Type == TEXT("tool_result") && B->GetStringField(TEXT("tool_use_id")) == TEXT("dup1"))
			{
				Dup1ResultCount++;
			}
		}
	}

	TSet<FString> UniqueIds(AllToolUseIds);
	TestEqual(TEXT("no duplicate tool_use ids after load"), UniqueIds.Num(), AllToolUseIds.Num());
	TestEqual(TEXT("dup1 tool_use appears exactly once"), Dup1AssistantCount, 1);
	TestEqual(TEXT("dup1 tool_result appears exactly once"), Dup1ResultCount, 1);
	TestTrue(TEXT("distinct turn survives dedup"), UniqueIds.Contains(TEXT("uniq2")));

	FKismetAISessionStorage::DeleteSession(Sid);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FSessionStorageStripLeadingReferencesBlock
//
// The agent prepends a "<references>\n...\n</references>\n\n" block to a user
// message when @-refs are present. Derived titles (history panel + tabs) must
// show the real prompt, not the machine context. StripLeadingReferencesBlock
// removes only a block at the very start; a tag mid-body is left untouched.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSessionStorageStripLeadingReferencesBlock,
	"KismetAI.SessionStorage.StripLeadingReferencesBlock", TestFlags)
bool FSessionStorageStripLeadingReferencesBlock::RunTest(const FString& Parameters)
{
	// Leading block (the FormatReferencesBlock shape) is stripped to the prompt.
	{
		FString S = TEXT("<references>\n- blueprint /Game/BP_Player.BP_Player (focused)\n</references>\n\nFix the jump");
		FKismetAISessionStorage::StripLeadingReferencesBlock(S);
		TestEqual(TEXT("leading references block removed"), S, FString(TEXT("Fix the jump")));
	}

	// Multi-line refs block.
	{
		FString S = TEXT("<references>\n- blueprint /Game/A.A\n- material /Game/M.M\n</references>\n\nDo the thing");
		FKismetAISessionStorage::StripLeadingReferencesBlock(S);
		TestEqual(TEXT("multi-line block removed"), S, FString(TEXT("Do the thing")));
	}

	// No leading block — message untouched, even if it mentions the tag.
	{
		FString S = TEXT("Why does <references> show up in my title?");
		FKismetAISessionStorage::StripLeadingReferencesBlock(S);
		TestEqual(TEXT("non-leading mention untouched"), S, FString(TEXT("Why does <references> show up in my title?")));
	}

	// Plain message with no tag at all.
	{
		FString S = TEXT("hello");
		FKismetAISessionStorage::StripLeadingReferencesBlock(S);
		TestEqual(TEXT("plain message untouched"), S, FString(TEXT("hello")));
	}

	return true;
}
