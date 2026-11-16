// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Typed tool outcome shared by handlers, execution and transport adapters.
// Transport success is separate from operation success: a batch may return a
// valid envelope containing failed operations. Producers report both explicitly;
// arbitrary payload text is never scanned to decide the live call's outcome.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct KISMETAIRUNTIME_API FToolExecutionResult
{
	bool bSuccess = false;
	// Set only by an owning pre-handler guard, never inferred from an error code.
	bool bHandlerNotStarted = false;
	// The operation can fail inside a successfully delivered batch/compile response.
	// Producers supply this explicitly; arbitrary payload fields are never inspected.
	bool bOperationsSucceeded = true;

	// Suggested HTTP status for the route adapter. Meaningless to the agent.
	int32 StatusCode = 200;

	// Success payload — becomes the "data" field of the envelope.
	TSharedPtr<FJsonObject> Data;

	// Failure detail. ErrorCode is the stable identifier the system prompt and
	// system_prompt_errors.txt teach the model to recognise, so it must not be
	// reworded casually.
	FString ErrorCode;
	FString Message;
	TArray<FString> Suggestions;

	static FToolExecutionResult Success(TSharedPtr<FJsonObject> InData = nullptr, bool bInOperationsSucceeded = true)
	{
		FToolExecutionResult R;
		R.bSuccess = true;
		R.bOperationsSucceeded = bInOperationsSucceeded;
		R.StatusCode = 200;
		R.Data = MoveTemp(InData);
		return R;
	}

	static FToolExecutionResult Error(int32 InStatus, const FString& InCode, const FString& InMessage,
		const TArray<FString>& InSuggestions = TArray<FString>())
	{
		FToolExecutionResult R;
		R.bSuccess = false;
		R.StatusCode = InStatus;
		R.ErrorCode = InCode;
		R.Message = InMessage;
		R.Suggestions = InSuggestions;
		return R;
	}

	static FToolExecutionResult NotStartedError(int32 InStatus, const FString& InCode, const FString& InMessage,
		const TArray<FString>& InSuggestions = TArray<FString>())
	{
		FToolExecutionResult R = Error(InStatus, InCode, InMessage, InSuggestions);
		R.bHandlerNotStarted = true;
		return R;
	}

	// The `{success, data}` / `{success, error}` envelope both callers share.
	// Suggestions are emitted only when non-empty; the HTTP path fills in
	// per-error-code defaults before calling this, so an empty array here means
	// "no suggestions", not "use the defaults".
	TSharedPtr<FJsonObject> ToEnvelope() const;

	// Serialized envelope, exactly as the HTTP route body and as the string the
	// agent hands the model.
	FString ToEnvelopeString() const;

	// What the model sees: the envelope, wrapped in <tool_use_error> on failure.
	// The system prompt teaches the model to read that wrapper; the live loop
	// consumes the typed flags instead of inspecting presentation text.
	FString ToModelString() const;
};
