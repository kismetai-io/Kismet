// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAITelemetry.h"
#include "KismetAIDiagnostics.h"

#include "KismetAIAuth.h"
#include "KismetAILog.h"
#include "KismetAITelemetryInternal.h"

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Interfaces/IPluginManager.h"
#include "Internationalization/Regex.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogKismetAITelemetry, Log, All);

namespace
{
	// ─── State ──────────────────────────────────────────────────────────────
	bool GInitialized = false;
	bool GEnabled = false;
	FString GMachineId;
	FString GPluginVersion;

	// Per-category prefs, mirrored from /auth/plugin/me. See header for the
	// rationale. Defaults match server schema defaults (all-on) so behaviour
	// before the first /me response matches behaviour after, for a fresh user
	// who hasn't changed anything.
	bool GPrefRuns      = true;
	bool GPrefToolCalls = true;
	bool GPrefErrors    = true;
	bool GPrefCrashes   = true;

	// Telemetry-consent acknowledgment, mirrored from /auth/plugin/me. See
	// header for the tri-state rationale: GConsentKnown gates whether
	// GConsented is meaningful, so callers never read an unconfirmed default
	// as a definitive "not consented".
	bool GConsentKnown = false;
	bool GConsented    = false;

	using ECategory = KismetAITelemetryInternal::ECategory;

	bool IsCategoryEnabled(ECategory Cat)
	{
		switch (Cat)
		{
			case ECategory::Always:    return true;
			case ECategory::Runs:      return GPrefRuns;
			case ECategory::ToolCalls: return GPrefToolCalls;
			case ECategory::Errors:    return GPrefErrors;
			case ECategory::Crashes:   return GPrefCrashes;
		}
		return true;
	}

	// ─── Helpers ────────────────────────────────────────────────────────────
	FString TelemetryJoinUrl(const FString& Base, const FString& Path)
	{
		if (Base.EndsWith(TEXT("/")))
		{
			return Base + (Path.StartsWith(TEXT("/")) ? Path.Mid(1) : Path);
		}
		return Base + (Path.StartsWith(TEXT("/")) ? Path : (TEXT("/") + Path));
	}

	// Hash the project name before transmission so the server never sees the
	// user's raw asset/project identifier. Unsalted SHA-1 of the UTF-8 bytes
	// is enough to make the value non-reversible while keeping it stable
	// across runs on the same machine for aggregation.
	FString HashProjectName(const FString& Name)
	{
		if (Name.IsEmpty()) return FString();
		const FTCHARToUTF8 Converted(*Name);
		FSHAHash Digest = FSHA1::HashBuffer(Converted.Get(), Converted.Length());
		return FString::Printf(TEXT("proj_%s"), *Digest.ToString().ToLower());
	}

	// In-process wrapper around the parametric sanitizer. See
	// KismetAITelemetryInternal::SanitizeForTelemetryWithRoots for the rules.
	// Defense in depth for diagnostic strings; SafeProjection also excludes
	// free-form text and content before either output boundary.
	FString SanitizeForTelemetry(FString Msg)
	{
		static const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		static const FString HomeDir    = FString(FPlatformProcess::UserHomeDir());
		return KismetAITelemetryInternal::SanitizeForTelemetryWithRoots(MoveTemp(Msg), ProjectDir, HomeDir);
	}

	FString GetOSString()
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

	FString GetUEVersionString()
	{
		return FEngineVersion::Current().ToString();
	}

	void TelemetryApplyVercelBypass(const TSharedRef<IHttpRequest>& Req)
	{
		const FString Token = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMET_VERCEL_BYPASS"));
		if (!Token.IsEmpty())
		{
			Req->SetHeader(TEXT("x-vercel-protection-bypass"), Token);
		}
	}

	void DispatchHttpPost(const FString& Endpoint, TSharedPtr<FJsonObject> Body, ECategory Cat = ECategory::Always)
	{
		// Neither the legacy network path nor its local fallback receives content.
		Body = KismetAIDiagnostics::SafeProjection(Body);
		KismetAIDiagnostics::Record(TEXT("legacy_telemetry"), Body);

		if (!GEnabled)
		{
			return;
		}

		// Per-category opt-out: the user disabled this telemetry category in
		// their dashboard settings. The server would 204 us anyway, but
		// skipping the POST means the content payload never leaves the
		// machine — which is what the Privacy Policy claims.
		if (!IsCategoryEnabled(Cat))
		{
			return;
		}

		// During engine exit, UObject CDOs (e.g. UKismetAISettings) and the
		// HTTP module may be torn down before our ShutdownModule runs. Touching
		// GetDefault<>() under those conditions crashes inside ModuleManager
		// unload. JSONL has the data; skip the network hop on the way out.
		if (IsEngineExitRequested())
		{
			return;
		}

		FKismetAIAuthSession Session;
		const bool bHasSession = KismetAIAuth::LoadSession(Session);
		if (!bHasSession)
		{
			// Pre-login or logged out — JSONL only. The post-login backfill
			// path (if implemented) can replay from the JSONL file.
			return;
		}

		auto Send = [Endpoint, Body, Session]()
		{
			const FString Url = TelemetryJoinUrl(KismetAIAuth::GetBackendUrl(), Endpoint);

			TSharedRef<FJsonObject> Wrapped = MakeShared<FJsonObject>();
			Wrapped->SetStringField(TEXT("machine_id"), GMachineId);
			Wrapped->SetStringField(TEXT("plugin_version"), GPluginVersion);
			Wrapped->SetStringField(TEXT("os"), GetOSString());
			Wrapped->SetStringField(TEXT("ue_version"), GetUEVersionString());
			if (Body.IsValid())
			{
				Wrapped->SetObjectField(TEXT("data"), Body);
			}

			FString BodyStr;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
			FJsonSerializer::Serialize(Wrapped, Writer);

			TSharedRef<IHttpRequest> Req = FHttpModule::Get().CreateRequest();
			Req->SetVerb(TEXT("POST"));
			Req->SetURL(Url);
			Req->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Session.Token));
			Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
			Req->SetHeader(TEXT("Accept"), TEXT("application/json"));
			Req->SetTimeout(10.0f);
			Req->SetContentAsString(BodyStr);
			TelemetryApplyVercelBypass(Req);

			Req->OnProcessRequestComplete().BindLambda(
				[Endpoint](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
				{
					if (!bOk || !Resp.IsValid())
					{
						KISMETAI_DEV_LOG(LogKismetAITelemetry, Verbose,
							TEXT("Telemetry %s: network error"), *Endpoint);
						return;
					}
					const int32 Code = Resp->GetResponseCode();
					if (Code == 404)
					{
						// Backend route not yet implemented. JSONL has the data;
						// silent.
						return;
					}
					if (Code < 200 || Code >= 300)
					{
						KISMETAI_DEV_LOG(LogKismetAITelemetry, Verbose,
							TEXT("Telemetry %s: HTTP %d"), *Endpoint, Code);
					}
				});

			Req->ProcessRequest();
		};

		if (IsInGameThread())
		{
			Send();
		}
		else
		{
			AsyncTask(ENamedThreads::GameThread, MoveTemp(Send));
		}
	}

	// ─── Log forwarder ──────────────────────────────────────────────────────
	// Captures Warning/Error UE_LOG output from any LogKismetAI* category so
	// they appear in the telemetry stream alongside agent runs. Filtered to
	// our own categories — forwarding all-of-engine would flood the queue.
	class FKismetAILogForwarder : public FOutputDevice
	{
	public:
		virtual void Serialize(const TCHAR* Msg, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Verbosity != ELogVerbosity::Warning && Verbosity != ELogVerbosity::Error)
			{
				return;
			}
			const FString CatStr = Category.ToString();
			if (!CatStr.StartsWith(TEXT("LogKismetAI")))
			{
				return;
			}
			TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("level"),
				Verbosity == ELogVerbosity::Error ? TEXT("error") : TEXT("warning"));
			Payload->SetStringField(TEXT("category"), CatStr);
			Payload->SetStringField(TEXT("message"), SanitizeForTelemetry(FString(Msg)).Left(2000));
			KismetAITelemetry::ReportEvent(
				Verbosity == ELogVerbosity::Error ? TEXT("log_error") : TEXT("log_warning"),
				Payload);
		}
	};

	FKismetAILogForwarder* GLogForwarder = nullptr;

	// ─── Crash scanner ──────────────────────────────────────────────────────
	FString CrashSeenMarkerPath()
	{
		return FPaths::ProjectSavedDir() / TEXT("Logs") / TEXT(".kismet_telemetry_crashes_seen");
	}

	FDateTime LoadLastSeenCrashTime()
	{
		FString Iso;
		if (FFileHelper::LoadFileToString(Iso, *CrashSeenMarkerPath()))
		{
			Iso.TrimStartAndEndInline();
			FDateTime Out;
			if (FDateTime::ParseIso8601(*Iso, Out))
			{
				return Out;
			}
		}
		return FDateTime::MinValue();
	}

	void SaveLastSeenCrashTime(const FDateTime& When)
	{
		const FString Path = CrashSeenMarkerPath();
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
		FFileHelper::SaveStringToFile(When.ToIso8601(), *Path);
	}

	void ScanCrashesOnce()
	{
		const FString CrashDir = FPaths::ProjectSavedDir() / TEXT("Crashes");
		if (!IFileManager::Get().DirectoryExists(*CrashDir))
		{
			return;
		}

		const FDateTime LastSeen = LoadLastSeenCrashTime();
		FDateTime NewestSeen = LastSeen;

		TArray<FString> Folders;
		IFileManager::Get().FindFiles(Folders, *(CrashDir / TEXT("*")), /*Files=*/false, /*Dirs=*/true);

		for (const FString& Folder : Folders)
		{
			const FString FolderPath = CrashDir / Folder;
			const FDateTime Modified = IFileManager::Get().GetTimeStamp(*FolderPath);
			if (Modified <= LastSeen)
			{
				continue;
			}
			if (Modified > NewestSeen)
			{
				NewestSeen = Modified;
			}

			TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("occurred_at"), Modified.ToIso8601());
			Payload->SetStringField(TEXT("folder"), Folder);

			// Best-effort: scoop XML.xml and CrashContext.runtime-xml if present.
			FString CrashCtx;
			FFileHelper::LoadFileToString(CrashCtx, *(FolderPath / TEXT("CrashContext.runtime-xml")));
			if (!CrashCtx.IsEmpty())
			{
				Payload->SetStringField(TEXT("crash_context"), CrashCtx.Left(64 * 1024));
			}
			FString CrashLog;
			FFileHelper::LoadFileToString(CrashLog, *(FolderPath / TEXT("KismetAI.log")));
			if (CrashLog.IsEmpty())
			{
				FFileHelper::LoadFileToString(CrashLog, *(FolderPath / TEXT("UnrealEditor.log")));
			}
			if (!CrashLog.IsEmpty())
			{
				// Tail of the log is what matters for triage.
				const int32 TailBytes = 32 * 1024;
				Payload->SetStringField(TEXT("log_tail"),
					CrashLog.Len() > TailBytes ? CrashLog.Right(TailBytes) : CrashLog);
			}

			KismetAITelemetry::ReportCrash(Payload);
		}

		if (NewestSeen > LastSeen)
		{
			SaveLastSeenCrashTime(NewestSeen);
		}
	}
}

// ─── Test-visible internals ────────────────────────────────────────────────

namespace KismetAITelemetryInternal
{
	ECategory CategoryForEventType(const FString& Type)
	{
		if (Type == TEXT("tool_call"))           return ECategory::ToolCalls;
		if (Type == TEXT("log_warning"))         return ECategory::Errors;
		if (Type == TEXT("log_error"))           return ECategory::Errors;
		if (Type == TEXT("api_error"))           return ECategory::Errors;
		if (Type == TEXT("t3d_inject_error"))    return ECategory::Errors;
		if (Type == TEXT("compile_error"))       return ECategory::Errors;
		return ECategory::Always;
	}

	FString SanitizeForTelemetryWithRoots(
		FString Msg,
		const FString& ProjectDir,
		const FString& HomeDir)
	{
		// Credentials and URL/path-bearing messages are omitted wholesale. JSON
		// payloads are separately projected through a strict metadata allowlist.
		for (const TCHAR* Sensitive : { TEXT("authorization"), TEXT("x-api-key"), TEXT("api_key"), TEXT("api-key"), TEXT("access_token"), TEXT("bearer "), TEXT("://"), TEXT("/tmp/"), TEXT("/private/var/"), TEXT("/var/folders/"), TEXT("\\Temp\\") })
		{
			if (Msg.Contains(Sensitive, ESearchCase::IgnoreCase)) return TEXT("<redacted>");
		}
		if (!ProjectDir.IsEmpty()) Msg.ReplaceInline(*ProjectDir, TEXT("<ProjectDir>/"), ESearchCase::IgnoreCase);
		if (!HomeDir.IsEmpty())    Msg.ReplaceInline(*HomeDir,    TEXT("<Home>/"),       ESearchCase::IgnoreCase);

		// Catch absolute and ../-traversal home paths that UE often emits.
		// Examples replaced:
		//   /Users/foo/projects/...    /home/foo/code/...    C:\Users\foo\...
		//   ../../../../foo/projects/...  ← UE strips "/Users/" during
		//                                   relative-path normalisation, so
		//                                   the home prefix isn't visible
		//                                   when the leak surfaces; we key
		//                                   on the 2+ traversal hops as the
		//                                   structural marker instead.
		auto ReplaceAll = [&Msg](const TCHAR* Pattern)
		{
			const FRegexPattern Pat(Pattern);
			FRegexMatcher M(Pat, Msg);
			TArray<FString> Hits;
			while (M.FindNext())
			{
				Hits.AddUnique(M.GetCaptureGroup(0));
			}
			for (const FString& Hit : Hits)
			{
				Msg.ReplaceInline(*Hit, TEXT("<Home>"));
			}
		};
		// 2-or-more ../ hops followed by a name segment. Legitimate code-side
		// relative paths almost never traverse this many levels; UE emits
		// 4–6-hop traversals when stripping an absolute home dir.
		ReplaceAll(TEXT("(?:\\.\\./){2,}[^/\\s\\\"]+"));
		ReplaceAll(TEXT("/Users/[^/\\s\\\"]+"));
		ReplaceAll(TEXT("/home/[^/\\s\\\"]+"));
		ReplaceAll(TEXT("[A-Za-z]:[\\\\/]Users[\\\\/][^\\\\/\\s\\\"]+"));
		return Msg;
	}
}

// ─── Public API ────────────────────────────────────────────────────────────

void KismetAITelemetry::Initialize()
{
	if (GInitialized)
	{
		return;
	}
	GInitialized = true;

	const FString Disable = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMET_TELEMETRY_DISABLE"));
	GEnabled = !(Disable == TEXT("1") || Disable.Equals(TEXT("true"), ESearchCase::IgnoreCase));

	GMachineId = KismetAIAuth::GetMachineId();

	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("KismetAI")))
	{
		GPluginVersion = Plugin->GetDescriptor().VersionName;
	}

	// Install log forwarder. GLog is the global engine log; FOutputDevice is
	// owned by us — must remove + delete in Shutdown.
	if (GLog && !GLogForwarder)
	{
		GLogForwarder = new FKismetAILogForwarder();
		GLog->AddOutputDevice(GLogForwarder);
	}

	// Scan crash dir async — never block startup on disk.
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, []() { ScanCrashesOnce(); });

	// plugin_loaded heartbeat.
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("hostname"), FPlatformProcess::ComputerName());
	ReportEvent(TEXT("plugin_loaded"), Payload);

	UE_LOG(LogKismetAITelemetry, Log, TEXT("Telemetry initialized (enabled=%d, machine=%s)"),
		GEnabled ? 1 : 0, *GMachineId);
}

void KismetAITelemetry::Shutdown()
{
	if (!GInitialized)
	{
		return;
	}

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	ReportEvent(TEXT("plugin_unloaded"), Payload);

	if (GLog && GLogForwarder)
	{
		GLog->RemoveOutputDevice(GLogForwarder);
		delete GLogForwarder;
		GLogForwarder = nullptr;
	}

	GInitialized = false;
	GEnabled = false;
}

bool KismetAITelemetry::IsEnabled()
{
	return GInitialized && GEnabled;
}

void KismetAITelemetry::UpdatePrefs(bool bRuns, bool bToolCalls, bool bErrors, bool bCrashes)
{
	GPrefRuns      = bRuns;
	GPrefToolCalls = bToolCalls;
	GPrefErrors    = bErrors;
	GPrefCrashes   = bCrashes;
	UE_LOG(LogKismetAITelemetry, Verbose,
		TEXT("Telemetry prefs updated: runs=%d tool_calls=%d errors=%d crashes=%d"),
		bRuns ? 1 : 0, bToolCalls ? 1 : 0, bErrors ? 1 : 0, bCrashes ? 1 : 0);
}

void KismetAITelemetry::UpdateConsent(bool bConsented)
{
	GConsentKnown = true;
	GConsented    = bConsented;
	UE_LOG(LogKismetAITelemetry, Verbose,
		TEXT("Telemetry consent updated: known=1 consented=%d"), bConsented ? 1 : 0);
}

bool KismetAITelemetry::IsConsentKnown()
{
	return GConsentKnown;
}

bool KismetAITelemetry::HasConsented()
{
	return GConsented;
}

FString KismetAITelemetry::GetLocalLogPath()
{
	return KismetAIDiagnostics::GetSafeLogPath();
}

void KismetAITelemetry::ReportRunStarted(const FRunStartPayload& P)
{
	if (!GInitialized) return;
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("client_run_id"), P.ClientRunId);
	Body->SetStringField(TEXT("started_at"), P.StartedAt.ToIso8601());
	Body->SetStringField(TEXT("model"), P.Model);
	Body->SetStringField(TEXT("session_id"), P.SessionId);
	Body->SetStringField(TEXT("initial_prompt"), P.InitialPrompt.Left(4000));
	Body->SetStringField(TEXT("project_name"), HashProjectName(P.ProjectName));
	Body->SetStringField(TEXT("blueprint_path"), P.BlueprintPath);
	DispatchHttpPost(TEXT("/api/v1/telemetry/runs/start"), Body, ECategory::Runs);
}

void KismetAITelemetry::ReportRunMessage(const FRunMessagePayload& P)
{
	if (!GInitialized) return;
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("client_run_id"), P.ClientRunId);
	Body->SetNumberField(TEXT("idx"), P.Idx);
	Body->SetStringField(TEXT("role"), P.Role);
	if (P.Content.IsValid())
	{
		Body->SetField(TEXT("content"), P.Content);
	}
	if (!P.Model.IsEmpty()) Body->SetStringField(TEXT("model"), P.Model);
	Body->SetNumberField(TEXT("input_tokens"), P.InputTokens);
	Body->SetNumberField(TEXT("output_tokens"), P.OutputTokens);
	Body->SetNumberField(TEXT("cache_read_tokens"), P.CacheReadTokens);
	Body->SetNumberField(TEXT("cache_creation_tokens"), P.CacheCreationTokens);
	Body->SetNumberField(TEXT("duration_ms"), P.DurationMs);
	DispatchHttpPost(TEXT("/api/v1/telemetry/runs/message"), Body, ECategory::Runs);
}

void KismetAITelemetry::ReportRunEnded(const FRunEndPayload& P)
{
	if (!GInitialized) return;
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("client_run_id"), P.ClientRunId);
	Body->SetStringField(TEXT("ended_at"), P.EndedAt.ToIso8601());
	Body->SetStringField(TEXT("status"), P.Status);
	Body->SetStringField(TEXT("stop_reason"), P.StopReason);
	Body->SetNumberField(TEXT("num_steps"), P.NumSteps);
	Body->SetNumberField(TEXT("num_tool_calls"), P.NumToolCalls);
	Body->SetNumberField(TEXT("input_tokens"), P.InputTokens);
	Body->SetNumberField(TEXT("output_tokens"), P.OutputTokens);
	Body->SetNumberField(TEXT("cache_read_tokens"), P.CacheReadTokens);
	Body->SetNumberField(TEXT("cache_creation_tokens"), P.CacheCreationTokens);
	Body->SetNumberField(TEXT("cost_usd"), P.CostUSD);
	if (!P.ErrorMessage.IsEmpty())
	{
		Body->SetStringField(TEXT("error_message"), SanitizeForTelemetry(P.ErrorMessage).Left(2000));
	}
	DispatchHttpPost(TEXT("/api/v1/telemetry/runs/end"), Body, ECategory::Runs);
}

void KismetAITelemetry::ReportToolCall(
	const FString& ClientRunId,
	const FString& ToolName,
	bool bSuccess,
	int32 ResultBytes,
	int32 DurationMs,
	const FString& ErrorMessage)
{
	if (!GInitialized) return;
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("client_run_id"), ClientRunId);
	Payload->SetStringField(TEXT("tool"), ToolName);
	Payload->SetBoolField(TEXT("success"), bSuccess);
	Payload->SetNumberField(TEXT("result_bytes"), ResultBytes);
	Payload->SetNumberField(TEXT("duration_ms"), DurationMs);
	if (!ErrorMessage.IsEmpty())
	{
		Payload->SetStringField(TEXT("error"), SanitizeForTelemetry(ErrorMessage).Left(2000));
	}
	ReportEvent(TEXT("tool_call"), Payload);
}

void KismetAITelemetry::ReportEvent(const FString& Type, TSharedPtr<FJsonObject> Payload)
{
	if (!GInitialized) return;
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("type"), Type);
	Body->SetStringField(TEXT("created_at"), FDateTime::UtcNow().ToIso8601());
	if (Payload.IsValid())
	{
		Body->SetObjectField(TEXT("payload"), Payload);
	}
	DispatchHttpPost(TEXT("/api/v1/telemetry/events"), Body, KismetAITelemetryInternal::CategoryForEventType(Type));
}

void KismetAITelemetry::ReportCrash(TSharedPtr<FJsonObject> CrashData)
{
	if (!GInitialized) return;
	if (!CrashData.IsValid())
	{
		CrashData = MakeShared<FJsonObject>();
	}
	DispatchHttpPost(TEXT("/api/v1/telemetry/crashes"), CrashData, ECategory::Crashes);
}
