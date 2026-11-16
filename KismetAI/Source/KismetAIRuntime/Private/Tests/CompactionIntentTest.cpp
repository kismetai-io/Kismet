// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Compaction must preserve the user's intent or stop the run.
//
// The bug: TrimConversation summarized the middle of the history and replaced
// it, even when the summarizer returned nothing usable — a missing compact
// prompt, a provider error, a timeout, empty text and a tool-only response all
// produced the same generic "[Conversation summarized ...]" marker, and the
// caller deleted the middle anyway. A correction the user gave in that span
// ("Never delete existing nodes") was gone from the next request while the
// original instruction ("delete the unused nodes") survived in the kept opening.
// Even a successful summary only saw the first 3,000 characters of a long user
// message, and tool results that microcompaction had already cleared.
//
// Every test here goes through the production overflow paths — a provider
// prompt_too_long on the main call, and the end-of-turn context-pressure check —
// with a resumed journal on disk, a scripted model client that records each
// outgoing request, and a scripted tool invoker whose call log is the "did the
// agent act" evidence. The assertion is always on the next request the model
// would see, or on the absence of any tool call after a visible stop.

#include "Fakes/ScriptedAgentSeams.h"
#include "KismetAIAnthropicProvider.h"
#include "KismetAIHistoryRepair.h"
#include "KismetAILLMProvider.h"
#include "KismetAIMinimaxProvider.h"
#include "KismetAIOpenAIProvider.h"
#include "KismetAISessionStorage.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

using namespace KismetAITestFakes;

constexpr auto CompactionIntentTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace CompactionIntentTest
{
	const TCHAR* const Correction = TEXT("Never delete existing nodes. From now on only add new nodes.");
	const TCHAR* const OriginalPrompt = TEXT("Clean up BP_Door: delete the unused nodes in its event graph.");
	const TCHAR* const ToolEvidence = TEXT("EVIDENCE_DELETE_FAILED_K2Node_CallFunction_7");
	const TCHAR* const LegacyMarker = TEXT("earlier turns removed to save context");
	const TCHAR* const PlausibleSummary =
		TEXT("<analysis>scratch</analysis><summary>The user asked to clean up BP_Door by deleting unused nodes. "
			 "The agent read the graph and deleted two nodes.</summary>");

	FKismetAIAgentConfig MakeConfig(const TCHAR* Model = TEXT("claude-sonnet-4-6"))
	{
		FKismetAIAgentConfig C;
		C.ApiKey = TEXT("test-key-unused");
		C.Model = Model;
		C.MaxSteps = 6;
		C.MaxRetries = 0;
		C.RetryJitterFactor = 0.0;
		C.ThinkingMode = EThinkingMode::Disabled;
		return C;
	}

	FString ToJsonLine(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Out + TEXT("\n");
	}

	FString UserLine(const FString& Text)
	{
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("type"), TEXT("user_message"));
		E->SetStringField(TEXT("uuid"), FGuid::NewGuid().ToString());
		E->SetStringField(TEXT("content"), Text);
		return ToJsonLine(E);
	}

	FString AssistantTextLine(const FString& Text)
	{
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("type"), TEXT("assistant_message"));
		E->SetStringField(TEXT("uuid"), FGuid::NewGuid().ToString());
		E->SetArrayField(TEXT("content"), { MakeShared<FJsonValueObject>(MakeTextBlock(Text)) });
		return ToJsonLine(E);
	}

	// One assistant tool call plus its recorded result.
	FString ToolRoundLines(const FString& Id, const FString& Name, const FString& Result)
	{
		TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
		Input->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_Door"));

		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("type"), TEXT("assistant_message"));
		A->SetStringField(TEXT("uuid"), FGuid::NewGuid().ToString());
		A->SetArrayField(TEXT("content"), { MakeShared<FJsonValueObject>(MakeToolUseBlock(Id, Name, Input)) });

		TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
		Block->SetStringField(TEXT("type"), TEXT("tool_result"));
		Block->SetStringField(TEXT("tool_use_id"), Id);
		Block->SetStringField(TEXT("content"), Result);

		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("type"), TEXT("tool_result_message"));
		R->SetStringField(TEXT("uuid"), FGuid::NewGuid().ToString());
		R->SetArrayField(TEXT("results"), { MakeShared<FJsonValueObject>(Block) });
		return ToJsonLine(A) + ToJsonLine(R);
	}

	// Original deletion request -> deletion work -> the user's correction ->
	// several more rounds, so the correction sits in the span TrimConversation
	// drops (it keeps the opening message and the last two rounds).
	FString BuildJournal(const FString& CorrectionMessage)
	{
		FString J;
		{
			TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
			S->SetStringField(TEXT("type"), TEXT("session_start"));
			S->SetNumberField(TEXT("v"), 1);
			S->SetStringField(TEXT("model"), TEXT("claude-sonnet-4-6"));
			J += ToJsonLine(S);
		}
		J += UserLine(OriginalPrompt);
		J += ToolRoundLines(TEXT("toolu_r1"), TEXT("read_blueprint"),
			FString(TEXT("{\"nodes\":[\"K2Node_CallFunction_7\"],\"note\":\"")) + ToolEvidence + TEXT("\"}"));
		J += ToolRoundLines(TEXT("toolu_r2"), TEXT("edit_blueprint"), TEXT("{\"success\":true,\"deleted\":2}"));
		J += AssistantTextLine(TEXT("Deleted two unused nodes."));
		J += UserLine(CorrectionMessage);
		J += AssistantTextLine(TEXT("Understood."));
		J += ToolRoundLines(TEXT("toolu_r3"), TEXT("read_blueprint"), TEXT("{\"nodes\":[]}"));
		J += ToolRoundLines(TEXT("toolu_r4"), TEXT("read_blueprint"), TEXT("{\"nodes\":[]}"));
		J += ToolRoundLines(TEXT("toolu_r5"), TEXT("read_blueprint"), TEXT("{\"nodes\":[]}"));
		J += AssistantTextLine(TEXT("Graph reviewed."));
		return J;
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

		FString Text() const
		{
			FString Out;
			FFileHelper::LoadFileToString(Out, *FKismetAISessionStorage::GetSessionFilePath(SessionId));
			return Out;
		}
	};

	FString Serialize(const TArray<TSharedPtr<FJsonValue>>& Messages)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Messages, Writer);
		return Out;
	}

	// Concatenated user-visible text of a request (string content and text blocks),
	// unescaped, so long strings can be compared exactly.
	FString UserText(const TArray<TSharedPtr<FJsonValue>>& Messages)
	{
		FString Out;
		for (const TSharedPtr<FJsonValue>& M : Messages)
		{
			const TSharedPtr<FJsonObject> O = M.IsValid() ? M->AsObject() : nullptr;
			if (!O.IsValid()) { continue; }
			FString S;
			if (O->TryGetStringField(TEXT("content"), S)) { Out += S; Out += TEXT("\n"); continue; }
			const TArray<TSharedPtr<FJsonValue>>* Blocks = nullptr;
			if (O->TryGetArrayField(TEXT("content"), Blocks))
			{
				for (const TSharedPtr<FJsonValue>& B : *Blocks)
				{
					const TSharedPtr<FJsonObject> BO = B.IsValid() ? B->AsObject() : nullptr;
					FString T;
					if (BO.IsValid() && BO->TryGetStringField(TEXT("text"), T)) { Out += T; Out += TEXT("\n"); }
				}
			}
		}
		return Out;
	}

	bool HasToolBlocks(const TArray<TSharedPtr<FJsonValue>>& Messages)
	{
		const FString S = Serialize(Messages);
		return S.Contains(TEXT("\"tool_use\"")) || S.Contains(TEXT("\"tool_result\""));
	}

	FModelCallOutcome PromptTooLong()
	{
		FModelCallOutcome O;
		O.Result = EModelCallResult::PromptTooLong;
		O.ErrorType = TEXT("invalid_request_error");
		O.ErrorMessage = TEXT("prompt is too long: 210000 tokens > 200000 maximum");
		O.HttpStatusCode = 400;
		return O;
	}

	FModelCallOutcome SummaryTimeout()
	{
		FModelCallOutcome O;
		O.Result = EModelCallResult::RetryableError;
		O.ErrorType = TEXT("timeout");
		O.ErrorMessage = TEXT("Request timed out");
		return O;
	}

	FModelCallOutcome SummaryProviderError()
	{
		FModelCallOutcome O;
		O.Result = EModelCallResult::FatalError;
		O.ErrorType = TEXT("api_error");
		O.ErrorMessage = TEXT("Internal server error");
		O.HttpStatusCode = 500;
		return O;
	}

	TSharedPtr<FJsonObject> MutationArgs()
	{
		TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
		Op->SetStringField(TEXT("action"), TEXT("set_pin_default"));
		Op->SetStringField(TEXT("node"), TEXT("K2Node_CallFunction_7"));
		Op->SetStringField(TEXT("pin"), TEXT("InString"));
		Op->SetStringField(TEXT("value"), TEXT("hi"));
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_Door"));
		A->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(Op) });
		return A;
	}

	// A provider prompt_too_long, then the given summary outcome, then a model
	// that would mutate the Blueprint if it were ever asked again.
	struct FPromptTooLongRun
	{
		FSessionFile Session;
		FScriptedModelClient Model;
		FScriptedToolInvoker Tools;
		FEventRecorder Rec;
		FString Result;

		FPromptTooLongRun(const FString& CorrectionMessage, const FModelCallOutcome& Summary)
			: Session(BuildJournal(CorrectionMessage))
			, Model({ PromptTooLong(), Summary,
				MakeToolTurn(TEXT("toolu_mut"), TEXT("edit_blueprint"), MutationArgs()),
				MakeTextTurn(TEXT("Done.")) })
		{
			Tools.SetResult(TEXT("edit_blueprint"), TEXT("{\"success\":true}"));
			FKismetAIAgent Agent(Tools, Model, MakeConfig());
			Result = Agent.RunTask(TEXT("Continue."), Rec.MakeCallback(), TEXT(""), Session.SessionId);
		}
	};

	// Either the run stopped visibly before any tool call, or every request the
	// model saw after the summary attempt still carried the correction.
	void ExpectCorrectionKeptOrStopped(FAutomationTestBase& Test, const FPromptTooLongRun& Run, const FString& Needle)
	{
		const TArray<FCapturedRequest>& Requests = Run.Model.GetRequests();
		for (int32 i = 2; i < Requests.Num(); ++i)
		{
			Test.TestTrue(FString::Printf(TEXT("request %d still carries the correction"), i),
				UserText(Requests[i].Messages).Contains(Needle));
			Test.TestFalse(FString::Printf(TEXT("request %d carries no generic removal marker"), i),
				Serialize(Requests[i].Messages).Contains(LegacyMarker));
		}
		if (Requests.Num() <= 2)
		{
			Test.TestEqual(TEXT("no tool ran after the failed compaction"), Run.Tools.CallLog.Num(), 0);
			Test.TestTrue(TEXT("the stop is visible as an error event"), Run.Rec.HasType(TEXT("error")));
		}
	}
}

namespace CIT = CompactionIntentTest;

// Summary call times out: the correction survives or the run stops before acting.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompaction_SummaryTimeoutKeepsCorrection,
	"KismetAI.Compaction.PromptTooLong.SummaryTimeoutKeepsCorrectionOrStops", CompactionIntentTestFlags)
bool FCompaction_SummaryTimeoutKeepsCorrection::RunTest(const FString& Parameters)
{
	CIT::FPromptTooLongRun Run(CIT::Correction, CIT::SummaryTimeout());
	CIT::ExpectCorrectionKeptOrStopped(*this, Run, CIT::Correction);
	TestEqual(TEXT("the mutation was never dispatched"), Run.Tools.CountCalls(TEXT("edit_blueprint")), 0);
	TestTrue(TEXT("the journal still holds the correction"), Run.Session.Text().Contains(TEXT("Never delete existing nodes")));

	// The chosen behavior: stop before another model turn, visibly and honestly.
	TestEqual(TEXT("main call + summary call only"), Run.Model.NumCalls(), 2);
	const FString* Error = Run.Rec.FirstJsonOfType(TEXT("error"));
	TestTrue(TEXT("error event names context compaction"), Error && Error->Contains(TEXT("context_compaction_failed")));
	TestTrue(TEXT("error event names the cause"), Error && Error->Contains(TEXT("summary_timeout")));
	TestTrue(TEXT("final text says nothing was removed"), Run.Result.Contains(TEXT("Nothing was removed")));
	const FString* Done = Run.Rec.FirstJsonOfType(TEXT("done"));
	TestTrue(TEXT("no done=completed after the stop"), Done == nullptr || !Done->Contains(TEXT("\"completed\"")));
	TestTrue(TEXT("the failed attempt is journaled"), Run.Session.Text().Contains(TEXT("\"type\":\"context_compaction\"")));
	return true;
}

// Summary call fails with a provider error: same contract.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompaction_SummaryProviderErrorKeepsCorrection,
	"KismetAI.Compaction.PromptTooLong.SummaryProviderErrorKeepsCorrectionOrStops", CompactionIntentTestFlags)
bool FCompaction_SummaryProviderErrorKeepsCorrection::RunTest(const FString& Parameters)
{
	CIT::FPromptTooLongRun Run(CIT::Correction, CIT::SummaryProviderError());
	CIT::ExpectCorrectionKeptOrStopped(*this, Run, CIT::Correction);
	TestEqual(TEXT("the mutation was never dispatched"), Run.Tools.CountCalls(TEXT("edit_blueprint")), 0);
	return true;
}

// Empty summary text: no generic marker may authorize dropping the middle.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompaction_EmptySummaryIsFailure,
	"KismetAI.Compaction.PromptTooLong.EmptySummaryIsFailure", CompactionIntentTestFlags)
bool FCompaction_EmptySummaryIsFailure::RunTest(const FString& Parameters)
{
	CIT::FPromptTooLongRun Run(CIT::Correction, MakeTextTurn(TEXT("  ")));
	CIT::ExpectCorrectionKeptOrStopped(*this, Run, CIT::Correction);
	TestEqual(TEXT("the mutation was never dispatched"), Run.Tools.CountCalls(TEXT("edit_blueprint")), 0);
	return true;
}

// Tool-only summary (the model answered with a tool call instead of text).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompaction_ToolOnlySummaryIsFailure,
	"KismetAI.Compaction.PromptTooLong.ToolOnlySummaryIsFailure", CompactionIntentTestFlags)
bool FCompaction_ToolOnlySummaryIsFailure::RunTest(const FString& Parameters)
{
	CIT::FPromptTooLongRun Run(CIT::Correction,
		MakeToolTurn(TEXT("toolu_sum"), TEXT("read_blueprint"), MakeShared<FJsonObject>()));
	CIT::ExpectCorrectionKeptOrStopped(*this, Run, CIT::Correction);
	TestEqual(TEXT("the mutation was never dispatched"), Run.Tools.CountCalls(TEXT("edit_blueprint")), 0);
	TestEqual(TEXT("the summary's tool call was not executed"), Run.Tools.CountCalls(TEXT("read_blueprint")), 0);
	return true;
}

// A plausible summary that omits the correction: the correction must still
// reach the next request (verbatim retention), and the run may proceed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompaction_PlausibleSummaryOmittingConstraint,
	"KismetAI.Compaction.PromptTooLong.PlausibleSummaryCannotDropConstraint", CompactionIntentTestFlags)
bool FCompaction_PlausibleSummaryOmittingConstraint::RunTest(const FString& Parameters)
{
	CIT::FPromptTooLongRun Run(CIT::Correction, MakeTextTurn(CIT::PlausibleSummary));
	const TArray<FCapturedRequest>& Requests = Run.Model.GetRequests();
	if (!TestTrue(TEXT("compaction committed and the run continued"), Requests.Num() >= 3))
	{
		return false;
	}
	const FString Next = CIT::UserText(Requests[2].Messages);
	TestTrue(TEXT("the correction reached the next request verbatim"), Next.Contains(CIT::Correction));
	TestTrue(TEXT("the summary is in the next request"), Next.Contains(TEXT("deleted two nodes")));
	TestTrue(TEXT("the original prompt is kept"), Next.Contains(CIT::OriginalPrompt));
	TestTrue(TEXT("the newest instruction is kept"), Next.Contains(TEXT("Continue.")));
	TestTrue(TEXT("the next request is smaller than the rejected one"),
		Requests[2].Messages.Num() < Requests[0].Messages.Num());

	// The correction must come after the summary so it is read as the newer word.
	const int32 SummaryAt = Next.Find(TEXT("deleted two nodes"));
	const int32 CorrectionAt = Next.Find(CIT::Correction);
	TestTrue(TEXT("the correction follows the summary"), SummaryAt != INDEX_NONE && CorrectionAt > SummaryAt);
	return true;
}

// The decisive constraint sits after character 3,000 of a long correction.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompaction_LateConstraintInLongMessage,
	"KismetAI.Compaction.PromptTooLong.LateConstraintInLongMessageSurvives", CompactionIntentTestFlags)
bool FCompaction_LateConstraintInLongMessage::RunTest(const FString& Parameters)
{
	FString Long = TEXT("Some context about the door before the real instruction. ");
	while (Long.Len() < 3600)
	{
		Long += TEXT("The hinge logic and the timeline are background detail, not instructions. ");
	}
	const FString Decisive = TEXT("DECISIVE: never delete existing nodes; only add new ones.");
	Long += Decisive;
	TestTrue(TEXT("fixture: constraint starts after char 3000"), Long.Find(Decisive) > 3000);

	CIT::FPromptTooLongRun Run(Long, MakeTextTurn(CIT::PlausibleSummary));
	const TArray<FCapturedRequest>& Requests = Run.Model.GetRequests();
	if (!TestTrue(TEXT("the run continued after compaction"), Requests.Num() >= 3))
	{
		CIT::ExpectCorrectionKeptOrStopped(*this, Run, Decisive);
		return true;
	}
	TestTrue(TEXT("the whole long message reached the next request"), CIT::UserText(Requests[2].Messages).Contains(Long));
	TestTrue(TEXT("the summarizer saw the decisive tail too"), CIT::UserText(Requests[1].Messages).Contains(Decisive));
	return true;
}

// The successful path still compacts, and the summary request is a text-only
// call: no live tool schemas, no historical tool blocks, evidence from tool
// results the main request had already cleared.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompaction_SuccessfulCompactionShape,
	"KismetAI.Compaction.PromptTooLong.SuccessShrinksAndSummaryHasNoTools", CompactionIntentTestFlags)
bool FCompaction_SuccessfulCompactionShape::RunTest(const FString& Parameters)
{
	CIT::FPromptTooLongRun Run(CIT::Correction, MakeTextTurn(CIT::PlausibleSummary));
	const TArray<FCapturedRequest>& Requests = Run.Model.GetRequests();
	if (!TestTrue(TEXT("summary + retry were sent"), Requests.Num() >= 3))
	{
		return false;
	}
	const FCapturedRequest& Summary = Requests[1];
	TestEqual(TEXT("summary request carries no tool schemas"), Summary.Tools.Num(), 0);
	TestFalse(TEXT("summary request carries no tool_use/tool_result blocks"), CIT::HasToolBlocks(Summary.Messages));
	TestTrue(TEXT("summary input includes tool-result evidence"), CIT::UserText(Summary.Messages).Contains(CIT::ToolEvidence));
	TestTrue(TEXT("main request still has tools"), Requests[2].Tools.Num() > 0);
	TestEqual(TEXT("the run carried on and dispatched the next tool"), Run.Tools.CountCalls(TEXT("edit_blueprint")), 1);
	TestFalse(TEXT("no error event on the successful path"), Run.Rec.HasType(TEXT("error")));
	return true;
}

// End-of-turn trigger: the main call reports usage near the context limit, the
// agent runs a tool, then compaction is attempted and the summary times out.
// The next request must still carry the correction.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompaction_EndOfTurnFailureKeepsHistory,
	"KismetAI.Compaction.EndOfTurn.SummaryFailureKeepsHistory", CompactionIntentTestFlags)
bool FCompaction_EndOfTurnFailureKeepsHistory::RunTest(const FString& Parameters)
{
	const FKismetAIAgentConfig Config = CIT::MakeConfig();
	// Nearly full, as a provider could report it for a request it accepted
	// (input + max_tokens within the window); budgets from it as a
	// measurement of that request.
	const int32 AtLimit = static_cast<int32>(
		(FKismetAIAgent::GetModelContextLimit(Config.Model) - FKismetAIContextBudget::OutputReserveCap) * 0.97);

	TSharedPtr<FJsonObject> ReadArgs = MakeShared<FJsonObject>();
	ReadArgs->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_Door"));

	CIT::FSessionFile Session(CIT::BuildJournal(CIT::Correction));
	FScriptedModelClient Model({
		MakeResponse({ MakeToolUseBlock(TEXT("toolu_eot"), TEXT("read_blueprint"), ReadArgs) }, TEXT("tool_use"), AtLimit, 50),
		CIT::SummaryTimeout(),
		MakeTextTurn(TEXT("Done.")) });
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;
	FKismetAIAgent Agent(Tools, Model, Config);
	Agent.RunTask(TEXT("Continue."), Rec.MakeCallback(), TEXT(""), Session.SessionId);

	const TArray<FCapturedRequest>& Requests = Model.GetRequests();
	if (!TestEqual(TEXT("main, summary, main"), Requests.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("second call was the summary (no tools)"), Requests[1].Tools.Num(), 0);
	const FString Next = CIT::UserText(Requests[2].Messages);
	TestTrue(TEXT("the correction is still in the next request"), Next.Contains(CIT::Correction));
	TestFalse(TEXT("no generic removal marker"), CIT::Serialize(Requests[2].Messages).Contains(CIT::LegacyMarker));
	TestTrue(TEXT("history was not shrunk by a failed compaction"),
		Requests[2].Messages.Num() > Requests[0].Messages.Num());
	return true;
}

// End-of-turn success: compaction commits, and the correction is still there.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompaction_EndOfTurnSuccessRetainsCorrection,
	"KismetAI.Compaction.EndOfTurn.SuccessRetainsCorrection", CompactionIntentTestFlags)
bool FCompaction_EndOfTurnSuccessRetainsCorrection::RunTest(const FString& Parameters)
{
	const FKismetAIAgentConfig Config = CIT::MakeConfig();
	// Nearly full, as a provider could report it for a request it accepted
	// (input + max_tokens within the window); budgets from it as a
	// measurement of that request.
	const int32 AtLimit = static_cast<int32>(
		(FKismetAIAgent::GetModelContextLimit(Config.Model) - FKismetAIContextBudget::OutputReserveCap) * 0.97);

	TSharedPtr<FJsonObject> ReadArgs = MakeShared<FJsonObject>();
	ReadArgs->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_Door"));

	CIT::FSessionFile Session(CIT::BuildJournal(CIT::Correction));
	FScriptedModelClient Model({
		MakeResponse({ MakeToolUseBlock(TEXT("toolu_eot"), TEXT("read_blueprint"), ReadArgs) }, TEXT("tool_use"), AtLimit, 50),
		MakeTextTurn(CIT::PlausibleSummary),
		MakeTextTurn(TEXT("Done.")) });
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;
	FKismetAIAgent Agent(Tools, Model, Config);
	Agent.RunTask(TEXT("Continue."), Rec.MakeCallback(), TEXT(""), Session.SessionId);

	const TArray<FCapturedRequest>& Requests = Model.GetRequests();
	if (!TestEqual(TEXT("main, summary, main"), Requests.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("summary call has no tools"), Requests[1].Tools.Num(), 0);
	const FString Next = CIT::UserText(Requests[2].Messages);
	TestTrue(TEXT("summary committed"), Next.Contains(TEXT("deleted two nodes")));
	TestTrue(TEXT("correction retained verbatim"), Next.Contains(CIT::Correction));
	TestTrue(TEXT("history shrank"), Requests[2].Messages.Num() < Requests[0].Messages.Num() + 2);
	return true;
}

// The user's own messages alone exceed the window: no safe context exists, so
// the run stops visibly and no tool is dispatched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompaction_NoSafeContextStops,
	"KismetAI.Compaction.PromptTooLong.NoSafeContextStopsBeforeTools", CompactionIntentTestFlags)
bool FCompaction_NoSafeContextStops::RunTest(const FString& Parameters)
{
	const TCHAR* const SmallModel = TEXT("claude-haiku-4-5-20251001");
	const int32 Limit = FKismetAIAgent::GetModelContextLimit(SmallModel);

	// ~5 characters per token is generous; this is well past the window.
	FString Huge = FString(CIT::Correction) + TEXT(" ");
	const FString Filler = TEXT("Pasted log line that the user wants kept as context for this edit. ");
	Huge.Reserve(Limit * 5 + 128);
	while (Huge.Len() < Limit * 5)
	{
		Huge += Filler;
	}

	CIT::FSessionFile Session(CIT::BuildJournal(Huge));
	FScriptedModelClient Model({ CIT::PromptTooLong(), MakeTextTurn(CIT::PlausibleSummary),
		MakeToolTurn(TEXT("toolu_mut"), TEXT("edit_blueprint"), CIT::MutationArgs()),
		MakeTextTurn(TEXT("Done.")) });
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;
	FKismetAIAgent Agent(Tools, Model, CIT::MakeConfig(SmallModel));
	const FString Result = Agent.RunTask(TEXT("Continue."), Rec.MakeCallback(), TEXT(""), Session.SessionId);

	TestEqual(TEXT("no tool was dispatched"), Tools.CallLog.Num(), 0);
	TestTrue(TEXT("an error event makes the stop visible"), Rec.HasType(TEXT("error")));
	const FString* Error = Rec.FirstJsonOfType(TEXT("error"));
	TestTrue(TEXT("error names context compaction"), Error && Error->Contains(TEXT("context_compaction_failed")));
	TestTrue(TEXT("final text explains the stop"), Result.Contains(TEXT("context")));
	for (const FCapturedRequest& R : Model.GetRequests())
	{
		TestFalse(TEXT("no request dropped the user's message for a marker"),
			CIT::Serialize(R.Messages).Contains(CIT::LegacyMarker));
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// TrimConversation as a transaction, driven directly
// ─────────────────────────────────────────────────────────────────────────────

namespace CompactionIntentTest
{
	TSharedPtr<FJsonValue> TextMessage(const TCHAR* Role, const FString& Text)
	{
		TSharedPtr<FJsonObject> M = MakeShared<FJsonObject>();
		M->SetStringField(TEXT("role"), Role);
		M->SetStringField(TEXT("content"), Text);
		return MakeShared<FJsonValueObject>(M);
	}

	void AddRound(TArray<TSharedPtr<FJsonValue>>& Messages, const FString& Id, const FString& Result)
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("role"), TEXT("assistant"));
		A->SetArrayField(TEXT("content"), { MakeShared<FJsonValueObject>(
			MakeToolUseBlock(Id, TEXT("read_blueprint"), MakeShared<FJsonObject>())) });
		Messages.Add(MakeShared<FJsonValueObject>(A));

		TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>();
		B->SetStringField(TEXT("type"), TEXT("tool_result"));
		B->SetStringField(TEXT("tool_use_id"), Id);
		B->SetStringField(TEXT("content"), Result);
		TSharedPtr<FJsonObject> U = MakeShared<FJsonObject>();
		U->SetStringField(TEXT("role"), TEXT("user"));
		U->SetArrayField(TEXT("content"), { MakeShared<FJsonValueObject>(B) });
		Messages.Add(MakeShared<FJsonValueObject>(U));
	}

	// Opening prompt, two rounds, a correction, three more rounds.
	TArray<TSharedPtr<FJsonValue>> BuildHistory()
	{
		TArray<TSharedPtr<FJsonValue>> M;
		M.Add(TextMessage(TEXT("user"), OriginalPrompt));
		AddRound(M, TEXT("t1"), FString(TEXT("first result ")) + ToolEvidence);
		AddRound(M, TEXT("t2"), TEXT("second result"));
		M.Add(TextMessage(TEXT("assistant"), TEXT("Deleted two nodes.")));
		M.Add(TextMessage(TEXT("user"), Correction));
		AddRound(M, TEXT("t3"), TEXT("third result"));
		AddRound(M, TEXT("t4"), TEXT("fourth result"));
		AddRound(M, TEXT("t5"), TEXT("fifth result"));
		return M;
	}
}

// A failed attempt leaves the caller's array, and every object in it, untouched:
// no microcompaction of the source, no reordering, no marker.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompaction_FailureIsAtomic,
	"KismetAI.Compaction.Trim.FailureLeavesHistoryUntouched", CompactionIntentTestFlags)
bool FCompaction_FailureIsAtomic::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FJsonValue>> Messages = CIT::BuildHistory();
	const TArray<TSharedPtr<FJsonValue>> Before = Messages;
	const FString BeforeJson = CIT::Serialize(Messages);

	FScriptedModelClient Model({ CIT::SummaryTimeout() });
	FScriptedToolInvoker Tools;
	FKismetAIAgent Agent(Tools, Model, CIT::MakeConfig());
	Agent.SetCompactPromptForTests(TEXT("Summarize."));
	const FKismetAICompactionResult Result = Agent.TrimConversation(Messages);

	TestEqual(TEXT("status is Failed"), (int32)Result.Status, (int32)ECompactionStatus::Failed);
	TestEqual(TEXT("classified as a timeout"), Result.FailureCode, FString(TEXT("summary_timeout")));
	TestEqual(TEXT("same message count"), Messages.Num(), Before.Num());
	for (int32 i = 0; i < FMath::Min(Messages.Num(), Before.Num()); ++i)
	{
		TestTrue(FString::Printf(TEXT("message %d is the same object"), i), Messages[i] == Before[i]);
	}
	TestEqual(TEXT("no object was mutated (tool results not cleared)"), CIT::Serialize(Messages), BeforeJson);
	TestTrue(TEXT("the result can be journaled"), Result.ToJson()->GetStringField(TEXT("failure_code")) == TEXT("summary_timeout"));
	return true;
}

// No compact prompt is a failure, not a licence to drop the middle.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompaction_NoCompactPromptIsFailure,
	"KismetAI.Compaction.Trim.MissingCompactPromptIsFailure", CompactionIntentTestFlags)
bool FCompaction_NoCompactPromptIsFailure::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FJsonValue>> Messages = CIT::BuildHistory();
	const int32 Before = Messages.Num();
	FScriptedModelClient Model({ MakeTextTurn(CIT::PlausibleSummary) });
	FScriptedToolInvoker Tools;
	FKismetAIAgent Agent(Tools, Model, CIT::MakeConfig());
	const FKismetAICompactionResult Result = Agent.TrimConversation(Messages);

	TestEqual(TEXT("summary_unavailable"), Result.FailureCode, FString(TEXT("summary_unavailable")));
	TestEqual(TEXT("history unchanged"), Messages.Num(), Before);
	TestEqual(TEXT("no model call was made"), Model.NumCalls(), 0);
	return true;
}

// Truncated summary (output cap) is a failure too.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompaction_TruncatedSummaryIsFailure,
	"KismetAI.Compaction.Trim.TruncatedSummaryIsFailure", CompactionIntentTestFlags)
bool FCompaction_TruncatedSummaryIsFailure::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FJsonValue>> Messages = CIT::BuildHistory();
	const int32 Before = Messages.Num();
	FScriptedModelClient Model({ MakeTextTurn(TEXT("<summary>The user asked"), TEXT("max_tokens")) });
	FScriptedToolInvoker Tools;
	FKismetAIAgent Agent(Tools, Model, CIT::MakeConfig());
	Agent.SetCompactPromptForTests(TEXT("Summarize."));
	const FKismetAICompactionResult Result = Agent.TrimConversation(Messages);

	TestEqual(TEXT("summary_truncated"), Result.FailureCode, FString(TEXT("summary_truncated")));
	TestEqual(TEXT("history unchanged"), Messages.Num(), Before);
	return true;
}

// Success: the projection shrinks, keeps the correction object itself after the
// summary, pairs every tool_use, and the summarizer read the evidence that the
// end-of-turn microcompaction had already cleared from the projection.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompaction_SuccessCommitsCandidate,
	"KismetAI.Compaction.Trim.SuccessCommitsVerbatimUserMessages", CompactionIntentTestFlags)
bool FCompaction_SuccessCommitsCandidate::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FJsonValue>> Messages = CIT::BuildHistory();
	const TSharedPtr<FJsonValue> CorrectionObject = Messages[6];
	TestTrue(TEXT("fixture: index 6 is the correction"), CIT::UserText({ CorrectionObject }).Contains(CIT::Correction));

	// Phase 1 first, as the end-of-turn path does: t1's body is cleared in the projection.
	TMap<FString, FString> Cleared;
	FKismetAIAgent::MicrocompactToolResults(Messages, 2, &Cleared);
	TestTrue(TEXT("t1 evidence captured"), Cleared.Contains(TEXT("t1")));
	TestFalse(TEXT("t1 body cleared in the projection"), CIT::Serialize(Messages).Contains(CIT::ToolEvidence));

	const FString Transcript = FKismetAIAgent::BuildSummaryTranscript(Messages, Cleared);
	TestTrue(TEXT("transcript restores the cleared evidence"), Transcript.Contains(CIT::ToolEvidence));
	TestTrue(TEXT("transcript has the correction"), Transcript.Contains(CIT::Correction));

	FScriptedModelClient Model({ MakeTextTurn(CIT::PlausibleSummary) });
	FScriptedToolInvoker Tools;
	FKismetAIAgent Agent(Tools, Model, CIT::MakeConfig());
	Agent.SetCompactPromptForTests(TEXT("Summarize."));
	const int32 Before = Messages.Num();
	const FKismetAICompactionResult Result = Agent.TrimConversation(Messages);

	TestTrue(TEXT("committed"), Result.IsCommitted());
	TestTrue(TEXT("summarized"), Result.bSummarized);
	TestTrue(TEXT("shrank"), Messages.Num() < Before);
	TestEqual(TEXT("one user message retained"), Result.RetainedUserMessages, 1);
	TestTrue(TEXT("opening prompt first"), CIT::UserText({ Messages[0] }).Contains(CIT::OriginalPrompt));
	TestTrue(TEXT("summary second"), CIT::UserText({ Messages[1] }).Contains(TEXT("deleted two nodes")));
	TestTrue(TEXT("the correction object itself follows the summary"), Messages[2] == CorrectionObject);

	// tool_use/tool_result pairing still holds after the repair the caller runs.
	TArray<TSharedPtr<FJsonValue>> Repaired = Messages;
	const KismetAIHistoryRepair::FRepairReport Report = KismetAIHistoryRepair::RepairToolPairing(Repaired);
	TestFalse(TEXT("candidate needed no pairing repair"), Report.HasChanges());
	return true;
}

// The summary request carries no tool schemas, so every encoder must omit the
// tools field (and tool_choice) rather than send an empty array.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompaction_ProvidersOmitEmptyTools,
	"KismetAI.Compaction.ProvidersOmitEmptyTools", CompactionIntentTestFlags)
bool FCompaction_ProvidersOmitEmptyTools::RunTest(const FString& Parameters)
{
	const TArray<TSharedPtr<FJsonValue>> Messages = { CIT::TextMessage(TEXT("user"), TEXT("Summarize this.")) };
	const TArray<TSharedPtr<FJsonValue>> NoTools;
	FLLMRequestShape Shape;

	{
		FKismetAIAgentConfig Cfg = CIT::MakeConfig();
		FAnthropicProvider Provider;
		const TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), NoTools, TEXT("sys"), Shape);
		TestFalse(TEXT("anthropic: no tools field"), Body->HasField(TEXT("tools")));
	}
	{
		FKismetAIAgentConfig Cfg = CIT::MakeConfig(TEXT("MiniMax-M3"));
		FMinimaxProvider Provider;
		const TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), NoTools, TEXT("sys"), Shape);
		TestFalse(TEXT("minimax: no tools field"), Body->HasField(TEXT("tools")));
	}
	{
		FKismetAIAgentConfig Cfg = CIT::MakeConfig(TEXT("gpt-4o"));
		FOpenAIProvider Provider;
		const TSharedPtr<FJsonObject> Body = Provider.BuildRequestBody(Cfg, KismetAIAgentCodec::FromAnthropicArray(Messages), NoTools, TEXT("sys"), Shape);
		TestFalse(TEXT("openai responses: no tools field"), Body->HasField(TEXT("tools")));
		TestFalse(TEXT("openai responses: no tool_choice"), Body->HasField(TEXT("tool_choice")));
	}
	return true;
}
