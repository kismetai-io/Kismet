// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Misc/AutomationTest.h"
#include "DisposableContent.h"
#include "RunEntryHarness.h"
#include "ScopedTestGrant.h"
#include "KismetAIAgent.h"
#include "KismetAIMcpRoute.h"
#include "KismetAIModuleToolInvoker.h"
#include "KismetAIToolExecutor.h"
#include "KismetAIPersistence.h"
#include "KismetAIReadRevision.h"
#include "KismetAISessionStorage.h"
#include "Misc/ScopeExit.h"
#include "BlueprintActionDatabase.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

namespace KismetAIReadRevisionTest
{
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	struct FFixture
	{
		KismetAIDisposableContent::FScopedDisposableMount Mount{ TEXT("ReadRevision") };
		TArray<UBlueprint*> Blueprints;
		UBlueprint* Blueprint = nullptr;
		UK2Node_CallFunction* Print = nullptr;

		FFixture() { Blueprint = Create(TEXT("BP_Revision"), Print); }
		~FFixture()
		{
			for (UBlueprint* BP : Blueprints)
			{
				FBlueprintActionDatabase::Get().ClearAssetActions(BP);
				for (UClass* Class : { BP->GeneratedClass.Get(), BP->SkeletonGeneratedClass.Get() })
				{
					if (Class) { Class->ClearFlags(RF_Public | RF_Standalone); Class->MarkAsGarbage(); }
				}
				BP->ClearFlags(RF_Public | RF_Standalone);
				BP->GetOutermost()->SetDirtyFlag(false);
				BP->MarkAsGarbage();
			}
			KismetAIDisposableContent::ForgetUndoHistory();
		}

		UBlueprint* Create(const TCHAR* Name, UK2Node_CallFunction*& OutPrint)
		{
			UPackage* Package = CreatePackage(*Mount.PackageName(Name));
			UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, Name,
				BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
			if (!BP || BP->UbergraphPages.IsEmpty()) return nullptr;
			Blueprints.Add(BP);
			UEdGraph* Graph = BP->UbergraphPages[0];
			OutPrint = NewObject<UK2Node_CallFunction>(Graph);
			OutPrint->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString),
				UKismetSystemLibrary::StaticClass());
			Graph->AddNode(OutPrint, false, false);
			OutPrint->CreateNewGuid();
			OutPrint->AllocateDefaultPins();
			FKismetEditorUtilities::CompileBlueprint(BP);
			FKismetAIMutationOutcome Saved;
			KismetAIPersistence::SaveExplicitly(BP, Saved);
			return BP;
		}

		TSharedPtr<FJsonObject> Read(UBlueprint* Target = nullptr, const TCHAR* Mode = TEXT("describe")) const
		{
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("blueprint"), (Target ? Target : Blueprint)->GetPathName());
			Args->SetStringField(TEXT("mode"), Mode);
			return Args;
		}

		TSharedPtr<FJsonObject> Edit(const FString& Revision, const TCHAR* Value = TEXT("6.25")) const
		{
			TSharedPtr<FJsonObject> Args = Read();
			Args->RemoveField(TEXT("mode"));
			if (!Revision.IsEmpty()) Args->SetStringField(TEXT("expected_revision"), Revision);
			TSharedPtr<FJsonObject> Operation = MakeShared<FJsonObject>();
			Operation->SetStringField(TEXT("action"), TEXT("set_pin_default"));
			Operation->SetStringField(TEXT("node"), Print->GetName());
			Operation->SetStringField(TEXT("pin"), TEXT("Duration"));
			Operation->SetStringField(TEXT("value"), Value);
			Args->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(Operation) });
			return Args;
		}
		UEdGraphPin* Pin() const { return Print->FindPin(TEXT("Duration")); }
	};

	FToolCallOutcome Call(const TCHAR* Tool, const TSharedPtr<FJsonObject>& Args)
	{
		FKismetAIModuleToolInvoker Invoker(FKismetAIModule::Get());
		FKismetAIToolExecutor Executor(Invoker);
		FToolCallContext Context;
		Context.ToolName = Tool;
		Context.Caller = FKismetAICallerContext::Verified(EKismetAIEntryPoint::Panel, TEXT("read-revision-test"));
		Context.Arguments = Args;
		Context.RequestBody = Args;
		return Executor.Execute(Context);
	}

	FString Revision(const FToolCallOutcome& Result)
	{
		FString Value;
		if (Result.Typed.Data) Result.Typed.Data->TryGetStringField(TEXT("asset_revision"), Value);
		return Value;
	}

	class FInterleavingPolicy final : public IKismetAIToolPolicy
	{
	public:
		mutable TFunction<void()> AtAdmission;
		virtual FPolicyVerdict Evaluate(const FPolicyContext&) const override { return FPolicyVerdict::Allow(); }
		virtual FPolicyVerdict EvaluateAdmission(const FPolicyContext&) const override
		{
			if (AtAdmission)
			{
				TFunction<void()> Work = MoveTemp(AtAdmission);
				Work();
			}
			return FPolicyVerdict::Allow();
		}
	};

	struct FScopedPolicy
	{
		explicit FScopedPolicy(const IKismetAIToolPolicy& Policy) { FKismetAIModule::Get().SetToolPolicyOverrideForTests(&Policy); }
		~FScopedPolicy() { FKismetAIModule::Get().SetToolPolicyOverrideForTests(nullptr); }
	};

	TSharedPtr<FJsonObject> Parse(FString Text)
	{
		Text.RemoveFromStart(TEXT("<tool_use_error>"));
		Text.RemoveFromEnd(TEXT("</tool_use_error>"));
		TSharedPtr<FJsonObject> Result;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Result);
		return Result;
	}

	TSharedPtr<FJsonObject> CallEntry(EKismetAIEntryPoint Entry, const TSharedPtr<FJsonObject>& Args)
	{
		FKismetAIModule& Module = FKismetAIModule::Get();
		const auto Caller = FKismetAICallerContext::Verified(Entry, TEXT("read-revision-test"));
		if (Entry == EKismetAIEntryPoint::Mcp)
			return Parse(KismetAIMcpRoute::CallTool(Module, Caller, TEXT("edit_blueprint"), Args));
		if (Entry == EKismetAIEntryPoint::Panel)
		{
			FKismetAIAgent Agent(MakeUnique<FKismetAIModuleToolInvoker>(Module), FKismetAIAgentConfig());
			Agent.SetCallerContext(Caller);
			return Parse(Agent.ExecuteToolForExternalClient(TEXT("edit_blueprint"), Args));
		}
		FString Text;
		FJsonSerializer::Serialize(Args.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
		FTCHARToUTF8 Utf8(*Text);
		FHttpServerRequest Request;
		Request.Body.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		TSharedPtr<FJsonObject> Reply;
		Module.ServeToolRoute(TEXT("edit_blueprint"), nullptr, Caller, Request,
			[&Reply](TUniquePtr<FHttpServerResponse>&& Response)
			{
				if (!Response) return;
				FUTF8ToTCHAR Decoded(reinterpret_cast<const ANSICHAR*>(Response->Body.GetData()), Response->Body.Num());
				Reply = Parse(FString(Decoded.Length(), Decoded.Get()));
			});
		return Reply;
	}
	struct FScriptResult
	{
		FString SessionId;
		TMap<FString, TSharedPtr<FJsonObject>> Tools;
		int32 Terminals = 0;
	};

	FScriptResult RunScript(const TArray<FModelCallOutcome>& Script, const FString& ResumeSession = FString(),
		TFunction<void(const FString&)> AfterTool = nullptr)
	{
		KismetAIRunEntryHarness::FSlowModelClient Model;
		Model.Script = Script;
		FKismetAIModuleToolInvoker Invoker(FKismetAIModule::Get());
		FKismetAIAgentConfig Config;
		Config.ApiKey = TEXT("unused-scripted-provider");
		Config.Model = TEXT("claude-sonnet-4-6");
		Config.MaxSteps = 8;
		Config.MaxRetries = 0;
		Config.ThinkingMode = EThinkingMode::Disabled;
		FKismetAIAgent Agent(Invoker, Model, Config);
		Agent.SetCallerContext(FKismetAICallerContext::Verified(EKismetAIEntryPoint::Panel, TEXT("read-revision-test")));
		FScriptResult Evidence;
		FOnAgentStep Step;
		Step.BindLambda([&](const FString& Type, const FString& Text)
		{
			if (Type == TEXT("done") || Type == TEXT("error")) ++Evidence.Terminals;
			if (Type != TEXT("tool_result")) return;
			const auto Event = Parse(Text);
			if (!Event) return;
			FString Id;
			Event->TryGetStringField(TEXT("tool_use_id"), Id);
			const TSharedPtr<FJsonObject>* Object = nullptr;
			TSharedPtr<FJsonObject> Result;
			if (Event->TryGetObjectField(TEXT("result"), Object)) Result = *Object;
			else
			{
				FString Raw;
				Event->TryGetStringField(TEXT("result"), Raw);
				Result = Parse(Raw);
			}
			Evidence.Tools.Add(Id, Result);
			if (AfterTool) AfterTool(Id);
		});
		Agent.RunTask(TEXT("Exercise read revision safety."), Step, FString(), ResumeSession);
		Evidence.SessionId = Agent.GetSessionId();
		return Evidence;
	}

	FString ErrorCode(const TSharedPtr<FJsonObject>& Result)
	{
		const TSharedPtr<FJsonObject>* Error = nullptr;
		FString Code;
		if (Result && Result->TryGetObjectField(TEXT("error"), Error)) (*Error)->TryGetStringField(TEXT("error_code"), Code);
		return Code;
	}

	bool Succeeded(const TSharedPtr<FJsonObject>& Result)
	{
		bool bSuccess = false;
		if (Result) Result->TryGetBoolField(TEXT("success"), bSuccess);
		return bSuccess;
	}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadRevision_HumanEdit, "KismetAI.ReadRevision.HumanEditPreserved", KismetAIReadRevisionTest::Flags)
bool FReadRevision_HumanEdit::RunTest(const FString& Parameters)
{
	using namespace KismetAIReadRevisionTest;
	FFixture F;
	if (!TestNotNull(TEXT("fixture"), F.Blueprint)) return false;
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Panel }, EKismetAIAuthorityOp::Edit, { *F.Mount.Scope() });
	const auto Read = Call(TEXT("read_blueprint"), F.Read());
	TestTrue(TEXT("production graph read succeeds"), Read.Typed.bSuccess);
	FString Token = Revision(Read);
	TestFalse(TEXT("read attaches a revision"), Token.IsEmpty());
	if (Token.IsEmpty()) Token = TEXT("missing-read-revision");
	F.Blueprint->GetOutermost()->SetDirtyFlag(true);
	F.Pin()->DefaultValue = TEXT("9.75");
	const auto Stale = Call(TEXT("edit_blueprint"), F.Edit(Token));
	TestEqual(TEXT("stale admission conflicts"), Stale.Typed.ErrorCode, FString(TEXT("ASSET_REVISION_CONFLICT")));
	TestTrue(TEXT("real revision guard proves handler never started"), Stale.Typed.bHandlerNotStarted);
	TestEqual(TEXT("conflict HTTP status"), Stale.Typed.StatusCode, 409);
	TestFalse(TEXT("conflict gives reread guidance"), Stale.Typed.Suggestions.IsEmpty());
	TestEqual(TEXT("human pin value survives"), F.Pin()->DefaultValue, FString(TEXT("9.75")));
	const auto Fresh = Call(TEXT("read_blueprint"), F.Read());
	TestTrue(TEXT("fresh revision changes"), Revision(Fresh) != Token);
	TestTrue(TEXT("freshly planned edit succeeds"), Call(TEXT("edit_blueprint"), F.Edit(Revision(Fresh))).Typed.bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadRevision_CompetingEdits, "KismetAI.ReadRevision.CompetingEdits", KismetAIReadRevisionTest::Flags)
bool FReadRevision_CompetingEdits::RunTest(const FString& Parameters)
{
	using namespace KismetAIReadRevisionTest;
	FFixture F;
	if (!TestNotNull(TEXT("fixture"), F.Blueprint)) return false;
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Panel }, EKismetAIAuthorityOp::Edit, { *F.Mount.Scope() });
	const FString Token = Revision(Call(TEXT("read_blueprint"), F.Read()));
	TestFalse(TEXT("read revision exists"), Token.IsEmpty());
	const auto First = Call(TEXT("edit_blueprint"), F.Edit(Token, TEXT("7.25")));
	TestTrue(TEXT("first edit admitted on current state"), First.Typed.bSuccess);
	const auto Second = Call(TEXT("edit_blueprint"), F.Edit(Token, TEXT("8.5")));
	TestEqual(TEXT("second shared-revision edit conflicts"), Second.Typed.ErrorCode, FString(TEXT("ASSET_REVISION_CONFLICT")));
	TestTrue(TEXT("first edit preserved"), F.Pin()->DefaultValue.StartsWith(TEXT("7.25")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadRevision_StableReads, "KismetAI.ReadRevision.ReadsAndDirtyFlag", KismetAIReadRevisionTest::Flags)
bool FReadRevision_StableReads::RunTest(const FString& Parameters)
{
	using namespace KismetAIReadRevisionTest;
	FFixture F;
	if (!TestNotNull(TEXT("fixture"), F.Blueprint)) return false;
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Panel }, EKismetAIAuthorityOp::Edit, { *F.Mount.Scope() });
	UK2Node_CallFunction* OtherPrint = nullptr;
	UBlueprint* Other = F.Create(TEXT("BP_Unrelated"), OtherPrint);
	if (!TestNotNull(TEXT("unrelated fixture"), Other)) return false;
	const FString Token = Revision(Call(TEXT("read_blueprint"), F.Read()));
	TestFalse(TEXT("read revision exists"), Token.IsEmpty());
	TestTrue(TEXT("unrelated asset read succeeds"), Call(TEXT("read_blueprint"), F.Read(Other)).Typed.bSuccess);
	OtherPrint->FindPin(TEXT("Duration"))->DefaultValue = TEXT("12.0");
	TestEqual(TEXT("unrelated edit does not change token"), Revision(Call(TEXT("read_blueprint"), F.Read())), Token);
	F.Blueprint->GetOutermost()->SetDirtyFlag(!F.Blueprint->GetOutermost()->IsDirty());
	TestEqual(TEXT("dirty flag alone does not change token"), Revision(Call(TEXT("read_blueprint"), F.Read())), Token);
	TestEqual(TEXT("T3D and describe carry the same asset revision"), Revision(Call(TEXT("read_blueprint"), F.Read(nullptr, TEXT("t3d")))), Token);
	TestTrue(TEXT("read-only activity leaves mutation admissible"), Call(TEXT("edit_blueprint"), F.Edit(Token)).Typed.bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadRevision_Replacement, "KismetAI.ReadRevision.ReplacedAsset", KismetAIReadRevisionTest::Flags)
bool FReadRevision_Replacement::RunTest(const FString& Parameters)
{
	using namespace KismetAIReadRevisionTest;
	FFixture F;
	if (!TestNotNull(TEXT("fixture"), F.Blueprint)) return false;
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Panel }, EKismetAIAuthorityOp::Edit, { *F.Mount.Scope() });
	FString Token = Revision(Call(TEXT("read_blueprint"), F.Read()));
	TestFalse(TEXT("read revision exists"), Token.IsEmpty());
	if (Token.IsEmpty()) Token = TEXT("missing-read-revision");
	const FString OriginalPath = F.Blueprint->GetPathName();
	TestTrue(TEXT("human moves original package"), F.Blueprint->GetOutermost()->Rename(
		*F.Mount.PackageName(TEXT("BP_Moved")), nullptr, REN_DontCreateRedirectors | REN_NonTransactional));
	TestTrue(TEXT("old disposable package file removed before replacement"),
		IFileManager::Get().Delete(*(F.Mount.Directory / TEXT("BP_Revision.uasset")), true));
	F.Blueprint = F.Create(TEXT("BP_Revision"), F.Print);
	if (!TestNotNull(TEXT("replacement fixture"), F.Blueprint)) return false;
	TestEqual(TEXT("display/object path reused"), F.Blueprint->GetPathName(), OriginalPath);
	const FString Before = F.Pin()->DefaultValue;
	const auto Result = Call(TEXT("edit_blueprint"), F.Edit(Token));
	TestEqual(TEXT("old resource token rejected"), Result.Typed.ErrorCode, FString(TEXT("ASSET_REVISION_CONFLICT")));
	TestEqual(TEXT("replacement untouched"), F.Pin()->DefaultValue, Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadRevision_TransportParity, "KismetAI.ReadRevision.TransportParity", KismetAIReadRevisionTest::Flags)
bool FReadRevision_TransportParity::RunTest(const FString& Parameters)
{
	using namespace KismetAIReadRevisionTest;
	FFixture F;
	if (!TestNotNull(TEXT("fixture"), F.Blueprint)) return false;
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Panel, EKismetAIEntryPoint::Rest, EKismetAIEntryPoint::Mcp },
		EKismetAIAuthorityOp::Edit, { *F.Mount.Scope() });
	FString Token = Revision(Call(TEXT("read_blueprint"), F.Read()));
	TestFalse(TEXT("read revision exists"), Token.IsEmpty());
	if (Token.IsEmpty()) Token = TEXT("missing-read-revision");
	F.Pin()->DefaultValue = TEXT("9.5");
	for (EKismetAIEntryPoint Entry : { EKismetAIEntryPoint::Panel, EKismetAIEntryPoint::Rest, EKismetAIEntryPoint::Mcp })
	{
		const auto Reply = CallEntry(Entry, F.Edit(Token));
		const TSharedPtr<FJsonObject>* Error = nullptr;
		FString Code;
		if (Reply && Reply->TryGetObjectField(TEXT("error"), Error)) (*Error)->TryGetStringField(TEXT("error_code"), Code);
		TestEqual(TEXT("each transport preserves conflict code"), Code, FString(TEXT("ASSET_REVISION_CONFLICT")));
		TestEqual(TEXT("each transport preserves human edit"), F.Pin()->DefaultValue, FString(TEXT("9.5")));
	}
	TestTrue(TEXT("legacy omitted revision remains explicitly compatible"), Call(TEXT("edit_blueprint"), F.Edit(FString())).Typed.bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadRevision_AdmissionInterleave, "KismetAI.ReadRevision.AdmissionInterleave", KismetAIReadRevisionTest::Flags)
bool FReadRevision_AdmissionInterleave::RunTest(const FString& Parameters)
{
	using namespace KismetAIReadRevisionTest;
	FFixture F;
	if (!TestNotNull(TEXT("fixture"), F.Blueprint)) return false;
	const FString Token = Revision(Call(TEXT("read_blueprint"), F.Read()));
	TestFalse(TEXT("read revision exists"), Token.IsEmpty());
	FInterleavingPolicy Policy;
	FScopedPolicy Scoped(Policy);
	FToolCallOutcome Inner;
	Policy.AtAdmission = [&F, &Token, &Inner]() { Inner = Call(TEXT("edit_blueprint"), F.Edit(Token, TEXT("7.25"))); };
	const auto Outer = Call(TEXT("edit_blueprint"), F.Edit(Token, TEXT("8.5")));
	TestTrue(TEXT("intervening current-state edit succeeds"), Inner.Typed.bSuccess);
	TestEqual(TEXT("outer check observes intervening edit"), Outer.Typed.ErrorCode, FString(TEXT("ASSET_REVISION_CONFLICT")));
	TestTrue(TEXT("intervening edit preserved"), F.Pin()->DefaultValue.StartsWith(TEXT("7.25")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadRevision_MalformedExpected, "KismetAI.ReadRevision.MalformedExpectedRevision", KismetAIReadRevisionTest::Flags)
bool FReadRevision_MalformedExpected::RunTest(const FString& Parameters)
{
	using namespace KismetAIReadRevisionTest;
	FFixture F;
	if (!TestNotNull(TEXT("fixture"), F.Blueprint)) return false;
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Panel }, EKismetAIAuthorityOp::Edit, { *F.Mount.Scope() });
	const FString Before = F.Pin()->DefaultValue;
	const auto Args = F.Edit(FString());
	Args->SetNumberField(TEXT("expected_revision"), 42);
	const auto Result = Call(TEXT("edit_blueprint"), Args);
	TestEqual(TEXT("malformed revision is rejected by validation"), Result.Typed.ErrorCode, FString(TEXT("VALIDATION_ERROR")));
	TestEqual(TEXT("malformed revision cannot mutate"), F.Pin()->DefaultValue, Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadRevision_RunCarryThrough, "KismetAI.ReadRevision.RunCarriesRevision", KismetAIReadRevisionTest::Flags)
bool FReadRevision_RunCarryThrough::RunTest(const FString& Parameters)
{
	using namespace KismetAIReadRevisionTest;
	using namespace KismetAIRunEntryHarness;
	FFixture F;
	if (!TestNotNull(TEXT("fixture"), F.Blueprint)) return false;
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Panel }, EKismetAIAuthorityOp::Edit, { *F.Mount.Scope() });
	const auto StaleArgs = F.Edit(FString(), TEXT("6.25"));
	const auto AgainArgs = F.Edit(FString(), TEXT("6.5"));
	const auto FreshArgs = F.Edit(FString(), TEXT("7.25"));
	const auto AdvancedArgs = F.Edit(FString(), TEXT("7.5"));
	for (const auto& Args : { StaleArgs, AgainArgs, FreshArgs, AdvancedArgs })
	{
		Args->SetStringField(TEXT("blueprint"), F.Blueprint->GetOutermost()->GetName());
	}
	FSlowModelClient Model;
	Model.Script = {
		ToolTurn(TEXT("initial_read"), TEXT("read_blueprint"), F.Read()),
		ToolTurn(TEXT("stale_edit"), TEXT("edit_blueprint"), StaleArgs),
		ToolTurn(TEXT("still_stale"), TEXT("edit_blueprint"), AgainArgs),
		ToolTurn(TEXT("fresh_read"), TEXT("read_blueprint"), F.Read()),
		ToolTurn(TEXT("fresh_edit"), TEXT("edit_blueprint"), FreshArgs),
		ToolTurn(TEXT("advanced_edit"), TEXT("edit_blueprint"), AdvancedArgs),
		TextTurn(TEXT("done"))
	};
	FKismetAIModuleToolInvoker Invoker(FKismetAIModule::Get());
	FKismetAIAgentConfig Config;
	Config.ApiKey = TEXT("unused-scripted-provider");
	Config.Model = TEXT("claude-sonnet-4-6");
	Config.MaxSteps = 8;
	Config.MaxRetries = 0;
	Config.ThinkingMode = EThinkingMode::Disabled;
	FKismetAIAgent Agent(Invoker, Model, Config);
	Agent.SetCallerContext(FKismetAICallerContext::Verified(EKismetAIEntryPoint::Panel, TEXT("read-revision-test")));
	TMap<FString, TSharedPtr<FJsonObject>> Results;
	bool bHumanEditApplied = false;
	bool bHumanEditPreservedAtConflicts = true;
	int32 Terminals = 0;
	FOnAgentStep Step;
	Step.BindLambda([&](const FString& Type, const FString& Text)
	{
		if (Type == TEXT("done") || Type == TEXT("error")) ++Terminals;
		if (Type != TEXT("tool_result")) return;
		const auto Event = Parse(Text);
		if (!Event) return;
		FString Id;
		Event->TryGetStringField(TEXT("tool_use_id"), Id);
		const TSharedPtr<FJsonObject>* Object = nullptr;
		TSharedPtr<FJsonObject> Result;
		if (Event->TryGetObjectField(TEXT("result"), Object)) Result = *Object;
		else
		{
			FString Raw;
			Event->TryGetStringField(TEXT("result"), Raw);
			Result = Parse(Raw);
		}
		Results.Add(Id, Result);
		if (Id == TEXT("initial_read"))
		{
			F.Pin()->DefaultValue = TEXT("9.75");
			F.Blueprint->GetOutermost()->SetDirtyFlag(true);
			bHumanEditApplied = true;
		}
		if (Id == TEXT("stale_edit") || Id == TEXT("still_stale"))
		{
			bHumanEditPreservedAtConflicts &= F.Pin()->DefaultValue == TEXT("9.75");
		}
	});
	Agent.RunTask(TEXT("Exercise read revisions using the scripted provider."), Step);
	TestTrue(TEXT("human change injected after real graph read"), bHumanEditApplied);
	TestEqual(TEXT("one terminal publication"), Terminals, 1);
	for (const TCHAR* Id : { TEXT("stale_edit"), TEXT("still_stale") })
	{
		const auto Result = Results.FindRef(Id);
		const TSharedPtr<FJsonObject>* Error = nullptr;
		FString Code;
		if (Result && Result->TryGetObjectField(TEXT("error"), Error)) (*Error)->TryGetStringField(TEXT("error_code"), Code);
		TestEqual(TEXT("omitted model token remains protected until reread"), Code, FString(TEXT("ASSET_REVISION_CONFLICT")));
	}
	TestTrue(TEXT("human value preserved through both conflicts"), bHumanEditPreservedAtConflicts);
	const auto Fresh = Results.FindRef(TEXT("fresh_edit"));
	bool bSuccess = false;
	if (Fresh) Fresh->TryGetBoolField(TEXT("success"), bSuccess);
	TestTrue(TEXT("fresh exact read enables new plan"), bSuccess);
	const auto Advanced = Results.FindRef(TEXT("advanced_edit"));
	bSuccess = false;
	if (Advanced) Advanced->TryGetBoolField(TEXT("success"), bSuccess);
	TestTrue(TEXT("own successful mutation advances observed revision"), bSuccess);
	TestTrue(TEXT("second planned value applied"), F.Pin()->DefaultValue.StartsWith(TEXT("7.5")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadRevision_CompileCurrent, "KismetAI.ReadRevision.CompileCurrentRevision", KismetAIReadRevisionTest::Flags)
bool FReadRevision_CompileCurrent::RunTest(const FString& Parameters)
{
	using namespace KismetAIReadRevisionTest;
	FFixture F;
	if (!TestNotNull(TEXT("fixture"), F.Blueprint)) return false;
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Panel }, EKismetAIAuthorityOp::Edit, { *F.Mount.Scope() });
	FString Token = Revision(Call(TEXT("read_blueprint"), F.Read()));
	TestFalse(TEXT("current revision exists"), Token.IsEmpty());
	const auto Args = F.Read();
	Args->RemoveField(TEXT("mode"));
	Args->SetStringField(TEXT("expected_revision"), Token);
	TestTrue(TEXT("current revision permits compile and save"), Call(TEXT("compile_blueprint"), Args).Typed.bSuccess);
	Token = Revision(Call(TEXT("read_blueprint"), F.Read()));
	TestTrue(TEXT("fresh post-compile revision permits edit"), Call(TEXT("edit_blueprint"), F.Edit(Token)).Typed.bSuccess);
	F.Pin()->DefaultValue = TEXT("9.75");
	Args->SetStringField(TEXT("expected_revision"), Token);
	const auto Stale = Call(TEXT("compile_blueprint"), Args);
	TestEqual(TEXT("compile also rejects stale assumptions"), Stale.Typed.ErrorCode, FString(TEXT("ASSET_REVISION_CONFLICT")));
	TestEqual(TEXT("stale compile preserves human value"), F.Pin()->DefaultValue, FString(TEXT("9.75")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadRevision_ReentrantBoundary, "KismetAI.ReadRevision.ReentrantAssetBoundary", KismetAIReadRevisionTest::Flags)
bool FReadRevision_ReentrantBoundary::RunTest(const FString& Parameters)
{
	using namespace KismetAIReadRevisionTest;
	FFixture F;
	if (!TestNotNull(TEXT("fixture"), F.Blueprint)) return false;
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Panel }, EKismetAIAuthorityOp::Edit, { *F.Mount.Scope() });
	UK2Node_CallFunction* OtherPrint = nullptr;
	UBlueprint* Other = F.Create(TEXT("BP_Unrelated"), OtherPrint);
	if (!TestNotNull(TEXT("unrelated fixture"), Other)) return false;
	const FString Token = Revision(Call(TEXT("read_blueprint"), F.Read()));
	const FString Before = F.Pin()->DefaultValue;
	FToolCallOutcome Nested;
	FToolCallOutcome NestedSave;
	FToolCallOutcome ReadOther;
	const auto Outer = KismetAIReadRevision::Run(TEXT("edit_blueprint"), F.Edit(Token), {}, false,
		[&](const TSharedPtr<FJsonObject>&)
		{
			Nested = Call(TEXT("edit_blueprint"), F.Edit(Token));
			const auto SaveArgs = MakeShared<FJsonObject>();
			SaveArgs->SetStringField(TEXT("action"), TEXT("save"));
			SaveArgs->SetStringField(TEXT("asset_path"), F.Blueprint->GetPathName().ToLower());
			NestedSave = Call(TEXT("manage_asset"), SaveArgs);
			ReadOther = Call(TEXT("read_blueprint"), F.Read(Other));
			return FToolExecutionResult::Success();
		});
	TestTrue(TEXT("outer current-state admission succeeds"), Outer.bSuccess);
	TestEqual(TEXT("same-asset reentrant mutation refused inside actual boundary"), Nested.Typed.ErrorCode, FString(TEXT("ASSET_BUSY")));
	TestEqual(TEXT("case-varied manage_asset cannot bypass asset boundary"), NestedSave.Typed.ErrorCode, FString(TEXT("ASSET_BUSY")));
	TestTrue(TEXT("unrelated read allowed while mutation boundary held"), ReadOther.Typed.bSuccess);
	TestEqual(TEXT("rejected nested mutation leaves pin unchanged"), F.Pin()->DefaultValue, Before);
	TestTrue(TEXT("asset boundary released on return"), Call(TEXT("edit_blueprint"), F.Edit(Token)).Typed.bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadRevision_UnsupportedWorker, "KismetAI.ReadRevision.UnsupportedWorkerArgument", KismetAIReadRevisionTest::Flags)
bool FReadRevision_UnsupportedWorker::RunTest(const FString& Parameters)
{
	using namespace KismetAIReadRevisionTest;
	FToolCallContext Context;
	Context.ToolName = TEXT("read_source");
	Context.Caller = FKismetAICallerContext::Verified(EKismetAIEntryPoint::Panel, TEXT("read-revision-test"));
	Context.Arguments = MakeShared<FJsonObject>();
	Context.Arguments->SetStringField(TEXT("scope"), TEXT("engine"));
	Context.Arguments->SetStringField(TEXT("mode"), TEXT("outline"));
	Context.Arguments->SetStringField(TEXT("file"), TEXT("Runtime/Engine/Classes/GameFramework/Actor.h"));
	Context.Arguments->SetStringField(TEXT("expected_revision"), TEXT("unsupported-token"));
	Context.RequestBody = Context.Arguments;
	struct FWorkerState
	{
		FToolCallContext Context;
		TFuture<FToolCallOutcome> Worker;
		double Deadline = FPlatformTime::Seconds() + 5.0;
	};
	class FVerifyUnsupported final : public IAutomationLatentCommand
	{
	public:
		FVerifyUnsupported(FAutomationTestBase* InTest, TSharedRef<FWorkerState> InState) : Test(InTest), State(InState) {}
		virtual bool Update() override
		{
			if (!State->Worker.IsReady() && FPlatformTime::Seconds() < State->Deadline) return false;
			if (!Test->TestTrue(TEXT("worker rejects unsupported revision promptly"), State->Worker.IsReady()))
			{
				State->Context.CancelToken.Cancel();
				return true;
			}
			const auto Result = State->Worker.Get();
			Test->TestEqual(TEXT("off-GT read_source rejects unsupported revision before handler"), Result.Typed.ErrorCode, FString(TEXT("VALIDATION_ERROR")));
			return true;
		}
	private:
		FAutomationTestBase* Test;
		TSharedRef<FWorkerState> State;
	};
	auto State = MakeShared<FWorkerState>();
	State->Context = Context;
	const auto Invoker = MakeShared<FKismetAIModuleToolInvoker, ESPMode::ThreadSafe>(FKismetAIModule::Get());
	State->Worker = Async(EAsyncExecution::Thread, [Invoker, Context]()
	{
		FKismetAIToolExecutor Executor(*Invoker);
		return Executor.Execute(Context);
	});
	ADD_LATENT_AUTOMATION_COMMAND(FVerifyUnsupported(this, State));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadRevision_Resume, "KismetAI.ReadRevision.ResumeRequiresFreshRead", KismetAIReadRevisionTest::Flags)
bool FReadRevision_Resume::RunTest(const FString& Parameters)
{
	using namespace KismetAIReadRevisionTest;
	using namespace KismetAIRunEntryHarness;
	FFixture F;
	if (!TestNotNull(TEXT("fixture"), F.Blueprint)) return false;
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Panel }, EKismetAIAuthorityOp::Edit, { *F.Mount.Scope() });
	const auto First = RunScript({ ToolTurn(TEXT("old_read"), TEXT("read_blueprint"), F.Read()), TextTurn(TEXT("done")) });
	TestTrue(TEXT("first run read succeeded"), Succeeded(First.Tools.FindRef(TEXT("old_read"))));
	TestFalse(TEXT("session persisted"), First.SessionId.IsEmpty());
	F.Pin()->DefaultValue = TEXT("9.75");
	F.Blueprint->GetOutermost()->SetDirtyFlag(true);
	bool bPreservedAtRefusal = false;
	const auto Resumed = RunScript({
		ToolTurn(TEXT("old_plan"), TEXT("edit_blueprint"), F.Edit(FString(), TEXT("6.25"))),
		ToolTurn(TEXT("fresh_read"), TEXT("read_blueprint"), F.Read()),
		ToolTurn(TEXT("fresh_plan"), TEXT("edit_blueprint"), F.Edit(FString(), TEXT("7.25"))),
		TextTurn(TEXT("done"))
	}, First.SessionId, [&](const FString& Id)
	{
		if (Id == TEXT("old_plan")) bPreservedAtRefusal = F.Pin()->DefaultValue == TEXT("9.75");
	});
	TestEqual(TEXT("resumed history cannot authorize blind mutation"), ErrorCode(Resumed.Tools.FindRef(TEXT("old_plan"))), FString(TEXT("REVISION_REQUIRED")));
	TestTrue(TEXT("human change preserved on resume refusal"), bPreservedAtRefusal);
	TestTrue(TEXT("fresh resumed read permits mutation"), Succeeded(Resumed.Tools.FindRef(TEXT("fresh_plan"))));
	TestTrue(TEXT("fresh resumed plan applied"), F.Pin()->DefaultValue.StartsWith(TEXT("7.25")));
	TestEqual(TEXT("resumed run publishes one terminal"), Resumed.Terminals, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadRevision_ExplicitPrecedence, "KismetAI.ReadRevision.ExplicitExpectationWins", KismetAIReadRevisionTest::Flags)
bool FReadRevision_ExplicitPrecedence::RunTest(const FString& Parameters)
{
	using namespace KismetAIReadRevisionTest;
	using namespace KismetAIRunEntryHarness;
	FFixture F;
	if (!TestNotNull(TEXT("fixture"), F.Blueprint)) return false;
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Panel }, EKismetAIAuthorityOp::Edit, { *F.Mount.Scope() });
	const FString OldToken = Revision(Call(TEXT("read_blueprint"), F.Read()));
	TestFalse(TEXT("old revision exists"), OldToken.IsEmpty());
	F.Pin()->DefaultValue = TEXT("9.75");
	bool bPreservedAtConflict = false;
	const auto Run = RunScript({
		ToolTurn(TEXT("current_read"), TEXT("read_blueprint"), F.Read()),
		ToolTurn(TEXT("explicit_old_plan"), TEXT("edit_blueprint"), F.Edit(OldToken, TEXT("6.25"))),
		ToolTurn(TEXT("current_plan"), TEXT("edit_blueprint"), F.Edit(FString(), TEXT("7.25"))),
		TextTurn(TEXT("done"))
	}, FString(), [&](const FString& Id)
	{
		if (Id == TEXT("explicit_old_plan")) bPreservedAtConflict = F.Pin()->DefaultValue == TEXT("9.75");
	});
	TestEqual(TEXT("explicit stale expectation is never replaced by newer observation"), ErrorCode(Run.Tools.FindRef(TEXT("explicit_old_plan"))), FString(TEXT("ASSET_REVISION_CONFLICT")));
	TestTrue(TEXT("human value survives explicit stale expectation"), bPreservedAtConflict);
	TestTrue(TEXT("current coordinator observation remains usable"), Succeeded(Run.Tools.FindRef(TEXT("current_plan"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadRevision_LegacyResume, "KismetAI.ReadRevision.LegacyResumeRequiresFreshRead", KismetAIReadRevisionTest::Flags)
bool FReadRevision_LegacyResume::RunTest(const FString& Parameters)
{
	using namespace KismetAIReadRevisionTest;
	using namespace KismetAIRunEntryHarness;
	FFixture F;
	if (!TestNotNull(TEXT("fixture"), F.Blueprint)) return false;
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Panel }, EKismetAIAuthorityOp::Edit, { *F.Mount.Scope() });
	const FString SessionId = FKismetAISessionStorage::CreateSessionId();
	ON_SCOPE_EXIT { FKismetAISessionStorage::DeleteSession(SessionId); };
	const auto User = MakeShared<FJsonObject>();
	User->SetStringField(TEXT("type"), TEXT("user_message"));
	User->SetStringField(TEXT("content"), TEXT("Continue editing the Blueprint from the previous plan."));
	TestTrue(TEXT("legacy unversioned message written"), FKismetAISessionStorage::AppendEntry(SessionId, User));
	const auto Assistant = MakeShared<FJsonObject>();
	Assistant->SetStringField(TEXT("type"), TEXT("assistant_message"));
	const auto Part = MakeShared<FJsonObject>();
	Part->SetStringField(TEXT("type"), TEXT("text"));
	Part->SetStringField(TEXT("text"), TEXT("The previous graph read is summarized here without any revision token."));
	Assistant->SetArrayField(TEXT("content"), { MakeShared<FJsonValueObject>(Part) });
	TestTrue(TEXT("legacy summary written"), FKismetAISessionStorage::AppendEntry(SessionId, Assistant));
	const auto Loaded = FKismetAISessionStorage::LoadSession(SessionId);
	TestEqual(TEXT("fixture really is unversioned legacy history"), Loaded.JournalVersion, 0);
	TestFalse(TEXT("legacy history projects messages"), Loaded.Messages.IsEmpty());
	F.Pin()->DefaultValue = TEXT("9.75");
	bool bPreserved = false;
	const auto Run = RunScript({
		ToolTurn(TEXT("legacy_plan"), TEXT("edit_blueprint"), F.Edit(FString(), TEXT("6.25"))),
		ToolTurn(TEXT("current_read"), TEXT("read_blueprint"), F.Read()),
		ToolTurn(TEXT("current_plan"), TEXT("edit_blueprint"), F.Edit(FString(), TEXT("7.25"))),
		TextTurn(TEXT("done"))
	}, SessionId, [&](const FString& Id)
	{
		if (Id == TEXT("legacy_plan")) bPreserved = F.Pin()->DefaultValue == TEXT("9.75");
	});
	TestEqual(TEXT("legacy history cannot bypass revision admission"), ErrorCode(Run.Tools.FindRef(TEXT("legacy_plan"))), FString(TEXT("REVISION_REQUIRED")));
	TestTrue(TEXT("legacy rejection preserves human edit"), bPreserved);
	TestTrue(TEXT("fresh read recovers legacy session"), Succeeded(Run.Tools.FindRef(TEXT("current_plan"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadRevision_ResumeExplicit, "KismetAI.ReadRevision.ResumeAcceptsCurrentExplicitToken", KismetAIReadRevisionTest::Flags)
bool FReadRevision_ResumeExplicit::RunTest(const FString& Parameters)
{
	using namespace KismetAIReadRevisionTest;
	using namespace KismetAIRunEntryHarness;
	FFixture F;
	if (!TestNotNull(TEXT("fixture"), F.Blueprint)) return false;
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Panel }, EKismetAIAuthorityOp::Edit, { *F.Mount.Scope() });
	const auto First = RunScript({ ToolTurn(TEXT("read"), TEXT("read_blueprint"), F.Read()), TextTurn(TEXT("done")) });
	const auto Result = First.Tools.FindRef(TEXT("read"));
	const TSharedPtr<FJsonObject>* Data = nullptr;
	FString Token;
	if (Result && Result->TryGetObjectField(TEXT("data"), Data)) (*Data)->TryGetStringField(TEXT("asset_revision"), Token);
	TestFalse(TEXT("prior run returned saved expectation"), Token.IsEmpty());
	const auto Resumed = RunScript({
		ToolTurn(TEXT("explicit_current"), TEXT("edit_blueprint"), F.Edit(Token, TEXT("7.25"))),
		TextTurn(TEXT("done"))
	}, First.SessionId);
	TestTrue(TEXT("still-current explicit token permits resumed mutation"), Succeeded(Resumed.Tools.FindRef(TEXT("explicit_current"))));
	TestTrue(TEXT("explicit resumed plan applied"), F.Pin()->DefaultValue.StartsWith(TEXT("7.25")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadRevision_PartialDoesNotAdvance, "KismetAI.ReadRevision.PartialAndFailedDoNotAdvance", KismetAIReadRevisionTest::Flags)
bool FReadRevision_PartialDoesNotAdvance::RunTest(const FString& Parameters)
{
	using namespace KismetAIReadRevisionTest;
	FFixture F;
	if (!TestNotNull(TEXT("fixture"), F.Blueprint)) return false;
	FKismetAIScopedTestGrant Grant({ EKismetAIEntryPoint::Panel }, EKismetAIAuthorityOp::Edit, { *F.Mount.Scope() });
	for (bool bPartial : { true, false })
	{
		const FString Token = Revision(Call(TEXT("read_blueprint"), F.Read()));
		const auto Result = KismetAIReadRevision::Run(TEXT("edit_blueprint"), F.Edit(Token), {}, false,
			[&](const TSharedPtr<FJsonObject>&)
			{
				F.Pin()->DefaultValue = bPartial ? TEXT("5.25") : TEXT("5.5");
				return bPartial ? FToolExecutionResult::Success(MakeShared<FJsonObject>(), false)
					: FToolExecutionResult::Error(500, TEXT("TEST_PARTIAL_FAILURE"), TEXT("Injected failure after a partial effect"));
			});
		TestFalse(TEXT("partial or failed result cannot advance observed revision"), Result.Data && Result.Data->HasField(TEXT("asset_revision")));
		const auto Retry = Call(TEXT("edit_blueprint"), F.Edit(Token));
		TestEqual(TEXT("old plan still conflicts after partial effect"), Retry.Typed.ErrorCode, FString(TEXT("ASSET_REVISION_CONFLICT")));
		TestEqual(TEXT("partial effect remains preserved"), F.Pin()->DefaultValue, FString(bPartial ? TEXT("5.25") : TEXT("5.5")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadRevision_PinTypeCoverage, "KismetAI.ReadRevision.PinTypeCoverage", KismetAIReadRevisionTest::Flags)
bool FReadRevision_PinTypeCoverage::RunTest(const FString& Parameters)
{
	using namespace KismetAIReadRevisionTest;
	FFixture F;
	if (!TestNotNull(TEXT("fixture"), F.Blueprint)) return false;
	UEdGraphPin* TypePin = F.Print->FindPin(TEXT("InString"));
	if (!TestNotNull(TEXT("type fixture pin"), TypePin)) return false;
	const FEdGraphPinType OriginalType = TypePin->PinType;
	ON_SCOPE_EXIT { TypePin->PinType = OriginalType; };
	const TCHAR* Cases[] = { TEXT("map terminal"), TEXT("const flag"), TEXT("member reference"), TEXT("weak pointer flag"), TEXT("object wrapper flag") };
	for (int32 CaseIndex = 0; CaseIndex < UE_ARRAY_COUNT(Cases); ++CaseIndex)
	{
		TypePin->PinType = OriginalType;
		if (CaseIndex == 0)
		{
			TypePin->PinType.ContainerType = EPinContainerType::Map;
			TypePin->PinType.PinCategory = TEXT("name");
			TypePin->PinType.PinValueType.TerminalCategory = TEXT("int");
		}
		const auto Before = Call(TEXT("read_blueprint"), F.Read());
		TestTrue(FString::Printf(TEXT("%s: baseline read succeeds"), Cases[CaseIndex]), Before.Typed.bSuccess);
		const FString Token = Revision(Before);
		TestFalse(FString::Printf(TEXT("%s: baseline token exists"), Cases[CaseIndex]), Token.IsEmpty());
		switch (CaseIndex)
		{
		case 0: TypePin->PinType.PinValueType.TerminalCategory = TEXT("string"); break;
		case 1: TypePin->PinType.bIsConst = !TypePin->PinType.bIsConst; break;
		case 2: TypePin->PinType.PinSubCategoryMemberReference.MemberName = TEXT("ChangedSignature"); break;
		case 3: TypePin->PinType.bIsWeakPointer = !TypePin->PinType.bIsWeakPointer; break;
		case 4: TypePin->PinType.bIsUObjectWrapper = !TypePin->PinType.bIsUObjectWrapper; break;
		}
		const FEdGraphPinType ChangedType = TypePin->PinType;
		const auto After = Call(TEXT("read_blueprint"), F.Read());
		TestTrue(FString::Printf(TEXT("%s: changed read succeeds"), Cases[CaseIndex]), After.Typed.bSuccess);
		TestTrue(FString::Printf(TEXT("%s: type-only edit changes revision"), Cases[CaseIndex]), Revision(After) != Token);
		bool bHandlerExecuted = false;
		const auto Result = KismetAIReadRevision::Run(TEXT("edit_blueprint"), F.Edit(Token), {}, false,
			[&](const TSharedPtr<FJsonObject>&)
			{
				bHandlerExecuted = true;
				return FToolExecutionResult::Success();
			});
		TestEqual(FString::Printf(TEXT("%s: stale type assumption conflicts"), Cases[CaseIndex]), Result.ErrorCode, FString(TEXT("ASSET_REVISION_CONFLICT")));
		TestFalse(FString::Printf(TEXT("%s: stale handler never executes"), Cases[CaseIndex]), bHandlerExecuted);
		TestTrue(FString::Printf(TEXT("%s: human type edit remains"), Cases[CaseIndex]), TypePin->PinType == ChangedType);
	}
	return true;
}
