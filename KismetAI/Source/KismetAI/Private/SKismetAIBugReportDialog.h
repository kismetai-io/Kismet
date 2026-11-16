// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class SMultiLineEditableTextBox;
class SCheckBox;
class SButton;
class STextBlock;

// Modal-style "Report a Bug" dialog. Collects a description and optionally
// the current session's transcript JSONL, then POSTs to
// {BACKEND_URL}/api/v1/bugs/report with the same bearer-auth + Vercel bypass
// pattern as KismetAITelemetry.
class SKismetAIBugReportDialog : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SKismetAIBugReportDialog) {}
		// Empty string when there is no active session — the "include
		// telemetry" checkbox is then disabled.
		SLATE_ARGUMENT(FString, CurrentSessionId)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// Convenience entry point. Creates an SWindow, adds the dialog as content,
	// and parents to the editor's main window if available. Non-modal so the
	// editor stays interactive while typing.
	static void OpenAsWindow(const FString& CurrentSessionId);

private:
	// Description hard cap (matches backend contract).
	static constexpr int32 MaxDescriptionLen = 10000;
	// Telemetry JSON size cap before truncation kicks in (1 MB).
	static constexpr int32 MaxTelemetryBytes = 1 * 1024 * 1024;

	// State
	FString CurrentSessionId;
	bool bSubmitting = false;

	// Widgets
	TSharedPtr<SMultiLineEditableTextBox> DescriptionBox;
	TSharedPtr<SCheckBox> IncludeTelemetryCheckbox;
	TSharedPtr<STextBlock> CharCounterText;
	TSharedPtr<STextBlock> TelemetryPreviewText;
	TSharedPtr<STextBlock> StatusText;
	TSharedPtr<SButton> SubmitButton;
	TSharedPtr<SButton> CancelButton;

	// Cached telemetry summary so the preview label only recomputes when the
	// checkbox flips, not on every paint.
	int32 CachedTelemetryEntryCount = 0;
	int32 CachedTelemetryBytes = 0;
	bool bTelemetryCached = false;

	// Handlers
	FReply OnSubmitClicked();
	FReply OnCancelClicked();
	void OnDescriptionChanged(const FText& NewText);
	void OnIncludeTelemetryChanged(ECheckBoxState NewState);

	// Helpers
	void UpdateCharCounter();
	void UpdateTelemetryPreview();
	void EnsureTelemetryCached();
	void SetStatusMessage(const FString& Message, bool bIsError);
	void SetButtonsEnabled(bool bEnabled);
	void CloseHostingWindow();

	// Build the request body and dispatch to the backend. Must run on the
	// game thread because it touches widget state and reads session storage.
	void DispatchSubmit(const FString& Description, bool bIncludeTelemetry);
};
