// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIAgent.h"
#include "KismetAIDiagnostics.h"
#include "Async/Async.h"
#include "KismetAIStreamEvents.h"
#include "KismetAIMutationOutcome.h"
#include "KismetAISessionStorage.h"
#include "Fakes/ScriptedAgentSeams.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#if PLATFORM_MAC || PLATFORM_LINUX
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace KismetAIDiagnosticBoundaryTest
{
	using namespace KismetAITestFakes;
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	struct FEnvironment
	{
		const FString Root = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("KismetDiagnosticsTest"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
		TMap<FString, FString> Previous;
		FEnvironment()
		{
			Set(TEXT("KISMET_DIAGNOSTICS_ROOT"), Root);
			Set(TEXT("KISMET_DIAGNOSTICS_DISABLE"), TEXT("0"));
			Set(TEXT("KISMET_DIAGNOSTICS_RAW_PROMPTS"), TEXT("0"));
		}
		~FEnvironment()
		{
			for (const auto& Pair : Previous) FPlatformMisc::SetEnvironmentVar(*Pair.Key, *Pair.Value);
			IFileManager::Get().DeleteDirectory(*Root, false, true);
		}
		void Set(const FString& Name, const FString& Value)
		{
			if (!Previous.Contains(Name)) Previous.Add(Name, FPlatformMisc::GetEnvironmentVariable(*Name));
			FPlatformMisc::SetEnvironmentVar(*Name, *Value);
		}
		FString Read(const TCHAR* Kind) const
		{
			FString Contents;
			FFileHelper::LoadFileToString(Contents, *(Root / FString::Printf(TEXT("%s-%u.jsonl"), Kind, FPlatformProcess::GetCurrentProcessId())));
			return Contents;
		}
	};

	FKismetAIAgentConfig DiagnosticConfig()
	{
		FKismetAIAgentConfig Result;
		Result.ApiKey = TEXT("diagnostic-fixture-key-unused");
		Result.Model = TEXT("claude-sonnet-4-6");
		Result.MaxSteps = 4;
		Result.MaxRetries = 0;
		Result.ThinkingMode = EThinkingMode::Disabled;
		return Result;
	}

	TSharedPtr<FJsonObject> MutationArgs()
	{
		auto Op = MakeShared<FJsonObject>();
		Op->SetStringField(TEXT("action"), TEXT("set_pin_default"));
		Op->SetStringField(TEXT("node"), TEXT("Node"));
		Op->SetStringField(TEXT("pin"), TEXT("InString"));
		Op->SetStringField(TEXT("value"), TEXT("DIAGNOSTIC_PRIVATE_ASSET_CONTENT"));
		auto Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("blueprint"), TEXT("/Game/DIAGNOSTIC_PRIVATE_ASSET"));
		Args->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(Op) });
		return Args;
	}

	FToolExecutionResult FailedSave()
	{
		FKismetAIMutationOutcome Outcome;
		Outcome.bAppliedInMemory = true;
		Outcome.CompileStatus = EKismetAICompileStatus::Succeeded;
		Outcome.SaveStatus = EKismetAISaveStatus::Failed;
		Outcome.SaveError = TEXT("Authorization: Bearer DIAGNOSTIC_SAVE_SECRET /private/tmp/DIAGNOSTIC_PRIVATE_PATH");
		Outcome.PackageName = TEXT("/Game/DIAGNOSTIC_PRIVATE_ASSET");
		auto Data = MakeShared<FJsonObject>();
		Data->SetObjectField(TEXT("outcome"), Outcome.ToJson());
		return FToolExecutionResult::Success(Data, false);
	}

	TArray<TSharedPtr<FJsonObject>> Parse(const FString& Contents)
	{
		TArray<TSharedPtr<FJsonObject>> Entries;
		TArray<FString> Lines;
		Contents.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			TSharedPtr<FJsonObject> Entry;
			if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Line), Entry) && Entry.IsValid()) Entries.Add(Entry);
		}
		return Entries;
	}

	TSharedPtr<FJsonObject> CallRecord(const FString& Session)
	{
		for (const auto& Entry : FKismetAISessionStorage::LoadAllEntries(Session))
		{
			FString Record;
			if (Entry->TryGetStringField(TEXT("record"), Record) && Record == TEXT("tool_call")) return Entry;
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDiagnostics_ReconstructFailures,
	"KismetAI.Diagnostics.ReconstructFailures", KismetAIDiagnosticBoundaryTest::Flags)
bool FDiagnostics_ReconstructFailures::RunTest(const FString& Parameters)
{
	using namespace KismetAIDiagnosticBoundaryTest;
	FEnvironment Environment;
	FModelCallOutcome Failure;
	Failure.Result = EModelCallResult::FatalError;
	Failure.ErrorType = TEXT("authentication_error");
	Failure.ErrorMessage = TEXT("https://user:DIAGNOSTIC_URL_SECRET@host/v1?key=DIAGNOSTIC_QUERY_SECRET");
	Failure.HttpStatusCode = 401;
	FScriptedModelClient Client({ MakeToolTurn(TEXT("save-failure"), TEXT("edit_blueprint"), MutationArgs()), Failure });
	FScriptedToolInvoker Invoker;
	Invoker.SetTypedResult(TEXT("edit_blueprint"), FailedSave());
	FKismetAIAgent Agent(Invoker, Client, DiagnosticConfig());
	FEventRecorder Events;
	Agent.RunTask(TEXT("DIAGNOSTIC_PRIVATE_PROMPT"), Events.MakeCallback());
	TestEqual(TEXT("one actual tool invocation"), Invoker.CountCalls(TEXT("edit_blueprint")), 1);
	TestEqual(TEXT("provider continuation reaches forced failure"), Client.NumCalls(), 2);
	const auto JournalCall = CallRecord(Agent.GetSessionId());
	if (!TestTrue(TEXT("authoritative call exists"), JournalCall.IsValid())) return false;
	TestEqual(TEXT("tool failure is authoritative"), JournalCall->GetStringField(TEXT("status")), FString(TEXT("failed")));
	TestEqual(TEXT("save failure persists"), JournalCall->GetObjectField(TEXT("mutation_outcome"))->GetStringField(TEXT("save_status")), FString(TEXT("failed")));
	FString Journal;
	FFileHelper::LoadFileToString(Journal, *FKismetAISessionStorage::GetSessionFilePath(Agent.GetSessionId()));
	TestTrue(TEXT("journal retains recovery content"), Journal.Contains(TEXT("DIAGNOSTIC_PRIVATE_PROMPT")) && Journal.Contains(TEXT("DIAGNOSTIC_SAVE_SECRET")));
	const FString Safe = Environment.Read(TEXT("safe"));
	// Only this synthetic fixture can opt into retaining paired diagnostic and
	// recovery evidence after its disposable test directory is removed.
	const FString EvidenceRoot = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMET_DIAGNOSTIC_TEST_EVIDENCE_ROOT"));
	if (!EvidenceRoot.IsEmpty())
	{
		TestTrue(TEXT("fixture evidence directory created"), IFileManager::Get().MakeDirectory(*EvidenceRoot, true));
		TestTrue(TEXT("safe fixture evidence retained"), FFileHelper::SaveStringToFile(Safe, *(EvidenceRoot / TEXT("safe.jsonl")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
		TestTrue(TEXT("synthetic journal evidence retained"), FFileHelper::SaveStringToFile(Journal, *(EvidenceRoot / TEXT("journal.jsonl")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
		TArray<TSharedPtr<FJsonValue>> CapturedEvents;
		for (const auto& Event : Events.Events)
		{
			auto Captured = MakeShared<FJsonObject>();
			Captured->SetStringField(TEXT("event"), Event.Key);
			Captured->SetStringField(TEXT("json"), Event.Value);
			CapturedEvents.Add(MakeShared<FJsonValueObject>(Captured));
		}
		FString UI;
		FJsonSerializer::Serialize(CapturedEvents, TJsonWriterFactory<>::Create(&UI));
		TestTrue(TEXT("synthetic UI evidence retained"), FFileHelper::SaveStringToFile(UI, *(EvidenceRoot / TEXT("ui-events.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	}
	TestFalse(TEXT("safe local evidence was emitted"), Safe.IsEmpty());
	TestFalse(TEXT("default diagnostics omit all private fixture values"), Safe.Contains(TEXT("DIAGNOSTIC_"), ESearchCase::CaseSensitive));
	bool bCallFound = false, bProviderFailed = false, bTerminalFound = false;
	for (const auto& Entry : Parse(Safe))
	{
		TestEqual(TEXT("diagnostics never certify a complete history"), Entry->GetStringField(TEXT("completeness")), FString(TEXT("not_guaranteed")));
		TestEqual(TEXT("diagnostics identify non-authoritative evidence"), Entry->GetStringField(TEXT("evidence")), FString(TEXT("diagnostic_only")));
		const TSharedPtr<FJsonObject>* Payload = nullptr;
		if (!Entry->TryGetObjectField(TEXT("payload"), Payload)) continue;
		FString CallId, Type, Status;
		(*Payload)->TryGetStringField(TEXT("call_id"), CallId);
		(*Payload)->TryGetStringField(TEXT("type"), Type);
		(*Payload)->TryGetStringField(TEXT("status"), Status);
		if (CallId == JournalCall->GetStringField(TEXT("call_id")) && Type == TEXT("tool_result_message"))
		{
			bCallFound = true;
			TestTrue(TEXT("observed append committed"), (*Payload)->GetBoolField(TEXT("journal_committed")));
			TestEqual(TEXT("same run identity as journal"), (*Payload)->GetStringField(TEXT("run_id")), JournalCall->GetStringField(TEXT("run_id")));
			TestEqual(TEXT("safe record explains failed save"), (*Payload)->GetObjectField(TEXT("mutation_outcome"))->GetStringField(TEXT("save_status")), FString(TEXT("failed")));
		}
		if (Entry->GetStringField(TEXT("event")) == TEXT("attempt_end") && Status == TEXT("failed"))
		{
			bProviderFailed = true;
			TestEqual(TEXT("failed provider belongs to same run"), (*Payload)->GetStringField(TEXT("run_id")), JournalCall->GetStringField(TEXT("run_id")));
			bool bUIIdentityMatched = false;
			for (const auto& UIEvent : Events.Events)
			{
				if (UIEvent.Key != TEXT("attempt_end")) continue;
				TSharedPtr<FJsonObject> UI;
				if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(UIEvent.Value), UI))
					bUIIdentityMatched |= UI->GetStringField(TEXT("attempt_id")) == (*Payload)->GetStringField(TEXT("attempt_id")) && UI->GetStringField(TEXT("status")) == TEXT("failed");
			}
			TestTrue(TEXT("attempt identity correlates with live UI"), bUIIdentityMatched);
			TestTrue(TEXT("attempt latency recorded"), (*Payload)->HasField(TEXT("duration_ms")));
		}
		if ((Type == TEXT("error") || Type == TEXT("done")) && (*Payload)->HasField(TEXT("phase"))) bTerminalFound = true;
	}
	TestTrue(TEXT("failed tool/save reconstructed without private content"), bCallFound);
	TestTrue(TEXT("subsequent provider failure reconstructed"), bProviderFailed);
	TestTrue(TEXT("terminal phase reconstructed"), bTerminalFound);
	TestTrue(TEXT("raw capture remains off"), Environment.Read(TEXT("raw")).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDiagnostics_RawCaptureToggle,
	"KismetAI.Diagnostics.RawCaptureToggle", KismetAIDiagnosticBoundaryTest::Flags)
bool FDiagnostics_RawCaptureToggle::RunTest(const FString& Parameters)
{
	using namespace KismetAIDiagnosticBoundaryTest;
	FEnvironment Environment;
	FScriptedToolInvoker Invoker;
	Environment.Set(TEXT("KISMET_DIAGNOSTICS_RAW_PROMPTS"), TEXT("1"));
	FScriptedModelClient FirstClient({ MakeTextTurn(TEXT("done")) });
	FKismetAIAgent First(Invoker, FirstClient, DiagnosticConfig());
	First.RunTask(TEXT("DIAGNOSTIC_RAW_OPTED_IN"), FOnAgentStep());
	Environment.Set(TEXT("KISMET_DIAGNOSTICS_RAW_PROMPTS"), TEXT("0"));
	FScriptedModelClient SecondClient({ MakeTextTurn(TEXT("done")) });
	FKismetAIAgent Second(Invoker, SecondClient, DiagnosticConfig());
	Second.RunTask(TEXT("DIAGNOSTIC_RAW_OPTED_OUT"), FOnAgentStep());
	const FString Raw = Environment.Read(TEXT("raw"));
#if PLATFORM_MAC || PLATFORM_LINUX
	TestTrue(TEXT("explicit local capture contains opted-in prompt"), Raw.Contains(TEXT("DIAGNOSTIC_RAW_OPTED_IN")));
	struct stat Info;
	const FString RawPath = Environment.Root / FString::Printf(TEXT("raw-%u.jsonl"), FPlatformProcess::GetCurrentProcessId());
	if (TestTrue(TEXT("raw log exists"), stat(TCHAR_TO_UTF8(*RawPath), &Info) == 0))
		TestEqual(TEXT("raw capture is owner-only"), static_cast<int32>(Info.st_mode & 0777), 0600);
#else
	TestTrue(TEXT("unsupported private permissions refuse raw capture"), Raw.IsEmpty());
#endif
	TestFalse(TEXT("next request honors capture off"), Raw.Contains(TEXT("DIAGNOSTIC_RAW_OPTED_OUT")));
	TestFalse(TEXT("safe log never copies raw prompt"), Environment.Read(TEXT("safe")).Contains(TEXT("DIAGNOSTIC_RAW_")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDiagnostics_MissingEventsPreserveJournal,
	"KismetAI.Diagnostics.MissingEventsPreserveJournal", KismetAIDiagnosticBoundaryTest::Flags)
bool FDiagnostics_MissingEventsPreserveJournal::RunTest(const FString& Parameters)
{
	using namespace KismetAIDiagnosticBoundaryTest;
	FEnvironment Environment;
	FScriptedModelClient Client({ MakeToolTurn(TEXT("omitted-diagnostic"), TEXT("edit_blueprint"), MutationArgs()), MakeTextTurn(TEXT("done")) });
	FScriptedToolInvoker Invoker;
	Invoker.SetTypedResult(TEXT("edit_blueprint"), FailedSave());
	Invoker.OnInvoke = [&](const FString&) { Environment.Set(TEXT("KISMET_DIAGNOSTICS_DISABLE"), TEXT("1")); };
	FKismetAIAgent Agent(Invoker, Client, DiagnosticConfig());
	Agent.RunTask(TEXT("edit"), FOnAgentStep());
	Environment.Set(TEXT("KISMET_DIAGNOSTICS_DISABLE"), TEXT("0"));
	const auto JournalCall = CallRecord(Agent.GetSessionId());
	TestTrue(TEXT("disabled diagnostics cannot remove actual tool record"), JournalCall.IsValid());
	if (JournalCall.IsValid()) TestTrue(TEXT("journal retains applied mutation despite missing diagnostic"), JournalCall->GetObjectField(TEXT("mutation_outcome"))->GetBoolField(TEXT("applied_in_memory")));
	const FString Safe = Environment.Read(TEXT("safe"));
	TestFalse(TEXT("observations before disabled boundary exist"), Safe.IsEmpty());
	bool bClaimedComplete = false, bCallObserved = false;
	for (const auto& Entry : Parse(Safe))
	{
		bClaimedComplete |= Entry->GetStringField(TEXT("completeness")) != TEXT("not_guaranteed");
		const TSharedPtr<FJsonObject>* Payload = nullptr;
		FString Type;
		if (Entry->TryGetObjectField(TEXT("payload"), Payload)) (*Payload)->TryGetStringField(TEXT("type"), Type);
		bCallObserved |= Type == TEXT("tool_result_message");
	}
	TestFalse(TEXT("missing tool event is genuine"), bCallObserved);
	TestFalse(TEXT("incomplete observations cannot certify no mutation"), bClaimedComplete);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDiagnostics_DisabledDoesNotGateRun,
	"KismetAI.Diagnostics.DisabledDoesNotGateRun", KismetAIDiagnosticBoundaryTest::Flags)
bool FDiagnostics_DisabledDoesNotGateRun::RunTest(const FString& Parameters)
{
	using namespace KismetAIDiagnosticBoundaryTest;
	FEnvironment Environment;
	Environment.Set(TEXT("KISMET_DIAGNOSTICS_DISABLE"), TEXT("1"));
	Environment.Set(TEXT("KISMET_DIAGNOSTICS_RAW_PROMPTS"), TEXT("1"));
	FScriptedToolInvoker Invoker;
	FScriptedModelClient Client({ MakeTextTurn(TEXT("done")) });
	FKismetAIAgent Agent(Invoker, Client, DiagnosticConfig());
	FEventRecorder Events;
	Agent.RunTask(TEXT("DIAGNOSTIC_DISABLED_PROMPT"), Events.MakeCallback());
	TestEqual(TEXT("run completed with diagnostics disabled"), Events.Count(TEXT("done")), 1);
	TestTrue(TEXT("safe log disabled"), Environment.Read(TEXT("safe")).IsEmpty());
	TestTrue(TEXT("disable overrides raw opt-in"), Environment.Read(TEXT("raw")).IsEmpty());
	FString Journal;
	FFileHelper::LoadFileToString(Journal, *FKismetAISessionStorage::GetSessionFilePath(Agent.GetSessionId()));
	TestTrue(TEXT("authoritative journal still writes"), Journal.Contains(TEXT("DIAGNOSTIC_DISABLED_PROMPT")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDiagnostics_PerAttemptUsage,
	"KismetAI.Diagnostics.PerAttemptUsage", KismetAIDiagnosticBoundaryTest::Flags)
bool FDiagnostics_PerAttemptUsage::RunTest(const FString& Parameters)
{
	using namespace KismetAIDiagnosticBoundaryTest;
	FEnvironment Environment;
	const FString Run = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	FKismetAIModelRequestStream Stream(FOnAgentStep(), {}, Run, Run + TEXT("/t0"), 0);
	FModelCallbacks Callbacks = Stream.MakeCallbacks();
	Stream.OpenAttempt(0);
	FModelCallOutcome Retry;
	Retry.Result = EModelCallResult::RetryableError;
	Retry.HttpStatusCode = 429;
	Retry.UsageReports = { MakeUsage(11, 1) };
	TestTrue(TEXT("owned stream receives actual single-attempt usage"), static_cast<bool>(Callbacks.OnAttemptCompleted));
	if (Callbacks.OnAttemptCompleted) Callbacks.OnAttemptCompleted(Retry);
	Stream.OnAttemptRetrying(0, Retry);
	Stream.OpenAttempt(1);
	FModelCallOutcome Final = MakeTextTurn(TEXT("DIAGNOSTIC_PRIVATE_RESPONSE"));
	Final.UsageReports = { MakeUsage(22, 2) };
	if (Callbacks.OnAttemptCompleted) Callbacks.OnAttemptCompleted(Final);
	// CallWithRetry returns accumulated reports; these must not be relabelled as attempt 1.
	Final.UsageReports.Insert(MakeUsage(11, 1), 0);
	Stream.CloseFinal(KismetAIStreamEvents::EAttemptStatus::Completed, Final);
	TArray<int32> Inputs;
	for (const auto& Entry : Parse(Environment.Read(TEXT("safe"))))
	{
		if (Entry->GetStringField(TEXT("event")) != TEXT("attempt_end")) continue;
		const auto Payload = Entry->GetObjectField(TEXT("payload"));
		TestEqual(TEXT("usage provenance reported"), Payload->GetStringField(TEXT("usage_provenance")), FString(TEXT("reported")));
		TestEqual(TEXT("usage scope is one attempt"), Payload->GetStringField(TEXT("usage_scope")), FString(TEXT("attempt")));
		Inputs.Add(Payload->GetObjectField(TEXT("usage"))->GetIntegerField(TEXT("input_tokens")));
	}
	TestEqual(TEXT("two attempt usage observations"), Inputs.Num(), 2);
	if (Inputs.Num() == 2)
	{
		TestEqual(TEXT("failed attempt retains its tokens"), Inputs[0], 11);
		TestEqual(TEXT("final attempt excludes failed-attempt tokens"), Inputs[1], 22);
	}
	const FString BeforeLateCallback = Environment.Read(TEXT("safe"));
	if (Callbacks.OnAttemptCompleted) Callbacks.OnAttemptCompleted(Final);
	TestEqual(TEXT("retained completion callback cannot reopen closed stream"), Environment.Read(TEXT("safe")), BeforeLateCallback);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDiagnostics_Retention,
	"KismetAI.Diagnostics.Retention", KismetAIDiagnosticBoundaryTest::Flags)
bool FDiagnostics_Retention::RunTest(const FString& Parameters)
{
	using namespace KismetAIDiagnosticBoundaryTest;
	FEnvironment Environment;
	IFileManager::Get().MakeDirectory(*Environment.Root, true);
	const FString OldRaw = Environment.Root / TEXT("raw-2147480000.jsonl");
	const FString OldSafe = Environment.Root / TEXT("safe-2147480000.previous.jsonl");
	const FString Unknown = Environment.Root / TEXT("unknown.jsonl");
	for (const FString& Path : {OldRaw, OldSafe, Unknown}) FFileHelper::SaveStringToFile(TEXT("owned-fixture"), *Path);
	IFileManager::Get().SetTimeStamp(*OldRaw, FDateTime::UtcNow() - FTimespan::FromDays(2));
	IFileManager::Get().SetTimeStamp(*OldSafe, FDateTime::UtcNow() - FTimespan::FromDays(8));
	IFileManager::Get().SetTimeStamp(*Unknown, FDateTime::UtcNow() - FTimespan::FromDays(8));
	FScriptedToolInvoker Invoker;
	FScriptedModelClient Client({ MakeTextTurn(TEXT("done")) });
	FKismetAIAgent Agent(Invoker, Client, DiagnosticConfig());
	Agent.RunTask(TEXT("retention check"), FOnAgentStep());
	TestFalse(TEXT("expired raw capture removed on next diagnostic use"), IFileManager::Get().FileExists(*OldRaw));
	TestFalse(TEXT("expired safe segment removed on next diagnostic use"), IFileManager::Get().FileExists(*OldSafe));
	TestTrue(TEXT("unknown file not owned by diagnostics preserved"), IFileManager::Get().FileExists(*Unknown));
	return true;
}

#if PLATFORM_MAC || PLATFORM_LINUX
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDiagnostics_RejectSymlinks,
	"KismetAI.Diagnostics.RejectSymlinks", KismetAIDiagnosticBoundaryTest::Flags)
bool FDiagnostics_RejectSymlinks::RunTest(const FString& Parameters)
{
	using namespace KismetAIDiagnosticBoundaryTest;
	FEnvironment Environment;
	IFileManager::Get().MakeDirectory(*Environment.Root, true);
	const FString Target = Environment.Root / TEXT("unrelated-target");
	FFileHelper::SaveStringToFile(TEXT("UNCHANGED_TARGET"), *Target);
	const FString RawPath = Environment.Root / FString::Printf(TEXT("raw-%u.jsonl"), FPlatformProcess::GetCurrentProcessId());
	if (!TestTrue(TEXT("raw symlink fixture"), symlink(TCHAR_TO_UTF8(*Target), TCHAR_TO_UTF8(*RawPath)) == 0)) return false;
	Environment.Set(TEXT("KISMET_DIAGNOSTICS_RAW_PROMPTS"), TEXT("1"));
	FScriptedToolInvoker Invoker;
	FScriptedModelClient FirstClient({ MakeTextTurn(TEXT("done")) });
	FKismetAIAgent First(Invoker, FirstClient, DiagnosticConfig());
	First.RunTask(TEXT("DIAGNOSTIC_SYMLINK_SECRET"), FOnAgentStep());
	FString Contents;
	FFileHelper::LoadFileToString(Contents, *Target);
	TestEqual(TEXT("raw capture never follows file symlink"), Contents, FString(TEXT("UNCHANGED_TARGET")));

	const FString TargetDir = Environment.Root / TEXT("target-directory");
	const FString LinkedRoot = Environment.Root / TEXT("linked-root");
	IFileManager::Get().MakeDirectory(*TargetDir, true);
	if (!TestTrue(TEXT("root symlink fixture"), symlink(TCHAR_TO_UTF8(*TargetDir), TCHAR_TO_UTF8(*LinkedRoot)) == 0)) return false;
	Environment.Set(TEXT("KISMET_DIAGNOSTICS_ROOT"), LinkedRoot);
	FScriptedModelClient SecondClient({ MakeTextTurn(TEXT("done")) });
	FKismetAIAgent Second(Invoker, SecondClient, DiagnosticConfig());
	Second.RunTask(TEXT("DIAGNOSTIC_LINKED_ROOT_SECRET"), FOnAgentStep());
	TArray<FString> Created;
	IFileManager::Get().FindFiles(Created, *(TargetDir / TEXT("*")), true, false);
	TestEqual(TEXT("linked diagnostic root refuses all writes"), Created.Num(), 0);
	return true;
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDiagnostics_DisabledExpiry,
	"KismetAI.Diagnostics.DisabledExpiry", KismetAIDiagnosticBoundaryTest::Flags)
bool FDiagnostics_DisabledExpiry::RunTest(const FString& Parameters)
{
	using namespace KismetAIDiagnosticBoundaryTest;
	FEnvironment Environment;
	IFileManager::Get().MakeDirectory(*Environment.Root, true);
	const FString OldRaw = Environment.Root / TEXT("raw-2147480000.jsonl");
	FFileHelper::SaveStringToFile(TEXT("EXPIRED_PRIVATE_PROMPT"), *OldRaw);
	IFileManager::Get().SetTimeStamp(*OldRaw, FDateTime::UtcNow() - FTimespan::FromDays(2));
	Environment.Set(TEXT("KISMET_DIAGNOSTICS_DISABLE"), TEXT("1"));
	Environment.Set(TEXT("KISMET_DIAGNOSTICS_RAW_PROMPTS"), TEXT("1"));
	FScriptedToolInvoker Invoker;
	FScriptedModelClient Client({ MakeTextTurn(TEXT("done")) });
	FKismetAIAgent Agent(Invoker, Client, DiagnosticConfig());
	FEventRecorder Events;
	Agent.RunTask(TEXT("DISABLED_PRIVATE_PROMPT"), Events.MakeCallback());
	TestFalse(TEXT("disable does not suspend expiry of retained private prompts"), IFileManager::Get().FileExists(*OldRaw));
	TestTrue(TEXT("disabled run writes no safe observations"), Environment.Read(TEXT("safe")).IsEmpty());
	TestTrue(TEXT("disabled run writes no new raw capture"), Environment.Read(TEXT("raw")).IsEmpty());
	TestEqual(TEXT("disabled run finishes normally"), Events.Count(TEXT("done")), 1);
	FString Journal;
	FFileHelper::LoadFileToString(Journal, *FKismetAISessionStorage::GetSessionFilePath(Agent.GetSessionId()));
	TestTrue(TEXT("disabled diagnostics leave authoritative journal working"), Journal.Contains(TEXT("DISABLED_PRIVATE_PROMPT")));
	return true;
}

#if PLATFORM_MAC || PLATFORM_LINUX
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDiagnostics_FIFOIsNonblocking,
	"KismetAI.Diagnostics.FIFOIsNonblocking", KismetAIDiagnosticBoundaryTest::Flags)
bool FDiagnostics_FIFOIsNonblocking::RunTest(const FString& Parameters)
{
	using namespace KismetAIDiagnosticBoundaryTest;
	FEnvironment Environment;
	IFileManager::Get().MakeDirectory(*Environment.Root, true);
	Environment.Set(TEXT("KISMET_DIAGNOSTICS_RAW_PROMPTS"), TEXT("1"));
	const FString RunId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	for (const bool bRaw : {true, false})
	{
		const FString Path = Environment.Root / FString::Printf(TEXT("%s-%u.jsonl"), bRaw ? TEXT("raw") : TEXT("safe"), FPlatformProcess::GetCurrentProcessId());
		if (!TestTrue(TEXT("disposable FIFO created"), mkfifo(TCHAR_TO_UTF8(*Path), 0600) == 0)) return false;
		auto Completion = Async(EAsyncExecution::Thread, [bRaw, RunId]()
		{
			if (bRaw) KismetAIDiagnostics::CapturePrompt(RunId, TEXT("FIFO_PRIVATE_PROMPT"));
			else
			{
				auto Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("run_id"), RunId);
				KismetAIDiagnostics::Record(TEXT("attempt_start"), Payload);
			}
		});
		const bool bPrompt = Completion.WaitFor(FTimespan::FromSeconds(1));
		// The regression blocks in open(O_WRONLY). A nonblocking reader releases
		// it so this negative test joins its worker instead of orphaning it.
		int Rescue = -1;
		if (!bPrompt) Rescue = open(TCHAR_TO_UTF8(*Path), O_RDWR | O_NONBLOCK);
		Completion.Wait();
		if (Rescue >= 0) close(Rescue);
		TestTrue(bRaw ? TEXT("raw capture refuses FIFO without blocking") : TEXT("safe logging refuses FIFO without blocking"), bPrompt);
		struct stat Info;
		TestTrue(TEXT("FIFO remains an unmodified nonregular object"), lstat(TCHAR_TO_UTF8(*Path), &Info) == 0 && S_ISFIFO(Info.st_mode));
		unlink(TCHAR_TO_UTF8(*Path));
	}
	return true;
}
#endif
