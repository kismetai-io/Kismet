// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIToolResult.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"

TSharedPtr<FJsonObject> FToolExecutionResult::ToEnvelope() const
{
	TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
	Envelope->SetBoolField(TEXT("success"), bSuccess);

	if (bSuccess)
	{
		if (Data.IsValid())
		{
			Envelope->SetObjectField(TEXT("data"), Data);
		}
		return Envelope;
	}

	TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
	ErrorObj->SetStringField(TEXT("error_code"), ErrorCode);
	ErrorObj->SetStringField(TEXT("message"), Message);

	TArray<TSharedPtr<FJsonValue>> SuggestionsArr;
	for (const FString& S : Suggestions)
	{
		SuggestionsArr.Add(MakeShared<FJsonValueString>(S));
	}
	ErrorObj->SetArrayField(TEXT("suggestions"), SuggestionsArr);

	Envelope->SetObjectField(TEXT("error"), ErrorObj);
	return Envelope;
}

FString FToolExecutionResult::ToEnvelopeString() const
{
	FString Out;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(ToEnvelope().ToSharedRef(), Writer);
	return Out;
}

FString FToolExecutionResult::ToModelString() const
{
	const FString Envelope = ToEnvelopeString();
	// The wrapper is what the loop's IsToolResultError and the system prompt
	// both key on. Success is returned bare.
	return bSuccess ? Envelope : FString::Printf(TEXT("<tool_use_error>%s</tool_use_error>"), *Envelope);
}
