// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// A resumed session must never lose the user's new instruction to
// tool_use / tool_result pairing repair.
//
// The bug: RunTask appended the new user message to the loaded history and only
// then ran PrepareHistory. When the history ended in an assistant tool batch
// with no results (an interrupted legacy batch), ValidateToolPairing saw
// [assistant(A, B), user(new instruction)], found no tool_result for A or B in
// the "next user message", and deleted both — the instruction stayed in the
// JSONL but never reached the model.
//
// These tests go through the real resume entry point: a journal file on disk,
// FKismetAIAgent::RunTask with that session id, a fake model client that
// records the exact outgoing request, and a fake tool invoker whose call log is
// the "did anything get edited" evidence.

#include "Fakes/ScriptedAgentSeams.h"
#include "KismetAIHistoryRepair.h"
#include "KismetAIJournalReducer.h"
#include "KismetAISessionStorage.h"
#include "KismetAIToolCallJournal.h"
#include "KismetAITurnContext.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"

using namespace KismetAITestFakes;

constexpr auto ResumePairingTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace ResumePairingTest
{
	FKismetAIAgentConfig MakeConfig(EThinkingMode Thinking)
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

	struct FSessionFile
	{
		FString SessionId;
		explicit FSessionFile(const FString& Jsonl)
		{
			SessionId = FKismetAISessionStorage::CreateSessionId();
			FFileHelper::SaveStringToFile(Jsonl, *FKismetAISessionStorage::GetSessionFilePath(SessionId),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}
		~FSessionFile() { FKismetAISessionStorage::DeleteSession(SessionId); }

		TArray<uint8> Bytes() const
		{
			TArray<uint8> Out;
			FFileHelper::LoadFileToArray(Out, *FKismetAISessionStorage::GetSessionFilePath(SessionId));
			return Out;
		}
	};

	TSharedPtr<FJsonObject> Obj(const TSharedPtr<FJsonValue>& V)
	{
		return V.IsValid() ? V->AsObject() : nullptr;
	}

	FString RoleOf(const TSharedPtr<FJsonValue>& Msg)
	{
		FString Role;
		if (const TSharedPtr<FJsonObject> O = Obj(Msg)) { O->TryGetStringField(TEXT("role"), Role); }
		return Role;
	}

	TArray<TSharedPtr<FJsonObject>> BlocksOfType(const TSharedPtr<FJsonValue>& Msg, const TCHAR* Type)
	{
		TArray<TSharedPtr<FJsonObject>> Out;
		const TSharedPtr<FJsonObject> O = Obj(Msg);
		const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
		if (O.IsValid() && O->TryGetArrayField(TEXT("content"), Content))
		{
			for (const TSharedPtr<FJsonValue>& B : *Content)
			{
				const TSharedPtr<FJsonObject> BO = Obj(B);
				FString T;
				if (BO.IsValid() && BO->TryGetStringField(TEXT("type"), T) && T == Type) { Out.Add(BO); }
			}
		}
		return Out;
	}

	TArray<FString> ToolUseIds(const TSharedPtr<FJsonValue>& Msg)
	{
		TArray<FString> Ids;
		for (const TSharedPtr<FJsonObject>& B : BlocksOfType(Msg, TEXT("tool_use"))) { Ids.Add(B->GetStringField(TEXT("id"))); }
		return Ids;
	}

	TArray<FString> ToolResultIds(const TSharedPtr<FJsonValue>& Msg)
	{
		TArray<FString> Ids;
		for (const TSharedPtr<FJsonObject>& B : BlocksOfType(Msg, TEXT("tool_result"))) { Ids.Add(B->GetStringField(TEXT("tool_use_id"))); }
		return Ids;
	}

	bool IsError(const TSharedPtr<FJsonObject>& Block)
	{
		bool b = false;
		return Block.IsValid() && Block->TryGetBoolField(TEXT("is_error"), b) && b;
	}

	FString ContentOf(const TSharedPtr<FJsonObject>& Block)
	{
		FString V;
		if (Block.IsValid()) { Block->TryGetStringField(TEXT("content"), V); }
		return V;
	}

	FString PlainContent(const TSharedPtr<FJsonValue>& Msg)
	{
		FString V;
		if (const TSharedPtr<FJsonObject> O = Obj(Msg)) { O->TryGetStringField(TEXT("content"), V); }
		return V;
	}

	// Every tool_use in the request has exactly one tool_result in the very next
	// message, every tool_result answers a tool_use right before it, and no
	// tool_use id repeats anywhere. This is what providers enforce.
	bool RequestIsWellPaired(FAutomationTestBase& Test, const TArray<TSharedPtr<FJsonValue>>& Messages, const FString& What)
	{
		bool bOk = true;
		TSet<FString> AllIds;
		for (int32 i = 0; i < Messages.Num(); ++i)
		{
			const TArray<FString> Uses = ToolUseIds(Messages[i]);
			for (const FString& Id : Uses)
			{
				if (AllIds.Contains(Id))
				{
					Test.AddError(FString::Printf(TEXT("%s: tool_use id '%s' repeats"), *What, *Id));
					bOk = false;
				}
				AllIds.Add(Id);
			}
			if (Uses.Num() > 0)
			{
				const TArray<FString> Results = (i + 1 < Messages.Num()) ? ToolResultIds(Messages[i + 1]) : TArray<FString>();
				if (Results != Uses)
				{
					Test.AddError(FString::Printf(TEXT("%s: message %d's tool_use [%s] is answered by [%s]"),
						*What, i, *FString::Join(Uses, TEXT(",")), *FString::Join(Results, TEXT(","))));
					bOk = false;
				}
			}
			if (ToolResultIds(Messages[i]).Num() > 0 && (i == 0 || ToolUseIds(Messages[i - 1]).Num() == 0))
			{
				Test.AddError(FString::Printf(TEXT("%s: message %d carries tool_result with no tool_use before it"), *What, i));
				bOk = false;
			}
		}
		return bOk;
	}

	int32 FindBatch(const TArray<TSharedPtr<FJsonValue>>& Messages, const FString& FirstToolUseId)
	{
		for (int32 i = 0; i < Messages.Num(); ++i)
		{
			const TArray<FString> Ids = ToolUseIds(Messages[i]);
			if (Ids.Num() > 0 && Ids[0] == FirstToolUseId) { return i; }
		}
		return INDEX_NONE;
	}

	TArray<TSharedPtr<FJsonValue>> DeepCopy(const TArray<TSharedPtr<FJsonValue>>& In)
	{
		// Through text, so nothing is shared with the original.
		FString Text;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Text);
		FJsonSerializer::Serialize(In, W);
		TArray<TSharedPtr<FJsonValue>> Out;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
		FJsonSerializer::Deserialize(R, Out);
		return Out;
	}

	bool SameJson(const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		return A.IsValid() && B.IsValid() && FJsonValue::CompareEqual(*A, *B);
	}

	bool SameMessages(const TArray<TSharedPtr<FJsonValue>>& A, const TArray<TSharedPtr<FJsonValue>>& B)
	{
		if (A.Num() != B.Num()) { return false; }
		for (int32 i = 0; i < A.Num(); ++i)
		{
			if (!SameJson(A[i], B[i])) { return false; }
		}
		return true;
	}

	bool StartsWith(const TArray<uint8>& Whole, const TArray<uint8>& Prefix)
	{
		return Whole.Num() >= Prefix.Num() && FMemory::Memcmp(Whole.GetData(), Prefix.GetData(), Prefix.Num()) == 0;
	}

	const TCHAR* StopInstruction()
	{
		return TEXT("Stop editing; only explain what you changed so far and what is left. Do not call any edit tool.");
	}

	// The interrupted two-tool batch, in either journal generation.
	//  bStamped=false: an older journal — batch written, no results at all.
	//  bStamped=true:  a journal — A recorded, B never recorded (crash).
	FString InterruptedBatchJournal(bool bStamped, bool bWithThinking)
	{
		const FString Thinking = bWithThinking
			? TEXT("{\"type\":\"thinking\",\"thinking\":\"Two edits.\",\"signature\":\"sig-abc\"},")
			: TEXT("");
		const FString Stamp = bStamped
			? TEXT("\"run_id\":\"r\",\"turn_id\":\"r/t0\",\"tool_call_count\":2,")
			: TEXT("");

		FString J;
		J += TEXT("{\"type\":\"session_start\",\"v\":1,\"model\":\"claude-sonnet-4-6\"}\n");
		J += TEXT("{\"type\":\"user_message\",\"content\":\"Rename Health to HitPoints in BP_Hero and wire it to the HUD.\"}\n");
		J += TEXT("{\"type\":\"assistant_message\",") + Stamp + TEXT("\"content\":[") + Thinking
			+ TEXT("{\"type\":\"text\",\"text\":\"Editing both nodes.\"},")
			+ TEXT("{\"type\":\"tool_use\",\"id\":\"toolu_A\",\"name\":\"edit_blueprint\",\"input\":{\"blueprint\":\"/Game/BP_Hero\"}},")
			+ TEXT("{\"type\":\"tool_use\",\"id\":\"toolu_B\",\"name\":\"edit_blueprint\",\"input\":{\"blueprint\":\"/Game/BP_Hero\"}}]}\n");
		if (bStamped)
		{
			J += TEXT("{\"type\":\"tool_result_message\",\"record\":\"tool_call\",\"schema\":1,\"run_id\":\"r\",\"turn_id\":\"r/t0\",\"call_id\":\"r/t0/c0\",")
				TEXT("\"call_index\":0,\"call_count\":2,\"tool_use_id\":\"toolu_A\",\"tool_name\":\"edit_blueprint\",\"status\":\"succeeded\",\"attempted\":true,")
				TEXT("\"results\":[{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_A\",\"content\":\"A_REAL_RESULT\"}]}\n");
		}
		J += TEXT("{\"type\":\"session_meta\",\"status\":\"aborted\",\"stop_reason\":\"aborted\"}\n");
		return J;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// The R1 / F2 reproduction: "Stop editing; only explain" after an interrupted
// two-tool batch. Legacy and stamped journals, thinking on and off.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResumePairing_StopInstructionSurvives,
	"KismetAI.ResumePairing.StopInstructionSurvivesInterruptedBatch", ResumePairingTestFlags)
bool FResumePairing_StopInstructionSurvives::RunTest(const FString& Parameters)
{
	using namespace ResumePairingTest;
	for (const bool bStamped : { false, true })
	{
		for (const EThinkingMode Thinking : { EThinkingMode::Disabled, EThinkingMode::Enabled })
		{
			const bool bThinking = Thinking != EThinkingMode::Disabled;
			const FString Case = FString::Printf(TEXT("[%s, thinking %s]"),
				bStamped ? TEXT("stamped") : TEXT("legacy"), bThinking ? TEXT("on") : TEXT("off"));

			FSessionFile File(InterruptedBatchJournal(bStamped, bThinking));
			const TArray<uint8> BytesBefore = File.Bytes();

			FScriptedModelClient Model({ KismetAITestFakes::MakeTextTurn(TEXT("A may have been applied; B never ran. Nothing else will be edited.")) });
			FScriptedToolInvoker Tools;
			FKismetAIAgent Agent(Tools, Model, MakeConfig(Thinking));
			Agent.RunTask(StopInstruction(), FOnAgentStep(), TEXT(""), File.SessionId);

			TestEqual(Case + TEXT(" no tool was dispatched"), Tools.CallLog.Num(), 0);
			TestTrue(Case + TEXT(" a request was made"), Model.NumCalls() >= 1);
			if (Model.NumCalls() < 1) { continue; }
			const TArray<TSharedPtr<FJsonValue>>& Sent = Model.GetRequests()[0].Messages;

			// The whole instruction is the last message, verbatim.
			TestEqual(Case + TEXT(" the new instruction is the last message"), RoleOf(Sent.Last()), FString(TEXT("user")));
			TestEqual(Case + TEXT(" the new instruction is sent in full"), PlainContent(Sent.Last()), FString(StopInstruction()));

			// The old request is still the first message.
			TestTrue(Case + TEXT(" the original request survives"), Sent.Num() > 0 && PlainContent(Sent[0]).Contains(TEXT("HitPoints")));

			// The interrupted batch is kept and answered honestly.
			const int32 Batch = FindBatch(Sent, TEXT("toolu_A"));
			TestTrue(Case + TEXT(" the interrupted batch is kept"), Batch != INDEX_NONE && Batch + 1 < Sent.Num());
			if (Batch != INDEX_NONE && Batch + 1 < Sent.Num())
			{
				const TArray<TSharedPtr<FJsonObject>> Results = BlocksOfType(Sent[Batch + 1], TEXT("tool_result"));
				TestEqual(Case + TEXT(" one result per call"), Results.Num(), 2);
				if (Results.Num() == 2)
				{
					if (bStamped)
					{
						TestTrue(Case + TEXT(" A keeps its recorded result"), ContentOf(Results[0]).Contains(TEXT("A_REAL_RESULT")));
						TestFalse(Case + TEXT(" A's recorded result is not turned into an error"), IsError(Results[0]));
					}
					else
					{
						TestTrue(Case + TEXT(" A's outcome is unknown"), IsError(Results[0])
							&& ContentOf(Results[0]).Contains(KismetAIToolCallJournal::OutcomeUnrecordedCode));
					}
					TestTrue(Case + TEXT(" B's outcome is unknown, not a success"), IsError(Results[1])
						&& ContentOf(Results[1]).Contains(KismetAIToolCallJournal::OutcomeUnrecordedCode));
				}
			}
			RequestIsWellPaired(*this, Sent, Case);

			// Repair is a projection: the journal's original bytes are untouched
			// (the resumed run only appends after them).
			const TArray<uint8> BytesAfter = File.Bytes();
			TestTrue(Case + TEXT(" the original journal bytes are unchanged"), StartsWith(BytesAfter, BytesBefore));
			TestTrue(Case + TEXT(" the resumed run appended its own entries"), BytesAfter.Num() > BytesBefore.Num());
		}
	}
	return true;
}

// Loading and repairing alone writes nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResumePairing_RepairNeverWritesJournal,
	"KismetAI.ResumePairing.RepairNeverWritesJournal", ResumePairingTestFlags)
bool FResumePairing_RepairNeverWritesJournal::RunTest(const FString& Parameters)
{
	using namespace ResumePairingTest;
	FSessionFile File(InterruptedBatchJournal(/*bStamped=*/false, /*bWithThinking=*/true));
	const TArray<uint8> Before = File.Bytes();

	TArray<TSharedPtr<FJsonValue>> Messages = FKismetAISessionStorage::LoadMessages(File.SessionId);
	KismetAIHistoryRepair::RepairToolPairing(Messages);
	FKismetAIContextPolicy::PrepareHistory(Messages, /*bThinkingEnabled=*/true);
	FKismetAIContextPolicy::PrepareHistory(Messages, /*bThinkingEnabled=*/false);

	TestTrue(TEXT("journal bytes identical after load + repair"), File.Bytes() == Before);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// A genuine structured user message (text + image) right after an incomplete
// call keeps every block through repeated normalization.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResumePairing_StructuredUserMessageSurvives,
	"KismetAI.ResumePairing.StructuredUserMessageSurvives", ResumePairingTestFlags)
bool FResumePairing_StructuredUserMessageSurvives::RunTest(const FString& Parameters)
{
	using namespace ResumePairingTest;
	const FString Jsonl =
		TEXT("{\"type\":\"session_start\",\"v\":1,\"model\":\"claude-sonnet-4-6\"}\n")
		TEXT("{\"type\":\"user_message\",\"content\":\"Add a jump to BP_Hero.\"}\n")
		TEXT("{\"type\":\"assistant_message\",\"content\":[")
			TEXT("{\"type\":\"thinking\",\"thinking\":\"x\",\"signature\":\"s\"},")
			TEXT("{\"type\":\"tool_use\",\"id\":\"toolu_J\",\"name\":\"edit_blueprint\",\"input\":{}}]}\n")
		// Queued while the batch was running: a screenshot and a correction.
		TEXT("{\"type\":\"user_message\",\"content\":\"Not like that - match this screenshot exactly.\",")
			TEXT("\"images\":[{\"media_type\":\"image/png\",\"data\":\"iVBORw0KGgoAAAANSUhEUg==\"}]}\n");

	for (const EThinkingMode Thinking : { EThinkingMode::Disabled, EThinkingMode::Enabled })
	{
		const bool bThinking = Thinking != EThinkingMode::Disabled;
		const FString Case = bThinking ? TEXT("[thinking on]") : TEXT("[thinking off]");
		FSessionFile File(Jsonl);

		// --- the pure path: load, then normalize repeatedly ---
		TArray<TSharedPtr<FJsonValue>> Loaded = FKismetAISessionStorage::LoadMessages(File.SessionId);
		// The one loader repairs pairing itself, so the unanswered call is
		// already answered (OUTCOME_UNRECORDED) before the image message.
		TestEqual(Case + TEXT(" loader: user, assistant, results, user(image)"), Loaded.Num(), 4);
		if (Loaded.Num() != 4) { return false; }
		TestTrue(Case + TEXT(" loader answers the open call as unrecorded"),
			BlocksOfType(Loaded[2], TEXT("tool_result")).Num() == 1
			&& ContentOf(BlocksOfType(Loaded[2], TEXT("tool_result"))[0]).Contains(KismetAIToolCallJournal::OutcomeUnrecordedCode));
		const TArray<TSharedPtr<FJsonValue>> Original = DeepCopy(Loaded);
		const TSharedPtr<FJsonValue> Structured = Original[3];

		KismetAIHistoryRepair::RepairToolPairing(Loaded);
		const TArray<TSharedPtr<FJsonValue>> AfterFirst = DeepCopy(Loaded);
		for (int32 Pass = 0; Pass < 3; ++Pass)
		{
			FKismetAIContextPolicy::PrepareHistory(Loaded, bThinking);
			KismetAIHistoryRepair::RepairToolPairing(Loaded);
		}
		TestTrue(Case + TEXT(" normalization is idempotent"), SameMessages(Loaded, AfterFirst));

		int32 Kept = 0;
		for (const TSharedPtr<FJsonValue>& M : Loaded)
		{
			if (SameJson(M, Structured)) { ++Kept; }
		}
		TestEqual(Case + TEXT(" the structured message survives exactly once, byte for byte"), Kept, 1);
		RequestIsWellPaired(*this, Loaded, Case + TEXT(" normalized"));

		// --- the real resume path ---
		FScriptedModelClient Model({ KismetAITestFakes::MakeTextTurn(TEXT("Understood.")) });
		FScriptedToolInvoker Tools;
		FKismetAIAgent Agent(Tools, Model, MakeConfig(Thinking));
		Agent.RunTask(TEXT("Explain first."), FOnAgentStep(), TEXT(""), File.SessionId);

		TestEqual(Case + TEXT(" no tool was dispatched"), Tools.CallLog.Num(), 0);
		TestTrue(Case + TEXT(" a request was made"), Model.NumCalls() >= 1);
		if (Model.NumCalls() < 1) { continue; }
		const TArray<TSharedPtr<FJsonValue>>& Sent = Model.GetRequests()[0].Messages;
		int32 SentKept = 0;
		for (const TSharedPtr<FJsonValue>& M : Sent)
		{
			if (SameJson(M, Structured)) { ++SentKept; }
		}
		TestEqual(Case + TEXT(" the outgoing request carries the image message intact"), SentKept, 1);
		const TArray<TSharedPtr<FJsonObject>> Images = Sent.Num() > 0 ? BlocksOfType(Structured, TEXT("image")) : TArray<TSharedPtr<FJsonObject>>();
		TestEqual(Case + TEXT(" the image block is there"), Images.Num(), 1);
		RequestIsWellPaired(*this, Sent, Case + TEXT(" sent"));
		TestEqual(Case + TEXT(" the newest instruction is last"), PlainContent(Sent.Last()), FString(TEXT("Explain first.")));
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Duplicate tool_use ids with one result missing: deterministic, and no result
// is ever paired with a call it cannot be shown to belong to.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResumePairing_DuplicateIdsNeverMisPair,
	"KismetAI.ResumePairing.DuplicateIdsNeverMisPair", ResumePairingTestFlags)
bool FResumePairing_DuplicateIdsNeverMisPair::RunTest(const FString& Parameters)
{
	using namespace ResumePairingTest;
	// Legacy: two calls share an id and one result was recorded under it. Which
	// call it belongs to is unknowable, so neither call is given it as its result.
	const FString Legacy =
		TEXT("{\"type\":\"user_message\",\"content\":\"Fix the HUD.\"}\n")
		TEXT("{\"type\":\"assistant_message\",\"content\":[")
			TEXT("{\"type\":\"tool_use\",\"id\":\"call_0\",\"name\":\"edit_blueprint\",\"input\":{}},")
			TEXT("{\"type\":\"tool_use\",\"id\":\"call_0\",\"name\":\"compile_blueprint\",\"input\":{}}]}\n")
		TEXT("{\"type\":\"tool_result_message\",\"results\":[")
			TEXT("{\"type\":\"tool_result\",\"tool_use_id\":\"call_0\",\"content\":\"RECORDED_ONCE\"}]}\n");

	// Stamped: call_index makes the pairing exact even though the ids collide.
	const FString Stamped =
		TEXT("{\"type\":\"user_message\",\"content\":\"Fix the HUD.\"}\n")
		TEXT("{\"type\":\"assistant_message\",\"run_id\":\"r\",\"turn_id\":\"r/t0\",\"tool_call_count\":2,\"content\":[")
			TEXT("{\"type\":\"tool_use\",\"id\":\"call_0\",\"name\":\"edit_blueprint\",\"input\":{}},")
			TEXT("{\"type\":\"tool_use\",\"id\":\"call_0\",\"name\":\"compile_blueprint\",\"input\":{}}]}\n")
		TEXT("{\"type\":\"tool_result_message\",\"record\":\"tool_call\",\"schema\":1,\"run_id\":\"r\",\"turn_id\":\"r/t0\",\"call_id\":\"r/t0/c0\",")
			TEXT("\"call_index\":0,\"call_count\":2,\"tool_use_id\":\"call_0\",\"tool_name\":\"edit_blueprint\",\"status\":\"succeeded\",\"attempted\":true,")
			TEXT("\"results\":[{\"type\":\"tool_result\",\"tool_use_id\":\"call_0\",\"content\":\"RECORDED_ONCE\"}]}\n");

	for (const bool bStamped : { false, true })
	{
		const FString Case = bStamped ? TEXT("[stamped]") : TEXT("[legacy]");
		FSessionFile File(bStamped ? Stamped : Legacy);

		TArray<TSharedPtr<FJsonValue>> FirstSent;
		for (int32 Run = 0; Run < 2; ++Run)
		{
			// The same file, resumed twice from scratch: the projection must not
			// depend on anything but the journal.
			FSessionFile Copy(bStamped ? Stamped : Legacy);
			FScriptedModelClient Model({ KismetAITestFakes::MakeTextTurn(TEXT("ok")) });
			FScriptedToolInvoker Tools;
			FKismetAIAgent Agent(Tools, Model, MakeConfig(EThinkingMode::Disabled));
			Agent.RunTask(TEXT("What happened?"), FOnAgentStep(), TEXT(""), Copy.SessionId);

			TestEqual(Case + TEXT(" nothing replayed"), Tools.CallLog.Num(), 0);
			TestTrue(Case + TEXT(" a request was made"), Model.NumCalls() >= 1);
			if (Model.NumCalls() < 1) { return false; }
			const TArray<TSharedPtr<FJsonValue>> Sent = DeepCopy(Model.GetRequests()[0].Messages);
			RequestIsWellPaired(*this, Sent, Case);
			TestEqual(Case + TEXT(" the question is last"), PlainContent(Sent.Last()), FString(TEXT("What happened?")));

			const int32 Batch = FindBatch(Sent, TEXT("call_0"));
			TestTrue(Case + TEXT(" both calls are kept"), Batch != INDEX_NONE && ToolUseIds(Sent[Batch]).Num() == 2);
			if (Batch == INDEX_NONE || Batch + 1 >= Sent.Num()) { return false; }
			const TArray<TSharedPtr<FJsonObject>> Results = BlocksOfType(Sent[Batch + 1], TEXT("tool_result"));
			TestEqual(Case + TEXT(" one result per call"), Results.Num(), 2);
			if (Results.Num() != 2) { return false; }

			if (bStamped)
			{
				TestTrue(Case + TEXT(" call 0 has its recorded result"), ContentOf(Results[0]) == TEXT("RECORDED_ONCE") && !IsError(Results[0]));
				TestTrue(Case + TEXT(" call 1 is unrecorded, not handed call 0's result"), IsError(Results[1])
					&& ContentOf(Results[1]).Contains(KismetAIToolCallJournal::OutcomeUnrecordedCode));
			}
			else
			{
				for (const TSharedPtr<FJsonObject>& R : Results)
				{
					TestTrue(Case + TEXT(" an unattributable result is an error"), IsError(R));
					TestTrue(Case + TEXT(" and says the outcome is ambiguous"), ContentOf(R).Contains(KismetAIHistoryRepair::OutcomeAmbiguousCode));
				}
			}

			if (Run == 0)
			{
				FirstSent = Sent;
			}
			else
			{
				TestTrue(Case + TEXT(" recovery is deterministic"), SameMessages(FirstSent, Sent));
			}
		}
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// The repair itself, on in-memory history (it also runs before every model call
// and after a trim, not only on resume).
// ─────────────────────────────────────────────────────────────────────────────

namespace ResumePairingTest
{
	TSharedPtr<FJsonValue> Msg(const TCHAR* Role, const TArray<TSharedPtr<FJsonObject>>& Blocks)
	{
		TSharedPtr<FJsonObject> M = MakeShared<FJsonObject>();
		M->SetStringField(TEXT("role"), Role);
		TArray<TSharedPtr<FJsonValue>> Content;
		for (const TSharedPtr<FJsonObject>& B : Blocks) { Content.Add(MakeShared<FJsonValueObject>(B)); }
		M->SetArrayField(TEXT("content"), Content);
		return MakeShared<FJsonValueObject>(M);
	}

	TSharedPtr<FJsonValue> PlainUser(const TCHAR* Text)
	{
		TSharedPtr<FJsonObject> M = MakeShared<FJsonObject>();
		M->SetStringField(TEXT("role"), TEXT("user"));
		M->SetStringField(TEXT("content"), Text);
		return MakeShared<FJsonValueObject>(M);
	}

	TSharedPtr<FJsonObject> Result(const TCHAR* Id, const TCHAR* Content)
	{
		TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>();
		B->SetStringField(TEXT("type"), TEXT("tool_result"));
		B->SetStringField(TEXT("tool_use_id"), Id);
		B->SetStringField(TEXT("content"), Content);
		return B;
	}

	TSharedPtr<FJsonObject> Image()
	{
		TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("type"), TEXT("base64"));
		Source->SetStringField(TEXT("media_type"), TEXT("image/png"));
		Source->SetStringField(TEXT("data"), TEXT("AAAA"));
		TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>();
		B->SetStringField(TEXT("type"), TEXT("image"));
		B->SetObjectField(TEXT("source"), Source);
		return B;
	}

	int32 CountSame(const TArray<TSharedPtr<FJsonValue>>& Messages, const TSharedPtr<FJsonValue>& Expected)
	{
		int32 N = 0;
		for (const TSharedPtr<FJsonValue>& M : Messages) { if (SameJson(M, Expected)) { ++N; } }
		return N;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResumePairing_GenuineUserMessagesArePreserved,
	"KismetAI.ResumePairing.GenuineUserMessagesArePreserved", ResumePairingTestFlags)
bool FResumePairing_GenuineUserMessagesArePreserved::RunTest(const FString& Parameters)
{
	using namespace ResumePairingTest;
	const TSharedPtr<FJsonValue> First = PlainUser(TEXT("first request"));
	const TSharedPtr<FJsonValue> Structured = Msg(TEXT("user"), { KismetAITestFakes::MakeTextBlock(TEXT("look at this")), Image() });
	const TSharedPtr<FJsonValue> Plain = PlainUser(TEXT("stop; only explain"));

	struct FShape { const TCHAR* Name; TArray<TSharedPtr<FJsonValue>> Messages; TArray<TSharedPtr<FJsonValue>> MustSurvive; };
	TArray<FShape> Shapes;

	// Batch with no results, then a plain instruction.
	Shapes.Add({ TEXT("unanswered batch + plain"), {
		First,
		Msg(TEXT("assistant"), { KismetAITestFakes::MakeToolUseBlock(TEXT("a"), TEXT("edit_blueprint"), nullptr), KismetAITestFakes::MakeToolUseBlock(TEXT("b"), TEXT("edit_blueprint"), nullptr) }),
		Plain }, { First, Plain } });

	// Batch with no results, then a structured message, then a plain one.
	Shapes.Add({ TEXT("unanswered batch + structured + plain"), {
		First,
		Msg(TEXT("assistant"), { KismetAITestFakes::MakeToolUseBlock(TEXT("c"), TEXT("edit_blueprint"), nullptr) }),
		Structured, Plain }, { First, Structured, Plain } });

	// Partial results, then the instruction.
	Shapes.Add({ TEXT("partial results + plain"), {
		First,
		Msg(TEXT("assistant"), { KismetAITestFakes::MakeToolUseBlock(TEXT("d"), TEXT("edit_blueprint"), nullptr), KismetAITestFakes::MakeToolUseBlock(TEXT("e"), TEXT("edit_blueprint"), nullptr) }),
		Msg(TEXT("user"), { Result(TEXT("d"), TEXT("done-d")) }),
		Plain }, { First, Plain } });

	// A batch followed directly by another assistant message, then the instruction.
	Shapes.Add({ TEXT("batch then assistant text"), {
		First,
		Msg(TEXT("assistant"), { KismetAITestFakes::MakeToolUseBlock(TEXT("f"), TEXT("edit_blueprint"), nullptr) }),
		Msg(TEXT("assistant"), { KismetAITestFakes::MakeTextBlock(TEXT("hmm")) }),
		Structured }, { First, Structured } });

	// An orphaned result envelope (its batch was trimmed) right before the instruction.
	Shapes.Add({ TEXT("orphan envelope"), {
		First,
		Msg(TEXT("assistant"), { KismetAITestFakes::MakeTextBlock(TEXT("ok")) }),
		Msg(TEXT("user"), { Result(TEXT("gone"), TEXT("x")) }),
		Plain }, { First, Plain } });

	// Trailing unanswered batch with nothing after it (the reducer's shape).
	Shapes.Add({ TEXT("trailing batch"), {
		First,
		Msg(TEXT("assistant"), { KismetAITestFakes::MakeToolUseBlock(TEXT("g"), TEXT("edit_blueprint"), nullptr) }) }, { First } });

	for (FShape& Shape : Shapes)
	{
		for (const bool bThinking : { false, true })
		{
			const FString Case = FString::Printf(TEXT("[%s, thinking %s]"), Shape.Name, bThinking ? TEXT("on") : TEXT("off"));
			TArray<TSharedPtr<FJsonValue>> Messages = DeepCopy(Shape.Messages);

			FKismetAIContextPolicy::PrepareHistory(Messages, bThinking);
			const TArray<TSharedPtr<FJsonValue>> Once = DeepCopy(Messages);
			FKismetAIContextPolicy::PrepareHistory(Messages, bThinking);
			FKismetAIAgent::ValidateToolPairing(Messages);

			for (const TSharedPtr<FJsonValue>& Must : Shape.MustSurvive)
			{
				TestEqual(Case + TEXT(" genuine user message kept exactly once"), CountSame(Messages, Must), 1);
			}
			RequestIsWellPaired(*this, Messages, Case);
			TestTrue(Case + TEXT(" idempotent"), SameMessages(Messages, Once));

			// Every synthesized result is an honest error, never a success.
			for (const TSharedPtr<FJsonValue>& M : Messages)
			{
				for (const TSharedPtr<FJsonObject>& R : BlocksOfType(M, TEXT("tool_result")))
				{
					const FString Body = ContentOf(R);
					if (Body.StartsWith(TEXT("done-"))) { continue; }
					TestTrue(Case + TEXT(" a missing result is an error"), IsError(R));
					TestTrue(Case + TEXT(" and says the outcome is unknown"), Body.Contains(KismetAIToolCallJournal::OutcomeUnrecordedCode));
				}
			}
		}
	}
	return true;
}

// A later turn that reuses an earlier turn's tool_use id with DIFFERENT content
// (some OpenAI-compatible hosts number calls per response) is a real turn, not
// a re-persisted copy: it is kept, renamed deterministically together with its
// own adjacent results, and never paired with the earlier turn's result.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResumePairing_ReusedIdAcrossTurns,
	"KismetAI.ResumePairing.ReusedIdAcrossTurns", ResumePairingTestFlags)
bool FResumePairing_ReusedIdAcrossTurns::RunTest(const FString& Parameters)
{
	using namespace ResumePairingTest;
	TArray<TSharedPtr<FJsonValue>> Messages = {
		PlainUser(TEXT("go")),
		Msg(TEXT("assistant"), { KismetAITestFakes::MakeToolUseBlock(TEXT("call_0"), TEXT("read_blueprint"), nullptr) }),
		Msg(TEXT("user"), { Result(TEXT("call_0"), TEXT("done-first")) }),
		Msg(TEXT("assistant"), { KismetAITestFakes::MakeTextBlock(TEXT("now edit")), KismetAITestFakes::MakeToolUseBlock(TEXT("call_0"), TEXT("edit_blueprint"), nullptr) }),
		Msg(TEXT("user"), { Result(TEXT("call_0"), TEXT("done-second")) }),
		PlainUser(TEXT("next")),
	};
	FKismetAIAgent::ValidateToolPairing(Messages);

	TestEqual(TEXT("nothing dropped"), Messages.Num(), 6);
	RequestIsWellPaired(*this, Messages, TEXT("reused id"));
	if (Messages.Num() == 6)
	{
		TestEqual(TEXT("first turn keeps its id"), ToolUseIds(Messages[1]), TArray<FString>{ TEXT("call_0") });
		TestEqual(TEXT("first result"), ContentOf(BlocksOfType(Messages[2], TEXT("tool_result"))[0]), FString(TEXT("done-first")));
		TestEqual(TEXT("second turn renamed"), ToolUseIds(Messages[3]), TArray<FString>{ TEXT("call_0_dup1") });
		TestEqual(TEXT("second result follows its own turn"), ContentOf(BlocksOfType(Messages[4], TEXT("tool_result"))[0]), FString(TEXT("done-second")));
	}

	// An identical copy of a turn (a double-written journal line) is still dropped.
	TArray<TSharedPtr<FJsonValue>> Copied = {
		PlainUser(TEXT("go")),
		Msg(TEXT("assistant"), { KismetAITestFakes::MakeToolUseBlock(TEXT("t"), TEXT("read_blueprint"), nullptr) }),
		Msg(TEXT("user"), { Result(TEXT("t"), TEXT("done-1")) }),
		Msg(TEXT("assistant"), { KismetAITestFakes::MakeToolUseBlock(TEXT("t"), TEXT("read_blueprint"), nullptr) }),
		Msg(TEXT("user"), { Result(TEXT("t"), TEXT("done-1")) }),
		PlainUser(TEXT("next")),
	};
	FKismetAIAgent::ValidateToolPairing(Copied);
	TestEqual(TEXT("the copy and its results are gone, the instruction is not"), Copied.Num(), 4);
	RequestIsWellPaired(*this, Copied, TEXT("copied turn"));
	TestEqual(TEXT("instruction last"), PlainContent(Copied.Last()), FString(TEXT("next")));
	return true;
}
