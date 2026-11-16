// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// One production journal loader and recovery projection.
//
// Every test here goes through a public loading or resume entry point — a
// journal file on disk read by FKismetAISessionStorage, or FKismetAIAgent::RunTask
// resuming that file with a fake model client that records the exact outgoing
// request. A pure reducer test cannot protect the resume path on its own: the
// production loader used to be a second, separate implementation.

#include "Fakes/ScriptedAgentSeams.h"
#include "Fixtures/SessionFixtures.h"
#include "KismetAIHistoryRepair.h"
#include "KismetAIJournalReducer.h"
#include "KismetAISessionStorage.h"
#include "KismetAIToolCallJournal.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

using namespace KismetAITestFakes;

constexpr auto JournalRecoveryTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace JournalRecoveryTest
{
	FKismetAIAgentConfig MakeConfig(EThinkingMode Thinking = EThinkingMode::Disabled)
	{
		FKismetAIAgentConfig C;
		C.ApiKey = TEXT("test-key-unused");
		C.Model = TEXT("claude-sonnet-4-6");
		C.MaxSteps = 4;
		C.MaxRetries = 0;
		C.RetryJitterFactor = 0.0;
		C.ThinkingMode = Thinking;
		return C;
	}

	// A journal file written byte for byte, deleted on scope exit.
	struct FJournalFile
	{
		FString SessionId;
		explicit FJournalFile(const FString& Jsonl)
		{
			SessionId = FKismetAISessionStorage::CreateSessionId();
			FFileHelper::SaveStringToFile(Jsonl, *Path(),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}
		~FJournalFile() { FKismetAISessionStorage::DeleteSession(SessionId); }
		FString Path() const { return FKismetAISessionStorage::GetSessionFilePath(SessionId); }
		TArray<uint8> Bytes() const
		{
			TArray<uint8> Out;
			FFileHelper::LoadFileToArray(Out, *Path());
			return Out;
		}
	};

	TSharedPtr<FJsonObject> ObjOf(const TSharedPtr<FJsonValue>& V)
	{
		return V.IsValid() ? V->AsObject() : nullptr;
	}

	FString RoleOfMsg(const TSharedPtr<FJsonValue>& Msg)
	{
		FString Role;
		if (const TSharedPtr<FJsonObject> O = ObjOf(Msg)) { O->TryGetStringField(TEXT("role"), Role); }
		return Role;
	}

	FString PlainText(const TSharedPtr<FJsonValue>& Msg)
	{
		FString V;
		if (const TSharedPtr<FJsonObject> O = ObjOf(Msg)) { O->TryGetStringField(TEXT("content"), V); }
		return V;
	}

	TArray<TSharedPtr<FJsonObject>> Blocks(const TSharedPtr<FJsonValue>& Msg, const TCHAR* Type)
	{
		TArray<TSharedPtr<FJsonObject>> Out;
		const TSharedPtr<FJsonObject> O = ObjOf(Msg);
		const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
		if (O.IsValid() && O->TryGetArrayField(TEXT("content"), Content))
		{
			for (const TSharedPtr<FJsonValue>& B : *Content)
			{
				const TSharedPtr<FJsonObject> BO = ObjOf(B);
				FString T;
				if (BO.IsValid() && BO->TryGetStringField(TEXT("type"), T) && T == Type) { Out.Add(BO); }
			}
		}
		return Out;
	}

	FString ResultText(const TSharedPtr<FJsonObject>& Block)
	{
		FString V;
		if (Block.IsValid()) { Block->TryGetStringField(TEXT("content"), V); }
		return V;
	}

	bool IsErrorResult(const TSharedPtr<FJsonObject>& Block)
	{
		bool b = false;
		return Block.IsValid() && Block->TryGetBoolField(TEXT("is_error"), b) && b;
	}

	FString ToJsonString(const TSharedPtr<FJsonValue>& V)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(V, FString(), W);
		return Out;
	}

	FString Dump(const TArray<TSharedPtr<FJsonValue>>& Messages)
	{
		TArray<FString> Parts;
		for (const TSharedPtr<FJsonValue>& M : Messages) { Parts.Add(ToJsonString(M)); }
		return FString::Join(Parts, TEXT("\n"));
	}

	int32 CountThinking(const TArray<TSharedPtr<FJsonValue>>& Messages)
	{
		int32 N = 0;
		for (const TSharedPtr<FJsonValue>& M : Messages)
		{
			N += Blocks(M, TEXT("thinking")).Num() + Blocks(M, TEXT("redacted_thinking")).Num();
		}
		return N;
	}

	// Every tool_use is answered by exactly one tool_result in the next message,
	// every tool_result answers the message before it, and no id repeats.
	bool WellPaired(FAutomationTestBase& Test, const TArray<TSharedPtr<FJsonValue>>& Messages, const FString& What)
	{
		bool bOk = true;
		TSet<FString> AllIds;
		for (int32 i = 0; i < Messages.Num(); ++i)
		{
			TArray<FString> Uses;
			for (const TSharedPtr<FJsonObject>& B : Blocks(Messages[i], TEXT("tool_use"))) { Uses.Add(B->GetStringField(TEXT("id"))); }
			for (const FString& Id : Uses)
			{
				if (AllIds.Contains(Id)) { Test.AddError(What + TEXT(": tool_use id repeats: ") + Id); bOk = false; }
				AllIds.Add(Id);
			}
			if (Uses.Num() > 0)
			{
				TArray<FString> Answers;
				if (i + 1 < Messages.Num())
				{
					for (const TSharedPtr<FJsonObject>& B : Blocks(Messages[i + 1], TEXT("tool_result"))) { Answers.Add(B->GetStringField(TEXT("tool_use_id"))); }
				}
				if (Answers != Uses)
				{
					Test.AddError(FString::Printf(TEXT("%s: message %d [%s] answered by [%s]"), *What, i,
						*FString::Join(Uses, TEXT(",")), *FString::Join(Answers, TEXT(","))));
					bOk = false;
				}
			}
		}
		return bOk;
	}

	// The thinking-free view the resume request is built from. Test-local on
	// purpose: it states what "the two readers agree" means without depending on
	// the production projection under test.
	TArray<TSharedPtr<FJsonValue>> WithoutThinking(const TArray<TSharedPtr<FJsonValue>>& Messages)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const TSharedPtr<FJsonValue>& M : Messages)
		{
			const TSharedPtr<FJsonObject> O = ObjOf(M);
			const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
			if (!O.IsValid() || RoleOfMsg(M) != TEXT("assistant") || !O->TryGetArrayField(TEXT("content"), Content))
			{
				Out.Add(M);
				continue;
			}
			TArray<TSharedPtr<FJsonValue>> Kept;
			for (const TSharedPtr<FJsonValue>& B : *Content)
			{
				const TSharedPtr<FJsonObject> BO = ObjOf(B);
				FString T;
				if (BO.IsValid() && BO->TryGetStringField(TEXT("type"), T) && (T == TEXT("thinking") || T == TEXT("redacted_thinking"))) { continue; }
				Kept.Add(B);
			}
			if (Kept.Num() == 0) { continue; }
			TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>(*O);
			Copy->SetArrayField(TEXT("content"), Kept);
			Out.Add(MakeShared<FJsonValueObject>(Copy));
		}
		return Out;
	}

	const TCHAR* Correction()
	{
		return TEXT("Stop changing BP_Hero. Use a Branch node instead of the Delay, keep the Print String, and \"explain\" first.\n- exactly as typed");
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// A crash mid-append leaves a final record with no newline. The next append
// used to land on the same line, so the glued line failed to parse and the
// first record of the resumed run — the user's new instruction — vanished on
// the following resume.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJournalRecovery_UnterminatedRecordDoesNotSwallowNextAppend,
	"KismetAI.JournalRecovery.UnterminatedRecordDoesNotSwallowNextAppend", JournalRecoveryTestFlags)
bool FJournalRecovery_UnterminatedRecordDoesNotSwallowNextAppend::RunTest(const FString& Parameters)
{
	using namespace JournalRecoveryTest;
	const FString Head =
		TEXT("{\"type\":\"session_start\",\"v\":1,\"model\":\"claude-sonnet-4-6\"}\n")
		TEXT("{\"type\":\"user_message\",\"uuid\":\"u1\",\"content\":\"Add a Delay to BP_Hero.\"}\n")
		TEXT("{\"type\":\"assistant_message\",\"uuid\":\"u2\",\"content\":[{\"type\":\"text\",\"text\":\"Working on it.\"}]}\n")
		// The editor died while writing this record: no closing bytes, no newline.
		TEXT("{\"type\":\"assistant_message\",\"uuid\":\"u3\",\"content\":[{\"type\":\"te");
	FJournalFile File(Head);
	const TArray<uint8> Before = File.Bytes();

	// The production resume: RunTask loads the journal and appends the new
	// instruction as its first record.
	{
		FScriptedModelClient Model({ MakeTextTurn(TEXT("Understood.")) });
		FScriptedToolInvoker Tools;
		FKismetAIAgent Agent(Tools, Model, MakeConfig());
		Agent.RunTask(Correction(), FOnAgentStep(), TEXT(""), File.SessionId);
		TestTrue(TEXT("the first resume reached the model"), Model.NumCalls() >= 1);
		if (Model.NumCalls() >= 1)
		{
			TestEqual(TEXT("first resume: the correction is sent verbatim"),
				PlainText(Model.GetRequests()[0].Messages.Last()), FString(Correction()));
		}
	}

	const TArray<uint8> After = File.Bytes();
	bool bPrefixKept = After.Num() >= Before.Num();
	for (int32 i = 0; bPrefixKept && i < Before.Num(); ++i) { bPrefixKept = After[i] == Before[i]; }
	TestTrue(TEXT("the damaged record's original bytes are preserved"), bPrefixKept);

	// The next resume must still see that instruction.
	const TArray<TSharedPtr<FJsonValue>> Loaded = FKismetAISessionStorage::LoadMessages(File.SessionId);
	int32 Found = 0;
	for (const TSharedPtr<FJsonValue>& M : Loaded)
	{
		if (PlainText(M) == Correction()) { ++Found; }
	}
	TestEqual(TEXT("the instruction appended after the damaged record survives reload"), Found, 1);

	// The fragment is now a sealed line: a recovered truncation, located, not
	// unexplained corruption.
	const FSessionSnapshot Snap = FKismetAISessionStorage::LoadSession(File.SessionId);
	TestTrue(TEXT("sealed journal is recovered, not damaged"), Snap.Integrity == EJournalIntegrity::Recovered);
	const FJournalDiagnostic* Sealed = Snap.Diagnostics.FindByPredicate([](const FJournalDiagnostic& D)
	{
		return D.Issue == EJournalIssue::SealedTruncatedRecord;
	});
	TestNotNull(TEXT("the sealed fragment is reported"), Sealed);
	if (Sealed)
	{
		TestEqual(TEXT("on line 4"), Sealed->Line, 4);
		const FString FirstThree = Head.Left(Head.Find(TEXT("{\"type\":\"assistant_message\",\"uuid\":\"u3\"")));
		TestEqual(TEXT("at the fragment's byte offset"), Sealed->ByteOffset, (int64)FTCHARToUTF8(*FirstThree).Length());
	}
	TestTrue(TEXT("resume keeps every user message"), Snap.bUserMessagesPreserved);

	{
		FScriptedModelClient Model({ MakeTextTurn(TEXT("Done.")) });
		FScriptedToolInvoker Tools;
		FKismetAIAgent Agent(Tools, Model, MakeConfig());
		Agent.RunTask(TEXT("Continue."), FOnAgentStep(), TEXT(""), File.SessionId);
		TestTrue(TEXT("the second resume reached the model"), Model.NumCalls() >= 1);
		if (Model.NumCalls() >= 1)
		{
			int32 Sent = 0;
			for (const TSharedPtr<FJsonValue>& M : Model.GetRequests()[0].Messages)
			{
				if (PlainText(M) == Correction()) { ++Sent; }
			}
			TestEqual(TEXT("second resume: the earlier correction is still in the request"), Sent, 1);
		}
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Some OpenAI-compatible hosts number tool calls per response, so a later turn
// can reuse an earlier turn's id with different content. The old production
// loader dropped every later turn that reused ANY id, together with its
// results: a completed mutation disappeared from resumed history. The reducer
// kept it and the pairing repair renamed the id.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJournalRecovery_CrossTurnIdReuseKeepsLaterTurn,
	"KismetAI.JournalRecovery.CrossTurnIdReuseKeepsLaterTurn", JournalRecoveryTestFlags)
bool FJournalRecovery_CrossTurnIdReuseKeepsLaterTurn::RunTest(const FString& Parameters)
{
	using namespace JournalRecoveryTest;
	FJournalFile File(
		TEXT("{\"type\":\"session_start\",\"v\":1,\"model\":\"gpt-compatible\"}\n")
		TEXT("{\"type\":\"user_message\",\"uuid\":\"c1\",\"content\":\"Add nodes A and B.\"}\n")
		TEXT("{\"type\":\"assistant_message\",\"uuid\":\"c2\",\"content\":[{\"type\":\"tool_use\",\"id\":\"call_0\",\"name\":\"insert_node\",\"input\":{\"node\":\"A\"}}]}\n")
		TEXT("{\"type\":\"tool_result_message\",\"uuid\":\"c3\",\"results\":[{\"type\":\"tool_result\",\"tool_use_id\":\"call_0\",\"content\":\"ADDED_A\"}]}\n")
		TEXT("{\"type\":\"assistant_message\",\"uuid\":\"c4\",\"content\":[{\"type\":\"tool_use\",\"id\":\"call_0\",\"name\":\"insert_node\",\"input\":{\"node\":\"B\"}}]}\n")
		TEXT("{\"type\":\"tool_result_message\",\"uuid\":\"c5\",\"results\":[{\"type\":\"tool_result\",\"tool_use_id\":\"call_0\",\"content\":\"ADDED_B\"}]}\n")
		TEXT("{\"type\":\"assistant_message\",\"uuid\":\"c6\",\"content\":[{\"type\":\"text\",\"text\":\"Both added.\"}]}\n"));

	auto CheckHistory = [this](const TArray<TSharedPtr<FJsonValue>>& Messages, const FString& What)
	{
		int32 Batches = 0;
		bool bSawA = false, bSawB = false;
		for (const TSharedPtr<FJsonValue>& M : Messages)
		{
			Batches += Blocks(M, TEXT("tool_use")).Num() > 0 ? 1 : 0;
			for (const TSharedPtr<FJsonObject>& R : Blocks(M, TEXT("tool_result")))
			{
				bSawA |= ResultText(R) == TEXT("ADDED_A");
				bSawB |= ResultText(R) == TEXT("ADDED_B");
			}
		}
		TestEqual(What + TEXT(": both tool turns are kept"), Batches, 2);
		TestTrue(What + TEXT(": A's result is kept"), bSawA);
		TestTrue(What + TEXT(": B's result (the later turn) is kept"), bSawB);
		WellPaired(*this, Messages, What);
	};

	CheckHistory(FKismetAISessionStorage::LoadMessages(File.SessionId), TEXT("loader"));

	FScriptedModelClient Model({ MakeTextTurn(TEXT("Yes, both exist.")) });
	FScriptedToolInvoker Tools;
	FKismetAIAgent Agent(Tools, Model, MakeConfig());
	Agent.RunTask(TEXT("Did both nodes get added?"), FOnAgentStep(), TEXT(""), File.SessionId);
	TestTrue(TEXT("a request was made"), Model.NumCalls() >= 1);
	if (Model.NumCalls() >= 1)
	{
		CheckHistory(Model.GetRequests()[0].Messages, TEXT("outgoing request"));
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// A journal from a newer, incompatible build must not be resumed by this one:
// appending this build's records to it would leave a file neither build can
// read reliably. The source file stays byte-identical.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJournalRecovery_FutureVersionRefusesResume,
	"KismetAI.JournalRecovery.FutureVersionRefusesResume", JournalRecoveryTestFlags)
bool FJournalRecovery_FutureVersionRefusesResume::RunTest(const FString& Parameters)
{
	using namespace JournalRecoveryTest;
	FJournalFile File(
		TEXT("{\"type\":\"session_start\",\"v\":2,\"model\":\"claude-opus-9\"}\n")
		TEXT("{\"type\":\"user_message\",\"uuid\":\"f1\",\"content\":\"Add a jump.\"}\n")
		TEXT("{\"type\":\"turn\",\"schema\":2,\"messages\":[{\"role\":\"assistant\",\"parts\":[]}]}\n"));
	const TArray<uint8> Before = File.Bytes();

	TArray<FString> Events;
	FString ErrorJson;
	FScriptedModelClient Model({ MakeTextTurn(TEXT("should not be reached")) });
	FScriptedToolInvoker Tools;
	FKismetAIAgent Agent(Tools, Model, MakeConfig());
	Agent.RunTask(TEXT("Continue."),
		FOnAgentStep::CreateLambda([&Events, &ErrorJson](const FString& Type, const FString& Json)
		{
			Events.Add(Type);
			if (Type == TEXT("error")) { ErrorJson = Json; }
		}),
		TEXT(""), File.SessionId);

	TestEqual(TEXT("no model request is made"), Model.NumCalls(), 0);
	TestEqual(TEXT("no tool runs"), Tools.CallLog.Num(), 0);
	TestTrue(TEXT("the source file is byte-identical"), File.Bytes() == Before);
	TestTrue(TEXT("the run reports an error"), Events.Contains(TEXT("error")));
	TestTrue(TEXT("the error names the unsupported version"), ErrorJson.Contains(TEXT("unsupported_session_version")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// The fixture corpus through the public file loader and through the pure
// reducer must give the same resume history (modulo prior-turn thinking, which
// the resume request never carries).
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJournalRecovery_LoaderMatchesReducer,
	"KismetAI.JournalRecovery.LoaderMatchesReducer", JournalRecoveryTestFlags)
bool FJournalRecovery_LoaderMatchesReducer::RunTest(const FString& Parameters)
{
	using namespace JournalRecoveryTest;
	TArray<TPair<FString, FString>> Corpus;
	Corpus.Emplace(TEXT("completed"), KismetAISessionFixtures::Completed());
	Corpus.Emplace(TEXT("aborted mid tool"), KismetAISessionFixtures::AbortedMidTool());
	Corpus.Emplace(TEXT("failed"), KismetAISessionFixtures::Failed());
	Corpus.Emplace(TEXT("truncated tail"), KismetAISessionFixtures::TruncatedTail());
	Corpus.Emplace(TEXT("cross-turn id reuse"),
		TEXT("{\"type\":\"user_message\",\"content\":\"go\"}\n")
		TEXT("{\"type\":\"assistant_message\",\"content\":[{\"type\":\"tool_use\",\"id\":\"call_0\",\"name\":\"insert_node\",\"input\":{\"node\":\"A\"}}]}\n")
		TEXT("{\"type\":\"tool_result_message\",\"results\":[{\"type\":\"tool_result\",\"tool_use_id\":\"call_0\",\"content\":\"A\"}]}\n")
		TEXT("{\"type\":\"assistant_message\",\"content\":[{\"type\":\"tool_use\",\"id\":\"call_0\",\"name\":\"insert_node\",\"input\":{\"node\":\"B\"}}]}\n")
		TEXT("{\"type\":\"tool_result_message\",\"results\":[{\"type\":\"tool_result\",\"tool_use_id\":\"call_0\",\"content\":\"B\"}]}\n"));
	Corpus.Emplace(TEXT("legacy result entry without results"),
		TEXT("{\"type\":\"user_message\",\"content\":\"go\"}\n")
		TEXT("{\"type\":\"assistant_message\",\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]}\n")
		TEXT("{\"type\":\"tool_result_message\",\"uuid\":\"weird\"}\n")
		TEXT("{\"type\":\"user_message\",\"content\":\"again\"}\n"));

	for (const TPair<FString, FString>& Case : Corpus)
	{
		FJournalFile File(Case.Value);
		TArray<TSharedPtr<FJsonValue>> Loaded = FKismetAISessionStorage::LoadMessages(File.SessionId);
		// What RunTask does with the loaded history before the new instruction.
		KismetAIHistoryRepair::RepairToolPairing(Loaded);
		const FSessionSnapshot Snap = KismetAIJournalReducer::ReduceJsonl(Case.Value);
		const FString A = Dump(WithoutThinking(Loaded));
		const FString B = Dump(WithoutThinking(KismetAIAgentCodec::ToAnthropicArray(Snap.Messages)));
		TestEqual(Case.Key + TEXT(": production loader and reducer agree"), A, B);
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Duplicate records and thinking-bearing legacy turns, then a fresh correction:
// its exact content reaches the outgoing request, with thinking on and off.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJournalRecovery_DuplicatesAndThinkingKeepFreshCorrection,
	"KismetAI.JournalRecovery.DuplicatesAndThinkingKeepFreshCorrection", JournalRecoveryTestFlags)
bool FJournalRecovery_DuplicatesAndThinkingKeepFreshCorrection::RunTest(const FString& Parameters)
{
	using namespace JournalRecoveryTest;
	const FString Batch =
		TEXT("{\"type\":\"assistant_message\",\"uuid\":\"d2\",\"content\":[")
			TEXT("{\"type\":\"thinking\",\"thinking\":\"I will add a delay\",\"signature\":\"SIG_A\"},")
			TEXT("{\"type\":\"tool_use\",\"id\":\"toolu_D\",\"name\":\"insert_node\",\"input\":{\"node\":\"Delay\"}}]}\n");
	const FString Result =
		TEXT("{\"type\":\"tool_result_message\",\"uuid\":\"d3\",\"results\":[{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_D\",\"content\":\"DELAY_ADDED\"}]}\n");
	const FString Jsonl =
		TEXT("{\"type\":\"session_start\",\"v\":1,\"model\":\"claude-sonnet-4-6\"}\n")
		TEXT("{\"type\":\"user_message\",\"uuid\":\"d1\",\"content\":\"Add a Delay then Print String.\"}\n")
		+ Batch + Result
		// Double-written: the same records persisted twice.
		+ Batch + Result
		+ TEXT("{\"type\":\"assistant_message\",\"uuid\":\"d4\",\"content\":[")
			TEXT("{\"type\":\"redacted_thinking\",\"data\":\"OPAQUE\"},")
			TEXT("{\"type\":\"text\",\"text\":\"Added the Delay.\"}]}\n")
		+ TEXT("{\"type\":\"assistant_message\",\"uuid\":\"d5\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"only thoughts\",\"signature\":\"SIG_B\"}]}\n");

	for (const EThinkingMode Thinking : { EThinkingMode::Disabled, EThinkingMode::Enabled })
	{
		const FString Case = Thinking == EThinkingMode::Disabled ? TEXT("[thinking off]") : TEXT("[thinking on]");
		FJournalFile File(Jsonl);
		FScriptedModelClient Model({ MakeTextTurn(TEXT("Understood.")) });
		FScriptedToolInvoker Tools;
		FKismetAIAgent Agent(Tools, Model, MakeConfig(Thinking));
		Agent.RunTask(Correction(), FOnAgentStep(), TEXT(""), File.SessionId);

		TestEqual(Case + TEXT(" nothing is replayed"), Tools.CallLog.Num(), 0);
		TestTrue(Case + TEXT(" a request was made"), Model.NumCalls() >= 1);
		if (Model.NumCalls() < 1) { continue; }
		const TArray<TSharedPtr<FJsonValue>>& Sent = Model.GetRequests()[0].Messages;
		TestEqual(Case + TEXT(" the correction is the last message"), RoleOfMsg(Sent.Last()), FString(TEXT("user")));
		TestEqual(Case + TEXT(" the correction is sent byte for byte"), PlainText(Sent.Last()), FString(Correction()));
		TestTrue(Case + TEXT(" the original request survives"), Sent.Num() > 0 && PlainText(Sent[0]) == TEXT("Add a Delay then Print String."));
		int32 DelayResults = 0;
		for (const TSharedPtr<FJsonValue>& M : Sent)
		{
			for (const TSharedPtr<FJsonObject>& R : Blocks(M, TEXT("tool_result")))
			{
				DelayResults += ResultText(R) == TEXT("DELAY_ADDED") ? 1 : 0;
			}
		}
		TestEqual(Case + TEXT(" the double-written batch is sent once"), DelayResults, 1);
		TestEqual(Case + TEXT(" prior-turn thinking is not resent"), CountThinking(Sent), 0);
		WellPaired(*this, Sent, Case);
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Opt-in: real-format journals already on this machine. Not committed to the
// repo (they hold users' prompts); point KISMETAI_JOURNAL_CORPUS_DIR at one or
// more session directories (separated by ';'). Every file goes through the
// public loader and must load resumable, keep every user message, project a
// well-paired request, and stay byte-identical. Without the variable the test
// only notes that it was skipped.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJournalRecovery_LocalCorpus,
	"KismetAI.JournalRecovery.LocalCorpus", JournalRecoveryTestFlags)
bool FJournalRecovery_LocalCorpus::RunTest(const FString& Parameters)
{
	using namespace JournalRecoveryTest;
	const FString Dirs = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMETAI_JOURNAL_CORPUS_DIR"));
	if (Dirs.IsEmpty())
	{
		AddInfo(TEXT("KISMETAI_JOURNAL_CORPUS_DIR not set; local corpus skipped"));
		return true;
	}

	int32 Files = 0;
	TMap<FString, int32> ByIntegrity;
	TArray<FString> DirList;
	Dirs.ParseIntoArray(DirList, TEXT(";"));
	for (const FString& Dir : DirList)
	{
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *(Dir / TEXT("*.jsonl")), true, false);
		for (const FString& Name : Names)
		{
			const FString Path = Dir / Name;
			TArray<uint8> Before;
			FFileHelper::LoadFileToArray(Before, *Path);
			++Files;

			const FSessionSnapshot Snap = FKismetAISessionStorage::LoadSessionFile(Path);
			++ByIntegrity.FindOrAdd(FSessionSnapshot::IntegrityToString(Snap.Integrity));
			const TArray<TSharedPtr<FJsonValue>> Request = KismetAIAgentCodec::ToAnthropicArray(KismetAIJournalReducer::ProjectResumeRequest(Snap.Messages));

			TestTrue(Name + TEXT(": resumable"), Snap.CanResume());
			TestTrue(Name + TEXT(": every user message kept"), Snap.bUserMessagesPreserved);
			WellPaired(*this, Request, Name);
			// The replayed context is what resume sends; with no applied
			// projection record it is exactly the transcript.
			WellPaired(*this, KismetAIAgentCodec::ToAnthropicArray(KismetAIJournalReducer::ProjectResumeRequest(Snap.Context)), Name + TEXT(" (context)"));
			if (Snap.ContextReplay.AppliedRecords == 0)
			{
				TestTrue(Name + TEXT(": context = transcript"), Dump(KismetAIAgentCodec::ToAnthropicArray(Snap.Context)) == Dump(KismetAIAgentCodec::ToAnthropicArray(Snap.Messages)));
			}
			++ByIntegrity.FindOrAdd(FString::Printf(TEXT("context_records_applied=%d"), Snap.ContextReplay.AppliedRecords > 0 ? 1 : 0));

			TArray<uint8> After;
			FFileHelper::LoadFileToArray(After, *Path);
			TestTrue(Name + TEXT(": file untouched"), After == Before);
		}
	}
	TArray<FString> Parts;
	for (const TPair<FString, int32>& P : ByIntegrity) { Parts.Add(FString::Printf(TEXT("%s=%d"), *P.Key, P.Value)); }
	AddInfo(FString::Printf(TEXT("Local corpus: %d file(s); integrity: %s"), Files, *FString::Join(Parts, TEXT(", "))));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Adversarial: the final record is cut halfway through call B's outcome. The
// preceding records are kept, B is explicitly uncertain, the location is
// reported, and the resumed request carries both facts plus the new correction.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJournalRecovery_TruncatedOutcomeMarksCallUncertain,
	"KismetAI.JournalRecovery.TruncatedOutcomeMarksCallUncertain", JournalRecoveryTestFlags)
bool FJournalRecovery_TruncatedOutcomeMarksCallUncertain::RunTest(const FString& Parameters)
{
	using namespace JournalRecoveryTest;
	const FString Complete =
		TEXT("{\"type\":\"session_start\",\"v\":1,\"model\":\"claude-sonnet-4-6\"}\n")
		TEXT("{\"type\":\"user_message\",\"uuid\":\"t1\",\"content\":\"Rename both variables.\"}\n")
		TEXT("{\"type\":\"assistant_message\",\"uuid\":\"t2\",\"run_id\":\"r\",\"turn_id\":\"r/t0\",\"tool_call_count\":2,\"content\":[")
			TEXT("{\"type\":\"tool_use\",\"id\":\"toolu_A\",\"name\":\"manage_blueprint\",\"input\":{}},")
			TEXT("{\"type\":\"tool_use\",\"id\":\"toolu_B\",\"name\":\"manage_blueprint\",\"input\":{}}]}\n")
		TEXT("{\"type\":\"tool_result_message\",\"uuid\":\"t3\",\"record\":\"tool_call\",\"schema\":1,\"run_id\":\"r\",\"turn_id\":\"r/t0\",\"call_id\":\"r/t0/c0\",\"call_index\":0,\"call_count\":2,")
			TEXT("\"tool_use_id\":\"toolu_A\",\"tool_name\":\"manage_blueprint\",\"status\":\"succeeded\",\"attempted\":true,")
			TEXT("\"results\":[{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_A\",\"content\":\"A_RENAMED\"}]}\n");
	const FString HalfOfB =
		TEXT("{\"type\":\"tool_result_message\",\"uuid\":\"t4\",\"record\":\"tool_call\",\"schema\":1,\"run_id\":\"r\",\"turn_id\":\"r/t0\",\"call_id\":\"r/t0/c1\",\"call_index\":1,\"call_count\":2,")
		TEXT("\"tool_use_id\":\"toolu_B\",\"tool_name\":\"manage_blueprint\",\"status\":\"succ");
	FJournalFile File(Complete + HalfOfB);
	const TArray<uint8> Before = File.Bytes();

	const FSessionSnapshot Snap = FKismetAISessionStorage::LoadSession(File.SessionId);
	TestTrue(TEXT("recovered"), Snap.Integrity == EJournalIntegrity::Recovered);
	TestTrue(TEXT("resumable"), Snap.CanResume());
	TestEqual(TEXT("one unrecorded call"), Snap.UnrecordedCalls, 1);
	TestEqual(TEXT("one diagnostic"), Snap.Diagnostics.Num(), 1);
	if (Snap.Diagnostics.Num() == 1)
	{
		const FJournalDiagnostic& D = Snap.Diagnostics[0];
		TestTrue(TEXT("truncated final record"), D.Issue == EJournalIssue::TruncatedFinalRecord);
		TestEqual(TEXT("line 5"), D.Line, 5);
		TestEqual(TEXT("byte offset of the cut record"), D.ByteOffset, (int64)FTCHARToUTF8(*Complete).Length());
		TestEqual(TEXT("byte length of the cut record"), D.ByteLength, (int64)FTCHARToUTF8(*HalfOfB).Length());
		TestTrue(TEXT("excerpt shows the cut record"), D.Excerpt.StartsWith(TEXT("{\"type\":\"tool_result_message\",\"uuid\":\"t4\"")));
	}

	// The resume.
	TArray<FString> Events;
	FScriptedModelClient Model({ MakeTextTurn(TEXT("A was renamed; B is uncertain.")) });
	FScriptedToolInvoker Tools;
	FKismetAIAgent Agent(Tools, Model, MakeConfig());
	Agent.RunTask(Correction(), FOnAgentStep::CreateLambda([&Events](const FString& Type, const FString&) { Events.Add(Type); }),
		TEXT(""), File.SessionId);

	TestEqual(TEXT("nothing is replayed"), Tools.CallLog.Num(), 0);
	TestTrue(TEXT("the recovery is announced"), Events.Contains(KismetAIJournalReducer::RecoveryEntryType));
	TestTrue(TEXT("a request was made"), Model.NumCalls() >= 1);
	if (Model.NumCalls() >= 1)
	{
		const TArray<TSharedPtr<FJsonValue>>& Sent = Model.GetRequests()[0].Messages;
		TestEqual(TEXT("the correction is last, verbatim"), PlainText(Sent.Last()), FString(Correction()));
		bool bA = false, bBUncertain = false;
		for (const TSharedPtr<FJsonValue>& M : Sent)
		{
			for (const TSharedPtr<FJsonObject>& R : Blocks(M, TEXT("tool_result")))
			{
				bA |= ResultText(R) == TEXT("A_RENAMED") && !IsErrorResult(R);
				bBUncertain |= R->GetStringField(TEXT("tool_use_id")) == TEXT("toolu_B") && IsErrorResult(R)
					&& ResultText(R).Contains(KismetAIToolCallJournal::OutcomeUnrecordedCode);
			}
		}
		TestTrue(TEXT("A's recorded outcome is sent"), bA);
		TestTrue(TEXT("B is sent as unrecorded, not as a success"), bBUncertain);
		WellPaired(*this, Sent, TEXT("truncated outcome"));
	}

	// The cut bytes are still in the file; the resume sealed them and appended.
	const TArray<uint8> After = File.Bytes();
	bool bPrefix = After.Num() > Before.Num();
	for (int32 i = 0; bPrefix && i < Before.Num(); ++i) { bPrefix = After[i] == Before[i]; }
	TestTrue(TEXT("original bytes preserved as a prefix"), bPrefix);
	const TArray<TSharedPtr<FJsonObject>> Entries = FKismetAISessionStorage::LoadAllEntries(File.SessionId);
	bool bSeal = false, bRecovery = false;
	for (const TSharedPtr<FJsonObject>& E : Entries)
	{
		const FString Type = E->GetStringField(TEXT("type"));
		bSeal |= Type == KismetAIJournalReducer::SealEntryType;
		bRecovery |= Type == KismetAIJournalReducer::RecoveryEntryType;
	}
	TestTrue(TEXT("a seal record follows the cut record"), bSeal);
	TestTrue(TEXT("the recovery is journaled"), bRecovery);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Adversarial: malformed data in the middle followed by valid records. The tail
// is not omitted; the damage is located and reported as Damaged, journaled and
// announced on resume; the file is not rewritten.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJournalRecovery_MidFileCorruptionIsExplicit,
	"KismetAI.JournalRecovery.MidFileCorruptionIsExplicit", JournalRecoveryTestFlags)
bool FJournalRecovery_MidFileCorruptionIsExplicit::RunTest(const FString& Parameters)
{
	using namespace JournalRecoveryTest;
	const FString Prefix =
		TEXT("{\"type\":\"session_start\",\"v\":1,\"model\":\"claude-sonnet-4-6\"}\n")
		TEXT("{\"type\":\"user_message\",\"uuid\":\"m1\",\"content\":\"Add health.\"}\n")
		TEXT("{\"type\":\"assistant_message\",\"uuid\":\"m2\",\"content\":[{\"type\":\"text\",\"text\":\"Added.\"}]}\n");
	const FString Garbage = TEXT("{\"type\":\"user_message\",\"uuid\":\"m3\",\"content\":\"Also add st}}");
	const FString Tail =
		TEXT("{\"type\":\"user_message\",\"uuid\":\"m4\",\"content\":\"KEEP_ME: never touch BP_Boss.\"}\n")
		TEXT("{\"type\":\"assistant_message\",\"uuid\":\"m5\",\"content\":[{\"type\":\"text\",\"text\":\"Noted.\"}]}\n");
	FJournalFile File(Prefix + Garbage + TEXT("\n") + Tail);
	const TArray<uint8> Before = File.Bytes();

	const FSessionSnapshot Snap = FKismetAISessionStorage::LoadSession(File.SessionId);
	TestTrue(TEXT("damaged"), Snap.Integrity == EJournalIntegrity::Damaged);
	TestTrue(TEXT("still resumable"), Snap.CanResume());
	TestEqual(TEXT("one skipped line"), Snap.SkippedEntryCount, 1);
	const FJournalDiagnostic* Bad = Snap.Diagnostics.FindByPredicate([](const FJournalDiagnostic& D) { return D.Issue == EJournalIssue::MalformedRecord; });
	TestNotNull(TEXT("the malformed record is reported"), Bad);
	if (Bad)
	{
		TestEqual(TEXT("on line 4"), Bad->Line, 4);
		TestEqual(TEXT("at its byte offset"), Bad->ByteOffset, (int64)FTCHARToUTF8(*Prefix).Length());
	}
	bool bKept = false;
	for (const TSharedPtr<FJsonValue>& M : KismetAIAgentCodec::ToAnthropicArray(Snap.Messages)) { bKept |= PlainText(M).StartsWith(TEXT("KEEP_ME")); }
	TestTrue(TEXT("records after the damage still load"), bKept);
	TestTrue(TEXT("loading wrote nothing"), File.Bytes() == Before);

	FString RecoveryJson;
	FScriptedModelClient Model({ MakeTextTurn(TEXT("ok")) });
	FScriptedToolInvoker Tools;
	FKismetAIAgent Agent(Tools, Model, MakeConfig());
	Agent.RunTask(Correction(), FOnAgentStep::CreateLambda([&RecoveryJson](const FString& Type, const FString& Json)
		{
			if (Type == KismetAIJournalReducer::RecoveryEntryType) { RecoveryJson = Json; }
		}), TEXT(""), File.SessionId);
	TestTrue(TEXT("the damage is announced"), RecoveryJson.Contains(TEXT("\"integrity\":\"damaged\"")) && RecoveryJson.Contains(TEXT("malformed_record")));
	TestTrue(TEXT("a request was made"), Model.NumCalls() >= 1);
	if (Model.NumCalls() >= 1)
	{
		const TArray<TSharedPtr<FJsonValue>>& Sent = Model.GetRequests()[0].Messages;
		bool bSentKept = false;
		for (const TSharedPtr<FJsonValue>& M : Sent) { bSentKept |= PlainText(M).StartsWith(TEXT("KEEP_ME")); }
		TestTrue(TEXT("the instruction after the damage is sent"), bSentKept);
		TestEqual(TEXT("the correction is last, verbatim"), PlainText(Sent.Last()), FString(Correction()));
	}
	const TArray<uint8> After = File.Bytes();
	bool bPrefix = After.Num() > Before.Num();
	for (int32 i = 0; bPrefix && i < Before.Num(); ++i) { bPrefix = After[i] == Before[i]; }
	TestTrue(TEXT("the damaged bytes are preserved; the resume only appends"), bPrefix);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Versions and record schemas.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJournalRecovery_VersionsAndSchemas,
	"KismetAI.JournalRecovery.VersionsAndSchemas", JournalRecoveryTestFlags)
bool FJournalRecovery_VersionsAndSchemas::RunTest(const FString& Parameters)
{
	using namespace JournalRecoveryTest;
	const FString Body = TEXT("{\"type\":\"user_message\",\"content\":\"hi\"}\n");

	{
		const FSessionSnapshot S = KismetAIJournalReducer::ReduceJsonl(Body);
		TestEqual(TEXT("no header: legacy, version 0"), S.JournalVersion, 0);
		TestTrue(TEXT("no header: intact and resumable"), S.Integrity == EJournalIntegrity::Intact && S.CanResume());
	}
	{
		const FSessionSnapshot S = KismetAIJournalReducer::ReduceJsonl(TEXT("{\"type\":\"session_start\",\"v\":1}\n") + Body);
		TestEqual(TEXT("v1"), S.JournalVersion, 1);
		TestTrue(TEXT("v1 intact"), S.Integrity == EJournalIntegrity::Intact);
	}
	for (const TCHAR* Bad : { TEXT("2"), TEXT("\"1\""), TEXT("1.5"), TEXT("-1") })
	{
		const FSessionSnapshot S = KismetAIJournalReducer::ReduceJsonl(FString::Printf(TEXT("{\"type\":\"session_start\",\"v\":%s}\n"), Bad) + Body);
		TestTrue(FString(TEXT("v=")) + Bad + TEXT(" is unsupported"), S.Integrity == EJournalIntegrity::UnsupportedVersion && !S.CanResume());
		TestEqual(FString(TEXT("v=")) + Bad + TEXT(" entries still readable"), S.Messages.Num(), 1);
	}

	// A tool_call record from a newer schema is not interpreted: its call is
	// uncertain, never a success.
	{
		const FSessionSnapshot S = KismetAIJournalReducer::ReduceJsonl(
			TEXT("{\"type\":\"session_start\",\"v\":1}\n")
			TEXT("{\"type\":\"user_message\",\"content\":\"go\"}\n")
			TEXT("{\"type\":\"assistant_message\",\"run_id\":\"r\",\"turn_id\":\"r/t0\",\"tool_call_count\":1,\"content\":[{\"type\":\"tool_use\",\"id\":\"toolu_X\",\"name\":\"edit_blueprint\",\"input\":{}}]}\n")
			TEXT("{\"type\":\"tool_result_message\",\"record\":\"tool_call\",\"schema\":2,\"run_id\":\"r\",\"turn_id\":\"r/t0\",\"call_index\":0,\"tool_use_id\":\"toolu_X\",")
				TEXT("\"results\":[{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_X\",\"content\":\"FUTURE_SHAPE\"}]}\n"));
		TestTrue(TEXT("unknown record schema: recovered"), S.Integrity == EJournalIntegrity::Recovered);
		TestTrue(TEXT("and reported"), S.Diagnostics.ContainsByPredicate([](const FJournalDiagnostic& D) { return D.Issue == EJournalIssue::UnsupportedRecordSchema && D.Line == 4; }));
		TestEqual(TEXT("its call is unrecorded"), S.UnrecordedCalls, 1);
		bool bUnrecorded = false, bFutureUsed = false;
		for (const TSharedPtr<FJsonValue>& M : KismetAIAgentCodec::ToAnthropicArray(S.Messages))
		{
			for (const TSharedPtr<FJsonObject>& R : Blocks(M, TEXT("tool_result")))
			{
				bUnrecorded |= IsErrorResult(R) && ResultText(R).Contains(KismetAIToolCallJournal::OutcomeUnrecordedCode);
				bFutureUsed |= ResultText(R).Contains(TEXT("FUTURE_SHAPE"));
			}
		}
		TestTrue(TEXT("projected OUTCOME_UNRECORDED"), bUnrecorded);
		TestFalse(TEXT("the uninterpreted payload is not used as the outcome"), bFutureUsed);
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Duplicate record identities.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJournalRecovery_DuplicateRecordIdentities,
	"KismetAI.JournalRecovery.DuplicateRecordIdentities", JournalRecoveryTestFlags)
bool FJournalRecovery_DuplicateRecordIdentities::RunTest(const FString& Parameters)
{
	using namespace JournalRecoveryTest;
	const FString User = TEXT("{\"type\":\"user_message\",\"uuid\":\"same\",\"content\":\"Make it red.\"}\n");
	{
		const FSessionSnapshot S = KismetAIJournalReducer::ReduceJsonl(User + User
			+ TEXT("{\"type\":\"assistant_message\",\"uuid\":\"a\",\"content\":[{\"type\":\"text\",\"text\":\"Done.\"}]}\n"));
		TestEqual(TEXT("an exact repeat is dropped"), S.Messages.Num(), 2);
		TestTrue(TEXT("recovered"), S.Integrity == EJournalIntegrity::Recovered);
		TestTrue(TEXT("reported on line 2"), S.Diagnostics.ContainsByPredicate([](const FJournalDiagnostic& D) { return D.Issue == EJournalIssue::DuplicateRecord && D.Line == 2; }));
	}
	{
		const FSessionSnapshot S = KismetAIJournalReducer::ReduceJsonl(User
			+ TEXT("{\"type\":\"user_message\",\"uuid\":\"same\",\"content\":\"No - make it blue.\"}\n"));
		TestEqual(TEXT("a conflicting record under the same id is kept"), S.Messages.Num(), 2);
		TestTrue(TEXT("both user messages survive"), S.bUserMessagesPreserved && S.UserMessageEntries == 2);
		TestTrue(TEXT("and the journal is damaged"), S.Integrity == EJournalIntegrity::Damaged);
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Canonical storage keeps thinking and provider-specific data; only the
// request projection drops prior-turn thinking, and it never mutates the
// snapshot.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJournalRecovery_CanonicalKeepsProviderData,
	"KismetAI.JournalRecovery.CanonicalKeepsProviderData", JournalRecoveryTestFlags)
bool FJournalRecovery_CanonicalKeepsProviderData::RunTest(const FString& Parameters)
{
	using namespace JournalRecoveryTest;
	FJournalFile File(
		TEXT("{\"type\":\"session_start\",\"v\":1}\n")
		TEXT("{\"type\":\"user_message\",\"content\":\"go\"}\n")
		TEXT("{\"type\":\"assistant_message\",\"content\":[")
			TEXT("{\"type\":\"thinking\",\"thinking\":\"t\",\"signature\":\"SIG\"},")
			TEXT("{\"type\":\"redacted_thinking\",\"data\":\"OPAQUE\"},")
			TEXT("{\"type\":\"text\",\"text\":\"hi\",\"provider_meta\":{\"item_id\":\"rs_1\"}},")
			TEXT("{\"type\":\"server_tool_use\",\"id\":\"srv_1\",\"name\":\"web_search\"}]}\n")
		TEXT("{\"type\":\"assistant_message\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"only\",\"signature\":\"S2\"}]}\n"));

	const FSessionSnapshot Snap = FKismetAISessionStorage::LoadSession(File.SessionId);
	TestEqual(TEXT("canonical: 3 messages"), Snap.Messages.Num(), 3);
	TestEqual(TEXT("canonical: thinking kept"), CountThinking(KismetAIAgentCodec::ToAnthropicArray(Snap.Messages)), 3);
	const FString Canonical = Dump(KismetAIAgentCodec::ToAnthropicArray(Snap.Messages));
	TestTrue(TEXT("canonical: signature kept"), Canonical.Contains(TEXT("\"signature\":\"SIG\"")));
	TestTrue(TEXT("canonical: unknown field kept"), Canonical.Contains(TEXT("\"provider_meta\":{\"item_id\":\"rs_1\"}")));
	TestTrue(TEXT("canonical: unknown block kept"), Canonical.Contains(TEXT("server_tool_use")));

	const TArray<TSharedPtr<FJsonValue>> Request = KismetAIAgentCodec::ToAnthropicArray(KismetAIJournalReducer::ProjectResumeRequest(Snap.Messages));
	TestEqual(TEXT("request: thinking-only turn dropped"), Request.Num(), 2);
	TestEqual(TEXT("request: no thinking"), CountThinking(Request), 0);
	TestTrue(TEXT("request: other blocks and fields kept"), Dump(Request).Contains(TEXT("provider_meta")) && Dump(Request).Contains(TEXT("server_tool_use")));
	TestEqual(TEXT("the snapshot is not mutated by projecting"), Dump(KismetAIAgentCodec::ToAnthropicArray(Snap.Messages)), Canonical);
	TestEqual(TEXT("LoadMessages is that projection"), Dump(FKismetAISessionStorage::LoadMessages(File.SessionId)), Dump(Request));
	return true;
}
