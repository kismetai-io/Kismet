// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;

// Legacy metadata telemetry compatibility. Every payload is projected through
// KismetAIDiagnostics' allowlist before local storage or existing HTTP dispatch.
// Prompts, tool content, asset identifiers and free-form errors are excluded.
// Local fallback is bounded safe JSONL; raw opt-in capture is a separate local
// API that cannot dispatch remotely.
// KISMET_TELEMETRY_DISABLE=1 disables legacy network dispatch;
// KISMET_DIAGNOSTICS_DISABLE=1 disables local diagnostic writes.

namespace KismetAITelemetry
{
	// Run lifecycle ---------------------------------------------------------

	struct KISMETAIRUNTIME_API FRunStartPayload
	{
		FString ClientRunId;
		FDateTime StartedAt;
		FString Model;
		FString SessionId;
		FString InitialPrompt;
		FString ProjectName;
		FString BlueprintPath;
	};

	struct KISMETAIRUNTIME_API FRunMessagePayload
	{
		FString ClientRunId;
		int32 Idx = 0;
		FString Role;                       // user, assistant, tool_result, system
		TSharedPtr<FJsonValue> Content;     // Compatibility input; excluded at dispatch.
		FString Model;
		int32 InputTokens = 0;
		int32 OutputTokens = 0;
		int32 CacheReadTokens = 0;
		int32 CacheCreationTokens = 0;
		int32 DurationMs = 0;
	};

	struct KISMETAIRUNTIME_API FRunEndPayload
	{
		FString ClientRunId;
		FDateTime EndedAt;
		FString Status;          // completed, failed, cancelled
		FString StopReason;      // end_turn, max_tokens, tool_use, max_steps, error
		int32 NumSteps = 0;
		int32 NumToolCalls = 0;
		int32 InputTokens = 0;
		int32 OutputTokens = 0;
		int32 CacheReadTokens = 0;
		int32 CacheCreationTokens = 0;
		double CostUSD = 0.0;
		FString ErrorMessage;
	};

	// Module entry / exit ---------------------------------------------------

	// Called from FKismetAIModule::StartupModule. Reads opt-out env var,
	// installs the log forwarder, scans Saved/Crashes for new dumps, posts
	// the plugin_loaded event.
	KISMETAIRUNTIME_API void Initialize();

	// Called from FKismetAIModule::ShutdownModule. Posts plugin_unloaded,
	// removes the log forwarder.
	KISMETAIRUNTIME_API void Shutdown();

	// Cheap toggle. False if KISMET_TELEMETRY_DISABLE=1 or never initialized.
	KISMETAIRUNTIME_API bool IsEnabled();

	// Per-category prefs mirrored from the server (/auth/plugin/me).
	//
	// Defaults are all-true so a fresh install (or any path that hasn't yet
	// completed a session validation) behaves like prior versions until the
	// first /me response lands. Once set, the values persist for the lifetime
	// of the process; KismetAIAuth refreshes them on every ValidateSession.
	//
	// Category prefs still gate the existing metadata HTTP dispatch. Local
	// observations use the independent bounded diagnostic sink and its opt-out.
	KISMETAIRUNTIME_API void UpdatePrefs(bool bRuns, bool bToolCalls, bool bErrors, bool bCrashes);

	// Telemetry-consent acknowledgment, mirrored from /auth/plugin/me
	// (profiles.telemetry_consented_at: null = not yet acknowledged).
	//
	// Tri-state: until the first /me response (or a successful Accept) lands,
	// the consent state is *unknown* — callers must NOT treat unknown as
	// "not consented" and must not gate UI on it. IsConsentKnown() reports
	// whether HasConsented() carries a server-confirmed value.
	KISMETAIRUNTIME_API void UpdateConsent(bool bConsented);
	KISMETAIRUNTIME_API bool IsConsentKnown();
	KISMETAIRUNTIME_API bool HasConsented();

	// Reporters -------------------------------------------------------------
	// All callable from any thread. They enqueue + dispatch HTTP from the
	// game thread. Safe local observations are attempted before HTTP dispatch.

	KISMETAIRUNTIME_API void ReportRunStarted(const FRunStartPayload& Payload);
	KISMETAIRUNTIME_API void ReportRunMessage(const FRunMessagePayload& Payload);
	KISMETAIRUNTIME_API void ReportRunEnded(const FRunEndPayload& Payload);

	// Tool call breadcrumb. Folded into events with type='tool_call'.
	KISMETAIRUNTIME_API void ReportToolCall(
		const FString& ClientRunId,
		const FString& ToolName,
		bool bSuccess,
		int32 ResultBytes,
		int32 DurationMs,
		const FString& ErrorMessage = TEXT(""));

	// Generic event. Type goes to events.type, Payload to events.payload jsonb.
	// Type values used today: plugin_loaded, plugin_unloaded, tool_call,
	// api_error, log_warning, log_error, t3d_inject_error, compile_error,
	// editor_map_opened.
	KISMETAIRUNTIME_API void ReportEvent(const FString& Type, TSharedPtr<FJsonObject> Payload);

	// Crash report (own table). Called by the startup scanner; can also be
	// called by a runtime crash hook in future.
	KISMETAIRUNTIME_API void ReportCrash(TSharedPtr<FJsonObject> CrashData);

	// Current process safe diagnostic path (changed from legacy unbounded JSONL).
	KISMETAIRUNTIME_API FString GetLocalLogPath();
}
