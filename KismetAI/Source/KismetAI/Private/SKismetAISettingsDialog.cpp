// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "SKismetAISettingsDialog.h"

#include "KismetAIKeyStore.h"
#include "KismetAIModule.h"
#include "KismetAIAgent.h"
#include "KismetAIProviderProfile.h"

#include "Containers/Ticker.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SHyperlink.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

TWeakPtr<SWindow> SKismetAISettingsDialog::OpenWindow;
TWeakPtr<SKismetAISettingsDialog> SKismetAISettingsDialog::OpenDialog;

namespace
{
	// A profile gets a row when it authenticates with a key the user supplies.
	// Local runtimes take none.
	bool TakesUserKey(const FKismetAIProviderProfile& Profile)
	{
		return !Profile.KeySlot.IsEmpty() && !Profile.bLocal && Profile.Auth != EKismetAIAuthStyle::None;
	}

	FString EnvValue(const FString& EnvVarName)
	{
		return EnvVarName.IsEmpty() ? FString() : FPlatformMisc::GetEnvironmentVariable(*EnvVarName);
	}
}

TArray<FString> SKismetAISettingsDialog::GetKeyProviderIds()
{
	TArray<FString> Ids;
	for (const FKismetAIProviderProfile& Profile : FKismetAIProviderRegistry::Get().GetProfiles())
	{
		if (TakesUserKey(Profile))
		{
			Ids.Add(Profile.Id);
		}
	}
	return Ids;
}

void SKismetAISettingsDialog::Construct(const FArguments& InArgs)
{
	for (const FKismetAIProviderProfile& Profile : FKismetAIProviderRegistry::Get().GetProfiles())
	{
		if (!TakesUserKey(Profile))
		{
			continue;
		}
		FKeyRow Row;
		Row.ProviderId = Profile.Id;
		Row.Slot = Profile.KeySlot;
		Row.EnvVarName = Profile.KeyEnvVar;
		Row.KeyUrl = Profile.KeyUrl;
		Row.DisplayName = FText::FromString(Profile.DisplayName.IsEmpty() ? Profile.Id : Profile.DisplayName);
		Rows.Add(MoveTemp(Row));
	}

	TSharedRef<SVerticalBox> KeyRows = SNew(SVerticalBox);
	for (int32 i = 0; i < Rows.Num(); ++i)
	{
		KeyRows->AddSlot()
			.AutoHeight()
			.Padding(0.f, 5.f)
			[
				BuildKeyRow(i)
			];
	}

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
		.Padding(FMargin(14.f))
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 0.f, 0.f, 2.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("API Keys")))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 0.f, 0.f, 6.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT(
					"Each model runs on its provider's key. Keys are stored encrypted on this machine and "
					"never sent to Kismet. A provider's environment variable overrides its stored key.")))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 0.f, 0.f, 4.f)
			[
				SNew(SSeparator)
			]

			+ SVerticalBox::Slot()
			.FillHeight(1.f)
			[
				SAssignNew(ScrollBox, SScrollBox)
				+ SScrollBox::Slot()
				.Padding(0.f, 0.f, 6.f, 0.f)
				[
					KeyRows
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 10.f, 0.f, 0.f)
			.HAlign(HAlign_Right)
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("Close")))
				.OnClicked_Lambda([this]() -> FReply
				{
					CloseHostingWindow();
					return FReply::Handled();
				})
			]
		]
	];
}

TSharedRef<SWidget> SKismetAISettingsDialog::BuildKeyRow(int32 RowIndex)
{
	FKeyRow& R = Rows[RowIndex];

	const FString Hint = R.EnvVarName.IsEmpty()
		? FString::Printf(TEXT("Paste your %s key"), *R.DisplayName.ToString())
		: FString::Printf(TEXT("Paste your %s key (or set %s)"), *R.DisplayName.ToString(), *R.EnvVarName);

	TSharedRef<SHorizontalBox> Header = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(R.DisplayName)
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
		];

	if (!R.KeyUrl.IsEmpty())
	{
		const FString Url = R.KeyUrl;
		Header->AddSlot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(10.f, 0.f, 0.f, 0.f)
			[
				SNew(SHyperlink)
				.Text(FText::FromString(TEXT("Get a key")))
				.ToolTipText(FText::FromString(Url))
				.OnNavigate_Lambda([Url]() { FPlatformProcess::LaunchURL(*Url, nullptr, nullptr); })
			];
	}

	Header->AddSlot()
		.FillWidth(1.f)
		.VAlign(VAlign_Center)
		.HAlign(HAlign_Right)
		[
			SAssignNew(R.Status, STextBlock)
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
		];

	TSharedRef<SWidget> Widget = SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			Header
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 3.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			.VAlign(VAlign_Center)
			[
				SAssignNew(R.Input, SEditableTextBox)
				.IsPassword(true)
				.HintText(FText::FromString(Hint))
				.OnTextCommitted_Lambda([this, RowIndex](const FText&, ETextCommit::Type CommitType)
				{
					if (CommitType == ETextCommit::OnEnter)
					{
						OnSaveClicked(RowIndex);
					}
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(6.f, 0.f, 0.f, 0.f)
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("Save")))
				.OnClicked_Lambda([this, RowIndex]() { return OnSaveClicked(RowIndex); })
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(6.f, 0.f, 0.f, 0.f)
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("Clear")))
				.OnClicked_Lambda([this, RowIndex]() { return OnClearClicked(RowIndex); })
			]
		];

	R.Widget = Widget;
	RefreshStatus(RowIndex);
	return Widget;
}

void SKismetAISettingsDialog::RefreshStatus(int32 RowIndex)
{
	FKeyRow& R = Rows[RowIndex];
	if (!R.Status.IsValid()) return;

	// The environment variable wins over the stored key (it is what the agent
	// sends), so say so rather than reporting "configured" for a key that is
	// not the one in use.
	FString Text;
	FSlateColor Color = FSlateColor::UseSubduedForeground();
	if (!EnvValue(R.EnvVarName).IsEmpty())
	{
		Text = FString::Printf(TEXT("from %s"), *R.EnvVarName);
		Color = FSlateColor(FLinearColor(0.4f, 0.8f, 0.4f));
	}
	else if (KismetAIKeyStore::HasProviderKey(R.Slot))
	{
		Text = TEXT("configured");
		Color = FSlateColor(FLinearColor(0.4f, 0.8f, 0.4f));
	}
	else
	{
		Text = TEXT("not set");
	}
	R.Status->SetText(FText::FromString(Text));
	R.Status->SetColorAndOpacity(Color);
}

FReply SKismetAISettingsDialog::OnSaveClicked(int32 RowIndex)
{
	FKeyRow& R = Rows[RowIndex];
	if (!R.Input.IsValid()) return FReply::Handled();

	FString Key = R.Input->GetText().ToString();
	Key.TrimStartAndEndInline();
	if (Key.IsEmpty())
	{
		return FReply::Handled();
	}

	if (KismetAIKeyStore::SaveProviderKey(R.Slot, Key))
	{
		// An environment variable keeps precedence, as it does at startup.
		const FString Env = EnvValue(R.EnvVarName);
		FKismetAIAgentConfig Config = FKismetAIModule::Get().GetAgentConfig();
		KismetAIProviders::SetApiKey(Config, R.Slot, Env.IsEmpty() ? Key : Env);
		FKismetAIModule::Get().SetAgentConfig(Config);

		// Never keep the plaintext in the widget after a successful save.
		R.Input->SetText(FText::GetEmpty());
		RefreshStatus(RowIndex);
	}
	else if (R.Status.IsValid())
	{
		R.Status->SetText(FText::FromString(TEXT("save failed")));
		R.Status->SetColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.4f, 0.4f)));
	}
	return FReply::Handled();
}

FReply SKismetAISettingsDialog::OnClearClicked(int32 RowIndex)
{
	FKeyRow& R = Rows[RowIndex];
	KismetAIKeyStore::ClearProviderKey(R.Slot);

	// Revert the running config to the environment variable (empty if unset).
	FKismetAIAgentConfig Config = FKismetAIModule::Get().GetAgentConfig();
	KismetAIProviders::SetApiKey(Config, R.Slot, EnvValue(R.EnvVarName));
	FKismetAIModule::Get().SetAgentConfig(Config);

	if (R.Input.IsValid())
	{
		R.Input->SetText(FText::GetEmpty());
	}
	RefreshStatus(RowIndex);
	return FReply::Handled();
}

void SKismetAISettingsDialog::FocusProvider(const FString& ProviderId)
{
	for (FKeyRow& R : Rows)
	{
		if (R.ProviderId != ProviderId || !R.Input.IsValid())
		{
			continue;
		}
		if (ScrollBox.IsValid() && R.Widget.IsValid())
		{
			ScrollBox->ScrollDescendantIntoView(R.Widget, /*InAnimateScroll=*/false, EDescendantScrollDestination::Center);
		}
		FSlateApplication::Get().SetKeyboardFocus(R.Input, EFocusCause::SetDirectly);
		return;
	}
}

void SKismetAISettingsDialog::CloseHostingWindow()
{
	if (TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(AsShared()))
	{
		Window->RequestDestroyWindow();
	}
}

void SKismetAISettingsDialog::OpenAsWindow(const FString& FocusProviderId)
{
	// One window: a second request (the model menu's "Add key" after "API
	// keys", say) brings it forward and moves focus to the asked-for row.
	TSharedPtr<SWindow> Existing = OpenWindow.Pin();
	TSharedPtr<SKismetAISettingsDialog> ExistingDialog = OpenDialog.Pin();
	if (Existing.IsValid() && ExistingDialog.IsValid())
	{
		Existing->BringToFront();
		if (!FocusProviderId.IsEmpty())
		{
			ExistingDialog->FocusProvider(FocusProviderId);
		}
		return;
	}

	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(FText::FromString(TEXT("Kismet Settings")))
		.ClientSize(FVector2D(520.f, 600.f))
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		.SizingRule(ESizingRule::UserSized);

	TSharedRef<SKismetAISettingsDialog> Dialog = SNew(SKismetAISettingsDialog);
	Window->SetContent(Dialog);
	OpenWindow = Window;
	OpenDialog = Dialog;

	TSharedPtr<SWindow> ParentWindow;
	if (FSlateApplication::IsInitialized())
	{
		ParentWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
	}

	if (ParentWindow.IsValid())
	{
		FSlateApplication::Get().AddWindowAsNativeChild(Window, ParentWindow.ToSharedRef());
	}
	else
	{
		FSlateApplication::Get().AddWindow(Window);
	}

	if (!FocusProviderId.IsEmpty())
	{
		// Layout has not run yet, so scrolling now would use zero geometry.
		// Focus once on the next tick, after the first paint.
		TWeakPtr<SKismetAISettingsDialog> WeakDialog = Dialog;
		const FString Id = FocusProviderId;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakDialog, Id](float) -> bool
		{
			if (TSharedPtr<SKismetAISettingsDialog> D = WeakDialog.Pin())
			{
				D->FocusProvider(Id);
			}
			return false;
		}));
	}
}
