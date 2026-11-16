// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
#include "KismetAIHttpTransport.h"
#include "KismetAIEngineCompat.h"

#include "HttpServerResponse.h"
#include "IPAddress.h"
#include "PlatformHttp.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "HAL/PlatformTime.h"
#include "Templates/UnrealTemplate.h"

DEFINE_LOG_CATEGORY_STATIC(LogKismetAIHttpTransport, Log, All);

namespace KismetAIHttpTransportPrivate
{
	// A 1 MiB request needs 16 ticks: about 5.3 s even at the editor's 3 Hz background rate.
	constexpr int32 ReadBytesPerTick = 64 * 1024;
	constexpr int32 WriteBytesPerTick = 64 * 1024;
	constexpr int32 AcceptsPerTick = 8;

	void CloseSocket(FSocket*& Socket)
	{
		if (Socket)
		{
			Socket->Close();
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
			Socket = nullptr;
		}
	}

	bool IsTokenChar(TCHAR Char)
	{
		return (Char >= 'a' && Char <= 'z') || (Char >= 'A' && Char <= 'Z') ||
			(Char >= '0' && Char <= '9') || FString(TEXT("!#$%&'*+-.^_`|~")).Contains(FString::Chr(Char));
	}

	TArray<uint8> Utf8Bytes(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}
}

FKismetAIHttpRequestParser::EResult FKismetAIHttpRequestParser::Reject(int32 Status, const TCHAR* Code)
{
	ErrorStatus = Status;
	ErrorCode = Code;
	Result = EResult::Rejected;
	return Result;
}

FKismetAIHttpRequestParser::EResult FKismetAIHttpRequestParser::Append(const uint8* Data, int32 Num)
{
	if (Result == EResult::Rejected) return Result;
	if (Num < 0 || (Num > 0 && !Data)) return Reject(400, TEXT("invalid_bytes"));
	if (Result == EResult::Complete)
	{
		return Num == 0 ? Result : Reject(400, TEXT("unexpected_body_bytes"));
	}
	if (!Request && HeaderBytes.IsEmpty()) HeaderBytes.Reserve(MaxHeaderBytes);
	int32 Offset = 0;
	while (!Request && Offset < Num)
	{
		if (HeaderBytes.Num() == MaxHeaderBytes) return Reject(431, TEXT("headers_too_large"));
		const uint8 Byte = Data[Offset++];
		if (Byte == 0 || Byte > 126 || (Byte < 32 && Byte != '\r' && Byte != '\n' && Byte != '\t'))
		{
			return Reject(400, TEXT("malformed_headers"));
		}
		HeaderBytes.Add(Byte);
		const int32 Size = HeaderBytes.Num();
		if (Size >= 4 && HeaderBytes[Size - 4] == '\r' && HeaderBytes[Size - 3] == '\n' &&
			HeaderBytes[Size - 2] == '\r' && HeaderBytes[Size - 1] == '\n')
		{
			if (!ParseHeaders()) return Result;
			HeaderBytes.Empty();
		}
	}
	if (Request)
	{
		const int32 Remaining = Num - Offset;
		if (Remaining > ContentLength - Request->Body.Num()) return Reject(400, TEXT("unexpected_body_bytes"));
		if (Remaining > 0) Request->Body.Append(Data + Offset, Remaining);
		if (Request->Body.Num() == ContentLength) Result = EResult::Complete;
	}
	return Result;
}

bool FKismetAIHttpRequestParser::ParseHeaders()
{
	using namespace KismetAIHttpTransportPrivate;
	const FUTF8ToTCHAR Utf8(reinterpret_cast<const ANSICHAR*>(HeaderBytes.GetData()), HeaderBytes.Num() - 4);
	const FString Text(Utf8.Length(), Utf8.Get());
	TArray<FString> Lines;
	Text.ParseIntoArray(Lines, TEXT("\r\n"), false);
	auto Fail = [this](int32 Status, const TCHAR* Code) { Reject(Status, Code); return false; };
	if (Lines.IsEmpty()) return Fail(400, TEXT("malformed_request_line"));
	TArray<FString> Parts;
	Lines[0].ParseIntoArray(Parts, TEXT(" "), false);
	if (Parts.Num() != 3 || Lines[0].Contains(TEXT("\r")) || Lines[0].Contains(TEXT("\n")) || Lines[0].Contains(TEXT("\t")) ||
		!Parts[1].StartsWith(TEXT("/")) || Parts[1].Contains(TEXT("#")))
	{
		return Fail(400, TEXT("malformed_request_line"));
	}
	TSharedPtr<FHttpServerRequest> Parsed = MakeShared<FHttpServerRequest>();
	if (Parts[2].Equals(TEXT("HTTP/1.1"), ESearchCase::CaseSensitive)) Parsed->HttpVersion = HttpVersion::EHttpServerHttpVersion::HTTP_VERSION_1_1;
	else if (Parts[2].Equals(TEXT("HTTP/1.0"), ESearchCase::CaseSensitive)) Parsed->HttpVersion = HttpVersion::EHttpServerHttpVersion::HTTP_VERSION_1_0;
	else return Fail(505, TEXT("unsupported_http_version"));

	const TPair<const TCHAR*, EHttpServerRequestVerbs> Methods[] = {
		{TEXT("GET"), EHttpServerRequestVerbs::VERB_GET}, {TEXT("POST"), EHttpServerRequestVerbs::VERB_POST},
		{TEXT("PUT"), EHttpServerRequestVerbs::VERB_PUT}, {TEXT("PATCH"), EHttpServerRequestVerbs::VERB_PATCH},
		{TEXT("DELETE"), EHttpServerRequestVerbs::VERB_DELETE}, {TEXT("HEAD"), KismetAIHttpCompat::VerbHead},
		{TEXT("OPTIONS"), EHttpServerRequestVerbs::VERB_OPTIONS}, {TEXT("TRACE"), KismetAIHttpCompat::VerbTrace},
		{TEXT("CONNECT"), KismetAIHttpCompat::VerbConnect}};
	for (const auto& Method : Methods)
	{
		if (Parts[0].Equals(Method.Key, ESearchCase::CaseSensitive)) Parsed->Verb = Method.Value;
	}
	if (Parsed->Verb == EHttpServerRequestVerbs::VERB_NONE) return Fail(405, TEXT("unsupported_method"));

	bool bHasLength = false;
	for (int32 Index = 1; Index < Lines.Num(); ++Index)
	{
		const FString& Line = Lines[Index];
		int32 Colon = INDEX_NONE;
		if (!Line.FindChar(':', Colon) || Colon == 0) return Fail(400, TEXT("malformed_header"));
		const FString Key = Line.Left(Colon).ToLower();
		for (TCHAR Char : Key)
		{
			if (!IsTokenChar(Char)) return Fail(400, TEXT("malformed_header_name"));
		}
		FString Value = Line.Mid(Colon + 1).TrimStartAndEnd();
		if (Value.Contains(TEXT("\r")) || Value.Contains(TEXT("\n"))) return Fail(400, TEXT("malformed_header_value"));
		if (Key == TEXT("transfer-encoding")) return Fail(400, TEXT("transfer_encoding_unsupported"));
		if (Key == TEXT("expect")) return Fail(417, TEXT("expectation_unsupported"));
		if (Key == TEXT("content-length"))
		{
			if (bHasLength || Value.IsEmpty()) return Fail(400, TEXT("invalid_content_length"));
			bHasLength = true;
			int64 Length = 0;
			for (TCHAR Char : Value)
			{
				if (Char < '0' || Char > '9') return Fail(400, TEXT("invalid_content_length"));
				Length = Length * 10 + Char - '0';
				if (Length > MaxBodyBytes) return Fail(413, TEXT("body_too_large"));
			}
			ContentLength = static_cast<int32>(Length);
		}
		Parsed->Headers.FindOrAdd(Key).Add(MoveTemp(Value));
	}
	if (ContentLength > 0 && (Parsed->Verb == EHttpServerRequestVerbs::VERB_GET || Parsed->Verb == KismetAIHttpCompat::VerbHead))
	{
		return Fail(400, TEXT("unexpected_content_length"));
	}
	FString Path = Parts[1];
	int32 Question = INDEX_NONE;
	if (Path.FindChar('?', Question))
	{
		TArray<FString> Pairs;
		Path.Mid(Question + 1).ParseIntoArray(Pairs, TEXT("&"), true);
		Path.LeftInline(Question);
		for (const FString& Pair : Pairs)
		{
			FString Key, Value;
			if (Pair.Split(TEXT("="), &Key, &Value))
			{
				Parsed->QueryParams.Add(FGenericPlatformHttp::UrlDecode(Key), FGenericPlatformHttp::UrlDecode(Value));
			}
		}
	}
	KismetAIHttpCompat::SetRequestPath(*Parsed, Path);
	Parsed->Body.Reserve(ContentLength);
	Request = MoveTemp(Parsed);
	return true;
}

struct FKismetAIHttpTransport::FConnection
{
	FSocket* Socket = nullptr;
	TSharedPtr<FInternetAddr> Peer;
	FKismetAIHttpRequestParser Parser;
	double AcceptedAt = FPlatformTime::Seconds();
	double LastReadAt = AcceptedAt;
	double WriteBlockedSince = 0;
	bool bDispatched = false;
	TUniquePtr<FHttpServerResponse> Response;
	// The chunked body of a streamed response; empty for an ordinary one.
	KismetAIHttpCompat::FStream Stream;
	TArray<uint8> Output;
	int32 OutputOffset = 0;
	bool bInitialBodyQueued = false;

	~FConnection() { Close(); }
	void Close()
	{
		KismetAIHttpTransportPrivate::CloseSocket(Socket);
		Response.Reset();
		Stream = KismetAIHttpCompat::FStream();
		Output.Empty();
		Parser = FKismetAIHttpRequestParser();
	}
	void Respond(TUniquePtr<FHttpServerResponse>&& InResponse)
	{
		// Take the stream first: a response refused below must not leave its stream parked.
		KismetAIHttpCompat::FStream InStream = InResponse ? KismetAIHttpCompat::TakeStream(*InResponse) : KismetAIHttpCompat::FStream();
		// All route callbacks use the game-thread dispatch contract. Fail closed on misuse.
		if (!ensure(IsInGameThread()) || !Socket || Response) return;
		Response = MoveTemp(InResponse);
		if (!Response) { Close(); return; }
		Stream = MoveTemp(InStream);
		bDispatched = true;
		Parser = FKismetAIHttpRequestParser();
		FString Headers = FString::Printf(TEXT("HTTP/1.1 %d Response\r\nConnection: close\r\n"), static_cast<int32>(Response->Code));
		const bool bStreaming = Stream.IsValid();
		if (!bStreaming) Headers += FString::Printf(TEXT("Content-Length: %d\r\n"), Response->Body.Num());
		for (const auto& Header : Response->Headers)
		{
			if (Header.Key.Equals(TEXT("connection"), ESearchCase::IgnoreCase) ||
				Header.Key.Equals(TEXT("content-length"), ESearchCase::IgnoreCase) ||
				Header.Key.Equals(TEXT("transfer-encoding"), ESearchCase::IgnoreCase)) continue;
			for (const FString& Value : Header.Value) Headers += Header.Key + TEXT(": ") + Value + TEXT("\r\n");
		}
		Headers += TEXT("\r\n");
		Output = KismetAIHttpTransportPrivate::Utf8Bytes(Headers);
		OutputOffset = 0;
	}
	void Reject(int32 Status, const FString& Code)
	{
		Respond(FHttpServerResponse::Error(static_cast<EHttpServerResponseCodes>(Status), Code, Code));
	}
	void Write(double Now)
	{
		using namespace KismetAIHttpTransportPrivate;
		int32 Budget = WriteBytesPerTick;
		while (Socket && Response && Budget > 0)
		{
			if (OutputOffset == Output.Num())
			{
				Output.Empty();
				OutputOffset = 0;
				if (!bInitialBodyQueued)
				{
					Output = MoveTemp(Response->Body);
					bInitialBodyQueued = true;
				}
				if (Output.IsEmpty() && Stream.Queue)
				{
					Stream.Queue->Dequeue(Output);
				}
				if (Output.IsEmpty())
				{
					WriteBlockedSince = 0;
					if (!Stream.Queue || Stream.IsComplete())
					{
						// Complete is published after the last enqueue; check the queue again after observing it.
						if (!Stream.Queue || !Stream.Queue->Dequeue(Output)) Close();
					}
					if (Output.IsEmpty()) return;
				}
			}
			if (WriteBlockedSince == 0) WriteBlockedSince = Now;
			int32 Sent = 0;
			const bool bSent = Socket->Send(Output.GetData() + OutputOffset, FMath::Min(Budget, Output.Num() - OutputOffset), Sent);
			if (!bSent && ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode() != SE_EWOULDBLOCK)
			{
				Close(); return;
			}
			if (Sent > 0)
			{
				OutputOffset += Sent;
				Budget -= Sent;
				WriteBlockedSince = Now;
			}
			else
			{
				if (Now - WriteBlockedSince >= ReadIdleSeconds) Close();
				return;
			}
		}
	}
};

FKismetAIHttpTransport::~FKismetAIHttpTransport() { Stop(); }

bool FKismetAIHttpTransport::IsLoopbackPeer(const FInternetAddr& Address)
{
	const TArray<uint8> Bytes = Address.GetRawIp();
	if (Bytes.Num() == 4) return Bytes[0] == 127;
	if (Bytes.Num() != 16) return false;
	bool bFirstTenZero = true;
	for (int32 Index = 0; Index < 10; ++Index) bFirstTenZero &= Bytes[Index] == 0;
	if (!bFirstTenZero) return false;
	if (Bytes[10] == 255 && Bytes[11] == 255) return Bytes[12] == 127;
	for (int32 Index = 10; Index < 15; ++Index) if (Bytes[Index] != 0) return false;
	return Bytes[15] == 1;
}

bool FKismetAIHttpTransport::Start(uint32 Port, const FString& BindAddress)
{
	check(IsInGameThread());
	if (Listener || Port == 0 || Port > 65535) return false;
	ISocketSubsystem* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!Subsystem) return false;
	TSharedRef<FInternetAddr> Address = Subsystem->CreateInternetAddr();
	bool bValid = false;
	Address->SetIp(*BindAddress, bValid);
	if (!bValid) return false;
	Address->SetPort(Port);
	Listener = Subsystem->CreateSocket(NAME_Stream, TEXT("KismetBoundedHttp"), Address->GetProtocolType());
	if (!Listener || !Listener->SetNonBlocking(true) || !Listener->Bind(*Address) || !Listener->Listen(MaxConnections))
	{
		UE_LOG(LogKismetAIHttpTransport, Error, TEXT("HttpListener unable to bind to %s:%u"), *BindAddress, Port);
		KismetAIHttpTransportPrivate::CloseSocket(Listener);
		return false;
	}
	const TWeakPtr<FKismetAIHttpTransport> Weak = AsShared();
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak](float Delta)
	{
		const auto Pinned = Weak.Pin();
		return Pinned && Pinned->Tick(Delta);
	}));
	return true;
}

void FKismetAIHttpTransport::Stop()
{
	check(IsInGameThread());
	if (TickHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	TickHandle.Reset();
	KismetAIHttpTransportPrivate::CloseSocket(Listener);
	for (const auto& Connection : Connections) Connection->Close();
	Connections.Empty();
	Routes.Empty();
}

FHttpRouteHandle FKismetAIHttpTransport::BindRoute(const FHttpPath& Path, const EHttpServerRequestVerbs& Verbs, const FHttpRequestHandler& Handler)
{
	check(IsInGameThread());
	if (!Path.IsValidPath() || Verbs == EHttpServerRequestVerbs::VERB_NONE || !Handler.IsBound()) return nullptr;
	for (const auto& Route : Routes)
	{
		if (Route->Path.Equals(Path.GetPath(), ESearchCase::CaseSensitive) && EnumHasAnyFlags(Route->Verbs, Verbs)) return nullptr;
	}
	FHttpRouteHandle Handle = MakeShared<FHttpRouteHandleInternal>(Path.GetPath(), Verbs, Handler);
	Routes.Add(Handle);
	Routes.Sort([](const FHttpRouteHandle& Left, const FHttpRouteHandle& Right) { return Left->Path.Len() > Right->Path.Len(); });
	return Handle;
}

void FKismetAIHttpTransport::UnbindRoute(const FHttpRouteHandle& Handle) { Routes.Remove(Handle); }

bool FKismetAIHttpTransport::Query(const TSharedPtr<FHttpServerRequest>& Request, const FHttpResultCallback& OnComplete)
{
	check(IsInGameThread());
	const FString Path = KismetAIHttpCompat::GetRequestPath(*Request);
	const TArray<FHttpRouteHandle> Snapshot = Routes;
	for (const auto& Route : Snapshot)
	{
		// A previous handler can stop the server or unbind a fallback while dispatch is active.
		if (!Routes.Contains(Route) || !EnumHasAnyFlags(Route->Verbs, Request->Verb)) continue;
		if (!Path.Equals(Route->Path, ESearchCase::CaseSensitive) && Route->Path != TEXT("/") &&
			!Path.StartsWith(Route->Path + TEXT("/"), ESearchCase::CaseSensitive)) continue;
		Request->RelativePath.SetPath(Path);
		Request->RelativePath.MakeRelative(Route->Path);
		if (Route->Handler.Execute(*Request, OnComplete)) return true;
	}
	Request->RelativePath.SetPath(Path);
	return false;
}

void FKismetAIHttpTransport::AcceptConnections(double Now)
{
	using namespace KismetAIHttpTransportPrivate;
	ISocketSubsystem* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	for (int32 Index = 0; Index < AcceptsPerTick && Listener; ++Index)
	{
		bool bPending = false;
		if (!Listener->HasPendingConnection(bPending) || !bPending) break;
		FSocket* Socket = Listener->Accept(TEXT("KismetHttpPeer"));
		if (!Socket) break;
		TSharedRef<FInternetAddr> Peer = Subsystem->CreateInternetAddr();
		if (!Socket->SetNonBlocking(true) || !Socket->GetPeerAddress(*Peer) || !IsLoopbackPeer(*Peer))
		{
			CloseSocket(Socket); continue;
		}
		if (Connections.Num() >= MaxConnections)
		{
			// Reclaim the oldest incomplete reader, never an executing request or an SSE response.
			const int32 Oldest = Connections.IndexOfByPredicate([](const auto& Connection) { return !Connection->bDispatched; });
			if (Oldest == INDEX_NONE) { CloseSocket(Socket); continue; }
			Connections[Oldest]->Close();
			Connections.RemoveAt(Oldest);
		}
		int32 Actual = 0;
		Socket->SetReceiveBufferSize(ReadBytesPerTick, Actual);
		Socket->SetSendBufferSize(WriteBytesPerTick, Actual);
		TSharedPtr<FConnection> Connection = MakeShared<FConnection>();
		Connection->Socket = Socket;
		Connection->Peer = Peer;
		Connection->AcceptedAt = Connection->LastReadAt = Now;
		Connections.Add(MoveTemp(Connection));
	}
}

bool FKismetAIHttpTransport::Tick(float DeltaTime)
{
	using namespace KismetAIHttpTransportPrivate;
	check(IsInGameThread());
	if (!Listener) return false;
	if (bInTick) return true;
	TGuardValue<bool> Guard(bInTick, true);
	const double Now = FPlatformTime::Seconds();
	Connections.RemoveAll([](const auto& Connection) { return !Connection->Socket; });
	AcceptConnections(Now);
	const TArray<TSharedPtr<FConnection>> Snapshot = Connections;
	for (const auto& Connection : Snapshot)
	{
		if (!Connection->Socket) continue;
		if (!Connection->bDispatched)
		{
			if (Now - Connection->LastReadAt >= ReadIdleSeconds || Now - Connection->AcceptedAt >= ReadDeadlineSeconds)
			{
				Connection->Reject(408, TEXT("request_read_timeout"));
			}
			else
			{
				uint8 Bytes[ReadBytesPerTick];
				int32 Read = 0;
				const bool bRead = Connection->Socket->Recv(Bytes, sizeof(Bytes), Read);
				if (!bRead)
				{
					Connection->Close(); continue;
				}
				if (Read > 0)
				{
					Connection->LastReadAt = Now;
					const auto Result = Connection->Parser.Append(Bytes, Read);
					if (Result == FKismetAIHttpRequestParser::EResult::Rejected)
					{
						Connection->Reject(Connection->Parser.GetErrorStatus(), Connection->Parser.GetErrorCode());
					}
					else if (Result == FKismetAIHttpRequestParser::EResult::Complete)
					{
						const auto Request = Connection->Parser.GetRequest();
						Request->PeerAddress = Connection->Peer;
						Connection->bDispatched = true;
						Connection->Parser = FKismetAIHttpRequestParser();
						const TWeakPtr<FConnection> WeakConnection = Connection;
						const FHttpResultCallback Callback = [WeakConnection](TUniquePtr<FHttpServerResponse>&& Response)
						{
							if (const auto Pinned = WeakConnection.Pin()) Pinned->Respond(MoveTemp(Response));
						};
						if (!Query(Request, Callback)) Connection->Reject(404, TEXT("not_found"));
					}
				}
			}
		}
		if (Connection->Socket && Connection->Response) Connection->Write(Now);
	}
	return Listener != nullptr;
}
