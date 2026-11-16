// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIServerAuth.h"
#include "KismetAIModule.h"

#include "HttpServerRequest.h"
#include "HttpServerResponse.h"

#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include <sys/stat.h>
#include <errno.h>
#endif

DEFINE_LOG_CATEGORY_STATIC(LogKismetAIServerAuth, Log, All);

namespace
{
	FString GServerAuthToken;
	bool    GServerAuthInitialized = false;

	const TCHAR* TokenFileName = TEXT("server_token");

	FString TokenPath()
	{
		return FKismetAIModule::GetConfigDir() / TokenFileName;
	}

	void RestrictTokenPerms(const FString& Path)
	{
#if PLATFORM_MAC || PLATFORM_LINUX
		FTCHARToUTF8 PathUtf8(*Path);
		if (chmod(PathUtf8.Get(), S_IRUSR | S_IWUSR) != 0)
		{
			UE_LOG(LogKismetAIServerAuth, Warning,
				TEXT("ServerAuth: chmod 0600 failed on %s (errno=%d) — token may be world-readable"),
				*Path, errno);
		}
#else
		(void)Path;
#endif
	}

	FString GenerateRandomTokenHex()
	{
		// 32 random bytes -> 64-char hex. FGuid uses platform RNG
		// (SecRandomCopyBytes / BCryptGenRandom / /dev/urandom).
		uint8 Bytes[32];
		for (int32 Filled = 0; Filled < 32; Filled += 16)
		{
			const FGuid G = FGuid::NewGuid();
			static_assert(sizeof(FGuid) == 16, "FGuid expected to be 16 bytes");
			FMemory::Memcpy(Bytes + Filled, &G, FMath::Min<int32>(16, 32 - Filled));
		}

		FString Hex;
		Hex.Reserve(64);
		for (int32 i = 0; i < 32; ++i)
		{
			Hex.Appendf(TEXT("%02x"), Bytes[i]);
		}
		return Hex;
	}

	bool ConstantTimeEquals(const FString& A, const FString& B)
	{
		// Length difference leaks length anyway, but reject that up-front so
		// the byte-compare loop is O(min(N,M)) rather than branching on it.
		if (A.Len() != B.Len())
		{
			return false;
		}
		uint32 Diff = 0;
		const TCHAR* PA = *A;
		const TCHAR* PB = *B;
		for (int32 i = 0; i < A.Len(); ++i)
		{
			Diff |= static_cast<uint32>(PA[i]) ^ static_cast<uint32>(PB[i]);
		}
		return Diff == 0;
	}
}

void KismetAIServerAuth::Initialize()
{
	if (GServerAuthInitialized)
	{
		return;
	}

	// 1. Honor an explicit override from the eval framework / CI runner.
	const FString EnvToken = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMETAI_SERVER_TOKEN"));
	if (!EnvToken.IsEmpty())
	{
		GServerAuthToken = EnvToken;
		GServerAuthInitialized   = true;
		UE_LOG(LogKismetAIServerAuth, Log, TEXT("Using KISMETAI_SERVER_TOKEN from environment"));
		return;
	}

	// 2. Generate fresh per-launch token and persist it (mode 0600) so local
	// tools can discover it without parsing logs.
	GServerAuthToken = GenerateRandomTokenHex();

	const FString Path = TokenPath();
	if (FFileHelper::SaveStringToFile(GServerAuthToken, *Path,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		RestrictTokenPerms(Path);
		UE_LOG(LogKismetAIServerAuth, Log, TEXT("Wrote per-launch token to %s"), *Path);
	}
	else
	{
		UE_LOG(LogKismetAIServerAuth, Warning,
			TEXT("Failed to persist server token to %s (auth will still work for in-process callers)"),
			*Path);
	}

	GServerAuthInitialized = true;
}

const FString& KismetAIServerAuth::GetToken()
{
	return GServerAuthToken;
}

FString KismetAIServerAuth::GetTokenPath()
{
	return TokenPath();
}

bool KismetAIServerAuth::CheckRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// Reject any browser-initiated request out of hand. Browsers always set
	// Origin on cross-origin POSTs and on same-origin XHR; legitimate native
	// callers (curl from CLI, the Python eval framework, the agent
	// in-process) never set it.
	if (const TArray<FString>* Origins = Request.Headers.Find(TEXT("Origin")))
	{
		for (const FString& O : *Origins)
		{
			if (!O.IsEmpty())
			{
				OnComplete(FHttpServerResponse::Error(
					EHttpServerResponseCodes::Forbidden,
					TEXT("origin_forbidden"),
					TEXT("Browser-originated requests are not allowed")));
				return false;
			}
		}
	}

	if (!GServerAuthInitialized || GServerAuthToken.IsEmpty())
	{
		OnComplete(FHttpServerResponse::Error(
			EHttpServerResponseCodes::ServerError,
			TEXT("auth_not_initialized"),
			TEXT("Server token not initialized")));
		return false;
	}

	const TArray<FString>* Hdr = Request.Headers.Find(TEXT("X-Kismet-Token"));
	if (!Hdr || Hdr->Num() == 0 || !ConstantTimeEquals((*Hdr)[0], GServerAuthToken))
	{
		OnComplete(FHttpServerResponse::Error(
			EHttpServerResponseCodes::Denied,
			TEXT("unauthorized"),
			TEXT("Missing or invalid X-Kismet-Token header")));
		return false;
	}

	return true;
}
