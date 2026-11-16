// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
#include "KismetAIEngineCompat.h"

#include "HAL/CriticalSection.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"

namespace KismetAIHttpCompat
{
#if KISMETAI_ENGINE_AT_LEAST_5_8

	FString GetRequestPath(const FHttpServerRequest& Request) { return Request.FullPath.GetPath(); }

	void SetRequestPath(FHttpServerRequest& Request, const FString& Path)
	{
		Request.FullPath.SetPath(Path);
		Request.RelativePath.SetPath(Path);
	}

	void AttachStream(FHttpServerResponse& Response, const TSharedPtr<FByteQueue>& Queue, const TSharedPtr<TAtomic<bool>>& Complete)
	{
		// No Content-Length: the socket close ends the body, so close after the
		// final write instead of idling into keep-alive.
		Response.Flags = EHttpServerResponseFlags::MultipleWriteStream | EHttpServerResponseFlags::CloseAfterWrite;
		Response.StreamingBodyQueue = Queue;
		Response.StreamingBodyComplete = Complete;
	}

	FStream PeekStream(const FHttpServerResponse& Response)
	{
		return FStream{ Response.StreamingBodyQueue, Response.StreamingBodyComplete };
	}

	FStream TakeStream(FHttpServerResponse& Response) { return PeekStream(Response); }

#else

	// Before 5.8 the response struct has nowhere to hold a stream, and a response
	// is moved through callbacks, so its address cannot identify it. The stream
	// is parked in this table under a random token carried in a private header.
	// TakeStream removes both, so the header never reaches the client and the
	// table holds an entry only between a handler creating a streamed response
	// and the transport (or a test) receiving it.
	namespace
	{
		const TCHAR* const StreamHeader = TEXT("X-KismetAI-Internal-Stream");
		FCriticalSection StreamLock;
		TMap<FString, FStream>& Streams()
		{
			static TMap<FString, FStream> Table;
			return Table;
		}
		FString StreamToken(const FHttpServerResponse& Response)
		{
			const TArray<FString>* Values = Response.Headers.Find(StreamHeader);
			return Values && Values->Num() == 1 ? (*Values)[0] : FString();
		}
	}

	FString GetRequestPath(const FHttpServerRequest& Request) { return Request.RelativePath.GetPath(); }

	void SetRequestPath(FHttpServerRequest& Request, const FString& Path) { Request.RelativePath.SetPath(Path); }

	void AttachStream(FHttpServerResponse& Response, const TSharedPtr<FByteQueue>& Queue, const TSharedPtr<TAtomic<bool>>& Complete)
	{
		const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		{
			FScopeLock Lock(&StreamLock);
			Streams().Add(Token, FStream{ Queue, Complete });
		}
		Response.Headers.Add(StreamHeader, { Token });
	}

	FStream PeekStream(const FHttpServerResponse& Response)
	{
		const FString Token = StreamToken(Response);
		if (Token.IsEmpty()) return FStream();
		FScopeLock Lock(&StreamLock);
		const FStream* Found = Streams().Find(Token);
		return Found ? *Found : FStream();
	}

	FStream TakeStream(FHttpServerResponse& Response)
	{
		const FString Token = StreamToken(Response);
		if (Token.IsEmpty()) return FStream();
		Response.Headers.Remove(StreamHeader);
		FStream Taken;
		FScopeLock Lock(&StreamLock);
		Streams().RemoveAndCopyValue(Token, Taken);
		return Taken;
	}

#endif
}
