// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

// Bearer-token authentication for the Kismet backend (separate from the BYOK
// Anthropic key handled by KismetAIKeyStore).
//
// Files in <ConfigDir>/:
//   credentials[-<host>].json
//                      { token, machine_id, user_id, email, display_name }
//                      Mode 0600. Token is the plaintext bearer used in
//                      Authorization: Bearer <token> headers. Server-side
//                      revocation makes this safe to keep at rest as plain
//                      JSON; tier-2 keystore upgrade may move it later.
//                      Scoped per backend host (production keeps the bare
//                      "credentials.json"; dev/custom get "credentials-<host>")
//                      so a token issued on one backend isn't 401'd-and-wiped
//                      when another backend is briefly active.
//   machine_id         A stable per-install UUID (text). Generated on first
//                      use and persisted forever — distinguishes multiple
//                      Kismet installs across reinstalls/migrations.

struct FKismetAIAuthSession
{
	FString Token;
	FString MachineId;
	FString UserId;
	FString Email;
	FString DisplayName;

	bool IsValid() const { return !Token.IsEmpty(); }
};

namespace KismetAIAuth
{
	// Backend URL resolution is inverted.
	//
	// GetBackendUrl used to read UKismetAISettings::Backend directly. That is a
	// UDeveloperSettings, so it drags in Engine + DeveloperSettings — which would
	// defeat the point of a runtime module that cannot reach an editor type.
	//
	// The editor module installs a resolver at StartupModule; the runtime keeps
	// the env-var override and the defaults. With no resolver installed (tests,
	// or a headless runtime) the production default applies.
	KISMETAIRUNTIME_API void SetBackendUrlResolver(TFunction<FString()> Resolver);

	// The development backend, so the editor module's resolver can name it
	// without duplicating the literal.
	KISMETAIRUNTIME_API FString GetDevelopmentBackendUrl();

	// Backend base URL. Resolution order: KISMET_BACKEND_URL env var ->
	// UKismetAISettings (Project Settings -> Plugins -> KismetAI) ->
	// https://kismetai.io.
	KISMETAIRUNTIME_API FString GetBackendUrl();

	// Reads/creates ~/.kismet/machine_id. Stable across launches; never
	// changes once written.
	KISMETAIRUNTIME_API FString GetMachineId();

	// File-backed session.
	KISMETAIRUNTIME_API bool HasSession();
	KISMETAIRUNTIME_API bool LoadSession(FKismetAIAuthSession& OutSession);
	KISMETAIRUNTIME_API bool SaveSession(const FKismetAIAuthSession& Session);
	KISMETAIRUNTIME_API bool ClearSession();

	// Result of GET /auth/plugin/me.
	enum class EValidateResult { Valid, Invalid, Transient };
	using FValidateCallback = TFunction<void(EValidateResult, const FKismetAIAuthSession&)>;

	// Async GET /auth/plugin/me with current bearer. Calls OnComplete on the
	// game thread. On 200 → Valid + refreshed session. On 401 → clears the
	// local session + Invalid + empty session. On network error / 5xx /
	// malformed → Transient + original local session (file untouched). If no
	// local session exists → Invalid synchronously without an HTTP call.
	KISMETAIRUNTIME_API void ValidateSession(FValidateCallback OnComplete);

	// Synchronous wrapper around ClearSession(). Returns true on success.
	KISMETAIRUNTIME_API bool Logout();

	// Records the user's telemetry-consent acknowledgment server-side via
	// POST /auth/plugin/telemetry/consent (a thin kismetai.io proxy that
	// forwards to the Supabase edge function). Requires a local session
	// bearer. OnComplete(true) on a 2xx — consent is now recorded and the
	// local KismetAITelemetry consent mirror is set to consented — else
	// OnComplete(false). Fires on the game thread.
	using FConsentCallback = TFunction<void(bool /*bOk*/)>;
	KISMETAIRUNTIME_API void AcceptTelemetryConsent(FConsentCallback OnComplete);

	// Result of POST /auth/plugin/start.
	struct FStartResult
	{
		bool bSuccess = false;
		FString ErrorMessage;

		FString Code;
		FString ConfirmNumber;   // 2-digit number to display in the plugin UI
		FString VerificationUrl;
		int32 ExpiresInSeconds = 0;
		int32 PollIntervalSeconds = 2;
	};
	using FStartCallback = TFunction<void(const FStartResult&)>;

	// Hits POST /auth/plugin/start with the given machine hint. Calls
	// OnComplete on the game thread.
	KISMETAIRUNTIME_API void BeginDeviceLogin(FStartCallback OnComplete);

	// Result of POST /auth/plugin/poll for one round.
	enum class EPollStatus
	{
		Pending,    // 200 { status: "pending" }
		Claimed,    // 200 { status: "claimed", token, user }
		NotFound,   // 404
		Expired,    // 410
		Error,      // network or server error
	};
	struct FPollResult
	{
		EPollStatus Status = EPollStatus::Error;
		FString ErrorMessage;
		FKismetAIAuthSession Session;   // populated when Status == Claimed
	};
	using FPollCallback = TFunction<void(const FPollResult&)>;

	// One round-trip POST /auth/plugin/poll. Caller drives the poll loop.
	// OnComplete fires on the game thread.
	KISMETAIRUNTIME_API void PollOnce(const FString& Code, FPollCallback OnComplete);
}
