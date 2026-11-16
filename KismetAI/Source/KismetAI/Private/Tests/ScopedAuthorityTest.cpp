// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Scoped authority for unattended editing, on the production path.
//
// Reproduction first: earlier an unattended caller (MCP, an HTTP agent
// run) could mutate or delete any asset it could name, because the shipping
// policy allowed every mutation and degraded "ask" to "allow" when nobody could
// be asked.

#include "Misc/AutomationTest.h"
#include "KismetAIModule.h"
#include "KismetAIMcpRoute.h"
#include "KismetAIModuleToolInvoker.h"
#include "KismetAIAgent.h"
#include "KismetAIToolExecutor.h"
#include "ScopedTestGrant.h"
#include "DisposableContent.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"

constexpr auto ScopedAuthorityTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace KismetAIScopedAuthorityTest
{
	struct FOutcome
	{
		bool bParsed = false;
		bool bSuccess = false;
		FString ErrorCode;
		FString Raw;
	};

	FOutcome AuthParse(FString Text)
	{
		FOutcome Out;
		Out.Raw = Text;
		Text.TrimStartAndEndInline();
		Text.RemoveFromStart(TEXT("<tool_use_error>"));
		Text.RemoveFromEnd(TEXT("</tool_use_error>"));
		TSharedPtr<FJsonObject> Env;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Env) || !Env.IsValid())
		{
			return Out;
		}
		Out.bParsed = true;
		Env->TryGetBoolField(TEXT("success"), Out.bSuccess);
		const TSharedPtr<FJsonObject>* Error = nullptr;
		if (Env->TryGetObjectField(TEXT("error"), Error) && Error && Error->IsValid())
		{
			(*Error)->TryGetStringField(TEXT("error_code"), Out.ErrorCode);
		}
		return Out;
	}

	FOutcome AuthViaMcp(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
	{
		return AuthParse(KismetAIMcpRoute::CallTool(FKismetAIModule::Get(),
			FKismetAICallerContext::Verified(EKismetAIEntryPoint::Mcp, TEXT("mcp-bearer")), Tool, Args));
	}

	FOutcome AuthViaAgentAs(EKismetAIEntryPoint EntryPoint, const FString& Tool, const TSharedPtr<FJsonObject>& Args)
	{
		FKismetAIAgentConfig Config;
		FKismetAIAgent Agent(MakeUnique<FKismetAIModuleToolInvoker>(FKismetAIModule::Get()), Config);
		Agent.SetCallerContext(FKismetAICallerContext::Verified(EntryPoint, TEXT("test")));
		return AuthParse(Agent.ExecuteToolForExternalClient(Tool, Args));
	}

	// An in-memory, never-saved Blueprint. Disposable: nothing touches disk.
	UBlueprint* MakeAuthBlueprint(const FString& PackagePath, const FString& AssetName)
	{
		UPackage* Pkg = CreatePackage(*PackagePath);
		if (!Pkg) return nullptr;
		return FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Pkg, *AssetName, BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	}

	UK2Node_CallFunction* AddAuthPrintNode(UEdGraph* Graph)
	{
		UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph);
		Node->FunctionReference.SetExternalMember(
			GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
			UKismetSystemLibrary::StaticClass());
		Graph->AddNode(Node, false, false);
		Node->CreateNewGuid();
		Node->AllocateDefaultPins();
		return Node;
	}

	TSharedPtr<FJsonObject> AuthSetDurationArgs(UBlueprint* BP, UK2Node_CallFunction* Print, const TCHAR* Value)
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

	TSharedPtr<FJsonObject> AuthDeleteArgs(const FString& AssetPath)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("action"), TEXT("delete"));
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		return Args;
	}

	// A granted edit autosaves outside eval mode, so the "in-memory" fixtures
	// below can reach disk. Every test removes the P105 folder when it ends
	// (the gate fails on fixtures left in Content).
	struct FP105ContentCleanup
	{
		~FP105ContentCleanup() { KismetAIDisposableContent::DeleteFixtureFolder(TEXT("/Game/KismetAITests/P105")); }
	};

	// Fixtures are in-memory, never saved, and uniquely named per test. Leave
	// them clean so nothing offers to save them; renaming a compiled Blueprint
	// out of the way is not safe mid-session.
	void AuthDiscard(UObject* Obj)
	{
		if (IsValid(Obj))
		{
			Obj->GetOutermost()->SetDirtyFlag(false);
		}
	}

	// REST as the router drives it: the tool-route adapter with the caller the
	// X-Kismet-Token gate records.
	FOutcome AuthViaRest(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
	{
		FString BodyStr;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Args.ToSharedRef(), W);
		FHttpServerRequest Req;
		FTCHARToUTF8 Utf8(*BodyStr);
		Req.Body.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		FString Captured;
		FKismetAIModule::Get().ServeToolRoute(*Tool, nullptr,
			FKismetAICallerContext::Verified(EKismetAIEntryPoint::Rest, TEXT("x-kismet-token")), Req,
			[&Captured](TUniquePtr<FHttpServerResponse>&& Response)
			{
				if (Response.IsValid())
				{
					const FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Response->Body.GetData()), Response->Body.Num());
					Captured = FString(Conv.Length(), Conv.Get());
				}
			});
		return AuthParse(Captured);
	}

	// One of the module's own routes, called the way the router calls it.
	TSharedPtr<FJsonObject> AuthCallRoute(bool (FKismetAIModule::*Route)(const FHttpServerRequest&, const FHttpResultCallback&),
		const FString& Body)
	{
		FHttpServerRequest Req;
		FTCHARToUTF8 Utf8(*Body);
		Req.Body.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		FString Captured;
		(FKismetAIModule::Get().*Route)(Req, [&Captured](TUniquePtr<FHttpServerResponse>&& Response)
		{
			if (Response.IsValid())
			{
				const FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Response->Body.GetData()), Response->Body.Num());
				Captured = FString(Conv.Length(), Conv.Get());
			}
		});
		TSharedPtr<FJsonObject> Out;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Captured);
		FJsonSerializer::Deserialize(R, Out);
		return Out;
	}

	TSharedPtr<FJsonObject> AuthCreateArgs(const FString& Path, const FString& Name)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_type"), TEXT("Blueprint"));
		Args->SetStringField(TEXT("path"), Path);
		Args->SetStringField(TEXT("name"), Name);
		Args->SetStringField(TEXT("parent_class"), TEXT("Actor"));
		return Args;
	}

	TSharedPtr<FJsonObject> AuthCompileArgs(const FString& Blueprint)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("blueprint"), Blueprint);
		return Args;
	}

	// Revokes a grant after the executor's pre-dispatch checks and before the
	// production invoker reaches the game-thread admission seam: authority
	// withdrawn while the call is queued.
	class FRevokeWhileQueuedInvoker final : public IKismetAIToolInvoker
	{
	public:
		FRevokeWhileQueuedInvoker(IKismetAIToolInvoker& InInner, FString InGrantId)
			: Inner(InInner), GrantId(MoveTemp(InGrantId)) {}
		virtual FKismetAIInvokeResult Invoke(const FKismetAIToolInvocation& Invocation) override
		{
			FKismetAIModule::Get().GetAuthorityStore().Revoke(GrantId, TEXT("automation-test"));
			return Inner.Invoke(Invocation);
		}
		virtual const IKismetAIToolPolicy* GetPolicy() const override { return Inner.GetPolicy(); }
		virtual TSet<FString> GetDispatchableToolNames() const override { return Inner.GetDispatchableToolNames(); }
		virtual FString ResolveHandlerName(const FString& T, const FString& M) const override { return Inner.ResolveHandlerName(T, M); }
	private:
		IKismetAIToolInvoker& Inner;
		FString GrantId;
	};

}

// Reproduction: an unattended MCP caller edits a project Blueprint with no grant.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScopedAuthority_UnattendedEditNeedsGrant, "KismetAI.ScopedAuthority.UnattendedEditNeedsGrant", ScopedAuthorityTestFlags)
bool FScopedAuthority_UnattendedEditNeedsGrant::RunTest(const FString& Parameters)
{
	using namespace KismetAIScopedAuthorityTest;
	FP105ContentCleanup ContentCleanup;

	UBlueprint* BP = MakeAuthBlueprint(TEXT("/Game/KismetAITests/P105/BP_P105UnattendedEdit"), TEXT("BP_P105UnattendedEdit"));
	UK2Node_CallFunction* Print = (BP && BP->UbergraphPages.Num() > 0) ? AddAuthPrintNode(BP->UbergraphPages[0]) : nullptr;
	UEdGraphPin* Duration = Print ? Print->FindPin(TEXT("Duration"), EGPD_Input) : nullptr;
	if (!Duration)
	{
		AddError(TEXT("fixture setup failed"));
		return false;
	}
	const FString Original = Duration->DefaultValue;

	const FOutcome Mcp = AuthViaMcp(TEXT("edit_blueprint"), AuthSetDurationArgs(BP, Print, TEXT("8.5")));
	TestFalse(TEXT("MCP edit without a grant is refused"), Mcp.bSuccess);
	TestEqual(TEXT("MCP: AUTHORITY_REQUIRED"), Mcp.ErrorCode, FString(TEXT("AUTHORITY_REQUIRED")));
	const FOutcome Http = AuthViaAgentAs(EKismetAIEntryPoint::HttpAgentRun, TEXT("edit_blueprint"), AuthSetDurationArgs(BP, Print, TEXT("9.5")));
	TestFalse(TEXT("HTTP agent-run edit without a grant is refused"), Http.bSuccess);
	TestEqual(TEXT("HTTP agent run: AUTHORITY_REQUIRED"), Http.ErrorCode, FString(TEXT("AUTHORITY_REQUIRED")));
	const FOutcome Rest = AuthViaRest(TEXT("edit_blueprint"), AuthSetDurationArgs(BP, Print, TEXT("10.5")));
	TestEqual(TEXT("REST: AUTHORITY_REQUIRED"), Rest.ErrorCode, FString(TEXT("AUTHORITY_REQUIRED")));
	TestEqual(TEXT("the pin is unchanged"), Duration->DefaultValue, Original);

	AuthDiscard(BP);
	return true;
}

// Reproduction: an unattended caller deletes a project asset with no grant.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScopedAuthority_UnattendedDeleteNeedsGrant, "KismetAI.ScopedAuthority.UnattendedDeleteNeedsGrant", ScopedAuthorityTestFlags)
bool FScopedAuthority_UnattendedDeleteNeedsGrant::RunTest(const FString& Parameters)
{
	using namespace KismetAIScopedAuthorityTest;
	FP105ContentCleanup ContentCleanup;

	UBlueprint* BP = MakeAuthBlueprint(TEXT("/Game/KismetAITests/P105/BP_P105UnattendedDelete"), TEXT("BP_P105UnattendedDelete"));
	if (!BP)
	{
		AddError(TEXT("fixture setup failed"));
		return false;
	}
	const FString Path = BP->GetPathName();

	const FOutcome Mcp = AuthViaMcp(TEXT("manage_asset"), AuthDeleteArgs(Path));
	TestFalse(TEXT("MCP delete without a grant is refused"), Mcp.bSuccess);
	TestNotNull(TEXT("the asset still exists"), FindObject<UBlueprint>(nullptr, *Path));

	AuthDiscard(FindObject<UBlueprint>(nullptr, *Path));
	return true;
}

// Deletion across panel, MCP and REST: denied without a grant, denied with a
// grant for another folder, allowed within the grant, denied after revocation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScopedAuthority_DeletionAcrossEntryPoints, "KismetAI.ScopedAuthority.DeletionAcrossEntryPoints", ScopedAuthorityTestFlags)
bool FScopedAuthority_DeletionAcrossEntryPoints::RunTest(const FString& Parameters)
{
	using namespace KismetAIScopedAuthorityTest;
	FP105ContentCleanup ContentCleanup;

	const FString Folder = TEXT("/Game/KismetAITests/P105/Del");
	TMap<FString, FString> Paths;
	for (const TCHAR* Who : { TEXT("Panel"), TEXT("Mcp"), TEXT("Rest"), TEXT("Revoked") })
	{
		const FString Name = FString::Printf(TEXT("BP_P105Del%s"), Who);
		UBlueprint* BP = MakeAuthBlueprint(Folder / Name, Name);
		if (!BP)
		{
			AddError(TEXT("fixture setup failed"));
			return false;
		}
		Paths.Add(Who, BP->GetPathName());
	}

	auto Run = [this](const TCHAR* Who, const FString& Path) -> FOutcome
	{
		const FString W(Who);
		if (W == TEXT("Panel")) return AuthViaAgentAs(EKismetAIEntryPoint::Panel, TEXT("manage_asset"), AuthDeleteArgs(Path));
		if (W == TEXT("Rest"))  return AuthViaRest(TEXT("manage_asset"), AuthDeleteArgs(Path));
		return AuthViaMcp(TEXT("manage_asset"), AuthDeleteArgs(Path));
	};
	const TCHAR* Entry[] = { TEXT("Panel"), TEXT("Mcp"), TEXT("Rest") };

	// No grant. The panel's default grant covers edit/create, not deletion, and
	// a non-interactive panel caller cannot be asked.
	for (const TCHAR* Who : Entry)
	{
		const FOutcome Out = Run(Who, Paths[Who]);
		TestFalse(FString::Printf(TEXT("%s: delete refused without a grant"), Who), Out.bSuccess);
		TestEqual(FString::Printf(TEXT("%s: AUTHORITY_REQUIRED"), Who), Out.ErrorCode, FString(TEXT("AUTHORITY_REQUIRED")));
		TestNotNull(FString::Printf(TEXT("%s: asset survives"), Who), FindObject<UBlueprint>(nullptr, *Paths[Who]));
	}

	// A delete grant for a different folder does not reach this one.
	{
		FKismetAIScopedTestGrant Elsewhere({ EKismetAIEntryPoint::Panel, EKismetAIEntryPoint::Mcp, EKismetAIEntryPoint::Rest },
			EKismetAIAuthorityOp::Delete, { TEXT("/Game/KismetAITests/P105/Elsewhere") });
		for (const TCHAR* Who : Entry)
		{
			TestFalse(FString::Printf(TEXT("%s: out-of-scope delete refused"), Who), Run(Who, Paths[Who]).bSuccess);
			TestNotNull(FString::Printf(TEXT("%s: asset survives out-of-scope"), Who), FindObject<UBlueprint>(nullptr, *Paths[Who]));
		}
	}

	// Within the grant: each entry point deletes its own asset.
	{
		FKismetAIScopedTestGrant Here({ EKismetAIEntryPoint::Panel, EKismetAIEntryPoint::Mcp, EKismetAIEntryPoint::Rest },
			EKismetAIAuthorityOp::Delete, { *Folder });
		for (const TCHAR* Who : Entry)
		{
			const FOutcome Out = Run(Who, Paths[Who]);
			TestTrue(FString::Printf(TEXT("%s: delete allowed within the grant (%s)"), Who, *Out.Raw.Left(300)), Out.bSuccess);
			TestNull(FString::Printf(TEXT("%s: asset deleted"), Who), FindObject<UBlueprint>(nullptr, *Paths[Who]));
		}
		// Revoked: the same caller, the same scope, refused again.
		Here.Revoke();
		const FOutcome After = Run(TEXT("Mcp"), Paths[TEXT("Revoked")]);
		TestEqual(TEXT("after revocation: AUTHORITY_REQUIRED"), After.ErrorCode, FString(TEXT("AUTHORITY_REQUIRED")));
		TestNotNull(TEXT("after revocation: asset survives"), FindObject<UBlueprint>(nullptr, *Paths[TEXT("Revoked")]));
	}

	AuthDiscard(FindObject<UBlueprint>(nullptr, *Paths[TEXT("Revoked")]));
	return true;
}

// Ordinary edits granted for asset folder A do not reach B: deleting, creating
// or editing in B needs its own grant, and the refusals leave no package,
// directory, node or dirty flag behind.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScopedAuthority_EditGrantStaysInScope, "KismetAI.ScopedAuthority.EditGrantStaysInScope", ScopedAuthorityTestFlags)
bool FScopedAuthority_EditGrantStaysInScope::RunTest(const FString& Parameters)
{
	using namespace KismetAIScopedAuthorityTest;
	FP105ContentCleanup ContentCleanup;

	const FString FolderA = TEXT("/Game/KismetAITests/P105/A");
	const FString FolderB = TEXT("/Game/KismetAITests/P105/B");
	UBlueprint* A = MakeAuthBlueprint(FolderA / TEXT("BP_P105A"), TEXT("BP_P105A"));
	UBlueprint* B = MakeAuthBlueprint(FolderB / TEXT("BP_P105B"), TEXT("BP_P105B"));
	UK2Node_CallFunction* PrintA = (A && A->UbergraphPages.Num()) ? AddAuthPrintNode(A->UbergraphPages[0]) : nullptr;
	UK2Node_CallFunction* PrintB = (B && B->UbergraphPages.Num()) ? AddAuthPrintNode(B->UbergraphPages[0]) : nullptr;
	if (!PrintA || !PrintB)
	{
		AddError(TEXT("fixture setup failed"));
		return false;
	}
	UPackage* PackageB = B->GetOutermost();
	PackageB->SetDirtyFlag(false);
	const int32 NodesB = B->UbergraphPages[0]->Nodes.Num();
	const FString DurationB = PrintB->FindPin(TEXT("Duration"), EGPD_Input)->DefaultValue;

	FKismetAIScopedTestGrant EditA({ EKismetAIEntryPoint::Mcp }, EKismetAIAuthorityOp::Edit, { *FolderA });

	const FOutcome EditInA = AuthViaMcp(TEXT("edit_blueprint"), AuthSetDurationArgs(A, PrintA, TEXT("4.25")));
	TestTrue(FString::Printf(TEXT("edit in A allowed (%s)"), *EditInA.Raw.Left(300)), EditInA.bSuccess);

	const FOutcome DeleteB = AuthViaMcp(TEXT("manage_asset"), AuthDeleteArgs(B->GetPathName()));
	TestEqual(TEXT("delete in B refused"), DeleteB.ErrorCode, FString(TEXT("AUTHORITY_REQUIRED")));
	TestNotNull(TEXT("B survives"), FindObject<UBlueprint>(nullptr, *B->GetPathName()));

	const FOutcome EditB = AuthViaMcp(TEXT("edit_blueprint"), AuthSetDurationArgs(B, PrintB, TEXT("6.5")));
	TestEqual(TEXT("edit in B refused"), EditB.ErrorCode, FString(TEXT("AUTHORITY_REQUIRED")));
	TestEqual(TEXT("B's pin unchanged"), PrintB->FindPin(TEXT("Duration"), EGPD_Input)->DefaultValue, DurationB);
	TestEqual(TEXT("B has no new nodes"), B->UbergraphPages[0]->Nodes.Num(), NodesB);
	TestFalse(TEXT("B's package is not dirtied"), PackageB->IsDirty());

	// Creation is its own operation class, in A and in B alike.
	const FString NewFolder = TEXT("/Game/KismetAITests/P105/BNew");
	for (const FString& Where : { FolderA, NewFolder })
	{
		const FOutcome Create = AuthViaMcp(TEXT("create_asset"), AuthCreateArgs(Where, TEXT("BP_P105Created")));
		TestEqual(FString::Printf(TEXT("create in %s refused"), *Where), Create.ErrorCode, FString(TEXT("AUTHORITY_REQUIRED")));
		const FString Pkg = Where / TEXT("BP_P105Created");
		TestNull(FString::Printf(TEXT("no package in memory for %s"), *Pkg), FindPackage(nullptr, *Pkg));
		TestFalse(FString::Printf(TEXT("no package on disk for %s"), *Pkg), FPackageName::DoesPackageExist(Pkg));
	}
	const FString NewDir = FPaths::ProjectContentDir() / TEXT("KismetAITests/P105/BNew");
	TestFalse(TEXT("no directory was created"), IFileManager::Get().DirectoryExists(*NewDir));

	AuthDiscard(A);
	AuthDiscard(B);
	return true;
}

// A grant revoked after the call passed its pre-dispatch checks — while it is
// queued for the game thread — stops it at admission, before the handler.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScopedAuthority_RevokedWhileQueued, "KismetAI.ScopedAuthority.RevokedWhileQueued", ScopedAuthorityTestFlags)
bool FScopedAuthority_RevokedWhileQueued::RunTest(const FString& Parameters)
{
	using namespace KismetAIScopedAuthorityTest;
	FP105ContentCleanup ContentCleanup;

	const FString Folder = TEXT("/Game/KismetAITests/P105/Queued");
	UBlueprint* BP = MakeAuthBlueprint(Folder / TEXT("BP_P105Queued"), TEXT("BP_P105Queued"));
	UK2Node_CallFunction* Print = (BP && BP->UbergraphPages.Num()) ? AddAuthPrintNode(BP->UbergraphPages[0]) : nullptr;
	if (!Print)
	{
		AddError(TEXT("fixture setup failed"));
		return false;
	}
	BP->GetOutermost()->SetDirtyFlag(false);
	const FString Original = Print->FindPin(TEXT("Duration"), EGPD_Input)->DefaultValue;

	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Mcp }, EKismetAIAuthorityOp::Edit, { *Folder });

	FKismetAIModuleToolInvoker Real(FKismetAIModule::Get());
	FRevokeWhileQueuedInvoker Revoking(Real, Grant.Id);
	FToolCallContext Ctx;
	Ctx.ToolName = TEXT("edit_blueprint");
	Ctx.Caller = FKismetAICallerContext::Verified(EKismetAIEntryPoint::Mcp, TEXT("mcp-bearer"));
	Ctx.Arguments = AuthSetDurationArgs(BP, Print, TEXT("7.75"));
	Ctx.RequestBody = Ctx.Arguments;
	FKismetAIToolExecutor Executor(Revoking);
	const FToolCallOutcome Out = Executor.Execute(Ctx);

	TestEqual(TEXT("refused at admission"), Out.Typed.ErrorCode, FString(TEXT("AUTHORITY_DENIED")));
	TestTrue(TEXT("reported as a denial"), Out.bWasDenied);
	TestEqual(TEXT("pin unchanged"), Print->FindPin(TEXT("Duration"), EGPD_Input)->DefaultValue, Original);
	TestFalse(TEXT("package not dirtied"), BP->GetOutermost()->IsDirty());

	AuthDiscard(BP);
	return true;
}

// A bare asset name is judged by where the handler's lookup would find it, not
// by how it was spelled: a unique leaf outside the grant is refused.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScopedAuthority_BareNameResolvesBeforeJudging, "KismetAI.ScopedAuthority.BareNameResolvesBeforeJudging", ScopedAuthorityTestFlags)
bool FScopedAuthority_BareNameResolvesBeforeJudging::RunTest(const FString& Parameters)
{
	using namespace KismetAIScopedAuthorityTest;
	FP105ContentCleanup ContentCleanup;

	UBlueprint* BP = MakeAuthBlueprint(TEXT("/Game/KismetAITests/P105/Out/BP_P105LeafOnly"), TEXT("BP_P105LeafOnly"));
	if (!BP)
	{
		AddError(TEXT("fixture setup failed"));
		return false;
	}
	BP->GetOutermost()->SetDirtyFlag(false);

	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Mcp }, EKismetAIAuthorityOp::Edit, { TEXT("/Game/KismetAITests/P105/In") });
	const FOutcome Out = AuthViaMcp(TEXT("compile_blueprint"), AuthCompileArgs(TEXT("BP_P105LeafOnly")));
	TestFalse(TEXT("refused"), Out.bSuccess);
	TestEqual(TEXT("at admission"), Out.ErrorCode, FString(TEXT("AUTHORITY_DENIED")));
	TestFalse(TEXT("not compiled into a dirty package"), BP->GetOutermost()->IsDirty());

	// The resolver found it without loading anything.
	FKismetAIAuthorityTarget T;
	T.Raw = TEXT("/Game/KismetAITests/P105/Out/BP_P105LeafOnly.BP_P105LeafOnly");
	T.Effect = EKismetAIAuthorityOp::Edit;
	const TArray<FString> Exact = FKismetAIModule::ResolveAuthorityTarget(T);
	TestTrue(TEXT("exact path resolves to its package"), Exact.Num() == 1 && Exact[0] == TEXT("/Game/KismetAITests/P105/Out/BP_P105LeafOnly"));
	T.Raw = TEXT("/Game/KismetAITests/P105/Out/BP_P105LeafOnly_C");
	const TArray<FString> Generated = FKismetAIModule::ResolveAuthorityTarget(T);
	TestTrue(TEXT("a generated-class path resolves to the Blueprint"), Generated.Num() == 1 && Generated[0] == TEXT("/Game/KismetAITests/P105/Out/BP_P105LeafOnly"));

	AuthDiscard(BP);
	return true;
}

// The panel's default authority: ordinary edits under /Game with no prompt,
// never deletion; the grant is listed, attributable and revocable.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScopedAuthority_PanelDefaultGrant, "KismetAI.ScopedAuthority.PanelDefaultGrant", ScopedAuthorityTestFlags)
bool FScopedAuthority_PanelDefaultGrant::RunTest(const FString& Parameters)
{
	using namespace KismetAIScopedAuthorityTest;
	FP105ContentCleanup ContentCleanup;

	FKismetAIAuthorityStore& Store = FKismetAIModule::Get().GetAuthorityStore();
	const FKismetAIAuthorityGrant* Default = nullptr;
	const TArray<FKismetAIAuthorityGrant> Grants = Store.GetGrants();
	for (const FKismetAIAuthorityGrant& G : Grants)
	{
		if (G.Label == FKismetAIModule::PanelDefaultGrantLabel) Default = &G;
	}
	if (!TestNotNull(TEXT("default panel grant installed"), Default))
	{
		return false;
	}
	TestTrue(TEXT("panel only"), Default->EntryPoints.Num() == 1 && Default->EntryPoints[0] == EKismetAIEntryPoint::Panel);
	TestTrue(TEXT("edit and create, not delete"), Default->Operations == (EKismetAIAuthorityOp::Edit | EKismetAIAuthorityOp::Create));

	UBlueprint* BP = MakeAuthBlueprint(TEXT("/Game/KismetAITests/P105/Panel/BP_P105Panel"), TEXT("BP_P105Panel"));
	UK2Node_CallFunction* Print = (BP && BP->UbergraphPages.Num()) ? AddAuthPrintNode(BP->UbergraphPages[0]) : nullptr;
	if (!Print)
	{
		AddError(TEXT("fixture setup failed"));
		return false;
	}
	TestTrue(TEXT("panel edits under /Game without a prompt"),
		AuthViaAgentAs(EKismetAIEntryPoint::Panel, TEXT("edit_blueprint"), AuthSetDurationArgs(BP, Print, TEXT("3.25"))).bSuccess);
	TestEqual(TEXT("applied"), Print->FindPin(TEXT("Duration"), EGPD_Input)->DefaultValue, FString(TEXT("3.25")));

	// Revoked: the panel is back to asking (here: refused, nobody to ask).
	const FString DefaultId = Default->Id;
	Store.Revoke(DefaultId, TEXT("automation-test"));
	TestEqual(TEXT("after revoking the default grant"),
		AuthViaAgentAs(EKismetAIEntryPoint::Panel, TEXT("edit_blueprint"), AuthSetDurationArgs(BP, Print, TEXT("5.5"))).ErrorCode,
		FString(TEXT("AUTHORITY_REQUIRED")));
	FKismetAIModule::Get().InstallDefaultAuthority();

	// Decisions are recorded with the grant that allowed them.
	bool bFoundAdmission = false;
	for (const FKismetAIAuthorityDecision& D : Store.GetRecentDecisions(50))
	{
		bFoundAdmission |= D.Phase == TEXT("admission") && D.bAllowed && D.GrantIds.Contains(DefaultId)
			&& D.EntryPoint == EKismetAIEntryPoint::Panel;
	}
	TestTrue(TEXT("the allowed edit is attributed to the default grant"), bFoundAdmission);

	AuthDiscard(BP);
	return true;
}

// The REST controls: create, list and revoke grants; panel authority is not
// grantable over REST.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScopedAuthority_RestControls, "KismetAI.ScopedAuthority.RestControls", ScopedAuthorityTestFlags)
bool FScopedAuthority_RestControls::RunTest(const FString& Parameters)
{
	using namespace KismetAIScopedAuthorityTest;
	FP105ContentCleanup ContentCleanup;

	auto ErrorCode = [](const TSharedPtr<FJsonObject>& Resp)
	{
		FString Code;
		const TSharedPtr<FJsonObject>* Err = nullptr;
		if (Resp.IsValid() && Resp->TryGetObjectField(TEXT("error"), Err) && Err)
		{
			(*Err)->TryGetStringField(TEXT("error_code"), Code);
		}
		return Code;
	};

	TestEqual(TEXT("panel authority is not grantable over REST"),
		ErrorCode(AuthCallRoute(&FKismetAIModule::HandleAuthorityGrant,
			TEXT("{\"entry_points\":[\"panel\"],\"operations\":[\"delete\"],\"scope\":[\"/Game\"]}"))),
		FString(TEXT("ENTRY_POINT_NOT_GRANTABLE")));
	TestEqual(TEXT("an unknown operation is refused"),
		ErrorCode(AuthCallRoute(&FKismetAIModule::HandleAuthorityGrant,
			TEXT("{\"entry_points\":[\"mcp\"],\"operations\":[\"everything\"],\"scope\":[\"/Game\"]}"))),
		FString(TEXT("INVALID_GRANT")));
	TestEqual(TEXT("a traversal scope is refused"),
		ErrorCode(AuthCallRoute(&FKismetAIModule::HandleAuthorityGrant,
			TEXT("{\"entry_points\":[\"mcp\"],\"operations\":[\"edit\"],\"scope\":[\"/Game/../Engine\"]}"))),
		FString(TEXT("INVALID_GRANT")));

	const TSharedPtr<FJsonObject> Created = AuthCallRoute(&FKismetAIModule::HandleAuthorityGrant,
		TEXT("{\"entry_points\":[\"mcp\"],\"operations\":[\"edit\"],\"scope\":\"/Game/KismetAITests/P105/Rest\",\"label\":\"p105-rest-test\"}"));
	FString GrantId;
	const TSharedPtr<FJsonObject>* Data = nullptr;
	const TSharedPtr<FJsonObject>* Grant = nullptr;
	if (Created.IsValid() && Created->TryGetObjectField(TEXT("data"), Data) && Data && (*Data)->TryGetObjectField(TEXT("grant"), Grant) && Grant)
	{
		(*Grant)->TryGetStringField(TEXT("id"), GrantId);
	}
	TestFalse(TEXT("grant created over REST"), GrantId.IsEmpty());
	const TOptional<FKismetAIAuthorityGrant> Stored = FKismetAIModule::Get().GetAuthorityStore().FindGrant(GrantId);
	TestTrue(TEXT("recorded as created by the token holder"), Stored.IsSet() && Stored->CreatedBy == TEXT("x-kismet-token"));

	const TSharedPtr<FJsonObject> Listed = AuthCallRoute(&FKismetAIModule::HandleAuthorityList, FString());
	FString ListedJson;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&ListedJson);
	if (Listed.IsValid()) FJsonSerializer::Serialize(Listed.ToSharedRef(), W);
	TestTrue(TEXT("listed"), ListedJson.Contains(GrantId));
	TestTrue(TEXT("decisions are listed too"), ListedJson.Contains(TEXT("\"decisions\"")));

	const TSharedPtr<FJsonObject> Revoked = AuthCallRoute(&FKismetAIModule::HandleAuthorityRevoke,
		TEXT("{\"grant_id\":\"p105-rest-test\"}"));
	double Count = 0;
	const TSharedPtr<FJsonObject>* RevData = nullptr;
	if (Revoked.IsValid() && Revoked->TryGetObjectField(TEXT("data"), RevData) && RevData) (*RevData)->TryGetNumberField(TEXT("revoked"), Count);
	TestEqual(TEXT("revoked by label"), (int32)Count, 1);
	TestFalse(TEXT("no longer active"), FKismetAIModule::Get().GetAuthorityStore().FindGrant(GrantId)->IsActive(FPlatformTime::Seconds()));
	return true;
}

// The answer channel: an approval accepts only the editor user's first answer.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScopedAuthority_AnswerChannel, "KismetAI.ScopedAuthority.AnswerChannel", ScopedAuthorityTestFlags)
bool FScopedAuthority_AnswerChannel::RunTest(const FString& Parameters)
{
	// Nothing pending: any answer is stale.
	FKismetAIAgentConfig Config;
	FKismetAIAgent Agent(MakeUnique<FKismetAIModuleToolInvoker>(FKismetAIModule::Get()), Config);
	TestFalse(TEXT("stale answer refused"), Agent.SubmitAnswer(TEXT("approval-none"), TEXT("{\"q\":\"Allow once\"}"), EKismetAIEntryPoint::Panel));
	TestFalse(TEXT("module refuses an answer nobody waits for"),
		FKismetAIModule::Get().SubmitAgentAnswer(TEXT("approval-none"), TEXT("\"Allow once\""), EKismetAIEntryPoint::Rest));
	return true;
}
