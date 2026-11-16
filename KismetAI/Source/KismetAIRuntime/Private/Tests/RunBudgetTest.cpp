// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
#include "Fakes/ScriptedAgentSeams.h"
#include "KismetAISessionStorage.h"
#include "KismetAIMutationOutcome.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
using namespace KismetAITestFakes;
constexpr auto RunBudgetFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
namespace
{
	FKismetAIAgentConfig BudgetConfig(int32 Models = 0, int32 Tools = 0, int64 Bytes = 0, double Spend = 0)
	{
		FKismetAIAgentConfig C; C.Model = TEXT("claude-sonnet-4-6"); C.ThinkingMode = EThinkingMode::Disabled;
		C.MaxSteps = 4; C.MaxRetries = 1; C.RetryBaseDelayMs = C.RetryMaxDelayMs = C.RetryJitterFactor = 0;
		C.RunLimits.MaxModelRequests = Models; C.RunLimits.MaxToolCalls = Tools;
		C.RunLimits.MaxOutputBytes = Bytes; C.RunLimits.MaxEstimatedSpendUSD = Spend; return C;
	}
	TSharedPtr<FJsonObject> BudgetArgs(const TCHAR* Path)
	{ auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("blueprint"), Path); return A; }
	const FString* BudgetTerminal(const FEventRecorder& Events)
	{ const FString* Done = Events.FirstJsonOfType(TEXT("done")); return Done ? Done : Events.FirstJsonOfType(TEXT("error")); }
	class FRetryBudgetModel final : public IKismetAIModelClient
	{
	public:
		int32 Attempts = 0;
		virtual FModelCallOutcome Call(const FModelCallRequest&, const FModelCallbacks&) override
		{ ++Attempts; FModelCallOutcome O; O.Result = EModelCallResult::RetryableError; O.HttpStatusCode = 503; O.RequestCount = 1; O.UsageReports.Add(MakeUsage(1000000, 1000000)); return O; }
		virtual FModelCallOutcome CallWithRetry(const FModelCallRequest& R, const FModelCallbacks& C) override
		{ return FKismetAICurlModelClient::RunRetryLoop(R, C, [&](const FModelCallRequest& A) { return Call(A, C); }, [] { return false; }); }
		virtual void Cancel() override {}
		virtual FString GetProviderName() const override { return TEXT("anthropic"); }
		virtual int32 CacheTtlSeconds() const override { return 0; }
	};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunBudget_Requests, "KismetAI.RunBudget.ModelBoundary", RunBudgetFlags)
bool FRunBudget_Requests::RunTest(const FString& Parameters)
{
	FScriptedModelClient Model({MakeToolTurn(TEXT("a"), TEXT("read_blueprint"), BudgetArgs(TEXT("/Game/A"))), MakeTextTurn(TEXT("done"))});
	FScriptedToolInvoker Tools; FEventRecorder Events; FKismetAIAgent Agent(Tools, Model, BudgetConfig(1));
	Agent.RunTask(TEXT("inspect"), Events.MakeCallback());
	TestEqual(TEXT("no second model request after limit"), Model.NumCalls(), 1);
	const FString* End = BudgetTerminal(Events);
	TestTrue(TEXT("terminal names exhausted budget"), End && End->Contains(TEXT("run_budget_exhausted")));
	TestTrue(TEXT("terminal records run limits"), End && End->Contains(TEXT("run_budget"))); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunBudget_ToolBatch, "KismetAI.RunBudget.ToolBatchPersistence", RunBudgetFlags)
bool FRunBudget_ToolBatch::RunTest(const FString& Parameters)
{
	FScriptedModelClient Model({MakeResponse({MakeToolUseBlock(TEXT("a"), TEXT("read_blueprint"), BudgetArgs(TEXT("/Game/A"))), MakeToolUseBlock(TEXT("b"), TEXT("read_blueprint"), BudgetArgs(TEXT("/Game/B")))}, TEXT("tool_use")), MakeTextTurn(TEXT("done"))});
	FScriptedToolInvoker Tools; FEventRecorder Events; FKismetAIAgent Agent(Tools, Model, BudgetConfig(0, 1));
	Agent.RunTask(TEXT("inspect both"), Events.MakeCallback());
	TestEqual(TEXT("only admitted first tool executes"), Tools.CallLog.Num(), 1);
	TestEqual(TEXT("no model request after tool budget exhaustion"), Model.NumCalls(), 1);
	const FString* End = BudgetTerminal(Events);
	TestTrue(TEXT("budget terminal remains durably committed"), End && End->Contains(TEXT("\"persistence_complete\":true")));
	TestTrue(TEXT("remaining call is not falsely completed"), End && End->Contains(TEXT("partial")));
	const auto Snapshot = FKismetAISessionStorage::LoadSession(Agent.GetSessionId());
	if (TestEqual(TEXT("two durable call dispositions"), Snapshot.CommittedCalls.Num(), 2))
	{
		TestEqual(TEXT("first call succeeded in journal"), Snapshot.CommittedCalls[0]->GetStringField(TEXT("status")), FString(TEXT("succeeded")));
		TestEqual(TEXT("second call not started in journal"), Snapshot.CommittedCalls[1]->GetStringField(TEXT("status")), FString(TEXT("not_started")));
	}
	TestTrue(TEXT("restored history retains paired results"), FKismetAISessionStorage::LoadMessages(Agent.GetSessionId()).Num() >= 3);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunBudget_FailedSpend, "KismetAI.RunBudget.FailedAttemptSpend", RunBudgetFlags)
bool FRunBudget_FailedSpend::RunTest(const FString& Parameters)
{
	FRetryBudgetModel Model; FScriptedToolInvoker Tools; FEventRecorder Events; FKismetAIAgent Agent(Tools, Model, BudgetConfig(0, 0, 0, 0.001));
	Agent.RunTask(TEXT("inspect"), Events.MakeCallback());
	TestEqual(TEXT("failed billed attempt exhausts estimate before retry"), Model.Attempts, 1);
	TestEqual(TEXT("failed attempt usage recorded once"), Agent.GetTokenStats().TotalInputTokens, 1000000);
	const FString* End = BudgetTerminal(Events);
	TestTrue(TEXT("estimated spending provenance is explicit"), End && End->Contains(TEXT("estimated"))); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunBudget_Output, "KismetAI.RunBudget.OutputBoundary", RunBudgetFlags)
bool FRunBudget_Output::RunTest(const FString& Parameters)
{
	FScriptedModelClient Model({MakeToolTurn(TEXT("a"), TEXT("read_blueprint"), BudgetArgs(TEXT("/Game/A"))), MakeTextTurn(TEXT("done"))});
	FScriptedToolInvoker Tools; FEventRecorder Events; FKismetAIAgent Agent(Tools, Model, BudgetConfig(0, 0, 1));
	Agent.RunTask(TEXT("inspect"), Events.MakeCallback());
	TestEqual(TEXT("output exhaustion prevents tools from model response"), Tools.CallLog.Num(), 0);
	TestEqual(TEXT("output exhaustion prevents another model request"), Model.NumCalls(), 1); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunBudget_UnknownCost, "KismetAI.RunBudget.UnknownCostFailsClosed", RunBudgetFlags)
bool FRunBudget_UnknownCost::RunTest(const FString& Parameters)
{
	for (const TCHAR* ModelName : {TEXT("unknown-local-model"), TEXT("gpt-6-unknown-budget-model")})
	{
		auto Config = BudgetConfig(0, 0, 0, 1); Config.Model = ModelName;
		FScriptedModelClient Model({MakeTextTurn(TEXT("done"))}); FScriptedToolInvoker Tools; FEventRecorder Events;
		FKismetAIAgent Agent(Tools, Model, Config); Agent.RunTask(TEXT("answer"), Events.MakeCallback());
		TestEqual(TEXT("unknown price never uses generic family fallback to admit spending"), Model.NumCalls(), 0);
		const FString* End = BudgetTerminal(Events);
		TestTrue(TEXT("unavailable estimate visible"), End && End->Contains(TEXT("run_budget_unavailable")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunBudget_Elapsed, "KismetAI.RunBudget.ElapsedAdmission", RunBudgetFlags)
bool FRunBudget_Elapsed::RunTest(const FString& Parameters)
{
	auto Config = BudgetConfig(); Config.RunLimits.MaxElapsedSeconds = 1;
	FKismetAIRunBudget Budget(Config, TEXT("anthropic"), FPlatformTime::Seconds() - 2);
	TestFalse(TEXT("admission elapsed limit refuses model"), Budget.StartModel().IsEmpty());
	TestFalse(TEXT("admission elapsed limit refuses tool"), Budget.StartTool().IsEmpty());
	TestEqual(TEXT("refusal does not consume attempts"), Budget.GetModelRequests(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunBudget_Reset, "KismetAI.RunBudget.ImmutableAndFreshContinuation", RunBudgetFlags)
bool FRunBudget_Reset::RunTest(const FString& Parameters)
{
	auto Config = BudgetConfig(1);
	FScriptedModelClient Model({MakeToolTurn(TEXT("a"), TEXT("read_blueprint"), BudgetArgs(TEXT("/Game/A"))), MakeTextTurn(TEXT("answer"))});
	FScriptedToolInvoker Tools; FEventRecorder First, Second; FKismetAIAgent Agent(Tools, Model, Config);
	Config.RunLimits.MaxModelRequests = 10;
	Agent.PrepareRunInputs(); Agent.RunTask(TEXT("inspect"), First.MakeCallback());
	TestEqual(TEXT("admitted copy retains original limit"), Model.NumCalls(), 1);
	const FString Session = Agent.GetSessionId();
	Agent.RunTask(TEXT("continue with an answer"), Second.MakeCallback(), FString(), Session);
	TestEqual(TEXT("user continuation gets fresh model allowance"), Model.NumCalls(), 2);
	const FString* End = BudgetTerminal(Second);
	TestTrue(TEXT("natural final text at exact request cap can complete"), End && End->Contains(TEXT("\"status\":\"completed\"")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunBudget_Mutation, "KismetAI.RunBudget.MutationOutcomeSurvivesExhaustion", RunBudgetFlags)
bool FRunBudget_Mutation::RunTest(const FString& Parameters)
{
	auto Edit = BudgetArgs(TEXT("/Game/A")); auto Op = MakeShared<FJsonObject>();
	Op->SetStringField(TEXT("action"), TEXT("set_pin_default")); Op->SetStringField(TEXT("node"), TEXT("Node"));
	Op->SetStringField(TEXT("pin"), TEXT("Value")); Op->SetStringField(TEXT("value"), TEXT("changed"));
	Edit->SetArrayField(TEXT("operations"), {MakeShared<FJsonValueObject>(Op)});
	FKismetAIMutationOutcome Applied; Applied.PackageName = TEXT("/Game/A"); Applied.bAppliedInMemory = true;
	Applied.SaveStatus = EKismetAISaveStatus::Saved; Applied.CompileStatus = EKismetAICompileStatus::NotRun;
	auto Data = MakeShared<FJsonObject>(); Data->SetObjectField(TEXT("outcome"), Applied.ToJson());
	FScriptedToolInvoker Tools; Tools.SetTypedResult(TEXT("edit_blueprint"), FToolExecutionResult::Success(Data));
	FScriptedModelClient Model({MakeResponse({MakeToolUseBlock(TEXT("a"), TEXT("edit_blueprint"), Edit),
		MakeToolUseBlock(TEXT("b"), TEXT("compile_blueprint"), BudgetArgs(TEXT("/Game/A")))}, TEXT("tool_use"))});
	FEventRecorder Events; FKismetAIAgent Agent(Tools, Model, BudgetConfig(0, 1)); Agent.RunTask(TEXT("edit then verify"), Events.MakeCallback());
	const auto Snapshot = FKismetAISessionStorage::LoadSession(Agent.GetSessionId());
	if (TestEqual(TEXT("both tool dispositions committed"), Snapshot.CommittedCalls.Num(), 2))
	{
		TestEqual(TEXT("executed mutation retained"), Snapshot.CommittedCalls[0]->GetStringField(TEXT("status")), FString(TEXT("succeeded")));
		TestTrue(TEXT("actual typed mutation outcome durable"), Snapshot.CommittedCalls[0]->HasField(TEXT("mutation_outcome")));
		TestEqual(TEXT("compile honestly not started"), Snapshot.CommittedCalls[1]->GetStringField(TEXT("status")), FString(TEXT("not_started")));
	}
	const FString* End = BudgetTerminal(Events);
	TestTrue(TEXT("compile obligation remains partial"), End && End->Contains(TEXT("partial")) && End->Contains(TEXT("/Game/A")));
	if (End) AddInfo(TEXT("BUDGET_MUTATION_TERMINAL ") + *End);
	TestEqual(TEXT("no edit replay or extra compile"), Tools.CallLog.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunBudget_Summary, "KismetAI.RunBudget.SummarySharesAttemptLimit", RunBudgetFlags)
bool FRunBudget_Summary::RunTest(const FString& Parameters)
{
	const FString Session = FKismetAISessionStorage::CreateSessionId();
	auto Header = MakeShared<FJsonObject>(); Header->SetStringField(TEXT("type"), TEXT("session_start")); Header->SetNumberField(TEXT("v"), 1);
	FKismetAISessionStorage::AppendEntry(Session, Header);
	for (int32 I = 0; I < 16; ++I)
	{
		auto Entry = MakeShared<FJsonObject>(); Entry->SetStringField(TEXT("uuid"), FGuid::NewGuid().ToString());
		Entry->SetStringField(TEXT("type"), I % 2 ? TEXT("assistant_message") : TEXT("user_message"));
		if (I % 2) Entry->SetArrayField(TEXT("content"), {MakeShared<FJsonValueObject>(MakeTextBlock(TEXT("Previous factual answer.")))});
		else Entry->SetStringField(TEXT("content"), TEXT("Previous factual question."));
		FKismetAISessionStorage::AppendEntry(Session, Entry);
	}
	FModelCallOutcome TooLong; TooLong.Result = EModelCallResult::PromptTooLong; TooLong.HttpStatusCode = 400;
	FScriptedModelClient Model({TooLong, MakeTextTurn(TEXT("<summary>Previous question and answer history.</summary>")), MakeTextTurn(TEXT("done"))});
	FScriptedToolInvoker Tools; FEventRecorder Events; FKismetAIAgent Agent(Tools, Model, BudgetConfig(2));
	Agent.RunTask(TEXT("continue"), Events.MakeCallback(), FString(), Session);
	TestEqual(TEXT("failed primary plus summary exhaust shared request allowance"), Model.NumCalls(), 2);
	if (Model.NumCalls() >= 2) TestTrue(TEXT("second call is summarizer"), Model.GetRequests()[1].Tools.IsEmpty());
	const FString* End = BudgetTerminal(Events);
	TestTrue(TEXT("budget prevents post-summary request"), End && End->Contains(TEXT("run_budget_exhausted")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunBudget_Loopback, "KismetAI.RunBudget.Loopback", RunBudgetFlags)
bool FRunBudget_Loopback::RunTest(const FString& Parameters)
{
	const FString Url = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMETAI_TEST_BUDGET_PROVIDER_URL"));
	if (Url.IsEmpty()) { AddInfo(TEXT("Local transport probe requires KISMETAI_TEST_BUDGET_PROVIDER_URL; deterministic budget scenarios ran separately.")); return true; }
	if (!TestTrue(TEXT("local-only fixture"), Url.StartsWith(TEXT("http://127.0.0.1:")))) return false;
	auto Config = BudgetConfig(0, 0, 0, 0.001); Config.ApiBaseUrl = Url; Config.ApiKey = TEXT("local-budget-fixture");
	Config.ProviderId = TEXT("anthropic"); Config.ProviderBaseUrls.Add(TEXT("anthropic"), Url); Config.bProviderEnvironmentResolved = true;
	FKismetAICurlModelClient Model(Config); FScriptedToolInvoker Tools; FEventRecorder Events; FKismetAIAgent Agent(Tools, Model, Config);
	Agent.RunTask(TEXT("Local budget transport probe."), Events.MakeCallback());
	TestEqual(TEXT("failed response reported input retained"), Agent.GetTokenStats().TotalInputTokens, 1000000);
	TestEqual(TEXT("budget prevents second transport attempt"), Agent.GetTokenStats().RequestCount, 1);
	const FString* End = BudgetTerminal(Events);
	TestTrue(TEXT("local failure ends honestly at spend limit"), End && End->Contains(TEXT("run_budget_exhausted")) && End->Contains(TEXT("\"persistence_complete\":true")));
	if (End) AddInfo(TEXT("BUDGET_LOOPBACK_TERMINAL ") + *End);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunBudget_UnknownUsage, "KismetAI.RunBudget.UnknownUsageIsNotFree", RunBudgetFlags)
bool FRunBudget_UnknownUsage::RunTest(const FString& Parameters)
{
	FModelCallOutcome Missing = MakeTextTurn(TEXT("answer")); Missing.Response->RemoveField(TEXT("usage"));
	FScriptedModelClient Model({Missing}); FScriptedToolInvoker Tools; FEventRecorder Events;
	FKismetAIAgent Agent(Tools, Model, BudgetConfig()); Agent.RunTask(TEXT("answer"), Events.MakeCallback());
	const FString* Usage = Events.FirstJsonOfType(TEXT("token_stats"));
	TestTrue(TEXT("live cost basis unavailable without token evidence"), Usage && Usage->Contains(TEXT("\"cost_basis\":\"unavailable\"")));
	TestFalse(TEXT("missing usage is incomplete"), Agent.GetTokenStats().bUsageComplete);
	FModelCallOutcome MissingTool = MakeToolTurn(TEXT("a"), TEXT("read_blueprint"), BudgetArgs(TEXT("/Game/A")));
	MissingTool.Response->RemoveField(TEXT("usage"));
	FScriptedModelClient CappedModel({MissingTool, MakeTextTurn(TEXT("done"))}); FEventRecorder CappedEvents;
	FKismetAIAgent Capped(Tools, CappedModel, BudgetConfig(0, 0, 0, 100)); Capped.RunTask(TEXT("inspect"), CappedEvents.MakeCallback());
	TestEqual(TEXT("unavailable failed/partial accounting stops further actions under spend cap"), Tools.CallLog.Num(), 0);
	const FString* End = BudgetTerminal(CappedEvents);
	TestTrue(TEXT("unknown cost is explicit"), End && End->Contains(TEXT("run_budget_unavailable")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunBudget_NoAttempt, "KismetAI.RunBudget.CancelBeforeAttemptHasNoCharge", RunBudgetFlags)
bool FRunBudget_NoAttempt::RunTest(const FString& Parameters)
{
	class FCancelledClient final : public IKismetAIModelClient
	{
	public:
		virtual FModelCallOutcome Call(const FModelCallRequest&, const FModelCallbacks&) override { return FModelCallOutcome(); }
		virtual FModelCallOutcome CallWithRetry(const FModelCallRequest& R, const FModelCallbacks& C) override
		{ return FKismetAICurlModelClient::RunRetryLoop(R, C, [](const FModelCallRequest&) { return FModelCallOutcome(); }, [] { return true; }); }
		virtual void Cancel() override {}
		virtual FString GetProviderName() const override { return TEXT("anthropic"); }
		virtual int32 CacheTtlSeconds() const override { return 0; }
	} Model;
	FScriptedToolInvoker Tools; FEventRecorder Events; FKismetAIAgent Agent(Tools, Model, BudgetConfig(1));
	Agent.RunTask(TEXT("answer"), Events.MakeCallback());
	const FString* End = BudgetTerminal(Events);
	TestTrue(TEXT("no fictional invoked attempt"), End && End->Contains(TEXT("\"model_requests\":0")));
	TestEqual(TEXT("no provider request charged"), Agent.GetTokenStats().RequestCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunBudget_UnstartedPersistence, "KismetAI.RunBudget.NotStartedPersistenceFailure", RunBudgetFlags)
bool FRunBudget_UnstartedPersistence::RunTest(const FString& Parameters)
{
	FScriptedModelClient Model({MakeResponse({MakeToolUseBlock(TEXT("a"), TEXT("read_blueprint"), BudgetArgs(TEXT("/Game/A"))),
		MakeToolUseBlock(TEXT("b"), TEXT("read_blueprint"), BudgetArgs(TEXT("/Game/B")))}, TEXT("tool_use"))});
	FScriptedToolInvoker Tools; FEventRecorder Events; FKismetAIAgent Agent(Tools, Model, BudgetConfig(0, 1));
	Agent.SetJournalAppendHookForTests([](const auto& Entry)
	{
		FString Status; Entry->TryGetStringField(TEXT("status"), Status); return Status != TEXT("not_started");
	});
	Agent.RunTask(TEXT("inspect both"), Events.MakeCallback());
	const FString* End = BudgetTerminal(Events);
	TestTrue(TEXT("required missing record is a persistence failure"), End && End->Contains(TEXT("session_journal_write_failed")));
	TestTrue(TEXT("terminal cannot claim complete persistence"), End && End->Contains(TEXT("\"persistence_complete\":false")));
	TestEqual(TEXT("second tool never runs"), Tools.CallLog.Num(), 1);
	bool bFirstCommitted = false;
	for (const auto& Entry : FKismetAISessionStorage::LoadAllEntries(Agent.GetSessionId()))
	{
		FString Status, Id; Entry->TryGetStringField(TEXT("status"), Status); Entry->TryGetStringField(TEXT("tool_use_id"), Id);
		bFirstCommitted |= Status == TEXT("succeeded") && Id == TEXT("a");
	}
	TestTrue(TEXT("real first outcome survives later persistence failure"), bFirstCommitted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunBudget_Parsing, "KismetAI.RunBudget.SettingsNormalizationAndUTF8", RunBudgetFlags)
bool FRunBudget_Parsing::RunTest(const FString& Parameters)
{
	FKismetAIRunLimits Limits; Limits.MaxToolCalls = 4; FString Error;
	FJsonObject Parent; auto Override = MakeShared<FJsonObject>(); Override->SetNumberField(TEXT("max_model_requests"), 2);
	Parent.SetObjectField(TEXT("run_budget"), Override);
	TestTrue(TEXT("valid partial override"), FKismetAIRunLimits::ReadSettings(Parent, Limits, Error));
	TestEqual(TEXT("omitted limit inherited"), Limits.MaxToolCalls, 4);
	Override->SetNumberField(TEXT("max_tool_calls"), -1);
	TestFalse(TEXT("invalid override rejected"), FKismetAIRunLimits::ReadSettings(Parent, Limits, Error));
	TestEqual(TEXT("invalid override transactional"), Limits.MaxToolCalls, 4);
	Override->SetNumberField(TEXT("max_tool_calls"), 0);
	TestTrue(TEXT("zero disables explicitly"), FKismetAIRunLimits::ReadSettings(Parent, Limits, Error));
	TestEqual(TEXT("zero persisted"), Limits.MaxToolCalls, 0);
	auto Config = BudgetConfig(0, 0, 2); FKismetAIRunBudget Budget(Config, TEXT("anthropic"), FPlatformTime::Seconds());
	Budget.ObserveToolOutput(TEXT("я"));
	TestEqual(TEXT("UTF8 byte accounting, not TCHAR count"), Budget.ToJson()->GetIntegerField(TEXT("output_bytes")), 2);
	TestFalse(TEXT("exact byte cap stops next action"), Budget.CheckTool().IsEmpty());
	return true;
}
