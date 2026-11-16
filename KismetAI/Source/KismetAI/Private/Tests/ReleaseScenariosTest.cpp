// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// The deterministic release failure scenarios, end to end through the
// production entry points.
//
// Each owning fix proved its invariant where it lives, often
// at handler or loop level with fakes. These tests prove the same invariants
// where a user or client reaches them: the panel launch path
// (FKismetAIModule::LaunchAgent), POST /api/v1/agent/runs with its cancel
// route, the MCP tools/call adapter and the REST tool routes. Behind them run
// the real executor, policy and grants, game-thread dispatcher, run
// controller, tool handlers, journal on disk and journal reader.
//
// What is controlled:
// - the provider: a scripted model client (RunEntryHarness.h) — no network;
// - the assets: file-backed Blueprints and Materials in a /Game mount backed by
//   a temporary directory (DisposableContent.h), so the project's Content is
//   never written;
// - journals and checkpoints: a per-test temporary folder
//   (KismetAITestSandbox.cpp);
// - timing: latches. The deadlines in PumpUntil only bound a failure.
//
// Evidence is always the asset itself — a fingerprint of every object's state,
// the package dirty flag and the package file's SHA-1 (KismetAIAssetState) —
// plus the journal as the production reader loads it, never only a returned
// error string.

#include "Misc/AutomationTest.h"
#include "RunEntryHarness.h"
#include "DisposableContent.h"
#include "ScopedTestGrant.h"
#include "KismetAIAssetState.h"
#include "KismetAIMcpRoute.h"
#include "KismetAIMutationIntent.h"
#include "KismetAIPersistence.h"
#include "KismetAISessionStorage.h"
#include "KismetAIToolCallJournal.h"
#include "BlueprintActionDatabase.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectHash.h"

namespace KismetAIReleaseScenarios
{
	using namespace KismetAIRunEntryHarness;
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	// ── Disposable assets ────────────────────────────────────────────────────

	// As asset deletion does: a Blueprint's node spawners leave the action
	// database and its classes go with it. A skeleton class left alive after
	// its Blueprint is collected crashes the next package reload.
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

	void Dispose(UObject* Asset)
	{
		if (!Asset) return;
		UPackage* Pkg = Asset->GetOutermost();
		ResetLoaders(Pkg);
		if (UBlueprint* BP = Cast<UBlueprint>(Asset))
		{
			Retire(BP);
		}
		else
		{
			Asset->ClearFlags(RF_Public | RF_Standalone);
			Asset->MarkAsGarbage();
		}
		Pkg->SetDirtyFlag(false);
		Pkg->MarkAsGarbage();
	}

	bool SaveToDisk(UObject* Asset)
	{
		UPackage* Pkg = Asset->GetOutermost();
		const FString File = FPackageName::LongPackageNameToFilename(Pkg->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		const bool bSaved = UPackage::SavePackage(Pkg, Asset, *File, Args);
		Pkg->SetDirtyFlag(false);
		return bSaved;
	}

	UK2Node_CallFunction* AddPrint(UEdGraph* Graph, const TCHAR* Text, int32 X)
	{
		UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
		Call->SetFlags(RF_Transactional);
		Call->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString), UKismetSystemLibrary::StaticClass());
		Graph->AddNode(Call, false, false);
		Call->CreateNewGuid();
		Call->NodePosX = X;
		Call->AllocateDefaultPins();
		Call->PostPlacedNewNode();
		Call->FindPin(TEXT("InString"))->DefaultValue = Text;
		return Call;
	}

	// A saved, compiled Blueprint: PrintA.then -> PrintB.execute, in its own
	// temporary /Game mount.
	struct FBlueprintFixture
	{
		KismetAIDisposableContent::FScopedDisposableMount Mount;
		FString Path;
		FGuid AGuid;
		FGuid BGuid;

		explicit FBlueprintFixture(const TCHAR* Stem) : Mount(Stem)
		{
			Path = Mount.PackageName(TEXT("BP_Scenario"));
			UPackage* Pkg = CreatePackage(*Path);
			UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Pkg, TEXT("BP_Scenario"),
				BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
			if (!Blueprint || Blueprint->UbergraphPages.Num() == 0) return;
			UEdGraph* Graph = Blueprint->UbergraphPages[0];
			UK2Node_CallFunction* A = AddPrint(Graph, TEXT("A-baseline"), 0);
			UK2Node_CallFunction* B = AddPrint(Graph, TEXT("B-baseline"), 400);
			Graph->GetSchema()->TryCreateConnection(A->FindPin(UEdGraphSchema_K2::PN_Then), B->FindPin(UEdGraphSchema_K2::PN_Execute));
			AGuid = A->NodeGuid;
			BGuid = B->NodeGuid;
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			SaveToDisk(Blueprint);
		}

		~FBlueprintFixture()
		{
			Dispose(BP());
			if (GEditor)
			{
				GEditor->ResetTransaction(FText::FromString(TEXT("KismetAI release scenario fixture")));
			}
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
		}

		bool IsValid() const { return AGuid.IsValid() && BGuid.IsValid() && BP() != nullptr; }

		UBlueprint* BP() const
		{
			UPackage* Pkg = FindPackage(nullptr, *Path);
			return Pkg ? Cast<UBlueprint>(Pkg->FindAssetInPackage()) : nullptr;
		}
		FString ObjectPath() const { return BP() ? BP()->GetPathName() : Path; }

		UEdGraphNode* Node(const FGuid& Guid) const
		{
			UBlueprint* Blueprint = BP();
			if (!Blueprint || Blueprint->UbergraphPages.Num() == 0) return nullptr;
			for (UEdGraphNode* N : Blueprint->UbergraphPages[0]->Nodes)
			{
				if (N && N->NodeGuid == Guid) return N;
			}
			return nullptr;
		}
		FString NodeName(const FGuid& Guid) const { UEdGraphNode* N = Node(Guid); return N ? N->GetName() : FString(TEXT("<missing>")); }
		FString Text(const FGuid& Guid) const
		{
			UEdGraphNode* N = Node(Guid);
			UEdGraphPin* P = N ? N->FindPin(TEXT("InString")) : nullptr;
			return P ? P->DefaultValue : FString(TEXT("<missing>"));
		}
		bool ALinkedToB() const
		{
			UEdGraphNode* A = Node(AGuid);
			UEdGraphNode* B = Node(BGuid);
			UEdGraphPin* Then = A ? A->FindPin(UEdGraphSchema_K2::PN_Then) : nullptr;
			UEdGraphPin* Exec = B ? B->FindPin(UEdGraphSchema_K2::PN_Execute) : nullptr;
			return Then && Exec && Then->LinkedTo.Contains(Exec) && Exec->LinkedTo.Contains(Then);
		}
		FKismetAIAssetFingerprint Capture() const { return KismetAIAssetState::Capture(Path); }
	};

	// A saved Material: Constant3Vector (red) -> Multiply.A, Multiply -> BaseColor.
	struct FMaterialFixture
	{
		KismetAIDisposableContent::FScopedDisposableMount Mount;
		FString Path;
		UMaterialExpressionConstant3Vector* Color = nullptr;

		explicit FMaterialFixture(const TCHAR* Stem) : Mount(Stem)
		{
			Path = Mount.PackageName(TEXT("M_Scenario"));
			UPackage* Pkg = CreatePackage(*Path);
			UMaterial* Material = NewObject<UMaterial>(Pkg, TEXT("M_Scenario"), RF_Public | RF_Standalone | RF_Transactional);
			Color = Cast<UMaterialExpressionConstant3Vector>(UMaterialEditingLibrary::CreateMaterialExpression(
				Material, UMaterialExpressionConstant3Vector::StaticClass(), -600, 0));
			UMaterialExpressionMultiply* Multiply = Cast<UMaterialExpressionMultiply>(UMaterialEditingLibrary::CreateMaterialExpression(
				Material, UMaterialExpressionMultiply::StaticClass(), -300, 0));
			if (!Color || !Multiply) { Color = nullptr; return; }
			Color->Constant = FLinearColor(1.f, 0.f, 0.f, 1.f);
			UMaterialEditingLibrary::ConnectMaterialExpressions(Color, TEXT(""), Multiply, TEXT("A"));
			UMaterialEditingLibrary::ConnectMaterialProperty(Multiply, TEXT(""), MP_BaseColor);
			SaveToDisk(Material);
		}

		~FMaterialFixture()
		{
			Dispose(Material());
			if (GEditor)
			{
				GEditor->ResetTransaction(FText::FromString(TEXT("KismetAI release scenario fixture")));
			}
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
		}

		UMaterial* Material() const
		{
			UPackage* Pkg = FindPackage(nullptr, *Path);
			return Pkg ? Cast<UMaterial>(Pkg->FindAssetInPackage()) : nullptr;
		}
		FKismetAIAssetFingerprint Capture() const { return KismetAIAssetState::Capture(Path); }
	};

	// ── State assertions ─────────────────────────────────────────────────────

	// The whole asset is as it was: every object's state, the dirty flag and
	// the file on disk. Logs each difference so a failure says what survived.
	bool ExpectUnchanged(FAutomationTestBase& T, const FString& What, const FKismetAIAssetFingerprint& Before,
		const FKismetAIAssetFingerprint& After)
	{
		const bool bCaptured = Before.HasStateEvidence() && After.HasStateEvidence();
		T.TestTrue(What + TEXT(": both states captured"), bCaptured);
		const bool bState = Before.SameState(After);
		if (!bState)
		{
			for (const FString& D : FKismetAIAssetFingerprint::DescribeDifferences(Before, After))
			{
				T.AddInfo(What + TEXT(": ") + D);
			}
		}
		T.TestTrue(What + TEXT(": every object's state equals the baseline"), bState);
		T.TestEqual(What + TEXT(": package dirty flag"), After.bPackageDirty, Before.bPackageDirty);
		T.TestTrue(What + TEXT(": package file unchanged"), Before.bFileExists && After.bFileExists && Before.FileSha1 == After.FileSha1);
		return bCaptured && bState && After.bPackageDirty == Before.bPackageDirty && Before.FileSha1 == After.FileSha1;
	}

	struct FUndoState
	{
		int32 Queue = -1;
		int32 UndoCount = -1;
		static FUndoState Now()
		{
			FUndoState S;
			if (GEditor && GEditor->Trans)
			{
				S.Queue = GEditor->Trans->GetQueueLength();
				S.UndoCount = GEditor->Trans->GetUndoCount();
			}
			return S;
		}
	};

	// A failed batch leaves nothing in the editor's history that Undo or Redo
	// could bring back.
	void ExpectNoHistoryLeft(FAutomationTestBase& T, const FString& What, const FUndoState& Before)
	{
		const FUndoState After = FUndoState::Now();
		T.TestEqual(What + TEXT(": no undo entry left behind"), After.Queue, Before.Queue);
		T.TestEqual(What + TEXT(": no redo entry left behind"), After.UndoCount, Before.UndoCount);
	}

	// ── Tool arguments ───────────────────────────────────────────────────────

	TSharedPtr<FJsonValue> Op(std::initializer_list<TPair<FString, FString>> Fields)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& F : Fields)
		{
			O->SetStringField(F.Key, F.Value);
		}
		return MakeShared<FJsonValueObject>(O);
	}

	TSharedPtr<FJsonObject> EditArgs(const FBlueprintFixture& F, const TArray<TSharedPtr<FJsonValue>>& Ops, bool bAtomic = true)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("blueprint"), F.ObjectPath());
		Args->SetArrayField(TEXT("operations"), Ops);
		Args->SetBoolField(TEXT("atomic"), bAtomic);
		return Args;
	}

	TSharedPtr<FJsonObject> SetTextArgs(const FBlueprintFixture& F, const FGuid& Node, const TCHAR* Value)
	{
		return EditArgs(F, { Op({ { TEXT("action"), TEXT("set_pin_default") }, { TEXT("node"), F.NodeName(Node) },
			{ TEXT("pin"), TEXT("InString") }, { TEXT("value"), Value } }) }, /*bAtomic=*/false);
	}

	TSharedPtr<FJsonObject> DeleteAssetArgs(const FString& AssetPath)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("action"), TEXT("delete"));
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		return Args;
	}

	TSharedPtr<FJsonObject> CreateAssetArgs(const FString& Folder, const TCHAR* Name)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_type"), TEXT("Blueprint"));
		Args->SetStringField(TEXT("path"), Folder);
		Args->SetStringField(TEXT("name"), Name);
		Args->SetStringField(TEXT("parent_class"), TEXT("Actor"));
		return Args;
	}

	FString Condensed(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), W);
		return Out;
	}

	// ── Entry points ─────────────────────────────────────────────────────────

	struct FEnvelope
	{
		bool bParsed = false;
		bool bSuccess = false;
		FString ErrorCode;
		FString Raw;
		TSharedPtr<FJsonObject> Json;
	};

	FEnvelope ParseEnvelope(FString Text)
	{
		FEnvelope Out;
		Out.Raw = Text;
		Text.TrimStartAndEndInline();
		Text.RemoveFromStart(TEXT("<tool_use_error>"));
		Text.RemoveFromEnd(TEXT("</tool_use_error>"));
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Out.Json) || !Out.Json.IsValid())
		{
			return Out;
		}
		Out.bParsed = true;
		Out.Json->TryGetBoolField(TEXT("success"), Out.bSuccess);
		const TSharedPtr<FJsonObject>* Error = nullptr;
		if (Out.Json->TryGetObjectField(TEXT("error"), Error) && Error && Error->IsValid())
		{
			(*Error)->TryGetStringField(TEXT("error_code"), Out.ErrorCode);
		}
		return Out;
	}

	// MCP tools/call, as the MCP route dispatches it after its bearer check.
	FEnvelope ViaMcp(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
	{
		return ParseEnvelope(KismetAIMcpRoute::CallTool(FKismetAIModule::Get(),
			FKismetAICallerContext::Verified(EKismetAIEntryPoint::Mcp, TEXT("mcp-bearer")), Tool, Args));
	}

	// A REST tool route, as the router drives it after the X-Kismet-Token gate.
	FEnvelope ViaRest(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
	{
		FString Captured;
		FKismetAIModule::Get().ServeToolRoute(*Tool, nullptr,
			FKismetAICallerContext::Verified(EKismetAIEntryPoint::Rest, TEXT("x-kismet-token")), MakeRequest(Condensed(Args)),
			[&Captured](TUniquePtr<FHttpServerResponse>&& Response)
			{
				if (Response.IsValid()) { Captured = BodyText(Response->Body); }
			});
		return ParseEnvelope(Captured);
	}

	// POST /api/v1/agent/runs. Returns the run id, or empty on refusal.
	FString SubmitRun(FAutomationTestBase& T, const TSharedRef<FJsonObject>& Body, FString* AssignedSessionId = nullptr)
	{
		const FRunReply Reply = Call(&FAccess::Submit, MakeRequest(Condensed(Body)));
		if (!T.TestEqual(TEXT("agent/runs accepted the run"), Reply.Code, 202) || !Reply.Data().IsValid())
		{
			T.AddInfo(Reply.Json.IsValid() ? Condensed(Reply.Json) : FString(TEXT("<no body>")));
			return FString();
		}
		if (AssignedSessionId) *AssignedSessionId = Reply.Data()->GetStringField(TEXT("session_id"));
		return Reply.Data()->GetStringField(TEXT("run_id"));
	}

	TSharedRef<FJsonObject> RunBody(const FString& Input, const FString& SessionId, std::initializer_list<const TCHAR*> Operations,
		const FString& Scope)
	{
		TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("input"), Input);
		if (!SessionId.IsEmpty()) Body->SetStringField(TEXT("session_id"), SessionId);
		if (Operations.size() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Ops;
			for (const TCHAR* O : Operations) { Ops.Add(MakeShared<FJsonValueString>(O)); }
			TSharedPtr<FJsonObject> Authority = MakeShared<FJsonObject>();
			Authority->SetArrayField(TEXT("operations"), Ops);
			Authority->SetArrayField(TEXT("scope"), { MakeShared<FJsonValueString>(Scope) });
			Body->SetObjectField(TEXT("authority"), Authority);
		}
		return Body;
	}

	void ExpectSessionHeader(FAutomationTestBase& T, const FString& SessionId)
	{
		int32 Headers = 0;
		for (const auto& Entry : FKismetAISessionStorage::LoadAllEntries(SessionId))
			if (Entry->GetStringField(TEXT("type")) == TEXT("session_start")) ++Headers;
		T.TestEqual(TEXT("fresh entry flow writes one durable header"), Headers, 1);
	}

	// Waits for the run's single terminal outcome and its worker.
	bool FinishRun(FAutomationTestBase& T, FScopedModuleSeams& Seams, const FString& RunId)
	{
		const FKismetAIExternalRunPtr Record = FAccess::Registry(Seams.Module).Find(RunId);
		const FKismetAIRunPtr Run = Record.IsValid() ? Record->GetControllerRun() : nullptr;
		const bool bFinished = T.TestTrue(TEXT("the run finishes"), PumpUntil([&Record]() { return Record.IsValid() && Record->IsFinished(); }));
		if (Run.IsValid())
		{
			T.TestTrue(TEXT("the run's worker exits"), FAccess::Session(Seams.Module).WaitForWorkerExit(Run.ToSharedRef(), PromptMs));
		}
		return bFinished;
	}

	TSharedPtr<FJsonObject> Outcome(const FString& RunId)
	{
		const FRunReply Status = GetStatus(RunId);
		const TSharedPtr<FJsonObject>* O = nullptr;
		return Status.Data().IsValid() && Status.Data()->TryGetObjectField(TEXT("outcome"), O) && O ? *O : nullptr;
	}

	// Every event the run emitted, as the events route returns them.
	TArray<TSharedPtr<FJsonObject>> Events(const FString& RunId)
	{
		TArray<TSharedPtr<FJsonObject>> Out;
		const FRunReply Page = Call(&FAccess::Events, MakeRequest(FString(),
			{ { TEXT("run_id"), RunId }, { TEXT("after"), TEXT("0") }, { TEXT("limit"), TEXT("1000") } }));
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Page.Data().IsValid() && Page.Data()->TryGetArrayField(TEXT("events"), Arr))
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr) { Out.Add(V->AsObject()); }
		}
		return Out;
	}

	int32 CountType(const TArray<TSharedPtr<FJsonObject>>& Evts, const TCHAR* Type)
	{
		int32 N = 0;
		for (const TSharedPtr<FJsonObject>& E : Evts) { N += E->GetStringField(TEXT("type")) == Type ? 1 : 0; }
		return N;
	}

	TSharedPtr<FJsonObject> LastData(const TArray<TSharedPtr<FJsonObject>>& Evts, const TCHAR* Type)
	{
		for (int32 I = Evts.Num() - 1; I >= 0; --I)
		{
			const TSharedPtr<FJsonObject>* D = nullptr;
			if (Evts[I]->GetStringField(TEXT("type")) == Type && Evts[I]->TryGetObjectField(TEXT("data"), D) && D)
			{
				return *D;
			}
		}
		return nullptr;
	}

	// The journal's per-call record for one tool_use id.
	TArray<TSharedPtr<FJsonObject>> CallRecords(const FString& SessionId, const TCHAR* ToolUseId)
	{
		TArray<TSharedPtr<FJsonObject>> Out;
		for (const TSharedPtr<FJsonObject>& E : FKismetAISessionStorage::LoadAllEntries(SessionId))
		{
			FString Type, Record, Id;
			if (E.IsValid() && E->TryGetStringField(TEXT("type"), Type) && Type == TEXT("tool_result_message")
				&& E->TryGetStringField(TEXT("record"), Record) && Record == KismetAIToolCallJournal::RecordToolCall
				&& E->TryGetStringField(TEXT("tool_use_id"), Id) && Id == ToolUseId)
			{
				Out.Add(E);
			}
		}
		return Out;
	}

	FString StringAt(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		FString V;
		return Obj.IsValid() && Obj->TryGetStringField(Field, V) ? V : FString();
	}

	TSharedPtr<FJsonObject> ObjectAt(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* V = nullptr;
		return Obj.IsValid() && Obj->TryGetObjectField(Field, V) && V ? *V : nullptr;
	}

	FString ContentText(const TSharedPtr<FJsonValue>& Message)
	{
		const TSharedPtr<FJsonObject> M = Message.IsValid() ? Message->AsObject() : nullptr;
		if (!M.IsValid()) return FString();
		FString Text;
		if (M->TryGetStringField(TEXT("content"), Text)) return Text;
		const TArray<TSharedPtr<FJsonValue>>* Blocks = nullptr;
		if (M->TryGetArrayField(TEXT("content"), Blocks))
		{
			for (const TSharedPtr<FJsonValue>& B : *Blocks)
			{
				FString Part;
				const TSharedPtr<FJsonObject> O = B->AsObject();
				if (O.IsValid() && O->TryGetStringField(TEXT("text"), Part)) Text += Part;
				else if (O.IsValid() && O->TryGetStringField(TEXT("content"), Part)) Text += Part;
			}
		}
		return Text;
	}

	// Every tool_use in Messages has exactly one tool_result in the next message.
	bool WellPaired(const TArray<TSharedPtr<FJsonValue>>& Messages, int32& OutToolUses)
	{
		OutToolUses = 0;
		for (int32 I = 0; I < Messages.Num(); ++I)
		{
			const TSharedPtr<FJsonObject> M = Messages[I]->AsObject();
			const TArray<TSharedPtr<FJsonValue>>* Blocks = nullptr;
			if (!M.IsValid() || !M->TryGetArrayField(TEXT("content"), Blocks)) continue;
			for (const TSharedPtr<FJsonValue>& B : *Blocks)
			{
				const TSharedPtr<FJsonObject> O = B->AsObject();
				if (!O.IsValid() || StringAt(O, TEXT("type")) != TEXT("tool_use")) continue;
				++OutToolUses;
				const FString Id = StringAt(O, TEXT("id"));
				int32 Results = 0;
				const TSharedPtr<FJsonObject> Next = I + 1 < Messages.Num() ? Messages[I + 1]->AsObject() : nullptr;
				const TArray<TSharedPtr<FJsonValue>>* NextBlocks = nullptr;
				if (Next.IsValid() && Next->TryGetArrayField(TEXT("content"), NextBlocks))
				{
					for (const TSharedPtr<FJsonValue>& R : *NextBlocks)
					{
						const TSharedPtr<FJsonObject> RO = R->AsObject();
						Results += RO.IsValid() && StringAt(RO, TEXT("type")) == TEXT("tool_result") && StringAt(RO, TEXT("tool_use_id")) == Id ? 1 : 0;
					}
				}
				if (Results != 1) return false;
			}
		}
		return true;
	}

	// An atomic batch that failed and was rolled back. The envelope's `success`
	// means "the tool ran"; what happened to the batch is in data: it is
	// rolled back, nothing is applied, and the outcome says restored.
	void ExpectRolledBackReply(FAutomationTestBase& T, const FString& Route, const FEnvelope& Out)
	{
		const TSharedPtr<FJsonObject> Data = ObjectAt(Out.Json, TEXT("data"));
		T.TestTrue(Route + TEXT(": answered with an envelope"), Out.bParsed && Data.IsValid());
		bool bRolledBack = false;
		T.TestTrue(Route + TEXT(": rolled_back"), Data.IsValid() && Data->TryGetBoolField(TEXT("rolled_back"), bRolledBack) && bRolledBack);
		T.TestEqual(Route + TEXT(": recovery_status"), StringAt(Data, TEXT("recovery_status")), FString(TEXT("restored")));
		const TSharedPtr<FJsonObject> MO = ObjectAt(Data, TEXT("outcome"));
		T.TestEqual(Route + TEXT(": outcome restored"), StringAt(MO, TEXT("recovery_status")), FString(TEXT("restored")));
		T.TestTrue(Route + TEXT(": outcome says nothing applied"), MO.IsValid() && !MO->GetBoolField(TEXT("applied_in_memory")));
		T.TestEqual(Route + TEXT(": nothing saved"), StringAt(MO, TEXT("save_status")), FString(TEXT("not_requested")));
	}

	bool AssetExists(const FString& PackageName)
	{
		return FindPackage(nullptr, *PackageName) != nullptr || FPackageName::DoesPackageExist(PackageName);
	}
}


// ─────────────────────────────────────────────────────────────────────────────
// Scenario 1 — failed atomic batch, Blueprint, through POST /agent/runs.
// The model sends one atomic edit_blueprint batch: a valid pin edit, then a
// connect from a node that does not exist. The asset must be exactly as it was
// (state, dirty flag, file, editor history), the journal must say "failed,
// restored", and the run must end with one terminal event.
// ─────────────────────────────────────────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReleaseScenarios_FailedBatchBlueprintViaAgentRun,
	"KismetAI.ReleaseScenarios.S01_FailedAtomicBatch.BlueprintViaAgentRun", KismetAIReleaseScenarios::Flags)
bool FReleaseScenarios_FailedBatchBlueprintViaAgentRun::RunTest(const FString& Parameters)
{
	using namespace KismetAIReleaseScenarios;
	FScopedModuleSeams Seams(FScopedModuleSeams::EPolicy::Production);
	if (FAccess::Session(Seams.Module).IsRunning()) { AddError(TEXT("a run is already active")); return false; }
	KismetAIPersistence::FScopedEvalModeOverride Autosave(false);
	FBlueprintFixture F(TEXT("S01"));
	if (!TestTrue(TEXT("fixture"), F.IsValid())) return false;
	const FKismetAIAssetFingerprint Before = F.Capture();
	const FUndoState History = FUndoState::Now();

	Seams.Seams->Model.Script = {
		ToolTurn(TEXT("toolu_s01"), TEXT("edit_blueprint"), EditArgs(F, {
			Op({ { TEXT("action"), TEXT("set_pin_default") }, { TEXT("node"), F.NodeName(F.BGuid) }, { TEXT("pin"), TEXT("InString") }, { TEXT("value"), TEXT("agent") } }),
			Op({ { TEXT("action"), TEXT("connect") }, { TEXT("source_node"), TEXT("K2Node_DoesNotExist_0") }, { TEXT("source_pin"), TEXT("then") },
				{ TEXT("target_node"), F.NodeName(F.BGuid) }, { TEXT("target_pin"), TEXT("execute") } }) })),
		TextTurn(TEXT("The batch failed.")) };
	FString SessionId;
	const FString RunId = SubmitRun(*this, RunBody(TEXT("p113 s01"), SessionId, { TEXT("edit") }, F.Mount.Scope()), &SessionId);
	if (RunId.IsEmpty()) return false;
	ON_SCOPE_EXIT { SettleRun(Seams.Module, RunId, Seams.Seams->Model); };
	FinishRun(*this, Seams, RunId);
	ExpectSessionHeader(*this, SessionId);

	ExpectUnchanged(*this, TEXT("Blueprint after the failed batch"), Before, F.Capture());
	TestEqual(TEXT("the valid edit is not left applied"), F.Text(F.BGuid), FString(TEXT("B-baseline")));
	TestTrue(TEXT("the A -> B link is intact"), F.ALinkedToB());
	ExpectNoHistoryLeft(*this, TEXT("Blueprint"), History);

	const TArray<TSharedPtr<FJsonObject>> Records = CallRecords(SessionId, TEXT("toolu_s01"));
	if (TestEqual(TEXT("one journal record for the call"), Records.Num(), 1))
	{
		TestEqual(TEXT("journal: failed"), StringAt(Records[0], TEXT("status")), FString(TEXT("failed")));
		const TSharedPtr<FJsonObject> MO = ObjectAt(Records[0], TEXT("mutation_outcome"));
		TestEqual(TEXT("journal: the batch was restored"), StringAt(MO, TEXT("recovery_status")), FString(TEXT("restored")));
		TestFalse(TEXT("journal: nothing applied"), MO.IsValid() && MO->GetBoolField(TEXT("applied_in_memory")));
	}
	const TArray<TSharedPtr<FJsonObject>> Evts = Events(RunId);
	TestEqual(TEXT("exactly one terminal event"), CountType(Evts, TEXT("done")), 1);
	TestEqual(TEXT("one tool call"), CountType(Evts, TEXT("tool_call")), 1);
	TestEqual(TEXT("failed requested batch remains partial after rollback"), StringAt(Outcome(RunId), TEXT("status")), FString(TEXT("partial")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Scenario 1 — failed atomic batch, Material, through MCP tools/call.
// ─────────────────────────────────────────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReleaseScenarios_FailedBatchMaterialViaMcp,
	"KismetAI.ReleaseScenarios.S01_FailedAtomicBatch.MaterialViaMcp", KismetAIReleaseScenarios::Flags)
bool FReleaseScenarios_FailedBatchMaterialViaMcp::RunTest(const FString& Parameters)
{
	using namespace KismetAIReleaseScenarios;
	FMaterialFixture M(TEXT("S01M"));
	if (!TestNotNull(TEXT("fixture"), M.Color)) return false;
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Mcp }, EKismetAIAuthorityOp::Edit, { *M.Mount.Scope() });
	const FKismetAIAssetFingerprint Before = M.Capture();
	const FUndoState History = FUndoState::Now();

	TSharedPtr<FJsonObject> SetColor = MakeShared<FJsonObject>();
	SetColor->SetStringField(TEXT("action"), TEXT("set_property"));
	SetColor->SetStringField(TEXT("node"), M.Color->GetName());
	SetColor->SetStringField(TEXT("property"), TEXT("Constant"));
	SetColor->SetArrayField(TEXT("value"), { MakeShared<FJsonValueNumber>(0.0), MakeShared<FJsonValueNumber>(1.0), MakeShared<FJsonValueNumber>(0.0) });
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("material"), M.Material()->GetPathName());
	Args->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(SetColor),
		Op({ { TEXT("action"), TEXT("connect") }, { TEXT("from_node"), TEXT("MaterialExpressionDoesNotExist_0") }, { TEXT("to_property"), TEXT("Roughness") } }) });
	Args->SetBoolField(TEXT("atomic"), true);

	const FEnvelope Out = ViaMcp(TEXT("edit_material"), Args);
	ExpectRolledBackReply(*this, TEXT("MCP edit_material"), Out);
	TestEqual(TEXT("MCP: status rolled_back"), StringAt(ObjectAt(Out.Json, TEXT("data")), TEXT("status")), FString(TEXT("rolled_back")));
	ExpectUnchanged(*this, TEXT("Material after the failed batch"), Before, M.Capture());
	TestEqual(TEXT("the valid edit is not left applied"), M.Color->Constant, FLinearColor(1.f, 0.f, 0.f, 1.f));
	ExpectNoHistoryLeft(*this, TEXT("Material"), History);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Scenario 2 — delete a connected node, then fail, through a REST tool route.
// The node must come back as the same object with the same GUID and its link;
// nothing may be dirtied, written or left in the editor's history.
// ─────────────────────────────────────────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReleaseScenarios_DeleteThenFailViaRest,
	"KismetAI.ReleaseScenarios.S02_DeleteThenFail.BlueprintViaRest", KismetAIReleaseScenarios::Flags)
bool FReleaseScenarios_DeleteThenFailViaRest::RunTest(const FString& Parameters)
{
	using namespace KismetAIReleaseScenarios;
	KismetAIPersistence::FScopedEvalModeOverride Autosave(false);
	FBlueprintFixture F(TEXT("S02"));
	if (!TestTrue(TEXT("fixture"), F.IsValid())) return false;
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Rest }, EKismetAIAuthorityOp::Edit, { *F.Mount.Scope() });
	const FKismetAIAssetFingerprint Before = F.Capture();
	const FUndoState History = FUndoState::Now();
	UEdGraphNode* const Deleted = F.Node(F.BGuid);

	const FEnvelope Out = ViaRest(TEXT("edit_blueprint"), EditArgs(F, {
		Op({ { TEXT("action"), TEXT("delete_node") }, { TEXT("node"), F.NodeName(F.BGuid) } }),
		Op({ { TEXT("action"), TEXT("delete_node") }, { TEXT("node"), TEXT("K2Node_DoesNotExist_0") } }) }));
	ExpectRolledBackReply(*this, TEXT("REST edit_blueprint"), Out);
	TestEqual(TEXT("REST: no operation counted as applied"), ObjectAt(Out.Json, TEXT("data")).IsValid()
		? static_cast<int32>(ObjectAt(Out.Json, TEXT("data"))->GetNumberField(TEXT("operations_succeeded"))) : -1, 0);

	TestTrue(TEXT("the deleted node is back as the same object"), F.Node(F.BGuid) == Deleted && ::IsValid(Deleted));
	TestTrue(TEXT("its A -> B link is back"), F.ALinkedToB());
	ExpectUnchanged(*this, TEXT("Blueprint after delete-then-fail"), Before, F.Capture());
	ExpectNoHistoryLeft(*this, TEXT("Blueprint"), History);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Scenario 3 — the package save fails after a successful edit, in an agent run.
// The edit is in memory only; the tool result, the journal record and the
// run's outcome (what the panel and clients show) must all say so.
// ─────────────────────────────────────────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReleaseScenarios_SaveFailureViaAgentRun,
	"KismetAI.ReleaseScenarios.S03_SaveFailure.ChangedButUnsavedEverywhere", KismetAIReleaseScenarios::Flags)
bool FReleaseScenarios_SaveFailureViaAgentRun::RunTest(const FString& Parameters)
{
	using namespace KismetAIReleaseScenarios;
	FScopedModuleSeams Seams(FScopedModuleSeams::EPolicy::Production);
	if (FAccess::Session(Seams.Module).IsRunning()) { AddError(TEXT("a run is already active")); return false; }
	KismetAIPersistence::FScopedEvalModeOverride Autosave(false);
	FBlueprintFixture F(TEXT("S03"));
	if (!TestTrue(TEXT("fixture"), F.IsValid())) return false;
	const FKismetAIAssetFingerprint Before = F.Capture();
	std::atomic<int32> SaveAttempts{ 0 };
	KismetAIPersistence::FScopedSaveFunctionOverride FailingDisk([&SaveAttempts, Path = F.Path](UPackage* Package, UObject*, const FString&, FString& OutError)
	{
		if (Package && Package->GetName() == Path)
		{
			++SaveAttempts;
			OutError = TEXT("injected: disk full");
			return false;
		}
		return true;
	});

	Seams.Seams->Model.Script = { ToolTurn(TEXT("toolu_s03"), TEXT("edit_blueprint"), SetTextArgs(F, F.BGuid, TEXT("agent"))), TextTurn(TEXT("Edited.")) };
	FString SessionId;
	const FString RunId = SubmitRun(*this, RunBody(TEXT("p113 s03"), SessionId, { TEXT("edit") }, F.Mount.Scope()), &SessionId);
	if (RunId.IsEmpty()) return false;
	ON_SCOPE_EXIT { SettleRun(Seams.Module, RunId, Seams.Seams->Model); };
	FinishRun(*this, Seams, RunId);
	ExpectSessionHeader(*this, SessionId);

	// The asset: changed in memory, dirty, and the file is what it was.
	TestTrue(TEXT("a save of the asset was attempted"), SaveAttempts.load() >= 1);
	TestEqual(TEXT("the edit is applied in memory"), F.Text(F.BGuid), FString(TEXT("agent")));
	const FKismetAIAssetFingerprint After = F.Capture();
	TestTrue(TEXT("the package is dirty"), After.bPackageDirty);
	TestTrue(TEXT("the file on disk is unchanged"), Before.FileSha1 == After.FileSha1 && !After.FileSha1.IsEmpty());

	// The tool result the model and the panel saw.
	const TArray<TSharedPtr<FJsonObject>> Evts = Events(RunId);
	const TSharedPtr<FJsonObject> Result = ObjectAt(LastData(Evts, TEXT("tool_result")), TEXT("result"));
	const TSharedPtr<FJsonObject> ResultOutcome = ObjectAt(ObjectAt(Result, TEXT("data")), TEXT("outcome"));
	TestEqual(TEXT("tool result: save failed"), StringAt(ResultOutcome, TEXT("save_status")), FString(TEXT("failed")));
	TestTrue(TEXT("tool result: carries the save error"), StringAt(ResultOutcome, TEXT("save_error")).Contains(TEXT("disk full")));
	TestTrue(TEXT("tool result: applied in memory"), ResultOutcome.IsValid() && ResultOutcome->GetBoolField(TEXT("applied_in_memory")));

	// The journal on disk.
	const TArray<TSharedPtr<FJsonObject>> Records = CallRecords(SessionId, TEXT("toolu_s03"));
	if (TestEqual(TEXT("one journal record"), Records.Num(), 1))
	{
		const TSharedPtr<FJsonObject> MO = ObjectAt(Records[0], TEXT("mutation_outcome"));
		TestEqual(TEXT("journal: save failed"), StringAt(MO, TEXT("save_status")), FString(TEXT("failed")));
		TestTrue(TEXT("journal: applied in memory"), MO.IsValid() && MO->GetBoolField(TEXT("applied_in_memory")));
	}

	// The run-level ledger: the mutation_outcomes event and the run outcome.
	const TSharedPtr<FJsonObject> Ledger = LastData(Evts, TEXT("mutation_outcomes"));
	TestTrue(TEXT("ledger event needs attention"), Ledger.IsValid() && Ledger->GetBoolField(TEXT("needs_attention")));
	const TArray<TSharedPtr<FJsonValue>>* Unsaved = nullptr;
	TestTrue(TEXT("ledger lists the package as unsaved"), Ledger.IsValid() && Ledger->TryGetArrayField(TEXT("unsaved_packages"), Unsaved)
		&& Unsaved->ContainsByPredicate([&F](const TSharedPtr<FJsonValue>& V) { return V->AsString() == F.Path; }));
	const TSharedPtr<FJsonObject> RunLedger = ObjectAt(Outcome(RunId), TEXT("mutation_outcomes"));
	TestTrue(TEXT("run outcome needs attention"), RunLedger.IsValid() && RunLedger->GetBoolField(TEXT("needs_attention")));
	const auto Completion = ObjectAt(ObjectAt(Outcome(RunId), TEXT("terminal")), TEXT("completion_summary"));
	TestEqual(TEXT("real edit/save failure is partial despite assistant prose"), StringAt(Completion, TEXT("status")), FString(TEXT("partial")));
	TestTrue(TEXT("real run summary names the changed asset"), Completion.IsValid()
		&& Completion->GetArrayField(TEXT("packages")).Num() == 1);
	TestEqual(TEXT("exactly one terminal event"), CountType(Evts, TEXT("done")), 1);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Scenario 4 — Stop between tool A and tool B of one turn, then resume with a
// new instruction. Stop is the real cancel route, posted after A's outcome is
// journaled and before B starts. A's change survives, B is recorded as not
// executed, the production reader pairs both, nothing is replayed, and the new
// instruction reaches the next model request verbatim.
// ─────────────────────────────────────────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReleaseScenarios_InterruptedTwoToolTurn,
	"KismetAI.ReleaseScenarios.S04_InterruptedTwoToolTurn.CancelThenResume", KismetAIReleaseScenarios::Flags)
bool FReleaseScenarios_InterruptedTwoToolTurn::RunTest(const FString& Parameters)
{
	using namespace KismetAIReleaseScenarios;
	FScopedModuleSeams Seams(FScopedModuleSeams::EPolicy::Production);
	if (FAccess::Session(Seams.Module).IsRunning()) { AddError(TEXT("a run is already active")); return false; }
	KismetAIPersistence::FScopedEvalModeOverride Autosave(false);
	FBlueprintFixture F(TEXT("S04"));
	if (!TestTrue(TEXT("fixture"), F.IsValid())) return false;

	// A's outcome is journaled -> the test posts Stop -> the worker continues.
	TSharedRef<FLatch> AJournaled = MakeShared<FLatch>();
	TSharedRef<FLatch> StopPosted = MakeShared<FLatch>();
	Seams.Seams->ConfigureAgent = [AJournaled, StopPosted](FKismetAIAgent& Agent)
	{
		Agent.SetJournalAppendHookForTests([AJournaled, StopPosted](const TSharedPtr<FJsonObject>& Entry)
		{
			FString Record, Id;
			if (Entry.IsValid() && Entry->TryGetStringField(TEXT("record"), Record) && Record == KismetAIToolCallJournal::RecordToolCall
				&& Entry->TryGetStringField(TEXT("tool_use_id"), Id) && Id == TEXT("toolu_A"))
			{
				AJournaled->Trigger();
				StopPosted->Wait(PromptMs);
			}
			return true;
		});
	};
	Seams.Seams->Model.Script = {
		ResponseTurn({ ToolUseBlock(TEXT("toolu_A"), TEXT("edit_blueprint"), SetTextArgs(F, F.AGuid, TEXT("A-applied"))),
			ToolUseBlock(TEXT("toolu_B"), TEXT("edit_blueprint"), SetTextArgs(F, F.BGuid, TEXT("B-never"))) }, TEXT("tool_use")),
		TextTurn(TEXT("A was applied; B never ran.")) };

	FString SessionId;
	const FString RunId = SubmitRun(*this, RunBody(TEXT("Set both texts."), SessionId, { TEXT("edit") }, F.Mount.Scope()), &SessionId);
	if (RunId.IsEmpty()) return false;
	ON_SCOPE_EXIT { StopPosted->Trigger(); SettleRun(Seams.Module, RunId, Seams.Seams->Model); };

	if (!TestTrue(TEXT("A ran and its outcome was journaled"), PumpUntil([&AJournaled]() { return AJournaled->Wait(0); }))) return false;
	const FRunReply Cancel = PostCancel(RunId);
	TestEqual(TEXT("cancel accepted"), Cancel.Code, 202);
	// A's tool_result event is out and B has not started.
	TestEqual(TEXT("caught between A and B"), Cancel.Data().IsValid() ? Cancel.Data()->GetStringField(TEXT("phase_at_request")) : FString(), FString(TEXT("between_steps")));
	StopPosted->Trigger();
	FinishRun(*this, Seams, RunId);
	ExpectSessionHeader(*this, SessionId);

	TestEqual(TEXT("run ends aborted"), StringAt(Outcome(RunId), TEXT("status")), FString(TEXT("aborted")));
	TestEqual(TEXT("A's change survives"), F.Text(F.AGuid), FString(TEXT("A-applied")));
	TestEqual(TEXT("B never ran"), F.Text(F.BGuid), FString(TEXT("B-baseline")));

	// The journal on disk, as written.
	const TArray<TSharedPtr<FJsonObject>> RecA = CallRecords(SessionId, TEXT("toolu_A"));
	const TArray<TSharedPtr<FJsonObject>> RecB = CallRecords(SessionId, TEXT("toolu_B"));
	if (TestEqual(TEXT("one record for A"), RecA.Num(), 1))
	{
		TestEqual(TEXT("A succeeded"), StringAt(RecA[0], TEXT("status")), FString(TEXT("succeeded")));
		TestTrue(TEXT("A carries its mutation outcome"), ObjectAt(RecA[0], TEXT("mutation_outcome")).IsValid());
	}
	if (TestEqual(TEXT("one record for B"), RecB.Num(), 1))
	{
		TestEqual(TEXT("B not started"), StringAt(RecB[0], TEXT("status")), FString(TEXT("not_started")));
		TestFalse(TEXT("B not attempted"), RecB[0]->GetBoolField(TEXT("attempted")));
		TestEqual(TEXT("B: cancelled"), StringAt(RecB[0], TEXT("not_started_reason")), FString(TEXT("cancelled")));
	}

	// The production reader.
	const FSessionSnapshot Snap = FKismetAISessionStorage::LoadSession(SessionId);
	TestTrue(TEXT("the session can resume"), Snap.CanResume());
	TestEqual(TEXT("no unrecorded call"), Snap.UnrecordedCalls, 0);
	TestEqual(TEXT("no uncertain mutation (B never started)"), Snap.UncertainCalls.Num(), 0);
	TestTrue(TEXT("the user's message is preserved"), Snap.bUserMessagesPreserved);

	// Resume with a new instruction through the same route and session.
	const FString NewInstruction = TEXT("Stop editing. Keep A as it is and do not touch PrintB; explain what happened.");
	const int32 RequestsBefore = Seams.Seams->Model.GetRequests().Num();
	const FString Resumed = SubmitRun(*this, RunBody(NewInstruction, SessionId, { TEXT("edit") }, F.Mount.Scope()));
	if (Resumed.IsEmpty()) return false;
	ON_SCOPE_EXIT { SettleRun(Seams.Module, Resumed, Seams.Seams->Model); };
	FinishRun(*this, Seams, Resumed);

	const TArray<TArray<TSharedPtr<FJsonValue>>> Requests = Seams.Seams->Model.GetRequests();
	if (!TestEqual(TEXT("the resumed run made one model request"), Requests.Num(), RequestsBefore + 1)) return false;
	const TArray<TSharedPtr<FJsonValue>>& Sent = Requests.Last();
	int32 ToolUses = 0;
	TestTrue(TEXT("every tool_use in the resumed request has exactly one tool_result"), WellPaired(Sent, ToolUses));
	TestEqual(TEXT("both calls of the interrupted turn are in the history"), ToolUses, 2);
	FString AllText;
	for (const TSharedPtr<FJsonValue>& M : Sent) { AllText += ContentText(M); }
	TestTrue(TEXT("B is shown to the model as not executed"), AllText.Contains(KismetAIToolCallJournal::NotExecutedCode));
	TestEqual(TEXT("the new instruction is the last message, verbatim"), ContentText(Sent.Last()), NewInstruction);
	TestEqual(TEXT("nothing replayed: A unchanged"), F.Text(F.AGuid), FString(TEXT("A-applied")));
	TestEqual(TEXT("nothing replayed: B unchanged"), F.Text(F.BGuid), FString(TEXT("B-baseline")));
	TestEqual(TEXT("the resumed run called no tool"), CountType(Events(Resumed), TEXT("tool_call")), 0);
	TestEqual(TEXT("one terminal event"), CountType(Events(Resumed), TEXT("done")), 1);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Scenario 8 — the same out-of-scope mutations through every entry point.
// MCP, REST and an HTTP agent run are unattended: refused AUTHORITY_REQUIRED
// even with a grant for another folder. The panel (launched through the
// module's real LaunchAgent) has edit/create under /Game by default but not
// delete, so its delete is asked in the panel; the editor user answers Deny.
// No route may create, delete, edit, dirty or write anything.
// ─────────────────────────────────────────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReleaseScenarios_PermissionParity,
	"KismetAI.ReleaseScenarios.S08_PermissionParity.OutOfScopeChangesNothing", KismetAIReleaseScenarios::Flags)
bool FReleaseScenarios_PermissionParity::RunTest(const FString& Parameters)
{
	using namespace KismetAIReleaseScenarios;
	FScopedModuleSeams Seams(FScopedModuleSeams::EPolicy::Production);
	FKismetAIModule& Module = Seams.Module;
	if (FAccess::Session(Module).IsRunning()) { AddError(TEXT("a run is already active")); return false; }
	KismetAIPersistence::FScopedEvalModeOverride Autosave(false);
	FBlueprintFixture F(TEXT("S08"));
	if (!TestTrue(TEXT("fixture"), F.IsValid())) return false;
	const FString Elsewhere = TEXT("/Game/__KismetAITest_S08_Elsewhere");
	const FString Created = F.Mount.PackageName(TEXT("BP_S08Created"));
	const FKismetAIAssetFingerprint Before = F.Capture();

	auto ExpectNothingChanged = [&](const FString& Route)
	{
		ExpectUnchanged(*this, Route, Before, F.Capture());
		TestTrue(Route + TEXT(": the Blueprint still exists"), F.BP() != nullptr);
		TestFalse(Route + TEXT(": nothing was created"), AssetExists(Created));
	};

	// MCP and REST, with a grant that covers another folder only.
	{
		FKismetAIScopedTestGrant Other({ EKismetAIEntryPoint::Mcp, EKismetAIEntryPoint::Rest },
			EKismetAIAuthorityOp::Edit | EKismetAIAuthorityOp::Create | EKismetAIAuthorityOp::Delete, { *Elsewhere });
		const TPair<const TCHAR*, TSharedPtr<FJsonObject>> Calls[] = {
			{ TEXT("edit_blueprint"), SetTextArgs(F, F.BGuid, TEXT("unauthorized")) },
			{ TEXT("manage_asset"), DeleteAssetArgs(F.ObjectPath()) },
			{ TEXT("create_asset"), CreateAssetArgs(F.Mount.Scope(), TEXT("BP_S08Created")) },
		};
		for (const TPair<const TCHAR*, TSharedPtr<FJsonObject>>& C : Calls)
		{
			const FEnvelope Mcp = ViaMcp(C.Key, C.Value);
			TestEqual(FString::Printf(TEXT("MCP %s: AUTHORITY_REQUIRED"), C.Key), Mcp.ErrorCode, FString(TEXT("AUTHORITY_REQUIRED")));
			const FEnvelope Rest = ViaRest(C.Key, C.Value);
			TestEqual(FString::Printf(TEXT("REST %s: AUTHORITY_REQUIRED"), C.Key), Rest.ErrorCode, FString(TEXT("AUTHORITY_REQUIRED")));
		}
		ExpectNothingChanged(TEXT("after MCP and REST"));
	}

	// POST /agent/runs: the model asks for the same three changes in one turn;
	// the run's own grant names another folder.
	Seams.Seams->Model.Script = {
		ResponseTurn({ ToolUseBlock(TEXT("toolu_edit"), TEXT("edit_blueprint"), SetTextArgs(F, F.BGuid, TEXT("unauthorized"))),
			ToolUseBlock(TEXT("toolu_delete"), TEXT("manage_asset"), DeleteAssetArgs(F.ObjectPath())),
			ToolUseBlock(TEXT("toolu_create"), TEXT("create_asset"), CreateAssetArgs(F.Mount.Scope(), TEXT("BP_S08Created"))) }, TEXT("tool_use")),
		TextTurn(TEXT("Not authorized.")),
		// The panel run that follows.
		ToolTurn(TEXT("toolu_panel_delete"), TEXT("manage_asset"), DeleteAssetArgs(F.ObjectPath())),
		TextTurn(TEXT("The user declined.")) };
	FString HttpSession;
	const FString RunId = SubmitRun(*this, RunBody(TEXT("p113 s08"), HttpSession, { TEXT("edit"), TEXT("create"), TEXT("delete") }, Elsewhere), &HttpSession);
	if (RunId.IsEmpty()) return false;
	ON_SCOPE_EXIT { SettleRun(Module, RunId, Seams.Seams->Model); };
	FinishRun(*this, Seams, RunId);
	ExpectSessionHeader(*this, HttpSession);
	for (const TCHAR* Id : { TEXT("toolu_edit"), TEXT("toolu_delete"), TEXT("toolu_create") })
	{
		const TArray<TSharedPtr<FJsonObject>> Rec = CallRecords(HttpSession, Id);
		TestTrue(FString::Printf(TEXT("agent run %s: refused, journaled as failed"), Id),
			Rec.Num() == 1 && StringAt(Rec[0], TEXT("status")) == TEXT("failed") && Condensed(Rec[0]).Contains(TEXT("AUTHORITY_REQUIRED")));
	}
	ExpectNothingChanged(TEXT("after the HTTP agent run"));

	// The panel launch path: the delete is asked; the editor user denies it.
	Module.SetCurrentSessionId(FString());
	Module.LaunchAgent(TEXT("p113 s08 panel: delete it"), FString(), FString(), FString());
	const FString PanelSession = Module.GetCurrentSessionId();
	TestTrue(TEXT("the panel run took the lease"), FAccess::Session(Module).IsRunning());
	FString QuestionId;
	const bool bAsked = PumpUntil([&Seams, &QuestionId]()
	{
		const TSharedPtr<FKismetAIAgent, ESPMode::ThreadSafe> Agent = Seams.Seams->GetLastAgent();
		QuestionId = Agent.IsValid() ? Agent->GetPendingQuestionId() : FString();
		return !QuestionId.IsEmpty();
	});
	TestTrue(TEXT("the panel asks the editor user before deleting"), bAsked);
	ExpectNothingChanged(TEXT("while the approval is pending"));
	if (bAsked)
	{
		TestFalse(TEXT("a REST token holder cannot answer the approval"),
			Module.SubmitAgentAnswer(QuestionId, TEXT("\"Allow once\""), EKismetAIEntryPoint::Rest));
		TestTrue(TEXT("the editor user's Deny is accepted"), Module.SubmitAgentAnswer(QuestionId, TEXT("\"Deny\""), EKismetAIEntryPoint::Panel));
	}
	else
	{
		FAccess::Session(Module).CancelActive();
	}
	TestTrue(TEXT("the panel run ends and its worker exits"), PumpUntil([&Module]()
	{
		return !FAccess::Session(Module).IsRunning() && FAccess::Session(Module).NumLiveWorkers() == 0;
	}));
	ExpectSessionHeader(*this, PanelSession);
	const TArray<TSharedPtr<FJsonObject>> PanelRec = CallRecords(PanelSession, TEXT("toolu_panel_delete"));
	TestTrue(TEXT("panel delete: denied, journaled as failed"), PanelRec.Num() == 1 && StringAt(PanelRec[0], TEXT("status")) == TEXT("failed"));
	ExpectNothingChanged(TEXT("after the denied panel run"));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Scenario 5 — the journal write after an applied edit fails (the crash
// window: the edit happened, its outcome is not durable). Through POST
// /agent/runs: insert_node lands, the outcome record cannot be written, the
// run stops. A resumed run reconciles the call against the asset, and the
// model's identical insert is refused rather than replayed — the node count is
// the evidence, since an insert is not idempotent.
// ─────────────────────────────────────────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReleaseScenarios_JournalFailureAfterApply,
	"KismetAI.ReleaseScenarios.S05_CommitWindow.JournalFailureAfterApplyIsNotReplayed", KismetAIReleaseScenarios::Flags)
bool FReleaseScenarios_JournalFailureAfterApply::RunTest(const FString& Parameters)
{
	using namespace KismetAIReleaseScenarios;
	FScopedModuleSeams Seams(FScopedModuleSeams::EPolicy::Production);
	if (FAccess::Session(Seams.Module).IsRunning()) { AddError(TEXT("a run is already active")); return false; }
	KismetAIPersistence::FScopedEvalModeOverride Autosave(false);
	FBlueprintFixture F(TEXT("S05"));
	if (!TestTrue(TEXT("fixture"), F.IsValid())) return false;
	auto NodeCount = [&F]() { UBlueprint* BP = F.BP(); return BP && BP->UbergraphPages.Num() ? BP->UbergraphPages[0]->Nodes.Num() : -1; };
	const int32 NodesBefore = NodeCount();
	// The failed write is logged at Error level by design.
	AddExpectedError(TEXT("Session journal write failed"), EAutomationExpectedErrorFlags::Contains, 1);

	// The first agent's outcome record for the insert cannot be written.
	TSharedRef<std::atomic<bool>> bFailNext = MakeShared<std::atomic<bool>>(true);
	Seams.Seams->ConfigureAgent = [bFailNext](FKismetAIAgent& Agent)
	{
		Agent.SetJournalAppendHookForTests([bFailNext](const TSharedPtr<FJsonObject>& Entry)
		{
			FString Record, Id;
			const bool bInsertOutcome = Entry.IsValid() && Entry->TryGetStringField(TEXT("record"), Record)
				&& Record == KismetAIToolCallJournal::RecordToolCall
				&& Entry->TryGetStringField(TEXT("tool_use_id"), Id) && Id == TEXT("toolu_ins");
			return !(bInsertOutcome && bFailNext->exchange(false));
		});
	};
	TSharedPtr<FJsonObject> Insert = MakeShared<FJsonObject>();
	Insert->SetStringField(TEXT("blueprint"), F.ObjectPath());
	Insert->SetStringField(TEXT("node"), TEXT("Print String"));
	Seams.Seams->Model.Script = {
		ToolTurn(TEXT("toolu_ins"), TEXT("insert_node"), Insert),
		ToolTurn(TEXT("toolu_again"), TEXT("insert_node"), Insert),
		TextTurn(TEXT("Done.")) };

	FString SessionId;
	const FString RunId = SubmitRun(*this, RunBody(TEXT("Add a Print String."), SessionId, { TEXT("edit") }, F.Mount.Scope()), &SessionId);
	if (RunId.IsEmpty()) return false;
	ON_SCOPE_EXIT { SettleRun(Seams.Module, RunId, Seams.Seams->Model); };
	FinishRun(*this, Seams, RunId);
	ExpectSessionHeader(*this, SessionId);

	TestEqual(TEXT("the insert landed"), NodeCount(), NodesBefore + 1);
	TestEqual(TEXT("the run stopped at the failed write (one model call)"), Seams.Seams->Model.Calls.load(), 1);
	const TSharedPtr<FJsonObject> First = Outcome(RunId);
	TestEqual(TEXT("the run's one terminal event is the error"), StringAt(First, TEXT("terminal_event")), FString(TEXT("error")));
	TestEqual(TEXT("no outcome record for the insert"), CallRecords(SessionId, TEXT("toolu_ins")).Num(), 0);
	{
		const FSessionSnapshot Snap = FKismetAISessionStorage::LoadSession(SessionId);
		TestEqual(TEXT("the reader lists the insert as uncertain"), Snap.UncertainCalls.Num(), 1);
	}

	// Resume. The model sends the identical insert again.
	const FString Resumed = SubmitRun(*this, RunBody(TEXT("Continue."), SessionId, { TEXT("edit") }, F.Mount.Scope()));
	if (Resumed.IsEmpty()) return false;
	ON_SCOPE_EXIT { SettleRun(Seams.Module, Resumed, Seams.Seams->Model); };
	FinishRun(*this, Seams, Resumed);

	TestEqual(TEXT("no second node: nothing replayed"), NodeCount(), NodesBefore + 1);
	const FSessionSnapshot Snap = FKismetAISessionStorage::LoadSession(SessionId);
	if (TestEqual(TEXT("one reconciliation"), Snap.Reconciliations.Num(), 1))
	{
		TestTrue(TEXT("changed_since_intent: the asset shows the insert"),
			Snap.Reconciliations[0].Verdict == EKismetAIReconcileVerdict::ChangedSinceIntent);
	}
	const TArray<TSharedPtr<FJsonObject>> Again = CallRecords(SessionId, TEXT("toolu_again"));
	TestTrue(TEXT("the identical insert is refused: RECONCILIATION_REQUIRED"),
		Again.Num() == 1 && Condensed(Again[0]).Contains(TEXT("RECONCILIATION_REQUIRED")));
	TestEqual(TEXT("the resumed run ends once"), CountType(Events(Resumed), TEXT("done")) + CountType(Events(Resumed), TEXT("error")), 1);
	TestEqual(TEXT("unreconciled application remains uncertain"), StringAt(Outcome(Resumed), TEXT("status")), FString(TEXT("uncertain")));
	return true;
}

// Long-session characterization: only the provider is scripted. Two real
// Blueprints, the run route, executor, context projection and disk journal are
// production. This measures transport/context invariants, not model quality.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLongSession_RealEditor,
	"KismetAI.LongSession.RealEditorCancelFaultAndResume", KismetAIReleaseScenarios::Flags)
bool FLongSession_RealEditor::RunTest(const FString& Parameters)
{
	using namespace KismetAIReleaseScenarios;
	FScopedModuleSeams Seams(FScopedModuleSeams::EPolicy::Production);
	if (FAccess::Session(Seams.Module).IsRunning()) { AddError(TEXT("a run is already active")); return false; }
	KismetAIPersistence::FScopedEvalModeOverride Autosave(false);
	FBlueprintFixture Target(TEXT("LongSessionTarget"));
	FBlueprintFixture Protected(TEXT("LongSessionProtected"));
	if (!TestTrue(TEXT("both disposable fixtures exist"), Target.IsValid() && Protected.IsValid())) return false;
	const auto ProtectedBefore = Protected.Capture();
	const int32 NodeCount = Target.BP()->UbergraphPages[0]->Nodes.Num();
	const FString SessionId = FKismetAISessionStorage::CreateSessionId();
	const FString Original = TEXT("Change both assets; delete unused nodes.");
	FString Correction;
	while (Correction.Len() < 12000) Correction += TEXT("Preserve existing graph nodes, pin names and links. ");
	Correction += TEXT("FINAL CORRECTION: only change the two PrintString values in ") + Target.Path
		+ TEXT(". Never edit ") + Protected.Path + TEXT(". Never delete nodes. Do not repeat a committed edit.");
	auto Append = [&](const TCHAR* Type, const TSharedPtr<FJsonObject>& Entry)
	{
		Entry->SetStringField(TEXT("type"), Type); Entry->SetStringField(TEXT("uuid"), FGuid::NewGuid().ToString());
		TestTrue(TEXT("history fixture persisted"), FKismetAISessionStorage::AppendEntry(SessionId, Entry));
	};
	auto User = [&](const FString& Text)
	{
		auto Entry = MakeShared<FJsonObject>(); Entry->SetStringField(TEXT("content"), Text); Append(TEXT("user_message"), Entry);
	};
	auto Header = MakeShared<FJsonObject>(); Header->SetNumberField(TEXT("v"), 1); Header->SetStringField(TEXT("model"), TEXT("claude-sonnet-4-6"));
	Append(TEXT("session_start"), Header); User(Original);
	auto Read = MakeShared<FJsonObject>(); Read->SetStringField(TEXT("blueprint"), Target.ObjectPath());
	for (int32 I = 0; I < 8; ++I)
	{
		const FString Id = FString::Printf(TEXT("long_history_%d"), I);
		auto A = MakeShared<FJsonObject>(); A->SetArrayField(TEXT("content"), { ToolUseBlock(Id, TEXT("read_blueprint"), Read) });
		Append(TEXT("assistant_message"), A);
		auto Block = MakeShared<FJsonObject>(); Block->SetStringField(TEXT("type"), TEXT("tool_result"));
		Block->SetStringField(TEXT("tool_use_id"), Id); Block->SetStringField(TEXT("content"), TEXT("{\"nodes\":[]}"));
		auto R = MakeShared<FJsonObject>(); R->SetArrayField(TEXT("results"), { MakeShared<FJsonValueObject>(Block) });
		Append(TEXT("tool_result_message"), R);
		if (I == 1) User(Correction);
	}
	const auto OldConfig = FAccess::Config(Seams.Module);
	ON_SCOPE_EXIT { FAccess::Config(Seams.Module) = OldConfig; };
	FAccess::Config(Seams.Module).Model = TEXT("claude-sonnet-4-6");
	auto PressureRead = [&](const TCHAR* Id)
	{
		auto Turn = ToolTurn(Id, TEXT("read_blueprint"), Read);
		Turn.Response->GetObjectField(TEXT("usage"))->SetNumberField(TEXT("input_tokens"),
			static_cast<int32>((FKismetAIAgent::GetModelContextLimit(TEXT("claude-sonnet-4-6")) - FKismetAIContextBudget::OutputReserveCap) * 0.97));
		return Turn;
	};
	FModelCallOutcome Fault; Fault.Result = EModelCallResult::FatalError; Fault.ErrorMessage = TEXT("local scripted provider interruption"); Fault.HttpStatusCode = 503;
	Seams.Seams->Model.Script = {
		PressureRead(TEXT("long_read1")), TextTurn(TEXT("<summary>Prior graph inspection.</summary>")),
		ResponseTurn({ ToolUseBlock(TEXT("long_A"), TEXT("edit_blueprint"), SetTextArgs(Target, Target.AGuid, TEXT("A-committed"))),
			ToolUseBlock(TEXT("long_B_cancelled"), TEXT("edit_blueprint"), SetTextArgs(Target, Target.BGuid, TEXT("B-never"))) }, TEXT("tool_use")),
		PressureRead(TEXT("long_read2")), TextTurn(TEXT("<summary>A is committed; B remains to do.</summary>")), Fault,
		ToolTurn(TEXT("long_fresh_read"), TEXT("read_blueprint"), Read),
		ToolTurn(TEXT("long_B_final"), TEXT("edit_blueprint"), SetTextArgs(Target, Target.BGuid, TEXT("B-completed"))),
		ToolTurn(TEXT("long_compile"), TEXT("compile_blueprint"), Read), TextTurn(TEXT("Finished the remaining operation.")) };
	TSharedRef<FLatch> AJournaled = MakeShared<FLatch>();
	TSharedRef<FLatch> StopPosted = MakeShared<FLatch>();
	Seams.Seams->ConfigureAgent = [AJournaled, StopPosted](FKismetAIAgent& Agent)
	{
		Agent.SetJournalAppendHookForTests([AJournaled, StopPosted](const TSharedPtr<FJsonObject>& Entry)
		{
			if (StringAt(Entry, TEXT("record")) == KismetAIToolCallJournal::RecordToolCall && StringAt(Entry, TEXT("tool_use_id")) == TEXT("long_A"))
			{
				AJournaled->Trigger(); StopPosted->Wait(PromptMs);
			}
			return true;
		});
	};
	const FString First = SubmitRun(*this, RunBody(TEXT("Apply the corrected target values."), SessionId, { TEXT("edit") }, Target.Mount.Scope()));
	if (First.IsEmpty()) return false;
	ON_SCOPE_EXIT { StopPosted->Trigger(); SettleRun(Seams.Module, First, Seams.Seams->Model); };
	if (!TestTrue(TEXT("first real mutation reached journal boundary"), PumpUntil([&]() { return AJournaled->Wait(0); }))) return false;
	TestEqual(TEXT("real cancel route accepted"), PostCancel(First).Code, 202); StopPosted->Trigger();
	if (!FinishRun(*this, Seams, First)) return false;
	TestEqual(TEXT("cancelled run ends exactly once"), CountType(Events(First), TEXT("done")) + CountType(Events(First), TEXT("error")), 1);
	TestEqual(TEXT("cancelled run reports aborted"), StringAt(Outcome(First), TEXT("status")), FString(TEXT("aborted")));
	TestEqual(TEXT("first committed effect survived cancel"), Target.Text(Target.AGuid), FString(TEXT("A-committed")));
	TestEqual(TEXT("second mutation did not start"), Target.Text(Target.BGuid), FString(TEXT("B-baseline")));
	const auto Cancelled = CallRecords(SessionId, TEXT("long_B_cancelled"));
	if (TestEqual(TEXT("cancelled call durable exactly once"), Cancelled.Num(), 1))
		TestEqual(TEXT("cancelled disposition is not_started"), StringAt(Cancelled[0], TEXT("status")), FString(TEXT("not_started")));
	Seams.Seams->ConfigureAgent = nullptr;
	const int32 AfterCancelRequestIndex = Seams.Seams->Model.Calls.load();
	const FString Second = SubmitRun(*this, RunBody(TEXT("Inspect before continuing; keep the committed A value."), SessionId, { TEXT("edit") }, Target.Mount.Scope()));
	if (Second.IsEmpty()) return false;
	ON_SCOPE_EXIT { SettleRun(Seams.Module, Second, Seams.Seams->Model); };
	if (!FinishRun(*this, Seams, Second)) return false;
	TestEqual(TEXT("provider interruption is one terminal event"), CountType(Events(Second), TEXT("done")) + CountType(Events(Second), TEXT("error")), 1);
	TestEqual(TEXT("provider failure did not repeat A"), Target.Text(Target.AGuid), FString(TEXT("A-committed")));
	TestEqual(TEXT("provider failure left pending B unchanged"), Target.Text(Target.BGuid), FString(TEXT("B-baseline")));
	const auto Interrupted = FKismetAISessionStorage::LoadSession(SessionId);
	TestTrue(TEXT("interrupted session remains recoverable"), Interrupted.CanResume());
	TestTrue(TEXT("two context compactions survive interruption"), Interrupted.ContextReplay.AppliedRecords >= 2);
	TestEqual(TEXT("no projection loss"), Interrupted.ContextReplay.NotReproducedRecords, 0);
	FAccess::Config(Seams.Module).Model = TEXT("claude-haiku-4-5-20251001");
	const FString ResumeInstruction = TEXT("Keep A unchanged. Read current state, change only B to B-completed, then compile. Never edit the protected asset.");
	const int32 AfterFaultRequestIndex = Seams.Seams->Model.Calls.load();
	const FString Third = SubmitRun(*this, RunBody(ResumeInstruction, SessionId, { TEXT("edit") }, Target.Mount.Scope()));
	if (Third.IsEmpty()) return false;
	ON_SCOPE_EXIT { SettleRun(Seams.Module, Third, Seams.Seams->Model); };
	if (!FinishRun(*this, Seams, Third)) return false;
	TestEqual(TEXT("final run ends exactly once"), CountType(Events(Third), TEXT("done")) + CountType(Events(Third), TEXT("error")), 1);
	TestEqual(TEXT("final run completes"), StringAt(Outcome(Third), TEXT("status")), FString(TEXT("completed")));
	TestEqual(TEXT("typed executed-operation summary is complete"), StringAt(ObjectAt(ObjectAt(Outcome(Third), TEXT("terminal")), TEXT("completion_summary")), TEXT("status")), FString(TEXT("complete")));
	TestEqual(TEXT("A is not replayed"), Target.Text(Target.AGuid), FString(TEXT("A-committed")));
	TestEqual(TEXT("remaining intended edit completed"), Target.Text(Target.BGuid), FString(TEXT("B-completed")));
	TestEqual(TEXT("no node added or deleted"), Target.BP()->UbergraphPages[0]->Nodes.Num(), NodeCount);
	TestTrue(TEXT("existing connection preserved"), Target.ALinkedToB());
	ExpectUnchanged(*this, TEXT("protected asset across compactions, cancellation and fault"), ProtectedBefore, Protected.Capture());
	TestEqual(TEXT("A has one committed call, never replayed"), CallRecords(SessionId, TEXT("long_A")).Num(), 1);
	TestEqual(TEXT("B completed once"), CallRecords(SessionId, TEXT("long_B_final")).Num(), 1);
	const auto Final = FKismetAISessionStorage::LoadSession(SessionId);
	int32 AppliedMutations = 0;
	for (const auto& Call : Final.CommittedCalls)
	{
		const auto Mutation = ObjectAt(Call, TEXT("mutation_outcome"));
		if (Mutation && Mutation->GetBoolField(TEXT("applied_in_memory")) && StringAt(Call, TEXT("tool_name")) == TEXT("edit_blueprint")) ++AppliedMutations;
	}
	TestEqual(TEXT("exactly two actual committed edits across all runs"), AppliedMutations, 2);
	const auto Compiled = CallRecords(SessionId, TEXT("long_compile"));
	if (TestEqual(TEXT("compile executed once"), Compiled.Num(), 1)) TestEqual(TEXT("compile succeeded"), StringAt(Compiled[0], TEXT("status")), FString(TEXT("succeeded")));
	TestEqual(TEXT("resumed run executes read, remaining edit and compile only"), CountType(Events(Third), TEXT("tool_call")), 3);
	TestEqual(TEXT("all ten scripted requests were consumed"), Seams.Seams->Model.Calls.load(), 10);
	const auto CapturedRequests = Seams.Seams->Model.GetRequests();
	const auto AcceptedA = CallRecords(SessionId, TEXT("long_A"));
	if (!TestEqual(TEXT("one authoritative A record for request comparison"), AcceptedA.Num(), 1)) return false;
	TestEqual(TEXT("accepted A succeeded"), StringAt(AcceptedA[0], TEXT("status")), FString(TEXT("succeeded")));
	const auto AcceptedEffect = ObjectAt(AcceptedA[0], TEXT("mutation_outcome"));
	TestTrue(TEXT("accepted A has typed applied evidence"), AcceptedEffect.IsValid() && AcceptedEffect->GetBoolField(TEXT("applied_in_memory")));
	if (Cancelled.Num() != 1) return false;
	for (const int32 RequestIndex : { AfterCancelRequestIndex, AfterFaultRequestIndex })
	{
		if (!TestTrue(TEXT("resumed main request was captured"), CapturedRequests.IsValidIndex(RequestIndex))) return false;
		for (const auto& Accepted : { AcceptedA[0], Cancelled[0] })
		{
			const auto& ExpectedResults = Accepted->GetArrayField(TEXT("results"));
			if (!TestEqual(TEXT("accepted call has one exact result block"), ExpectedResults.Num(), 1)) return false;
			const FString ExpectedId = StringAt(Accepted, TEXT("tool_use_id"));
			int32 Matches = 0;
			for (const auto& Message : CapturedRequests[RequestIndex])
			{
				const TArray<TSharedPtr<FJsonValue>>* Blocks = nullptr;
				if (!Message->AsObject()->TryGetArrayField(TEXT("content"), Blocks)) continue;
				for (const auto& Block : *Blocks)
				{
					if (StringAt(Block->AsObject(), TEXT("tool_use_id")) != ExpectedId) continue;
					++Matches;
					if (RequestIndex == AfterFaultRequestIndex && ExpectedId == TEXT("long_A"))
					{
						const FString Body = StringAt(Block->AsObject(), TEXT("content"));
						int32 Newline = INDEX_NONE; Body.FindChar(TEXT('\n'), Newline);
						TSharedPtr<FJsonObject> Hint;
						if (!TestTrue(TEXT("compacted applied outcome remains available to resumed model"), Newline != INDEX_NONE
							&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body.Mid(Newline + 1)), Hint) && Hint.IsValid())) continue;
						TestEqual(TEXT("context hint does not claim trusted provenance"), StringAt(Hint, TEXT("provenance")), FString(TEXT("reported_unverified")));
						TestTrue(TEXT("valid reported outcome is available"), Hint->GetBoolField(TEXT("outcome_available")));
						const auto Reported = ObjectAt(Hint, TEXT("reported_outcome"));
						if (!TestTrue(TEXT("reported fields exist"), Reported.IsValid())) continue;
						for (const TCHAR* Field : { TEXT("package"), TEXT("compile_status"), TEXT("save_status"), TEXT("recovery_status") })
							TestEqual(FString(TEXT("reported field equals separately accepted typed evidence: ")) + Field, StringAt(Reported, Field), StringAt(AcceptedEffect, Field));
						TestEqual(TEXT("reported application matches accepted typed evidence"), Reported->GetBoolField(TEXT("applied_in_memory")), AcceptedEffect->GetBoolField(TEXT("applied_in_memory")));
					}
					else TestEqual(TEXT("resumed model sees exact accepted outcome"), Condensed(Block->AsObject()), Condensed(ExpectedResults[0]->AsObject()));
				}
			}
			TestEqual(TEXT("committed and not-started outcomes each survive once"), Matches, 1);
		}
	}
	int32 MainRequests = 0;
	for (const auto& Request : Seams.Seams->Model.GetRequests())
	{
		int32 Uses = 0;
		TestTrue(TEXT("every captured request is paired"), WellPaired(Request, Uses));
		int32 ExactCorrections = 0;
		for (const auto& Message : Request) if (ContentText(Message) == Correction) ++ExactCorrections;
		// Summary requests intentionally contain a wrapped transcript, whereas
		// main requests retain the genuine instruction as its own exact message.
		bool bMain = false;
		for (const auto& Message : Request) if (ContentText(Message) == Original) bMain = true;
		if (bMain)
		{
			++MainRequests;
			TestEqual(TEXT("full correction remains enforceable in each main request"), ExactCorrections, 1);
		}
	}
	TestEqual(TEXT("every main request was checked for exact intent"), MainRequests, 8);
	TestTrue(TEXT("user preservation invariant survives all disruptions"), Final.bUserMessagesPreserved);
	TestEqual(TEXT("all executed calls have durable outcomes"), Final.UnrecordedCalls, 0);
	return true;
}
