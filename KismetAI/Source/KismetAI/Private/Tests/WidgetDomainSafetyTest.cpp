// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Misc/AutomationTest.h"
#include "RunEntryHarness.h"
#include "DisposableContent.h"
#include "ScopedTestGrant.h"
#include "KismetAIAssetState.h"
#include "KismetAICheckpoints.h"
#include "KismetAIMcpRoute.h"
#include "KismetAIPersistence.h"
#include "KismetAIToolCallJournal.h"
#include "KismetAISessionStorage.h"
#include "HAL/PlatformMisc.h"
#include "Async/Async.h"
#include "BlueprintActionDatabase.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Editor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "WidgetBlueprint.h"

namespace KismetAIWidgetDomainTest
{
	using namespace KismetAIRunEntryHarness;
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	TSharedPtr<FJsonObject> Parse(FString Text)
	{
		Text.RemoveFromStart(TEXT("<tool_use_error>"));
		Text.RemoveFromEnd(TEXT("</tool_use_error>"));
		TSharedPtr<FJsonObject> Result;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Result);
		return Result;
	}

	TSharedPtr<FJsonObject> ObjectAt(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
	{
		const TSharedPtr<FJsonObject>* Value = nullptr;
		return Object && Object->TryGetObjectField(Key, Value) ? *Value : nullptr;
	}

	FString StringAt(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
	{
		FString Value;
		if (Object) Object->TryGetStringField(Key, Value);
		return Value;
	}

	FString Json(const TSharedPtr<FJsonObject>& Object)
	{
		FString Text;
		FJsonSerializer::Serialize(Object.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
		return Text;
	}

	struct FFixture
	{
		KismetAIDisposableContent::FScopedDisposableMount Mount{ TEXT("WidgetDomain") };
		FString Path = Mount.PackageName(TEXT("WBP_Domain"));
		UWidgetBlueprint* Blueprint = nullptr;
		bool bSaved = false;

		FFixture()
		{
			UPackage* Package = CreatePackage(*Path);
			Blueprint = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(), Package,
				TEXT("WBP_Domain"), BPTYPE_Normal, UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
			if (!Blueprint || !Blueprint->WidgetTree) return;
			UCanvasPanel* Root = Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
			Blueprint->WidgetTree->RootWidget = Root;
			UTextBlock* Label = Blueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Label"));
			Label->SetText(FText::FromString(TEXT("baseline")));
			Root->AddChild(Label);
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			FKismetAIMutationOutcome Saved;
			KismetAIPersistence::SaveExplicitly(Blueprint, Saved);
			bSaved = Saved.SaveStatus == EKismetAISaveStatus::Saved;
		}

		~FFixture()
		{
			if (!Blueprint) return;
			UPackage* Package = Blueprint->GetOutermost();
			ResetLoaders(Package);
			FBlueprintActionDatabase::Get().ClearAssetActions(Blueprint);
			for (UClass* Class : { Blueprint->GeneratedClass.Get(), Blueprint->SkeletonGeneratedClass.Get() })
			{
				if (Class) { Class->ClearFlags(RF_Public | RF_Standalone); Class->MarkAsGarbage(); }
			}
			Blueprint->ClearFlags(RF_Public | RF_Standalone);
			Blueprint->MarkAsGarbage();
			Package->SetDirtyFlag(false);
			Package->MarkAsGarbage();
			if (GEditor) GEditor->ResetTransaction(FText::FromString(TEXT("Widget domain fixture")));
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
		}

		UTextBlock* Label() const { return Cast<UTextBlock>(Blueprint->WidgetTree->FindWidget(TEXT("Label"))); }
		FString Text() const { return Label()->GetText().ToString(); }
		TSharedPtr<FJsonObject> Args(const TCHAR* Action, const TSharedPtr<FJsonObject>& Properties) const
		{
			TSharedPtr<FJsonObject> Operation = MakeShared<FJsonObject>();
			Operation->SetStringField(TEXT("action"), Action);
			Operation->SetStringField(TEXT("name"), TEXT("Label"));
			Operation->SetObjectField(TEXT("properties"), Properties);
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("widget"), Blueprint->GetPathName());
			Result->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(Operation) });
			return Result;
		}
		TSharedPtr<FJsonObject> Edit(bool bUnknown = false) const
		{
			TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
			Properties->SetStringField(TEXT("Text"), TEXT("edited"));
			if (bUnknown) Properties->SetStringField(TEXT("NoSuchWidgetProperty"), TEXT("invalid"));
			return Args(TEXT("set_widget_property"), Properties);
		}
	};

	TSharedPtr<FJsonObject> CallEntry(EKismetAIEntryPoint Entry, const FString& Tool, const TSharedPtr<FJsonObject>& Args)
	{
		FKismetAIModule& Module = FKismetAIModule::Get();
		const FKismetAICallerContext Caller = FKismetAICallerContext::Verified(Entry, TEXT("widget-domain-test"));
		if (Entry == EKismetAIEntryPoint::Mcp) return Parse(KismetAIMcpRoute::CallTool(Module, Caller, Tool, Args));
		if (Entry == EKismetAIEntryPoint::Rest)
		{
			TSharedPtr<FJsonObject> Reply;
			Module.ServeToolRoute(*Tool, nullptr, Caller, MakeRequest(Json(Args)), [&Reply](TUniquePtr<FHttpServerResponse>&& Response)
			{
				if (Response) Reply = Parse(BodyText(Response->Body));
			});
			return Reply;
		}
		FKismetAIAgent Agent(MakeUnique<FKismetAIModuleToolInvoker>(Module), FKismetAIAgentConfig());
		Agent.SetCallerContext(Caller);
		return Parse(Agent.ExecuteToolForExternalClient(Tool, Args));
	}

	const EKismetAIEntryPoint Entries[] = { EKismetAIEntryPoint::Panel, EKismetAIEntryPoint::Rest, EKismetAIEntryPoint::Mcp };

	struct FRunEvidence
	{
		FString Session;
		TMap<FString, TSharedPtr<FJsonObject>> Results;
		TSharedPtr<FJsonObject> Ledger;
		int32 Terminals = 0;
	};

	FRunEvidence Run(const TArray<FModelCallOutcome>& Script)
	{
		FSlowModelClient Model;
		Model.Script = Script;
		FKismetAIModuleToolInvoker Invoker(FKismetAIModule::Get());
		FKismetAIAgentConfig Config;
		Config.ApiKey = TEXT("unused-scripted-provider");
		Config.Model = TEXT("claude-sonnet-4-6");
		Config.MaxSteps = 5;
		Config.MaxRetries = 0;
		Config.ThinkingMode = EThinkingMode::Disabled;
		FKismetAIAgent Agent(Invoker, Model, Config);
		Agent.SetCallerContext(FKismetAICallerContext::Verified(EKismetAIEntryPoint::Panel, TEXT("widget-domain-test")));
		FRunEvidence Evidence;
		FOnAgentStep Step;
		Step.BindLambda([&](const FString& Type, const FString& Text)
		{
			const TSharedPtr<FJsonObject> Event = Parse(Text);
			if (Type == TEXT("tool_result"))
			{
				TSharedPtr<FJsonObject> Result = ObjectAt(Event, TEXT("result"));
				if (!Result) Result = Parse(StringAt(Event, TEXT("result")));
				Evidence.Results.Add(StringAt(Event, TEXT("tool_use_id")), Result);
			}
			if (Type == TEXT("mutation_outcomes")) Evidence.Ledger = Event;
			if (Type == TEXT("done") || Type == TEXT("error")) ++Evidence.Terminals;
		});
		Agent.RunTask(TEXT("Exercise widget domain safety."), Step);
		Evidence.Session = Agent.GetSessionId();
		return Evidence;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDomainRejectedParity, "KismetAI.WidgetDomain.RejectedParity", KismetAIWidgetDomainTest::Flags)
bool FWidgetDomainRejectedParity::RunTest(const FString& Parameters)
{
	using namespace KismetAIWidgetDomainTest;
	FFixture Fixture;
	if (!TestTrue(TEXT("fixture saved"), Fixture.bSaved)) return false;
	const FKismetAIAssetFingerprint Before = KismetAIAssetState::Capture(Fixture.Path);
	for (EKismetAIEntryPoint Entry : { EKismetAIEntryPoint::Rest, EKismetAIEntryPoint::Mcp })
	{
		const TSharedPtr<FJsonObject> Reply = CallEntry(Entry, TEXT("edit_widget"), Fixture.Edit());
		TestEqual(TEXT("ungranted mutation rejected"), StringAt(ObjectAt(Reply, TEXT("error")), TEXT("error_code")), FString(TEXT("AUTHORITY_REQUIRED")));
	}
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Panel, EKismetAIEntryPoint::Rest, EKismetAIEntryPoint::Mcp }, EKismetAIAuthorityOp::Edit, { *Fixture.Mount.Scope() });
	TSharedPtr<FJsonObject> Malformed = Fixture.Edit();
	Malformed->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()) });
	for (EKismetAIEntryPoint Entry : Entries)
	{
		const TSharedPtr<FJsonObject> Reply = CallEntry(Entry, TEXT("edit_widget"), Malformed);
		TestEqual(TEXT("malformed mutation rejected"), StringAt(ObjectAt(Reply, TEXT("error")), TEXT("error_code")), FString(TEXT("VALIDATION_ERROR")));
	}
	const FKismetAIAssetFingerprint After = KismetAIAssetState::Capture(Fixture.Path);
	TestTrue(TEXT("rejected requests preserve memory"), Before.SameState(After));
	TestTrue(TEXT("rejected requests preserve disk"), Before.SameFile(After));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDomainReadParity, "KismetAI.WidgetDomain.ReadParity", KismetAIWidgetDomainTest::Flags)
bool FWidgetDomainReadParity::RunTest(const FString& Parameters)
{
	using namespace KismetAIWidgetDomainTest;
	FFixture Fixture;
	if (!TestTrue(TEXT("fixture saved"), Fixture.bSaved)) return false;
	for (const TCHAR* Mode : { TEXT("tree"), TEXT("overview") })
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("widget"), Fixture.Blueprint->GetPathName());
		Args->SetStringField(TEXT("mode"), Mode);
		FString Baseline;
		for (EKismetAIEntryPoint Entry : Entries)
		{
			const TSharedPtr<FJsonObject> Reply = CallEntry(Entry, TEXT("read_widget"), Args);
			if (!TestTrue(TEXT("read succeeded"), Reply && Reply->GetBoolField(TEXT("success")))) continue;
			const FString Data = Json(ObjectAt(Reply, TEXT("data")));
			if (Baseline.IsEmpty()) Baseline = Data;
			else TestEqual(TEXT("all callers read the same domain payload"), Data, Baseline);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDomainPartialWidgetProperty, "KismetAI.WidgetDomain.PartialWidgetPropertyOutcome", KismetAIWidgetDomainTest::Flags)
bool FWidgetDomainPartialWidgetProperty::RunTest(const FString& Parameters)
{
	using namespace KismetAIWidgetDomainTest;
	KismetAIPersistence::FScopedEvalModeOverride Autosave(false);
	for (EKismetAIEntryPoint Entry : Entries)
	{
		FFixture Fixture;
		if (!TestTrue(TEXT("fixture saved"), Fixture.bSaved)) return false;
		FKismetAIScopedTestGrant Grant({ Entry }, EKismetAIAuthorityOp::Edit, { *Fixture.Mount.Scope() });
		int32 Saves = 0;
		KismetAIPersistence::FScopedSaveFunctionOverride FailSave([&](UPackage*, UObject*, const FString&, FString& Error)
		{
			++Saves; Error = TEXT("injected widget save failure"); return false;
		});
		AddExpectedMessage(TEXT("Save failed for"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
		const TSharedPtr<FJsonObject> Reply = CallEntry(Entry, TEXT("edit_widget"), Fixture.Edit(true));
		const TSharedPtr<FJsonObject> Data = ObjectAt(Reply, TEXT("data"));
		const TSharedPtr<FJsonObject> Outcome = ObjectAt(Data, TEXT("outcome"));
		TestEqual(TEXT("valid property really changed"), Fixture.Text(), FString(TEXT("edited")));
		TestTrue(TEXT("failed operation retains truthful applied flag"), Outcome && Outcome->GetBoolField(TEXT("applied_in_memory")));
		TestEqual(TEXT("partial mutation attempts shared save"), Saves, 1);
		TestEqual(TEXT("partial mutation reports failed save"), StringAt(Outcome, TEXT("save_status")), FString(TEXT("failed")));
		TestTrue(TEXT("operation still reports failure"), Data && Data->GetNumberField(TEXT("operations_failed")) == 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDomainPartialSlotProperty, "KismetAI.WidgetDomain.PartialSlotPropertyOutcome", KismetAIWidgetDomainTest::Flags)
bool FWidgetDomainPartialSlotProperty::RunTest(const FString& Parameters)
{
	using namespace KismetAIWidgetDomainTest;
	KismetAIPersistence::FScopedEvalModeOverride Autosave(false);
	FFixture Fixture;
	if (!TestTrue(TEXT("fixture saved"), Fixture.bSaved)) return false;
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Mcp }, EKismetAIAuthorityOp::Edit, { *Fixture.Mount.Scope() });
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetNumberField(TEXT("z_order"), 7);
	Properties->SetNumberField(TEXT("NoSuchSlotProperty"), 9);
	const TSharedPtr<FJsonObject> Reply = CallEntry(EKismetAIEntryPoint::Mcp, TEXT("edit_widget"), Fixture.Args(TEXT("set_slot_property"), Properties));
	const TSharedPtr<FJsonObject> Outcome = ObjectAt(ObjectAt(Reply, TEXT("data")), TEXT("outcome"));
	TestEqual(TEXT("valid slot property really changed"), Cast<UCanvasPanelSlot>(Fixture.Label()->Slot)->GetZOrder(), 7);
	TestTrue(TEXT("partial slot mutation reported"), Outcome && Outcome->GetBoolField(TEXT("applied_in_memory")));
	TestEqual(TEXT("partial slot mutation saved"), StringAt(Outcome, TEXT("save_status")), FString(TEXT("saved")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDomainQueuedCancellation, "KismetAI.WidgetDomain.QueuedCancellation", KismetAIWidgetDomainTest::Flags)
bool FWidgetDomainQueuedCancellation::RunTest(const FString& Parameters)
{
	using namespace KismetAIWidgetDomainTest;
	FFixture Fixture;
	if (!TestTrue(TEXT("fixture saved"), Fixture.bSaved)) return false;
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Panel }, EKismetAIAuthorityOp::Edit, { *Fixture.Mount.Scope() });
	const FKismetAIAssetFingerprint Before = KismetAIAssetState::Capture(Fixture.Path);
	TSharedRef<KismetAIRunEntryHarness::FLatch, ESPMode::ThreadSafe> Queued = MakeShared<KismetAIRunEntryHarness::FLatch, ESPMode::ThreadSafe>();
	TSharedRef<TFunction<void()>, ESPMode::ThreadSafe> Held = MakeShared<TFunction<void()>, ESPMode::ThreadSafe>();
	FKismetAIModule::Get().SetGameThreadSchedulerForTests([Queued, Held](TFunction<void()> Callback)
	{
		*Held = MoveTemp(Callback);
		Queued->Trigger();
	});
	ON_SCOPE_EXIT { FKismetAIModule::Get().SetGameThreadSchedulerForTests(nullptr); if (*Held) (*Held)(); };
	TSharedRef<FKismetAIAgent, ESPMode::ThreadSafe> Agent = MakeShared<FKismetAIAgent, ESPMode::ThreadSafe>(
		MakeUnique<FKismetAIModuleToolInvoker>(FKismetAIModule::Get()), FKismetAIAgentConfig());
	Agent->SetCallerContext(FKismetAICallerContext::Verified(EKismetAIEntryPoint::Panel, TEXT("widget-domain-test")));
	const TSharedPtr<FJsonObject> Args = Fixture.Edit();
	TFuture<FString> Worker = Async(EAsyncExecution::Thread, [Agent, Args]() { return Agent->ExecuteToolForExternalClient(TEXT("edit_widget"), Args); });
	const bool bQueued = Queued->Wait(KismetAIRunEntryHarness::PromptMs);
	TestTrue(TEXT("widget call queued"), bQueued);
	Agent->Abort();
	const bool bReleased = Worker.WaitFor(FTimespan::FromMilliseconds(KismetAIRunEntryHarness::PromptMs));
	TestTrue(TEXT("cancellation releases worker without editor mutation"), bReleased);
	if (*Held) { (*Held)(); *Held = nullptr; }
	if (!bReleased) PumpUntil([&]() { return Worker.IsReady(); });
	if (!TestTrue(TEXT("worker completed"), Worker.IsReady())) return false;
	TestTrue(TEXT("cancelled before domain entry"), KismetAIToolCallJournal::IsExecutorCancelledResult(Worker.Get()));
	const FKismetAIAssetFingerprint After = KismetAIAssetState::Capture(Fixture.Path);
	TestTrue(TEXT("queued cancellation preserves memory"), Before.SameState(After));
	TestTrue(TEXT("queued cancellation preserves disk"), Before.SameFile(After));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDomainSaveFailure, "KismetAI.WidgetDomain.SaveFailureRetainsCheckpoint", KismetAIWidgetDomainTest::Flags)
bool FWidgetDomainSaveFailure::RunTest(const FString& Parameters)
{
	using namespace KismetAIWidgetDomainTest;
	KismetAIPersistence::FScopedEvalModeOverride Autosave(false);
	FFixture Fixture;
	if (!TestTrue(TEXT("fixture saved"), Fixture.bSaved)) return false;
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Panel }, EKismetAIAuthorityOp::Edit, { *Fixture.Mount.Scope() });
	const FKismetAIAssetFingerprint Before = KismetAIAssetState::Capture(Fixture.Path);
	int32 Saves = 0;
	KismetAIPersistence::FScopedSaveFunctionOverride FailSave([&](UPackage*, UObject*, const FString&, FString& Error)
	{
		++Saves; Error = TEXT("injected widget disk full"); return false;
	});
	AddExpectedMessage(TEXT("Save failed for"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	const FRunEvidence Evidence = Run({ KismetAIRunEntryHarness::ToolTurn(TEXT("widget-edit"), TEXT("edit_widget"), Fixture.Edit()), KismetAIRunEntryHarness::TextTurn(TEXT("Edited.")) });
	TestEqual(TEXT("one terminal"), Evidence.Terminals, 1);
	TestEqual(TEXT("edit survives save failure"), Fixture.Text(), FString(TEXT("edited")));
	TestEqual(TEXT("one autosave attempt"), Saves, 1);
	const FKismetAIAssetFingerprint After = KismetAIAssetState::Capture(Fixture.Path);
	TestTrue(TEXT("failed save preserves baseline file"), Before.SameFile(After));
	TestTrue(TEXT("unsaved widget remains dirty"), After.bPackageDirty);
	const TSharedPtr<FJsonObject>* Result = Evidence.Results.Find(TEXT("widget-edit"));
	const TSharedPtr<FJsonObject> Outcome = Result ? ObjectAt(ObjectAt(*Result, TEXT("data")), TEXT("outcome")) : nullptr;
	TestEqual(TEXT("real agent retains save failure"), StringAt(Outcome, TEXT("save_status")), FString(TEXT("failed")));
	TestTrue(TEXT("real agent reports applied edit"), Outcome && Outcome->GetBoolField(TEXT("applied_in_memory")));
	const TArray<TSharedPtr<FJsonValue>>* Unsaved = nullptr;
	TestTrue(TEXT("run ledger retains unsaved package"), Evidence.Ledger && Evidence.Ledger->TryGetArrayField(TEXT("unsaved_packages"), Unsaved)
		&& Unsaved->ContainsByPredicate([&](const TSharedPtr<FJsonValue>& Value) { return Value->AsString() == Fixture.Path; }));
	FKismetAICheckpointStore& Store = FKismetAIModule::Get().GetCheckpointStore();
	const FString RunId = Store.FindLatestPendingRun(Evidence.Session);
	const TArray<FKismetAICheckpointInfo> Checkpoints = Store.ListRun(Evidence.Session, RunId);
	TestTrue(TEXT("recovery checkpoint is retained"), Checkpoints.ContainsByPredicate([&](const FKismetAICheckpointInfo& Info)
	{
		return Info.AssetPath == Fixture.Path && Info.bRestorable && Info.bChangedByRun && Info.Status == TEXT("pending");
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDomainMixedFailure, "KismetAI.WidgetDomain.MixedDomainFailureRetainsWidget", KismetAIWidgetDomainTest::Flags)
bool FWidgetDomainMixedFailure::RunTest(const FString& Parameters)
{
	using namespace KismetAIWidgetDomainTest;
	KismetAIPersistence::FScopedEvalModeOverride Autosave(false);
	FFixture Fixture;
	if (!TestTrue(TEXT("fixture saved"), Fixture.bSaved)) return false;
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Panel }, EKismetAIAuthorityOp::Edit, { *Fixture.Mount.Scope() });
	TSharedPtr<FJsonObject> MaterialArgs = MakeShared<FJsonObject>();
	MaterialArgs->SetStringField(TEXT("material"), Fixture.Mount.PackageName(TEXT("MissingMaterial")));
	TSharedPtr<FJsonObject> Delete = MakeShared<FJsonObject>();
	Delete->SetStringField(TEXT("action"), TEXT("delete_node"));
	Delete->SetStringField(TEXT("node"), TEXT("MissingExpression"));
	MaterialArgs->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(Delete) });
	MaterialArgs->SetBoolField(TEXT("atomic"), true);
	const FRunEvidence Evidence = Run({ KismetAIRunEntryHarness::ToolTurn(TEXT("widget-first"), TEXT("edit_widget"), Fixture.Edit()),
		KismetAIRunEntryHarness::ToolTurn(TEXT("material-failure"), TEXT("edit_material"), MaterialArgs), KismetAIRunEntryHarness::TextTurn(TEXT("Material failed; widget retained.")) });
	TestEqual(TEXT("one terminal"), Evidence.Terminals, 1);
	TestEqual(TEXT("earlier domain edit remains"), Fixture.Text(), FString(TEXT("edited")));
	const TSharedPtr<FJsonObject>* Failure = Evidence.Results.Find(TEXT("material-failure"));
	TestEqual(TEXT("second domain actually failed in handler"), Failure ? StringAt(ObjectAt(*Failure, TEXT("error")), TEXT("error_code")) : FString(), FString(TEXT("MATERIAL_NOT_FOUND")));
	const TSharedPtr<FJsonObject>* Edited = Evidence.Results.Find(TEXT("widget-first"));
	const TSharedPtr<FJsonObject> Outcome = Edited ? ObjectAt(ObjectAt(*Edited, TEXT("data")), TEXT("outcome")) : nullptr;
	TestEqual(TEXT("no invented cross-domain rollback"), StringAt(Outcome, TEXT("recovery_status")), FString(TEXT("not_tracked")));
	const TArray<TSharedPtr<FJsonValue>>* Packages = nullptr;
	TestTrue(TEXT("terminal ledger retains first domain effect"), Evidence.Ledger && Evidence.Ledger->TryGetArrayField(TEXT("packages"), Packages)
		&& Packages->ContainsByPredicate([&](const TSharedPtr<FJsonValue>& Value)
		{
			const TSharedPtr<FJsonObject> Entry = Value->AsObject();
			return StringAt(Entry, TEXT("package")) == Fixture.Path && Entry->GetBoolField(TEXT("applied_in_memory"))
				&& StringAt(Entry, TEXT("recovery_status")) == TEXT("not_tracked");
		}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDomainPanelLaunch, "KismetAI.WidgetDomain.PanelLaunch", KismetAIWidgetDomainTest::Flags)
bool FWidgetDomainPanelLaunch::RunTest(const FString& Parameters)
{
	using namespace KismetAIWidgetDomainTest;
	KismetAIPersistence::FScopedEvalModeOverride Autosave(false);
	FFixture Fixture;
	if (!TestTrue(TEXT("fixture saved"), Fixture.bSaved)) return false;
	FScopedModuleSeams Seams(FScopedModuleSeams::EPolicy::Production);
	FKismetAIModule& Module = Seams.Module;
	if (!TestFalse(TEXT("no preexisting run"), FAccess::Session(Module).IsRunning())) return false;
	const FString PreviousSession = Module.GetCurrentSessionId();
	ON_SCOPE_EXIT { Module.SetCurrentSessionId(PreviousSession); };
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Panel }, EKismetAIAuthorityOp::Edit, { *Fixture.Mount.Scope() });
	Seams.Seams->Model.Script = { KismetAIRunEntryHarness::ToolTurn(TEXT("panel-widget"), TEXT("edit_widget"), Fixture.Edit()), KismetAIRunEntryHarness::TextTurn(TEXT("Edited.")) };
	Module.SetCurrentSessionId(FString());
	Module.LaunchAgent(TEXT("widget through panel launch"), FString(), FString(), FString());
	const FString SessionId = Module.GetCurrentSessionId();
	const bool bFinished = PumpUntil([&]() { return !FAccess::Session(Module).IsRunning() && FAccess::Session(Module).NumLiveWorkers() == 0; });
	if (!bFinished)
	{
		FAccess::Session(Module).CancelActive();
		Seams.Seams->Model.Unblock();
		PumpUntil([&]() { return !FAccess::Session(Module).IsRunning() && FAccess::Session(Module).NumLiveWorkers() == 0; });
	}
	TestTrue(TEXT("panel worker completed"), bFinished);
	TestEqual(TEXT("panel applies widget edit"), Fixture.Text(), FString(TEXT("edited")));
	bool bRecorded = false;
	int32 SessionHeaders = 0;
	for (const auto& Entry : FKismetAISessionStorage::LoadAllEntries(SessionId))
		if (Entry->GetStringField(TEXT("type")) == TEXT("session_start")) ++SessionHeaders;
	TestEqual(TEXT("fresh panel flow writes one durable header"), SessionHeaders, 1);
	for (const TSharedPtr<FJsonObject>& Entry : FKismetAISessionStorage::LoadAllEntries(SessionId))
	{
		if (StringAt(Entry, TEXT("tool_use_id")) != TEXT("panel-widget")) continue;
		const TSharedPtr<FJsonObject> Outcome = ObjectAt(Entry, TEXT("mutation_outcome"));
		bRecorded |= Outcome && Outcome->GetBoolField(TEXT("applied_in_memory")) && StringAt(Outcome, TEXT("save_status")) == TEXT("saved");
	}
	TestTrue(TEXT("panel journals actual saved domain result"), bRecorded);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDomainLoopback, "KismetAI.WidgetDomain.Loopback", KismetAIWidgetDomainTest::Flags)
bool FWidgetDomainLoopback::RunTest(const FString& Parameters)
{
	using namespace KismetAIWidgetDomainTest;
	FString Url = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMETAI_TEST_WIDGET_PROVIDER_URL"));
	if (Url.IsEmpty())
	{
		AddInfo(TEXT("Actual curl probe not selected; set KISMETAI_TEST_WIDGET_PROVIDER_URL."));
		return true;
	}
	Url.RemoveFromEnd(TEXT("/"));
	const FString Prefix = TEXT("http://127.0.0.1:");
	const FString Port = Url.Mid(Prefix.Len());
	if (!Url.StartsWith(Prefix) || !Port.IsNumeric() || FCString::Atoi(*Port) < 1 || FCString::Atoi(*Port) > 65535)
	{
		AddError(TEXT("Widget fixture requires an explicit loopback port"));
		return false;
	}
	KismetAIPersistence::FScopedEvalModeOverride Autosave(false);
	FFixture Fixture;
	if (!TestTrue(TEXT("fixture saved"), Fixture.bSaved)) return false;
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Panel }, EKismetAIAuthorityOp::Edit, { *Fixture.Mount.Scope() });
	const FKismetAIAssetFingerprint Before = KismetAIAssetState::Capture(Fixture.Path);
	int32 Saves = 0;
	KismetAIPersistence::FScopedSaveFunctionOverride FailSave([&](UPackage*, UObject*, const FString&, FString& Error)
	{
		++Saves; Error = TEXT("injected loopback disk full"); return false;
	});
	AddExpectedMessage(TEXT("Save failed for"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	FKismetAIAgentConfig Config;
	Config.ApiBaseUrl = Url;
	Config.ApiKey = TEXT("unused-loopback-key");
	Config.Model = TEXT("claude-sonnet-4-6");
	Config.MaxSteps = 5;
	Config.MaxRetries = 0;
	Config.ThinkingMode = EThinkingMode::Disabled;
	FKismetAIAgent Agent(MakeUnique<FKismetAIModuleToolInvoker>(FKismetAIModule::Get()), Config);
	Agent.SetCallerContext(FKismetAICallerContext::Verified(EKismetAIEntryPoint::Panel, TEXT("widget-domain-test")));
	TSharedPtr<FJsonObject> Prompt = MakeShared<FJsonObject>();
	Prompt->SetStringField(TEXT("widget"), Fixture.Blueprint->GetPathName());
	Prompt->SetStringField(TEXT("missing_material"), Fixture.Mount.PackageName(TEXT("MissingMaterial")));
	TSharedPtr<FJsonObject> Ledger;
	bool bMaterialFailed = false;
	int32 Terminals = 0;
	FOnAgentStep Step;
	Step.BindLambda([&](const FString& Type, const FString& Text)
	{
		const TSharedPtr<FJsonObject> Event = Parse(Text);
		if (Type == TEXT("tool_result"))
		{
			TSharedPtr<FJsonObject> Result = ObjectAt(Event, TEXT("result"));
			if (!Result) Result = Parse(StringAt(Event, TEXT("result")));
			bMaterialFailed |= StringAt(ObjectAt(Result, TEXT("error")), TEXT("error_code")) == TEXT("MATERIAL_NOT_FOUND");
		}
		if (Type == TEXT("mutation_outcomes")) Ledger = Event;
		if (Type == TEXT("done") || Type == TEXT("error")) ++Terminals;
	});
	Agent.RunTask(TEXT("WidgetFixture: ") + Json(Prompt), Step);
	TestEqual(TEXT("one terminal from actual curl run"), Terminals, 1);
	TestEqual(TEXT("actual provider response reached widget service"), Fixture.Text(), FString(TEXT("edited")));
	TestEqual(TEXT("one shared autosave attempt"), Saves, 1);
	TestTrue(TEXT("second domain failed without reverting widget"), bMaterialFailed);
	TestTrue(TEXT("failed save did not alter disk"), Before.SameFile(KismetAIAssetState::Capture(Fixture.Path)));
	const TArray<TSharedPtr<FJsonValue>>* Unsaved = nullptr;
	TestTrue(TEXT("actual run reports retained unsaved edit"), Ledger && Ledger->TryGetArrayField(TEXT("unsaved_packages"), Unsaved)
		&& Unsaved->ContainsByPredicate([&](const TSharedPtr<FJsonValue>& Value) { return Value->AsString() == Fixture.Path; }));
	return true;
}
