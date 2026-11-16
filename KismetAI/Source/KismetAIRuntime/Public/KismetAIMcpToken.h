// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

// Bearer token for the MCP endpoint (POST /mcp).
//
// Per install, not per launch: an MCP client stores the header in its own
// config (`claude mcp add ... --header "Authorization: Bearer ..."`), so a token
// that changed on every editor start would break every client on every start.
// That is the difference from KismetAIServerAuth's X-Kismet-Token, which guards
// the /api/v1 routes and is regenerated each launch.
//
// Stored at <ConfigDir>/mcp_token (normally ~/.kismet/mcp_token), mode 0600.
// Delete the file to rotate it; the next MCP request writes a new one.
namespace KismetAIMcpToken
{
	KISMETAIRUNTIME_API FString GetTokenPath();

	// The current token, generating and persisting a fresh 64-hex-char one when
	// the file is missing or does not hold a usable token. Re-applies owner-only
	// permissions on every call. Returns empty if no token could be persisted:
	// a token no client can read is useless, and the endpoint fails closed.
	// Never logs the token value.
	KISMETAIRUNTIME_API FString GetOrCreateToken();

	// At least 32 chars of [A-Za-z0-9_-]. Lets a user drop in a token of their
	// own choosing without the server silently replacing it.
	KISMETAIRUNTIME_API bool IsUsableToken(const FString& Token);
}
