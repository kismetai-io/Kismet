// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "SKismetAIPanel.h"
#include "KismetAIModule.h"
#include "KismetAIBridge.h"
#include "KismetAIAuth.h"
#include "KismetAIEmbeddedConfig.h"
#include "KismetAITelemetry.h"
#include "KismetAILLMProvider.h"
#include "KismetAIProviderProfile.h"
#include "KismetAILog.h"
#include "SKismetAIBugReportDialog.h"
#include "SKismetAISettingsDialog.h"

#include "SWebBrowser.h"
#include "WebBrowserModule.h"
#include "IWebBrowserWindow.h"

#include "Widgets/Text/STextBlock.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SSpacer.h"
#include "Styling/AppStyle.h"

#include "UObject/StrongObjectPtr.h"
#include "Async/Async.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"

#include "Subsystems/AssetEditorSubsystem.h"
#include "Engine/Blueprint.h"
#include "Editor.h"

DEFINE_LOG_CATEGORY_STATIC(LogKismetAIPanel, Log, All);

// ─────────────────────────────────────────────────────────────────────────────
// Construction / Destruction
// ─────────────────────────────────────────────────────────────────────────────

void SKismetAIPanel::Construct(const FArguments& InArgs)
{

	// Check CEF availability. Note: IWebBrowserModule::IsAvailable() only returns true
	// if the module is already loaded — it does not trigger a load. On some platforms
	// (notably Windows) nothing loads WebBrowser before our panel is constructed, so we
	// must explicitly load it here via FModuleManager::LoadModulePtr.
	IWebBrowserModule* WebModule = FModuleManager::LoadModulePtr<IWebBrowserModule>("WebBrowser");
	if (!WebModule || !WebModule->IsWebModuleAvailable())
	{
		KISMETAI_LOG(LogKismetAIPanel, Error,
			TEXT("WebBrowser unavailable (module=%p, CEF=%s). Chat UI cannot be shown."),
			WebModule, (WebModule && WebModule->IsWebModuleAvailable()) ? TEXT("loaded") : TEXT("not loaded"));

		ChildSlot
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("Kismet: WebBrowser module not available. CEF is required for the chat UI.")))
			.AutoWrapText(true)
		];
		return;
	}

	// Register with module for broadcast events
	FKismetAIModule::Get().RegisterPanel(this);

	// Create bridge UObject
	Bridge = TStrongObjectPtr<UKismetAIBridge>(NewObject<UKismetAIBridge>());
	Bridge->OwnerPanel = SharedThis(this);

	// Loading overlay — dark screen with title, shown while WebBrowser loads
	SAssignNew(LoadingOverlay, SBorder)
	.BorderImage(FCoreStyle::Get().GetBrush("GenericWhiteBox"))
	.BorderBackgroundColor(FColor(36, 36, 36, 255)) // matches the page body: Unreal panel grey #242424
	.Padding(0)
	.HAlign(HAlign_Center)
	.VAlign(VAlign_Center)
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Center)
		.Padding(0, 0, 0, 8)
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("Kismet")))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 18))
			.ColorAndOpacity(FSlateColor(FLinearColor(0.784f, 0.784f, 0.784f))) // UE5: #C8C8C8
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Center)
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("Loading...")))
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
			.ColorAndOpacity(FSlateColor(FLinearColor(0.502f, 0.502f, 0.502f))) // UE5: #808080
		]
	];

	ChildSlot
	[
		SNew(SVerticalBox)

		// Fallback toolbar. Docs, Keystore and Report Bug normally live in the
		// page's own "more" menu; this native strip is shown only until the page
		// reports in (UKismetAIBridge::NotifyUiReady), so those actions, Report Bug
		// above all, stay reachable when the web UI fails to load or boot.
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(6.f, 3.f))
			.Visibility_Lambda([this]() { return bWebUiReady ? EVisibility::Collapsed : EVisibility::Visible; })
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				[
					SNew(SSpacer)
				]

				// Docs — opens the documentation site in the user's browser.
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0, 0, 8, 0)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ToolTipText(FText::FromString(TEXT("Open the Kismet documentation")))
					.OnClicked_Lambda([]() -> FReply
					{
						const FString DocsUrl = KismetAIAuth::GetBackendUrl() + TEXT("/docs");
						FPlatformProcess::LaunchURL(*DocsUrl, nullptr, nullptr);
						return FReply::Handled();
					})
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0, 0, 4, 0)
						[
							SNew(SImage)
							.Image(FAppStyle::Get().GetBrush("Icons.Help"))
							.ColorAndOpacity(FSlateColor::UseForeground())
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(FText::FromString(TEXT("Docs")))
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						]
					]
				]

				// Keystore (API keys, …) — native dialog, opened from the toolbar.
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0, 0, 8, 0)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ToolTipText(FText::FromString(TEXT("Keystore — manage API keys")))
					.OnClicked_Lambda([]() -> FReply
					{
						SKismetAISettingsDialog::OpenAsWindow();
						return FReply::Handled();
					})
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0, 0, 4, 0)
						[
							SNew(SImage)
							.Image(FAppStyle::Get().GetBrush("Icons.Settings"))
							.ColorAndOpacity(FSlateColor::UseForeground())
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(FText::FromString(TEXT("Keystore")))
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						]
					]
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ToolTipText(FText::FromString(TEXT("Report a bug to the KismetAI team")))
					.OnClicked_Lambda([this]() -> FReply
					{
						SKismetAIBugReportDialog::OpenAsWindow(
							FKismetAIModule::Get().GetCurrentSessionId());
						return FReply::Handled();
					})
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0, 0, 4, 0)
						[
							SNew(SImage)
							.Image(FAppStyle::Get().GetBrush("Icons.Warning"))
							.ColorAndOpacity(FSlateColor::UseForeground())
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(FText::FromString(TEXT("Report Bug")))
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						]
					]
				]
			]
		]

		// Chat area (WebBrowser + loading overlay)
		+ SVerticalBox::Slot()
		.FillHeight(1.f)
		[
			SNew(SOverlay)

			// Layer 0: WebBrowser (behind)
			+ SOverlay::Slot()
			[
				SAssignNew(WebBrowser, SWebBrowser)
			.ShowControls(false)
			.ShowAddressBar(false)
			.ShowErrorMessage(true)
			.SupportsTransparency(false)
			.BackgroundColor(FColor(36, 36, 36, 255)) // matches the page body: Unreal panel grey #242424
			.BrowserFrameRate(30)
			.OnLoadCompleted_Lambda([this]()
			{
				// WKWebView fires this on a background thread on macOS.
				// Marshal all Slate/UObject work to the game thread.
				TWeakPtr<SKismetAIPanel> WeakSelf = SharedThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakSelf]()
				{
					TSharedPtr<SKismetAIPanel> Self = WeakSelf.Pin();
					if (!Self.IsValid()) return;

					Self->bWebBrowserReady = true;

					// Bind bridge so JS can call window.ue.kismetai.*
					Self->WebBrowser->BindUObject(TEXT("kismetai"), Self->Bridge.Get(), /*bIsPermanent=*/true);

					// Flush any JS calls that were queued before the page loaded
					for (const FString& JS : Self->PendingJSCalls)
					{
						Self->WebBrowser->ExecuteJavascript(JS);
					}
					Self->PendingJSCalls.Empty();

					// Read the session email before pushing so we can compare after validation
					FKismetAIAuthSession PreValidateSession;
					KismetAIAuth::LoadSession(PreValidateSession);
					FString PreValidateEmail = PreValidateSession.Email;

					Self->PushInitialState();

					// Async validate session on open; update UI if server response changes state.
					// PushInitialState is called BEFORE ValidateSession — UI shows sign-in
					//      overlay immediately, without waiting for HTTP.
					// Transient failures never sign out the user.
					KismetAIAuth::ValidateSession([WeakSelf, PreValidateEmail](KismetAIAuth::EValidateResult Result, const FKismetAIAuthSession& RefreshedSession)
					{
						TSharedPtr<SKismetAIPanel> PinnedSelf = WeakSelf.Pin();
						if (!PinnedSelf.IsValid()) return;

						if (Result == KismetAIAuth::EValidateResult::Invalid)
						{
							// ValidateSession already cleared the local session file.
							// Re-push so UI reflects signed-out state.
							PinnedSelf->PushInitialState();
						}
						else if (Result == KismetAIAuth::EValidateResult::Valid)
						{
							// Re-push if the email changed (e.g. server returned an
							// updated profile) OR the server just revealed that the
							// user hasn't acknowledged the telemetry notice — the
							// earlier PushInitialState ran with consent still
							// unknown, so the gate needs a refresh to appear.
							const bool bNeedsConsentGate =
								KismetAITelemetry::IsConsentKnown() && !KismetAITelemetry::HasConsented();
							if (RefreshedSession.Email != PreValidateEmail || bNeedsConsentGate)
							{
								PinnedSelf->PushInitialState();
							}
							// Otherwise do nothing — avoids unnecessary flicker.
						}
						// EValidateResult::Transient → do nothing.
					});

					// Hide loading overlay
					if (Self->LoadingOverlay.IsValid())
					{
						Self->LoadingOverlay->SetVisibility(EVisibility::Collapsed);
					}
				});
			})
			.OnConsoleMessage(FOnConsoleMessageDelegate::CreateLambda(
				[](const FString& Message, const FString& Source, int32 Line, EWebBrowserConsoleLogSeverity Severity)
				{
					switch (Severity)
					{
					case EWebBrowserConsoleLogSeverity::Warning:
						KISMETAI_DEV_LOG(LogKismetAIPanel, Warning, TEXT("[WebUI] %s (%s:%d)"), *Message, *Source, Line);
						break;
					case EWebBrowserConsoleLogSeverity::Error:
						KISMETAI_DEV_LOG(LogKismetAIPanel, Error, TEXT("[WebUI] %s (%s:%d)"), *Message, *Source, Line);
						break;
					default:
						KISMETAI_DEV_LOG(LogKismetAIPanel, Log, TEXT("[WebUI] %s"), *Message);
						break;
					}
				}))
		]

			// Layer 1: Loading overlay (on top, hides when page loads)
			+ SOverlay::Slot()
			[
				LoadingOverlay.ToSharedRef()
			]
		]
	];

	LoadHTML();
}

SKismetAIPanel::~SKismetAIPanel()
{
	FKismetAIModule::Get().UnregisterPanel(this);
}

// ─────────────────────────────────────────────────────────────────────────────
// HTML loading
// ─────────────────────────────────────────────────────────────────────────────

void SKismetAIPanel::LoadHTML()
{
	// A (re)load starts with the fallback toolbar showing until the page reports in.
	bWebUiReady = false;

	// Resolve Config/ directory relative to the plugin module
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("KismetAI"));
	FString ConfigDir;
	if (Plugin.IsValid())
	{
		ConfigDir = Plugin->GetBaseDir() / TEXT("Config");
	}
	else
	{
		// Fallback: relative to module binary
		FString ModulePath = FModuleManager::Get().GetModuleFilename(TEXT("KismetAI"));
		ConfigDir = FPaths::GetPath(FPaths::GetPath(FPaths::GetPath(ModulePath))) / TEXT("Config");
	}

	FString HtmlPath = ConfigDir / TEXT("ui.html");
	FString HtmlContent;

	if (KismetAIEmbeddedConfig::LoadConfigFile(TEXT("ui.html"), HtmlPath, HtmlContent))
	{
		const TCHAR* Source = KismetAIEmbeddedConfig::IsEmbedded() ? TEXT("embedded") : *HtmlPath;
		KISMETAI_DEV_LOG(LogKismetAIPanel, Log, TEXT("Loaded ui.html from %s (%d chars)"), Source, HtmlContent.Len());

		const FString PluginVersion = Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : FString();
		HtmlContent.ReplaceInline(TEXT("{{KISMETAI_VERSION}}"), *PluginVersion, ESearchCase::CaseSensitive);

		// Env-aware backend host (kismetai.io vs dev.kismetai.io). Used by the
		// login footer to build documentation / bug-report URLs that match the
		// configured deployment stage.
		const FString BackendUrl = KismetAIAuth::GetBackendUrl();
		HtmlContent.ReplaceInline(TEXT("{{KISMETAI_BACKEND_URL}}"), *BackendUrl, ESearchCase::CaseSensitive);

		// On macOS, WKWebView's LoadString (loadHTMLString) can fail silently with large content.
		// Instead, write to a temp file and load via file:// URL for reliable rendering.
		// ForceUTF8WithoutBOM matches the page's <meta charset="utf-8">. UE's default
		// SaveStringToFile encoding is UTF-16 LE with BOM, which Chromium (CEF on Win64)
		// accepts for HTML parsing but trips some inline-script behaviour (markdown library
		// silently not initialising), leaving messages rendered as plain text.
		FString TempDir = FPaths::ProjectSavedDir() / TEXT("KismetAI");
		IFileManager::Get().MakeDirectory(*TempDir, true);
		FString TempPath = TempDir / TEXT("ui.html");
		if (FFileHelper::SaveStringToFile(HtmlContent, *TempPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			FString FileUrl = TEXT("file://") + FPaths::ConvertRelativePathToFull(TempPath);
			KISMETAI_DEV_LOG(LogKismetAIPanel, Log, TEXT("Loading WebUI from %s"), *FileUrl);
			WebBrowser->LoadURL(FileUrl);
		}
		else
		{
			KISMETAI_LOG(LogKismetAIPanel, Error, TEXT("Failed to write temp ui.html to %s"), *TempPath);
			WebBrowser->LoadString(HtmlContent, TEXT("about:kismetai"));
		}
	}
	else
	{
		KISMETAI_LOG(LogKismetAIPanel, Error, TEXT("Failed to load ui.html from %s"), *HtmlPath);
		FString ErrorHtml = FString::Printf(
			TEXT("<html><body style='background:#111;color:#f88;padding:20px;font-family:system-ui'>"
				 "<h2>KismetAI: Failed to load ui.html</h2>"
				 "<p>Expected at: %s</p>"
				 "</body></html>"),
			*HtmlPath);
		WebBrowser->LoadString(ErrorHtml, TEXT("about:kismetai-error"));
	}
}

void SKismetAIPanel::PushInitialState()
{
	FKismetAIAuthSession Session;
	KismetAIAuth::LoadSession(Session);
	bool bHasSession = !Session.Token.IsEmpty();
	FString SessionEmail = Session.Email;

	const FKismetAIAgentConfig& Config = FKismetAIModule::Get().GetAgentConfig();
	bool bHasApiKey = KismetAIProviders::HasUsableKey(Config, KismetAIProviders::ResolveProfile(Config));
	FString ModelDisplay = FKismetAIAgent::GetModelDisplayName(Config.Model);
	FString ThinkingMode = FKismetAIAgent::ThinkingModeToString(Config.ThinkingMode);
	FString ThinkingDisplay = FKismetAIAgent::GetThinkingModeDisplayName(Config.ThinkingMode);
	FString EffortLevel = FKismetAIAgent::EffortLevelToString(Config.EffortLevel);
	// Telemetry-consent gate (shown after sign-in). Tri-state: until the
	// server confirms (first /me), "known" is false and the JS keeps the gate
	// hidden — we never block on an unconfirmed value.
	const bool bConsentKnown = KismetAITelemetry::IsConsentKnown();
	const bool bConsented    = KismetAITelemetry::HasConsented();
	ExecuteJavascriptSafe(FString::Printf(
		TEXT("window.onInitialState && window.onInitialState('%s', %s, %s, '%s', '%s', '%s', %d, '%s', %s, '%s', %s, %s)"),
		*ActiveBlueprintPath.Replace(TEXT("'"), TEXT("\\'")),
		FKismetAIModule::Get().IsHttpServerRunning() ? TEXT("true") : TEXT("false"),
		bHasApiKey ? TEXT("true") : TEXT("false"),
		*ModelDisplay.Replace(TEXT("'"), TEXT("\\'")),
		*ThinkingMode.Replace(TEXT("'"), TEXT("\\'")),
		*ThinkingDisplay.Replace(TEXT("'"), TEXT("\\'")),
		Config.ThinkingBudgetTokens,
		*EffortLevel.Replace(TEXT("'"), TEXT("\\'")),
		bHasSession ? TEXT("true") : TEXT("false"),
		*SessionEmail.Replace(TEXT("'"), TEXT("\\'")),
		bConsentKnown ? TEXT("true") : TEXT("false"),
		bConsented ? TEXT("true") : TEXT("false")));
}

// ─────────────────────────────────────────────────────────────────────────────
// JavaScript communication
// ─────────────────────────────────────────────────────────────────────────────

void SKismetAIPanel::ExecuteJavascriptSafe(const FString& Script)
{
	if (bWebBrowserReady && WebBrowser.IsValid())
	{
		WebBrowser->ExecuteJavascript(Script);
	}
	else
	{
		PendingJSCalls.Add(Script);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Agent lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void SKismetAIPanel::LaunchAgentWithRefs(const FString& UserMessage, const FString& RefsJson, const FString& ImagesJson)
{
	FKismetAIModule& Module = FKismetAIModule::Get();
	Module.LaunchAgent(UserMessage, ImagesJson, Module.GetCurrentSessionId(), RefsJson);
}

void SKismetAIPanel::LaunchAgentFromWeb(const FString& UserMessage, bool bIncludeBlueprint, const FString& ImagesJson)
{
	// Shim: build a one-element focused-BP refs array when a blueprint is active,
	// otherwise pass an empty array so LaunchAgentWithRefs/LaunchAgent receive valid JSON.
	FString RefsJson = TEXT("[]");
	if (bIncludeBlueprint && !ActiveBlueprintPath.IsEmpty())
	{
		// Derive short asset name from the path (last segment before the dot)
		FString ShortName = ActiveBlueprintPath;
		int32 DotIdx;
		if (ShortName.FindLastChar(TEXT('.'), DotIdx))
		{
			ShortName = ShortName.Mid(DotIdx + 1);
		}
		else
		{
			int32 SlashIdx;
			if (ShortName.FindLastChar(TEXT('/'), SlashIdx))
			{
				ShortName = ShortName.Mid(SlashIdx + 1);
			}
		}

		// Escape the path and name for inline JSON (UE asset paths are ASCII-safe,
		// but escape quotes defensively).
		auto EscJson = [](const FString& S) -> FString
		{
			return S.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
		};

		RefsJson = FString::Printf(
			TEXT("[{\"path\":\"%s\",\"name\":\"%s\",\"class\":\"Blueprint\",\"focused\":true}]"),
			*EscJson(ActiveBlueprintPath),
			*EscJson(ShortName));
	}

	LaunchAgentWithRefs(UserMessage, RefsJson, ImagesJson);
}

void SKismetAIPanel::AbortAgent()
{
	FKismetAIModule::Get().AbortAgent();
}

void SKismetAIPanel::NotifyAgentStopped(bool bWillContinue)
{
	bPendingAgentStopped = true;
	bPendingStopWillContinue = bWillContinue;
}

// ─────────────────────────────────────────────────────────────────────────────
// Blueprint tracking
// ─────────────────────────────────────────────────────────────────────────────

void SKismetAIPanel::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	// Deliver pending agent-stopped signal.
	// WKWebView drops ExecuteJavascript calls when the panel is hidden (e.g., user
	// switched to a Blueprint tab). We retry on each Tick until the panel is visible.
	if (bPendingAgentStopped && bWebBrowserReady)
	{
		const FString Script = FString::Printf(
			TEXT("window.onAgentStopped && window.onAgentStopped(%s)"),
			bPendingStopWillContinue ? TEXT("true") : TEXT("false"));
		ExecuteJavascriptSafe(Script);
		bPendingAgentStopped = false;
		bPendingStopWillContinue = false;
	}

	// Periodic Kismet-session re-validation. Fires every 5 minutes while the
	// panel is alive so a dashboard-side revoke takes effect within ~5 min,
	// not just at next panel reopen. Skipped when no local session exists
	// (ValidateSession itself returns Invalid synchronously without HTTP).
	TimeSinceLastAuthCheck += InDeltaTime;
	if (TimeSinceLastAuthCheck >= 300.f)
	{
		TimeSinceLastAuthCheck = 0.f;
		if (KismetAIAuth::HasSession())
		{
			TWeakPtr<SKismetAIPanel> WeakSelf = SharedThis(this);
			KismetAIAuth::ValidateSession([WeakSelf](KismetAIAuth::EValidateResult Result, const FKismetAIAuthSession&)
			{
				if (Result != KismetAIAuth::EValidateResult::Invalid) return;
				TSharedPtr<SKismetAIPanel> Pinned = WeakSelf.Pin();
				if (Pinned.IsValid())
				{
					// Local credentials already cleared by ValidateSession.
					// Re-push UI so the sign-in overlay appears.
					Pinned->PushInitialState();
				}
			});
		}
	}

	TimeSinceLastBPCheck += InDeltaTime;
	if (TimeSinceLastBPCheck < 0.5f) return;
	TimeSinceLastBPCheck = 0.f;

	FString PrevPath = ActiveBlueprintPath;
	DetectFocusedBlueprint();

	if (ActiveBlueprintPath != PrevPath)
	{
		ExecuteJavascriptSafe(FString::Printf(
			TEXT("window.onBlueprintChanged && window.onBlueprintChanged('%s')"),
			*ActiveBlueprintPath.Replace(TEXT("'"), TEXT("\\'"))));
	}
}

void SKismetAIPanel::DetectFocusedBlueprint()
{
	if (!GEditor) return;

	UAssetEditorSubsystem* AssetEditorSub = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditorSub) return;

	TArray<UObject*> EditedAssets = AssetEditorSub->GetAllEditedAssets();

	TArray<FString> CurrentOpenPaths;
	for (UObject* Asset : EditedAssets)
	{
		if (UBlueprint* BP = Cast<UBlueprint>(Asset))
		{
			CurrentOpenPaths.Add(BP->GetOutermost()->GetPathName());
		}
	}

	if (CurrentOpenPaths.IsEmpty())
	{
		ActiveBlueprintPath.Empty();
		PreviousOpenBlueprintPaths.Empty();
		return;
	}

	// Detect newly opened Blueprints
	for (const FString& Path : CurrentOpenPaths)
	{
		if (!PreviousOpenBlueprintPaths.Contains(Path))
		{
			ActiveBlueprintPath = Path;
			PreviousOpenBlueprintPaths = CurrentOpenPaths;
			return;
		}
	}

	// If tracked Blueprint was closed, switch to another
	if (!CurrentOpenPaths.Contains(ActiveBlueprintPath))
	{
		ActiveBlueprintPath = CurrentOpenPaths[0];
	}

	PreviousOpenBlueprintPaths = CurrentOpenPaths;
}
