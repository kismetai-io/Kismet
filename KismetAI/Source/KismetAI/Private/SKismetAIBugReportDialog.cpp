// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "SKismetAIBugReportDialog.h"

#include "KismetAIAuth.h"
#include "KismetAILog.h"
#include "KismetAISessionStorage.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformMisc.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"

DEFINE_LOG_CATEGORY_STATIC(LogKismetAIBugReport, Log, All);

namespace
{
	// Mirrors KismetAITelemetry::JoinUrl + ApplyVercelBypass — duplicated here
	// rather than exposed because the bug-report path is intentionally a
	// standalone endpoint (different schema, different rate-limit budget on
	// the backend) and shouldn't piggyback on the telemetry plumbing.
	FString BugReportJoinUrl(const FString& Base, const FString& Path)
	{
		if (Base.EndsWith(TEXT("/")))
		{
			return Base + (Path.StartsWith(TEXT("/")) ? Path.Mid(1) : Path);
		}
		return Base + (Path.StartsWith(TEXT("/")) ? Path : (TEXT("/") + Path));
	}

	void BugReportApplyVercelBypass(const TSharedRef<IHttpRequest>& Req)
	{
		const FString Token = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMET_VERCEL_BYPASS"));
		if (!Token.IsEmpty())
		{
			Req->SetHeader(TEXT("x-vercel-protection-bypass"), Token);
		}
	}

	// Mirrors KismetAITelemetry::GetOSString — kept local so the bug-report path
	// stays self-contained (see note above).
	FString BugReportOSString()
	{
#if PLATFORM_MAC
		return TEXT("mac");
#elif PLATFORM_WINDOWS
		return TEXT("windows");
#elif PLATFORM_LINUX
		return TEXT("linux");
#else
		return TEXT("unknown");
#endif
	}

	FString BugReportPluginVersion()
	{
		if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("KismetAI")))
		{
			return Plugin->GetDescriptor().VersionName;
		}
		return FString();
	}

	FString JsonObjectToString(TSharedRef<FJsonObject> Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	// Serialize a JsonValue array as a JSON array string and return both the
	// string and its length. Used for size measurement during truncation.
	FString JsonArrayToString(const TArray<TSharedPtr<FJsonValue>>& Arr)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Arr, Writer);
		return Out;
	}

	// Parse a single JSONL line into a JsonValue (object). Returns null on
	// parse failure — caller should skip those lines silently.
	TSharedPtr<FJsonValue> ParseEntryToValue(TSharedPtr<FJsonObject> Entry)
	{
		if (!Entry.IsValid())
		{
			return nullptr;
		}
		return MakeShared<FJsonValueObject>(Entry);
	}

	// Build the telemetry array from the session storage and truncate from the
	// front (drop oldest) until the serialized length is within MaxBytes.
	// Appends a "_truncated" marker entry to the END of the array when any
	// drops occur, so the consumer sees one extra entry it can recognise.
	TArray<TSharedPtr<FJsonValue>> BuildTelemetryArray(const FString& SessionId, int32 MaxBytes)
	{
		TArray<TSharedPtr<FJsonObject>> Entries = FKismetAISessionStorage::LoadAllEntries(SessionId);

		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(Entries.Num());
		for (TSharedPtr<FJsonObject>& Entry : Entries)
		{
			TSharedPtr<FJsonValue> V = ParseEntryToValue(Entry);
			if (V.IsValid())
			{
				Values.Add(V);
			}
		}

		// Initial measurement.
		FString Serialized = JsonArrayToString(Values);
		if (Serialized.Len() <= MaxBytes)
		{
			return Values;
		}

		// Drop from the front until we fit (leaving one slot for the
		// _truncated marker). Re-measure each pass — entries vary widely in
		// size (a tool_result with raw T3D dwarfs a token_stats event), so a
		// linear estimate is unreliable.
		int32 DroppedCount = 0;
		while (Values.Num() > 0 && Serialized.Len() > MaxBytes)
		{
			// Drop a chunk at a time to avoid quadratic behaviour on big
			// transcripts. 32 entries is small enough that we won't massively
			// overshoot on small files but big enough to converge quickly on
			// 10k-entry sessions.
			int32 ChunkSize = FMath::Min(32, Values.Num());
			Values.RemoveAt(0, ChunkSize, EAllowShrinking::No);
			DroppedCount += ChunkSize;
			Serialized = JsonArrayToString(Values);
		}

		// Append marker so admins know the array was clipped from the head.
		TSharedPtr<FJsonObject> Marker = MakeShared<FJsonObject>();
		Marker->SetBoolField(TEXT("_truncated"), true);
		Marker->SetNumberField(TEXT("dropped_count"), DroppedCount);
		Values.Add(MakeShared<FJsonValueObject>(Marker));

		return Values;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

void SKismetAIBugReportDialog::Construct(const FArguments& InArgs)
{
	CurrentSessionId = InArgs._CurrentSessionId;
	const bool bHasSession = !CurrentSessionId.IsEmpty();

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
		.Padding(16.f)
		[
			SNew(SVerticalBox)

			// Title row
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 8)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Report a Bug")))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Description")))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
			]

			// Description box (~6 rows tall via min height)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 4)
			[
				SNew(SBox)
				.MinDesiredHeight(120.f)
				.MinDesiredWidth(480.f)
				[
					SAssignNew(DescriptionBox, SMultiLineEditableTextBox)
					.AutoWrapText(true)
					.AlwaysShowScrollbars(false)
					.HintText(FText::FromString(TEXT("What went wrong? Steps to reproduce, what you expected, what happened.")))
					.OnTextChanged(this, &SKismetAIBugReportDialog::OnDescriptionChanged)
				]
			]

			// Char counter
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Right)
			.Padding(0, 0, 0, 12)
			[
				SAssignNew(CharCounterText, STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("0 / %d"), MaxDescriptionLen)))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
			]

			// Telemetry checkbox
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 2)
			[
				SAssignNew(IncludeTelemetryCheckbox, SCheckBox)
				.IsEnabled(bHasSession)
				.IsChecked(ECheckBoxState::Unchecked)
				.OnCheckStateChanged(this, &SKismetAIBugReportDialog::OnIncludeTelemetryChanged)
				[
					SNew(STextBlock)
					.Text(FText::FromString(bHasSession
						? TEXT("Include current session telemetry (will attach prompts, tool calls, and responses)")
						: TEXT("Include current session telemetry (no active session)")))
					.AutoWrapText(true)
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(22, 0, 0, 12)
			[
				SAssignNew(TelemetryPreviewText, STextBlock)
				.Text(FText::GetEmpty())
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
				.Visibility(EVisibility::Collapsed)
			]

			// Status line (success / error / submitting)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 8)
			[
				SAssignNew(StatusText, STextBlock)
				.Text(FText::GetEmpty())
				.AutoWrapText(true)
				.Visibility(EVisibility::Collapsed)
			]

			// Buttons
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Right)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0, 0, 8, 0)
				[
					SAssignNew(CancelButton, SButton)
					.Text(FText::FromString(TEXT("Cancel")))
					.OnClicked(this, &SKismetAIBugReportDialog::OnCancelClicked)
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SAssignNew(SubmitButton, SButton)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.Text(FText::FromString(TEXT("Submit")))
					.OnClicked(this, &SKismetAIBugReportDialog::OnSubmitClicked)
				]
			]
		]
	];
}

// ─────────────────────────────────────────────────────────────────────────────
// Static window factory
// ─────────────────────────────────────────────────────────────────────────────

void SKismetAIBugReportDialog::OpenAsWindow(const FString& CurrentSessionId)
{
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(FText::FromString(TEXT("Report a Bug")))
		.ClientSize(FVector2D(560.f, 380.f))
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		.SizingRule(ESizingRule::UserSized);

	TSharedRef<SKismetAIBugReportDialog> Dialog = SNew(SKismetAIBugReportDialog)
		.CurrentSessionId(CurrentSessionId);

	Window->SetContent(Dialog);

	// Parent to the editor's main window when available so the dialog floats
	// above the editor instead of appearing as a peer at the OS level.
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
}

// ─────────────────────────────────────────────────────────────────────────────
// Handlers
// ─────────────────────────────────────────────────────────────────────────────

FReply SKismetAIBugReportDialog::OnCancelClicked()
{
	if (bSubmitting)
	{
		// Submission in flight — let the user cancel anyway. The HTTP
		// callback uses a weak ref and is a no-op when the widget is gone.
	}
	CloseHostingWindow();
	return FReply::Handled();
}

FReply SKismetAIBugReportDialog::OnSubmitClicked()
{
	if (bSubmitting)
	{
		return FReply::Handled();
	}

	const FString Description = DescriptionBox.IsValid()
		? DescriptionBox->GetText().ToString()
		: FString();

	const FString Trimmed = Description.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		SetStatusMessage(TEXT("Please enter a description."), /*bIsError=*/true);
		return FReply::Handled();
	}

	if (Description.Len() > MaxDescriptionLen)
	{
		SetStatusMessage(
			FString::Printf(TEXT("Description is too long (%d / %d). Please shorten."),
				Description.Len(), MaxDescriptionLen),
			/*bIsError=*/true);
		return FReply::Handled();
	}

	const bool bIncludeTelemetry =
		IncludeTelemetryCheckbox.IsValid() &&
		IncludeTelemetryCheckbox->IsChecked() &&
		!CurrentSessionId.IsEmpty();

	bSubmitting = true;
	SetButtonsEnabled(false);
	SetStatusMessage(TEXT("Submitting..."), /*bIsError=*/false);

	DispatchSubmit(Description, bIncludeTelemetry);
	return FReply::Handled();
}

void SKismetAIBugReportDialog::OnDescriptionChanged(const FText& /*NewText*/)
{
	UpdateCharCounter();
}

void SKismetAIBugReportDialog::OnIncludeTelemetryChanged(ECheckBoxState NewState)
{
	UpdateTelemetryPreview();
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

void SKismetAIBugReportDialog::UpdateCharCounter()
{
	if (!CharCounterText.IsValid() || !DescriptionBox.IsValid()) return;
	const int32 Len = DescriptionBox->GetText().ToString().Len();
	CharCounterText->SetText(FText::FromString(
		FString::Printf(TEXT("%d / %d"), Len, MaxDescriptionLen)));

	const bool bOver = Len > MaxDescriptionLen;
	CharCounterText->SetColorAndOpacity(FSlateColor(bOver
		? FLinearColor(0.95f, 0.4f, 0.4f)
		: FLinearColor(0.6f, 0.6f, 0.6f)));
}

void SKismetAIBugReportDialog::EnsureTelemetryCached()
{
	if (bTelemetryCached) return;
	bTelemetryCached = true;

	if (CurrentSessionId.IsEmpty())
	{
		CachedTelemetryEntryCount = 0;
		CachedTelemetryBytes = 0;
		return;
	}

	// Loading the full session is fine — sessions are JSONL and at most a few
	// MB even for very long runs. Loader is synchronous game-thread code.
	TArray<TSharedPtr<FJsonObject>> Entries =
		FKismetAISessionStorage::LoadAllEntries(CurrentSessionId);
	CachedTelemetryEntryCount = Entries.Num();

	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Reserve(Entries.Num());
	for (TSharedPtr<FJsonObject>& E : Entries)
	{
		if (E.IsValid()) Values.Add(MakeShared<FJsonValueObject>(E));
	}
	CachedTelemetryBytes = JsonArrayToString(Values).Len();
}

void SKismetAIBugReportDialog::UpdateTelemetryPreview()
{
	if (!TelemetryPreviewText.IsValid() || !IncludeTelemetryCheckbox.IsValid()) return;

	const bool bChecked = IncludeTelemetryCheckbox->IsChecked();
	if (!bChecked || CurrentSessionId.IsEmpty())
	{
		TelemetryPreviewText->SetVisibility(EVisibility::Collapsed);
		return;
	}

	EnsureTelemetryCached();

	const float Kb = static_cast<float>(CachedTelemetryBytes) / 1024.f;
	const bool bWillTruncate = CachedTelemetryBytes > MaxTelemetryBytes;
	const FString Suffix = bWillTruncate
		? FString::Printf(TEXT(" (will be truncated to ~%d KB)"), MaxTelemetryBytes / 1024)
		: FString();

	TelemetryPreviewText->SetText(FText::FromString(
		FString::Printf(TEXT("%d entries, ~%.1f KB%s"),
			CachedTelemetryEntryCount, Kb, *Suffix)));
	TelemetryPreviewText->SetVisibility(EVisibility::Visible);
}

void SKismetAIBugReportDialog::SetStatusMessage(const FString& Message, bool bIsError)
{
	if (!StatusText.IsValid()) return;
	StatusText->SetText(FText::FromString(Message));
	StatusText->SetColorAndOpacity(FSlateColor(bIsError
		? FLinearColor(0.95f, 0.4f, 0.4f)
		: FLinearColor(0.6f, 0.85f, 0.6f)));
	StatusText->SetVisibility(Message.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible);
}

void SKismetAIBugReportDialog::SetButtonsEnabled(bool bEnabled)
{
	if (SubmitButton.IsValid()) SubmitButton->SetEnabled(bEnabled);
	// Cancel stays enabled even mid-submit so the user is never trapped.
	if (CancelButton.IsValid()) CancelButton->SetEnabled(true);
	if (DescriptionBox.IsValid()) DescriptionBox->SetEnabled(bEnabled);
	if (IncludeTelemetryCheckbox.IsValid())
	{
		IncludeTelemetryCheckbox->SetEnabled(bEnabled && !CurrentSessionId.IsEmpty());
	}
}

void SKismetAIBugReportDialog::CloseHostingWindow()
{
	TSharedPtr<SWindow> Host = FSlateApplication::Get().FindWidgetWindow(SharedThis(this));
	if (Host.IsValid())
	{
		Host->RequestDestroyWindow();
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Dispatch
// ─────────────────────────────────────────────────────────────────────────────

void SKismetAIBugReportDialog::DispatchSubmit(const FString& Description, bool bIncludeTelemetry)
{
	// Build the body on the game thread (touches session storage + auth).
	FKismetAIAuthSession Session;
	const bool bHasSession = KismetAIAuth::LoadSession(Session);
	if (!bHasSession)
	{
		bSubmitting = false;
		SetButtonsEnabled(true);
		SetStatusMessage(TEXT("You must be signed in to submit a bug report."), /*bIsError=*/true);
		return;
	}

	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("description"), Description);
	if (!CurrentSessionId.IsEmpty())
	{
		Data->SetStringField(TEXT("session_id"), CurrentSessionId);
	}

	if (bIncludeTelemetry && !CurrentSessionId.IsEmpty())
	{
		TArray<TSharedPtr<FJsonValue>> Values =
			BuildTelemetryArray(CurrentSessionId, MaxTelemetryBytes);
		Data->SetArrayField(TEXT("telemetry"), Values);
	}

	// Wrap in the telemetry envelope the backend's resolvePluginCaller expects:
	// a top-level machine_id (required — absence yields "machine_id_required")
	// plus os/plugin_version/ue_version, with the payload under "data". We must
	// send all three metadata fields because the server upserts the machines row
	// with onConflict=machine_id, so omitting them would clobber the values
	// telemetry populated with null.
	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("machine_id"), KismetAIAuth::GetMachineId());
	Body->SetStringField(TEXT("os"), BugReportOSString());
	Body->SetStringField(TEXT("plugin_version"), BugReportPluginVersion());
	Body->SetStringField(TEXT("ue_version"), FEngineVersion::Current().ToString());
	Body->SetObjectField(TEXT("data"), Data);

	const FString Url = BugReportJoinUrl(KismetAIAuth::GetBackendUrl(), TEXT("/api/v1/bugs/report"));

	FString BodyStr = JsonObjectToString(Body);

	TSharedRef<IHttpRequest> Req = FHttpModule::Get().CreateRequest();
	Req->SetVerb(TEXT("POST"));
	Req->SetURL(Url);
	Req->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Session.Token));
	Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Req->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Req->SetTimeout(30.0f);
	Req->SetContentAsString(BodyStr);
	BugReportApplyVercelBypass(Req);

	// Weak ref so a closed dialog doesn't keep itself alive via the HTTP
	// callback. The lambda may fire on a worker thread (UE HTTP impl varies
	// by platform) — bounce all UI work to the game thread.
	TWeakPtr<SKismetAIBugReportDialog> WeakSelf =
		StaticCastSharedRef<SKismetAIBugReportDialog>(AsShared());

	Req->OnProcessRequestComplete().BindLambda(
		[WeakSelf](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
		{
			AsyncTask(ENamedThreads::GameThread, [WeakSelf, Resp, bOk]()
			{
				TSharedPtr<SKismetAIBugReportDialog> Self = WeakSelf.Pin();
				if (!Self.IsValid()) return;

				Self->bSubmitting = false;
				Self->SetButtonsEnabled(true);

				if (!bOk || !Resp.IsValid())
				{
					Self->SetStatusMessage(TEXT("Failed: network error. Please try again."), /*bIsError=*/true);
					return;
				}

				const int32 Code = Resp->GetResponseCode();
				if (Code >= 200 && Code < 300)
				{
					Self->SetStatusMessage(TEXT("Reported. Thank you!"), /*bIsError=*/false);

					// Auto-close after 2 seconds. Schedule on game thread via
					// a delayed AsyncTask — we don't have a Tick of our own
					// and SetTimer requires a UWorld which we may not have
					// in editor contexts.
					TWeakPtr<SKismetAIBugReportDialog> CloseWeak = WeakSelf;
					FFunctionGraphTask::CreateAndDispatchWhenReady(
						[CloseWeak]()
						{
							FPlatformProcess::Sleep(2.0f);
							AsyncTask(ENamedThreads::GameThread, [CloseWeak]()
							{
								TSharedPtr<SKismetAIBugReportDialog> S = CloseWeak.Pin();
								if (S.IsValid()) S->CloseHostingWindow();
							});
						},
						TStatId(), nullptr, ENamedThreads::AnyBackgroundThreadNormalTask);
					return;
				}

				// Non-2xx: try to extract {error: "..."} from the body.
				FString Detail = FString::Printf(TEXT("HTTP %d"), Code);
				const FString RespBody = Resp->GetContentAsString();
				if (!RespBody.IsEmpty())
				{
					TSharedPtr<FJsonObject> ErrJson;
					TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RespBody);
					if (FJsonSerializer::Deserialize(Reader, ErrJson) && ErrJson.IsValid())
					{
						FString Err;
						if (ErrJson->TryGetStringField(TEXT("error"), Err) && !Err.IsEmpty())
						{
							Detail = Err;
						}
					}
				}
				Self->SetStatusMessage(
					FString::Printf(TEXT("Failed: %s"), *Detail),
					/*bIsError=*/true);
			});
		});

	Req->ProcessRequest();
}
