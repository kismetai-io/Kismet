// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "KismetAIBridge.generated.h"

class SKismetAIPanel;

/**
 * Bridge UObject exposed to JavaScript via SWebBrowser::BindUObject.
 * All UFUNCTIONs become callable from JS as window.ue.kismetai.<lowercasename>().
 * Return values become JS Promises.
 */
UCLASS()
class UKismetAIBridge : public UObject
{
	GENERATED_BODY()

public:
	/** Called from JS when user clicks Send. Shim: builds a one-element
	 *  focused-BP refs array from the active blueprint and calls SendMessageWithRefs. */
	UFUNCTION()
	void SendMessage(const FString& Text, bool bIncludeBlueprint);

	/** Called from JS when user sends a message with pasted images.
	 *  ImagesJson is a JSON array of {media_type, data} objects.
	 *  Shim: same as SendMessage but with images. */
	UFUNCTION()
	void SendMessageWithImages(const FString& Text, bool bIncludeBlueprint, const FString& ImagesJson);

	/**
	 * Primary send entry point for the @-reference UI.
	 * RefsJson  — JSON array of {path, name, class, focused?} from the @-mention picker.
	 * ImagesJson — JSON array of {media_type, data} objects (may be empty "").
	 * Runs through the standard session-gate before launching the agent.
	 */
	UFUNCTION()
	void SendMessageWithRefs(const FString& Text, const FString& RefsJson, const FString& ImagesJson);

	UFUNCTION()
	FString SkillCommand(const FString& Action, const FString& Payload);

	UFUNCTION()
	void SkillRegistryRequest(const FString& Action, const FString& Payload);

	/** Called from JS when user clicks Stop. */
	UFUNCTION()
	void Abort();

	/** Called from JS when the user answers an ask_user prompt. AnswersJson is a
	 *  JSON object mapping each question to the selected option (or free text). */
	UFUNCTION()
	void AnswerQuestion(const FString& QuestionId, const FString& AnswersJson);

	/** Returns the currently focused Blueprint asset path. */
	UFUNCTION()
	FString GetActiveBlueprintPath();

	/**
	 * Opens an asset in its editor. Called from JS when the user clicks an asset
	 * path rendered in agent output (e.g. "/Game/Foo/BP_Bar.BP_Bar"). Accepts a
	 * full object path or a package path (the ".Name" is appended when absent).
	 * Returns true if the asset was found and an editor opened. No-op + false for
	 * an unknown path (clicking a hallucinated path does nothing).
	 */
	UFUNCTION()
	bool OpenAsset(const FString& AssetPath);

	/**
	 * Search project assets by name substring and optional type filter.
	 * Called from JS for @-mention autocomplete.
	 *
	 * Query        — case-insensitive name substring (empty = all names).
	 * TypeFilter   — friendly class name: Blueprint, Material, Texture2D, …
	 *                (empty = all types; unknown → empty result).
	 *
	 * Returns a JSON array of up to 50 objects: [{path, name, class}, …]
	 * where:
	 *   path  — full object path e.g. "/Game/Blueprints/BP_Player.BP_Player"
	 *   name  — short asset name e.g. "BP_Player"
	 *   class — friendly class name e.g. "Blueprint"
	 *
	 * No assets are loaded. Runs on the game thread.
	 */
	UFUNCTION()
	FString SearchAssets(const FString& Query, const FString& TypeFilter);

	/** Returns true if the HTTP server on :8323 is running. */
	UFUNCTION()
	bool GetServerStatus();

	/** Returns current system clipboard text. */
	UFUNCTION()
	FString GetClipboard();

	/** Sets system clipboard text. */
	UFUNCTION()
	void SetClipboard(const FString& Text);

	/** Checks clipboard for an image. Saves to temp file, returns JSON {filename, path} or empty. */
	UFUNCTION()
	FString GetClipboardImage();

	/** Reads base64 data for a previously pasted clipboard image by filename. */
	UFUNCTION()
	FString ReadImageBase64(const FString& Filename);

	// --- Telemetry consent ---

	/** True once the server (via /auth/plugin/me) has told us whether the user
	 *  has acknowledged the telemetry notice. Until then the consent state is
	 *  unknown and the UI must not show the notice. */
	UFUNCTION()
	bool IsTelemetryConsentKnown();

	/** True if the user has acknowledged the telemetry notice. Only meaningful
	 *  when IsTelemetryConsentKnown() is true. */
	UFUNCTION()
	bool IsTelemetryConsented();

	/** Records telemetry-consent acknowledgment server-side (async POST). On
	 *  completion, pushes window.onTelemetryConsentResult(ok) to JS; on success
	 *  also re-pushes the initial state so the gate clears. */
	UFUNCTION()
	void AcceptTelemetryConsent();

	/** True if a key is set for the active model's provider (env var or keystore). */
	UFUNCTION()
	bool HasApiKey();

	/** Store the key for the active model's provider (OpenAI -> openai_key.enc,
	 *  MiniMax -> minimax_key.enc, otherwise anthropic_key.enc) and update config. */
	UFUNCTION()
	bool SetApiKey(const FString& Key);

	/** Clear the stored key for the active model's provider and revert config to env. */
	UFUNCTION()
	void ClearApiKey();

	/** Open the native Kismet Settings window (API keys, …), focused on the selected model's provider. */
	UFUNCTION()
	void OpenSettings();

	/**
	 * Open the API keys window focused on one provider profile (e.g. "google").
	 * An id that takes no key just opens the window. Brings an open window forward.
	 */
	UFUNCTION()
	void OpenApiKeys(const FString& ProviderId);

	/** Open the native bug-report window for the current session. Game-thread Slate. */
	UFUNCTION()
	void OpenBugReport();

	/** Open the documentation site in the system default browser. */
	UFUNCTION()
	void OpenDocs();

	/**
	 * Called by the page once its script has booted. Until then the panel keeps
	 * its native fallback toolbar visible, so Report Bug stays reachable when the
	 * web UI fails to load or its script throws on startup.
	 */
	UFUNCTION()
	void NotifyUiReady();

	// --- Device login ---

	/**
	 * Initiates device-login flow with the Kismet backend.
	 * Result is pushed to JS via window.onKismetLoginResult(obj), where obj is
	 * { code, verification_url, expires_in_seconds, poll_interval_seconds } on
	 * success or { error: "..." } on failure.
	 */
	UFUNCTION()
	void BeginKismetLogin();

	/**
	 * Polls the Kismet backend for one round to check login status.
	 * Result is pushed to JS via window.onKismetLoginPollResult(obj), where obj is
	 * { status: "pending"|"claimed"|"expired"|"not_found"|"error",
	 *   email?: string, display_name?: string, error?: string }.
	 * On "claimed", saves the session to ~/.kismet/credentials.json before pushing.
	 */
	UFUNCTION()
	void PollKismetLogin(const FString& Code);

	/** Logs out of the Kismet backend session (clears ~/.kismet/credentials.json). */
	UFUNCTION()
	void KismetLogout();

	/** Opens the given URL in the system default browser. */
	UFUNCTION()
	void OpenInBrowser(const FString& Url);

	/** Build (execute) a saved plan: reads the .plan.md and starts a fresh agent run to implement it. */
	UFUNCTION()
	void BuildPlan(const FString& FilePath);

	/**
	 * Open a saved .plan.md in a chosen editor. EditorName is a source-code
	 * accessor FName (as returned by GetSourceCodeEditors); "system" or empty
	 * opens it with the OS default app. Falls back to the OS default if the
	 * named accessor cannot open the file.
	 */
	UFUNCTION()
	void OpenPlanInEditor(const FString& FilePath, const FString& EditorName);

	/**
	 * List the source-code editors available on this machine for the View Plan
	 * chooser. Returns JSON [{name, label}] of accessors that can open files.
	 */
	UFUNCTION()
	FString GetSourceCodeEditors();

	/** The remembered "View Plan" editor name (accessor FName, or "" for OS default). */
	UFUNCTION()
	FString GetPlanEditor();

	/** Persist the preferred "View Plan" editor (accessor FName, or "" for OS default). */
	UFUNCTION()
	void SetPlanEditor(const FString& EditorName);

	/** Returns true if a valid Kismet session exists on disk. */
	UFUNCTION()
	bool HasKismetSession();

	/**
	 * Returns the email address from the stored Kismet session,
	 * or an empty string if no session exists.
	 */
	UFUNCTION()
	FString GetSessionEmail();

	/** Returns the current model ID (e.g. "claude-opus-4-6"). */
	UFUNCTION()
	FString GetModel();

	/**
	 * Switch the model. Accepts aliases (opus, sonnet, haiku, opus-1m, sonnet-1m)
	 * or full model IDs. Persists to ~/.kismet/settings.json.
	 * Returns the resolved model ID on success, empty string on failure.
	 */
	UFUNCTION()
	FString SetModel(const FString& ModelAlias);

	/** Returns a JSON array of model ids last picked via SetModel, most recent first (at most 3). */
	UFUNCTION()
	FString GetRecentModels();

	/** Returns JSON array of available models [{alias, model_id, display_name, context_window}]. */
	UFUNCTION()
	FString GetAvailableModels();

	/**
	 * Returns JSON array of provider profiles (Config/providers.json):
	 * [{id, display_name, protocol, local, custom_models, has_key, key_url,
	 *   base_url, base_url_overridden, active}]. Never includes key values.
	 */
	UFUNCTION()
	FString GetProviders();

	/**
	 * Select a provider profile and model. Empty ProviderId = derive the
	 * provider from the model (the pre-profile behaviour). A catalog alias or id
	 * resolves as SetModel does; profiles with custom models also accept any id.
	 * BaseUrl, when non-empty, overrides the profile's endpoint (local runtimes,
	 * custom servers); "-" clears an override. Persists to settings.json.
	 * Returns the model id now in use, or empty on failure.
	 */
	UFUNCTION()
	FString SetProvider(const FString& ProviderId, const FString& Model, const FString& BaseUrl);

	/**
	 * Set the context window a local or custom model really runs with
	 * (Tokens <= 0 clears it back to the reported or assumed figure). Persists
	 * to settings.json. Returns the window now in use, or 0 when refused
	 * (cloud provider, unknown profile, empty id).
	 */
	UFUNCTION()
	int32 SetModelContextWindow(const FString& ProviderId, const FString& ModelId, int32 Tokens);

	/** Re-list local servers and refresh stale cloud model lists in the background. */
	UFUNCTION()
	void RefreshModels();

	/** True once the first-run account welcome has been answered (signed in or skipped). */
	UFUNCTION()
	bool IsAccountWelcomeSeen();

	/** Marks the first-run account welcome as answered. Persists to settings.json. */
	UFUNCTION()
	void MarkAccountWelcomeSeen();

	/**
	 * Returns current thinking mode as string: "disabled", "enabled", "adaptive".
	 */
	UFUNCTION()
	FString GetThinkingMode();

	/**
	 * Switch thinking mode. Accepts "disabled"/"off", "enabled"/"budget", "adaptive".
	 * Optionally set budget tokens for "enabled" mode (comma-separated: "enabled,10000").
	 * Persists to ~/.kismet/settings.json.
	 * Returns the resolved mode string on success.
	 */
	UFUNCTION()
	FString SetThinkingMode(const FString& Mode);

	/**
	 * Returns current thinking budget tokens (only relevant when mode == "enabled").
	 */
	UFUNCTION()
	int32 GetThinkingBudgetTokens();

	/**
	 * Returns current effort level as string: "low", "medium", "high".
	 */
	UFUNCTION()
	FString GetEffortLevel();

	/**
	 * Set effort level. Accepts "low", "medium", "high".
	 * Persists to ~/.kismet/settings.json.
	 * Returns the resolved level string on success, empty on invalid input.
	 */
	UFUNCTION()
	FString SetEffortLevel(const FString& Level);

	/**
	 * Returns current fast-mode state as "on" or "off".
	 * Fast mode trades 2x token price for ~2.5x speed on models with a fast tier.
	 */
	UFUNCTION()
	FString GetFastMode();

	/**
	 * Set fast mode. Accepts "on"/"true"/"1" or "off"/"false"/"0".
	 * Persists to ~/.kismet/settings.json.
	 * Returns the resolved state ("on"/"off"); empty string on invalid input.
	 */
	UFUNCTION()
	FString SetFastMode(const FString& State);

	// --- Project context (/init onboarding) ---

	/**
	 * Create or import the project-context file (AGENTS.md) at the project root.
	 * - If a chain file (KISMET/AGENTS/CLAUDE/GEMINI/KISMETAI.md) already exists,
	 *   does nothing and reports status "exists".
	 * - Else if other-format agent-tool rules are found (.cursorrules, .windsurfrules,
	 *   .cursor/rules/, …), concatenates them into AGENTS.md → status "imported".
	 * - Else writes a seeded template → status "created".
	 * Opens the written file for the user. Runs on the game thread.
	 * Returns JSON {status, target, imported_from:[…], message}.
	 */
	UFUNCTION()
	FString InitProjectContext();

	/**
	 * Reports whether a project-context file exists and what could be imported,
	 * for the first-load onboarding banner.
	 * Returns JSON {has_context, context_file, importable:[…], onboarding_dismissed}.
	 */
	UFUNCTION()
	FString GetProjectContextStatus();

	/** Marks the onboarding banner dismissed and persists it to settings.json. */
	UFUNCTION()
	void DismissOnboarding();

	/**
	 * Detect every other-tool config the user could import (for the /import panel).
	 * Returns JSON {target, existing_target, onboarding_dismissed, sources:[{id,label,is_dir}]}.
	 */
	UFUNCTION()
	FString GetImportSources();

	/**
	 * Import the selected sources into the Kismet context file. SelectedIdsJson is
	 * a JSON array of source ids (as returned by GetImportSources). Appends to an
	 * existing KISMET.md/AGENTS.md or creates AGENTS.md, then opens it.
	 * Returns JSON {status, target, imported_from:[…], message}.
	 */
	UFUNCTION()
	FString ImportSources(const FString& SelectedIdsJson);

	// --- Session management ---

	/** Returns JSON array of session summaries for the history sidebar. */
	UFUNCTION()
	FString ListSessions();

	/** Load a session's entries for UI replay. Returns JSON array of all entries. */
	UFUNCTION()
	FString LoadSession(const FString& SessionId);

	/** Delete a session by ID. Returns true on success. */
	UFUNCTION()
	bool DeleteSession(const FString& SessionId);

	/** Start a new session (clears current session context in the panel). */
	UFUNCTION()
	void NewSession();

	/** Set the active session ID without loading entries (for tab switching). */
	UFUNCTION()
	void SetSessionId(const FString& SessionId);

	/** Save tab state JSON to Saved/KismetAI/tabs.json. */
	UFUNCTION()
	void SaveTabState(const FString& Json);

	/** Load tab state JSON from Saved/KismetAI/tabs.json. Returns empty if not found. */
	UFUNCTION()
	FString LoadTabState();

	// --- Message queue bridge ---

	/** Enqueue a user message. If no agent is running, dispatches immediately. */
	UFUNCTION()
	void EnqueueMessage(const FString& Text, const FString& RefsJson, const FString& ImagesJson);

	/** Promote a new message to PendingSteer; if agent is running, aborts it so the finalize picks up the steer. */
	UFUNCTION()
	void SteerNow(const FString& Text, const FString& RefsJson, const FString& ImagesJson);

	/** Promote an existing queued message to PendingSteer by Id. */
	UFUNCTION()
	void SteerQueued(const FString& Id);

	/** Remove a queued message by Id. */
	UFUNCTION()
	void RemoveQueued(const FString& Id);

	/** Move a queued message to a new index by Id. */
	UFUNCTION()
	void MoveQueued(const FString& Id, int32 NewIndex);

	/** Edit the text of a queued message by Id. */
	UFUNCTION()
	void EditQueued(const FString& Id, const FString& NewText);

	/** Returns the current queue state as a JSON array. */
	UFUNCTION()
	FString GetQueueJson();

	// --- Diff accept/reject ---

	/** Accept all pending Blueprint changes (clear markers, keep nodes). */
	UFUNCTION()
	void AcceptChanges();

	/** Reject all pending Blueprint changes (delete added nodes, recompile). */
	UFUNCTION()
	void RejectChanges();

	/** Reapply the last rejected changes (re-inject stored T3D). */
	UFUNCTION()
	void ReapplyChanges();

	/** Request the current diff state (pushes onDiffReady/onDiffResolved to caller panel). */
	UFUNCTION()
	void RequestDiffState();

	// --- Auto-update ---

	/** Fetches the manifest, finds an asset matching this engine+platform, then
	 *  pushes window.onUpdateCheckResult({current, latest, hasUpdate, hasMatchingAsset,
	 *  url, size, notes, error}) to JS. */
	UFUNCTION()
	void CheckForUpdate();

	/** Same as CheckForUpdate but only runs once per editor process lifetime.
	 *  Subsequent calls (e.g. from panel reloads) are no-ops. */
	UFUNCTION()
	void AutoCheckForUpdate();

	/** Downloads AssetURL, extracts, applies via rename-aside. Pushes
	 *  window.onUpdateInstallResult({ok, error}) when done. */
	UFUNCTION()
	void InstallUpdate(const FString& AssetURL);

	/** Restart the editor (UnrealEdMisc warns about unsaved work). */
	UFUNCTION()
	void RestartEditor();

	/** Set by SKismetAIPanel on construction. */
	TWeakPtr<SKismetAIPanel> OwnerPanel;
};
