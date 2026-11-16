// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// A controlled provider transport for the production model-client path.
//
// FKismetAICurlModelClient::Call is curl plus three pieces of production code:
// the incremental stream decoder, the attempt classifier and (in CallWithRetry)
// the retry loop. This fake replaces only curl. Each scripted attempt is the
// byte stream curl would have written, curl's exit code and the HTTP status;
// everything after the bytes arrive runs the same code a real call runs, in the
// chunk sizes the test chooses (so a UTF-8 sequence or an escaped quote can be
// split across reads).

#pragma once

#include "CoreMinimal.h"
#include "KismetAIModelClient.h"
#include "KismetAILLMProvider.h"
#include "HAL/ThreadSafeBool.h"
#include "Dom/JsonObject.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace KismetAIFixtureTransport
{
	// curl exit codes the transport reports for a cut connection.
	inline constexpr int32 CurlPartialFile = 18;
	inline constexpr int32 CurlRecvError = 56;

	inline FString Condensed(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), W);
		return Out;
	}

	// Builds a wire-accurate Anthropic SSE stream. Payload text is JSON-encoded
	// by the serializer, so any character can be scripted; non-ASCII characters
	// stay raw UTF-8 on the wire, as the provider sends them.
	class FAnthropicStreamBuilder
	{
	public:
		FAnthropicStreamBuilder& MessageStart(int32 InputTokens = 20)
		{
			TSharedPtr<FJsonObject> Usage = MakeShared<FJsonObject>();
			Usage->SetNumberField(TEXT("input_tokens"), InputTokens);
			Usage->SetNumberField(TEXT("output_tokens"), 1);
			TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
			Msg->SetObjectField(TEXT("usage"), Usage);
			TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();
			D->SetStringField(TEXT("type"), TEXT("message_start"));
			D->SetObjectField(TEXT("message"), Msg);
			return Event(TEXT("message_start"), D);
		}

		FAnthropicStreamBuilder& BlockStart(const FString& BlockType, const FString& ToolId = FString(), const FString& ToolName = FString())
		{
			TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
			Block->SetStringField(TEXT("type"), BlockType);
			if (BlockType == TEXT("tool_use"))
			{
				Block->SetStringField(TEXT("id"), ToolId);
				Block->SetStringField(TEXT("name"), ToolName);
				Block->SetObjectField(TEXT("input"), MakeShared<FJsonObject>());
			}
			TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();
			D->SetStringField(TEXT("type"), TEXT("content_block_start"));
			D->SetNumberField(TEXT("index"), BlockIndex);
			D->SetObjectField(TEXT("content_block"), Block);
			return Event(TEXT("content_block_start"), D);
		}

		FAnthropicStreamBuilder& Delta(const FString& DeltaType, const FString& Field, const FString& Value)
		{
			TSharedPtr<FJsonObject> Delta = MakeShared<FJsonObject>();
			Delta->SetStringField(TEXT("type"), DeltaType);
			Delta->SetStringField(Field, Value);
			TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();
			D->SetStringField(TEXT("type"), TEXT("content_block_delta"));
			D->SetNumberField(TEXT("index"), BlockIndex);
			D->SetObjectField(TEXT("delta"), Delta);
			return Event(TEXT("content_block_delta"), D);
		}

		FAnthropicStreamBuilder& Text(const FString& T) { return Delta(TEXT("text_delta"), TEXT("text"), T); }
		FAnthropicStreamBuilder& Thinking(const FString& T) { return Delta(TEXT("thinking_delta"), TEXT("thinking"), T); }
		FAnthropicStreamBuilder& Signature(const FString& T) { return Delta(TEXT("signature_delta"), TEXT("signature"), T); }
		FAnthropicStreamBuilder& ToolJson(const FString& T) { return Delta(TEXT("input_json_delta"), TEXT("partial_json"), T); }

		FAnthropicStreamBuilder& BlockStop()
		{
			TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();
			D->SetStringField(TEXT("type"), TEXT("content_block_stop"));
			D->SetNumberField(TEXT("index"), BlockIndex);
			++BlockIndex;
			return Event(TEXT("content_block_stop"), D);
		}

		FAnthropicStreamBuilder& MessageEnd(const FString& StopReason)
		{
			TSharedPtr<FJsonObject> DeltaObj = MakeShared<FJsonObject>();
			DeltaObj->SetStringField(TEXT("stop_reason"), StopReason);
			TSharedPtr<FJsonObject> Usage = MakeShared<FJsonObject>();
			Usage->SetNumberField(TEXT("output_tokens"), 12);
			TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();
			D->SetStringField(TEXT("type"), TEXT("message_delta"));
			D->SetObjectField(TEXT("delta"), DeltaObj);
			D->SetObjectField(TEXT("usage"), Usage);
			Event(TEXT("message_delta"), D);
			TSharedPtr<FJsonObject> Stop = MakeShared<FJsonObject>();
			Stop->SetStringField(TEXT("type"), TEXT("message_stop"));
			return Event(TEXT("message_stop"), Stop);
		}

		// Raw bytes appended as-is (e.g. half an event, to model a cut stream).
		FAnthropicStreamBuilder& Raw(const FString& Text)
		{
			Wire += Text;
			return *this;
		}

		TArray<uint8> Bytes() const
		{
			FTCHARToUTF8 Utf8(*Wire, Wire.Len());
			TArray<uint8> Out;
			Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
			return Out;
		}

	private:
		FAnthropicStreamBuilder& Event(const TCHAR* Name, const TSharedPtr<FJsonObject>& Data)
		{
			Wire += FString::Printf(TEXT("event: %s\ndata: %s\n\n"), Name, *Condensed(Data));
			return *this;
		}

		FString Wire;
		int32 BlockIndex = 0;
	};

	// One transport attempt as curl would have reported it.
	struct FScriptedAttempt
	{
		TArray<uint8> Bytes;
		int32 ExitCode = 0;
		int32 HttpStatus = 200;
		int32 ChunkSize = 7;
		// Fires after the bytes were decoded, before the attempt is classified
		// (a Stop here is seen by the classifier, as curl's wait loop sees it).
		TFunction<void()> OnDelivered;
		// Fires after classification, just before the outcome is returned (a Stop
		// here races a completed attempt).
		TFunction<void()> OnClassified;
	};

	// Decode and classify one attempt through the production code.
	inline FModelCallOutcome RunAttempt(const FScriptedAttempt& Attempt, const FModelCallbacks& Callbacks,
		TFunctionRef<bool()> IsCancelled, const FString& Model = TEXT("claude-sonnet-4-6"))
	{
		TUniquePtr<ILLMProvider> Provider = MakeProviderForModel(Model);
		TSharedPtr<ILLMStreamState> State = Provider->CreateStreamState();
		State->OnThinkingComplete = Callbacks.OnThinkingComplete;
		State->OnThinkingDelta = Callbacks.OnThinkingDelta;

		FKismetAIStreamDecoder Decoder(*State, Callbacks);
		const int32 Step = FMath::Max(1, Attempt.ChunkSize);
		for (int32 Offset = 0; Offset < Attempt.Bytes.Num(); Offset += Step)
		{
			Decoder.Feed(Attempt.Bytes.GetData() + Offset, FMath::Min(Step, Attempt.Bytes.Num() - Offset));
		}
		Decoder.Finish();
		if (Attempt.OnDelivered) { Attempt.OnDelivered(); }

		FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Attempt.Bytes.GetData()), Attempt.Bytes.Num());
		const FString Raw(Conv.Length(), Conv.Get());
		FModelCallOutcome Outcome = FKismetAICurlModelClient::ClassifyAttempt(*Provider, *State, Raw,
			Attempt.ExitCode, Attempt.HttpStatus, /*bExecOk=*/true, IsCancelled());
		if (Attempt.OnClassified) { Attempt.OnClassified(); }
		return Outcome;
	}

	// IKismetAIModelClient over scripted attempts: Call runs the next attempt,
	// CallWithRetry runs the production retry loop over them.
	class FFixtureTransportModelClient final : public IKismetAIModelClient
	{
	public:
		explicit FFixtureTransportModelClient(TArray<FScriptedAttempt> InAttempts)
			: Attempts(MoveTemp(InAttempts)) {}

		virtual FModelCallOutcome Call(const FModelCallRequest& Request, const FModelCallbacks& Callbacks) override
		{
			if (Next >= Attempts.Num())
			{
				FModelCallOutcome None;
				None.Result = EModelCallResult::FatalError;
				None.ErrorType = TEXT("fixture_exhausted");
				None.ErrorMessage = TEXT("FFixtureTransportModelClient: no scripted attempt left");
				return None;
			}
			TUniquePtr<ILLMProvider> Encoder = MakeProviderForConfig(Request.Config);
			FLLMRequestShape Shape;
			CapturedBodies.Add(Condensed(Encoder->BuildRequestBody(Request.Config, Request.Messages, Request.Tools, Request.SystemPrompt, Shape)));
			CapturedEndpoints.Add(Encoder->GetEndpointUrl(Request.Config));
			CapturedHeaders.Add(Encoder->GetExtraCurlHeaders(Request.Config, Shape));
			CapturedConfigs.Add(Request.Config);
			CapturedMessages.Add(KismetAIAgentCodec::FromAnthropicArray(KismetAIAgentCodec::ToAnthropicArray(Request.Messages)));
			const FScriptedAttempt& Attempt = Attempts[Next++];
			return RunAttempt(Attempt, Callbacks, [this]() { return static_cast<bool>(bCancelled); }, Request.Config.Model);
		}

		virtual FModelCallOutcome CallWithRetry(const FModelCallRequest& Request, const FModelCallbacks& Callbacks) override
		{
			return FKismetAICurlModelClient::RunRetryLoop(Request, Callbacks,
				[this, &Callbacks](const FModelCallRequest& AttemptRequest) { return Call(AttemptRequest, Callbacks); },
				[this]() { return static_cast<bool>(bCancelled); });
		}

		virtual void Cancel() override { bCancelled = true; }
		virtual FString GetProviderName() const override { return TEXT("anthropic"); }
		virtual int32 CacheTtlSeconds() const override { return 300; }

		int32 AttemptsUsed() const { return Next; }
		TArray<TArray<FAgentMessage>> CapturedMessages;
		TArray<FString> CapturedBodies;
		TArray<FString> CapturedEndpoints;
		TArray<TArray<FString>> CapturedHeaders;
		TArray<FKismetAIAgentConfig> CapturedConfigs;

	private:
		TArray<FScriptedAttempt> Attempts;
		int32 Next = 0;
		FThreadSafeBool bCancelled = false;
	};
}
