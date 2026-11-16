// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
#include "RunEntryHarness.h"
#include "KismetAISourceConfinement.h"
#include "KismetAISourceSearch.h"
#include "KismetAISessionStorage.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"

namespace SourcePanelTest
{
	TSharedPtr<FJsonObject> ToolData(const KismetAIRunEntryHarness::FSlowModelClient& Model, const FString& Id)
	{
		const auto Requests = Model.GetRequests();
		if (Requests.IsEmpty()) return nullptr;
		for (const auto& Message : Requests.Last())
		{
			const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
			if (!Message->AsObject()->TryGetArrayField(TEXT("content"), Content)) continue;
			for (const auto& Block : *Content)
			{
				const auto Object = Block->AsObject();
				FString ToolId, Text;
				if (!Object || !Object->TryGetStringField(TEXT("tool_use_id"), ToolId) || ToolId != Id
					|| !Object->TryGetStringField(TEXT("content"), Text)) continue;
				TSharedPtr<FJsonObject> Envelope;
				const TSharedPtr<FJsonObject>* Data = nullptr;
				if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Envelope)
					&& Envelope->TryGetObjectField(TEXT("data"), Data)) return *Data;
			}
		}
		return nullptr;
	}
}

// This is the actual panel LaunchAgent path with a scripted model and production
// executor, GT admission, filesystem worker, session journal and finalization.
// Core Search.ActiveCancellation separately cancels inside traversal at entry128.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourcePanelPaginationCancel, "KismetAI.ReadSource.Panel.PaginationAndCancel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourcePanelPaginationCancel::RunTest(const FString& Parameters)
{
	using namespace KismetAIRunEntryHarness;
	FScopedModuleSeams Seams;
	auto& Module = Seams.Module;
	auto& Model = Seams.Seams->Model;
	if (FAccess::Session(Module).IsRunning()) { AddError(TEXT("Another run is active")); return false; }
	const FString Base = FString(FPlatformProcess::UserTempDir()) / (TEXT("KismetP206Panel-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	ON_SCOPE_EXIT { KismetAISourceSearch::Close(); IFileManager::Get().DeleteDirectory(*Base, false, true); };
	FKismetAISourceRoots Roots;
	Roots.ProjectDir = Base / TEXT("Project");
	Roots.EngineDir = Base / TEXT("Engine");
	Roots.bIncludeDiscoveredPlugins = false;
	const FString Source = Roots.ProjectDir / TEXT("Source");
	IFileManager::Get().MakeDirectory(*Source, true);
	for (int32 Index = 0; Index < 53; ++Index)
	{
		if (!TestTrue(TEXT("Panel fixture created"), FFileHelper::SaveStringToFile(TEXT("// panel source\n"), *(Source / FString::Printf(TEXT("Entry%03d.h"), Index))))) return false;
	}
	KismetAISourceConfinement::FScopedRootsOverride Override(Roots);
	Module.SetCurrentSessionId(FString());
	// Installed after roots override: workers always drain before that snapshot seam dies.
	ON_SCOPE_EXIT
	{
		FAccess::Session(Module).CancelActive();
		Model.Unblock();
		TestTrue(TEXT("Panel worker cleanup completes"), PumpUntil([&] { return !FAccess::Session(Module).IsRunning() && FAccess::Session(Module).NumLiveWorkers() == 0; }));
	};
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("mode"), TEXT("find"));
	Args->SetStringField(TEXT("scope"), TEXT("project"));
	Args->SetStringField(TEXT("pattern"), TEXT("*.h"));
	Args->SetNumberField(TEXT("limit"), 20);
	Model.Script = { KismetAIRunEntryHarness::ToolTurn(TEXT("source_page1"), TEXT("read_source"), Args), KismetAIRunEntryHarness::TextTurn(TEXT("placeholder")), KismetAIRunEntryHarness::TextTurn(TEXT("done")) };
	Model.BlockOnCall = 1;
	Module.LaunchAgent(TEXT("Find source headers in pages."), FString(), FString(), FString());
	const FString SessionId = Module.GetCurrentSessionId();
	TestTrue(TEXT("Actual panel lease admitted"), FAccess::Session(Module).IsRunning());
	if (!TestTrue(TEXT("First page reached next model call"), PumpUntil([&] { return Model.Calls.load() == 2 && Model.Entered.Wait(0); }))) return false;
	const auto First = SourcePanelTest::ToolData(Model, TEXT("source_page1"));
	if (!TestTrue(TEXT("Panel model received structured first page"), First.IsValid())) return false;
	TestEqual(TEXT("First page result cap"), First->GetNumberField(TEXT("count")), 20.0);
	TestTrue(TEXT("Panel page carries actual work metrics"), First->GetObjectField(TEXT("metrics"))->GetNumberField(TEXT("entries_considered")) <= 1024);
	auto NextArgs = MakeShared<FJsonObject>();
	NextArgs->Values = Args->Values;
	NextArgs->SetStringField(TEXT("cursor"), First->GetStringField(TEXT("cursor")));
	Model.Script[1] = KismetAIRunEntryHarness::ToolTurn(TEXT("source_page2"), TEXT("read_source"), NextArgs);
	Model.BlockOnCall = 2;
	Model.Entered.Event->Reset();
	Model.Unblock();
	// Do not pump GT while rearming the model latch: the second source call must
	// pass GT admission before it can finish, so its following model call cannot
	// consume the previous manual-reset signal in this interval.
	const double RearmDeadline = FPlatformTime::Seconds() + 5;
	while (Model.Completed.load() < 2 && FPlatformTime::Seconds() < RearmDeadline) FPlatformProcess::SleepNoStats(0.001f);
	if (!TestEqual(TEXT("Provider leaves first page barrier"), Model.Completed.load(), 2)) return false;
	Model.Wake.Event->Reset();
	if (!TestTrue(TEXT("Second page reached model"), PumpUntil([&] { return Model.Calls.load() == 3 && Model.Entered.Wait(0); }))) return false;
	const auto Second = SourcePanelTest::ToolData(Model, TEXT("source_page2"));
	if (!TestTrue(TEXT("Panel model received continued page"), Second.IsValid())) return false;
	TestEqual(TEXT("Second page result cap"), Second->GetNumberField(TEXT("count")), 20.0);
	TArray<FString> FirstLines, SecondLines;
	First->GetStringField(TEXT("result")).ParseIntoArrayLines(FirstLines);
	Second->GetStringField(TEXT("result")).ParseIntoArrayLines(SecondLines);
	int32 SessionHeaders = 0;
	for (const auto& Entry : FKismetAISessionStorage::LoadAllEntries(SessionId))
		if (Entry->GetStringField(TEXT("type")) == TEXT("session_start")) ++SessionHeaders;
	TestEqual(TEXT("fresh panel flow writes one durable header"), SessionHeaders, 1);
	for (const FString& Line : SecondLines) TestFalse(TEXT("Panel continuation never replays first page"), FirstLines.Contains(Line));
	TestTrue(TEXT("Panel cancel reaches active run"), FAccess::Session(Module).CancelActive());
	if (!TestTrue(TEXT("Cancelled panel releases lease and worker"), PumpUntil([&] { return !FAccess::Session(Module).IsRunning() && FAccess::Session(Module).NumLiveWorkers() == 0; }))) return false;
	int32 TerminalMetadata = 0;
	for (const auto& Entry : FKismetAISessionStorage::LoadAllEntries(SessionId))
	{
		FString Type, Status;
		if (Entry->TryGetStringField(TEXT("type"), Type) && Type == TEXT("session_meta")
			&& Entry->TryGetStringField(TEXT("status"), Status) && !Status.IsEmpty())
		{
			++TerminalMetadata;
			TestEqual(TEXT("Panel terminal persisted as aborted"), Status, FString(TEXT("aborted")));
		}
	}
	TestEqual(TEXT("One terminal metadata record"), TerminalMetadata, 1);
	FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	TestEqual(TEXT("No late provider continuation"), Model.Calls.load(), 3);
	TestFalse(TEXT("Cancelled panel run is not revived"), FAccess::Session(Module).IsRunning());
	TestEqual(TEXT("No orphan panel worker"), FAccess::Session(Module).NumLiveWorkers(), 0);
	return true;
}
