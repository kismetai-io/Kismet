// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "HttpRouteHandle.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "KismetAIToolExecution.h"

class FKismetAIModule;
class FKismetAIHttpTransport;

// HTTP binding for the MCP endpoint. The protocol itself is FKismetAIMcpServer
// (KismetAIRuntime); this file only turns an FHttpServerRequest into bytes and
// request metadata, and the server's answer back into an FHttpServerResponse.
//
//   POST   /mcp   JSON-RPC (Streamable HTTP, JSON responses only, no sessions)
//   GET    /mcp   405 — no server-initiated SSE stream
//   DELETE /mcp   405 — no sessions to terminate
//
// Auth is NOT the /api/v1 X-Kismet-Token gate: MCP clients send
// "Authorization: Bearer <~/.kismet/mcp_token>" (KismetAIMcpToken.h), and the
// peer must be loopback.
namespace KismetAIMcpRoute
{
	void Bind(FKismetAIModule& Module, const TSharedPtr<FKismetAIHttpTransport>& Router, TArray<FHttpRouteHandle>& OutHandles);

	// What one MCP tools/call runs once the transport has admitted the request.
	// Exposed so KismetAI.ExecutionParity drives the production MCP path rather
	// than a copy of it.
	// Caller is what the MCP transport verified (bearer token, loopback peer,
	// Origin) — the route passes it; it is never assumed here.
	FString CallTool(FKismetAIModule& Module, const FKismetAICallerContext& Caller,
		const FString& ToolName, const TSharedPtr<FJsonObject>& Arguments);

	// Every schema the MCP server advertises (core + on-demand files).
	TArray<TSharedPtr<FJsonValue>> GetToolSchemas(FKismetAIModule& Module);
}
