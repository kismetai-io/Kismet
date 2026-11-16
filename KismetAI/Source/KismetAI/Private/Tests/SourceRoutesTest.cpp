// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
#include "KismetAIModule.h"
#include "KismetAIEngineCompat.h"
#include "KismetAIMcpRoute.h"
#include "KismetAIHttpTransport.h"
#include "KismetAISourceJobs.h"
#include "KismetAISourceConfinement.h"
#include "KismetAISourceSearch.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "KismetAIMcpToken.h"
#include "KismetAIToolPolicy.h"
#include "HttpServerResponse.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace KismetAISourceRouteTests
{
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	TSharedPtr<FHttpServerRequest> Request(const FString& Body)
	{
		auto Out = MakeShared<FHttpServerRequest>();
		Out->Verb = EHttpServerRequestVerbs::VERB_POST;
		KismetAIHttpCompat::SetRequestPath(*Out, TEXT("/mcp"));
		Out->Headers.Add(TEXT("Authorization"), { TEXT("Bearer ") + KismetAIMcpToken::GetOrCreateToken() });
		auto Peer = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
		bool bValid = false;
		Peer->SetIp(TEXT("127.0.0.1"), bValid);
		Out->PeerAddress = Peer;
		FTCHARToUTF8 Bytes(*Body);
		Out->Body.Append(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
		return Out;
	}
	FString BodyOf(const FHttpServerResponse& Response)
	{
		const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Response.Body.GetData()), Response.Body.Num());
		return FString(Text.Length(), Text.Get());
	}
	class FRejectAndCount final : public IKismetAIToolPolicy
	{
	public:
		mutable int32 Calls = 0;
		virtual FPolicyVerdict Evaluate(const FPolicyContext&) const override
		{
			++Calls;
			return FPolicyVerdict::Deny(TEXT("Fixture refuses accidental tool execution"));
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAISourceMixedBatchTest, "KismetAI.SourceRoutes.MixedBatchHasNoSideEffects", KismetAISourceRouteTests::Flags)
bool FKismetAISourceMixedBatchTest::RunTest(const FString& Parameters)
{
	using namespace KismetAISourceRouteTests;
	FKismetAIModule& Module = FKismetAIModule::Get();
	FRejectAndCount Policy;
	Module.SetToolPolicyOverrideForTests(&Policy);
	ON_SCOPE_EXIT { Module.SetToolPolicyOverrideForTests(nullptr); };
	auto Transport = MakeShared<FKismetAIHttpTransport>();
	TArray<FHttpRouteHandle> Handles;
	KismetAIMcpRoute::Bind(Module, Transport, Handles);
	FString Response;
	int32 Callbacks = 0;
	const FString Body = TEXT("[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"read_source\",\"arguments\":{\"mode\":\"find\",\"scope\":\"project\",\"pattern\":\"*.h\"}}},{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"create_asset\",\"arguments\":{\"path\":\"/Game/KismetAITest/P206MixedMustNotExist\",\"class\":\"Material\"}}}]");
	TestTrue(TEXT("production route accepted request"), Transport->Query(Request(Body), [&Response, &Callbacks](TUniquePtr<FHttpServerResponse>&& Result)
	{
		Response = BodyOf(*Result); ++Callbacks;
	}));
	TestEqual(TEXT("one immediate refusal"), Callbacks, 1);
	TestTrue(TEXT("mixed batch explicitly refused"), Response.Contains(TEXT("Send source reads separately")));
	TestEqual(TEXT("neither source nor mutation reached executor policy"), Policy.Calls, 0);
	TestEqual(TEXT("no async job admitted"), Module.GetSourceJobs().NumActive(), 0);
	TArray<TSharedPtr<FJsonValue>> Refusals;
	TestTrue(TEXT("mixed refusal preserves batch shape"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response), Refusals));
	if (TestEqual(TEXT("each request gets its own error"), Refusals.Num(), 2))
	{
		TestEqual(TEXT("first id preserved"), Refusals[0]->AsObject()->GetNumberField(TEXT("id")), 1.0);
		TestEqual(TEXT("second id preserved"), Refusals[1]->AsObject()->GetNumberField(TEXT("id")), 2.0);
	}
	FString NotificationBody = Body.Replace(TEXT("\"id\":1,"), TEXT("")).Replace(TEXT("\"id\":2,"), TEXT(""));
	int32 NotificationStatus = 0;
	FString NotificationResponse;
	Transport->Query(Request(NotificationBody), [&NotificationStatus, &NotificationResponse](TUniquePtr<FHttpServerResponse>&& Result)
	{
		NotificationStatus = static_cast<int32>(Result->Code);
		NotificationResponse = BodyOf(*Result);
	});
	TestEqual(TEXT("notification-only mixed batch receives 202"), NotificationStatus, 202);
	TestTrue(TEXT("notifications have no JSON-RPC reply"), NotificationResponse.IsEmpty());
	TestEqual(TEXT("notifications cause no tool dispatch"), Policy.Calls, 0);
	FString StringIdResponse;
	Transport->Query(Request(Body.Replace(TEXT("\"id\":2"), TEXT("\"id\":\"mutation-id\""))), [&StringIdResponse](TUniquePtr<FHttpServerResponse>&& Result)
	{
		StringIdResponse = BodyOf(*Result);
	});
	Refusals.Reset();
	TestTrue(TEXT("string-id batch response parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(StringIdResponse), Refusals));
	if (TestEqual(TEXT("string-id requests both answered"), Refusals.Num(), 2))
		TestEqual(TEXT("string id retains type and value"), Refusals[1]->AsObject()->GetStringField(TEXT("id")), FString(TEXT("mutation-id")));


	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAISourceBatchCancelTest, "KismetAI.SourceRoutes.SourceBatchCancellation", KismetAISourceRouteTests::Flags)
bool FKismetAISourceBatchCancelTest::RunTest(const FString& Parameters)
{
	using namespace KismetAISourceRouteTests;
	FKismetAIModule& Module = FKismetAIModule::Get();
	FCriticalSection Mutex;
	TArray<TFunction<void()>> Held;
	FEvent* Queued = FPlatformProcess::GetSynchEventFromPool(true);
	Module.SetGameThreadSchedulerForTests([&Mutex, &Held, Queued](TFunction<void()> Work)
	{
		{ FScopeLock Lock(&Mutex); Held.Add(MoveTemp(Work)); }
		Queued->Trigger();
	});
	ON_SCOPE_EXIT
	{
		Module.SetGameThreadSchedulerForTests(nullptr);
		for (TFunction<void()>& Work : Held) Work();
		FPlatformProcess::ReturnSynchEventToPool(Queued);
	};
	auto Transport = MakeShared<FKismetAIHttpTransport>();
	TArray<FHttpRouteHandle> Handles;
	KismetAIMcpRoute::Bind(Module, Transport, Handles);
	FString Response;
	bool bCancelHeader = false;
	int32 Callbacks = 0;
	const FString Body = TEXT("[{\"jsonrpc\":\"2.0\",\"id\":701,\"method\":\"tools/call\",\"params\":{\"name\":\"read_source\",\"arguments\":{\"mode\":\"find\",\"scope\":\"project\",\"pattern\":\"*.h\"}}},{\"jsonrpc\":\"2.0\",\"id\":702,\"method\":\"tools/call\",\"params\":{\"name\":\"read_source\",\"arguments\":{\"mode\":\"find\",\"scope\":\"project\",\"pattern\":\"*.cpp\"}}}]");
	Transport->Query(Request(Body), [&Response, &Callbacks, &bCancelHeader](TUniquePtr<FHttpServerResponse>&& Result)
	{
		Response = BodyOf(*Result);
		bCancelHeader = Result->Headers.Contains(TEXT("X-Kismet-Cancel-Requested"));
		++Callbacks;
	});
	TestEqual(TEXT("source batch is deferred"), Callbacks, 0);
	TestTrue(TEXT("real worker queued GT admission"), Queued->Wait(5000));
	const FString MixedWithCancellation = TEXT("[{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":701}},{\"jsonrpc\":\"2.0\",\"id\":703,\"method\":\"tools/call\",\"params\":{\"name\":\"read_source\",\"arguments\":{\"mode\":\"find\",\"pattern\":\"*.h\"}}},{\"jsonrpc\":\"2.0\",\"id\":704,\"method\":\"tools/call\",\"params\":{\"name\":\"create_asset\",\"arguments\":{}}}]");
	FString MixedResponse;
	Transport->Query(Request(MixedWithCancellation), [&MixedResponse](TUniquePtr<FHttpServerResponse>&& Result) { MixedResponse = BodyOf(*Result); });
	TestTrue(TEXT("mixed batch with cancellation refused"), MixedResponse.Contains(TEXT("Send source reads separately")));
	TestEqual(TEXT("refused mixed batch did not cancel existing source work"), Module.GetSourceJobs().NumCancelledForTests(), 0);
	TArray<TSharedPtr<FJsonValue>> MixedReplies;
	TestTrue(TEXT("mixed refusal JSON parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(MixedResponse), MixedReplies));
	TestEqual(TEXT("cancellation notification omitted from error replies"), MixedReplies.Num(), 2);
	int32 CancelStatus = 0;
	Transport->Query(Request(TEXT("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":701}}")), [&CancelStatus](TUniquePtr<FHttpServerResponse>&& Result)
	{
		CancelStatus = static_cast<int32>(Result->Code);
	});
	TestEqual(TEXT("cancellation acknowledged without another worker"), CancelStatus, 202);
	Module.GetSourceJobs().WaitWorkersForTests();
	Module.GetSourceJobs().PumpCompletions();
	TestEqual(TEXT("one terminal batch delivery"), Callbacks, 1);
	TestTrue(TEXT("cancellation explicitly marked"), bCancelHeader);
	TArray<TSharedPtr<FJsonValue>> Results;
	TestTrue(TEXT("batch is valid JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response), Results));
	TestEqual(TEXT("both request ids answered"), Results.Num(), 2);
	for (const TSharedPtr<FJsonValue>& Result : Results)
	{
		const auto Data = Result->AsObject()->GetObjectField(TEXT("result"));
		TestTrue(TEXT("cancelled source result is an error"), Data->GetBoolField(TEXT("isError")));
		const auto Content = Data->GetArrayField(TEXT("content"));
		TestTrue(TEXT("remaining calls cannot restart batch"), Content.Num() > 0 && Content[0]->AsObject()->GetStringField(TEXT("text")).Contains(TEXT("CANCELLED")));
	}
	TestEqual(TEXT("source workers drained"), Module.GetSourceJobs().NumActive(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAISourceLateReadyCancelTest, "KismetAI.SourceRoutes.LateReadyCancellationRevokesCursor", KismetAISourceRouteTests::Flags)
bool FKismetAISourceLateReadyCancelTest::RunTest(const FString& Parameters)
{
	using namespace KismetAISourceRouteTests;
	FKismetAIModule& Module = FKismetAIModule::Get();
	KismetAISourceSearch::Close();
	ON_SCOPE_EXIT { KismetAISourceSearch::Close(); };
	const FString Base = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("KismetP206Late-"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Base, false, true); };
	FKismetAISourceRoots Roots;
	Roots.ProjectDir = Base / TEXT("Project");
	Roots.EngineDir = Base / TEXT("Engine");
	Roots.bIncludeDiscoveredPlugins = false;
	const FString Source = Roots.ProjectDir / TEXT("Source");
	IFileManager::Get().MakeDirectory(*Source, true);
	for (int32 Index = 0; Index < 53; ++Index)
	{
		if (!FFileHelper::SaveStringToFile(TEXT("// late cancellation fixture\n"), *(Source / FString::Printf(TEXT("Match%03d.h"), Index))))
		{
			AddError(TEXT("Could not create late-cancellation fixture"));
			return false;
		}
	}
	KismetAISourceConfinement::FScopedRootsOverride Override(Roots);
	FCriticalSection Mutex;
	TArray<TFunction<void()>> Held;
	FEvent* Queued = FPlatformProcess::GetSynchEventFromPool(true);
	Module.SetGameThreadSchedulerForTests([&Mutex, &Held, Queued](TFunction<void()> Work)
	{
		{ FScopeLock Lock(&Mutex); Held.Add(MoveTemp(Work)); }
		Queued->Trigger();
	});
	ON_SCOPE_EXIT
	{
		Module.SetGameThreadSchedulerForTests(nullptr);
		for (TFunction<void()>& Work : Held) Work();
		FPlatformProcess::ReturnSynchEventToPool(Queued);
	};
	auto Transport = MakeShared<FKismetAIHttpTransport>();
	TArray<FHttpRouteHandle> Handles;
	KismetAIMcpRoute::Bind(Module, Transport, Handles);
	FString Response;
	bool bCancelHeader = false;
	int32 Callbacks = 0;
	Transport->Query(Request(TEXT("{\"jsonrpc\":\"2.0\",\"id\":801,\"method\":\"tools/call\",\"params\":{\"name\":\"read_source\",\"arguments\":{\"mode\":\"find\",\"scope\":\"project\",\"pattern\":\"*.h\",\"limit\":20}}}")),
		[&Response, &Callbacks, &bCancelHeader](TUniquePtr<FHttpServerResponse>&& Result)
		{
			Response = BodyOf(*Result);
			bCancelHeader = Result->Headers.Contains(TEXT("X-Kismet-Cancel-Requested"));
			++Callbacks;
		});
	if (!TestTrue(TEXT("worker queued real GT admission"), Queued->Wait(5000)))
	{
		Transport->Query(Request(TEXT("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":801}}")), [](TUniquePtr<FHttpServerResponse>&&) {});
		Module.GetSourceJobs().WaitWorkersForTests();
		Module.GetSourceJobs().PumpCompletions();
		return false;
	}
	TArray<TFunction<void()>> ToRun;
	{ FScopeLock Lock(&Mutex); ToRun = MoveTemp(Held); }
	for (TFunction<void()>& Work : ToRun) Work();
	Module.GetSourceJobs().WaitWorkersForTests();
	TestEqual(TEXT("completed worker has not delivered its result"), Callbacks, 0);
	int32 CancelStatus = 0;
	Transport->Query(Request(TEXT("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":801}}")),
		[&CancelStatus](TUniquePtr<FHttpServerResponse>&& Result) { CancelStatus = static_cast<int32>(Result->Code); });
	TestEqual(TEXT("late cancellation acknowledged"), CancelStatus, 202);
	Module.GetSourceJobs().PumpCompletions();
	TestEqual(TEXT("one terminal delivery"), Callbacks, 1);
	TestTrue(TEXT("late cancellation marked on preserved evidence"), bCancelHeader);
	TSharedPtr<FJsonObject> Rpc;
	if (!TestTrue(TEXT("response is JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response), Rpc))) return false;
	const TSharedPtr<FJsonObject>* RpcResult = nullptr;
	if (!TestTrue(TEXT("response retains completed MCP result"), Rpc->TryGetObjectField(TEXT("result"), RpcResult))) return false;
	const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
	if (!TestTrue(TEXT("completed result has content"), (*RpcResult)->TryGetArrayField(TEXT("content"), Content) && Content->Num() == 1)) return false;
	const FString Text = (*Content)[0]->AsObject()->GetStringField(TEXT("text"));
	TSharedPtr<FJsonObject> Envelope;
	if (!TestTrue(TEXT("source evidence is structured JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Envelope))) return false;
	const TSharedPtr<FJsonObject>* Data = nullptr;
	if (!TestTrue(TEXT("completed source evidence retained"), Envelope->TryGetObjectField(TEXT("data"), Data))) return false;
	FString Cursor;
	if (!TestTrue(TEXT("fixture exceeded one page"), (*Data)->TryGetStringField(TEXT("cursor"), Cursor) && !Cursor.IsEmpty())) return false;
	auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("mode"), TEXT("find"));
	Args->SetStringField(TEXT("scope"), TEXT("project"));
	Args->SetStringField(TEXT("pattern"), TEXT("*.h"));
	Args->SetNumberField(TEXT("limit"), 20);
	Args->SetStringField(TEXT("cursor"), Cursor);
	const FToolExecutionResult Continued = Module.DoReadSource(Args);
	TestFalse(TEXT("late cancellation revokes the returned cursor"), Continued.bSuccess);
	TestEqual(TEXT("cancelled cursor cannot resume in a fresh call"), Continued.ErrorCode, FString(TEXT("INVALID_CURSOR")));
	TestEqual(TEXT("no source jobs retained"), Module.GetSourceJobs().NumActive(), 0);
	return true;
}
