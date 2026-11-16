// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIModule.h"
#include "KismetAIEngineCompat.h"
#include "KismetAIToolResult.h"
#include "KismetAIServerAuth.h"
#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "KismetAIHttpTransport.h"
#include "HttpServerResponse.h"
#include "K2Node_IfThenElse.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/FeedbackContext.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopedSlowTask.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	constexpr auto SafetyFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	class FScopedTestSaving
	{
	public:
		FScopedTestSaving()
		{
#if UE_VERSION_OLDER_THAN(5, 8, 0)
			bPrevious = GIsSavingPackage;
			GIsSavingPackage = true;
#else
			bPrevious = UE::IsSavingPackage();
			UE::SetIsSavingPackage(true);
#endif
		}
		~FScopedTestSaving()
		{
#if UE_VERSION_OLDER_THAN(5, 8, 0)
			GIsSavingPackage = bPrevious;
#else
			UE::SetIsSavingPackage(bPrevious);
#endif
		}
	private:
		bool bPrevious = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSafety_SaveRejectsTypedWork,
	"KismetAI.EditorSafety.SaveRejectsTypedWork", SafetyFlags)
bool FEditorSafety_SaveRejectsTypedWork::RunTest(const FString& Parameters)
{
	bool bRan = false;
	FToolExecutionResult Result;
	{
		FScopedTestSaving Saving;
		Result = FKismetAIModule::Get().RunOnGameThread([&bRan]()
		{
			bRan = true;
			return FToolExecutionResult::Success();
		});
	}
	TestFalse(TEXT("no tool side effects while package serialization forbids object access"), bRan);
	TestEqual(TEXT("caller receives retryable status"), Result.StatusCode, 503);
	TestEqual(TEXT("caller knows the editor is busy"), Result.ErrorCode, FString(TEXT("EDITOR_BUSY")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSafety_GarbageCollectionRejectsWork,
	"KismetAI.EditorSafety.GarbageCollectionRejectsWork", SafetyFlags)
bool FEditorSafety_GarbageCollectionRejectsWork::RunTest(const FString& Parameters)
{
	FKismetAIModule& Module = FKismetAIModule::Get();
	bool bObservedCollection = false;
	bool bRan = false;
	FToolExecutionResult Result;
	const FDelegateHandle Handle = FCoreUObjectDelegates::GetGarbageCollectStartedDelegate().AddLambda([&]()
	{
		bObservedCollection = IsGarbageCollecting();
		Result = Module.RunOnGameThread([&]()
		{
			bRan = true;
			return FToolExecutionResult::Success();
		});
	});
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	FCoreUObjectDelegates::GetGarbageCollectStartedDelegate().Remove(Handle);
	TestTrue(TEXT("test exercised a real collection critical section"), bObservedCollection);
	TestFalse(TEXT("no tool side effects while GC owns the object graph"), bRan);
	TestEqual(TEXT("GC is reported as retryable editor busy"), Result.ErrorCode, FString(TEXT("EDITOR_BUSY")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSafety_SlowTaskRejectsReentrantWork,
	"KismetAI.EditorSafety.SlowTaskRejectsReentrantWork", SafetyFlags)
bool FEditorSafety_SlowTaskRejectsReentrantWork::RunTest(const FString& Parameters)
{
	if (!TestFalse(TEXT("test begins outside a slow task"), GIsSlowTask)) return false;
	FKismetAIModule& Module = FKismetAIModule::Get();
	FFeedbackContext Context;
	bool bObservedSlowTask = false;
	bool bObservedSaving = false;
	bool bRan = false;
	FToolExecutionResult Result;
	Context.OnStartSlowTask().AddLambda([&](const FText&)
	{
		bObservedSlowTask = GIsSlowTask;
#if UE_VERSION_OLDER_THAN(5, 8, 0)
		bObservedSaving = GIsSavingPackage;
#else
		bObservedSaving = UE::IsSavingPackage();
#endif
		Result = Module.RunOnGameThread([&]()
		{
			bRan = true;
			return FToolExecutionResult::Success();
		});
	});
	{
		// Drive the engine's modal slow-task state without creating a native window.
		FScopedSlowTask SlowTask(1.0f, FText::FromString(TEXT("Validation safety regression")), true, Context);
		SlowTask.MakeDialog();
	}
	TestTrue(TEXT("real slow-task startup invoked the reentrant callback"), bObservedSlowTask);
	TestFalse(TEXT("validation is not covered by the package-saving flag"), bObservedSaving);
	TestFalse(TEXT("no editor work during modal slow-task startup"), bRan);
	TestEqual(TEXT("slow-task caller receives retryable status"), Result.StatusCode, 503);
	TestEqual(TEXT("slow task is reported as editor busy"), Result.ErrorCode, FString(TEXT("EDITOR_BUSY")));
	TestFalse(TEXT("slow-task state is restored"), GIsSlowTask);
	bool bResumed = false;
	const FToolExecutionResult Resumed = Module.RunOnGameThread([&]()
	{
		bResumed = true;
		return FToolExecutionResult::Success();
	});
	TestTrue(TEXT("ordinary tool work resumes after validation"), bResumed && Resumed.bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSafety_SaveRejectsLegacyWork,
	"KismetAI.EditorSafety.SaveRejectsLegacyWork", SafetyFlags)
bool FEditorSafety_SaveRejectsLegacyWork::RunTest(const FString& Parameters)
{
	FString Result;
	const TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	{
		FScopedTestSaving Saving;
		// Empty arguments safely fail validation on the old path, without loading an asset.
		Result = FKismetAIModule::Get().ExecuteHandlerDirect(&FKismetAIModule::HandleCompile, Args);
	}
	TestTrue(TEXT("legacy dispatch rejects work before entering the handler"), Result.Contains(TEXT("EDITOR_BUSY")));
	TestTrue(TEXT("agent receives an explicit tool error"), Result.StartsWith(TEXT("<tool_use_error>")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSafety_WorkerDispatchUsesGameThread,
	"KismetAI.EditorSafety.WorkerDispatchUsesGameThread", SafetyFlags)
bool FEditorSafety_WorkerDispatchUsesGameThread::RunTest(const FString& Parameters)
{
	struct FWorkerState
	{
		TFuture<FToolExecutionResult> Result;
		double Deadline = FPlatformTime::Seconds() + 10.0;
	};
	class FVerifyWorker final : public IAutomationLatentCommand
	{
	public:
		FVerifyWorker(FAutomationTestBase* InTest, TSharedRef<FWorkerState> InState) : Test(InTest), State(InState) {}
		virtual bool Update() override
		{
			if (!State->Result.IsReady() && FPlatformTime::Seconds() < State->Deadline) return false;
			if (Test->TestTrue(TEXT("worker dispatch completes"), State->Result.IsReady()))
			{
				Test->TestTrue(TEXT("worker requests execute safely on the game thread"), State->Result.Get().bSuccess);
			}
			return true;
		}
	private:
		FAutomationTestBase* Test;
		TSharedRef<FWorkerState> State;
	};
	auto State = MakeShared<FWorkerState>();
	State->Result = Async(EAsyncExecution::ThreadPool, []()
	{
		return FKismetAIModule::Get().RunOnGameThread([]()
		{
			return IsInGameThread() ? FToolExecutionResult::Success()
				: FToolExecutionResult::Error(500, TEXT("WRONG_THREAD"), TEXT("Object work escaped the game thread"));
		});
	});
	ADD_LATENT_AUTOMATION_COMMAND(FVerifyWorker(this, State));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSafety_SaveRejectsHttpWork,
	"KismetAI.EditorSafety.SaveRejectsHttpWork", SafetyFlags)
bool FEditorSafety_SaveRejectsHttpWork::RunTest(const FString& Parameters)
{
	TSharedPtr<FKismetAIHttpTransport> Router = FKismetAIModule::Get().HttpRouter;
	if (!TestTrue(TEXT("product HTTP router is running"), Router.IsValid())) return false;
	TSharedPtr<FHttpServerRequest> Request = MakeShared<FHttpServerRequest>();
	KismetAIHttpCompat::SetRequestPath(*Request, TEXT("/api/v1/compile"));
	Request->Verb = EHttpServerRequestVerbs::VERB_POST;
	Request->Headers.Add(TEXT("X-Kismet-Token"), { KismetAIServerAuth::GetToken() });
	Request->Body = { '{', '}' };
	int32 Status = 0;
	FString Body;
	{
		FScopedTestSaving Saving;
		Router->Query(Request, [&Status, &Body](TUniquePtr<FHttpServerResponse>&& Response)
		{
			Status = static_cast<int32>(Response->Code);
			FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Response->Body.GetData()), Response->Body.Num());
			Body = FString(Text.Length(), Text.Get());
		});
	}
	TestEqual(TEXT("authenticated HTTP caller gets retryable status"), Status, 503);
	TestTrue(TEXT("HTTP body explains the busy editor"), Body.Contains(TEXT("EDITOR_BUSY")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSafety_EditSaveReload,
	"KismetAI.EditorSafety.EditSaveReload", SafetyFlags)
bool FEditorSafety_EditSaveReload::RunTest(const FString& Parameters)
{
	const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Mount = TEXT("/Game/__KismetSafety_") + Id + TEXT("/");
	const FString Directory = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("KismetSafety_") + Id) + TEXT("/");
	IFileManager::Get().MakeDirectory(*Directory, true);
	FPackageName::RegisterMountPoint(Mount, Directory);
	ON_SCOPE_EXIT
	{
		FPackageName::UnRegisterMountPoint(Mount, Directory);
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	const FString PackageName = Mount + TEXT("BP_Saved");
	UPackage* Package = CreatePackage(*PackageName);
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package,
		TEXT("BP_Saved"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("disposable Blueprint created"), Blueprint)) return false;
	const FString AssetPath = Blueprint->GetPathName();
	FKismetAIModule& Module = FKismetAIModule::Get();
	TSharedPtr<FJsonObject> Insert = MakeShared<FJsonObject>();
	Insert->SetStringField(TEXT("blueprint"), AssetPath);
	Insert->SetStringField(TEXT("graph"), TEXT("EventGraph"));
	Insert->SetStringField(TEXT("node"), TEXT("Branch"));
	const FToolExecutionResult Edited = Module.RunOnGameThread([&]() { return Module.DoInsertNode(Insert); });
	if (!TestTrue(TEXT("real tool inserts a node"), Edited.bSuccess)) return false;
	FGuid NodeGuid;
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Cast<UK2Node_IfThenElse>(Node)) NodeGuid = Node->NodeGuid;
		}
	}
	if (!TestTrue(TEXT("inserted Branch has identity"), NodeGuid.IsValid())) return false;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString EvalFlag = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMETAI_EVAL_ALLOW"));
	if (EvalFlag != TEXT("1") && !EvalFlag.Equals(TEXT("true"), ESearchCase::IgnoreCase))
	{
		TestTrue(TEXT("ordinary editor automatically persists the edit"), IFileManager::Get().FileExists(*Filename));
	}
	TSharedPtr<FJsonObject> Compile = MakeShared<FJsonObject>();
	Compile->SetStringField(TEXT("blueprint"), AssetPath);
	if (!TestTrue(TEXT("edited Blueprint compiles"), Module.RunOnGameThread([&]() { return Module.DoCompileBlueprint(Compile); }).bSuccess)) return false;
	TSharedPtr<FJsonObject> Save = MakeShared<FJsonObject>();
	Save->SetStringField(TEXT("asset_path"), AssetPath);
	Save->SetStringField(TEXT("action"), TEXT("save"));
	if (!TestTrue(TEXT("explicit save succeeds"), Module.RunOnGameThread([&]() { return Module.DoManageAsset(Save); }).bSuccess)) return false;
	TWeakObjectPtr<UBlueprint> Previous = Blueprint;
	Blueprint = nullptr;
	FText UnloadError;
	if (!TestTrue(TEXT("saved package unloads"), UPackageTools::UnloadPackages({ Package }, UnloadError)))
	{
		AddError(UnloadError.ToString());
		return false;
	}
	Package = nullptr;
	TestFalse(TEXT("original in-memory Blueprint was unloaded"), Previous.IsValid());
	Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!TestNotNull(TEXT("Blueprint reloads from disk"), Blueprint)) return false;
	bool bFound = false;
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			bFound |= Cast<UK2Node_IfThenElse>(Node) != nullptr && Node->NodeGuid == NodeGuid;
		}
	}
	TestTrue(TEXT("same inserted node survives save and disk reload"), bFound);
	TestTrue(TEXT("reloaded Blueprint compiles"), Module.RunOnGameThread([&]() { return Module.DoCompileBlueprint(Compile); }).bSuccess);
	return true;
}
