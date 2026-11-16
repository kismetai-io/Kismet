// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// The same request through every production entry point.
//
// Editor tools are reachable three ways:
//   - the agent loop (Slate panel and POST /api/v1/agent/run) — the model's
//     tool_use goes through FKismetAIAgent::ExecuteTool;
//   - MCP (POST /mcp tools/call) — KismetAIMcpRoute::CallTool;
//   - REST (POST /api/v1/<route>) — the route adapter the router binds.
//
// Previously, the first two shared FKismetAIToolExecutor (registry validation,
// policy, cancellation) while REST called the Do* handlers directly after
// parsing JSON. These tests replay one malformed / out-of-scope corpus through
// all three and require the same outcome and no side effect.

#include "Misc/AutomationTest.h"
#include "KismetAIModule.h"
#include "KismetAIMcpRoute.h"
#include "KismetAIMcpServer.h"
#include "KismetAIModuleToolInvoker.h"
#include "KismetAIAgent.h"
#include "KismetAIToolExecutor.h"
#include "KismetAIToolPolicy.h"
#include "ScopedTestGrant.h"
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

constexpr auto ParityTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace KismetAIParity
{
	// What every entry point is compared on: did it succeed, and if not, which
	// stable error code did it report.
	struct FOutcome
	{
		bool bParsed = false;
		bool bSuccess = false;
		FString ErrorCode;
		FString Raw;
	};

	FOutcome FromEnvelopeString(FString Text)
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

	TSharedPtr<FJsonObject> Clone(const TSharedPtr<FJsonObject>& In)
	{
		FString S;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&S);
		FJsonSerializer::Serialize(In.ToSharedRef(), W);
		TSharedPtr<FJsonObject> Out;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(S);
		FJsonSerializer::Deserialize(R, Out);
		return Out;
	}

	// Agent loop: the path every model tool_use takes (panel and /agent/run).
	FOutcome ViaAgent(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
	{
		FKismetAIAgentConfig Config;
		FKismetAIAgent Agent(MakeUnique<FKismetAIModuleToolInvoker>(FKismetAIModule::Get()), Config);
		Agent.SetCallerContext(FKismetAICallerContext::Verified(EKismetAIEntryPoint::Panel, TEXT("editor-user")));
		return FromEnvelopeString(Agent.ExecuteToolForExternalClient(Tool, Clone(Args)));
	}

	// MCP: a real JSON-RPC tools/call through the production server and call fn.
	FOutcome ViaMcp(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
	{
		FKismetAIModule& Module = FKismetAIModule::Get();
		FKismetAIMcpServer Server(
			KismetAIMcpRoute::GetToolSchemas(Module),
			FKismetAIModuleToolInvoker::DispatchableToolNames(),
			[&Module](const FString& Name, const TSharedPtr<FJsonObject>& A)
			{
				return KismetAIMcpRoute::CallTool(Module,
					FKismetAICallerContext::Verified(EKismetAIEntryPoint::Mcp, TEXT("mcp-bearer")), Name, A);
			},
			TEXT("test"));

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("name"), Tool);
		Params->SetObjectField(TEXT("arguments"), Clone(Args));
		TSharedPtr<FJsonObject> Rpc = MakeShared<FJsonObject>();
		Rpc->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		Rpc->SetNumberField(TEXT("id"), 1);
		Rpc->SetStringField(TEXT("method"), TEXT("tools/call"));
		Rpc->SetObjectField(TEXT("params"), Params);
		FString Body;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Body);
		FJsonSerializer::Serialize(Rpc.ToSharedRef(), W);

		bool bNoContent = false;
		const FString Response = Server.HandleJsonRpc(Body, bNoContent);

		FOutcome Out;
		Out.Raw = Response;
		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Response);
		if (!FJsonSerializer::Deserialize(R, Parsed) || !Parsed.IsValid())
		{
			return Out;
		}
		const TSharedPtr<FJsonObject>* Result = nullptr;
		if (!Parsed->TryGetObjectField(TEXT("result"), Result) || !Result)
		{
			return Out;
		}
		const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
		if (!(*Result)->TryGetArrayField(TEXT("content"), Content) || !Content || Content->Num() == 0)
		{
			return Out;
		}
		FString Text;
		(*Content)[0]->AsObject()->TryGetStringField(TEXT("text"), Text);
		Out = FromEnvelopeString(Text);
		Out.Raw = Response;
		return Out;
	}

	using FRestHandler = bool (FKismetAIModule::*)(const FHttpServerRequest&, const FHttpResultCallback&);

	// REST: the route adapter the router binds for this path.
	FOutcome ViaRest(FRestHandler Handler, const TSharedPtr<FJsonObject>& Body)
	{
		return FromEnvelopeString(FKismetAIModule::Get().ExecuteHandlerDirect(Handler, Clone(Body)));
	}

	UBlueprint* MakeParityBlueprint(const TCHAR* PackagePath, const TCHAR* AssetName)
	{
		UPackage* Pkg = CreatePackage(PackagePath);
		if (!Pkg) return nullptr;
		return FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Pkg, AssetName, BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	}

	UK2Node_CallFunction* AddParityPrintNode(UEdGraph* Graph)
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

	TSharedPtr<FJsonObject> SetDurationArgs(UBlueprint* BP, UK2Node_CallFunction* Print, const TCHAR* Value)
	{
		TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
		Op->SetStringField(TEXT("action"), TEXT("set_pin_default"));
		Op->SetStringField(TEXT("node"), Print->GetName());
		Op->SetStringField(TEXT("pin"), TEXT("Duration"));
		Op->SetStringField(TEXT("value"), Value);
		TArray<TSharedPtr<FJsonValue>> Ops = { MakeShared<FJsonValueObject>(Op) };
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Args->SetArrayField(TEXT("operations"), Ops);
		return Args;
	}

	// A policy for the denied corpus: refuses chosen tools before dispatch, or
	// at the game-thread admission seam.
	class FParityPolicy final : public IKismetAIToolPolicy
	{
	public:
		virtual FPolicyVerdict Evaluate(const FPolicyContext& Context) const override
		{
			if (DenyTools.Contains(Context.ToolName))
			{
				return FPolicyVerdict::Deny(TEXT("denied by the parity policy"), TEXT("POLICY_DENIED"));
			}
			// A destructive manage_asset action, denied by what the call does
			// rather than by tool name alone.
			FString Action;
			if (Context.ToolName == TEXT("manage_asset") && Context.Arguments.IsValid()
				&& Context.Arguments->TryGetStringField(TEXT("action"), Action) && Action == TEXT("delete"))
			{
				return FPolicyVerdict::Deny(TEXT("deletion is not granted"), TEXT("POLICY_DENIED"));
			}
			return FPolicyVerdict::Allow();
		}
		virtual FPolicyVerdict EvaluateAdmission(const FPolicyContext& Context) const override
		{
			++AdmissionCalls;
			SeenAtAdmission.Add(Context.Caller.EntryPoint);
			return AdmissionDenyTools.Contains(Context.ToolName)
				? FPolicyVerdict::Deny(TEXT("denied at admission"), TEXT("ADMISSION_DENIED"))
				: FPolicyVerdict::Allow();
		}
		TSet<FString> DenyTools;
		TSet<FString> AdmissionDenyTools;
		mutable int32 AdmissionCalls = 0;
		mutable TArray<EKismetAIEntryPoint> SeenAtAdmission;
	};

	// Installs a policy on the module for the scope of a test.
	struct FScopedModulePolicy
	{
		explicit FScopedModulePolicy(const IKismetAIToolPolicy& Policy) { FKismetAIModule::Get().SetToolPolicyOverrideForTests(&Policy); }
		~FScopedModulePolicy() { FKismetAIModule::Get().SetToolPolicyOverrideForTests(nullptr); }
	};

	// One corpus entry through all three entry points; every one must refuse
	// with ExpectedCode.
	void ExpectSameRefusal(FAutomationTestBase& Test, const TCHAR* Label, const FString& Tool,
		FRestHandler RestHandler, const TSharedPtr<FJsonObject>& Args, const FString& ExpectedCode)
	{
		const FOutcome Agent = ViaAgent(Tool, Args);
		const FOutcome Mcp = ViaMcp(Tool, Args);
		const FOutcome Rest = ViaRest(RestHandler, Args);

		const TPair<const TCHAR*, const FOutcome*> All[] = {
			{ TEXT("agent"), &Agent }, { TEXT("mcp"), &Mcp }, { TEXT("rest"), &Rest } };
		for (const TPair<const TCHAR*, const FOutcome*>& E : All)
		{
			const FString What = FString::Printf(TEXT("%s via %s"), Label, E.Key);
			Test.TestTrue(What + TEXT(": parsed"), E.Value->bParsed);
			Test.TestFalse(What + TEXT(": refused"), E.Value->bSuccess);
			if (E.Value->ErrorCode != ExpectedCode)
			{
				Test.AddError(FString::Printf(TEXT("%s: error_code '%s', expected '%s'. Raw: %s"),
					*What, *E.Value->ErrorCode, *ExpectedCode, *E.Value->Raw.Left(400)));
			}
		}
	}
}

// A batch whose first operation is valid and whose second is malformed. The
// agent and MCP reject the batch before touching the graph; REST used to apply
// operation 0 (atomic defaults to false) and then fail operation 1.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExecutionParity_MalformedEditBatch, "KismetAI.ExecutionParity.MalformedEditBatch", ParityTestFlags)
bool FExecutionParity_MalformedEditBatch::RunTest(const FString& Parameters)
{
	using namespace KismetAIParity;

	UBlueprint* BP = MakeParityBlueprint(TEXT("/Temp/KismetP104ParityEdit"), TEXT("BP_KismetP104ParityEdit"));
	if (!BP || BP->UbergraphPages.Num() == 0)
	{
		AddError(TEXT("could not create test blueprint"));
		return false;
	}
	UK2Node_CallFunction* Print = AddParityPrintNode(BP->UbergraphPages[0]);
	UEdGraphPin* DurationPin = Print ? Print->FindPin(TEXT("Duration"), EGPD_Input) : nullptr;
	if (!DurationPin)
	{
		AddError(TEXT("PrintString Duration pin not found"));
		return false;
	}
	const FString OriginalDuration = DurationPin->DefaultValue;

	TSharedPtr<FJsonObject> Valid = MakeShared<FJsonObject>();
	Valid->SetStringField(TEXT("action"), TEXT("set_pin_default"));
	Valid->SetStringField(TEXT("node"), Print->GetName());
	Valid->SetStringField(TEXT("pin"), TEXT("Duration"));
	Valid->SetStringField(TEXT("value"), TEXT("7.5"));

	TSharedPtr<FJsonObject> Malformed = MakeShared<FJsonObject>();
	Malformed->SetStringField(TEXT("action"), TEXT("connect"));
	Malformed->SetStringField(TEXT("source_node"), Print->GetName());
	Malformed->SetStringField(TEXT("source_pin"), TEXT("then"));
	// target_node / target_pin missing.

	TArray<TSharedPtr<FJsonValue>> Ops;
	Ops.Add(MakeShared<FJsonValueObject>(Valid));
	Ops.Add(MakeShared<FJsonValueObject>(Malformed));

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), BP->GetPathName());
	Args->SetArrayField(TEXT("operations"), Ops);

	ExpectSameRefusal(*this, TEXT("malformed edit batch"), TEXT("edit_blueprint"),
		&FKismetAIModule::HandleEditGraph, Args, TEXT("VALIDATION_ERROR"));

	TestEqual(TEXT("no entry point applied the valid half of the batch"), DurationPin->DefaultValue, OriginalDuration);
	return true;
}

// read_source: restrictions the validator enforces must hold on every route.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExecutionParity_ReadSourceRestrictions, "KismetAI.ExecutionParity.ReadSourceRestrictions", ParityTestFlags)
bool FExecutionParity_ReadSourceRestrictions::RunTest(const FString& Parameters)
{
	using namespace KismetAIParity;

	// An unknown scope must not be silently searched as "nothing" (success, 0
	// results) on one route and rejected on another.
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("mode"), TEXT("find"));
		Args->SetStringField(TEXT("pattern"), TEXT("KismetAI*"));
		Args->SetStringField(TEXT("scope"), TEXT("everywhere"));
		ExpectSameRefusal(*this, TEXT("unknown scope"), TEXT("read_source"),
			&FKismetAIModule::HandleReadSource, Args, TEXT("INVALID_SCOPE"));
	}

	// An unknown mode must not fall through to a different reader.
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("mode"), TEXT("everything"));
		Args->SetStringField(TEXT("file"), TEXT("Source/KismetDevProject/KismetDevProject.h"));
		ExpectSameRefusal(*this, TEXT("unknown mode"), TEXT("read_source"),
			&FKismetAIModule::HandleReadSource, Args, TEXT("VALIDATION_ERROR"));
	}

	// Unsupported recursive syntax is refused consistently; broad extension
	// globs are allowed under the independent traversal budgets.
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("mode"), TEXT("find"));
		Args->SetStringField(TEXT("pattern"), TEXT("**.h"));
		Args->SetStringField(TEXT("scope"), TEXT("engine"));
		ExpectSameRefusal(*this, TEXT("unsupported recursive pattern"), TEXT("read_source"),
			&FKismetAIModule::HandleReadSource, Args, TEXT("UNSUPPORTED_PATTERN"));
	}

	// Traversal is refused before the reader runs, on every route.
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("mode"), TEXT("raw"));
		Args->SetStringField(TEXT("file"), TEXT("../../../../etc/hosts"));
		ExpectSameRefusal(*this, TEXT("traversal"), TEXT("read_source"),
			&FKismetAIModule::HandleReadSource, Args, TEXT("VALIDATION_ERROR"));
	}
	return true;
}

// An invalid asset path for a creating operation: same code everywhere, and no
// package appears.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExecutionParity_InvalidAssetPath, "KismetAI.ExecutionParity.InvalidAssetPath", ParityTestFlags)
bool FExecutionParity_InvalidAssetPath::RunTest(const FString& Parameters)
{
	using namespace KismetAIParity;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_type"), TEXT("Blueprint"));
	Args->SetStringField(TEXT("name"), TEXT("BP_KismetP104OutOfScope"));
	Args->SetStringField(TEXT("path"), TEXT("/Engine/KismetP104OutOfScope"));

	ExpectSameRefusal(*this, TEXT("create outside /Game"), TEXT("create_asset"),
		&FKismetAIModule::HandleCreateAsset, Args, TEXT("VALIDATION_ERROR"));

	TestNull(TEXT("no package was created"),
		FindPackage(nullptr, TEXT("/Engine/KismetP104OutOfScope/BP_KismetP104OutOfScope")));
	return true;
}

// Denied corpus: one module policy governs every entry point and every route
// alias, and a denied destructive operation leaves the asset in place.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExecutionParity_PolicyDeniesEveryRoute, "KismetAI.ExecutionParity.PolicyDeniesEveryRoute", ParityTestFlags)
bool FExecutionParity_PolicyDeniesEveryRoute::RunTest(const FString& Parameters)
{
	using namespace KismetAIParity;

	UBlueprint* BP = MakeParityBlueprint(TEXT("/Temp/KismetP104ParityDeny"), TEXT("BP_KismetP104ParityDeny"));
	if (!BP)
	{
		AddError(TEXT("could not create test blueprint"));
		return false;
	}
	const FString BPPath = BP->GetPathName();

	FParityPolicy Policy;
	Policy.DenyTools.Add(TEXT("read_blueprint"));
	FScopedModulePolicy Scoped(Policy);

	// Destructive: delete the asset. Same refusal everywhere; the asset survives.
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("action"), TEXT("delete"));
		Args->SetStringField(TEXT("asset_path"), BPPath);
		ExpectSameRefusal(*this, TEXT("denied delete"), TEXT("manage_asset"),
			&FKismetAIModule::HandleManageAsset, Args, TEXT("POLICY_DENIED"));
		TestNotNull(TEXT("the asset still exists"), FindObject<UBlueprint>(nullptr, *BPPath));
	}

	// Every alias of a denied read: three agent/MCP modes, two REST routes.
	for (const TCHAR* Mode : { TEXT("describe"), TEXT("t3d"), TEXT("overview") })
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("blueprint"), BPPath);
		Args->SetStringField(TEXT("mode"), Mode);
		const FOutcome Agent = ViaAgent(TEXT("read_blueprint"), Args);
		const FOutcome Mcp = ViaMcp(TEXT("read_blueprint"), Args);
		TestEqual(FString::Printf(TEXT("agent read_blueprint %s denied"), Mode), Agent.ErrorCode, FString(TEXT("POLICY_DENIED")));
		TestEqual(FString::Printf(TEXT("mcp read_blueprint %s denied"), Mode), Mcp.ErrorCode, FString(TEXT("POLICY_DENIED")));
	}
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("blueprint"), BPPath);
		TestEqual(TEXT("REST /describe_graph denied"), ViaRest(&FKismetAIModule::HandleDescribeGraph, Args).ErrorCode, FString(TEXT("POLICY_DENIED")));
		TestEqual(TEXT("REST /export_t3d denied"), ViaRest(&FKismetAIModule::HandleExportT3D, Args).ErrorCode, FString(TEXT("POLICY_DENIED")));
		// A body that names another mode does not move a REST alias off the
		// tool the policy judged.
		Args->SetStringField(TEXT("mode"), TEXT("overview"));
		TestEqual(TEXT("REST /describe_graph with a mode in the body denied"),
			ViaRest(&FKismetAIModule::HandleDescribeGraph, Args).ErrorCode, FString(TEXT("POLICY_DENIED")));
	}
	return true;
}

// The game-thread admission seam on the real module path: a policy that
// refuses at admission stops the real handler for every entry point, and each
// entry point reaches admission with its own caller identity.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExecutionParity_AdmissionDeniesEveryRoute, "KismetAI.ExecutionParity.AdmissionDeniesEveryRoute", ParityTestFlags)
bool FExecutionParity_AdmissionDeniesEveryRoute::RunTest(const FString& Parameters)
{
	using namespace KismetAIParity;

	UBlueprint* BP = MakeParityBlueprint(TEXT("/Temp/KismetP104ParityAdmit"), TEXT("BP_KismetP104ParityAdmit"));
	UK2Node_CallFunction* Print = (BP && BP->UbergraphPages.Num() > 0) ? AddParityPrintNode(BP->UbergraphPages[0]) : nullptr;
	UEdGraphPin* DurationPin = Print ? Print->FindPin(TEXT("Duration"), EGPD_Input) : nullptr;
	if (!DurationPin)
	{
		AddError(TEXT("fixture setup failed"));
		return false;
	}
	const FString OriginalDuration = DurationPin->DefaultValue;

	FParityPolicy Policy;
	Policy.AdmissionDenyTools.Add(TEXT("edit_blueprint"));
	FScopedModulePolicy Scoped(Policy);

	ExpectSameRefusal(*this, TEXT("admission deny"), TEXT("edit_blueprint"),
		&FKismetAIModule::HandleEditGraph, SetDurationArgs(BP, Print, TEXT("9.0")), TEXT("ADMISSION_DENIED"));
	TestEqual(TEXT("no entry point changed the pin"), DurationPin->DefaultValue, OriginalDuration);
	TestEqual(TEXT("admission ran once per entry point"), Policy.AdmissionCalls, 3);
	TestTrue(TEXT("agent reached admission as the panel"), Policy.SeenAtAdmission.Contains(EKismetAIEntryPoint::Panel));
	TestTrue(TEXT("MCP reached admission as MCP"), Policy.SeenAtAdmission.Contains(EKismetAIEntryPoint::Mcp));
	TestTrue(TEXT("REST handler reached admission as in-process"), Policy.SeenAtAdmission.Contains(EKismetAIEntryPoint::InProcess));
	return true;
}

namespace KismetAIParity
{
	// Cancels the call's token after the executor's pre-dispatch checks, then
	// hands the call to the production invoker — cancellation arriving while
	// the call is queued for the game thread.
	class FCancelWhileQueuedInvoker final : public IKismetAIToolInvoker
	{
	public:
		FCancelWhileQueuedInvoker(IKismetAIToolInvoker& InInner, FKismetAICancellationToken InToken)
			: Inner(InInner), Token(InToken) {}
		virtual FKismetAIInvokeResult Invoke(const FKismetAIToolInvocation& Invocation) override
		{
			Token.Cancel();
			return Inner.Invoke(Invocation);
		}
		virtual const IKismetAIToolPolicy* GetPolicy() const override { return Inner.GetPolicy(); }
		virtual TSet<FString> GetDispatchableToolNames() const override { return Inner.GetDispatchableToolNames(); }
		virtual FString ResolveHandlerName(const FString& T, const FString& M) const override { return Inner.ResolveHandlerName(T, M); }
	private:
		IKismetAIToolInvoker& Inner;
		FKismetAICancellationToken Token;
	};
}

// Cancellation after normalization and validation but before invocation: the
// real handler does not run and the outcome is CANCELLED. Also: an aborted
// agent refuses before dispatch with the same code.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExecutionParity_CancelBeforeInvocation, "KismetAI.ExecutionParity.CancelBeforeInvocation", ParityTestFlags)
bool FExecutionParity_CancelBeforeInvocation::RunTest(const FString& Parameters)
{
	using namespace KismetAIParity;

	UBlueprint* BP = MakeParityBlueprint(TEXT("/Temp/KismetP104ParityCancel"), TEXT("BP_KismetP104ParityCancel"));
	UK2Node_CallFunction* Print = (BP && BP->UbergraphPages.Num() > 0) ? AddParityPrintNode(BP->UbergraphPages[0]) : nullptr;
	UEdGraphPin* DurationPin = Print ? Print->FindPin(TEXT("Duration"), EGPD_Input) : nullptr;
	if (!DurationPin)
	{
		AddError(TEXT("fixture setup failed"));
		return false;
	}
	const FString OriginalDuration = DurationPin->DefaultValue;

	// The shared service, as REST drives it, with the token cancelled while queued.
	// REST holds an explicit edit grant, so the call is refused for the
	// reason under test rather than for want of authority.
	{
		FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Rest }, EKismetAIAuthorityOp::Edit, { TEXT("/Temp/KismetP104ParityCancel") });
		FKismetAIModuleToolInvoker Real(FKismetAIModule::Get());
		FToolCallContext Ctx;
		Ctx.ToolName = TEXT("edit_blueprint");
		Ctx.Caller = FKismetAICallerContext::Verified(EKismetAIEntryPoint::Rest, TEXT("x-kismet-token"));
		Ctx.Arguments = SetDurationArgs(BP, Print, TEXT("3.0"));
		Ctx.RequestBody = Ctx.Arguments;
		FCancelWhileQueuedInvoker Cancelling(Real, Ctx.CancelToken);
		FKismetAIToolExecutor Executor(Cancelling);
		const FToolCallOutcome Out = Executor.Execute(Ctx);
		TestTrue(TEXT("service: cancelled"), Out.bWasCancelled);
		TestEqual(TEXT("service: typed code"), Out.Typed.ErrorCode, FString(TEXT("CANCELLED")));
		TestEqual(TEXT("service: pin unchanged"), DurationPin->DefaultValue, OriginalDuration);
	}

	// The agent path: Stop before the call starts.
	{
		FKismetAIAgentConfig Config;
		FKismetAIAgent Agent(MakeUnique<FKismetAIModuleToolInvoker>(FKismetAIModule::Get()), Config);
		Agent.Abort();
		const FOutcome Out = FromEnvelopeString(Agent.ExecuteToolForExternalClient(TEXT("edit_blueprint"), SetDurationArgs(BP, Print, TEXT("4.0"))));
		TestEqual(TEXT("agent: CANCELLED"), Out.ErrorCode, FString(TEXT("CANCELLED")));
		TestEqual(TEXT("agent: pin unchanged"), DurationPin->DefaultValue, OriginalDuration);
	}
	return true;
}

// A successful request: same success through every entry point, same effect.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExecutionParity_SuccessEverywhere, "KismetAI.ExecutionParity.SuccessEverywhere", ParityTestFlags)
bool FExecutionParity_SuccessEverywhere::RunTest(const FString& Parameters)
{
	using namespace KismetAIParity;

	UBlueprint* BP = MakeParityBlueprint(TEXT("/Temp/KismetP104ParityOk"), TEXT("BP_KismetP104ParityOk"));
	UK2Node_CallFunction* Print = (BP && BP->UbergraphPages.Num() > 0) ? AddParityPrintNode(BP->UbergraphPages[0]) : nullptr;
	UEdGraphPin* DurationPin = Print ? Print->FindPin(TEXT("Duration"), EGPD_Input) : nullptr;
	if (!DurationPin)
	{
		AddError(TEXT("fixture setup failed"));
		return false;
	}

	// Transports need explicit authority; in-process REST handlers keep
	// the editor's.
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Panel, EKismetAIEntryPoint::Mcp }, EKismetAIAuthorityOp::Edit,
		{ TEXT("/Temp/KismetP104ParityOk") });

	const FOutcome Agent = ViaAgent(TEXT("edit_blueprint"), SetDurationArgs(BP, Print, TEXT("1.5")));
	TestTrue(TEXT("agent succeeded"), Agent.bSuccess);
	TestEqual(TEXT("agent applied"), DurationPin->DefaultValue, FString(TEXT("1.5")));

	const FOutcome Mcp = ViaMcp(TEXT("edit_blueprint"), SetDurationArgs(BP, Print, TEXT("2.5")));
	TestTrue(TEXT("mcp succeeded"), Mcp.bSuccess);
	TestEqual(TEXT("mcp applied"), DurationPin->DefaultValue, FString(TEXT("2.5")));

	const FOutcome Rest = ViaRest(&FKismetAIModule::HandleEditGraph, SetDurationArgs(BP, Print, TEXT("3.5")));
	TestTrue(TEXT("rest succeeded"), Rest.bSuccess);
	TestEqual(TEXT("rest applied"), DurationPin->DefaultValue, FString(TEXT("3.5")));

	// A read through a REST alias and the agent's mode returns success too.
	TSharedPtr<FJsonObject> Read = MakeShared<FJsonObject>();
	Read->SetStringField(TEXT("blueprint"), BP->GetPathName());
	TestTrue(TEXT("REST /describe_graph succeeded"), ViaRest(&FKismetAIModule::HandleDescribeGraph, Read).bSuccess);
	Read->SetStringField(TEXT("mode"), TEXT("describe"));
	TestTrue(TEXT("agent read_blueprint describe succeeded"), ViaAgent(TEXT("read_blueprint"), Read).bSuccess);
	return true;
}
