// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

// Automation tests for insert_node's `node_class` filter.
//
// Menu names are not unique. An input axis exposes BOTH a value getter
// (K2Node_GetInputAxisValue) and an event (K2Node_InputAxisEvent) under the same
// name, and the action-database scan takes the first exact match and stops — so
// whichever it yields first wins and the other node type is unreachable by ANY
// query string. `node_class` narrows the search to the node type actually wanted.
//
// These drive the real handler via ExecuteHandlerDirect against BPs in named
// /Temp packages so FindBlueprint (StaticLoadObject) resolves them.

#include "KismetAIModule.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"

namespace InsertNodeClassTestHelpers
{
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	static TSharedPtr<FJsonObject> RunHandler(
		bool (FKismetAIModule::*Handler)(const FHttpServerRequest&, const FHttpResultCallback&),
		TSharedPtr<FJsonObject> Body)
	{
		FString Resp = FKismetAIModule::Get().ExecuteHandlerDirect(Handler, Body);
		Resp.RemoveFromStart(TEXT("<tool_use_error>"));
		Resp.RemoveFromEnd(TEXT("</tool_use_error>"));
		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp);
		FJsonSerializer::Deserialize(Reader, Parsed);
		return Parsed;
	}

	static UBlueprint* MakeActorBP(const TCHAR* PkgName, FString& OutPath)
	{
		UPackage* Pkg = CreatePackage(PkgName);
		if (!Pkg) return nullptr;
		const FName BPName = MakeUniqueObjectName(Pkg, UBlueprint::StaticClass(), TEXT("BP_InsertNodeClass"));
		UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Pkg, BPName, BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (BP) OutPath = BP->GetPathName();
		return BP;
	}

	// Insert `NodeQuery`, optionally filtered by `NodeClass`. Returns the spawned
	// node's class name, or "" plus OutError when the handler reported a failure.
	static FString InsertAndGetClass(const FString& BPPath, const FString& NodeQuery,
		const FString& NodeClass, FString& OutError)
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("blueprint"), BPPath);
		Body->SetStringField(TEXT("graph"), TEXT("EventGraph"));
		Body->SetStringField(TEXT("node"), NodeQuery);
		if (!NodeClass.IsEmpty())
		{
			Body->SetStringField(TEXT("node_class"), NodeClass);
		}

		TSharedPtr<FJsonObject> Env = RunHandler(&FKismetAIModule::HandleInsertNode, Body);
		if (!Env.IsValid()) { OutError = TEXT("no/invalid response"); return FString(); }

		bool bSuccess = false;
		Env->TryGetBoolField(TEXT("success"), bSuccess);
		if (!bSuccess)
		{
			const TSharedPtr<FJsonObject>* ErrObj = nullptr;
			if (Env->TryGetObjectField(TEXT("error"), ErrObj) && ErrObj)
			{
				(*ErrObj)->TryGetStringField(TEXT("message"), OutError);
			}
			return FString();
		}

		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (!Env->TryGetObjectField(TEXT("data"), Data) || !Data) { OutError = TEXT("no data"); return FString(); }
		FString Cls;
		(*Data)->TryGetStringField(TEXT("node_class"), Cls);
		return Cls;
	}
}

// A filter naming a node class that no spawner produces must fail cleanly rather
// than silently falling back to some other node — and the message has to name the
// filter, or the agent cannot tell why it missed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInsertNodeClass_UnknownClassRejected,
	"KismetAI.InsertNodeClass.UnknownClassRejected", InsertNodeClassTestHelpers::Flags)
bool FInsertNodeClass_UnknownClassRejected::RunTest(const FString& Parameters)
{
	using namespace InsertNodeClassTestHelpers;
	FString BPPath;
	UBlueprint* BP = MakeActorBP(TEXT("/Temp/InsertNodeClassA"), BPPath);
	if (!TestNotNull(TEXT("blueprint created"), BP)) return false;

	FString Err;
	const FString Cls = InsertAndGetClass(BPPath, TEXT("Branch"), TEXT("K2Node_DoesNotExist"), Err);
	TestTrue(TEXT("no node spawned for an unknown node_class"), Cls.IsEmpty());
	TestTrue(TEXT("error names the node_class filter"), Err.Contains(TEXT("node_class")));
	return true;
}

// The filter must not change behaviour when it agrees with what would have been
// chosen anyway — Branch is unambiguous, so filtered and unfiltered must match.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInsertNodeClass_MatchingFilterIsNoOp,
	"KismetAI.InsertNodeClass.MatchingFilterIsNoOp", InsertNodeClassTestHelpers::Flags)
bool FInsertNodeClass_MatchingFilterIsNoOp::RunTest(const FString& Parameters)
{
	using namespace InsertNodeClassTestHelpers;
	FString BPPath;
	UBlueprint* BP = MakeActorBP(TEXT("/Temp/InsertNodeClassB"), BPPath);
	if (!TestNotNull(TEXT("blueprint created"), BP)) return false;

	FString ErrA, ErrB, ErrC;
	const FString Unfiltered = InsertAndGetClass(BPPath, TEXT("Branch"), FString(), ErrA);
	TestEqual(TEXT("Branch spawns K2Node_IfThenElse unfiltered"), Unfiltered, TEXT("K2Node_IfThenElse"));

	const FString Filtered = InsertAndGetClass(BPPath, TEXT("Branch"), TEXT("K2Node_IfThenElse"), ErrB);
	TestEqual(TEXT("same class with an agreeing filter"), Filtered, Unfiltered);

	// The "K2Node_" prefix is optional, so agents can pass either form.
	const FString NoPrefix = InsertAndGetClass(BPPath, TEXT("Branch"), TEXT("IfThenElse"), ErrC);
	TestEqual(TEXT("prefix-less filter resolves identically"), NoPrefix, Unfiltered);
	return true;
}

// Standard macros such as DoOnce are resolved by a fallback path that
// does not consult the action database. That path must respect the filter too, or
// a filtered query could be satisfied by a node type it explicitly excluded.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInsertNodeClass_MacroFallbackRespectsFilter,
	"KismetAI.InsertNodeClass.MacroFallbackRespectsFilter", InsertNodeClassTestHelpers::Flags)
bool FInsertNodeClass_MacroFallbackRespectsFilter::RunTest(const FString& Parameters)
{
	using namespace InsertNodeClassTestHelpers;
	FString BPPath;
	UBlueprint* BP = MakeActorBP(TEXT("/Temp/InsertNodeClassC"), BPPath);
	if (!TestNotNull(TEXT("blueprint created"), BP)) return false;

	FString ErrA, ErrB;
	const FString Allowed = InsertAndGetClass(BPPath, TEXT("Sequence"), TEXT("K2Node_ExecutionSequence"), ErrA);
	TestEqual(TEXT("Sequence still resolves with an agreeing filter"), Allowed, TEXT("K2Node_ExecutionSequence"));

	// Excluding the class it would have produced must yield nothing, not a macro.
	const FString Excluded = InsertAndGetClass(BPPath, TEXT("DoOnce"), TEXT("K2Node_IfThenElse"), ErrB);
	TestTrue(TEXT("macro fallback does not bypass the node_class filter"), Excluded.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInsertNodeClass_OperatorRouteRespectsFilter,
	"KismetAI.InsertNodeClass.OperatorRouteRespectsFilter", InsertNodeClassTestHelpers::Flags)
bool FInsertNodeClass_OperatorRouteRespectsFilter::RunTest(const FString& Parameters)
{
	using namespace InsertNodeClassTestHelpers;
	FString BPPath;
	UBlueprint* BP = MakeActorBP(TEXT("/Temp/InsertNodeClassOperator"), BPPath);
	if (!TestNotNull(TEXT("blueprint created"), BP)) return false;
	FString AllowedError, RejectedError;
	TestEqual(TEXT("matching class keeps canonical operator routing"),
		InsertAndGetClass(BPPath, TEXT("Greater"), TEXT("PromotableOperator"), AllowedError),
		FString(TEXT("K2Node_PromotableOperator")));
	TestTrue(TEXT("operator fast path cannot bypass explicit class filter"),
		InsertAndGetClass(BPPath, TEXT("Greater"), TEXT("K2Node_IfThenElse"), RejectedError).IsEmpty());
	TestTrue(TEXT("rejected operator reports class filter"), RejectedError.Contains(TEXT("node_class")));
	return true;
}
