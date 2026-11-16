// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Checkpoints that can be restored, and restoration that never claims
// more than it did. Fixtures are disposable, file-backed Blueprints under
// /Game/__KismetAITest_P111, created and deleted by each test; checkpoints go
// to the module's store under a session id unique to the test and are deleted
// with it.
//
// The first two tests were written against the older Reject (in-memory
// baseline, delete added nodes by name, put pin defaults back) and failed
// there: a node the agent deleted stayed deleted, and a human edit made after
// the run was overwritten.

#include "Misc/AutomationTest.h"
#include "KismetAIModule.h"
#include "KismetAIAgent.h"
#include "KismetAIAssetState.h"
#include "KismetAICheckpoints.h"
#include "KismetAIModelClientInterface.h"
#include "KismetAIModuleToolInvoker.h"
#include "KismetAIMutationIntent.h"
#include "KismetAIPersistence.h"
#include "KismetAISessionStorage.h"
#include "KismetAIToolCallJournal.h"
#include "KismetAIToolInvoker.h"
#include "DisposableContent.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "PackageTools.h"
#include "BlueprintActionDatabase.h"
#include "DiffUtils.h"
#include "Misc/PackagePath.h"
#include "Editor.h"
#include "UObject/UObjectHash.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

constexpr auto CheckpointRecoveryTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace CheckpointRecoveryTest
{
	FString DisposablePackageName(const TCHAR* Stem)
	{
		return FString::Printf(TEXT("/Game/__KismetAITest_P111/%s_%s"), Stem,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	}

	FString UniqueId(const TCHAR* Stem)
	{
		return FString::Printf(TEXT("p111-%s-%s"), Stem, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12));
	}

	UBlueprint* Find(const FString& PackagePath)
	{
		UPackage* Pkg = FindPackage(nullptr, *PackagePath);
		return Pkg ? Cast<UBlueprint>(Pkg->FindAssetInPackage()) : nullptr;
	}

	UEdGraph* EventGraph(UBlueprint* BP)
	{
		return BP && BP->UbergraphPages.Num() > 0 ? BP->UbergraphPages[0] : nullptr;
	}

	UK2Node_CallFunction* AddPrint(UEdGraph* Graph, const FString& Text, int32 X)
	{
		UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
		Call->SetFlags(RF_Transactional);
		Call->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString), UKismetSystemLibrary::StaticClass());
		Graph->AddNode(Call, false, false);
		Call->CreateNewGuid();
		Call->NodePosX = X;
		Call->AllocateDefaultPins();
		Call->PostPlacedNewNode();  // as a placed node: sets EnabledState for development-only functions
		if (UEdGraphPin* In = Call->FindPin(TEXT("InString")))
		{
			In->DefaultValue = Text;
		}
		return Call;
	}

	UEdGraphNode* FindByGuid(UBlueprint* BP, const FGuid& Guid)
	{
		if (UEdGraph* Graph = EventGraph(BP))
		{
			for (UEdGraphNode* N : Graph->Nodes)
			{
				if (N && N->NodeGuid == Guid) return N;
			}
		}
		return nullptr;
	}

	FString PinDefault(UEdGraphNode* Node)
	{
		UEdGraphPin* P = Node ? Node->FindPin(TEXT("InString")) : nullptr;
		return P ? P->DefaultValue : FString(TEXT("<no node>"));
	}

	void SetPin(UBlueprint* BP, const FGuid& Guid, const FString& Value)
	{
		if (UEdGraphNode* N = FindByGuid(BP, Guid))
		{
			N->FindPin(TEXT("InString"))->DefaultValue = Value;
		}
	}

	bool HasVariable(const UBlueprint* BP, const FName Name)
	{
		return BP && BP->NewVariables.ContainsByPredicate([Name](const FBPVariableDescription& V) { return V.VarName == Name; });
	}

	bool SaveToDisk(UBlueprint* BP)
	{
		UPackage* Pkg = BP->GetOutermost();
		const FString File = FPackageName::LongPackageNameToFilename(Pkg->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Pkg, BP, *File, Args);
	}

	FString FileSha(const FString& PackagePath)
	{
		FString Sha;
		int64 Size = -1;
		KismetAIAssetState::HashFile(KismetAIAssetState::PackageFilename(PackagePath), Sha, Size);
		return Sha;
	}

	// Retire a Blueprint the way asset deletion does for what matters here:
	// its node-spawners leave the action database, and its generated and
	// skeleton classes go with it. A skeleton class left alive after its
	// Blueprint is collected crashes the next insert_node (its spawners ask the
	// class for its Blueprint) or package reload (UPackageTools rebuilds every
	// loaded Blueprint's dependencies).
	void Retire(UBlueprint* BP)
	{
		if (!BP) return;
		FBlueprintActionDatabase::Get().ClearAssetActions(BP);
		for (UClass* Class : { BP->GeneratedClass.Get(), BP->SkeletonGeneratedClass.Get() })
		{
			if (Class)
			{
				Class->ClearFlags(RF_Public | RF_Standalone);
				Class->MarkAsGarbage();
			}
		}
		BP->ClearFlags(RF_Public | RF_Standalone);
		BP->MarkAsGarbage();
	}

	// These tests reload packages. Disposed fixtures — here and in other test
	// files — can still be held by the undo buffer, alive past collection while
	// their skeleton classes lose them. Retire every garbage Blueprint, drop the
	// undo history and collect, so only live Blueprints remain.
	void PurgeLeftovers()
	{
		TArray<UObject*> Blueprints;
		GetObjectsOfClass(UBlueprint::StaticClass(), Blueprints);
		for (UObject* Object : Blueprints)
		{
			UBlueprint* BP = Cast<UBlueprint>(Object);
			if (BP && !IsValid(BP))
			{
				Retire(BP);
			}
		}
		if (GEditor)
		{
			GEditor->ResetTransaction(FText::FromString(TEXT("KismetAI test fixture")));
		}
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	}

	// A file-backed Blueprint with one Print String ("Hello"), saved, compiled.
	struct FFixture
	{
		FString Path;
		FGuid PrintGuid;
		FString Session;
		FString Run;

		explicit FFixture(const TCHAR* Stem)
		{
			PurgeLeftovers();
			Path = DisposablePackageName(Stem);
			Session = UniqueId(TEXT("session"));
			Run = UniqueId(TEXT("run"));
			UPackage* Pkg = CreatePackage(*Path);
			UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Pkg,
				FName(*FPackageName::GetShortName(Path)), BPTYPE_Normal,
				UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
			PrintGuid = AddPrint(EventGraph(BP), TEXT("Hello"), 0)->NodeGuid;
			FKismetEditorUtilities::CompileBlueprint(BP);
			SaveToDisk(BP);
		}

		~FFixture()
		{
			if (UBlueprint* BP = Find(Path))
			{
				UPackage* Pkg = BP->GetOutermost();
				ResetLoaders(Pkg);
				Retire(BP);
				Pkg->SetDirtyFlag(false);
				Pkg->MarkAsGarbage();
			}
			PurgeLeftovers();
			IFileManager::Get().Delete(*FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension()), false, true, true);
			// The last fixture out removes /Game/__KismetAITest_P111 too.
			KismetAIDisposableContent::PruneEmptyFixtureFolders(FPackageName::GetLongPackagePath(Path));
			FKismetAIModule& Mod = FKismetAIModule::Get();
			IFileManager::Get().DeleteDirectory(*(Mod.GetCheckpointStore().GetRoot() / Session), false, true);
		}

		UBlueprint* BP() const { return Find(Path); }

		FKismetAIMutationContext Context(const FString& InRun = FString()) const
		{
			FKismetAIMutationContext C;
			C.SessionId = Session;
			C.RunId = InRun.IsEmpty() ? Run : InRun;
			C.CallId = C.RunId + TEXT("/t0/c0");
			C.ToolName = TEXT("edit_blueprint");
			C.Targets = { Path };
			return C;
		}

		// One agent call: before-state recorded, Edit applied, after-state recorded.
		FKismetAIAssetFingerprint AgentCall(TFunctionRef<void(UBlueprint*)> Edit, const FString& InRun = FString()) const
		{
			FKismetAIModule& Mod = FKismetAIModule::Get();
			const TArray<FKismetAIAssetFingerprint> Before = Mod.BeginAgentMutation(Context(InRun));
			Edit(BP());
			Mod.EndAgentMutation(Context(InRun));
			return Before.Num() == 1 ? Before[0] : FKismetAIAssetFingerprint();
		}

		FString AssetDir(const FString& InRun = FString()) const
		{
			return FKismetAIModule::Get().GetCheckpointStore().AssetDirectory(Session, InRun.IsEmpty() ? Run : InRun, Path);
		}
	};

	TSharedPtr<FJsonObject> ReadJson(const FString& File)
	{
		FString Text;
		TSharedPtr<FJsonObject> Obj;
		if (FFileHelper::LoadFileToString(Text, *File))
		{
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			FJsonSerializer::Deserialize(Reader, Obj);
		}
		return Obj;
	}

	void WriteJson(const FString& File, const TSharedPtr<FJsonObject>& Obj)
	{
		FString Text;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		FFileHelper::SaveStringToFile(Text, *File);
	}

	FString Statuses(const TArray<FKismetAIRestoreReport>& Reports)
	{
		TArray<FString> Out;
		for (const FKismetAIRestoreReport& R : Reports)
		{
			Out.Add(FString::Printf(TEXT("%s: %s (%s)"), *R.AssetPath, FKismetAIRestoreReport::StatusToString(R.Status), *R.Explanation));
		}
		return FString::Join(Out, TEXT(" | "));
	}

	// Activates a session in the module for the panel-facing calls and puts
	// the previous one back.
	struct FActiveSession
	{
		FString Previous;
		explicit FActiveSession(const FString& Session)
		{
			Previous = FKismetAIModule::Get().GetCurrentSessionId();
			FKismetAIModule::Get().SetCurrentSessionId(Session);
		}
		~FActiveSession() { FKismetAIModule::Get().SetCurrentSessionId(Previous); }
	};
}

// ─────────────────────────────────────────────────────────────────────────────
// The fingerprint is only evidence if an unchanged asset keeps it: through a
// compile, through a copy of its file loaded under another name, and through a
// reload from disk.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCheckpointRecovery_FingerprintIsStable,
	"KismetAI.CheckpointRecovery.FingerprintIsStable", CheckpointRecoveryTestFlags)
bool FCheckpointRecovery_FingerprintIsStable::RunTest(const FString& Parameters)
{
	using namespace CheckpointRecoveryTest;
	FFixture F(TEXT("BP_Stable"));
	const FKismetAIAssetFingerprint A = KismetAIAssetState::Capture(F.Path);
	TestTrue(TEXT("captured with evidence"), A.HasStateEvidence());
	TestTrue(TEXT("file recorded"), A.bFileExists && !A.FileSha1.IsEmpty());
	TestTrue(TEXT("same state twice"), A.SameState(KismetAIAssetState::Capture(F.Path)));

	FKismetEditorUtilities::CompileBlueprint(F.BP());
	const FKismetAIAssetFingerprint B = KismetAIAssetState::Capture(F.Path);
	TestTrue(TEXT("a compile does not change the described state"), A.SameState(B));
	if (!A.SameState(B))
	{
		AddInfo(FString::Join(FKismetAIAssetFingerprint::DescribeDifferences(A, B), TEXT("; ")));
	}

	const FKismetAIAssetFingerprint OnDisk = KismetAIAssetState::CaptureFile(KismetAIAssetState::PackageFilename(F.Path), F.Path);
	TestTrue(TEXT("a copy of the file describes the same state"), A.SameState(OnDisk));
	if (!A.SameState(OnDisk))
	{
		AddInfo(FString::Join(FKismetAIAssetFingerprint::DescribeDifferences(A, OnDisk), TEXT("; ")));
	}

	const TArray<FString> NodeBefore = KismetAIAssetState::DescribeObject(FindByGuid(F.BP(), F.PrintGuid));
	const TArray<FString> CdoBefore = KismetAIAssetState::DescribeObject(F.BP()->GeneratedClass->GetDefaultObject());
	FText Error;
	UPackageTools::ReloadPackages({ F.BP()->GetOutermost() }, Error, EReloadPackagesInteractionMode::AssumePositive);
	const FKismetAIAssetFingerprint Reloaded = KismetAIAssetState::Capture(F.Path);
	{
		const TArray<FString> NodeAfter = KismetAIAssetState::DescribeObject(FindByGuid(F.BP(), F.PrintGuid));
		const TArray<FString> CdoAfter = KismetAIAssetState::DescribeObject(F.BP()->GeneratedClass->GetDefaultObject());
		for (const FString& L : NodeBefore) { if (!NodeAfter.Contains(L)) AddInfo(TEXT("node before only: ") + L.Left(300)); }
		for (const FString& L : NodeAfter) { if (!NodeBefore.Contains(L)) AddInfo(TEXT("node after only: ") + L.Left(300)); }
		for (const FString& L : CdoBefore) { if (!CdoAfter.Contains(L)) AddInfo(TEXT("cdo before only: ") + L.Left(300)); }
		for (const FString& L : CdoAfter) { if (!CdoBefore.Contains(L)) AddInfo(TEXT("cdo after only: ") + L.Left(300)); }
	}
	TestTrue(TEXT("a reload from disk describes the same state"), A.SameState(Reloaded));
	if (!A.SameState(Reloaded))
	{
		AddInfo(FString::Join(FKismetAIAssetFingerprint::DescribeDifferences(A, Reloaded), TEXT("; ")));
	}

	SetPin(F.BP(), F.PrintGuid, TEXT("Changed"));
	const FKismetAIAssetFingerprint C = KismetAIAssetState::Capture(F.Path);
	TestFalse(TEXT("a pin default change is visible"), A.SameState(C));
	const TArray<FString> Diff = FKismetAIAssetFingerprint::DescribeDifferences(A, C);
	TestTrue(TEXT("and names the node"), Diff.ContainsByPredicate([](const FString& D) { return D.StartsWith(TEXT("changed: ")) && D.Contains(TEXT("K2Node_CallFunction")); }));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Reproduction 1 (fails older): Reject brings back what the run deleted,
// removes what it added, and puts back what it changed — node, link-free pin
// default and a variable — in memory and on disk. Reapply puts the run's
// result back.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCheckpointRecovery_RejectRestoresDeletedNode,
	"KismetAI.CheckpointRecovery.RejectRestoresDeletedNode", CheckpointRecoveryTestFlags)
bool FCheckpointRecovery_RejectRestoresDeletedNode::RunTest(const FString& Parameters)
{
	using namespace CheckpointRecoveryTest;
	FKismetAIModule& Mod = FKismetAIModule::Get();
	FFixture F(TEXT("BP_Deleted"));
	FActiveSession Active(F.Session);
	Mod.TakeSessionBaseline(F.Session, F.BP()->GetPathName(), EventGraph(F.BP())->GetName());
	const FString FileBefore = FileSha(F.Path);

	FGuid AgentNode;
	const FKismetAIAssetFingerprint Before = F.AgentCall([&](UBlueprint* BP)
	{
		UEdGraph* Graph = EventGraph(BP);
		Graph->RemoveNode(FindByGuid(BP, F.PrintGuid));
		AgentNode = AddPrint(Graph, TEXT("Agent"), 400)->NodeGuid;
		FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("P111Flag"), FEdGraphPinType(UEdGraphSchema_K2::PC_Boolean, NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType()));
		// The autosave a production run makes.
		SaveToDisk(BP);
	});
	TestTrue(TEXT("the checkpoint manifest exists"), IFileManager::Get().FileExists(*(F.AssetDir() / TEXT("manifest.json"))));
	TestTrue(TEXT("the checkpoint file exists"), IFileManager::Get().FileExists(*(F.AssetDir() / TEXT("checkpoint.uasset"))));
	TestNotEqual(TEXT("the run's autosave changed the file"), FileSha(F.Path), FileBefore);

	Mod.FinalizeRunDiff(F.Run);
	TestTrue(TEXT("Reject is offered"), Mod.HasPendingDiff());
	Mod.RejectChanges();

	UBlueprint* BP = F.BP();
	TestNotNull(TEXT("the node the agent deleted is back after Reject"), FindByGuid(BP, F.PrintGuid));
	TestEqual(TEXT("with its pin default"), PinDefault(FindByGuid(BP, F.PrintGuid)), FString(TEXT("Hello")));
	TestNull(TEXT("the node the agent added is gone"), FindByGuid(BP, AgentNode));
	TestFalse(TEXT("the variable the agent added is gone"), HasVariable(BP, TEXT("P111Flag")));
	TestFalse(TEXT("no pending diff after a full restore"), Mod.HasPendingDiff());
	TestTrue(TEXT("Reapply is offered"), Mod.CanReapply());

	const FKismetAIAssetFingerprint Memory = KismetAIAssetState::Capture(F.Path);
	TestTrue(TEXT("memory equals the checkpoint"), Memory.SameState(Before));
	const FKismetAIAssetFingerprint Disk = KismetAIAssetState::CaptureFile(KismetAIAssetState::PackageFilename(F.Path), F.Path);
	TestTrue(TEXT("the file on disk equals the checkpoint"), Disk.SameState(Before));
	TestFalse(TEXT("the reloaded package is clean"), BP->GetOutermost()->IsDirty());

	Mod.ReapplyChanges();
	BP = F.BP();
	TestNull(TEXT("Reapply removes the node again"), FindByGuid(BP, F.PrintGuid));
	TestNotNull(TEXT("Reapply brings the agent's node back"), FindByGuid(BP, AgentNode));
	TestTrue(TEXT("and its variable"), HasVariable(BP, TEXT("P111Flag")));
	TestTrue(TEXT("Reject is offered again"), Mod.HasPendingDiff());

	Mod.AcceptChanges();
	TestFalse(TEXT("Accept deletes the checkpoint file"), IFileManager::Get().FileExists(*(F.AssetDir() / TEXT("checkpoint.uasset"))));
	const TSharedPtr<FJsonObject> Manifest = ReadJson(F.AssetDir() / TEXT("manifest.json"));
	TestTrue(TEXT("the manifest stays as a receipt"), Manifest.IsValid() && Manifest->GetStringField(TEXT("status")) == TEXT("accepted"));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Reproduction 2 (fails older): a human edit made after the run survives
// Reject. The restore is refused with the evidence; nothing is written.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCheckpointRecovery_RejectPreservesLaterHumanEdit,
	"KismetAI.CheckpointRecovery.RejectPreservesLaterHumanEdit", CheckpointRecoveryTestFlags)
bool FCheckpointRecovery_RejectPreservesLaterHumanEdit::RunTest(const FString& Parameters)
{
	using namespace CheckpointRecoveryTest;
	FKismetAIModule& Mod = FKismetAIModule::Get();
	FFixture F(TEXT("BP_Human"));
	FActiveSession Active(F.Session);
	Mod.TakeSessionBaseline(F.Session, F.BP()->GetPathName(), EventGraph(F.BP())->GetName());

	F.AgentCall([&](UBlueprint* BP) { SetPin(BP, F.PrintGuid, TEXT("Agent")); });
	Mod.FinalizeRunDiff(F.Run);

	// The user edits the same pin after the run, then presses Reject.
	SetPin(F.BP(), F.PrintGuid, TEXT("Human"));
	const FString FileBefore = FileSha(F.Path);
	Mod.RejectChanges();

	TestEqual(TEXT("the human edit made after the run survives Reject"), PinDefault(FindByGuid(F.BP(), F.PrintGuid)), FString(TEXT("Human")));
	TestEqual(TEXT("the file was not touched"), FileSha(F.Path), FileBefore);
	TestTrue(TEXT("the run's changes are still pending"), Mod.HasPendingDiff());

	const TArray<FKismetAIRestoreReport> Reports = Mod.RestoreRunCheckpoints(F.Session, F.Run, false);
	TestTrue(TEXT("reported as a conflict"), Reports.Num() == 1 && Reports[0].Status == EKismetAIRestoreStatus::Conflict);
	TestTrue(TEXT("with the changed node as evidence"), Reports.Num() == 1
		&& Reports[0].Evidence.ContainsByPredicate([](const FString& E) { return E.Contains(TEXT("K2Node_CallFunction")); }));
	return true;
}

// A change made between two of the run's calls (by a person, during the run)
// blocks the restore too: restoring would discard it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCheckpointRecovery_EditBetweenCallsBlocksRestore,
	"KismetAI.CheckpointRecovery.EditBetweenCallsBlocksRestore", CheckpointRecoveryTestFlags)
bool FCheckpointRecovery_EditBetweenCallsBlocksRestore::RunTest(const FString& Parameters)
{
	using namespace CheckpointRecoveryTest;
	FKismetAIModule& Mod = FKismetAIModule::Get();
	FFixture F(TEXT("BP_Between"));
	F.AgentCall([&](UBlueprint* BP) { SetPin(BP, F.PrintGuid, TEXT("A1")); });
	const FGuid Human = AddPrint(EventGraph(F.BP()), TEXT("Mine"), 800)->NodeGuid;
	F.AgentCall([&](UBlueprint* BP) { SetPin(BP, F.PrintGuid, TEXT("A2")); });

	const TSharedPtr<FJsonObject> Manifest = ReadJson(F.AssetDir() / TEXT("manifest.json"));
	TestTrue(TEXT("the change is noted as made outside the run"), Manifest.IsValid() && Manifest->GetBoolField(TEXT("foreign_change")));

	const TArray<FKismetAIRestoreReport> Reports = Mod.RestoreRunCheckpoints(F.Session, F.Run, false);
	TestTrue(TEXT("conflict"), Reports.Num() == 1 && Reports[0].Status == EKismetAIRestoreStatus::Conflict);
	TestTrue(TEXT("evidence names the added node"), Reports.Num() == 1
		&& Reports[0].Evidence.ContainsByPredicate([](const FString& E) { return E.StartsWith(TEXT("added: ")); }));
	TestNotNull(TEXT("the human's node is still there"), FindByGuid(F.BP(), Human));
	TestEqual(TEXT("the run's last edit is still there"), PinDefault(FindByGuid(F.BP(), F.PrintGuid)), FString(TEXT("A2")));
	return true;
}

// A checkpoint from an earlier run is stale once a later run changed the asset.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCheckpointRecovery_StaleCheckpointIsRefused,
	"KismetAI.CheckpointRecovery.StaleCheckpointIsRefused", CheckpointRecoveryTestFlags)
bool FCheckpointRecovery_StaleCheckpointIsRefused::RunTest(const FString& Parameters)
{
	using namespace CheckpointRecoveryTest;
	FKismetAIModule& Mod = FKismetAIModule::Get();
	FFixture F(TEXT("BP_Stale"));
	const FString Run2 = UniqueId(TEXT("run2"));
	F.AgentCall([&](UBlueprint* BP) { SetPin(BP, F.PrintGuid, TEXT("R1")); });
	F.AgentCall([&](UBlueprint* BP) { SetPin(BP, F.PrintGuid, TEXT("R2")); }, Run2);

	const TArray<FKismetAIRestoreReport> Old = Mod.RestoreRunCheckpoints(F.Session, F.Run, false);
	TestTrue(TEXT("the older run's checkpoint is refused"), Old.Num() == 1 && Old[0].Status == EKismetAIRestoreStatus::Conflict);
	TestEqual(TEXT("the later run's result stays"), PinDefault(FindByGuid(F.BP(), F.PrintGuid)), FString(TEXT("R2")));

	const TArray<FKismetAIRestoreReport> Latest = Mod.RestoreRunCheckpoints(F.Session, Run2, false);
	TestTrue(TEXT("the latest run restores"), Latest.Num() == 1 && Latest[0].Succeeded());
	TestEqual(TEXT("to its own checkpoint"), PinDefault(FindByGuid(F.BP(), F.PrintGuid)), FString(TEXT("R1")));
	AddInfo(Statuses(Latest));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Missing, corrupt and incompatible checkpoints: reported as such, nothing
// touched, never "restored".
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCheckpointRecovery_DamagedCheckpointsAreReported,
	"KismetAI.CheckpointRecovery.DamagedCheckpointsAreReported", CheckpointRecoveryTestFlags)
bool FCheckpointRecovery_DamagedCheckpointsAreReported::RunTest(const FString& Parameters)
{
	using namespace CheckpointRecoveryTest;
	FKismetAIModule& Mod = FKismetAIModule::Get();
	FFixture F(TEXT("BP_Damaged"));
	F.AgentCall([&](UBlueprint* BP) { SetPin(BP, F.PrintGuid, TEXT("Agent")); SaveToDisk(BP); });
	const FKismetAIAssetFingerprint AgentState = KismetAIAssetState::Capture(F.Path);
	const FString Dir = F.AssetDir();
	const FString CheckpointFile = Dir / TEXT("checkpoint.uasset");
	const FString ManifestFile = Dir / TEXT("manifest.json");
	TArray<uint8> GoodBytes;
	FFileHelper::LoadFileToArray(GoodBytes, *CheckpointFile);
	const TSharedPtr<FJsonObject> GoodManifest = ReadJson(ManifestFile);

	auto Untouched = [&](const TCHAR* What)
	{
		TestTrue(FString::Printf(TEXT("%s: the asset is untouched"), What), KismetAIAssetState::Capture(F.Path).SameState(AgentState));
		TestEqual(FString::Printf(TEXT("%s: the file is untouched"), What), FileSha(F.Path), AgentState.FileSha1);
	};
	auto RestoreOnce = [&]() { return Mod.RestoreRunCheckpoints(F.Session, F.Run, false); };

	{
		IFileManager::Get().Delete(*CheckpointFile);
		const TArray<FKismetAIRestoreReport> R = RestoreOnce();
		TestTrue(TEXT("deleted file: checkpoint_unavailable"), R.Num() == 1 && R[0].Status == EKismetAIRestoreStatus::CheckpointUnavailable);
		Untouched(TEXT("deleted file"));
	}
	{
		TArray<uint8> Bad = GoodBytes;
		Bad[Bad.Num() / 2] ^= 0x5A;
		FFileHelper::SaveArrayToFile(Bad, *CheckpointFile);
		const TArray<FKismetAIRestoreReport> R = RestoreOnce();
		TestTrue(TEXT("flipped byte: checkpoint_corrupt"), R.Num() == 1 && R[0].Status == EKismetAIRestoreStatus::CheckpointCorrupt);
		Untouched(TEXT("flipped byte"));
	}
	{
		// Bytes that match a (forged) hash but are not the package: loading
		// them for verification fails, so the state cannot be proven.
		AddExpectedError(TEXT("is invalid. Check that the file is of the expected type"), EAutomationExpectedErrorFlags::Contains, 0);
		TArray<uint8> Garbage;
		Garbage.Init(0x42, 4096);
		FFileHelper::SaveArrayToFile(Garbage, *CheckpointFile);
		FString Sha;
		int64 Size = -1;
		KismetAIAssetState::HashFile(CheckpointFile, Sha, Size);
		const TSharedPtr<FJsonObject> Forged = ReadJson(ManifestFile);
		Forged->GetObjectField(TEXT("checkpoint"))->SetStringField(TEXT("sha1"), Sha);
		WriteJson(ManifestFile, Forged);
		const TArray<FKismetAIRestoreReport> R = RestoreOnce();
		TestTrue(TEXT("unloadable bytes: checkpoint_unverifiable"), R.Num() == 1 && R[0].Status == EKismetAIRestoreStatus::CheckpointUnverifiable);
		Untouched(TEXT("unloadable bytes"));
	}
	FFileHelper::SaveArrayToFile(GoodBytes, *CheckpointFile);
	{
		FFileHelper::SaveStringToFile(TEXT("{ not json"), *ManifestFile);
		const TArray<FKismetAIRestoreReport> R = RestoreOnce();
		TestTrue(TEXT("garbled manifest: corrupt"), R.Num() == 1 && R[0].Status == EKismetAIRestoreStatus::CheckpointCorrupt);
		Untouched(TEXT("garbled manifest"));
	}
	{
		const TSharedPtr<FJsonObject> Other = MakeShared<FJsonObject>();
		Other->Values = GoodManifest->Values;
		Other->SetStringField(TEXT("engine_version"), TEXT("4.27.2-0+++UE4+Release-4.27"));
		WriteJson(ManifestFile, Other);
		const TArray<FKismetAIRestoreReport> R = RestoreOnce();
		TestTrue(TEXT("other engine: incompatible"), R.Num() == 1 && R[0].Status == EKismetAIRestoreStatus::CheckpointIncompatible);
		Untouched(TEXT("other engine"));
	}
	{
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
		const TArray<FKismetAIRestoreReport> R = RestoreOnce();
		TestTrue(TEXT("no checkpoint at all: unavailable"), R.Num() == 1 && R[0].Status == EKismetAIRestoreStatus::CheckpointUnavailable);
		TestFalse(TEXT("never claims success"), R.Num() == 1 && R[0].Succeeded());
		Untouched(TEXT("no checkpoint"));
	}

	// The good checkpoint still restores once it is back.
	WriteJson(ManifestFile, GoodManifest);
	FFileHelper::SaveArrayToFile(GoodBytes, *CheckpointFile);
	const TArray<FKismetAIRestoreReport> R = RestoreOnce();
	TestTrue(TEXT("intact again: restored"), R.Num() == 1 && R[0].Succeeded());
	AddInfo(Statuses(R));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Adversarial: the save during restoration fails. Memory and disk are stated;
// "restored" is never reported.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCheckpointRecovery_RestoreFailureIsVisible,
	"KismetAI.CheckpointRecovery.RestoreFailureIsVisible", CheckpointRecoveryTestFlags)
bool FCheckpointRecovery_RestoreFailureIsVisible::RunTest(const FString& Parameters)
{
	using namespace CheckpointRecoveryTest;
	FKismetAIModule& Mod = FKismetAIModule::Get();
	FFixture F(TEXT("BP_Fail"));
	const FKismetAIAssetFingerprint Before = F.AgentCall([&](UBlueprint* BP) { SetPin(BP, F.PrintGuid, TEXT("Agent")); SaveToDisk(BP); });
	const FKismetAIAssetFingerprint AgentState = KismetAIAssetState::Capture(F.Path);
	FKismetAICheckpointStore& Store = Mod.GetCheckpointStore();

	{
		FKismetAICheckpointStore::FScopedFault Fault(FKismetAICheckpointStore::EFault::WriteFails);
		const FKismetAIRestoreReport R = Store.Restore(F.Session, F.Run, F.Path);
		TestTrue(TEXT("write fails: restore_failed"), R.Status == EKismetAIRestoreStatus::RestoreFailed);
		TestTrue(TEXT("memory still holds the run's result"), KismetAIAssetState::Capture(F.Path).SameState(AgentState));
		TestEqual(TEXT("disk still holds the run's result"), FileSha(F.Path), AgentState.FileSha1);
		TestTrue(TEXT("the report says so"), R.MemoryState.Contains(TEXT("run's result")) && R.DiskState == TEXT("unchanged"));
	}
	{
		FKismetAICheckpointStore::FScopedFault Fault(FKismetAICheckpointStore::EFault::ReloadFails);
		const FKismetAIRestoreReport R = Store.Restore(F.Session, F.Run, F.Path);
		TestTrue(TEXT("reload fails: restore_failed"), R.Status == EKismetAIRestoreStatus::RestoreFailed);
		TestEqual(TEXT("the previous file was put back"), FileSha(F.Path), AgentState.FileSha1);
		TestTrue(TEXT("memory untouched"), KismetAIAssetState::Capture(F.Path).SameState(AgentState));
	}
	{
		FKismetAICheckpointStore::FScopedFault Fault(FKismetAICheckpointStore::EFault::ReloadAndRollbackFail);
		const FKismetAIRestoreReport R = Store.Restore(F.Session, F.Run, F.Path);
		TestTrue(TEXT("reload and rollback fail: unresolved"), R.Status == EKismetAIRestoreStatus::Unresolved);
		TestFalse(TEXT("not reported as restored"), R.Succeeded());
		const FKismetAIAssetFingerprint Memory = KismetAIAssetState::Capture(F.Path);
		const FKismetAIAssetFingerprint Disk = KismetAIAssetState::CaptureFile(KismetAIAssetState::PackageFilename(F.Path), F.Path);
		TestTrue(TEXT("memory holds the run's result"), Memory.SameState(AgentState));
		TestTrue(TEXT("disk holds the checkpoint"), Disk.SameState(Before));
		TestTrue(TEXT("the report states both sides"), R.MemoryState.Contains(TEXT("run's result")) && R.DiskState == TEXT("checkpoint"));
		TestTrue(TEXT("and that they differ"), R.Explanation.Contains(TEXT("they differ")));
		const TSharedPtr<FJsonObject> Manifest = ReadJson(F.AssetDir() / TEXT("manifest.json"));
		TestTrue(TEXT("the manifest records it"), Manifest.IsValid() && Manifest->GetStringField(TEXT("status")) == TEXT("unresolved"));
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Recovery after a restart: the checkpoint is found from disk by a fresh store
// and restores; and when the run's edits were never saved, the asset that
// loads is already the checkpoint, which is proven rather than overwritten.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCheckpointRecovery_RecoversAfterRestart,
	"KismetAI.CheckpointRecovery.RecoversAfterRestart", CheckpointRecoveryTestFlags)
bool FCheckpointRecovery_RecoversAfterRestart::RunTest(const FString& Parameters)
{
	using namespace CheckpointRecoveryTest;
	FKismetAIModule& Mod = FKismetAIModule::Get();
	{
		FFixture F(TEXT("BP_Restart"));
		const FKismetAIAssetFingerprint Before = F.AgentCall([&](UBlueprint* BP) { SetPin(BP, F.PrintGuid, TEXT("Saved")); SaveToDisk(BP); });
		// "Restart": the live package is replaced by what is on disk, and a
		// new store knows only what is in the checkpoint folder.
		FText Error;
		UPackageTools::ReloadPackages({ F.BP()->GetOutermost() }, Error, EReloadPackagesInteractionMode::AssumePositive);
		FKismetAICheckpointStore Fresh(Mod.GetCheckpointStore().GetRoot());
		TestTrue(TEXT("the pending checkpoint is found"), Fresh.ListPending().ContainsByPredicate([&F](const FKismetAICheckpointInfo& I) { return I.AssetPath == F.Path && I.SessionId == F.Session; }));
		TestEqual(TEXT("its run is the session's latest"), Fresh.FindLatestPendingRun(F.Session), F.Run);
		const FKismetAIRestoreReport R = Fresh.Restore(F.Session, F.Run, F.Path);
		TestTrue(TEXT("restored after restart"), R.Succeeded());
		TestEqual(TEXT("to the pre-run value"), PinDefault(FindByGuid(F.BP(), F.PrintGuid)), FString(TEXT("Hello")));
		TestTrue(TEXT("verified"), KismetAIAssetState::Capture(F.Path).SameState(Before));
	}
	{
		FFixture F(TEXT("BP_Unsaved"));
		const FKismetAIAssetFingerprint Before = F.AgentCall([&](UBlueprint* BP) { SetPin(BP, F.PrintGuid, TEXT("NeverSaved")); });
		const FString FileBefore = FileSha(F.Path);
		FText Error;
		UPackageTools::ReloadPackages({ F.BP()->GetOutermost() }, Error, EReloadPackagesInteractionMode::AssumePositive);
		TestEqual(TEXT("the unsaved edit is gone after the restart"), PinDefault(FindByGuid(F.BP(), F.PrintGuid)), FString(TEXT("Hello")));
		const TArray<FKismetAIRestoreReport> R = Mod.RestoreRunCheckpoints(F.Session, FString(), false);
		TestTrue(TEXT("the session's run is found and already at its checkpoint"), R.Num() == 1 && R[0].Succeeded());
		TestTrue(TEXT("says nothing was changed"), R.Num() == 1 && R[0].Explanation.Contains(TEXT("Nothing was changed")));
		TestEqual(TEXT("the file was not rewritten"), FileSha(F.Path), FileBefore);
		TestTrue(TEXT("state equals the checkpoint"), KismetAIAssetState::Capture(F.Path).SameState(Before));
	}
	return true;
}

// Edits the user had not saved when the run started are part of the checkpoint:
// Reject keeps them, and says they are now saved.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCheckpointRecovery_PreexistingUnsavedEditsArePreserved,
	"KismetAI.CheckpointRecovery.PreexistingUnsavedEditsArePreserved", CheckpointRecoveryTestFlags)
bool FCheckpointRecovery_PreexistingUnsavedEditsArePreserved::RunTest(const FString& Parameters)
{
	using namespace CheckpointRecoveryTest;
	FKismetAIModule& Mod = FKismetAIModule::Get();
	FFixture F(TEXT("BP_Preexisting"));
	SetPin(F.BP(), F.PrintGuid, TEXT("UserUnsaved"));
	F.BP()->GetOutermost()->SetDirtyFlag(true);
	F.AgentCall([&](UBlueprint* BP) { SetPin(BP, F.PrintGuid, TEXT("Agent")); });

	const TArray<FKismetAIRestoreReport> R = Mod.RestoreRunCheckpoints(F.Session, F.Run, false);
	TestTrue(TEXT("restored"), R.Num() == 1 && R[0].Succeeded());
	TestEqual(TEXT("the user's unsaved edit is back"), PinDefault(FindByGuid(F.BP(), F.PrintGuid)), FString(TEXT("UserUnsaved")));
	TestTrue(TEXT("the report discloses that it is now saved"), R.Num() == 1 && R[0].bIncludesPreviouslyUnsavedEdits);
	return true;
}

// Assets a checkpoint cannot cover say so.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCheckpointRecovery_UnsupportedTargetsSaySo,
	"KismetAI.CheckpointRecovery.UnsupportedTargetsSaySo", CheckpointRecoveryTestFlags)
bool FCheckpointRecovery_UnsupportedTargetsSaySo::RunTest(const FString& Parameters)
{
	using namespace CheckpointRecoveryTest;
	FKismetAIModule& Mod = FKismetAIModule::Get();
	FFixture F(TEXT("BP_Supported"));
	const FString Missing = DisposablePackageName(TEXT("BP_NotYetCreated"));
	FKismetAIMutationContext Ctx = F.Context();
	Ctx.Targets = { Missing };
	const TArray<FKismetAIAssetFingerprint> Before = Mod.BeginAgentMutation(Ctx);
	TestTrue(TEXT("a create target is captured as absent"), Before.Num() == 1 && Before[0].bCaptured && !Before[0].bExists);
	const FKismetAIRestoreReport R = Mod.GetCheckpointStore().Restore(F.Session, F.Run, Missing);
	TestTrue(TEXT("unsupported"), R.Status == EKismetAIRestoreStatus::Unsupported);
	TestTrue(TEXT("because the run created it"), R.Explanation.Contains(TEXT("did not exist before the run")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// End to end through the agent: the real module invoker journals a real
// before-state and keeps a checkpoint; a crash after the insert landed but
// before its outcome record is reconciled from the asset, the resumed model's
// identical insert is refused, and the checkpoint undoes the insert.
// ─────────────────────────────────────────────────────────────────────────────

namespace CheckpointRecoveryTest
{
	class FScriptedModel final : public IKismetAIModelClient
	{
	public:
		explicit FScriptedModel(TArray<TSharedPtr<FJsonObject>> InResponses) : Responses(MoveTemp(InResponses)) {}
		virtual FModelCallOutcome Call(const FModelCallRequest& Request, const FModelCallbacks& Callbacks) override { return Next(Request); }
		virtual FModelCallOutcome CallWithRetry(const FModelCallRequest& Request, const FModelCallbacks& Callbacks) override { return Next(Request); }
		virtual void Cancel() override {}
		virtual FString GetProviderName() const override { return TEXT("anthropic"); }
		virtual int32 CacheTtlSeconds() const override { return 300; }
		TArray<TArray<TSharedPtr<FJsonValue>>> Sent;
	private:
		FModelCallOutcome Next(const FModelCallRequest& Request)
		{
			Sent.Add(KismetAIAgentCodec::ToAnthropicArray(Request.Messages));
			FModelCallOutcome Out;
			Out.Result = EModelCallResult::Success;
			Out.HttpStatusCode = 200;
			Out.RequestCount = 1;
			const TSharedPtr<FJsonObject>& Fixture = Responses[FMath::Min(Sent.Num() - 1, Responses.Num() - 1)];
			Out.AssistantMessage.Role = EAgentRole::Assistant;
			Out.AssistantMessage.Parts = KismetAIAgentCodec::FromParts(Fixture->GetArrayField(TEXT("content")));
			Out.Response = MakeShared<FJsonObject>(*Fixture);
			Out.Response->RemoveField(TEXT("content"));
			return Out;
		}
		TArray<TSharedPtr<FJsonObject>> Responses;
	};

	TSharedPtr<FJsonObject> Response(const TSharedPtr<FJsonObject>& Block, const TCHAR* StopReason)
	{
		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetArrayField(TEXT("content"), { MakeShared<FJsonValueObject>(Block) });
		R->SetStringField(TEXT("stop_reason"), StopReason);
		TSharedPtr<FJsonObject> Usage = MakeShared<FJsonObject>();
		Usage->SetNumberField(TEXT("input_tokens"), 100);
		Usage->SetNumberField(TEXT("output_tokens"), 10);
		R->SetObjectField(TEXT("usage"), Usage);
		return R;
	}

	TSharedPtr<FJsonObject> ToolTurn(const TCHAR* Id, const TSharedPtr<FJsonObject>& Input)
	{
		TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>();
		B->SetStringField(TEXT("type"), TEXT("tool_use"));
		B->SetStringField(TEXT("id"), Id);
		B->SetStringField(TEXT("name"), TEXT("insert_node"));
		B->SetObjectField(TEXT("input"), Input);
		return Response(B, TEXT("tool_use"));
	}

	TSharedPtr<FJsonObject> TextTurn(const TCHAR* Text)
	{
		TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>();
		B->SetStringField(TEXT("type"), TEXT("text"));
		B->SetStringField(TEXT("text"), Text);
		return Response(B, TEXT("end_turn"));
	}

	FKismetAIAgentConfig AgentConfig()
	{
		FKismetAIAgentConfig C;
		C.ApiKey = TEXT("test-key-unused");
		C.Model = TEXT("claude-sonnet-4-6");
		C.MaxSteps = 4;
		C.MaxRetries = 0;
		C.RetryJitterFactor = 0.0;
		return C;
	}

	int32 NodeCount(const FString& Path)
	{
		UEdGraph* G = EventGraph(Find(Path));
		return G ? G->Nodes.Num() : -1;
	}

	FString Condensed(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), W);
		return Out;
	}
}

static bool E2EBody(FAutomationTestBase& T)
{
	using namespace CheckpointRecoveryTest;
	FKismetAIModule& Mod = FKismetAIModule::Get();
	FFixture F(TEXT("BP_Agent"));
	KismetAIPersistence::FScopedEvalModeOverride NoAutosave(true);  // the insert stays in memory
	const FKismetAIAssetFingerprint PreRun = KismetAIAssetState::Capture(F.Path);
	const int32 NodesBefore = NodeCount(F.Path);

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), F.Path);
	Args->SetStringField(TEXT("node"), TEXT("Print String"));

	// 1. A real run: insert_node through the module invoker.
	FKismetAIModuleToolInvoker Invoker(Mod);
	FScriptedModel Model({ ToolTurn(TEXT("toolu_ins"), Args), TextTurn(TEXT("Inserted.")) });
	FKismetAIAgent Agent(Invoker, Model, AgentConfig());
	Agent.RunTask(TEXT("Add a Print String."), FOnAgentStep(), TEXT(""), TEXT(""));
	const FString Session = Agent.GetSessionId();
	ON_SCOPE_EXIT
	{
		FKismetAISessionStorage::DeleteSession(Session);
		IFileManager::Get().DeleteDirectory(*(Mod.GetCheckpointStore().GetRoot() / Session), false, true);
	};
	T.TestEqual(TEXT("the insert landed"), NodeCount(F.Path), NodesBefore + 1);

	// The journal holds the real before-state, and the run kept a checkpoint.
	const TArray<TSharedPtr<FJsonObject>> Entries = FKismetAISessionStorage::LoadAllEntries(Session);
	FString RunId;
	int32 IntentLine = INDEX_NONE;
	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		FString Type;
		if (Entries[i]->TryGetStringField(TEXT("type"), Type) && Type == KismetAIMutationIntent::IntentEntryType)
		{
			IntentLine = i;
			RunId = Entries[i]->GetStringField(TEXT("run_id"));
			TArray<FKismetAIAssetFingerprint> Targets;
			TArray<FString> Unresolved;
			KismetAIMutationIntent::ReadIntentTargets(Entries[i], Targets, Unresolved);
			T.TestTrue(TEXT("intent holds the asset's real before-state"), Targets.Num() == 1 && Targets[0].SameState(PreRun));
		}
	}
	T.TestTrue(TEXT("an intent was journaled"), IntentLine != INDEX_NONE);
	const TArray<FKismetAICheckpointInfo> Run = Mod.GetCheckpointStore().ListRun(Session, RunId);
	T.TestTrue(TEXT("the run kept a restorable checkpoint"), Run.Num() == 1 && Run[0].bRestorable && Run[0].bChangedByRun);

	// 2. The crash: a copy of the journal that ends right after the intent, as
	// if the editor died between the insert and its outcome record.
	const FString CrashSession = FKismetAISessionStorage::CreateSessionId();
	ON_SCOPE_EXIT { FKismetAISessionStorage::DeleteSession(CrashSession); };
	{
		FString Jsonl;
		for (int32 i = 0; i <= IntentLine && i < Entries.Num(); ++i)
		{
			Jsonl += Condensed(Entries[i]) + TEXT("\n");
		}
		FFileHelper::SaveStringToFile(Jsonl, *FKismetAISessionStorage::GetSessionFilePath(CrashSession), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
	FScriptedModel Resumed({ ToolTurn(TEXT("toolu_again"), Args), TextTurn(TEXT("Done.")) });
	FKismetAIAgent Agent2(Invoker, Resumed, AgentConfig());
	Agent2.RunTask(TEXT("Continue."), FOnAgentStep(), TEXT(""), CrashSession);
	T.TestEqual(TEXT("no second copy is inserted"), NodeCount(F.Path), NodesBefore + 1);
	const FSessionSnapshot Snap = FKismetAISessionStorage::LoadSession(CrashSession);
	T.TestTrue(TEXT("one reconciliation"), Snap.Reconciliations.Num() == 1);
	if (Snap.Reconciliations.Num() == 1)
	{
		const FKismetAIReconciliation& R = Snap.Reconciliations[0];
		T.TestTrue(TEXT("changed_since_intent"), R.Verdict == EKismetAIReconcileVerdict::ChangedSinceIntent);
		T.TestTrue(TEXT("memory differs, disk does not (the insert was never saved)"), R.Assets.Num() == 1
			&& R.Assets[0].MemoryMatchesBefore.IsSet() && !R.Assets[0].MemoryMatchesBefore.GetValue()
			&& R.Assets[0].DiskMatchesBefore.IsSet() && R.Assets[0].DiskMatchesBefore.GetValue());
		T.TestTrue(TEXT("the new node is the evidence"), R.Assets.Num() == 1
			&& R.Assets[0].Differences.ContainsByPredicate([](const FString& D) { return D.StartsWith(TEXT("added: ")) && D.Contains(TEXT("K2Node")); }));
	}

	// 3. The checkpoint undoes the insert, verified.
	const TArray<FKismetAIRestoreReport> Reports = Mod.RestoreRunCheckpoints(Session, RunId, false);
	T.TestTrue(TEXT("restored"), Reports.Num() == 1 && Reports[0].Succeeded());
	T.TestEqual(TEXT("node count back"), NodeCount(F.Path), NodesBefore);
	T.TestTrue(TEXT("state equals the pre-run state"), KismetAIAssetState::Capture(F.Path).SameState(PreRun));
	T.AddInfo(Statuses(Reports));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCheckpointRecovery_AgentCrashWindowEndToEnd,
	"KismetAI.CheckpointRecovery.AgentCrashWindowEndToEnd", CheckpointRecoveryTestFlags)
bool FCheckpointRecovery_AgentCrashWindowEndToEnd::RunTest(const FString& Parameters)
{
	return E2EBody(*this);
}

