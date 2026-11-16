// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "HttpRequestHandler.h"
#include "HttpResultCallback.h"
#include "HttpRouteHandle.h"
#include "HttpServerRequest.h"

class FSocket;
class FInternetAddr;

/** Incremental framing gate: never allocates a declared body until its size is accepted. */
class FKismetAIHttpRequestParser
{
public:
	enum class EResult { Incomplete, Complete, Rejected };
	static constexpr int32 MaxBodyBytes = 1024 * 1024;
	static constexpr int32 MaxHeaderBytes = 16 * 1024;

	EResult Append(const uint8* Data, int32 Num);
	TSharedPtr<FHttpServerRequest> GetRequest() const { return Request; }
	int32 GetErrorStatus() const { return ErrorStatus; }
	FString GetErrorCode() const { return ErrorCode; }
	int32 GetBufferedBytes() const { return HeaderBytes.Num() + (Request ? Request->Body.Num() : 0); }

private:
	bool ParseHeaders();
	EResult Reject(int32 Status, const TCHAR* Code);
	TArray<uint8> HeaderBytes;
	TSharedPtr<FHttpServerRequest> Request;
	EResult Result = EResult::Incomplete;
	int32 ContentLength = 0;
	int32 ErrorStatus = 0;
	FString ErrorCode;
};

/** Local HTTP adapter. Owns its socket so engine global listener settings cannot bypass its limits. */
class FKismetAIHttpTransport : public TSharedFromThis<FKismetAIHttpTransport>
{
public:
	static constexpr int32 MaxConnections = 32;
	static constexpr double ReadIdleSeconds = 5.0;
	static constexpr double ReadDeadlineSeconds = 15.0;

	~FKismetAIHttpTransport();
	bool Start(uint32 Port, const FString& BindAddress = TEXT("127.0.0.1"));
	void Stop();
	FHttpRouteHandle BindRoute(const FHttpPath& Path, const EHttpServerRequestVerbs& Verbs, const FHttpRequestHandler& Handler);
	void UnbindRoute(const FHttpRouteHandle& Handle);
	bool Query(const TSharedPtr<FHttpServerRequest>& Request, const FHttpResultCallback& OnComplete);
	static bool IsLoopbackPeer(const FInternetAddr& Address);

private:
	struct FConnection;
	bool Tick(float DeltaTime);
	void AcceptConnections(double Now);
	FSocket* Listener = nullptr;
	FTSTicker::FDelegateHandle TickHandle;
	TArray<TSharedPtr<FConnection>> Connections;
	TArray<FHttpRouteHandle> Routes;
	bool bInTick = false;
};
