// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIAuth.h"
#include "KismetAIPaths.h"
#include "KismetAITelemetry.h"

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include <sys/stat.h>
#include <errno.h>
#endif

DEFINE_LOG_CATEGORY_STATIC(LogKismetAIAuth, Log, All);

namespace
{
	const TCHAR* DefaultBackendUrl = TEXT("https://kismetai.io");
	const TCHAR* DevelopmentBackendUrl = TEXT("https://dev.kismetai.io");
	// Production / legacy session slot. Kept as the bare "credentials.json" name
	// so existing installs (which are overwhelmingly on production) are not
	// logged out by the introduction of per-backend scoping below.
	const TCHAR* CredentialsFileName = TEXT("credentials.json");
	const TCHAR* MachineIdFileName = TEXT("machine_id");

	// Host portion of the active backend, lowercased, scheme/path stripped.
	// e.g. "https://dev.kismetai.io" -> "dev.kismetai.io".
	FString BackendHostSlug()
	{
		FString Host = KismetAIAuth::GetBackendUrl();
		Host.RemoveFromStart(TEXT("https://"));
		Host.RemoveFromStart(TEXT("http://"));
		int32 SlashIdx = INDEX_NONE;
		if (Host.FindChar(TEXT('/'), SlashIdx))
		{
			Host.LeftInline(SlashIdx);
		}
		Host.TrimStartAndEndInline();
		Host.ToLowerInline();
		return Host;
	}

	// Per-backend credentials path. A bearer token is only valid against the
	// backend that issued it: a dev token returns 401 from production (and vice
	// versa), and ValidateSession's 401 path then wipes the local session. By
	// scoping the file to the backend host, a dev and a production session can
	// coexist on disk — so switching backends, an update that resolves a
	// different effective backend, or a startup-order race where GetBackendUrl()
	// briefly returns the production default before DefaultKismetAI.ini loads no
	// longer destroys the session for the env you actually logged into. The
	// wrong-backend 401 only clears that backend's (empty/stale) slot.
	//
	// Production keeps the historical "credentials.json" filename; every other
	// backend gets "credentials-<host>.json".
	FString CredentialsPath()
	{
		const FString Dir = KismetAIPaths::GetConfigDir();
		const FString Host = BackendHostSlug();
		if (Host.IsEmpty() || Host == TEXT("kismetai.io"))
		{
			return Dir / CredentialsFileName;
		}

		// Sanitize the host for use as a filename component.
		FString Safe;
		Safe.Reserve(Host.Len());
		for (const TCHAR C : Host)
		{
			Safe.AppendChar((FChar::IsAlnum(C) || C == TEXT('.') || C == TEXT('-')) ? C : TEXT('-'));
		}
		return Dir / FString::Printf(TEXT("credentials-%s.json"), *Safe);
	}

	FString MachineIdPath()
	{
		return KismetAIPaths::GetConfigDir() / MachineIdFileName;
	}

	void RestrictAuthPerms(const FString& Path)
	{
#if PLATFORM_MAC || PLATFORM_LINUX
		FTCHARToUTF8 PathUtf8(*Path);
		if (chmod(PathUtf8.Get(), S_IRUSR | S_IWUSR) != 0)
		{
			UE_LOG(LogKismetAIAuth, Warning,
				TEXT("Auth: chmod 0600 failed on %s (errno=%d) — credentials may be world-readable"),
				*Path, errno);
		}
#else
		(void)Path;
#endif
	}

	void RunOnGameThread(TFunction<void()> Fn)
	{
		if (IsInGameThread())
		{
			Fn();
		}
		else
		{
			AsyncTask(ENamedThreads::GameThread, MoveTemp(Fn));
		}
	}

	FString JoinUrl(const FString& Base, const FString& Path)
	{
		if (Base.EndsWith(TEXT("/")))
		{
			return Base + (Path.StartsWith(TEXT("/")) ? Path.Mid(1) : Path);
		}
		return Base + (Path.StartsWith(TEXT("/")) ? Path : (TEXT("/") + Path));
	}

	// Vercel deployment-protection bypass for dev.kismetai.io. The token is read
	// from KISMET_VERCEL_BYPASS at request time so it never enters source.
	void ApplyVercelBypass(const TSharedRef<IHttpRequest>& Req)
	{
		const FString Token = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMET_VERCEL_BYPASS"));
		if (!Token.IsEmpty())
		{
			Req->SetHeader(TEXT("x-vercel-protection-bypass"), Token);
		}
	}

}

namespace
{
	// Installed by the editor module at startup. See KismetAIAuth.h.
	TFunction<FString()> GBackendUrlResolver;
}

FString KismetAIAuth::GetDevelopmentBackendUrl()
{
	return DevelopmentBackendUrl;
}

void KismetAIAuth::SetBackendUrlResolver(TFunction<FString()> Resolver)
{
	GBackendUrlResolver = MoveTemp(Resolver);
}

FString KismetAIAuth::GetBackendUrl()
{
	// Env var wins over everything, as before.
	const FString Env = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMET_BACKEND_URL"));
	if (!Env.IsEmpty())
	{
		return Env;
	}

	// Then whatever the editor module resolved from UKismetAISettings. Absent a
	// resolver — tests, or a runtime with no editor — fall through to the
	// production default, which is what the settings path returns anyway when
	// Backend is Production or Custom-with-an-empty-URL.
	if (GBackendUrlResolver)
	{
		const FString Resolved = GBackendUrlResolver();
		if (!Resolved.IsEmpty())
		{
			return Resolved;
		}
	}

	return DefaultBackendUrl;
}

FString KismetAIAuth::GetMachineId()
{
	const FString Path = MachineIdPath();
	FString Existing;
	if (FFileHelper::LoadFileToString(Existing, *Path))
	{
		Existing.TrimStartAndEndInline();
		if (!Existing.IsEmpty())
		{
			return Existing;
		}
	}

	const FString NewId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
	if (FFileHelper::SaveStringToFile(NewId, *Path))
	{
		RestrictAuthPerms(Path);
	}
	else
	{
		UE_LOG(LogKismetAIAuth, Warning, TEXT("Failed to persist machine_id at %s"), *Path);
	}
	return NewId;
}

bool KismetAIAuth::HasSession()
{
	return IFileManager::Get().FileExists(*CredentialsPath());
}

bool KismetAIAuth::LoadSession(FKismetAIAuthSession& OutSession)
{
	OutSession = FKismetAIAuthSession();

	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *CredentialsPath()))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return false;
	}

	Root->TryGetStringField(TEXT("token"), OutSession.Token);
	Root->TryGetStringField(TEXT("machine_id"), OutSession.MachineId);
	Root->TryGetStringField(TEXT("user_id"), OutSession.UserId);
	Root->TryGetStringField(TEXT("email"), OutSession.Email);
	Root->TryGetStringField(TEXT("display_name"), OutSession.DisplayName);
	return OutSession.IsValid();
}

bool KismetAIAuth::SaveSession(const FKismetAIAuthSession& Session)
{
	if (!Session.IsValid())
	{
		return false;
	}

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("token"), Session.Token);
	Root->SetStringField(TEXT("machine_id"), Session.MachineId);
	Root->SetStringField(TEXT("user_id"), Session.UserId);
	Root->SetStringField(TEXT("email"), Session.Email);
	Root->SetStringField(TEXT("display_name"), Session.DisplayName);

	FString Out;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		return false;
	}

	const FString Path = CredentialsPath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
	if (!FFileHelper::SaveStringToFile(Out, *Path))
	{
		UE_LOG(LogKismetAIAuth, Warning, TEXT("Failed to write %s"), *Path);
		return false;
	}
	RestrictAuthPerms(Path);
	return true;
}

bool KismetAIAuth::ClearSession()
{
	const FString Path = CredentialsPath();
	if (!IFileManager::Get().FileExists(*Path))
	{
		return true;
	}
	return IFileManager::Get().Delete(*Path, /*RequireExists=*/false, /*EvenReadOnly=*/true);
}

// ─────────────────────────────────────────────────────────────────────────────
// HTTP — GET /auth/plugin/me
// ─────────────────────────────────────────────────────────────────────────────

void KismetAIAuth::ValidateSession(FValidateCallback OnComplete)
{
	FKismetAIAuthSession Session;
	if (!LoadSession(Session))
	{
		// No local session — invalid, no HTTP call.
		OnComplete(EValidateResult::Invalid, FKismetAIAuthSession{});
		return;
	}

	const FString Url = JoinUrl(GetBackendUrl(), TEXT("/auth/plugin/me"));

	const TSharedRef<IHttpRequest> Req = FHttpModule::Get().CreateRequest();
	Req->SetVerb(TEXT("GET"));
	Req->SetURL(Url);
	Req->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Session.Token));
	Req->SetHeader(TEXT("Accept"), TEXT("application/json"));
	ApplyVercelBypass(Req);

	Req->OnProcessRequestComplete().BindLambda(
		[OnComplete = MoveTemp(OnComplete), Session](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
		{
			if (!bOk || !Resp.IsValid())
			{
				UE_LOG(LogKismetAIAuth, Warning, TEXT("ValidateSession: network error."));
				RunOnGameThread([OnComplete, Session]() { OnComplete(EValidateResult::Transient, Session); });
				return;
			}

			const int32 HttpCode = Resp->GetResponseCode();

			if (HttpCode == 401)
			{
				UE_LOG(LogKismetAIAuth, Log, TEXT("ValidateSession: 401 — clearing session."));
				KismetAIAuth::ClearSession();
				RunOnGameThread([OnComplete]() { OnComplete(EValidateResult::Invalid, FKismetAIAuthSession{}); });
				return;
			}

			if (HttpCode < 200 || HttpCode >= 300)
			{
				UE_LOG(LogKismetAIAuth, Warning, TEXT("ValidateSession: HTTP %d — transient failure."), HttpCode);
				RunOnGameThread([OnComplete, Session]() { OnComplete(EValidateResult::Transient, Session); });
				return;
			}

			// 200 — parse { user: { id, email, display_name, role } }
			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp->GetContentAsString());
			if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
			{
				UE_LOG(LogKismetAIAuth, Warning, TEXT("ValidateSession: malformed JSON response — transient failure."));
				RunOnGameThread([OnComplete, Session]() { OnComplete(EValidateResult::Transient, Session); });
				return;
			}

			const TSharedPtr<FJsonObject>* UserObj = nullptr;
			if (!Root->TryGetObjectField(TEXT("user"), UserObj) || !UserObj || !UserObj->IsValid())
			{
				UE_LOG(LogKismetAIAuth, Warning, TEXT("ValidateSession: response missing 'user' object — transient failure."));
				RunOnGameThread([OnComplete, Session]() { OnComplete(EValidateResult::Transient, Session); });
				return;
			}

			FString UserId;
			if (!(*UserObj)->TryGetStringField(TEXT("id"), UserId) || UserId.IsEmpty())
			{
				UE_LOG(LogKismetAIAuth, Warning, TEXT("ValidateSession: response missing user.id — transient failure."));
				RunOnGameThread([OnComplete, Session]() { OnComplete(EValidateResult::Transient, Session); });
				return;
			}

			FKismetAIAuthSession Refreshed;
			Refreshed.Token      = Session.Token;
			Refreshed.MachineId  = Session.MachineId;
			Refreshed.UserId     = UserId;

			FString Email;
			(*UserObj)->TryGetStringField(TEXT("email"), Email);
			Refreshed.Email = Email;

			// display_name may be null/missing — empty string is the correct signal
			// for the UI (do NOT fall back to email).
			FString DisplayName;
			(*UserObj)->TryGetStringField(TEXT("display_name"), DisplayName);
			Refreshed.DisplayName = DisplayName;

			// Mirror per-category telemetry prefs from the response. Missing
			// fields default to true to match the profiles-table schema, so an
			// older server (or a transient parse miss) behaves like prior
			// versions instead of going dark.
			bool bRuns = true, bToolCalls = true, bErrors = true, bCrashes = true;
			const TSharedPtr<FJsonObject>* TelObj = nullptr;
			if (Root->TryGetObjectField(TEXT("telemetry"), TelObj) && TelObj && TelObj->IsValid())
			{
				(*TelObj)->TryGetBoolField(TEXT("runs"),       bRuns);
				(*TelObj)->TryGetBoolField(TEXT("tool_calls"), bToolCalls);
				(*TelObj)->TryGetBoolField(TEXT("errors"),     bErrors);
				(*TelObj)->TryGetBoolField(TEXT("crashes"),    bCrashes);

				// consented_at is a timestamp string when the user has
				// acknowledged the telemetry notice, null/absent otherwise.
				// A successful 200 is authoritative, so we mark consent known
				// either way — an absent/null field means "known: not consented".
				FString ConsentedAt;
				const bool bConsented =
					(*TelObj)->TryGetStringField(TEXT("consented_at"), ConsentedAt) && !ConsentedAt.IsEmpty();
				KismetAITelemetry::UpdateConsent(bConsented);
			}
			KismetAITelemetry::UpdatePrefs(bRuns, bToolCalls, bErrors, bCrashes);

			RunOnGameThread([OnComplete, Refreshed]() { OnComplete(EValidateResult::Valid, Refreshed); });
		});

	Req->ProcessRequest();
}

bool KismetAIAuth::Logout()
{
	return ClearSession();
}

// ─────────────────────────────────────────────────────────────────────────────
// HTTP — POST /auth/plugin/telemetry/consent
// ─────────────────────────────────────────────────────────────────────────────

void KismetAIAuth::AcceptTelemetryConsent(FConsentCallback OnComplete)
{
	FKismetAIAuthSession Session;
	if (!LoadSession(Session) || Session.Token.IsEmpty())
	{
		// No bearer → nothing to consent on behalf of. Game thread already.
		OnComplete(false);
		return;
	}

	const FString Url = JoinUrl(GetBackendUrl(), TEXT("/auth/plugin/telemetry/consent"));

	const TSharedRef<IHttpRequest> Req = FHttpModule::Get().CreateRequest();
	Req->SetVerb(TEXT("POST"));
	Req->SetURL(Url);
	Req->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Session.Token));
	Req->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	ApplyVercelBypass(Req);

	Req->OnProcessRequestComplete().BindLambda(
		[OnComplete = MoveTemp(OnComplete)](FHttpRequestPtr, FHttpResponsePtr Resp, bool bNetOk)
		{
			const int32 HttpCode = (bNetOk && Resp.IsValid()) ? Resp->GetResponseCode() : 0;
			const bool bSuccess = (HttpCode >= 200 && HttpCode < 300);

			if (bSuccess)
			{
				// Mirror locally so the UI gate clears immediately, without
				// waiting for the next /auth/plugin/me round-trip.
				KismetAITelemetry::UpdateConsent(true);
			}
			else
			{
				UE_LOG(LogKismetAIAuth, Warning,
					TEXT("AcceptTelemetryConsent: failed (net_ok=%d http=%d)"), bNetOk ? 1 : 0, HttpCode);
			}

			RunOnGameThread([OnComplete, bSuccess]() { OnComplete(bSuccess); });
		});

	Req->ProcessRequest();
}

// ─────────────────────────────────────────────────────────────────────────────
// HTTP — POST /auth/plugin/start
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	void FillMachineHint(const TSharedRef<FJsonObject>& Hint)
	{
		Hint->SetStringField(TEXT("machine_id"), KismetAIAuth::GetMachineId());

#if PLATFORM_MAC
		Hint->SetStringField(TEXT("os"), TEXT("mac"));
#elif PLATFORM_WINDOWS
		Hint->SetStringField(TEXT("os"), TEXT("windows"));
#elif PLATFORM_LINUX
		Hint->SetStringField(TEXT("os"), TEXT("linux"));
#endif

		Hint->SetStringField(TEXT("hostname"), FPlatformProcess::ComputerName());

		FString PluginVersion;
		if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("KismetAI")))
		{
			PluginVersion = Plugin->GetDescriptor().VersionName;
		}
		Hint->SetStringField(TEXT("plugin_version"), PluginVersion);
	}
}

void KismetAIAuth::BeginDeviceLogin(FStartCallback OnComplete)
{
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	const TSharedRef<FJsonObject> Hint = MakeShared<FJsonObject>();
	FillMachineHint(Hint);
	Body->SetObjectField(TEXT("machine_hint"), Hint);

	FString BodyStr;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
	FJsonSerializer::Serialize(Body, Writer);

	const FString Url = JoinUrl(GetBackendUrl(), TEXT("/auth/plugin/start"));

	const TSharedRef<IHttpRequest> Req = FHttpModule::Get().CreateRequest();
	Req->SetVerb(TEXT("POST"));
	Req->SetURL(Url);
	Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Req->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Req->SetContentAsString(BodyStr);
	ApplyVercelBypass(Req);

	Req->OnProcessRequestComplete().BindLambda(
		[OnComplete = MoveTemp(OnComplete)](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
		{
			FStartResult Result;
			if (!bOk || !Resp.IsValid())
			{
				Result.ErrorMessage = TEXT("Network error contacting Kismet backend.");
				RunOnGameThread([OnComplete, Result]() { OnComplete(Result); });
				return;
			}
			const int32 Code = Resp->GetResponseCode();
			if (Code < 200 || Code >= 300)
			{
				Result.ErrorMessage = FString::Printf(TEXT("Backend HTTP %d."), Code);
				RunOnGameThread([OnComplete, Result]() { OnComplete(Result); });
				return;
			}

			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp->GetContentAsString());
			if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
			{
				Result.ErrorMessage = TEXT("Malformed response from backend.");
				RunOnGameThread([OnComplete, Result]() { OnComplete(Result); });
				return;
			}

			Root->TryGetStringField(TEXT("code"), Result.Code);
			Root->TryGetStringField(TEXT("confirm_number"), Result.ConfirmNumber);
			Root->TryGetStringField(TEXT("verification_url"), Result.VerificationUrl);
			Root->TryGetNumberField(TEXT("expires_in_seconds"), Result.ExpiresInSeconds);
			int32 PollInterval = 2;
			Root->TryGetNumberField(TEXT("poll_interval_seconds"), PollInterval);
			Result.PollIntervalSeconds = FMath::Max(1, PollInterval);

			if (Result.Code.IsEmpty() || Result.VerificationUrl.IsEmpty() || Result.ConfirmNumber.IsEmpty())
			{
				Result.ErrorMessage = TEXT("Backend response missing code, confirm_number, or verification_url.");
				RunOnGameThread([OnComplete, Result]() { OnComplete(Result); });
				return;
			}

			Result.bSuccess = true;
			RunOnGameThread([OnComplete, Result]() { OnComplete(Result); });
		});

	Req->ProcessRequest();
}

// ─────────────────────────────────────────────────────────────────────────────
// HTTP — POST /auth/plugin/poll
// ─────────────────────────────────────────────────────────────────────────────

void KismetAIAuth::PollOnce(const FString& Code, FPollCallback OnComplete)
{
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("code"), Code);

	FString BodyStr;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
	FJsonSerializer::Serialize(Body, Writer);

	const FString Url = JoinUrl(GetBackendUrl(), TEXT("/auth/plugin/poll"));

	const TSharedRef<IHttpRequest> Req = FHttpModule::Get().CreateRequest();
	Req->SetVerb(TEXT("POST"));
	Req->SetURL(Url);
	Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Req->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Req->SetContentAsString(BodyStr);
	ApplyVercelBypass(Req);

	Req->OnProcessRequestComplete().BindLambda(
		[OnComplete = MoveTemp(OnComplete)](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
		{
			FPollResult Result;

			if (!bOk || !Resp.IsValid())
			{
				Result.Status = EPollStatus::Error;
				Result.ErrorMessage = TEXT("Network error.");
				RunOnGameThread([OnComplete, Result]() { OnComplete(Result); });
				return;
			}

			const int32 Code = Resp->GetResponseCode();
			if (Code == 404)
			{
				Result.Status = EPollStatus::NotFound;
				RunOnGameThread([OnComplete, Result]() { OnComplete(Result); });
				return;
			}
			if (Code == 410)
			{
				Result.Status = EPollStatus::Expired;
				RunOnGameThread([OnComplete, Result]() { OnComplete(Result); });
				return;
			}
			if (Code < 200 || Code >= 300)
			{
				Result.Status = EPollStatus::Error;
				Result.ErrorMessage = FString::Printf(TEXT("Backend HTTP %d."), Code);
				RunOnGameThread([OnComplete, Result]() { OnComplete(Result); });
				return;
			}

			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp->GetContentAsString());
			if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
			{
				Result.Status = EPollStatus::Error;
				Result.ErrorMessage = TEXT("Malformed response.");
				RunOnGameThread([OnComplete, Result]() { OnComplete(Result); });
				return;
			}

			FString Status;
			Root->TryGetStringField(TEXT("status"), Status);
			if (Status == TEXT("pending"))
			{
				Result.Status = EPollStatus::Pending;
				RunOnGameThread([OnComplete, Result]() { OnComplete(Result); });
				return;
			}
			if (Status != TEXT("claimed"))
			{
				Result.Status = EPollStatus::Error;
				Result.ErrorMessage = FString::Printf(TEXT("Unexpected status: %s"), *Status);
				RunOnGameThread([OnComplete, Result]() { OnComplete(Result); });
				return;
			}

			FString Token;
			Root->TryGetStringField(TEXT("token"), Token);
			if (Token.IsEmpty())
			{
				Result.Status = EPollStatus::Error;
				Result.ErrorMessage = TEXT("Claimed response missing token.");
				RunOnGameThread([OnComplete, Result]() { OnComplete(Result); });
				return;
			}

			Result.Status = EPollStatus::Claimed;
			Result.Session.Token = Token;
			Result.Session.MachineId = GetMachineId();

			const TSharedPtr<FJsonObject>* UserObj = nullptr;
			if (Root->TryGetObjectField(TEXT("user"), UserObj) && UserObj && UserObj->IsValid())
			{
				(*UserObj)->TryGetStringField(TEXT("id"), Result.Session.UserId);
				(*UserObj)->TryGetStringField(TEXT("email"), Result.Session.Email);
				(*UserObj)->TryGetStringField(TEXT("display_name"), Result.Session.DisplayName);
			}

			RunOnGameThread([OnComplete, Result]() { OnComplete(Result); });
		});

	Req->ProcessRequest();
}
