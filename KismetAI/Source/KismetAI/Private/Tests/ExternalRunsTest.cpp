// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// External runs have an id, progress, status, cancel and a defined
// disconnect/reattach behaviour.
//
// The module tests drive the real HTTP handlers with a scripted model client
// that parks inside a model call until the test lets it go, so "the response
// arrived before the model call completed" is forced by a latch, not by
// timing. Editor tool calls go through the module's real invoker and
// dispatcher to the game thread, which the test pumps; the edit is counted
// at policy admission, which every mutation passes exactly once.

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "RunEntryHarness.h"
#include "KismetAIExternalRuns.h"
#include "KismetAISessionStorage.h"
#include "KismetAISessionController.h"
#include "KismetAIModule.h"
#include "KismetAIModuleToolInvoker.h"
#include "KismetAIAgent.h"
#include "KismetAIModelClientInterface.h"
#include "KismetAIToolPolicy.h"
#include "Async/TaskGraphInterfaces.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include <atomic>

namespace KismetAIExternalRunsTest
{
	using namespace KismetAIRunEntryHarness;
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	// A disposable in-memory Blueprint with one PrintString node.
	struct FPrintFixture
	{
		UBlueprint* BP = nullptr;
		UK2Node_CallFunction* Print = nullptr;

		explicit FPrintFixture(const TCHAR* Name)
		{
			UPackage* Pkg = CreatePackage(*FString::Printf(TEXT("/Temp/KismetP109/%s"), Name));
			BP = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Pkg, Name, BPTYPE_Normal,
				UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
			if (!BP || BP->UbergraphPages.Num() == 0) { return; }
			UEdGraph* Graph = BP->UbergraphPages[0];
			Print = NewObject<UK2Node_CallFunction>(Graph);
			Print->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
				UKismetSystemLibrary::StaticClass());
			Graph->AddNode(Print, false, false);
			Print->CreateNewGuid();
			Print->AllocateDefaultPins();
		}

		FString Duration() const
		{
			UEdGraphPin* Pin = Print ? Print->FindPin(TEXT("Duration"), EGPD_Input) : nullptr;
			return Pin ? Pin->DefaultValue : FString(TEXT("<no pin>"));
		}

		TSharedPtr<FJsonObject> SetDurationArgs(const TCHAR* Value) const
		{
			TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
			Op->SetStringField(TEXT("action"), TEXT("set_pin_default"));
			Op->SetStringField(TEXT("node"), Print->GetName());
			Op->SetStringField(TEXT("pin"), TEXT("Duration"));
			Op->SetStringField(TEXT("value"), Value);
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("blueprint"), BP->GetPathName());
			Args->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(Op) });
			return Args;
		}
	};

	FKismetAIExternalRunRef MakeRecord(const TCHAR* RunId, int32 MaxEvents = 20000)
	{
		FKismetAIExternalRunRecord::FLimits Limits;
		Limits.MaxEvents = MaxEvents;
		return MakeShared<FKismetAIExternalRunRecord, ESPMode::ThreadSafe>(RunId, FString(), TEXT("fp"), TEXT("s1"), Limits);
	}
}


// ── Record and registry ───────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExternalRuns_RecordCursorsAndOneOutcome,
	"KismetAI.ExternalRuns.RecordCursorsAndOneOutcome", KismetAIExternalRunsTest::Flags)
bool FExternalRuns_RecordCursorsAndOneOutcome::RunTest(const FString& Parameters)
{
	using namespace KismetAIExternalRunsTest;
	FKismetAIExternalRunRef R = MakeRecord(TEXT("run-a"));
	R->Append(TEXT("attempt_start"), TEXT("{\"type\":\"attempt_start\",\"run_id\":\"run-a\"}"));
	R->Append(TEXT("tool_call"), TEXT("{\"type\":\"tool_call\",\"tool\":\"edit_blueprint\"}"));
	TestEqual(TEXT("phase while a tool runs"), R->GetPhase(), FString(TEXT("tool")));
	R->Append(TEXT("tool_result"), TEXT("{\"type\":\"tool_result\",\"result\":{\"success\":false}}"));
	R->Append(TEXT("mutation_outcomes"), TEXT("{\"type\":\"mutation_outcomes\",\"needs_attention\":true}"));
	R->Append(TEXT("done"), TEXT("{\"type\":\"done\",\"status\":\"max_steps\",\"run_id\":\"run-a\"}"));
	R->Append(TEXT("done"), TEXT("{\"type\":\"done\",\"status\":\"completed\"}"));   // a second terminal never wins
	TestEqual(TEXT("finalizing after the terminal event"), R->GetPhase(), FString(TEXT("finalizing")));

	const FKismetAIRunEventPage All = R->Read(0, 0);
	TestEqual(TEXT("run event + 6"), All.Events.Num(), 7);
	TestEqual(TEXT("seq 1 is the run itself"), All.Events[0].Type, FString(TEXT("run")));
	for (int32 I = 0; I < All.Events.Num(); ++I) { TestEqual(TEXT("contiguous seq"), All.Events[I].Seq, static_cast<int64>(I + 1)); }
	TestEqual(TEXT("tool events are SSE 'step'"), All.Events[2].Name, FString(TEXT("step")));

	const FKismetAIRunEventPage Tail = R->Read(5, 1);
	TestTrue(TEXT("cursor resumes after 5, limit 1"), Tail.Events.Num() == 1 && Tail.Events[0].Seq == 6 && Tail.bMore);
	TestFalse(TEXT("not finished before Finish"), Tail.bFinished);

	R->Finish(TEXT("no_terminal_event"));
	R->Finish(TEXT("not_started"));   // idempotent
	TSharedRef<FJsonObject> S = R->BuildStatusJson({});
	TestEqual(TEXT("state"), S->GetStringField(TEXT("state")), FString(TEXT("finished")));
	const TSharedPtr<FJsonObject> Outcome = S->GetObjectField(TEXT("outcome"));
	TestEqual(TEXT("first terminal event's status"), Outcome->GetStringField(TEXT("status")), FString(TEXT("max_steps")));
	TestEqual(TEXT("terminal event"), Outcome->GetStringField(TEXT("terminal_event")), FString(TEXT("done")));
	TestTrue(TEXT("ledger in the outcome"), Outcome->GetObjectField(TEXT("mutation_outcomes"))->GetBoolField(TEXT("needs_attention")));
	const TSharedPtr<FJsonObject> Progress = S->GetObjectField(TEXT("progress"));
	TestEqual(TEXT("tool calls"), static_cast<int32>(Progress->GetNumberField(TEXT("tool_calls"))), 1);
	TestEqual(TEXT("tool errors"), static_cast<int32>(Progress->GetNumberField(TEXT("tool_errors"))), 1);
	TestTrue(TEXT("finished page"), R->Read(7, 10).bFinished);

	FKismetAIExternalRunRef Empty = MakeRecord(TEXT("run-b"));
	Empty->Finish(TEXT("not_started"));
	TestEqual(TEXT("fallback when no terminal event"),
		Empty->BuildStatusJson({})->GetObjectField(TEXT("outcome"))->GetStringField(TEXT("status")), FString(TEXT("not_started")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExternalRuns_UncertainTerminalIsPreserved,
	"KismetAI.ExternalRuns.UncertainTerminalIsPreserved", KismetAIExternalRunsTest::Flags)
bool FExternalRuns_UncertainTerminalIsPreserved::RunTest(const FString& Parameters)
{
	auto R = KismetAIExternalRunsTest::MakeRecord(TEXT("uncertain"));
	R->Append(TEXT("mutation_outcomes"), TEXT(R"({"needs_attention":true})"));
	R->Append(TEXT("error"), TEXT(R"({"type":"error","status":"uncertain","outcome":"uncertain","message":"Outcome commit failed"})"));
	R->Append(TEXT("done"), TEXT(R"({"type":"done","status":"completed"})"));
	R->Finish(TEXT("no_terminal_event"));
	const auto Outcome = R->BuildStatusJson({})->GetObjectField(TEXT("outcome"));
	TestEqual(TEXT("uncertainty survives external finalization"), Outcome->GetStringField(TEXT("status")), FString(TEXT("uncertain")));
	TestEqual(TEXT("first terminal remains error"), Outcome->GetStringField(TEXT("terminal_event")), FString(TEXT("error")));
	TestTrue(TEXT("partial mutation ledger retained"), Outcome->GetObjectField(TEXT("mutation_outcomes"))->GetBoolField(TEXT("needs_attention")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExternalRuns_TypedOutcomeControlsProgress,
	"KismetAI.ExternalRuns.TypedOutcomeControlsProgress", KismetAIExternalRunsTest::Flags)
bool FExternalRuns_TypedOutcomeControlsProgress::RunTest(const FString& Parameters)
{
	auto R = KismetAIExternalRunsTest::MakeRecord(TEXT("typed-outcomes"));
	R->Append(TEXT("tool_result"), TEXT(R"({"is_error":true,"result":{"success":true}})"));
	TestEqual(TEXT("operation failure overrides successful envelope"),
		static_cast<int32>(R->BuildStatusJson({})->GetObjectField(TEXT("progress"))->GetNumberField(TEXT("tool_errors"))), 1);
	R->Append(TEXT("tool_result"), TEXT(R"({"is_error":false,"result":"opaque <tool_use_error> text"})"));
	TestEqual(TEXT("typed success keeps ordinary payload opaque"),
		static_cast<int32>(R->BuildStatusJson({})->GetObjectField(TEXT("progress"))->GetNumberField(TEXT("tool_errors"))), 1);
	R->Append(TEXT("tool_result"), TEXT(R"({"result":{"success":false}})"));
	TestEqual(TEXT("legacy event fallback"),
		static_cast<int32>(R->BuildStatusJson({})->GetObjectField(TEXT("progress"))->GetNumberField(TEXT("tool_errors"))), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExternalRuns_ExpiredCursorIsReported,
	"KismetAI.ExternalRuns.ExpiredCursorIsReported", KismetAIExternalRunsTest::Flags)
bool FExternalRuns_ExpiredCursorIsReported::RunTest(const FString& Parameters)
{
	using namespace KismetAIExternalRunsTest;
	FKismetAIExternalRunRef R = MakeRecord(TEXT("run-c"), 10);
	for (int32 I = 0; I < 40; ++I)
	{
		R->Append(TEXT("text_delta"), TEXT("{\"type\":\"text_delta\",\"text\":\"x\"}"));
	}
	const FKismetAIRunEventPage Old = R->Read(0, 100);
	TestTrue(TEXT("cursor before the window is expired"), Old.bExpired && Old.Events.Num() == 0);
	TestTrue(TEXT("bounded"), Old.LastSeq - Old.FirstRetainedSeq + 1 <= 10);
	TestEqual(TEXT("every event got a seq"), Old.LastSeq, static_cast<int64>(41));
	const FKismetAIRunEventPage Resume = R->Read(Old.FirstRetainedSeq - 1, 100);
	TestTrue(TEXT("resume_after is readable"), !Resume.bExpired && Resume.Events.Num() > 0 && Resume.Events[0].Seq == Old.FirstRetainedSeq);
	const TSharedPtr<FJsonObject> Events = R->BuildStatusJson({})->GetObjectField(TEXT("events"));
	TestTrue(TEXT("status reports dropped events"), Events->GetNumberField(TEXT("dropped")) > 0);

	// Over HTTP: an explicit 410 with where to resume, not a silent skip.
	FKismetAIModule& Module = FKismetAIModule::Get();
	FKismetAIExternalRunRef Http = MakeRecord(TEXT("p109-expired-run"), 10);
	for (int32 I = 0; I < 40; ++I) { Http->Append(TEXT("text_delta"), TEXT("{\"type\":\"text_delta\"}")); }
	Http->Finish(TEXT("completed"));
	FAccess::Registry(Module).Add(Http);
	const FRunReply Reply = GetEvents(TEXT("p109-expired-run"), 0);
	TestEqual(TEXT("410"), Reply.Code, 410);
	TestEqual(TEXT("cursor_expired"), Reply.ErrorCode(), FString(TEXT("cursor_expired")));
	TestTrue(TEXT("resume_after given"), Reply.Data().IsValid()
		&& Reply.Data()->GetNumberField(TEXT("resume_after")) == static_cast<double>(Http->Read(1000, 1).FirstRetainedSeq - 1));
	FRunReply Stream = GetEvents(TEXT("p109-expired-run"), 0, /*bStream=*/true);
	TestEqual(TEXT("a stream from an expired cursor is refused the same way"), Stream.Code, 410);
	const FRunReply Unknown = GetStatus(TEXT("no-such-run"));
	TestEqual(TEXT("unknown run 404"), Unknown.Code, 404);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExternalRuns_RetentionIsBounded,
	"KismetAI.ExternalRuns.RetentionIsBounded", KismetAIExternalRunsTest::Flags)
bool FExternalRuns_RetentionIsBounded::RunTest(const FString& Parameters)
{
	using namespace KismetAIExternalRunsTest;
	FKismetAIExternalRunRegistry::FLimits Limits;
	Limits.MaxFinishedRuns = 2;
	Limits.FinishedTtl = FTimespan::FromMinutes(10.0);
	FKismetAIExternalRunRegistry Registry(Limits);
	FKismetAIExternalRunRef Active = MakeRecord(TEXT("active"));
	Registry.Add(Active);
	for (int32 I = 0; I < 4; ++I)
	{
		FKismetAIExternalRunRef Done = MakeRecord(*FString::Printf(TEXT("done-%d"), I));
		Done->Finish(TEXT("completed"));
		Registry.Add(Done);
	}
	TestEqual(TEXT("active + 2 newest finished"), Registry.Num(), 3);
	TestTrue(TEXT("oldest finished evicted"), !Registry.Find(TEXT("done-0")).IsValid() && !Registry.Find(TEXT("done-1")).IsValid());
	TestTrue(TEXT("newest kept"), Registry.Find(TEXT("done-3")).IsValid());
	Registry.Prune(FDateTime::UtcNow() + FTimespan::FromHours(1.0));
	TestEqual(TEXT("TTL evicts finished, never the active run"), Registry.Num(), 1);
	TestTrue(TEXT("active kept"), Registry.Find(TEXT("active")).IsValid());
	return true;
}

// ── Module: the HTTP contract ─────────────────────────────────────────────────

// E7 reproduction on the route that existed earlier: POST /agent/run
// against a provider that takes 2 s. Before, the handler ran the loop inline
// and answered once, after `done`, with the model call long completed. Now it
// answers at once with a live stream that names the run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExternalRuns_CompatRouteAnswersBeforeModelCompletes,
	"KismetAI.ExternalRuns.CompatRouteAnswersBeforeModelCompletes", KismetAIExternalRunsTest::Flags)
bool FExternalRuns_CompatRouteAnswersBeforeModelCompletes::RunTest(const FString& Parameters)
{
	using namespace KismetAIExternalRunsTest;
	FScopedModuleSeams Seams;
	FKismetAIModule& Module = Seams.Module;
	if (FAccess::Session(Module).IsRunning()) { AddError(TEXT("a run is already active")); return false; }
	Seams.Seams->Model.BlockOnCall = 0;
	Seams.Seams->Model.BlockTimeoutMs = 2000;

	const double T0 = FPlatformTime::Seconds();
	FRunReply Reply = Call(&FAccess::Run, MakeRequest(TEXT("{\"input\":\"p109 compat\"}")));
	const double ReplyMs = (FPlatformTime::Seconds() - T0) * 1000.0;
	const int32 CompletedAtReply = Seams.Seams->Model.Completed.load();
	UE_LOG(LogTemp, Display, TEXT("evidence: /agent/run answered in %.1f ms, model calls completed: %d"), ReplyMs, CompletedAtReply);
	TestEqual(TEXT("200"), Reply.Code, 200);
	TestEqual(TEXT("answered before the model call completed"), CompletedAtReply, 0);
	const bool bStreaming = Reply.GetStream().IsValid();
	TestTrue(TEXT("a live stream, not a buffered body"), bStreaming);
	const TArray<FString>* RunHeader = Reply.Response.IsValid() ? Reply.Response->Headers.Find(TEXT("X-Kismet-Run-Id")) : nullptr;
	TestTrue(TEXT("names the run"), RunHeader && RunHeader->Num() == 1);
	const FString RunId = RunHeader && RunHeader->Num() == 1 ? (*RunHeader)[0] : FString();
	ON_SCOPE_EXIT { SettleRun(Module, RunId, Seams.Seams->Model); };
	if (bStreaming)
	{
		const FString First = Reply.DrainStream();
		TestTrue(TEXT("first frame is the run"), First.Contains(TEXT("event: run")) && First.Contains(RunId));
		TestFalse(TEXT("no done in the first chunk"), First.Contains(TEXT("event: done")));
	}
	const FKismetAIExternalRunPtr Record = FAccess::Registry(Module).Find(RunId);
	TestTrue(TEXT("the run completes on its own"), PumpUntil([&Record]() { return Record.IsValid() && Record->IsFinished(); }));
	return true;
}

// E7 reproduction: the submit answers, and progress is observable, while the
// model call is still in flight. Previously, the only route ran the loop
// inline and answered after `done`.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExternalRuns_ProgressBeforeCompletion,
	"KismetAI.ExternalRuns.ProgressBeforeCompletion", KismetAIExternalRunsTest::Flags)
bool FExternalRuns_ProgressBeforeCompletion::RunTest(const FString& Parameters)
{
	using namespace KismetAIExternalRunsTest;
	FScopedModuleSeams Seams;
	FKismetAIModule& Module = Seams.Module;
	if (FAccess::Session(Module).IsRunning()) { AddError(TEXT("a run is already active")); return false; }
	Seams.Seams->Model.BlockOnCall = 0;

	const double T0 = FPlatformTime::Seconds();
	const FRunReply Submit = Call(&FAccess::Submit, MakeRequest(TEXT("{\"input\":\"p109 slow\"}")));
	const double SubmitMs = (FPlatformTime::Seconds() - T0) * 1000.0;
	if (!TestEqual(TEXT("202 Accepted"), Submit.Code, 202)) return false;
	const FString RunId = Submit.Data()->GetStringField(TEXT("run_id"));
	ON_SCOPE_EXIT { SettleRun(Module, RunId, Seams.Seams->Model); };
	TestFalse(TEXT("run id issued at admission"), RunId.IsEmpty());
	TestEqual(TEXT("model call not completed when submit answered"), Seams.Seams->Model.Completed.load(), 0);
	UE_LOG(LogTemp, Display, TEXT("evidence: submit answered in %.1f ms, model calls completed: %d"), SubmitMs, Seams.Seams->Model.Completed.load());

	if (!TestTrue(TEXT("the worker is inside the model call"), Seams.Seams->Model.Entered.Wait(PromptMs))) return false;
	// The game thread is free: this handler runs on it while the model call blocks.
	const FRunReply Running = GetStatus(RunId);
	TestEqual(TEXT("status 200"), Running.Code, 200);
	TestEqual(TEXT("running"), StateOf(Running), FString(TEXT("running")));
	TestEqual(TEXT("phase model"), Running.Data()->GetStringField(TEXT("phase")), FString(TEXT("model")));
	TestTrue(TEXT("no outcome yet"), Running.Data()->HasTypedField<EJson::Null>(TEXT("outcome")));
	TestEqual(TEXT("worker running"), Running.Data()->GetObjectField(TEXT("worker"))->GetStringField(TEXT("state")), FString(TEXT("running")));

	const FRunReply Early = GetEvents(RunId, 0);
	const TArray<FString> EarlyTypes = EventTypes(Early);
	TestTrue(TEXT("progress before completion"), EarlyTypes.Contains(TEXT("run")) && EarlyTypes.Contains(TEXT("attempt_start")));
	TestFalse(TEXT("no done yet"), EarlyTypes.Contains(TEXT("done")));

	// A live stream attached now carries the same progress in its first chunk.
	FRunReply Stream = GetEvents(RunId, 0, /*bStream=*/true);
	TestEqual(TEXT("stream 200"), Stream.Code, 200);
	const FString Chunk = Stream.DrainStream();
	TestTrue(TEXT("SSE frames with ids"), Chunk.Contains(TEXT("id: 1\nevent: run\n")) && Chunk.Contains(TEXT("attempt_start")));
	TestFalse(TEXT("stream has no done yet"), Chunk.Contains(TEXT("event: done")));
	Stream.Response.Reset();   // client goes away; the run is unaffected

	Seams.Seams->Model.Unblock();
	const FKismetAIExternalRunPtr Record = FAccess::Registry(Module).Find(RunId);
	TestTrue(TEXT("finishes"), PumpUntil([&Record]() { return Record.IsValid() && Record->IsFinished(); }));
	const FRunReply Final = GetStatus(RunId);
	TestEqual(TEXT("finished"), StateOf(Final), FString(TEXT("finished")));
	const TSharedPtr<FJsonObject> Outcome = Final.Data()->GetObjectField(TEXT("outcome"));
	TestEqual(TEXT("completed"), Outcome->GetStringField(TEXT("status")), FString(TEXT("completed")));
	// One id: the done event's run_id is the HTTP run id.
	TestEqual(TEXT("done.run_id is the run id"), Outcome->GetObjectField(TEXT("terminal"))->GetStringField(TEXT("run_id")), RunId);
	TestFalse(TEXT("lease released"), FAccess::Session(Module).IsRunning());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExternalRuns_CancelDuringRun,
	"KismetAI.ExternalRuns.CancelDuringRun", KismetAIExternalRunsTest::Flags)
bool FExternalRuns_CancelDuringRun::RunTest(const FString& Parameters)
{
	using namespace KismetAIExternalRunsTest;
	FScopedModuleSeams Seams;
	FKismetAIModule& Module = Seams.Module;
	if (FAccess::Session(Module).IsRunning()) { AddError(TEXT("a run is already active")); return false; }
	Seams.Seams->Model.BlockOnCall = 0;

	const FRunReply Submit = Call(&FAccess::Submit, MakeRequest(TEXT("{\"input\":\"p109 cancel me\"}")));
	if (!TestEqual(TEXT("202"), Submit.Code, 202)) return false;
	const FString RunId = Submit.Data()->GetStringField(TEXT("run_id"));
	ON_SCOPE_EXIT { SettleRun(Module, RunId, Seams.Seams->Model); };
	if (!TestTrue(TEXT("in the model call"), Seams.Seams->Model.Entered.Wait(PromptMs))) return false;

	const double T0 = FPlatformTime::Seconds();
	const FRunReply First = PostCancel(RunId);
	UE_LOG(LogTemp, Display, TEXT("evidence: cancel acknowledged in %.1f ms during the model call"), (FPlatformTime::Seconds() - T0) * 1000.0);
	TestEqual(TEXT("202 on the first cancel"), First.Code, 202);
	TestEqual(TEXT("requested"), First.Data()->GetStringField(TEXT("cancel")), FString(TEXT("requested")));
	TestEqual(TEXT("caught in the model phase"), First.Data()->GetStringField(TEXT("phase_at_request")), FString(TEXT("model")));
	const FRunReply Second = PostCancel(RunId);
	TestEqual(TEXT("idempotent: 200"), Second.Code, 200);
	TestEqual(TEXT("already_requested"), Second.Data()->GetStringField(TEXT("cancel")), FString(TEXT("already_requested")));

	const FKismetAIExternalRunPtr Record = FAccess::Registry(Module).Find(RunId);
	TestTrue(TEXT("finishes after cancel"), PumpUntil([&Record]() { return Record.IsValid() && Record->IsFinished(); }));
	const FRunReply Final = GetStatus(RunId);
	const TSharedPtr<FJsonObject> Outcome = Final.Data()->GetObjectField(TEXT("outcome"));
	TestEqual(TEXT("aborted"), Outcome->GetStringField(TEXT("status")), FString(TEXT("aborted")));
	TestTrue(TEXT("outcome records the cancel"), Outcome->GetBoolField(TEXT("cancel_requested")));
	TestEqual(TEXT("one model call"), Seams.Seams->Model.Calls.load(), 1);
	const FRunReply Late = PostCancel(RunId);
	TestEqual(TEXT("already_finished"), Late.Data()->GetStringField(TEXT("cancel")), FString(TEXT("already_finished")));
	TestEqual(TEXT("unknown run"), PostCancel(TEXT("no-such-run")).Code, 404);
	return true;
}

// The adversarial case: an edit is applied, the client drops before the run
// answers, reconnects and even resubmits the same request. The edit happens
// once and the run executes once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExternalRuns_DisconnectReattachNoDuplicate,
	"KismetAI.ExternalRuns.DisconnectReattachNoDuplicate", KismetAIExternalRunsTest::Flags)
bool FExternalRuns_DisconnectReattachNoDuplicate::RunTest(const FString& Parameters)
{
	using namespace KismetAIExternalRunsTest;
	FScopedModuleSeams Seams;
	FKismetAIModule& Module = Seams.Module;
	if (FAccess::Session(Module).IsRunning()) { AddError(TEXT("a run is already active")); return false; }
	// Call 0 asks for a tool, call 1 (after the tool result) blocks.
	FPrintFixture Fixture(TEXT("BP_KismetP109Once"));
	if (!TestNotNull(TEXT("fixture"), Fixture.Print)) return false;
	Seams.Seams->Model.Script = { ToolTurn(TEXT("toolu_1"), TEXT("edit_blueprint"), Fixture.SetDurationArgs(TEXT("7.5"))), TextTurn(TEXT("all done")) };
	Seams.Seams->Model.BlockOnCall = 1;

	const FString Body = TEXT("{\"input\":\"p109 once\",\"client_request_id\":\"p109-req-1\"}");
	FRunReply Live = Call(&FAccess::Run, MakeRequest(Body));
	if (!TestEqual(TEXT("compat route streams 200"), Live.Code, 200)) return false;
	TestTrue(TEXT("streaming response"), Live.GetStream().IsValid());
	const FString Header = Live.Response->Headers.Contains(TEXT("X-Kismet-Run-Id")) ? Live.Response->Headers[TEXT("X-Kismet-Run-Id")][0] : FString();
	const FString First = Live.DrainStream();
	TestTrue(TEXT("first frame names the run"), First.Contains(TEXT("event: run")) && First.Contains(Header));
	const FString RunId = Header;
	ON_SCOPE_EXIT { SettleRun(Module, RunId, Seams.Seams->Model); };

	// The tool call is dispatched to the game thread; pump until the worker
	// is back in the (blocking) second model call.
	TestTrue(TEXT("tool ran, second model call entered"), PumpUntil([&Seams]() { return Seams.Seams->Model.Entered.Wait(0); }));
	const int32 AdmittedAfterEdit = Seams.Policy.Admissions.load();
	TestEqual(TEXT("the edit was admitted once"), AdmittedAfterEdit, 1);
	TestEqual(TEXT("the edit applied"), Fixture.Duration(), FString(TEXT("7.5")));

	// Disconnect: the response (and its queue) is gone. The run continues.
	Live.Response.Reset();
	TestEqual(TEXT("still running"), StateOf(GetStatus(RunId)), FString(TEXT("running")));

	// Retry the same submission: it names the same run, starts nothing.
	const FRunReply Again = Call(&FAccess::Submit, MakeRequest(Body));
	TestEqual(TEXT("existing run: 200"), Again.Code, 200);
	TestEqual(TEXT("same run"), Again.Data()->GetStringField(TEXT("run_id")), RunId);
	TestTrue(TEXT("marked existing"), Again.Data()->GetBoolField(TEXT("existing")));
	// A different request under the same id is a conflict.
	const FRunReply Conflict = Call(&FAccess::Submit, MakeRequest(TEXT("{\"input\":\"something else\",\"client_request_id\":\"p109-req-1\"}")));
	TestEqual(TEXT("409 conflict"), Conflict.Code, 409);
	TestEqual(TEXT("request_id_conflict"), Conflict.ErrorCode(), FString(TEXT("request_id_conflict")));

	Seams.Seams->Model.Unblock();
	const FKismetAIExternalRunPtr Record = FAccess::Registry(Module).Find(RunId);
	TestTrue(TEXT("finishes"), PumpUntil([&Record]() { return Record.IsValid() && Record->IsFinished(); }));

	// Reattach from the start: the whole history, one done, no second run.
	const FRunReply Page = GetEvents(RunId, 0);
	const TArray<FString> Types = EventTypes(Page);
	int32 Done = 0;
	for (const FString& T : Types) { Done += T == TEXT("done") ? 1 : 0; }
	TestEqual(TEXT("exactly one terminal event"), Done, 1);
	TestEqual(TEXT("one tool_call"), Types.FilterByPredicate([](const FString& T) { return T == TEXT("tool_call"); }).Num(), 1);
	TestEqual(TEXT("tool admitted once in total"), Seams.Policy.Admissions.load(), 1);
	TestEqual(TEXT("two model calls in total"), Seams.Seams->Model.Calls.load(), 2);
	TestEqual(TEXT("a repeated id after the end still names the run"),
		Call(&FAccess::Submit, MakeRequest(Body)).Data()->GetStringField(TEXT("run_id")), RunId);
	TestEqual(TEXT("still two model calls"), Seams.Seams->Model.Calls.load(), 2);

	// A stream attached after the end replays and completes.
	FRunReply Replay = GetEvents(RunId, 0, /*bStream=*/true);
	const FString All = Replay.DrainStream();
	TestTrue(TEXT("replayed stream ends with done"), All.Contains(TEXT("event: done")));
	TestTrue(TEXT("replayed stream is complete"), Replay.GetStream().IsComplete());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExternalRuns_InvalidRunBudget,
	"KismetAI.ExternalRuns.InvalidRunBudget", KismetAIExternalRunsTest::Flags)
bool FExternalRuns_InvalidRunBudget::RunTest(const FString& Parameters)
{
	using namespace KismetAIExternalRunsTest;
	for (const TCHAR* Budget : { TEXT("null"), TEXT("[]"), TEXT("\"unlimited\""),
		TEXT("{\"max_model_requests\":-1}"), TEXT("{\"max_tool_calls\":1.5}"),
		TEXT("{\"max_elapsed_seconds\":\"10\"}"), TEXT("{\"max_output_bytes\":-1}"),
		TEXT("{\"max_estimated_spend_usd\":-0.01}"), TEXT("{\"max_elapsed_seconds\":1e999}") })
	{
		FScopedModuleSeams Seams;
		const FString Body = FString::Printf(TEXT("{\"input\":\"validate budget\",\"run_budget\":%s}"), Budget);
		const FRunReply Reply = Call(&FAccess::Submit, MakeRequest(Body));
		TestEqual(*FString::Printf(TEXT("malformed budget rejected before admission: %s"), Budget), Reply.Code, 400);
		if (Reply.Code == 202 && Reply.Data().IsValid())
		{
			SettleRun(Seams.Module, Reply.Data()->GetStringField(TEXT("run_id")), Seams.Seams->Model);
		}
		else
		{
			TestEqual(TEXT("rejected limits never call the provider"), Seams.Seams->Model.Calls.load(), 0);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExternalRuns_RunBudgetAdmission,
	"KismetAI.ExternalRuns.RunBudgetAdmission", KismetAIExternalRunsTest::Flags)
bool FExternalRuns_RunBudgetAdmission::RunTest(const FString& Parameters)
{
	using namespace KismetAIExternalRunsTest;
	FScopedModuleSeams Seams;
	FPrintFixture Fixture(TEXT("BP_KismetP209Budget"));
	if (!TestNotNull(TEXT("read fixture"), Fixture.Print)) return false;
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), Fixture.BP->GetPathName());
	Args->SetStringField(TEXT("mode"), TEXT("describe"));
	Seams.Seams->Model.Script = { ToolTurn(TEXT("read"), TEXT("read_blueprint"), Args), TextTurn(TEXT("finished")) };
	const FRunReply Reply = Call(&FAccess::Submit, MakeRequest(TEXT("{\"input\":\"inspect the fixture\",\"run_budget\":{\"max_model_requests\":1}}")));
	if (!TestEqual(TEXT("valid limits admitted"), Reply.Code, 202)) return false;
	const FString RunId = Reply.Data()->GetStringField(TEXT("run_id"));
	ON_SCOPE_EXIT { SettleRun(Seams.Module, RunId, Seams.Seams->Model); };
	const FKismetAIExternalRunPtr Record = FAccess::Registry(Seams.Module).Find(RunId);
	if (!TestTrue(TEXT("bounded run finishes"), PumpUntil([&Record]() { return Record.IsValid() && Record->IsFinished(); }))) return false;
	TestEqual(TEXT("HTTP model limit reaches the production worker"), Seams.Seams->Model.Calls.load(), 1);
	const FRunReply Status = GetStatus(RunId);
	const auto Outcome = Status.Data()->GetObjectField(TEXT("outcome"));
	const auto Terminal = Outcome->GetObjectField(TEXT("terminal"));
	TestEqual(TEXT("exhausted request budget is partial"), Outcome->GetStringField(TEXT("status")), FString(TEXT("partial")));
	TestEqual(TEXT("specific limit stop"), Terminal->GetStringField(TEXT("stop_reason")), FString(TEXT("run_budget_exhausted")));
	TestTrue(TEXT("budget terminal remains committed"), Terminal->GetBoolField(TEXT("persistence_complete")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExternalRuns_RunBudgetRequestIdentity,
	"KismetAI.ExternalRuns.RunBudgetRequestIdentity", KismetAIExternalRunsTest::Flags)
bool FExternalRuns_RunBudgetRequestIdentity::RunTest(const FString& Parameters)
{
	using namespace KismetAIExternalRunsTest;
	FScopedModuleSeams Seams;
	Seams.Seams->Model.BlockOnCall = 0;
	const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Body = FString::Printf(TEXT("{\"input\":\"bounded request\",\"client_request_id\":\"%s\",\"run_budget\":{\"max_model_requests\":1}}"), *Id);
	const FRunReply Reply = Call(&FAccess::Submit, MakeRequest(Body));
	if (!TestEqual(TEXT("initial request accepted"), Reply.Code, 202)) return false;
	const FString RunId = Reply.Data()->GetStringField(TEXT("run_id"));
	ON_SCOPE_EXIT { SettleRun(Seams.Module, RunId, Seams.Seams->Model); };
	const FString Changed = Body.Replace(TEXT("\"max_model_requests\":1"), TEXT("\"max_model_requests\":2"));
	const FRunReply Conflict = Call(&FAccess::Submit, MakeRequest(Changed));
	TestEqual(TEXT("same request id cannot change admitted limits"), Conflict.Code, 409);
	TestEqual(TEXT("budget conflict is identified"), Conflict.ErrorCode(), FString(TEXT("request_id_conflict")));
	const FRunReply Same = Call(&FAccess::Submit, MakeRequest(Body));
	TestTrue(TEXT("same limits safely reattach"), Same.Data().IsValid() && Same.Data()->GetStringField(TEXT("run_id")) == RunId);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExternalRuns_RunBudgetSettings,
	"KismetAI.ExternalRuns.RunBudgetSettings", KismetAIExternalRunsTest::Flags)
bool FExternalRuns_RunBudgetSettings::RunTest(const FString& Parameters)
{
	using namespace KismetAIExternalRunsTest;
	FKismetAIModule& Module = FKismetAIModule::Get();
	const FKismetAIAgentConfig Original = FAccess::Config(Module);
	const FString OriginalDir = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMET_CONFIG_DIR"));
	const FString Directory = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("KismetP209Settings"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Directory, true);
	FPlatformMisc::SetEnvironmentVar(TEXT("KISMET_CONFIG_DIR"), *Directory);
	ON_SCOPE_EXIT
	{
		FAccess::Config(Module) = Original;
		FPlatformMisc::SetEnvironmentVar(TEXT("KISMET_CONFIG_DIR"), *OriginalDir);
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	const FString Path = Directory / TEXT("settings.json");
	FAccess::Config(Module) = FKismetAIAgentConfig();
	const FString Valid = TEXT("{\"run_budget\":{\"max_elapsed_seconds\":12.5,\"max_model_requests\":2,\"max_tool_calls\":3,\"max_output_bytes\":4096,\"max_estimated_spend_usd\":0.25}}");
	if (!TestTrue(TEXT("write disposable settings"), FFileHelper::SaveStringToFile(Valid, *Path))) return false;
	FAccess::LoadSettings(Module);
	const auto& Limits = FAccess::Config(Module).RunLimits;
	TestEqual(TEXT("settings elapsed cap reaches config"), Limits.MaxElapsedSeconds, 12.5);
	TestEqual(TEXT("settings model cap reaches config"), Limits.MaxModelRequests, 2);
	TestEqual(TEXT("settings tool cap reaches config"), Limits.MaxToolCalls, 3);
	TestEqual(TEXT("settings byte cap reaches config"), Limits.MaxOutputBytes, static_cast<int64>(4096));
	TestEqual(TEXT("settings spend cap reaches config"), Limits.MaxEstimatedSpendUSD, 0.25);
	Module.SaveSettings();
	FString Saved;
	FFileHelper::LoadFileToString(Saved, *Path);
	TestTrue(TEXT("saved settings retain configured limits"), Saved.Contains(TEXT("max_model_requests")) && Saved.Contains(TEXT("max_estimated_spend_usd")));
	for (const FString& Invalid : { FString(TEXT("{\"run_budget\":{\"max_model_requests\":-1}}")), FString(TEXT("{\"run_budget\":")) })
	{
		FAccess::Config(Module) = FKismetAIAgentConfig();
		FFileHelper::SaveStringToFile(Invalid, *Path);
		FAccess::LoadSettings(Module);
		TestFalse(TEXT("invalid persisted limit cannot silently become unlimited"), FAccess::Config(Module).RunBudgetError.IsEmpty());
		Module.SaveSettings();
		FFileHelper::LoadFileToString(Saved, *Path);
		TestEqual(TEXT("unrelated save preserves invalid settings for correction"), Saved, Invalid);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExternalRuns_AssignedSessionHeader,
	"KismetAI.ExternalRuns.AssignedSessionHeader", KismetAIExternalRunsTest::Flags)
bool FExternalRuns_AssignedSessionHeader::RunTest(const FString& Parameters)
{
	using namespace KismetAIExternalRunsTest;
	FScopedModuleSeams Seams;
	FString SessionId;
	for (int32 Turn = 0; Turn < 2; ++Turn)
	{
		const FString Body = Turn == 0 ? TEXT("{\"input\":\"new session header\"}")
			: FString::Printf(TEXT("{\"input\":\"resume same session\",\"session_id\":\"%s\"}"), *SessionId);
		const FRunReply Reply = Call(&FAccess::Submit, MakeRequest(Body));
		if (!TestEqual(TEXT("run admitted"), Reply.Code, 202)) return false;
		const FString RunId = Reply.Data()->GetStringField(TEXT("run_id"));
		SessionId = Reply.Data()->GetStringField(TEXT("session_id"));
		ON_SCOPE_EXIT { SettleRun(Seams.Module, RunId, Seams.Seams->Model); };
		const FKismetAIExternalRunPtr Record = FAccess::Registry(Seams.Module).Find(RunId);
		if (!TestTrue(TEXT("run settled"), PumpUntil([&Record]() { return Record.IsValid() && Record->IsFinished(); }))) return false;
		const auto Entries = FKismetAISessionStorage::LoadAllEntries(SessionId);
		int32 Headers = 0;
		for (const auto& Entry : Entries) if (Entry->GetStringField(TEXT("type")) == TEXT("session_start"))
		{
			++Headers;
			TestTrue(TEXT("header contains actual prompt"), Entry->HasTypedField<EJson::String>(TEXT("system_prompt")));
		}
		TestEqual(TEXT("exactly one durable session header on fresh and resumed runs"), Headers, 1);
	}
	TestEqual(TEXT("two model turns exercised"), Seams.Seams->Model.Calls.load(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExternalRuns_MissingResumeRefused,
	"KismetAI.ExternalRuns.MissingResumeRefused", KismetAIExternalRunsTest::Flags)
bool FExternalRuns_MissingResumeRefused::RunTest(const FString& Parameters)
{
	using namespace KismetAIExternalRunsTest;
	FScopedModuleSeams Seams;
	const FString SessionId = FKismetAISessionStorage::CreateSessionId();
	const FString Body = FString::Printf(TEXT("{\"input\":\"resume missing history\",\"session_id\":\"%s\"}"), *SessionId);
	const FRunReply Reply = Call(&FAccess::Submit, MakeRequest(Body));
	if (!TestEqual(TEXT("admitted run reports terminal refusal"), Reply.Code, 202)) return false;
	const FString RunId = Reply.Data()->GetStringField(TEXT("run_id"));
	ON_SCOPE_EXIT { SettleRun(Seams.Module, RunId, Seams.Seams->Model); };
	const FKismetAIExternalRunPtr Record = FAccess::Registry(Seams.Module).Find(RunId);
	if (!TestTrue(TEXT("run settled"), PumpUntil([&Record]() { return Record.IsValid() && Record->IsFinished(); }))) return false;
	TestEqual(TEXT("missing resume must never call model"), Seams.Seams->Model.Calls.load(), 0);
	TestFalse(TEXT("missing resume must not create replacement journal"), IFileManager::Get().FileExists(*FKismetAISessionStorage::GetSessionFilePath(SessionId)));
	return true;
}
