// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
#pragma once

// Differences between the supported engine versions (5.6 to 5.9), in one place.
//
// The plugin serves HTTP through its own transport (KismetAIHttpTransport) and
// only borrows the engine's FHttpServerRequest/FHttpServerResponse structs to
// carry data. Engine 5.8 added a full path, streaming fields and more verbs to
// those structs; earlier engines lack them, so the helpers below supply the
// same behaviour there. Everything else in the plugin goes through these
// helpers instead of touching the newer members directly.

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HttpPath.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Templates/Atomic.h"

#define KISMETAI_ENGINE_AT_LEAST_5_8 (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8))

// GetObjectsWith*/ForEachObjectWith* took a bool before EGetObjectsFlags existed.
#if KISMETAI_ENGINE_AT_LEAST_5_8
	#define KISMETAI_INCLUDE_NESTED_OBJECTS EGetObjectsFlags::IncludeNestedObjects
#else
	#define KISMETAI_INCLUDE_NESTED_OBJECTS true
#endif

namespace KismetAIHttpCompat
{
	using FByteQueue = TQueue<TArray<uint8>, EQueueMode::Spsc>;

	/** A response body written in chunks. Queue is null for an ordinary response. */
	struct FStream
	{
		TSharedPtr<FByteQueue> Queue;
		TSharedPtr<TAtomic<bool>> Complete;
		bool IsValid() const { return Queue.IsValid(); }
		bool IsComplete() const { return Complete.IsValid() && Complete->Load(); }
	};

#if KISMETAI_ENGINE_AT_LEAST_5_8
	inline constexpr EHttpServerRequestVerbs VerbHead = EHttpServerRequestVerbs::VERB_HEAD;
	inline constexpr EHttpServerRequestVerbs VerbTrace = EHttpServerRequestVerbs::VERB_TRACE;
	inline constexpr EHttpServerRequestVerbs VerbConnect = EHttpServerRequestVerbs::VERB_CONNECT;
#else
	// Not in the enum yet. Same bit values the engine later assigned; they are
	// only parsed so that such a request can be refused by route matching.
	inline constexpr EHttpServerRequestVerbs VerbHead = static_cast<EHttpServerRequestVerbs>(1 << 6);
	inline constexpr EHttpServerRequestVerbs VerbTrace = static_cast<EHttpServerRequestVerbs>(1 << 7);
	inline constexpr EHttpServerRequestVerbs VerbConnect = static_cast<EHttpServerRequestVerbs>(1 << 8);
#endif

	/** The request path without its query string. */
	FString GetRequestPath(const FHttpServerRequest& Request);
	/** Sets the path as the transport's parser does (full and relative path alike). */
	void SetRequestPath(FHttpServerRequest& Request, const FString& Path);

	/** Makes Response a chunked stream that ends when Complete is set and Queue is drained. */
	void AttachStream(FHttpServerResponse& Response, const TSharedPtr<FByteQueue>& Queue, const TSharedPtr<TAtomic<bool>>& Complete);
	/** The stream attached to Response, if any, leaving it attached. */
	FStream PeekStream(const FHttpServerResponse& Response);
	/** Hands the stream over to the caller, which then owns it. Call once, before writing headers. */
	FStream TakeStream(FHttpServerResponse& Response);
}
