// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Characterization tests for the agent loop.
//
// These pin what RunTask *currently does*, not what it should do. They are the
// regression contract for the rest of the agent-core overhaul: through
// Move this behavior between classes, and these tests are how we know the
// behavior survived the move. Where a test documents something questionable,
// the comment says so rather than the test asserting an improvement.
//
// Previously, this file could not exist: RunTask reached the network through
// private methods and the editor through FKismetAIModule&, so driving it needed
// a live API key and a loaded editor. KismetAI.Agent had one test for 1,288
// lines carrying six loop-health rules, adaptive budgeting, truncation
// continuation and two-phase compaction.

#include "Fakes/ScriptedAgentSeams.h"
#include "Misc/AutomationTest.h"
#include "KismetAIMutationOutcome.h"

using namespace KismetAITestFakes;

constexpr auto LoopTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	// Validation runs inside ExecuteTool before the invoker is reached, so a
	// scripted tool call needs arguments the real validator accepts. Getting this
	// wrong shows up as "the tool ran 0 times", not as a validation error.
	TSharedPtr<FJsonObject> BlueprintArgs(const TCHAR* Path)
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("blueprint"), Path);
		return A;
	}

	// edit_blueprint additionally requires a non-empty operations array.
	TSharedPtr<FJsonObject> EditArgs(const TCHAR* Path, const TCHAR* NodeName)
	{
		TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
		Op->SetStringField(TEXT("action"), TEXT("set_pin_default"));
		Op->SetStringField(TEXT("node"), NodeName);
		Op->SetStringField(TEXT("pin"), TEXT("InString"));
		Op->SetStringField(TEXT("value"), TEXT("hi"));

		TSharedPtr<FJsonObject> A = BlueprintArgs(Path);
		A->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(Op) });
		return A;
	}

	// insert_node requires blueprint + node (the menu name).
	TSharedPtr<FJsonObject> InsertArgs(const TCHAR* Path, const TCHAR* NodeName)
	{
		TSharedPtr<FJsonObject> A = BlueprintArgs(Path);
		A->SetStringField(TEXT("node"), NodeName);
		return A;
	}

	// search_assets requires at least one of query / class_filter / path.
	TSharedPtr<FJsonObject> SearchArgs(const TCHAR* Query)
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("query"), Query);
		return A;
	}

	// A config that keeps runs short and deterministic.
	FKismetAIAgentConfig MakeTestConfig(int32 MaxSteps = 8)
	{
		FKismetAIAgentConfig C;
		C.ApiKey = TEXT("test-key-unused");
		C.Model = TEXT("claude-sonnet-4-6");
		C.MaxSteps = MaxSteps;
		C.MaxRetries = 0;
		C.RetryJitterFactor = 0.0;
		// Thinking off: the thinking floor rewrites MaxTokens at run start and
		// would mask the adaptive-budget assertions below.
		C.ThinkingMode = EThinkingMode::Disabled;
		return C;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Turn shape
// ─────────────────────────────────────────────────────────────────────────────

// The smoke test that proves the seams work: a whole run, no editor, no network.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoop_TextOnlyTurnCompletes, "KismetAI.AgentLoop.TextOnlyTurnCompletes", LoopTestFlags)
bool FLoop_TextOnlyTurnCompletes::RunTest(const FString& Parameters)
{
	FScriptedModelClient Model({ MakeTextTurn(TEXT("All done.")) });
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;

	FKismetAIAgent Agent(Tools, Model, MakeTestConfig());
	const FString Result = Agent.RunTask(TEXT("do nothing"), Rec.MakeCallback());

	TestEqual(TEXT("returns the assistant text"), Result, FString(TEXT("All done.")));
	TestEqual(TEXT("one model call"), Model.NumCalls(), 1);
	TestEqual(TEXT("no tools invoked"), Tools.CallLog.Num(), 0);
	TestTrue(TEXT("emits session_start"), Rec.HasType(TEXT("session_start")));
	TestEqual(TEXT("emits exactly one done"), Rec.Count(TEXT("done")), 1);

	const FString* Done = Rec.FirstJsonOfType(TEXT("done"));
	TestTrue(TEXT("done reports completed"), Done && Done->Contains(TEXT("\"status\":\"completed\"")));
	return true;
}

// tool_use -> tool runs -> result is paired back by tool_use_id on the next call.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoop_ToolCallRoundTrip, "KismetAI.AgentLoop.ToolCallRoundTrip", LoopTestFlags)
bool FLoop_ToolCallRoundTrip::RunTest(const FString& Parameters)
{
	FScriptedModelClient Model({
		MakeToolTurn(TEXT("toolu_1"), TEXT("compile_blueprint"), BlueprintArgs(TEXT("/Game/BP_Test"))),
		MakeTextTurn(TEXT("Compiled.")),
	});
	FScriptedToolInvoker Tools;
	Tools.SetResult(TEXT("compile_blueprint"), TEXT("{\"success\":true,\"errors\":[]}"));
	FEventRecorder Rec;

	FKismetAIAgent Agent(Tools, Model, MakeTestConfig());
	Agent.RunTask(TEXT("compile it"), Rec.MakeCallback());

	TestEqual(TEXT("tool invoked once"), Tools.CountCalls(TEXT("compile_blueprint")), 1);
	TestEqual(TEXT("two model calls"), Model.NumCalls(), 2);
	TestEqual(TEXT("one tool_call event"), Rec.Count(TEXT("tool_call")), 1);
	TestEqual(TEXT("one tool_result event"), Rec.Count(TEXT("tool_result")), 1);

	// The second request must carry the assistant tool_use and a paired user
	// tool_result — the provider invariant ValidateToolPairing protects.
	if (Model.GetRequests().Num() < 2)
	{
		AddError(TEXT("expected a second model call"));
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& Msgs = Model.GetRequests()[1].Messages;
	bool bFoundToolUse = false, bFoundToolResult = false;
	for (const TSharedPtr<FJsonValue>& M : Msgs)
	{
		const TSharedPtr<FJsonObject> Obj = M->AsObject();
		if (!Obj.IsValid()) continue;
		const TArray<TSharedPtr<FJsonValue>>* Content;
		if (!Obj->TryGetArrayField(TEXT("content"), Content)) continue;
		for (const TSharedPtr<FJsonValue>& B : *Content)
		{
			const TSharedPtr<FJsonObject> BO = B->AsObject();
			if (!BO.IsValid()) continue;
			FString Type; BO->TryGetStringField(TEXT("type"), Type);
			FString Id;
			if (Type == TEXT("tool_use") && BO->TryGetStringField(TEXT("id"), Id) && Id == TEXT("toolu_1"))
			{
				bFoundToolUse = true;
			}
			if (Type == TEXT("tool_result") && BO->TryGetStringField(TEXT("tool_use_id"), Id) && Id == TEXT("toolu_1"))
			{
				bFoundToolResult = true;
			}
		}
	}
	TestTrue(TEXT("second request carries the tool_use"), bFoundToolUse);
	TestTrue(TEXT("second request carries the paired tool_result"), bFoundToolResult);
	return true;
}

// Several tool calls in one turn produce results in call order, in one user message.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoop_MultipleToolsInOneTurn, "KismetAI.AgentLoop.MultipleToolsInOneTurn", LoopTestFlags)
bool FLoop_MultipleToolsInOneTurn::RunTest(const FString& Parameters)
{
	FModelCallOutcome Two = MakeResponse({
		MakeToolUseBlock(TEXT("t1"), TEXT("read_blueprint"), BlueprintArgs(TEXT("/Game/BP_A"))),
		MakeToolUseBlock(TEXT("t2"), TEXT("search_assets"),  SearchArgs(TEXT("Player"))),
	}, TEXT("tool_use"));

	FScriptedModelClient Model({ Two, MakeTextTurn(TEXT("ok")) });
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;

	FKismetAIAgent Agent(Tools, Model, MakeTestConfig());
	Agent.RunTask(TEXT("look around"), Rec.MakeCallback());

	// Guard before indexing: a bare TArray operator[] on an unexpected result
	// fires a check() and takes down the whole automation run, turning one
	// failed assertion into a lost suite.
	if (Tools.CallLog.Num() != 2)
	{
		AddError(FString::Printf(TEXT("expected 2 tool calls, got %d"), Tools.CallLog.Num()));
		return false;
	}
	TestEqual(TEXT("in call order (first)"), Tools.CallLog[0], FString(TEXT("read_blueprint")));
	TestEqual(TEXT("in call order (second)"), Tools.CallLog[1], FString(TEXT("search_assets")));
	TestEqual(TEXT("two tool_result events"), Rec.Count(TEXT("tool_result")), 2);
	return true;
}

// Exhausting the turn budget ends the run as max_steps, not as an error.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoop_MaxStepsExhausted, "KismetAI.AgentLoop.MaxStepsExhausted", LoopTestFlags)
bool FLoop_MaxStepsExhausted::RunTest(const FString& Parameters)
{
	// Always asks for a tool, never finishes.
	FScriptedModelClient Model({ MakeToolTurn(TEXT("t"), TEXT("read_blueprint"), BlueprintArgs(TEXT("/Game/BP"))) });
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;

	FKismetAIAgent Agent(Tools, Model, MakeTestConfig(/*MaxSteps=*/3));
	Agent.RunTask(TEXT("spin"), Rec.MakeCallback());

	TestEqual(TEXT("stops at the turn budget"), Model.NumCalls(), 3);
	const FString* Done = Rec.FirstJsonOfType(TEXT("done"));
	TestTrue(TEXT("done reports max_steps"), Done && Done->Contains(TEXT("\"status\":\"max_steps\"")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Loop health (R1–R6). See 00-PLAN.md §6.7 — none of this may be lost in the
// refactor, and until now none of it was covered.
// ─────────────────────────────────────────────────────────────────────────────

// R3: an identical repeat of a read-only call is short-circuited, not re-run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoop_R3_DuplicateReadShortCircuits, "KismetAI.AgentLoop.R3_DuplicateReadShortCircuits", LoopTestFlags)
bool FLoop_R3_DuplicateReadShortCircuits::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Args = BlueprintArgs(TEXT("/Game/BP_Test"));

	FScriptedModelClient Model({
		MakeResponse({ MakeToolUseBlock(TEXT("t1"), TEXT("read_blueprint"), Args) }, TEXT("tool_use")),
		MakeResponse({ MakeToolUseBlock(TEXT("t2"), TEXT("read_blueprint"), Args) }, TEXT("tool_use")),
		MakeTextTurn(TEXT("done")),
	});
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;

	FKismetAIAgent Agent(Tools, Model, MakeTestConfig());
	Agent.RunTask(TEXT("read twice"), Rec.MakeCallback());

	TestEqual(TEXT("the tool only actually ran once"), Tools.CountCalls(TEXT("read_blueprint")), 1);

	// The second result is the DUPLICATE_CALL marker rather than a re-read.
	bool bSawDuplicateMarker = false;
	for (const TPair<FString, FString>& E : Rec.Events)
	{
		if (E.Key == TEXT("tool_result") && E.Value.Contains(TEXT("DUPLICATE_CALL")))
		{
			bSawDuplicateMarker = true;
		}
	}
	TestTrue(TEXT("second call returns DUPLICATE_CALL"), bSawDuplicateMarker);
	return true;
}

// R3b: an identical resend of a mutation that already failed is short-circuited.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoop_R3b_RepeatedFailedEditShortCircuits, "KismetAI.AgentLoop.R3b_RepeatedFailedEditShortCircuits", LoopTestFlags)
bool FLoop_R3b_RepeatedFailedEditShortCircuits::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Args = EditArgs(TEXT("/Game/BP_Test"), TEXT("PrintString"));

	FScriptedModelClient Model({
		MakeResponse({ MakeToolUseBlock(TEXT("t1"), TEXT("edit_blueprint"), Args) }, TEXT("tool_use")),
		MakeResponse({ MakeToolUseBlock(TEXT("t2"), TEXT("edit_blueprint"), Args) }, TEXT("tool_use")),
		MakeTextTurn(TEXT("gave up")),
	});
	FScriptedToolInvoker Tools;
	Tools.SetTypedResult(TEXT("edit_blueprint"), FToolExecutionResult::NotStartedError(400, TEXT("PIN_NOT_FOUND"), TEXT("no such pin")));
	FEventRecorder Rec;

	FKismetAIAgent Agent(Tools, Model, MakeTestConfig());
	Agent.RunTask(TEXT("edit twice"), Rec.MakeCallback());

	TestEqual(TEXT("the failing edit only ran once"), Tools.CountCalls(TEXT("edit_blueprint")), 1);

	bool bSawRepeatMarker = false;
	for (const TPair<FString, FString>& E : Rec.Events)
	{
		if (E.Key == TEXT("tool_result") && E.Value.Contains(TEXT("REPEATED_FAILED_EDIT")))
		{
			bSawRepeatMarker = true;
		}
	}
	TestTrue(TEXT("second identical failing edit is refused"), bSawRepeatMarker);
	TestEqual(TEXT("unchanged failure terminates without another model request"), Model.NumCalls(), 2);
	return true;
}

// A successful mutation on another resource does not repair this failed edit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoop_R3b_SuccessClearsFailedSet, "KismetAI.AgentLoop.R3b_SuccessClearsFailedSet", LoopTestFlags)
bool FLoop_R3b_SuccessClearsFailedSet::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> ArgsA = EditArgs(TEXT("/Game/A"), TEXT("PrintString"));
	const TSharedPtr<FJsonObject> ArgsB = InsertArgs(TEXT("/Game/B"), TEXT("Print String"));

	FScriptedModelClient Model({
		MakeResponse({ MakeToolUseBlock(TEXT("t1"), TEXT("edit_blueprint"), ArgsA) }, TEXT("tool_use")),
		MakeResponse({ MakeToolUseBlock(TEXT("t2"), TEXT("insert_node"),    ArgsB) }, TEXT("tool_use")),
		MakeResponse({ MakeToolUseBlock(TEXT("t3"), TEXT("edit_blueprint"), ArgsA) }, TEXT("tool_use")),
		MakeTextTurn(TEXT("done")),
	});
	FScriptedToolInvoker Tools;
	Tools.SetTypedResult(TEXT("edit_blueprint"), FToolExecutionResult::NotStartedError(400, TEXT("PIN_NOT_FOUND"), TEXT("no such pin")));
	Tools.SetResult(TEXT("insert_node"), TEXT("{\"success\":true}"));
	FEventRecorder Rec;

	FKismetAIAgent Agent(Tools, Model, MakeTestConfig());
	Agent.RunTask(TEXT("edit, fix, edit"), Rec.MakeCallback());

	// The first operation ran; an unrelated success cannot authorize its repeat.
	TestEqual(TEXT("unrelated success does not replay failed operation"), Tools.CountCalls(TEXT("edit_blueprint")), 1);
	TestEqual(TEXT("repeat stops before another model request"), Model.NumCalls(), 3);
	return true;
}

// R4: three tool turns with no successful mutation appends a NO PROGRESS note.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoop_R4_NoProgressNudge, "KismetAI.AgentLoop.R4_NoProgressNudge", LoopTestFlags)
bool FLoop_R4_NoProgressNudge::RunTest(const FString& Parameters)
{
	// Distinct args each turn so R3's duplicate guard doesn't fire instead.
	auto ReadTurn = [](const TCHAR* Id, const TCHAR* Path)
	{
		return MakeResponse({ MakeToolUseBlock(Id, TEXT("read_blueprint"), BlueprintArgs(Path)) }, TEXT("tool_use"));
	};

	FScriptedModelClient Model({
		ReadTurn(TEXT("t1"), TEXT("/Game/A")),
		ReadTurn(TEXT("t2"), TEXT("/Game/B")),
		ReadTurn(TEXT("t3"), TEXT("/Game/C")),
		ReadTurn(TEXT("t4"), TEXT("/Game/D")),
		MakeTextTurn(TEXT("done")),
	});
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;

	FKismetAIAgent Agent(Tools, Model, MakeTestConfig(/*MaxSteps=*/6));
	Agent.RunTask(TEXT("investigate forever"), Rec.MakeCallback());

	// The note rides in the tool-result message, so look at what the model saw.
	bool bSawNoProgress = false;
	for (const FCapturedRequest& Req : Model.GetRequests())
	{
		for (const TSharedPtr<FJsonValue>& M : Req.Messages)
		{
			const TSharedPtr<FJsonObject> Obj = M->AsObject();
			if (!Obj.IsValid()) continue;
			const TArray<TSharedPtr<FJsonValue>>* Content;
			if (!Obj->TryGetArrayField(TEXT("content"), Content)) continue;
			for (const TSharedPtr<FJsonValue>& B : *Content)
			{
				const TSharedPtr<FJsonObject> BO = B->AsObject();
				FString Text;
				if (BO.IsValid() && BO->TryGetStringField(TEXT("text"), Text) && Text.Contains(TEXT("NO PROGRESS")))
				{
					bSawNoProgress = true;
				}
			}
		}
	}
	TestTrue(TEXT("NO PROGRESS note reaches the model after 3 barren turns"), bSawNoProgress);
	return true;
}

// R1: the model is warned on what will be its final working turn.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoop_R1_FinalTurnBudgetNote, "KismetAI.AgentLoop.R1_FinalTurnBudgetNote", LoopTestFlags)
bool FLoop_R1_FinalTurnBudgetNote::RunTest(const FString& Parameters)
{
	auto ReadTurn = [](const TCHAR* Id, const TCHAR* Path)
	{
		return MakeResponse({ MakeToolUseBlock(Id, TEXT("read_blueprint"), BlueprintArgs(Path)) }, TEXT("tool_use"));
	};

	FScriptedModelClient Model({ ReadTurn(TEXT("t1"), TEXT("/Game/A")), ReadTurn(TEXT("t2"), TEXT("/Game/B")) });
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;

	// MaxSteps 3: after turn 1, TurnsRemaining == 2 -> the "N working turns
	// remain" note; after turn 2, TurnsRemaining == 1 -> the FINAL TURN note.
	FKismetAIAgent Agent(Tools, Model, MakeTestConfig(/*MaxSteps=*/3));
	Agent.RunTask(TEXT("work"), Rec.MakeCallback());

	bool bSawFinalTurn = false, bSawBudget = false;
	for (const FCapturedRequest& Req : Model.GetRequests())
	{
		for (const TSharedPtr<FJsonValue>& M : Req.Messages)
		{
			const TSharedPtr<FJsonObject> Obj = M->AsObject();
			if (!Obj.IsValid()) continue;
			const TArray<TSharedPtr<FJsonValue>>* Content;
			if (!Obj->TryGetArrayField(TEXT("content"), Content)) continue;
			for (const TSharedPtr<FJsonValue>& B : *Content)
			{
				const TSharedPtr<FJsonObject> BO = B->AsObject();
				FString Text;
				if (!BO.IsValid() || !BO->TryGetStringField(TEXT("text"), Text)) continue;
				if (Text.Contains(TEXT("FINAL TURN NEXT"))) { bSawFinalTurn = true; }
				if (Text.Contains(TEXT("working turns remain"))) { bSawBudget = true; }
			}
		}
	}
	TestTrue(TEXT("R2 budget note appears"), bSawBudget);
	TestTrue(TEXT("R1 final-turn note appears"), bSawFinalTurn);
	return true;
}

// R6: a text-only turn that describes an action it did not take gets one nudge,
// and the run continues rather than ending.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoop_R6_PrematureEndTurnNudge, "KismetAI.AgentLoop.R6_PrematureEndTurnNudge", LoopTestFlags)
bool FLoop_R6_PrematureEndTurnNudge::RunTest(const FString& Parameters)
{
	FScriptedModelClient Model({
		MakeTextTurn(TEXT("I will add a PrintString node to the graph.")),
		MakeTextTurn(TEXT("Actually finished now.")),
	});
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;

	FKismetAIAgent Agent(Tools, Model, MakeTestConfig());
	Agent.RunTask(TEXT("add a node"), Rec.MakeCallback());

	TestEqual(TEXT("the run continued instead of ending on the first turn"), Model.NumCalls(), 2);
	TestEqual(TEXT("only one done event"), Rec.Count(TEXT("done")), 1);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Termination and errors
// ─────────────────────────────────────────────────────────────────────────────

// A fatal provider outcome ends the run as failed and emits a structured error.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoop_FatalErrorEndsRun, "KismetAI.AgentLoop.FatalErrorEndsRun", LoopTestFlags)
bool FLoop_FatalErrorEndsRun::RunTest(const FString& Parameters)
{
	FModelCallOutcome Fatal;
	Fatal.Result = EModelCallResult::FatalError;
	Fatal.HttpStatusCode = 401;
	Fatal.ErrorType = TEXT("authentication_error");
	Fatal.ErrorMessage = TEXT("invalid x-api-key");

	FScriptedModelClient Model({ Fatal });
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;

	FKismetAIAgent Agent(Tools, Model, MakeTestConfig());
	Agent.RunTask(TEXT("anything"), Rec.MakeCallback());

	TestEqual(TEXT("one error event"), Rec.Count(TEXT("error")), 1);
	const FString* Err = Rec.FirstJsonOfType(TEXT("error"));
	TestTrue(TEXT("error carries the wire error_code"),
		Err && Err->Contains(TEXT("\"error_code\":\"authentication_error\"")));
	TestTrue(TEXT("error carries suggestions"), Err && Err->Contains(TEXT("suggestions")));
	TestEqual(TEXT("no tools ran"), Tools.CallLog.Num(), 0);
	return true;
}

// Abort during a run ends it as aborted, not as an error.
//
// Note RunTask clears bAborted at entry, so an abort issued *before* the run is
// deliberately ignored — a stale stop cannot kill the next run. The abort has to
// land mid-run, which is what the OnInvoke hook is for.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoop_AbortEndsRunAsAborted, "KismetAI.AgentLoop.AbortEndsRunAsAborted", LoopTestFlags)
bool FLoop_AbortEndsRunAsAborted::RunTest(const FString& Parameters)
{
	FScriptedModelClient Model({
		MakeResponse({ MakeToolUseBlock(TEXT("t1"), TEXT("read_blueprint"), BlueprintArgs(TEXT("/Game/BP"))) }, TEXT("tool_use")),
		MakeTextTurn(TEXT("should never be reached")),
	});
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;

	FKismetAIAgent Agent(Tools, Model, MakeTestConfig());
	Tools.OnInvoke = [&Agent](const FString&) { Agent.Abort(); };

	Agent.RunTask(TEXT("stop me"), Rec.MakeCallback());

	const FString* Done = Rec.FirstJsonOfType(TEXT("done"));
	TestTrue(TEXT("done reports aborted"), Done && Done->Contains(TEXT("\"status\":\"aborted\"")));
	TestEqual(TEXT("the run stopped after the aborting turn"), Model.NumCalls(), 1);
	TestTrue(TEXT("the model client was cancelled"), Model.WasCancelled());
	return true;
}

// The counterpart. Previously, an abort issued before RunTask was silently
// discarded by RunTask (it replaced the token), which is how a Stop pressed
// between launch and the worker's start was lost. Now that Stop belongs to the
// run that starts next — that run ends "aborted" with no model request — and
// it is consumed there, so it still cannot kill a later run. (The session
// controller never cancels a run whose worker has exited, so a late click
// cannot reach a finished agent.)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoop_AbortBeforeRunIsCleared, "KismetAI.AgentLoop.AbortBeforeRunIsCleared", LoopTestFlags)
bool FLoop_AbortBeforeRunIsCleared::RunTest(const FString& Parameters)
{
	FScriptedModelClient Model({ MakeTextTurn(TEXT("ran anyway")) });
	FScriptedToolInvoker Tools;

	FKismetAIAgent Agent(Tools, Model, MakeTestConfig());
	Agent.Abort();
	FEventRecorder First;
	Agent.RunTask(TEXT("go"), First.MakeCallback());
	const FString* FirstDone = First.FirstJsonOfType(TEXT("done"));
	TestTrue(TEXT("the run the Stop landed before ends aborted"), FirstDone && FirstDone->Contains(TEXT("\"status\":\"aborted\"")));
	TestEqual(TEXT("without a model request"), Model.NumCalls(), 0);

	FEventRecorder Second;
	const FString Result = Agent.RunTask(TEXT("go again"), Second.MakeCallback());
	TestEqual(TEXT("the next run proceeds normally"), Result, FString(TEXT("ran anyway")));
	const FString* Done = Second.FirstJsonOfType(TEXT("done"));
	TestTrue(TEXT("done reports completed"), Done && Done->Contains(TEXT("\"status\":\"completed\"")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Adaptive token budget
// ─────────────────────────────────────────────────────────────────────────────

// After a write_blueprint turn the next budget drops to MaxTokensLow (the next
// turn is expected to be verification); after a read/compile it rises to High.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoop_AdaptiveMaxTokens, "KismetAI.AgentLoop.AdaptiveMaxTokens", LoopTestFlags)
bool FLoop_AdaptiveMaxTokens::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> A = BlueprintArgs(TEXT("/Game/BP"));
	A->SetStringField(TEXT("t3d"), TEXT("Begin Object Class=/Script/BlueprintGraph.K2Node_CallFunction Name=\"N0\"\nEnd Object\n"));

	FScriptedModelClient Model({
		MakeResponse({ MakeToolUseBlock(TEXT("t1"), TEXT("write_blueprint"), A) }, TEXT("tool_use")),
		MakeResponse({ MakeToolUseBlock(TEXT("t2"), TEXT("compile_blueprint"), A) }, TEXT("tool_use")),
		MakeTextTurn(TEXT("done")),
	});
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;

	FKismetAIAgentConfig Config = MakeTestConfig();
	FKismetAIAgent Agent(Tools, Model, Config);
	Agent.RunTask(TEXT("write then compile"), Rec.MakeCallback());

	if (Model.GetRequests().Num() < 3)
	{
		AddError(TEXT("expected three model calls"));
		return false;
	}
	// Call 2 follows write_blueprint -> low; call 3 follows compile_blueprint -> high.
	TestEqual(TEXT("budget drops to Low after write_blueprint"),
		Model.GetRequests()[1].MaxTokens, Config.MaxTokensLow);
	TestEqual(TEXT("budget rises to High after compile_blueprint"),
		Model.GetRequests()[2].MaxTokens, Config.MaxTokensHigh);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Request composition
// ─────────────────────────────────────────────────────────────────────────────

// Every request carries the system prompt and the composed tool array. The core
// tools are always present; extended ones only after discover_tools.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoop_RequestCarriesPromptAndCoreTools, "KismetAI.AgentLoop.RequestCarriesPromptAndCoreTools", LoopTestFlags)
bool FLoop_RequestCarriesPromptAndCoreTools::RunTest(const FString& Parameters)
{
	FScriptedModelClient Model({ MakeTextTurn(TEXT("ok")) });
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;

	FKismetAIAgent Agent(Tools, Model, MakeTestConfig());
	Agent.RunTask(TEXT("hello"), Rec.MakeCallback());

	if (Model.GetRequests().Num() == 0)
	{
		AddError(TEXT("no request captured"));
		return false;
	}
	const FCapturedRequest& Req = Model.GetRequests()[0];
	TestTrue(TEXT("system prompt is non-empty"), Req.SystemPrompt.Len() > 1000);
	TestTrue(TEXT("core tools are advertised"), Req.Tools.Num() >= 10);

	// The last tool must carry cache_control for prompt caching — dropping this
	// turns every turn into a cache miss.
	const TSharedPtr<FJsonObject> LastTool = Req.Tools.Last()->AsObject();
	TestTrue(TEXT("last tool carries cache_control"),
		LastTool.IsValid() && LastTool->HasField(TEXT("cache_control")));

	// The user's message is the first thing the model sees.
	TestTrue(TEXT("messages are non-empty"), Req.Messages.Num() >= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoop_OpaqueContentSurvives, "KismetAI.AgentLoop.OpaqueContentSurvives", LoopTestFlags)
bool FLoop_OpaqueContentSurvives::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Extension = MakeShared<FJsonObject>();
	Extension->SetStringField(TEXT("type"), TEXT("provider_extension"));
	Extension->SetStringField(TEXT("opaque"), TEXT("preserve-this"));
	FScriptedModelClient Model({ MakeResponse({ Extension,
		MakeToolUseBlock(TEXT("opaque-call"), TEXT("compile_blueprint"), BlueprintArgs(TEXT("/Game/BP"))) }, TEXT("tool_use")), MakeTextTurn(TEXT("Done.")) });
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;
	FKismetAIAgent Agent(Tools, Model, MakeTestConfig());
	Agent.RunTask(TEXT("compile"), Rec.MakeCallback());
	bool bFound = false;
	if (Model.GetRequests().Num() > 1)
	{
		for (const auto& Message : Model.GetRequests()[1].Messages)
		{
			const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
			if (!Message->AsObject()->TryGetArrayField(TEXT("content"), Parts)) continue;
			for (const auto& Part : *Parts)
			{
				FString Opaque;
				if (Part->AsObject()->TryGetStringField(TEXT("opaque"), Opaque)) bFound |= Opaque == TEXT("preserve-this");
			}
		}
	}
	TestTrue(TEXT("the real loop retains provider extension content in its next request"), bFound);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoop_TypedOutcomeControlsEvents, "KismetAI.AgentLoop.TypedOutcomeControlsEvents", LoopTestFlags)
bool FLoop_TypedOutcomeControlsEvents::RunTest(const FString& Parameters)
{
	FScriptedModelClient Model({ MakeToolTurn(TEXT("typed-call"), TEXT("compile_blueprint"), BlueprintArgs(TEXT("/Game/BP"))), MakeTextTurn(TEXT("Done.")) });
	FScriptedToolInvoker Tools;
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Example = MakeShared<FJsonObject>();
	Example->SetBoolField(TEXT("success"), false);
	Data->SetObjectField(TEXT("example"), Example);
	Tools.SetTypedResult(TEXT("compile_blueprint"), FToolExecutionResult::Success(Data));
	FEventRecorder Rec;
	FKismetAIAgent Agent(Tools, Model, MakeTestConfig());
	Agent.RunTask(TEXT("compile"), Rec.MakeCallback());
	const FString* Event = Rec.FirstJsonOfType(TEXT("tool_result"));
	TestTrue(TEXT("event carries authoritative success and call identity"), Event && Event->Contains(TEXT("\"is_error\":false")) && Event->Contains(TEXT("typed-call")));
	if (Model.GetRequests().Num() < 2) { AddError(TEXT("missing next request")); return false; }
	const auto Typed = KismetAIAgentCodec::FromAnthropicArray(Model.GetRequests()[1].Messages);
	bool bFound = false;
	for (const auto& Message : Typed) for (const auto& Part : Message.Parts)
	{
		if (Part.Type == EAgentPartType::ToolResult && Part.ToolUseId == TEXT("typed-call"))
		{
			bFound = true;
			TestFalse(TEXT("nested success:false is data, not an error"), Part.bIsError);
		}
	}
	TestTrue(TEXT("typed result paired to call"), bFound);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoop_InvalidTypedArgumentsRefused, "KismetAI.AgentLoop.InvalidTypedArgumentsRefused", LoopTestFlags)
bool FLoop_InvalidTypedArgumentsRefused::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Block = MakeToolUseBlock(TEXT("bad-call"), TEXT("enter_plan_mode"), nullptr);
	Block->SetStringField(TEXT("input"), TEXT("not-an-object"));
	FScriptedModelClient Model({ MakeResponse({ Block }, TEXT("tool_use")), MakeTextTurn(TEXT("Done.")) });
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;
	FKismetAIAgent Agent(Tools, Model, MakeTestConfig());
	Agent.RunTask(TEXT("inspect"), Rec.MakeCallback());
	TestEqual(TEXT("malformed arguments never dispatched"), Tools.CallLog.Num(), 0);
	const FString* Event = Rec.FirstJsonOfType(TEXT("tool_result"));
	TestTrue(TEXT("invalid arguments produce typed error event"), Event && Event->Contains(TEXT("INVALID_ARGUMENTS")) && Event->Contains(TEXT("\"is_error\":true")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoop_OperationFailureControlsEvents, "KismetAI.AgentLoop.OperationFailureControlsEvents", LoopTestFlags)
bool FLoop_OperationFailureControlsEvents::RunTest(const FString& Parameters)
{
	FScriptedModelClient Model({ MakeToolTurn(TEXT("operation-call"), TEXT("compile_blueprint"), BlueprintArgs(TEXT("/Game/BP"))), MakeTextTurn(TEXT("Done.")) });
	FScriptedToolInvoker Tools;
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("detail"), FString::ChrN(6000, TEXT('x')));
	Tools.SetTypedResult(TEXT("compile_blueprint"), FToolExecutionResult::Success(Payload, false));
	FEventRecorder Rec;
	FKismetAIAgent Agent(Tools, Model, MakeTestConfig());
	Agent.RunTask(TEXT("compile"), Rec.MakeCallback());
	const FString* Event = Rec.FirstJsonOfType(TEXT("tool_result"));
	TestTrue(TEXT("operation failure survives a successful transport envelope"), Event && Event->Contains(TEXT("\"is_error\":true")));
	if (Model.GetRequests().Num() < 2) { AddError(TEXT("missing next request")); return false; }
	bool bError = false;
	for (const auto& Message : KismetAIAgentCodec::FromAnthropicArray(Model.GetRequests()[1].Messages))
		for (const auto& Part : Message.Parts) if (Part.ToolUseId == TEXT("operation-call") && Part.Type == EAgentPartType::ToolResult)
		{
			bError = Part.bIsError;
			TestFalse(TEXT("transport-success error payload keeps its unwrapped presentation"), Part.ToolResultContent.Contains(TEXT("</tool_use_error>")));
		}
	TestTrue(TEXT("model result agrees with event failure"), bError);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoop_StateChangeRetry, "KismetAI.AgentLoop.StateChangePermitsRetry", LoopTestFlags)
bool FLoop_StateChangeRetry::RunTest(const FString& Parameters)
{
	const auto Args = EditArgs(TEXT("/Game/A"), TEXT("PrintString"));
	FScriptedModelClient Model({
		MakeToolTurn(TEXT("e1"), TEXT("edit_blueprint"), Args),
		MakeToolTurn(TEXT("r"), TEXT("read_blueprint"), BlueprintArgs(TEXT("/Game/A"))),
		MakeToolTurn(TEXT("e2"), TEXT("edit_blueprint"), Args),
		MakeTextTurn(TEXT("Stopped.")) });
	FScriptedToolInvoker Tools;
	Tools.SetTypedResult(TEXT("edit_blueprint"), FToolExecutionResult::NotStartedError(400, TEXT("PIN_NOT_FOUND"), TEXT("no pin")));
	auto Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_revision"), TEXT("changed"));
	Data->SetStringField(TEXT("asset_revision_path"), TEXT("/Game/A.A"));
	Tools.SetTypedResult(TEXT("read_blueprint"), FToolExecutionResult::Success(Data));
	FKismetAIAgent Agent(Tools, Model, MakeTestConfig());
	Agent.RunTask(TEXT("Read repaired state and retry."), FOnAgentStep());
	TestEqual(TEXT("new observed state permits a fresh operation"), Tools.CountCalls(TEXT("edit_blueprint")), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoop_RevisionReadDedupes, "KismetAI.AgentLoop.RevisionReadDedupesImmediately", LoopTestFlags)
bool FLoop_RevisionReadDedupes::RunTest(const FString& Parameters)
{
	const auto Args = BlueprintArgs(TEXT("/Game/A"));
	FScriptedModelClient Model({ MakeToolTurn(TEXT("r1"), TEXT("read_blueprint"), Args),
		MakeToolTurn(TEXT("r2"), TEXT("read_blueprint"), Args), MakeTextTurn(TEXT("Read.")) });
	FScriptedToolInvoker Tools;
	auto Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_revision"), TEXT("revision"));
	Data->SetStringField(TEXT("asset_revision_path"), TEXT("/Game/A.A"));
	Tools.SetTypedResult(TEXT("read_blueprint"), FToolExecutionResult::Success(Data));
	FKismetAIAgent Agent(Tools, Model, MakeTestConfig());
	Agent.RunTask(TEXT("Read twice."), FOnAgentStep());
	TestEqual(TEXT("record observation after successful result"), Tools.CountCalls(TEXT("read_blueprint")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoop_PartialWriteBarrier, "KismetAI.AgentLoop.PartialWriteBarrierSurvivesResume", LoopTestFlags)
bool FLoop_PartialWriteBarrier::RunTest(const FString& Parameters)
{
	const auto Args = InsertArgs(TEXT("/Game/A"), TEXT("Print String"));
	FScriptedModelClient Model({ MakeToolTurn(TEXT("i1"), TEXT("insert_node"), Args), MakeTextTurn(TEXT("Save failed.")) });
	FScriptedToolInvoker Tools;
	FKismetAIMutationOutcome Applied;
	Applied.PackageName = TEXT("/Game/A");
	Applied.bAppliedInMemory = true;
	Applied.CompileStatus = EKismetAICompileStatus::Succeeded;
	Applied.SaveStatus = EKismetAISaveStatus::Failed;
	Applied.SaveError = TEXT("fixture write denied");
	auto Data = MakeShared<FJsonObject>();
	Data->SetObjectField(TEXT("outcome"), Applied.ToJson());
	Tools.SetTypedResult(TEXT("insert_node"), FToolExecutionResult::Success(Data));
	FKismetAIAgent First(Tools, Model, MakeTestConfig());
	First.RunTask(TEXT("Insert."), FOnAgentStep());
	FScriptedModelClient ResumeModel({ MakeToolTurn(TEXT("i2"), TEXT("insert_node"), Args), MakeTextTurn(TEXT("Done.")) });
	FScriptedToolInvoker ResumeTools;
	FKismetAIAgent Resume(ResumeTools, ResumeModel, MakeTestConfig());
	FEventRecorder Events;
	Resume.RunTask(TEXT("Continue."), Events.MakeCallback(), TEXT(""), First.GetSessionId());
	TestEqual(TEXT("already-applied write is not repeated"), ResumeTools.CountCalls(TEXT("insert_node")), 0);
	TestEqual(TEXT("reconciliation stop does not spend another request"), ResumeModel.NumCalls(), 1);
	return true;
}
