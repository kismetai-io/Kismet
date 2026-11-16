// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"

struct FHttpServerRequest;
struct FHttpServerResponse;
typedef TFunction<void(TUniquePtr<FHttpServerResponse>&& Response)> FHttpResultCallback;

// Per-launch bearer-token auth for the local HTTP servers (KismetAI :8323 and
// KismetAIEval :8324). Both bind to 127.0.0.1, but the browser CORS rules treat
// `Content-Type: text/plain` POSTs as "simple requests" — the response is
// blocked but the side effect runs. Any malicious page in any tab can mutate
// Blueprint graphs unless the server demands a header CORS would have to
// preflight (and rejects requests that carry an Origin header at all).
namespace KismetAIServerAuth
{
	// Initialize the per-launch token. Honors $KISMETAI_SERVER_TOKEN if set
	// (used by the eval framework so it can talk back to the editor); otherwise
	// generates 32 random bytes and persists hex-encoded to
	// ~/.kismet/server_token (mode 0600). Idempotent.
	KISMETAI_API void Initialize();

	// Returns the active token. Empty before Initialize() runs.
	KISMETAI_API const FString& GetToken();

	// Filesystem path of the persisted token. Public so eval tooling can
	// discover it without parsing logs.
	KISMETAI_API FString GetTokenPath();

	// Returns true if the request is authorized. On reject, dispatches a 401
	// or 403 response via OnComplete and returns false — the caller should
	// `return true` from the route handler immediately.
	KISMETAI_API bool CheckRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
}
