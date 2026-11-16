// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Model Context Protocol server logic (PH5).
//
// Lets an external agent (Claude Code, Codex CLI, Cursor) drive KismetAI's
// editor tools over the MCP Streamable HTTP transport. This class is the whole
// protocol half: JSON-RPC parsing, method dispatch, the tools/list catalogue,
// result shaping, and the access checks (loopback peer, Origin, bearer token).
// It is Core/Json only so it can be tested with no HTTP server and no editor;
// the route that feeds it bytes lives in the KismetAI module
// (KismetAIMcpRoute.cpp).
//
// What is exposed is derived, never listed by hand: a tool appears in
// tools/list only when it has a JSON schema, the editor invoker can dispatch
// it, and the registry does not mark it in-process. The five in-process tools
// (enter_plan_mode, exit_plan_mode, ask_user, update_todos, discover_tools)
// touch KismetAI's own loop state and mean nothing to an outside agent.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Runs one tool and returns the result exactly as KismetAI's own model would see
// it, including any <tool_use_error> wrapper. Production wires this to
// FKismetAIAgent::ExecuteToolForExternalClient so argument marshalling,
// validation and truncation are the agent's, not a second copy.
using FKismetAIMcpToolCallFn = TFunction<FString(const FString& /*ToolName*/, const TSharedPtr<FJsonObject>& /*Arguments*/)>;

// The parts of an HTTP request the access check needs, transport-neutral so the
// check is testable without FHttpServerRequest.
struct KISMETAIRUNTIME_API FKismetAIMcpRequestMeta
{
	// Numeric peer address as the socket layer reports it ("127.0.0.1", "::1",
	// "::ffff:127.0.0.1"). Empty when the transport could not report one.
	FString PeerAddress;

	// Every value of the Origin header (usually zero or one).
	TArray<FString> Origins;

	// Every value of the Authorization header.
	TArray<FString> Authorization;
};

enum class EKismetAIMcpAccess : uint8
{
	Allowed,
	// Peer is not a loopback address, or the transport did not report one. 403.
	ForbiddenPeer,
	// Origin header present and not "null" or a localhost origin. 403. This is
	// the DNS-rebinding guard the MCP transport spec asks for.
	ForbiddenOrigin,
	// No server token configured. 500 — fail closed rather than open.
	NotConfigured,
	// Missing, malformed or wrong bearer token. 401.
	Unauthorized,
};

class KISMETAIRUNTIME_API FKismetAIMcpServer
{
public:
	// JSON-RPC 2.0 error codes used here.
	static constexpr int32 ParseError = -32700;
	static constexpr int32 InvalidRequest = -32600;
	static constexpr int32 MethodNotFound = -32601;
	static constexpr int32 InvalidParams = -32602;
	static constexpr int32 InternalError = -32603;

	// ToolSchemas: every tool schema KismetAI ships, in the Anthropic shape
	// ({name, description, input_schema}). DispatchableToolNames: what the editor
	// invoker can actually run. ServerVersion: the plugin's VersionName.
	FKismetAIMcpServer(
		const TArray<TSharedPtr<FJsonValue>>& ToolSchemas,
		const TSet<FString>& DispatchableToolNames,
		FKismetAIMcpToolCallFn InCallTool,
		const FString& InServerVersion);

	// Handle one HTTP POST body: a JSON-RPC request, notification, response, or
	// a batch of them. Returns the JSON response body. bOutNoContent is set when
	// nothing needs answering (only notifications/responses were sent) — the
	// HTTP layer then replies 202 Accepted with an empty body.
	FString HandleJsonRpc(const FString& Body, bool& bOutNoContent);

	// Tool names in tools/list order (sorted).
	const TArray<FString>& GetExposedToolNames() const { return ExposedToolNames; }

	// Protocol revisions this server speaks, newest first. initialize echoes the
	// client's revision when it is one of these, otherwise answers with the first.
	static const TArray<FString>& GetSupportedProtocolVersions();

	// --- access checks (pure, exposed for tests) ---------------------------

	// Order: peer, origin, token. Peer first so a remote caller learns nothing
	// about whether its token was right.
	static EKismetAIMcpAccess CheckAccess(const FKismetAIMcpRequestMeta& Meta, const FString& ExpectedToken);

	// 127.0.0.0/8, ::1, and their IPv4-mapped IPv6 forms. Empty is not loopback.
	static bool IsLoopbackAddress(const FString& PeerAddress);

	// No Origin, an empty one, "null", or http(s)://localhost|127.0.0.1|[::1]
	// with any port.
	static bool IsOriginAllowed(const TArray<FString>& Origins);

	// Exactly one "Bearer <token>" value whose token equals ExpectedToken
	// (constant time). An empty ExpectedToken never authorizes.
	static bool IsAuthorized(const TArray<FString>& AuthorizationHeaders, const FString& ExpectedToken);

	static bool ConstantTimeEquals(const FString& A, const FString& B);

	// Split a tool result into MCP text + isError. Strips a surrounding
	// <tool_use_error>...</tool_use_error> wrapper (isError=true); an unwrapped
	// JSON object with "success":false is also an error.
	static void ShapeToolResult(const FString& RawResult, FString& OutText, bool& bOutIsError);

	// "read_blueprint" -> "Read Blueprint".
	static FString MakeToolTitle(const FString& ToolName);

private:
	// One parsed JSON-RPC message. Returns null for notifications and for
	// client-sent responses, which get no reply.
	TSharedPtr<FJsonObject> HandleMessage(const TSharedPtr<FJsonValue>& Message);

	TSharedPtr<FJsonObject> HandleInitialize(const TSharedPtr<FJsonObject>& Params) const;
	TSharedPtr<FJsonObject> HandleToolsCall(const TSharedPtr<FJsonObject>& Params, FString& OutErrorMessage);

	static TSharedPtr<FJsonObject> MakeResult(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Result);
	static TSharedPtr<FJsonObject> MakeError(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message);

	// tools/list entries, built once in the constructor.
	TArray<TSharedPtr<FJsonValue>> ToolListEntries;
	TArray<FString> ExposedToolNames;
	TSet<FString> ExposedToolSet;

	FKismetAIMcpToolCallFn CallTool;
	FString ServerVersion;
};
