// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class SEditableTextBox;
class SScrollBox;
class STextBlock;
class SWidget;
class SWindow;

// "Kismet Settings" window. Hosts the API Keys section: one row per provider
// profile in Config/providers.json that takes a key, in that file's order
// (which matches the model menu). Each row shows the key's status, a "Get a
// key" link, a password field, Save and Clear.
//
// Keys are written to the per-machine encrypted keystore (KismetAIKeyStore) and
// applied to the running FKismetAIAgentConfig immediately — no editor restart.
// Stored key values are never displayed; rows show only their status.
class SKismetAISettingsDialog : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SKismetAISettingsDialog) {}
		// Provider profile id whose row is scrolled into view and focused.
		SLATE_ARGUMENT(FString, FocusProviderId)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// Opens the window, or brings the open one to the front, and focuses the
	// FocusProviderId row when it names a provider that takes a key. Parented
	// to the editor's main window when available; non-modal.
	static void OpenAsWindow(const FString& FocusProviderId = FString());

	// Provider ids shown as rows, in display order. Exposed for tests.
	static TArray<FString> GetKeyProviderIds();

private:
	struct FKeyRow
	{
		FString ProviderId;
		FString Slot;          // keystore slot
		FString EnvVarName;    // overrides the stored key when set
		FString KeyUrl;        // where to get a key; may be empty
		FText DisplayName;

		TSharedPtr<SWidget> Widget;
		TSharedPtr<SEditableTextBox> Input;
		TSharedPtr<STextBlock> Status;
	};

	TArray<FKeyRow> Rows;
	TSharedPtr<SScrollBox> ScrollBox;

	TSharedRef<SWidget> BuildKeyRow(int32 RowIndex);
	void RefreshStatus(int32 RowIndex);
	void FocusProvider(const FString& ProviderId);

	FReply OnSaveClicked(int32 RowIndex);
	FReply OnClearClicked(int32 RowIndex);

	void CloseHostingWindow();

	static TWeakPtr<SWindow> OpenWindow;
	static TWeakPtr<SKismetAISettingsDialog> OpenDialog;
};
