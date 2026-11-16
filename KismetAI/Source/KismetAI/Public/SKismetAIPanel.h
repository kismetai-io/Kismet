// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "UObject/StrongObjectPtr.h"
#include "KismetAIAgent.h" // FKismetAIAgent, FOnAgentStep
#include "KismetAIBridge.h"

class SWebBrowser;

class SKismetAIPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SKismetAIPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SKismetAIPanel();
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	// --- Public API ---

	/**
	 * Primary launch entry — accepts a pre-built refs JSON array from JS.
	 * RefsJson: JSON array of {path, name, class, focused?} objects (may be empty "[]").
	 * Delegates directly to FKismetAIModule::LaunchAgent with the refs JSON.
	 */
	void LaunchAgentWithRefs(const FString& UserMessage, const FString& RefsJson, const FString& ImagesJson = TEXT(""));

	/**
	 * Legacy shim — builds a one-element focused-BP refs array when bIncludeBlueprint is
	 * true and ActiveBlueprintPath is non-empty, then calls LaunchAgentWithRefs.
	 * Kept for compatibility with existing GateAndLaunch callers.
	 */
	void LaunchAgentFromWeb(const FString& UserMessage, bool bIncludeBlueprint, const FString& ImagesJson = TEXT(""));
	void AbortAgent();
	FString GetActiveBlueprintPathString() const { return ActiveBlueprintPath; }

	// Execute JS on the web browser (buffered until ready)
	void ExecuteJavascriptSafe(const FString& Script);

	// Called by module when agent stops (sets pending flag for Tick delivery).
	// bWillContinue is true when another queued/steered message will launch immediately.
	void NotifyAgentStopped(bool bWillContinue);

	// Re-evaluate login state and refresh the JS overlay accordingly.
	// Public so the bridge can show the sign-in overlay after a session-gate
	// rejects an agent run, and the periodic re-validation tick can do the
	// same on a server-side revoke.
	void PushInitialState();

	// The page reports in through the bridge once its script has booted. The
	// native toolbar is only a fallback for when that never happens.
	void SetWebUiReady(bool bReady) { bWebUiReady = bReady; }

private:
	// WebBrowser
	TSharedPtr<SWebBrowser> WebBrowser;
	bool bWebBrowserReady = false;
	bool bWebUiReady = false;
	TArray<FString> PendingJSCalls;

	// Loading overlay (shown while WebBrowser loads)
	TSharedPtr<SWidget> LoadingOverlay;

	// Bridge UObject (prevent GC via strong ref)
	TStrongObjectPtr<UKismetAIBridge> Bridge;

	// Pending signals (retried on Tick if WebView was hidden)
	bool bPendingAgentStopped = false;
	bool bPendingStopWillContinue = false;

	// Blueprint tracking
	FString ActiveBlueprintPath;
	float TimeSinceLastBPCheck = 0.f;
	TArray<FString> PreviousOpenBlueprintPaths;
	void DetectFocusedBlueprint();

	// Periodic re-validation of the Kismet session while the panel is alive.
	// Catches dashboard-side revocations within the tick interval even when
	// the user makes no agent calls.
	float TimeSinceLastAuthCheck = 0.f;

	// HTML loading
	void LoadHTML();
};
