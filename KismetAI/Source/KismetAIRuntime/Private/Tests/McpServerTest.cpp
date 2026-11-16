// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// PH5: the MCP protocol layer, tested with no HTTP server and no editor.
//
// FKismetAIMcpServer takes bytes and returns bytes; tools/call is driven
// through a real FKismetAIAgent on the scripted seams, so these tests also pin
// that an external call goes through the agent's marshalling and validation
// rather than straight to the invoker.

#include "Fakes/ScriptedAgentSeams.h"
#include "KismetAIMcpServer.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

using namespace KismetAITestFakes;

constexpr auto McpTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	const TCHAR* const McpAgentLocalTools[] = {
		TEXT("enter_plan_mode"),
		TEXT("exit_plan_mode"),
		TEXT("ask_user"),
		TEXT("update_todos"),
		TEXT("discover_tools"),
	};

	TSharedPtr<FJsonValue> MakeSchema(const FString& Name, bool bWithInputSchema = true)
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("name"), Name);
		Schema->SetStringField(TEXT("description"), FString::Printf(TEXT("%s description"), *Name));
		if (bWithInputSchema)
		{
			TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
			Input->SetStringField(TEXT("type"), TEXT("object"));
			Input->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
			Schema->SetObjectField(TEXT("input_schema"), Input);
		}
		return MakeShared<FJsonValueObject>(Schema);
	}

	TSharedPtr<FJsonObject> ParseMcpResponse(const FString& Json)
	{
		TSharedPtr<FJsonObject> Obj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Obj);
		return Obj;
	}

	int32 McpErrorCodeOf(const TSharedPtr<FJsonObject>& Response)
	{
		const TSharedPtr<FJsonObject>* Error = nullptr;
		if (Response.IsValid() && Response->TryGetObjectField(TEXT("error"), Error) && Error && Error->IsValid())
		{
			return static_cast<int32>((*Error)->GetNumberField(TEXT("code")));
		}
		return 0;
	}

	// A server over a fixed schema set with a recording call function.
	struct FRecordingServer
	{
		TArray<FString> CalledTools;
		TArray<TSharedPtr<FJsonObject>> CalledArgs;
		FString NextResult = TEXT("{\"success\":true}");
		TUniquePtr<FKismetAIMcpServer> Server;

		FRecordingServer()
		{
			TArray<TSharedPtr<FJsonValue>> Schemas = {
				MakeSchema(TEXT("read_blueprint")),
				MakeSchema(TEXT("write_blueprint")),
				MakeSchema(TEXT("compile_blueprint")),
				MakeSchema(TEXT("ask_user")),
			};
			TSet<FString> Dispatchable = { TEXT("read_blueprint"), TEXT("write_blueprint"), TEXT("compile_blueprint"), TEXT("ask_user") };
			Server = MakeUnique<FKismetAIMcpServer>(Schemas, Dispatchable,
				[this](const FString& Name, const TSharedPtr<FJsonObject>& Args)
				{
					CalledTools.Add(Name);
					CalledArgs.Add(Args);
					return NextResult;
				},
				TEXT("1.2.3"));
		}

		TSharedPtr<FJsonObject> Send(const FString& Body, bool& bNoContent)
		{
			return ParseMcpResponse(Server->HandleJsonRpc(Body, bNoContent));
		}

		TSharedPtr<FJsonObject> Send(const FString& Body)
		{
			bool bNoContent = false;
			return Send(Body, bNoContent);
		}
	};
}

// ─────────────────────────────────────────────────────────────────────────────
// initialize / notifications/initialized / ping
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIMcpInitializeTest, "KismetAI.Mcp.Initialize", McpTestFlags)
bool FKismetAIMcpInitializeTest::RunTest(const FString& Parameters)
{
	FRecordingServer S;

	// A supported revision is echoed.
	bool bNoContent = true;
	TSharedPtr<FJsonObject> Response = S.Send(TEXT(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"test","version":"0"}}})"), bNoContent);
	TestFalse(TEXT("initialize gets a body"), bNoContent);
	TestTrue(TEXT("response parses"), Response.IsValid());
	if (!Response.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("jsonrpc"), Response->GetStringField(TEXT("jsonrpc")), FString(TEXT("2.0")));
	TestEqual(TEXT("id echoed"), static_cast<int32>(Response->GetNumberField(TEXT("id"))), 1);

	const TSharedPtr<FJsonObject> Result = Response->GetObjectField(TEXT("result"));
	TestEqual(TEXT("echoes supported revision"), Result->GetStringField(TEXT("protocolVersion")), FString(TEXT("2025-06-18")));
	TestFalse(TEXT("tools.listChanged is false"),
		Result->GetObjectField(TEXT("capabilities"))->GetObjectField(TEXT("tools"))->GetBoolField(TEXT("listChanged")));
	const TSharedPtr<FJsonObject> ServerInfo = Result->GetObjectField(TEXT("serverInfo"));
	TestEqual(TEXT("serverInfo.name"), ServerInfo->GetStringField(TEXT("name")), FString(TEXT("kismetai")));
	TestEqual(TEXT("serverInfo.version"), ServerInfo->GetStringField(TEXT("version")), FString(TEXT("1.2.3")));

	// An unknown revision gets our newest.
	Response = S.Send(TEXT(R"({"jsonrpc":"2.0","id":"a","method":"initialize","params":{"protocolVersion":"1999-01-01"}})"));
	TestEqual(TEXT("falls back to latest"),
		Response->GetObjectField(TEXT("result"))->GetStringField(TEXT("protocolVersion")),
		FKismetAIMcpServer::GetSupportedProtocolVersions()[0]);
	TestEqual(TEXT("string id echoed"), Response->GetStringField(TEXT("id")), FString(TEXT("a")));

	// notifications/initialized: no body at all (the route turns this into 202).
	bNoContent = false;
	const FString NotificationBody = S.Server->HandleJsonRpc(TEXT(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"), bNoContent);
	TestTrue(TEXT("notification -> no content"), bNoContent);
	TestTrue(TEXT("notification -> empty body"), NotificationBody.IsEmpty());

	// Even an unknown notification is never answered.
	bNoContent = false;
	S.Server->HandleJsonRpc(TEXT(R"({"jsonrpc":"2.0","method":"notifications/whatever"})"), bNoContent);
	TestTrue(TEXT("unknown notification -> no content"), bNoContent);

	// A client-sent response has nothing to correlate with: also no content.
	bNoContent = false;
	S.Server->HandleJsonRpc(TEXT(R"({"jsonrpc":"2.0","id":9,"result":{}})"), bNoContent);
	TestTrue(TEXT("client response -> no content"), bNoContent);

	// ping
	Response = S.Send(TEXT(R"({"jsonrpc":"2.0","id":2,"method":"ping"})"));
	TestTrue(TEXT("ping returns an empty result"), Response->HasField(TEXT("result")));

	TestEqual(TEXT("no tool was called"), S.CalledTools.Num(), 0);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// tools/list
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIMcpToolsListTest, "KismetAI.Mcp.ToolsList", McpTestFlags)
bool FKismetAIMcpToolsListTest::RunTest(const FString& Parameters)
{
	// Every agent-local tool is offered as schema AND claimed dispatchable, so
	// the only thing keeping them out is the registry's bIsInProcess.
	TArray<TSharedPtr<FJsonValue>> Schemas = {
		MakeSchema(TEXT("read_blueprint")),
		MakeSchema(TEXT("write_blueprint")),
		MakeSchema(TEXT("layout_graph"), /*bWithInputSchema=*/false),
		MakeSchema(TEXT("search_assets")),       // schema, but not dispatchable below
		MakeSchema(TEXT("not_a_registry_tool")), // dispatchable, but unknown to the registry
	};
	TSet<FString> Dispatchable = { TEXT("read_blueprint"), TEXT("write_blueprint"), TEXT("layout_graph"),
		TEXT("not_a_registry_tool"), TEXT("compile_blueprint") /* dispatchable, no schema */ };
	for (const TCHAR* Local : McpAgentLocalTools)
	{
		Schemas.Add(MakeSchema(Local));
		Dispatchable.Add(Local);
	}

	FKismetAIMcpServer Server(Schemas, Dispatchable, nullptr, TEXT("0.1.0"));

	const TArray<FString> Expected = { TEXT("layout_graph"), TEXT("read_blueprint"), TEXT("write_blueprint") };
	TestTrue(TEXT("exposed = schemas AND dispatchable AND registry editor tools"), Server.GetExposedToolNames() == Expected);

	bool bNoContent = false;
	const TSharedPtr<FJsonObject> Response = ParseMcpResponse(Server.HandleJsonRpc(TEXT(R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})"), bNoContent));
	const TArray<TSharedPtr<FJsonValue>>& Tools = Response->GetObjectField(TEXT("result"))->GetArrayField(TEXT("tools"));
	TestEqual(TEXT("tools/list size"), Tools.Num(), Expected.Num());

	for (const TSharedPtr<FJsonValue>& ToolValue : Tools)
	{
		const TSharedPtr<FJsonObject> Tool = ToolValue->AsObject();
		const FString Name = Tool->GetStringField(TEXT("name"));
		for (const TCHAR* Local : McpAgentLocalTools)
		{
			TestNotEqual(TEXT("agent-local tool not exposed"), Name, FString(Local));
		}

		const TSharedPtr<FJsonObject>* InputSchema = nullptr;
		TestTrue(FString::Printf(TEXT("%s has inputSchema"), *Name), Tool->TryGetObjectField(TEXT("inputSchema"), InputSchema));
		if (InputSchema)
		{
			TestEqual(FString::Printf(TEXT("%s inputSchema.type"), *Name), (*InputSchema)->GetStringField(TEXT("type")), FString(TEXT("object")));
		}
		TestFalse(FString::Printf(TEXT("%s description"), *Name), Tool->GetStringField(TEXT("description")).IsEmpty());

		const TSharedPtr<FJsonObject> Annotations = Tool->GetObjectField(TEXT("annotations"));
		if (Name == TEXT("read_blueprint"))
		{
			TestEqual(TEXT("title"), Tool->GetStringField(TEXT("title")), FString(TEXT("Read Blueprint")));
			TestTrue(TEXT("read_blueprint readOnlyHint"), Annotations->GetBoolField(TEXT("readOnlyHint")));
			TestFalse(TEXT("read-only tools carry no destructiveHint"), Annotations->HasField(TEXT("destructiveHint")));
		}
		else if (Name == TEXT("write_blueprint"))
		{
			TestFalse(TEXT("write_blueprint not read-only"), Annotations->GetBoolField(TEXT("readOnlyHint")));
			TestTrue(TEXT("write_blueprint destructiveHint"), Annotations->GetBoolField(TEXT("destructiveHint")));
		}
		else if (Name == TEXT("layout_graph"))
		{
			TestFalse(TEXT("layout_graph not read-only"), Annotations->GetBoolField(TEXT("readOnlyHint")));
			TestFalse(TEXT("layout_graph is not a mutation in the registry"), Annotations->GetBoolField(TEXT("destructiveHint")));
		}
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// tools/call through a real agent on scripted seams
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIMcpToolsCallTest, "KismetAI.Mcp.ToolsCall", McpTestFlags)
bool FKismetAIMcpToolsCallTest::RunTest(const FString& Parameters)
{
	FScriptedToolInvoker Invoker;
	Invoker.SetResult(TEXT("read_blueprint"), TEXT("{\"success\":true,\"t3d\":\"Begin Object\"}"));
	Invoker.SetResult(TEXT("compile_blueprint"), ToolError(TEXT("COMPILE_FAILED"), TEXT("bad pin")));
	FScriptedModelClient Model(TArray<FModelCallOutcome>{});
	FKismetAIAgentConfig Config;
	FKismetAIAgent Agent(Invoker, Model, Config);

	TArray<TSharedPtr<FJsonValue>> Schemas = {
		MakeSchema(TEXT("read_blueprint")),
		MakeSchema(TEXT("compile_blueprint")),
		MakeSchema(TEXT("ask_user")),
	};
	TSet<FString> Dispatchable = Invoker.GetDispatchableToolNames();
	Dispatchable.Add(TEXT("ask_user"));

	FKismetAIMcpServer Server(Schemas, Dispatchable,
		[&Agent](const FString& Name, const TSharedPtr<FJsonObject>& Args)
		{
			return Agent.ExecuteToolForExternalClient(Name, Args);
		},
		TEXT("0.1.0"));

	// 1. Success: routed to the invoker with the agent-marshalled body. The
	//    t3d branch adds strip=true and graph="" — neither came from the caller,
	//    so seeing them proves the call went through ExecuteTool's marshalling.
	bool bNoContent = false;
	TSharedPtr<FJsonObject> Response = ParseMcpResponse(Server.HandleJsonRpc(TEXT(
		R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"read_blueprint","arguments":{"blueprint":"/Game/BP_Test","mode":"t3d"}}})"), bNoContent));
	TestEqual(TEXT("read_blueprint dispatched once"), Invoker.CountCalls(TEXT("read_blueprint")), 1);
	if (Invoker.ArgsLog.Num() >= 1 && Invoker.ArgsLog[0].IsValid())
	{
		const TSharedPtr<FJsonObject>& Body = Invoker.ArgsLog[0];
		TestEqual(TEXT("blueprint forwarded"), Body->GetStringField(TEXT("blueprint")), FString(TEXT("/Game/BP_Test")));
		bool bStrip = false;
		TestTrue(TEXT("marshalled: strip set"), Body->TryGetBoolField(TEXT("strip"), bStrip) && bStrip);
		TestTrue(TEXT("marshalled: graph defaulted"), Body->HasField(TEXT("graph")));
		TestFalse(TEXT("marshalled: mode is not forwarded in the body"), Body->HasField(TEXT("mode")));
	}
	TSharedPtr<FJsonObject> Result = Response->GetObjectField(TEXT("result"));
	TestFalse(TEXT("success -> isError false"), Result->GetBoolField(TEXT("isError")));
	const TArray<TSharedPtr<FJsonValue>>& Content = Result->GetArrayField(TEXT("content"));
	TestEqual(TEXT("one content block"), Content.Num(), 1);
	if (Content.Num() == 1)
	{
		TestEqual(TEXT("text block"), Content[0]->AsObject()->GetStringField(TEXT("type")), FString(TEXT("text")));
		TestTrue(TEXT("result text forwarded"), Content[0]->AsObject()->GetStringField(TEXT("text")).Contains(TEXT("Begin Object")));
	}

	// 2. Tool failure: <tool_use_error> is stripped into isError=true.
	Response = ParseMcpResponse(Server.HandleJsonRpc(TEXT(
		R"({"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"compile_blueprint","arguments":{"blueprint":"/Game/BP_Test"}}})"), bNoContent));
	Result = Response->GetObjectField(TEXT("result"));
	TestTrue(TEXT("tool error -> isError true"), Result->GetBoolField(TEXT("isError")));
	const FString ErrorText = Result->GetArrayField(TEXT("content"))[0]->AsObject()->GetStringField(TEXT("text"));
	TestFalse(TEXT("wrapper stripped"), ErrorText.Contains(TEXT("tool_use_error")));
	TestTrue(TEXT("error payload kept"), ErrorText.Contains(TEXT("COMPILE_FAILED")));

	// 3. Validation failure never reaches the invoker, and reports isError. An
	//    empty path rather than a missing one: marshalling reads the field before
	//    validation runs, and a missing field logs a LogJson error that would
	//    fail this test for the wrong reason.
	const int32 CallsBefore = Invoker.CallLog.Num();
	Response = ParseMcpResponse(Server.HandleJsonRpc(TEXT(
		R"({"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"read_blueprint","arguments":{"blueprint":""}}})"), bNoContent));
	TestTrue(TEXT("invalid args -> isError"), Response->GetObjectField(TEXT("result"))->GetBoolField(TEXT("isError")));
	TestEqual(TEXT("invalid args not dispatched"), Invoker.CallLog.Num(), CallsBefore);

	// 4. Agent-local tool: not exposed, so a protocol error, and the agent refuses
	//    it too if reached directly.
	Response = ParseMcpResponse(Server.HandleJsonRpc(TEXT(
		R"({"jsonrpc":"2.0","id":13,"method":"tools/call","params":{"name":"ask_user","arguments":{"question":"?"}}})"), bNoContent));
	TestEqual(TEXT("agent-local tool -> -32602"), McpErrorCodeOf(Response), FKismetAIMcpServer::InvalidParams);
	const FString Direct = Agent.ExecuteToolForExternalClient(TEXT("enter_plan_mode"), nullptr);
	TestTrue(TEXT("agent refuses in-process tool"), Direct.Contains(TEXT("UNKNOWN_TOOL")));

	// 5. Unknown tool -> -32602; missing name -> -32602; non-object arguments -> -32602.
	Response = ParseMcpResponse(Server.HandleJsonRpc(TEXT(
		R"({"jsonrpc":"2.0","id":14,"method":"tools/call","params":{"name":"no_such_tool"}})"), bNoContent));
	TestEqual(TEXT("unknown tool -> -32602"), McpErrorCodeOf(Response), FKismetAIMcpServer::InvalidParams);
	Response = ParseMcpResponse(Server.HandleJsonRpc(TEXT(
		R"({"jsonrpc":"2.0","id":15,"method":"tools/call","params":{}})"), bNoContent));
	TestEqual(TEXT("missing name -> -32602"), McpErrorCodeOf(Response), FKismetAIMcpServer::InvalidParams);
	Response = ParseMcpResponse(Server.HandleJsonRpc(TEXT(
		R"({"jsonrpc":"2.0","id":16,"method":"tools/call","params":{"name":"read_blueprint","arguments":[1]}})"), bNoContent));
	TestEqual(TEXT("array arguments -> -32602"), McpErrorCodeOf(Response), FKismetAIMcpServer::InvalidParams);

	TestEqual(TEXT("ask_user never dispatched"), Invoker.CountCalls(TEXT("ask_user")), 0);
	TestEqual(TEXT("no model calls"), Model.NumCalls(), 0);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// JSON-RPC error paths
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIMcpJsonRpcErrorsTest, "KismetAI.Mcp.JsonRpcErrors", McpTestFlags)
bool FKismetAIMcpJsonRpcErrorsTest::RunTest(const FString& Parameters)
{
	FRecordingServer S;

	TSharedPtr<FJsonObject> Response = S.Send(TEXT(R"({"jsonrpc":"2.0","id":5,"method":"resources/list"})"));
	TestEqual(TEXT("unknown method -> -32601"), McpErrorCodeOf(Response), FKismetAIMcpServer::MethodNotFound);
	TestEqual(TEXT("id echoed on error"), static_cast<int32>(Response->GetNumberField(TEXT("id"))), 5);

	Response = S.Send(TEXT("{not json"));
	TestEqual(TEXT("malformed -> -32700"), McpErrorCodeOf(Response), FKismetAIMcpServer::ParseError);
	const TSharedPtr<FJsonValue> ParseErrorId = Response->TryGetField(TEXT("id"));
	TestTrue(TEXT("parse error id is null"), ParseErrorId.IsValid() && ParseErrorId->IsNull());

	Response = S.Send(TEXT(""));
	TestEqual(TEXT("empty body -> -32700"), McpErrorCodeOf(Response), FKismetAIMcpServer::ParseError);

	Response = S.Send(TEXT(R"({"id":6,"method":"ping"})"));
	TestEqual(TEXT("missing jsonrpc -> -32600"), McpErrorCodeOf(Response), FKismetAIMcpServer::InvalidRequest);

	Response = S.Send(TEXT(R"({"jsonrpc":"2.0","id":7})"));
	TestEqual(TEXT("missing method -> -32600"), McpErrorCodeOf(Response), FKismetAIMcpServer::InvalidRequest);

	Response = S.Send(TEXT(R"({"jsonrpc":"2.0","id":8,"method":"tools/list","params":[1,2]})"));
	TestEqual(TEXT("positional params -> -32602"), McpErrorCodeOf(Response), FKismetAIMcpServer::InvalidParams);

	// A non-object message inside a batch. (A bare top-level scalar is rejected
	// by UE's JSON reader before it is a message at all, so it is a -32700.)
	{
		bool bBatchNoContent = false;
		const FString ScalarBatch = S.Server->HandleJsonRpc(TEXT("[42]"), bBatchNoContent);
		TArray<TSharedPtr<FJsonValue>> ScalarResponses;
		TSharedRef<TJsonReader<>> ScalarReader = TJsonReaderFactory<>::Create(ScalarBatch);
		FJsonSerializer::Deserialize(ScalarReader, ScalarResponses);
		TestEqual(TEXT("one answer for [42]"), ScalarResponses.Num(), 1);
		if (ScalarResponses.Num() == 1)
		{
			TestEqual(TEXT("non-object message -> -32600"), McpErrorCodeOf(ScalarResponses[0]->AsObject()), FKismetAIMcpServer::InvalidRequest);
		}
	}

	// Batch: requests answered in order, notifications skipped.
	bool bNoContent = false;
	const FString BatchBody = S.Server->HandleJsonRpc(TEXT(
		R"([{"jsonrpc":"2.0","id":1,"method":"ping"},{"jsonrpc":"2.0","method":"notifications/initialized"},{"jsonrpc":"2.0","id":2,"method":"nope"}])"), bNoContent);
	TestFalse(TEXT("batch with requests has a body"), bNoContent);
	TArray<TSharedPtr<FJsonValue>> BatchResponses;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BatchBody);
	TestTrue(TEXT("batch response is an array"), FJsonSerializer::Deserialize(Reader, BatchResponses));
	TestEqual(TEXT("two answers"), BatchResponses.Num(), 2);
	if (BatchResponses.Num() == 2)
	{
		TestTrue(TEXT("first is a result"), BatchResponses[0]->AsObject()->HasField(TEXT("result")));
		TestEqual(TEXT("second is -32601"), McpErrorCodeOf(BatchResponses[1]->AsObject()), FKismetAIMcpServer::MethodNotFound);
	}

	bNoContent = false;
	S.Server->HandleJsonRpc(TEXT(R"([{"jsonrpc":"2.0","method":"notifications/initialized"}])"), bNoContent);
	TestTrue(TEXT("all-notification batch -> no content"), bNoContent);

	Response = S.Send(TEXT("[]"));
	TestEqual(TEXT("empty batch -> -32600"), McpErrorCodeOf(Response), FKismetAIMcpServer::InvalidRequest);

	TestEqual(TEXT("no tool was called"), S.CalledTools.Num(), 0);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Access: loopback peer, Origin, bearer token
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIMcpAccessTest, "KismetAI.Mcp.Access", McpTestFlags)
bool FKismetAIMcpAccessTest::RunTest(const FString& Parameters)
{
	const FString Token = TEXT("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");

	auto Meta = [&Token](const FString& Peer, TArray<FString> Origins, TArray<FString> Auth)
	{
		FKismetAIMcpRequestMeta M;
		M.PeerAddress = Peer;
		M.Origins = MoveTemp(Origins);
		M.Authorization = MoveTemp(Auth);
		return M;
	};
	const FString Good = FString::Printf(TEXT("Bearer %s"), *Token);

	// Allowed
	TestTrue(TEXT("127.0.0.1 + token"), FKismetAIMcpServer::CheckAccess(Meta(TEXT("127.0.0.1"), {}, { Good }), Token) == EKismetAIMcpAccess::Allowed);
	TestTrue(TEXT("::1"), FKismetAIMcpServer::CheckAccess(Meta(TEXT("::1"), {}, { Good }), Token) == EKismetAIMcpAccess::Allowed);
	TestTrue(TEXT("::ffff:127.0.0.1"), FKismetAIMcpServer::CheckAccess(Meta(TEXT("::ffff:127.0.0.1"), {}, { Good }), Token) == EKismetAIMcpAccess::Allowed);
	TestTrue(TEXT("lowercase bearer scheme"), FKismetAIMcpServer::CheckAccess(Meta(TEXT("127.0.0.1"), {}, { TEXT("bearer ") + Token }), Token) == EKismetAIMcpAccess::Allowed);
	TestTrue(TEXT("Origin null"), FKismetAIMcpServer::CheckAccess(Meta(TEXT("127.0.0.1"), { TEXT("null") }, { Good }), Token) == EKismetAIMcpAccess::Allowed);
	TestTrue(TEXT("Origin localhost:port"), FKismetAIMcpServer::CheckAccess(Meta(TEXT("127.0.0.1"), { TEXT("http://localhost:6274") }, { Good }), Token) == EKismetAIMcpAccess::Allowed);
	TestTrue(TEXT("Origin [::1]"), FKismetAIMcpServer::IsOriginAllowed({ TEXT("http://[::1]:8080") }));

	// Peer
	TestTrue(TEXT("LAN peer refused"), FKismetAIMcpServer::CheckAccess(Meta(TEXT("192.168.1.20"), {}, { Good }), Token) == EKismetAIMcpAccess::ForbiddenPeer);
	TestTrue(TEXT("unknown peer refused"), FKismetAIMcpServer::CheckAccess(Meta(TEXT(""), {}, { Good }), Token) == EKismetAIMcpAccess::ForbiddenPeer);
	TestFalse(TEXT("mapped non-loopback"), FKismetAIMcpServer::IsLoopbackAddress(TEXT("::ffff:10.0.0.1")));
	TestFalse(TEXT("127 lookalike"), FKismetAIMcpServer::IsLoopbackAddress(TEXT("127.0.0.1.evil")));
	TestFalse(TEXT("signed octet"), FKismetAIMcpServer::IsLoopbackAddress(TEXT("127.-1.0.1")));
	TestTrue(TEXT("127/8"), FKismetAIMcpServer::IsLoopbackAddress(TEXT("127.1.2.3")));

	// Origin (DNS rebinding guard)
	TestTrue(TEXT("foreign Origin refused"), FKismetAIMcpServer::CheckAccess(Meta(TEXT("127.0.0.1"), { TEXT("https://evil.example") }, { Good }), Token) == EKismetAIMcpAccess::ForbiddenOrigin);
	TestFalse(TEXT("localhost.evil"), FKismetAIMcpServer::IsOriginAllowed({ TEXT("http://localhost.evil.example") }));
	TestFalse(TEXT("userinfo trick"), FKismetAIMcpServer::IsOriginAllowed({ TEXT("http://localhost@evil.example") }));
	TestFalse(TEXT("non-http scheme"), FKismetAIMcpServer::IsOriginAllowed({ TEXT("file://localhost") }));
	TestFalse(TEXT("bad port"), FKismetAIMcpServer::IsOriginAllowed({ TEXT("http://localhost:80x") }));

	// Token
	TestTrue(TEXT("missing token"), FKismetAIMcpServer::CheckAccess(Meta(TEXT("127.0.0.1"), {}, {}), Token) == EKismetAIMcpAccess::Unauthorized);
	TestTrue(TEXT("wrong token"), FKismetAIMcpServer::CheckAccess(Meta(TEXT("127.0.0.1"), {}, { TEXT("Bearer nope") }), Token) == EKismetAIMcpAccess::Unauthorized);
	TestTrue(TEXT("token prefix only"), FKismetAIMcpServer::CheckAccess(Meta(TEXT("127.0.0.1"), {}, { TEXT("Bearer ") + Token.Left(63) }), Token) == EKismetAIMcpAccess::Unauthorized);
	TestTrue(TEXT("no scheme"), FKismetAIMcpServer::CheckAccess(Meta(TEXT("127.0.0.1"), {}, { Token }), Token) == EKismetAIMcpAccess::Unauthorized);
	TestTrue(TEXT("two Authorization headers"), FKismetAIMcpServer::CheckAccess(Meta(TEXT("127.0.0.1"), {}, { Good, Good }), Token) == EKismetAIMcpAccess::Unauthorized);
	TestTrue(TEXT("no configured token fails closed"), FKismetAIMcpServer::CheckAccess(Meta(TEXT("127.0.0.1"), {}, { TEXT("Bearer ") }), FString()) == EKismetAIMcpAccess::NotConfigured);

	// Constant-time compare still compares.
	TestTrue(TEXT("equal"), FKismetAIMcpServer::ConstantTimeEquals(Token, Token));
	TestFalse(TEXT("different length"), FKismetAIMcpServer::ConstantTimeEquals(Token, Token + TEXT("0")));
	TestFalse(TEXT("same length, different"), FKismetAIMcpServer::ConstantTimeEquals(TEXT("abcd"), TEXT("abce")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Result shaping
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIMcpResultShapingTest, "KismetAI.Mcp.ResultShaping", McpTestFlags)
bool FKismetAIMcpResultShapingTest::RunTest(const FString& Parameters)
{
	FString Text;
	bool bIsError = false;

	FKismetAIMcpServer::ShapeToolResult(TEXT("<tool_use_error>{\"success\":false}</tool_use_error>"), Text, bIsError);
	TestTrue(TEXT("wrapped -> error"), bIsError);
	TestEqual(TEXT("wrapper stripped"), Text, FString(TEXT("{\"success\":false}")));

	FKismetAIMcpServer::ShapeToolResult(TEXT("<tool_use_error>{\"a\":1}</tool_use_error>\n[truncated]"), Text, bIsError);
	TestTrue(TEXT("wrapped + trailing note -> error"), bIsError);
	TestFalse(TEXT("closing tag removed"), Text.Contains(TEXT("tool_use_error")));
	TestTrue(TEXT("trailing note kept"), Text.Contains(TEXT("[truncated]")));

	FKismetAIMcpServer::ShapeToolResult(TEXT("{\"success\":false,\"error\":{}}"), Text, bIsError);
	TestTrue(TEXT("unwrapped success:false -> error"), bIsError);

	FKismetAIMcpServer::ShapeToolResult(TEXT("{\"success\":true,\"note\":\"\\\"success\\\":false\"}"), Text, bIsError);
	TestFalse(TEXT("success:true with the substring inside a string is not an error"), bIsError);

	FKismetAIMcpServer::ShapeToolResult(TEXT("## BP_Test Overview"), Text, bIsError);
	TestFalse(TEXT("plain text -> not error"), bIsError);
	TestEqual(TEXT("plain text unchanged"), Text, FString(TEXT("## BP_Test Overview")));

	TestEqual(TEXT("title"), FKismetAIMcpServer::MakeToolTitle(TEXT("set_material_instance_parameter")), FString(TEXT("Set Material Instance Parameter")));
	return true;
}
