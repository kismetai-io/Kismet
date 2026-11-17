// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIBridge.h"
#include "KismetAIPlatformCommands.h"
#include "SKismetAIPanel.h"
#include "SKismetAISettingsDialog.h"
#include "SKismetAIBugReportDialog.h"
#include "KismetAIModule.h"
#include "KismetAIAgent.h"
#include "KismetAIAuth.h"
#include "KismetAITelemetry.h"
#include "KismetAIKeyStore.h"
#include "KismetAILLMProvider.h"
#include "KismetAIProviderProfile.h"
#include "KismetAISessionStorage.h"
#include "KismetAIUpdater.h"
#include "KismetAIProjectContext.h"
#include "KismetAILog.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Async/Async.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "ISourceCodeAccessModule.h"
#include "ISourceCodeAccessor.h"
#include "Features/IModularFeatures.h"
#include "Modules/ModuleManager.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

namespace
{
	void LaunchFromBridge(TWeakPtr<SKismetAIPanel> WeakPanel, const FString& Text, bool bIncludeBlueprint, const FString& ImagesJson)
	{
		if (const auto Panel = WeakPanel.Pin()) Panel->LaunchAgentFromWeb(Text, bIncludeBlueprint, ImagesJson);
	}

	void LaunchWithRefsFromBridge(TWeakPtr<SKismetAIPanel> WeakPanel, const FString& Text, const FString& RefsJson, const FString& ImagesJson)
	{
		if (const auto Panel = WeakPanel.Pin()) Panel->LaunchAgentWithRefs(Text, RefsJson, ImagesJson);
	}
}

void UKismetAIBridge::SendMessage(const FString& Text, bool bIncludeBlueprint)
{
	LaunchFromBridge(OwnerPanel, Text, bIncludeBlueprint, FString());
}

void UKismetAIBridge::Abort()
{
	TSharedPtr<SKismetAIPanel> Panel = OwnerPanel.Pin();
	if (Panel.IsValid())
	{
		Panel->AbortAgent();
	}
}

void UKismetAIBridge::AnswerQuestion(const FString& QuestionId, const FString& AnswersJson)
{
	// Bridge UFUNCTIONs are invoked on the game thread; SubmitAgentAnswer just
	// triggers an event, so no bounce is needed.
	// The panel is the editor user, the only answerer an approval accepts.
	FKismetAIModule::Get().SubmitAgentAnswer(QuestionId, AnswersJson, EKismetAIEntryPoint::Panel);
}

FString UKismetAIBridge::GetActiveBlueprintPath()
{
	TSharedPtr<SKismetAIPanel> Panel = OwnerPanel.Pin();
	return Panel.IsValid() ? Panel->GetActiveBlueprintPathString() : FString();
}

bool UKismetAIBridge::OpenAsset(const FString& AssetPath)
{
	// Bridge UFUNCTIONs are invoked on the game thread by the WebBrowser, so
	// GEditor / asset-registry access here needs no thread bounce.
	if (!GEditor)
	{
		return false;
	}

	FString Path = AssetPath;
	Path.TrimStartAndEndInline();
	if (!Path.StartsWith(TEXT("/")))
	{
		return false; // only project/engine content paths are clickable
	}

	// Normalize a package path ("/Game/Foo/BP_Bar") to an object path
	// ("/Game/Foo/BP_Bar.BP_Bar"). Agent output uses either form.
	if (!Path.Contains(TEXT(".")))
	{
		FString AssetName;
		if (Path.Split(TEXT("/"), nullptr, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd)
			&& !AssetName.IsEmpty())
		{
			Path = Path + TEXT(".") + AssetName;
		}
	}

	// Resolve via the asset registry first so a hallucinated path is a no-op
	// rather than a load attempt. Mirrors the @-ref resolution path.
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(Path));
	if (!AssetData.IsValid())
	{
		KISMETAI_LOG(LogTemp, Verbose, TEXT("OpenAsset: no asset at '%s'"), *Path);
		return false;
	}

	UObject* Asset = AssetData.GetAsset(); // loads on demand
	if (!Asset)
	{
		return false;
	}

	UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!Subsystem)
	{
		return false;
	}
	return Subsystem->OpenEditorForAsset(Asset);
}

FString UKismetAIBridge::SearchAssets(const FString& Query, const FString& TypeFilter)
{
	// Must run on the game thread: bridge UFUNCTIONs are always called on
	// the game thread by SWebBrowser, so no bounce needed here.
	// No assets are loaded — only FAssetData tag reads.
	constexpr int32 MaxResults = 50;
	TArray<FAssetData> Assets;
	FKismetAIModule::QueryAssets(Query, TypeFilter, MaxResults, Assets);

	// Serialize as [{path, name, class}, …] — compact keys for JS autocomplete.
	FString Json = TEXT("[");
	for (int32 i = 0; i < Assets.Num(); i++)
	{
		if (i > 0)
		{
			Json += TEXT(",");
		}
		const FAssetData& Asset = Assets[i];

		// Escape the three string fields for JSON. None of these contain
		// characters that require escaping in practice (UE asset paths /
		// names are restricted ASCII), but we escape quotes defensively.
		auto EscapeJson = [](const FString& In) -> FString
		{
			return In
				.Replace(TEXT("\\"), TEXT("\\\\"))
				.Replace(TEXT("\""), TEXT("\\\""));
		};

		Json += FString::Printf(
			TEXT("{\"path\":\"%s\",\"name\":\"%s\",\"class\":\"%s\"}"),
			*EscapeJson(Asset.GetObjectPathString()),
			*EscapeJson(Asset.AssetName.ToString()),
			*EscapeJson(Asset.AssetClassPath.GetAssetName().ToString()));
	}
	Json += TEXT("]");
	return Json;
}

bool UKismetAIBridge::GetServerStatus()
{
	return FKismetAIModule::Get().IsHttpServerRunning();
}

FString UKismetAIBridge::GetClipboard()
{
	FString Text;
	FPlatformApplicationMisc::ClipboardPaste(Text);
	return Text;
}

FString UKismetAIBridge::GetClipboardImage()
{
#if PLATFORM_MAC
	// Run swift on a background thread to avoid blocking the game thread
	// (ExecProcess blocks, which prevents WKWebView promise resolution).
	// Result is pushed to JS via ExecuteJavascriptSafe callback.
	TWeakPtr<SKismetAIPanel> WeakPanel = OwnerPanel;

	static int32 ClipCounter = 0;
	int32 ThisClip = ClipCounter++;
	// Confine clipboard images to a dedicated subdir so the /api/v1/image
	// reader can't be tricked into serving sibling files (request_body.json,
	// response_sse.txt, etc.) that the agent writes alongside.
	FString TempDir = FPaths::ProjectSavedDir() / TEXT("KismetAI") / TEXT("clipboard");
	FString PngPath = TempDir / FString::Printf(TEXT("clipboard_%d.png"), ThisClip);

	Async(EAsyncExecution::ThreadPool, [WeakPanel, TempDir, PngPath]()
	{
		IFileManager::Get().MakeDirectory(*TempDir, true);

		FString SwiftCode = FString::Printf(TEXT(
			"import AppKit\n"
			"let pb = NSPasteboard.general\n"
			"guard let data = pb.data(forType: .png) ?? pb.data(forType: .tiff) else { exit(0) }\n"
			"guard let rep = NSBitmapImageRep(data: data) else { exit(0) }\n"
			"var finalRep = rep\n"
			"let maxDim = 1568\n"
			"let w = rep.pixelsWide, h = rep.pixelsHigh\n"
			"if w > maxDim || h > maxDim {\n"
			"    let scale = Double(maxDim) / Double(max(w, h))\n"
			"    let nw = Int(Double(w) * scale), nh = Int(Double(h) * scale)\n"
			"    let img = NSImage(size: NSSize(width: nw, height: nh))\n"
			"    img.lockFocus()\n"
			"    rep.draw(in: NSRect(x: 0, y: 0, width: nw, height: nh))\n"
			"    if let nr = NSBitmapImageRep(focusedViewRect: NSRect(x: 0, y: 0, width: nw, height: nh)) { finalRep = nr }\n"
			"    img.unlockFocus()\n"
			"}\n"
			"guard let png = finalRep.representation(using: .png, properties: [:]) else { exit(0) }\n"
			"let path = \"%s\"\n"
			"try! png.write(to: URL(fileURLWithPath: path))\n"
			"print(png.base64EncodedString())\n"
		), *PngPath);

		FString ScriptPath = TempDir / TEXT("clipboard_image.swift");
		FFileHelper::SaveStringToFile(SwiftCode, *ScriptPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

		int32 ExitCode = -1;
		FString StdOut, StdErr;
		FPlatformProcess::ExecProcess(
			TEXT("/usr/bin/swift"),
			*ScriptPath,
			&ExitCode, &StdOut, &StdErr);

		KISMETAI_DEV_LOG(LogTemp, Log, TEXT("[KismetAI] GetClipboardImage: exit=%d, stdout=%d chars"), ExitCode, StdOut.Len());

		if (ExitCode == 0 && !StdOut.IsEmpty())
		{
			StdOut.TrimStartAndEndInline();
			FString FullPath = FPaths::ConvertRelativePathToFull(PngPath);
			FString Filename = FPaths::GetCleanFilename(PngPath);

			// Save base64 to separate file for reading at send time
			FString Base64Path = PngPath + TEXT(".b64");
			FFileHelper::SaveStringToFile(StdOut, *Base64Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

			// Push result to JS on the game thread
			FString JsCall = FString::Printf(
				TEXT("window.onClipboardImage && window.onClipboardImage({filename:'%s',path:'file://%s'})"),
				*Filename, *FullPath);

			KISMETAI_TRACE(LogTemp, TEXT("[KismetAI] Dispatching JS callback: %s"), *JsCall.Left(200));

			AsyncTask(ENamedThreads::GameThread, [WeakPanel, JsCall]()
			{
				TSharedPtr<SKismetAIPanel> Panel = WeakPanel.Pin();
				if (Panel.IsValid())
				{
					KISMETAI_DEV_LOG(LogTemp, Log, TEXT("[KismetAI] Executing JS callback on game thread"));
					Panel->ExecuteJavascriptSafe(JsCall);
				}
				else
				{
					KISMETAI_DEV_LOG(LogTemp, Warning, TEXT("[KismetAI] Panel expired, can't deliver clipboard image"));
				}
			});
		}
		else
		{
			// No image — tell JS to fall through to text paste
			AsyncTask(ENamedThreads::GameThread, [WeakPanel]()
			{
				TSharedPtr<SKismetAIPanel> Panel = WeakPanel.Pin();
				if (Panel.IsValid())
				{
					Panel->ExecuteJavascriptSafe(TEXT("window.onClipboardImageEmpty && window.onClipboardImageEmpty()"));
				}
			});
		}
	});

	return FString(); // Returns immediately; result comes via JS callback
#else
	return FString();
#endif
}

FString UKismetAIBridge::ReadImageBase64(const FString& Filename)
{
	if (Filename.Contains(TEXT(".."))
		|| Filename.Contains(TEXT("/"))
		|| Filename.Contains(TEXT("\\"))
		|| Filename.Contains(TEXT(":")))
	{
		return FString();
	}
	FString Base64Path = FPaths::ProjectSavedDir() / TEXT("KismetAI") / TEXT("clipboard") / Filename + TEXT(".b64");
	FString Base64;
	if (FFileHelper::LoadFileToString(Base64, *Base64Path))
	{
		Base64.TrimStartAndEndInline();
		return Base64;
	}
	return FString();
}

void UKismetAIBridge::SetClipboard(const FString& Text)
{
	FPlatformApplicationMisc::ClipboardCopy(*Text);
}

void UKismetAIBridge::SendMessageWithImages(const FString& Text, bool bIncludeBlueprint, const FString& ImagesJson)
{
	LaunchFromBridge(OwnerPanel, Text, bIncludeBlueprint, ImagesJson);
}

void UKismetAIBridge::SendMessageWithRefs(const FString& Text, const FString& RefsJson, const FString& ImagesJson)
{
	LaunchWithRefsFromBridge(OwnerPanel, Text, RefsJson, ImagesJson);
}

bool UKismetAIBridge::IsTelemetryConsentKnown()
{
	return KismetAITelemetry::IsConsentKnown();
}

bool UKismetAIBridge::IsTelemetryConsented()
{
	return KismetAITelemetry::HasConsented();
}

void UKismetAIBridge::AcceptTelemetryConsent()
{
	TWeakPtr<SKismetAIPanel> WeakPanel = OwnerPanel;
	KismetAIAuth::AcceptTelemetryConsent([WeakPanel](bool bOk)
	{
		TSharedPtr<SKismetAIPanel> Panel = WeakPanel.Pin();
		if (!Panel.IsValid()) return;

		// On success, re-push the initial state so applyLoginState clears the
		// consent gate (the local consent mirror was set in the auth layer).
		if (bOk)
		{
			Panel->PushInitialState();
		}
		Panel->ExecuteJavascriptSafe(FString::Printf(
			TEXT("window.onTelemetryConsentResult && window.onTelemetryConsentResult(%s)"),
			bOk ? TEXT("true") : TEXT("false")));
	});
}

// API-key entry is unified across providers: it targets whichever provider the
// currently-selected model routes to. The frontend's single /login flow therefore
// stores the OpenAI key in openai_key.enc when a GPT model is active, the MiniMax
// key in minimax_key.enc when a MiniMax model is active, and the Anthropic key in
// anthropic_key.enc otherwise. The Anthropic path also sweeps legacy plaintext
// credentials.

static FString GetCredentialsPath()
{
	return FKismetAIModule::GetConfigDir() / TEXT("credentials");
}

// Legacy ~/.kismetai/credentials path — still consulted on delete so we don't
// leave an old key behind after a brand-name migration.
static FString GetLegacyCredentialsPath()
{
	return FPlatformMisc::GetEnvironmentVariable(TEXT("HOME")) / TEXT(".kismetai") / TEXT("credentials");
}

bool UKismetAIBridge::HasApiKey()
{
	// Provider-aware via the run's profile: the selected model's provider (or the
	// explicitly chosen profile) decides which key slot counts. Local profiles
	// need no key. never another slot's key.
	const FKismetAIAgentConfig& Config = FKismetAIModule::Get().GetAgentConfig();
	return KismetAIProviders::HasUsableKey(Config, KismetAIProviders::ResolveProfile(Config));
}

bool UKismetAIBridge::SetApiKey(const FString& Key)
{
	FString Trimmed = Key;
	Trimmed.TrimStartAndEndInline();
	if (Trimmed.IsEmpty()) return false;

	FKismetAIAgentConfig Config = FKismetAIModule::Get().GetAgentConfig();
	const FKismetAIProviderProfile& Profile = KismetAIProviders::ResolveProfile(Config);
	// Local profiles take no key.
	if (Profile.KeySlot.IsEmpty())
	{
		return false;
	}
	if (!KismetAIKeyStore::SaveProviderKey(Profile.KeySlot, Trimmed))
	{
		return false;
	}

	if (Profile.KeySlot == TEXT("anthropic"))
	{
		// Sweep any leftover plaintext from older builds so it can't shadow.
		IFileManager& FileMgr = IFileManager::Get();
		FileMgr.Delete(*GetCredentialsPath(), false, true);
		FileMgr.Delete(*GetLegacyCredentialsPath(), false, true);
	}

	KismetAIProviders::SetApiKey(Config, Profile.KeySlot, Trimmed);
	FKismetAIModule::Get().SetAgentConfig(Config);
	return true;
}

void UKismetAIBridge::ClearApiKey()
{
	FKismetAIAgentConfig Config = FKismetAIModule::Get().GetAgentConfig();
	const FKismetAIProviderProfile& Profile = KismetAIProviders::ResolveProfile(Config);
	if (Profile.KeySlot.IsEmpty()) return;

	KismetAIKeyStore::ClearProviderKey(Profile.KeySlot);
	if (Profile.KeySlot == TEXT("anthropic"))
	{
		// Also remove any legacy plaintext that may still be around.
		IFileManager& FileMgr = IFileManager::Get();
		FileMgr.Delete(*GetCredentialsPath());
		FileMgr.Delete(*GetLegacyCredentialsPath());
	}

	// Revert the running config to the environment variable (empty if unset).
	const FString EnvKey = Profile.KeyEnvVar.IsEmpty() ? FString() : FPlatformMisc::GetEnvironmentVariable(*Profile.KeyEnvVar);
	KismetAIProviders::SetApiKey(Config, Profile.KeySlot, EnvKey);
	FKismetAIModule::Get().SetAgentConfig(Config);
}

void UKismetAIBridge::OpenSettings()
{
	// The key most likely wanted is the one the selected model needs.
	OpenApiKeys(KismetAIProviders::ResolveProviderId(FKismetAIModule::Get().GetAgentConfig()));
}

void UKismetAIBridge::OpenApiKeys(const FString& ProviderId)
{
	// Bridge UFUNCTIONs are invoked on the game thread; opening the Slate window
	// directly is safe. Guard anyway so a future off-thread caller can't crash
	// FSlateApplication.
	const FString Id = ProviderId.TrimStartAndEnd();
	if (IsInGameThread())
	{
		SKismetAISettingsDialog::OpenAsWindow(Id);
	}
	else
	{
		AsyncTask(ENamedThreads::GameThread, [Id]() { SKismetAISettingsDialog::OpenAsWindow(Id); });
	}
}

// Forward declaration — defined in the Session management section below.
static FString JsonObjectToCompactString(TSharedPtr<FJsonObject> Obj);

// ─────────────────────────────────────────────────────────────────────────────
// Device login
// ─────────────────────────────────────────────────────────────────────────────

void UKismetAIBridge::BeginKismetLogin()
{
	TWeakPtr<SKismetAIPanel> WeakPanel = OwnerPanel;
	KismetAIAuth::BeginDeviceLogin([WeakPanel](const KismetAIAuth::FStartResult& R)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (R.bSuccess)
		{
			Obj->SetStringField(TEXT("code"),                  R.Code);
			Obj->SetStringField(TEXT("confirm_number"),        R.ConfirmNumber);
			Obj->SetStringField(TEXT("verification_url"),      R.VerificationUrl);
			Obj->SetNumberField(TEXT("expires_in_seconds"),    R.ExpiresInSeconds);
			Obj->SetNumberField(TEXT("poll_interval_seconds"), R.PollIntervalSeconds);
		}
		else
		{
			Obj->SetStringField(TEXT("error"), R.ErrorMessage);
		}

		FString Js = FString::Printf(
			TEXT("window.onKismetLoginResult && window.onKismetLoginResult(%s)"),
			*JsonObjectToCompactString(Obj));

		TSharedPtr<SKismetAIPanel> Panel = WeakPanel.Pin();
		if (Panel.IsValid()) Panel->ExecuteJavascriptSafe(Js);
	});
}

void UKismetAIBridge::PollKismetLogin(const FString& Code)
{
	TWeakPtr<SKismetAIPanel> WeakPanel = OwnerPanel;

	if (Code.IsEmpty())
	{
		TSharedPtr<SKismetAIPanel> Panel = WeakPanel.Pin();
		if (Panel.IsValid())
		{
			Panel->ExecuteJavascriptSafe(
				TEXT("window.onKismetLoginPollResult && window.onKismetLoginPollResult({status:'error',error:'missing code'})"));
		}
		return;
	}

	KismetAIAuth::PollOnce(Code, [WeakPanel](const KismetAIAuth::FPollResult& R)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		switch (R.Status)
		{
		case KismetAIAuth::EPollStatus::Pending:
			Obj->SetStringField(TEXT("status"), TEXT("pending"));
			break;

		case KismetAIAuth::EPollStatus::Claimed:
			// Save session before pushing to JS so credentials.json is written before JS sees success.
			KismetAIAuth::SaveSession(R.Session);
			Obj->SetStringField(TEXT("status"),       TEXT("claimed"));
			Obj->SetStringField(TEXT("email"),        R.Session.Email);
			Obj->SetStringField(TEXT("display_name"), R.Session.DisplayName);
			// Do NOT include the token in the response.
			// Fetch /auth/plugin/me once right after login so the telemetry
			// consent state is known immediately, then re-push the initial
			// state — this lets the consent notice gate before the user starts
			// chatting instead of waiting for the next panel open.
			KismetAIAuth::ValidateSession([WeakPanel](KismetAIAuth::EValidateResult, const FKismetAIAuthSession&)
			{
				TSharedPtr<SKismetAIPanel> P = WeakPanel.Pin();
				if (P.IsValid()) P->PushInitialState();
			});
			break;

		case KismetAIAuth::EPollStatus::NotFound:
			Obj->SetStringField(TEXT("status"), TEXT("not_found"));
			break;

		case KismetAIAuth::EPollStatus::Expired:
			Obj->SetStringField(TEXT("status"), TEXT("expired"));
			break;

		case KismetAIAuth::EPollStatus::Error:
			Obj->SetStringField(TEXT("status"), TEXT("error"));
			Obj->SetStringField(TEXT("error"),  R.ErrorMessage);
			break;
		}

		FString Js = FString::Printf(
			TEXT("window.onKismetLoginPollResult && window.onKismetLoginPollResult(%s)"),
			*JsonObjectToCompactString(Obj));

		TSharedPtr<SKismetAIPanel> Panel = WeakPanel.Pin();
		if (Panel.IsValid()) Panel->ExecuteJavascriptSafe(Js);
	});
}

void UKismetAIBridge::KismetLogout()
{
	KismetAIAuth::Logout();
}

void UKismetAIBridge::OpenBugReport()
{
	if (IsInGameThread())
	{
		SKismetAIBugReportDialog::OpenAsWindow(FKismetAIModule::Get().GetCurrentSessionId());
	}
}

void UKismetAIBridge::OpenDocs()
{
	const FString DocsUrl = KismetAIAuth::GetBackendUrl() + TEXT("/docs");
	FPlatformProcess::LaunchURL(*DocsUrl, nullptr, nullptr);
}

void UKismetAIBridge::NotifyUiReady()
{
	if (TSharedPtr<SKismetAIPanel> Panel = OwnerPanel.Pin())
	{
		Panel->SetWebUiReady(true);
	}
}

void UKismetAIBridge::OpenInBrowser(const FString& Url)
{
	FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
}

void UKismetAIBridge::BuildPlan(const FString& FilePath)
{
	FString Markdown;
	if (!FFileHelper::LoadFileToString(Markdown, *FilePath))
	{
		KISMETAI_LOG(LogTemp, Warning, TEXT("BuildPlan: could not read plan file %s"), *FilePath);
		return;
	}
	// Build via concatenation, not Printf — the plan markdown is data and may
	// contain '%' sequences; keeping it out of the format string avoids any
	// ambiguity for future editors.
	FString Prompt = TEXT("Implement this approved plan. The plan file is at ") + FilePath + TEXT(".\n\n") + Markdown;
	LaunchWithRefsFromBridge(OwnerPanel, Prompt, TEXT("[]"), TEXT("[]"));
}

void UKismetAIBridge::OpenPlanInEditor(const FString& FilePath, const FString& EditorName)
{
	bool bOpened = false;

	// A specific editor was chosen — find that accessor by name and use it.
	if (!EditorName.IsEmpty() && EditorName != TEXT("system"))
	{
		const FName Wanted(*EditorName);
		TArray<ISourceCodeAccessor*> Accessors =
			IModularFeatures::Get().GetModularFeatureImplementations<ISourceCodeAccessor>(
				TEXT("SourceCodeAccessor"));
		for (ISourceCodeAccessor* Accessor : Accessors)
		{
			if (Accessor && Accessor->GetFName() == Wanted && Accessor->CanAccessSourceCode())
			{
				bOpened = Accessor->OpenSourceFiles(TArray<FString>{ FilePath });
				break;
			}
		}
	}
	// No editor chosen (empty) — fall back to the configured default accessor.
	else if (EditorName.IsEmpty())
	{
		ISourceCodeAccessModule* SCAModule =
			FModuleManager::GetModulePtr<ISourceCodeAccessModule>("SourceCodeAccess");
		if (SCAModule && SCAModule->GetAccessor().CanAccessSourceCode())
		{
			bOpened = SCAModule->GetAccessor().OpenSourceFiles(TArray<FString>{ FilePath });
		}
	}

	// "system", an unknown accessor, or any failure → OS default app.
	if (!bOpened)
	{
		FPlatformProcess::LaunchURL(*(TEXT("file://") + FilePath), nullptr, nullptr);
	}
}

FString UKismetAIBridge::GetSourceCodeEditors()
{
	TArray<TSharedPtr<FJsonValue>> Editors;
	TArray<ISourceCodeAccessor*> Accessors =
		IModularFeatures::Get().GetModularFeatureImplementations<ISourceCodeAccessor>(
			TEXT("SourceCodeAccessor"));
	for (ISourceCodeAccessor* Accessor : Accessors)
	{
		if (!Accessor || !Accessor->CanAccessSourceCode())
		{
			continue;
		}
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Accessor->GetFName().ToString());
		Obj->SetStringField(TEXT("label"), Accessor->GetNameText().ToString());
		Editors.Add(MakeShared<FJsonValueObject>(Obj));
	}

	FString Output;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
	FJsonSerializer::Serialize(Editors, Writer);
	return Output;
}

FString UKismetAIBridge::GetPlanEditor()
{
	return FKismetAIModule::Get().GetAgentConfig().PlanEditor;
}

void UKismetAIBridge::SetPlanEditor(const FString& EditorName)
{
	FKismetAIAgentConfig Config = FKismetAIModule::Get().GetAgentConfig();
	Config.PlanEditor = EditorName;
	FKismetAIModule::Get().SetAgentConfig(Config);
	FKismetAIModule::Get().SaveSettings();
}

bool UKismetAIBridge::HasKismetSession()
{
	// Use LoadSession (token-non-empty) instead of HasSession (file-exists),
	// so the JS bridge agrees with PushInitialState. A 0-byte or corrupt
	// credentials.json otherwise flipped the UI to a "Signed in as
	// (unknown)" state with no real session.
	FKismetAIAuthSession Session;
	return KismetAIAuth::LoadSession(Session);
}

FString UKismetAIBridge::GetSessionEmail()
{
	FKismetAIAuthSession Session;
	if (KismetAIAuth::LoadSession(Session))
	{
		return Session.Email;
	}
	return FString();
}

FString UKismetAIBridge::GetModel()
{
	return FKismetAIModule::Get().GetAgentConfig().Model;
}

FString UKismetAIBridge::SetModel(const FString& ModelAlias)
{
	FString Resolved = FKismetAIAgent::ResolveModelAlias(ModelAlias);
	if (Resolved.IsEmpty())
	{
		return FString();
	}

	FKismetAIAgentConfig Config = FKismetAIModule::Get().GetAgentConfig();
	Config.Model = Resolved;
	// A picker row names its own provider in the catalog; drop any explicit
	// profile so Opus picked after Ollama does not get sent to Ollama.
	Config.ProviderId.Empty();
	// Most recent first, no duplicates, capped: feeds the picker's Recent section.
	Config.RecentModels.Remove(Resolved);
	Config.RecentModels.Insert(Resolved, 0);
	if (Config.RecentModels.Num() > FKismetAIAgentConfig::MaxRecentModels)
	{
		Config.RecentModels.SetNum(FKismetAIAgentConfig::MaxRecentModels);
	}
	FKismetAIModule::Get().SetAgentConfig(Config);
	FKismetAIModule::Get().SaveSettings();
	return Resolved;
}

FString UKismetAIBridge::GetRecentModels()
{
	TArray<TSharedPtr<FJsonValue>> Ids;
	for (const FString& ModelId : FKismetAIModule::Get().GetAgentConfig().RecentModels)
	{
		Ids.Add(MakeShared<FJsonValueString>(ModelId));
	}
	FString Json;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Ids, Writer);
	return Json;
}

FString UKismetAIBridge::GetAvailableModels()
{
	// Serialized with the JSON writer: display names and ids come from
	// Config/models.json now, so they are data and must be escaped.
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FKismetAIAgent::FModelAliasInfo& Model : FKismetAIAgent::GetAvailableModels())
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("alias"), Model.Alias);
		Row->SetStringField(TEXT("model_id"), Model.ModelId);
		Row->SetStringField(TEXT("display_name"), Model.DisplayName);
		Row->SetNumberField(TEXT("context_window"), Model.ContextWindow);
		Row->SetStringField(TEXT("tier"), Model.Tier);
		Row->SetStringField(TEXT("recommended_effort"), Model.RecommendedEffort);
		Row->SetStringField(TEXT("provider"), Model.Provider);
		TArray<TSharedPtr<FJsonValue>> Effort;
		for (const FString& Level : Model.EffortLevels)
		{
			Effort.Add(MakeShared<FJsonValueString>(Level));
		}
		Row->SetArrayField(TEXT("effort_levels"), Effort);
		Row->SetBoolField(TEXT("supports_fast_mode"), Model.bSupportsFastMode);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	FString Json;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Rows, Writer);
	return Json;
}

FString UKismetAIBridge::GetProviders()
{
	const FKismetAIAgentConfig& Config = FKismetAIModule::Get().GetAgentConfig();
	const FString ActiveId = KismetAIProviders::ResolveProviderId(Config);
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FKismetAIProviderProfile& Profile : FKismetAIProviderRegistry::Get().GetProfiles())
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("id"), Profile.Id);
		Row->SetStringField(TEXT("display_name"), Profile.DisplayName);
		Row->SetStringField(TEXT("protocol"),
			Profile.Protocol == EKismetAIWireProtocol::AnthropicMessages ? TEXT("anthropic_messages")
			: Profile.Protocol == EKismetAIWireProtocol::OpenAIResponses ? TEXT("openai_responses")
			: TEXT("openai_chat"));
		Row->SetBoolField(TEXT("local"), Profile.bLocal);
		Row->SetBoolField(TEXT("custom_models"), Profile.bCustomModels);
		Row->SetBoolField(TEXT("needs_key"), Profile.Auth != EKismetAIAuthStyle::None);
		Row->SetBoolField(TEXT("has_key"), KismetAIProviders::HasUsableKey(Config, Profile));
		Row->SetStringField(TEXT("key_url"), Profile.KeyUrl);
		Row->SetStringField(TEXT("key_env"), Profile.KeyEnvVar);
		Row->SetStringField(TEXT("base_url"), KismetAIProviders::GetBaseUrl(Config, Profile));
		Row->SetBoolField(TEXT("base_url_overridden"), Config.ProviderBaseUrls.Contains(Profile.Id));
		Row->SetBoolField(TEXT("active"), Profile.Id == ActiveId);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	FString Json;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Rows, Writer);
	return Json;
}

FString UKismetAIBridge::SetProvider(const FString& ProviderId, const FString& Model, const FString& BaseUrl)
{
	const FString Id = ProviderId.TrimStartAndEnd();
	const FKismetAIProviderProfile* Profile = Id.IsEmpty() ? nullptr : FKismetAIProviderRegistry::Get().Find(Id);
	if (!Id.IsEmpty() && !Profile)
	{
		return FString();
	}

	FString ModelId = Model.TrimStartAndEnd();
	const FString Resolved = FKismetAIAgent::ResolveModelAlias(ModelId);
	if (!Resolved.IsEmpty())
	{
		ModelId = Resolved;
	}
	else if (!(Profile && Profile->bCustomModels && !ModelId.IsEmpty()))
	{
		return FString();  // unknown model on a profile with a fixed catalog
	}

	FKismetAIAgentConfig Config = FKismetAIModule::Get().GetAgentConfig();
	Config.ProviderId = Profile ? Profile->Id : FString();
	Config.Model = ModelId;
	const FString Url = BaseUrl.TrimStartAndEnd();
	if (Profile && Url == TEXT("-"))
	{
		Config.ProviderBaseUrls.Remove(Profile->Id);
	}
	else if (Profile && !Url.IsEmpty())
	{
		if (!Url.StartsWith(TEXT("http://")) && !Url.StartsWith(TEXT("https://")))
		{
			return FString();
		}
		Config.ProviderBaseUrls.Add(Profile->Id, Url);
	}
	FKismetAIModule::Get().SetAgentConfig(Config);
	FKismetAIModule::Get().SaveSettings();
	return ModelId;
}

bool UKismetAIBridge::IsAccountWelcomeSeen()
{
	return FKismetAIModule::Get().GetAgentConfig().bAccountWelcomeSeen;
}

void UKismetAIBridge::MarkAccountWelcomeSeen()
{
	FKismetAIAgentConfig Config = FKismetAIModule::Get().GetAgentConfig();
	if (Config.bAccountWelcomeSeen)
	{
		return;
	}
	Config.bAccountWelcomeSeen = true;
	FKismetAIModule::Get().SetAgentConfig(Config);
	FKismetAIModule::Get().SaveSettings();
}

FString UKismetAIBridge::GetThinkingMode()
{
	return FKismetAIAgent::ThinkingModeToString(FKismetAIModule::Get().GetAgentConfig().ThinkingMode);
}

FString UKismetAIBridge::SetThinkingMode(const FString& Mode)
{
	// Parse optional budget: "enabled,10000"
	FString ModeStr = Mode;
	int32 BudgetOverride = 0;
	int32 CommaIdx;
	if (Mode.FindChar(TEXT(','), CommaIdx))
	{
		ModeStr = Mode.Left(CommaIdx);
		FString BudgetStr = Mode.Mid(CommaIdx + 1);
		BudgetOverride = FCString::Atoi(*BudgetStr);
	}

	EThinkingMode NewMode = FKismetAIAgent::StringToThinkingMode(ModeStr);

	FKismetAIAgentConfig Config = FKismetAIModule::Get().GetAgentConfig();
	Config.ThinkingMode = NewMode;
	if (BudgetOverride > 0)
	{
		Config.ThinkingBudgetTokens = BudgetOverride;
	}
	FKismetAIModule::Get().SetAgentConfig(Config);
	FKismetAIModule::Get().SaveSettings();

	return FKismetAIAgent::ThinkingModeToString(NewMode);
}

int32 UKismetAIBridge::GetThinkingBudgetTokens()
{
	return FKismetAIModule::Get().GetAgentConfig().ThinkingBudgetTokens;
}

FString UKismetAIBridge::GetEffortLevel()
{
	return FKismetAIAgent::EffortLevelToString(FKismetAIModule::Get().GetAgentConfig().EffortLevel);
}

FString UKismetAIBridge::SetEffortLevel(const FString& Level)
{
	FString Lower = Level.TrimStartAndEnd().ToLower();

	TOptional<EEffortLevel> Parsed = FKismetAIAgent::StringToEffortLevel(Lower);
	if (!Parsed.IsSet())
	{
		return FString();
	}

	FKismetAIAgentConfig Config = FKismetAIModule::Get().GetAgentConfig();
	Config.EffortLevel = Parsed.GetValue();
	FKismetAIModule::Get().SetAgentConfig(Config);
	FKismetAIModule::Get().SaveSettings();
	return FKismetAIAgent::EffortLevelToString(Parsed.GetValue());
}

FString UKismetAIBridge::GetFastMode()
{
	return FKismetAIModule::Get().GetAgentConfig().bFastMode ? TEXT("on") : TEXT("off");
}

FString UKismetAIBridge::SetFastMode(const FString& State)
{
	const FString Lower = State.TrimStartAndEnd().ToLower();

	bool bEnable;
	if (Lower == TEXT("on") || Lower == TEXT("true") || Lower == TEXT("1"))
	{
		bEnable = true;
	}
	else if (Lower == TEXT("off") || Lower == TEXT("false") || Lower == TEXT("0"))
	{
		bEnable = false;
	}
	else
	{
		return FString();  // invalid input
	}

	FKismetAIAgentConfig Config = FKismetAIModule::Get().GetAgentConfig();
	Config.bFastMode = bEnable;
	FKismetAIModule::Get().SetAgentConfig(Config);
	FKismetAIModule::Get().SaveSettings();
	return bEnable ? TEXT("on") : TEXT("off");
}

// ─────────────────────────────────────────────────────────────────────────────
// Message queue bridge
// ─────────────────────────────────────────────────────────────────────────────

void UKismetAIBridge::EnqueueMessage(const FString& Text, const FString& RefsJson, const FString& ImagesJson)
{
	FKismetAIModule::Get().EnqueueUserMessage(Text, RefsJson, ImagesJson);
}

void UKismetAIBridge::SteerNow(const FString& Text, const FString& RefsJson, const FString& ImagesJson)
{
	FKismetAIModule::Get().SteerNow(Text, RefsJson, ImagesJson);
}

void UKismetAIBridge::SteerQueued(const FString& Id)
{
	FKismetAIModule::Get().SteerQueued(Id);
}

void UKismetAIBridge::RemoveQueued(const FString& Id)
{
	FKismetAIModule::Get().RemoveQueued(Id);
}

void UKismetAIBridge::MoveQueued(const FString& Id, int32 NewIndex)
{
	FKismetAIModule::Get().MoveQueued(Id, NewIndex);
}

void UKismetAIBridge::EditQueued(const FString& Id, const FString& NewText)
{
	FKismetAIModule::Get().EditQueued(Id, NewText);
}

FString UKismetAIBridge::GetQueueJson()
{
	return FKismetAIModule::Get().GetQueueJson();
}

// ─────────────────────────────────────────────────────────────────────────────
// Project context (/init onboarding)
// ─────────────────────────────────────────────────────────────────────────────

FString UKismetAIBridge::InitProjectContext()
{
	// Bridge UFUNCTIONs run on the game thread, so file IO here is safe.
	const FString ProjectRoot = FPaths::ProjectDir();
	FKismetAIProjectContext::FInitResult Result =
		FKismetAIProjectContext::InitProjectContext(ProjectRoot);

	// Open the file for the user after a successful create/import (or if it
	// already exists — they likely want to edit it).
	if (Result.Status != FKismetAIProjectContext::EInitStatus::Failed)
	{
		const FString AbsPath = FPaths::ConvertRelativePathToFull(
			ProjectRoot / FKismetAIProjectContext::GetTargetFileName());
		if (IFileManager::Get().FileExists(*AbsPath))
		{
			FPlatformProcess::LaunchURL(*(TEXT("file://") + AbsPath), nullptr, nullptr);
		}
	}

	return FKismetAIProjectContext::InitResultToJson(Result);
}

FString UKismetAIBridge::GetProjectContextStatus()
{
	const FString ProjectRoot = FPaths::ProjectDir();
	FKismetAIProjectContext::FScanResult Scan =
		FKismetAIProjectContext::ScanProjectRoot(ProjectRoot);
	const bool bDismissed = FKismetAIModule::Get().GetAgentConfig().bOnboardingDismissed;
	return FKismetAIProjectContext::StatusToJson(Scan, bDismissed);
}

void UKismetAIBridge::DismissOnboarding()
{
	FKismetAIAgentConfig Config = FKismetAIModule::Get().GetAgentConfig();
	Config.bOnboardingDismissed = true;
	FKismetAIModule::Get().SetAgentConfig(Config);
	FKismetAIModule::Get().SaveSettings();
}

FString UKismetAIBridge::GetImportSources()
{
	const FString ProjectRoot = FPaths::ProjectDir();
	const TArray<FKismetAIProjectContext::FDetectedSource> Sources =
		FKismetAIProjectContext::DetectSources(ProjectRoot);
	const FString ExistingTarget = FKismetAIProjectContext::FindExistingTarget(ProjectRoot);
	const bool bDismissed = FKismetAIModule::Get().GetAgentConfig().bOnboardingDismissed;
	return FKismetAIProjectContext::SourcesToJson(Sources, ExistingTarget, bDismissed);
}

FString UKismetAIBridge::ImportSources(const FString& SelectedIdsJson)
{
	// Parse the JSON array of source ids the user checked in the panel.
	TArray<FString> SelectedIds;
	TArray<TSharedPtr<FJsonValue>> Parsed;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SelectedIdsJson);
	if (FJsonSerializer::Deserialize(Reader, Parsed))
	{
		for (const TSharedPtr<FJsonValue>& Val : Parsed)
		{
			FString Id;
			if (Val.IsValid() && Val->TryGetString(Id) && !Id.IsEmpty())
			{
				SelectedIds.Add(Id);
			}
		}
	}

	const FString ProjectRoot = FPaths::ProjectDir();
	const FKismetAIProjectContext::FInitResult Result =
		FKismetAIProjectContext::ImportSelected(ProjectRoot, SelectedIds);

	// Open the written file so the user can review/edit the merged result.
	if (Result.Status != FKismetAIProjectContext::EInitStatus::Failed &&
		IFileManager::Get().FileExists(*Result.TargetPath))
	{
		FPlatformProcess::LaunchURL(*(TEXT("file://") + Result.TargetPath), nullptr, nullptr);
	}

	return FKismetAIProjectContext::InitResultToJson(Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// Session management
// ─────────────────────────────────────────────────────────────────────────────

static FString JsonObjectToCompactString(TSharedPtr<FJsonObject> Obj)
{
	FString Output;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
	FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
	return Output;
}

FString UKismetAIBridge::ListSessions()
{
	TArray<FKismetAISessionInfo> Sessions = FKismetAISessionStorage::ListSessions(50);

	FString Json = TEXT("[");
	for (int32 i = 0; i < Sessions.Num(); i++)
	{
		if (i > 0) Json += TEXT(",");

		const FKismetAISessionInfo& S = Sessions[i];

		// Escape title for JSON
		FString EscapedTitle = S.Title
			.Replace(TEXT("\\"), TEXT("\\\\"))
			.Replace(TEXT("\""), TEXT("\\\""))
			.Replace(TEXT("\n"), TEXT("\\n"))
			.Replace(TEXT("\r"), TEXT("\\r"))
			.Replace(TEXT("\t"), TEXT("\\t"));

		Json += FString::Printf(
			TEXT("{\"id\":\"%s\",\"title\":\"%s\",\"blueprint\":\"%s\",\"model\":\"%s\",")
			TEXT("\"created\":\"%s\",\"modified\":\"%s\",")
			TEXT("\"cost_usd\":%.6f,\"turn_count\":%d,\"input_tokens\":%d,\"output_tokens\":%d}"),
			*S.SessionId,
			*EscapedTitle,
			*S.BlueprintPath,
			*S.Model,
			*S.Created.ToIso8601(),
			*S.Modified.ToIso8601(),
			S.CostUSD, S.TurnCount, S.TotalInputTokens, S.TotalOutputTokens);
	}
	Json += TEXT("]");
	return Json;
}

FString UKismetAIBridge::LoadSession(const FString& SessionId)
{
	TArray<TSharedPtr<FJsonObject>> Entries = FKismetAISessionStorage::LoadAllEntries(SessionId);

	FString Json = TEXT("[");
	for (int32 i = 0; i < Entries.Num(); i++)
	{
		if (i > 0) Json += TEXT(",");
		Json += JsonObjectToCompactString(Entries[i]);
	}
	Json += TEXT("]");

	// Track this session at the module level
	FKismetAIModule::Get().SetCurrentSessionId(SessionId);

	return Json;
}

bool UKismetAIBridge::DeleteSession(const FString& SessionId)
{
	return FKismetAISessionStorage::DeleteSession(SessionId);
}

void UKismetAIBridge::NewSession()
{
	FKismetAIModule::Get().SetCurrentSessionId(FString());
}

void UKismetAIBridge::SetSessionId(const FString& SessionId)
{
	FKismetAIModule::Get().SetCurrentSessionId(SessionId);
}

static FString GetTabStatePath()
{
	return FPaths::ProjectSavedDir() / TEXT("KismetAI") / TEXT("tabs.json");
}

void UKismetAIBridge::SaveTabState(const FString& Json)
{
	FString Path = GetTabStatePath();
	FString Dir = FPaths::GetPath(Path);
	IFileManager::Get().MakeDirectory(*Dir, true);
	FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

FString UKismetAIBridge::LoadTabState()
{
	FString Path = GetTabStatePath();
	FString Content;
	if (FFileHelper::LoadFileToString(Content, *Path))
	{
		return Content;
	}
	return FString();
}

void UKismetAIBridge::AcceptChanges()
{
	FKismetAIModule::Get().AcceptChanges();
}

void UKismetAIBridge::RejectChanges()
{
	FKismetAIModule::Get().RejectChanges();
}

void UKismetAIBridge::ReapplyChanges()
{
	FKismetAIModule::Get().ReapplyChanges();
}

void UKismetAIBridge::RequestDiffState()
{
	TSharedPtr<SKismetAIPanel> Panel = OwnerPanel.Pin();
	if (!Panel.IsValid()) return;
	FKismetAIModule::Get().PushDiffStateToPanel(Panel.Get());
}

// ─────────────────────────────────────────────────────────────────────────────
// Auto-update
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	// Escape a string for embedding into a single-quoted JS literal.
	FString EscapeForJsString(const FString& In)
	{
		FString Out = In;
		Out.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Out.ReplaceInline(TEXT("'"),  TEXT("\\'"));
		Out.ReplaceInline(TEXT("\r"), TEXT("\\r"));
		Out.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		Out.ReplaceInline(TEXT("\t"), TEXT("\\t"));
		return Out;
	}
}

void UKismetAIBridge::CheckForUpdate()
{
	TWeakPtr<SKismetAIPanel> WeakPanel = OwnerPanel;
	FKismetAIUpdater::CheckAsync([WeakPanel](FKismetAIUpdateInfo Info)
	{
		// Build JSON payload via FJsonObject -> string for safe escaping.
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("current"),           Info.CurrentVersion);
		Obj->SetStringField(TEXT("latest"),            Info.LatestVersion);
		Obj->SetBoolField  (TEXT("hasUpdate"),         Info.bHasUpdate);
		Obj->SetBoolField  (TEXT("hasMatchingAsset"),  Info.bHasMatchingAsset);
		Obj->SetStringField(TEXT("url"),               Info.AssetURL);
		Obj->SetNumberField(TEXT("size"),              static_cast<double>(Info.AssetSizeBytes));
		Obj->SetStringField(TEXT("notes"),             Info.ReleaseNotes);
		Obj->SetStringField(TEXT("error"),             Info.ErrorMessage);

		FString Json;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Json);
		FJsonSerializer::Serialize(Obj, W);

		FString Js = FString::Printf(
			TEXT("window.onUpdateCheckResult && window.onUpdateCheckResult(%s)"),
			*Json);

		TSharedPtr<SKismetAIPanel> Panel = WeakPanel.Pin();
		if (Panel.IsValid()) Panel->ExecuteJavascriptSafe(Js);
	});
}

void UKismetAIBridge::InstallUpdate(const FString& AssetURL)
{
	TWeakPtr<SKismetAIPanel> WeakPanel = OwnerPanel;
	FKismetAIUpdater::InstallAsync(AssetURL, [WeakPanel](bool bOk, FString Err)
	{
		FString Js = FString::Printf(
			TEXT("window.onUpdateInstallResult && window.onUpdateInstallResult({ok:%s,error:'%s'})"),
			bOk ? TEXT("true") : TEXT("false"),
			*EscapeForJsString(Err));

		TSharedPtr<SKismetAIPanel> Panel = WeakPanel.Pin();
		if (Panel.IsValid()) Panel->ExecuteJavascriptSafe(Js);
	});
}

void UKismetAIBridge::AutoCheckForUpdate()
{
	// A Fab copy is updated by the Epic Games Launcher: no launch-time check
	// and no banner. A manual /update still explains why.
	if (!FKismetAIUpdater::IsSelfUpdateEnabled()) return;
	static bool bAlreadyChecked = false;
	if (bAlreadyChecked) return;
	bAlreadyChecked = true;
	CheckForUpdate();
}

void UKismetAIBridge::RestartEditor()
{
	FKismetAIUpdater::RestartEditor();
}

FString UKismetAIBridge::SkillCommand(const FString& Action, const FString& Payload)
{
	return KismetAIPlatformCommands::Local(Action, Payload);
}

void UKismetAIBridge::SkillRegistryRequest(const FString& Action, const FString& Payload)
{
	const TWeakPtr<SKismetAIPanel> WeakPanel = OwnerPanel;
	KismetAIPlatformCommands::Registry(Action, Payload, [WeakPanel](const FString& Json)
	{
		if (const auto Panel = WeakPanel.Pin())
		{
			Panel->ExecuteJavascriptSafe(TEXT("window.onSkillRegistryResult && window.onSkillRegistryResult(") + Json + TEXT(")"));
		}
	});
}
