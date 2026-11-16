// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Test doubles for the agent's two outward seams. Together these let an
// automation test drive FKismetAIAgent::RunTask with no editor, no network and
// no API key — which is what makes the loop's behavior assertable at all.

#pragma once

#include "CoreMinimal.h"
#include "KismetAIAgent.h"
#include "KismetAIModelClient.h"
#include "KismetAIToolInvoker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

namespace KismetAITestFakes
{
	// --- response builders ------------------------------------------------
	//
	// Both providers normalize to the Anthropic response shape before the loop
	// sees it (ILLMStreamState::BuildResponseJson), so scripting at this level
	// is provider-neutral.

	inline TSharedPtr<FJsonObject> MakeUsage(int32 In = 100, int32 Out = 50)
	{
		TSharedPtr<FJsonObject> U = MakeShared<FJsonObject>();
		U->SetNumberField(TEXT("input_tokens"), In);
		U->SetNumberField(TEXT("output_tokens"), Out);
		return U;
	}

	inline TSharedPtr<FJsonObject> MakeTextBlock(const FString& Text)
	{
		TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>();
		B->SetStringField(TEXT("type"), TEXT("text"));
		B->SetStringField(TEXT("text"), Text);
		return B;
	}

	inline TSharedPtr<FJsonObject> MakeToolUseBlock(const FString& Id, const FString& Name,
		const TSharedPtr<FJsonObject>& Input)
	{
		TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>();
		B->SetStringField(TEXT("type"), TEXT("tool_use"));
		B->SetStringField(TEXT("id"), Id);
		B->SetStringField(TEXT("name"), Name);
		B->SetObjectField(TEXT("input"), Input.IsValid() ? Input : MakeShared<FJsonObject>());
		return B;
	}

	// A whole assistant turn: content blocks + stop_reason + usage.
	inline FModelCallOutcome MakeResponse(
		const TArray<TSharedPtr<FJsonObject>>& Blocks,
		const FString& StopReason,
		int32 InputTokens = 100,
		int32 OutputTokens = 50)
	{
		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Content;
		for (const TSharedPtr<FJsonObject>& B : Blocks)
		{
			Content.Add(MakeShared<FJsonValueObject>(B));
		}
		Resp->SetArrayField(TEXT("content"), Content);
		Resp->SetStringField(TEXT("stop_reason"), StopReason);
		Resp->SetObjectField(TEXT("usage"), MakeUsage(InputTokens, OutputTokens));

		FModelCallOutcome Outcome;
		Outcome.Result = EModelCallResult::Success;
		Outcome.Response = Resp;
		Outcome.AssistantMessage.Role = EAgentRole::Assistant;
		Outcome.AssistantMessage.Parts = KismetAIAgentCodec::FromParts(Content);
		Outcome.Response->RemoveField(TEXT("content"));
		Outcome.HttpStatusCode = 200;
		return Outcome;
	}

	// Convenience: a plain text turn that ends the run.
	inline FModelCallOutcome MakeTextTurn(const FString& Text, const FString& StopReason = TEXT("end_turn"))
	{
		return MakeResponse({ MakeTextBlock(Text) }, StopReason);
	}

	// Convenience: a turn that calls one tool.
	inline FModelCallOutcome MakeToolTurn(const FString& ToolId, const FString& ToolName,
		const TSharedPtr<FJsonObject>& Input = nullptr)
	{
		return MakeResponse({ MakeToolUseBlock(ToolId, ToolName, Input) }, TEXT("tool_use"));
	}

	// --- model client -----------------------------------------------------

	// What the loop actually sent, captured per call so tests can assert on the
	// request rather than on side effects.
	struct FCapturedRequest
	{
		TArray<TSharedPtr<FJsonValue>> Messages;
		TArray<TSharedPtr<FJsonValue>> Tools;
		FString SystemPrompt;
		int32 MaxTokens = 0;
		bool bWithRetry = false;
	};

	// Returns scripted outcomes in order. Once the script runs out it keeps
	// returning the last entry, so a test that under-scripts fails on an
	// assertion rather than hanging or crashing.
	class FScriptedModelClient final : public IKismetAIModelClient
	{
	public:
		explicit FScriptedModelClient(TArray<FModelCallOutcome> InScript)
			: Script(MoveTemp(InScript)) {}

		virtual FModelCallOutcome Call(const FModelCallRequest& Request, const FModelCallbacks& Callbacks) override
		{
			return Respond(Request, Callbacks, /*bWithRetry=*/false);
		}

		virtual FModelCallOutcome CallWithRetry(const FModelCallRequest& Request, const FModelCallbacks& Callbacks) override
		{
			return Respond(Request, Callbacks, /*bWithRetry=*/true);
		}

		virtual void Cancel() override { bCancelled = true; }
		virtual FString GetProviderName() const override { return TEXT("anthropic"); }
		virtual int32 CacheTtlSeconds() const override { return 300; }

		const TArray<FCapturedRequest>& GetRequests() const { return Requests; }
		int32 NumCalls() const { return Requests.Num(); }
		bool WasCancelled() const { return bCancelled; }

		// Text emitted through OnTextDelta for the Nth scripted turn, so a test
		// can exercise the streaming path without a transport.
		TMap<int32, FString> TextDeltasByCall;

	private:
		FModelCallOutcome Respond(const FModelCallRequest& Request, const FModelCallbacks& Callbacks, bool bWithRetry)
		{
			FCapturedRequest Cap;
			Cap.Messages = KismetAIAgentCodec::ToAnthropicArray(Request.Messages);
			Cap.Tools = Request.Tools;
			Cap.SystemPrompt = Request.SystemPrompt;
			Cap.MaxTokens = Request.Config.MaxTokens;
			Cap.bWithRetry = bWithRetry;
			const int32 Index = Requests.Add(MoveTemp(Cap));

			if (const FString* Delta = TextDeltasByCall.Find(Index))
			{
				if (Callbacks.OnTextDelta) { Callbacks.OnTextDelta(*Delta); }
			}

			if (Script.Num() == 0)
			{
				FModelCallOutcome Empty;
				Empty.Result = EModelCallResult::FatalError;
				Empty.ErrorMessage = TEXT("FScriptedModelClient: no script");
				return Empty;
			}
			FModelCallOutcome Outcome = Script[FMath::Min(Index, Script.Num() - 1)];
			if (Outcome.RequestCount == 0) Outcome.RequestCount = 1;
			return Outcome;
		}

		TArray<FModelCallOutcome> Script;
		TArray<FCapturedRequest> Requests;
		bool bCancelled = false;
	};

	// --- tool invoker -----------------------------------------------------

	// Canned results by tool name, plus a call log. Unscripted tools return a
	// generic success so a test only has to script what it asserts on.
	class FScriptedToolInvoker final : public IKismetAIToolInvoker
	{
	public:
		virtual FKismetAIInvokeResult Invoke(const FKismetAIToolInvocation& Invocation) override
		{
			// Honour the admission seam as the production invoker does: a call
			// refused there is not "made", so it is not logged either.
			if (const TOptional<FToolExecutionResult> Refused = Invocation.RunAdmission())
			{
				return FKismetAIInvokeResult::FromTyped(Refused.GetValue());
			}
			const FString& ToolName = Invocation.ToolName;
			CallLog.Add(ToolName);
			ArgsLog.Add(Invocation.RequestBody);
			if (OnInvoke) { OnInvoke(ToolName); }
			if (const FToolExecutionResult* Typed = TypedResults.Find(ToolName)) { return FKismetAIInvokeResult::FromTyped(*Typed); }
			if (const FString* Canned = Results.Find(ToolName))
			{
				return FKismetAIInvokeResult::FromModelString(*Canned);
			}
			return FKismetAIInvokeResult::FromModelString(TEXT("{\"success\":true}"));
		}

		virtual TSet<FString> GetDispatchableToolNames() const override
		{
			TSet<FString> Names;
			for (const TPair<FString, FString>& Pair : Results) { Names.Add(Pair.Key); }
			return Names;
		}

		virtual FString ResolveHandlerName(const FString& ToolName, const FString& Mode) const override
		{
			return Results.Contains(ToolName) ? TEXT("Scripted") : FString();
		}

		// Script a tool's result. Use the <tool_use_error> wrapper to simulate failure.
		void SetResult(const FString& ToolName, const FString& Result) { Results.Add(ToolName, Result); }
		void SetTypedResult(const FString& ToolName, const FToolExecutionResult& Result) { TypedResults.Add(ToolName, Result); }

		int32 CountCalls(const FString& ToolName) const
		{
			int32 N = 0;
			for (const FString& S : CallLog) { if (S == ToolName) { ++N; } }
			return N;
		}

		// Asset state by package path. A path with no entry is described
		// as "not captured", like an invoker without asset state.
		virtual TArray<FKismetAIAssetFingerprint> CaptureAssetState(const TArray<FString>& PackagePaths) override
		{
			TArray<FKismetAIAssetFingerprint> Out;
			for (const FString& Path : PackagePaths)
			{
				CaptureLog.Add(Path);
				if (const FKismetAIAssetFingerprint* State = AssetStates.Find(Path))
				{
					Out.Add(*State);
					continue;
				}
				FKismetAIAssetFingerprint F;
				F.AssetPath = Path;
				F.UnsupportedReason = TEXT("not scripted");
				Out.Add(MoveTemp(F));
			}
			return Out;
		}

		virtual TArray<FKismetAIAssetFingerprint> BeginMutation(const FKismetAIMutationContext& Context) override
		{
			BeginLog.Add(Context);
			return CaptureAssetState(Context.Targets);
		}

		virtual void EndMutation(const FKismetAIMutationContext& Context) override
		{
			EndLog.Add(Context);
		}

		TMap<FString, FKismetAIAssetFingerprint> AssetStates;
		TArray<FString> CaptureLog;
		TArray<FKismetAIMutationContext> BeginLog;
		TArray<FKismetAIMutationContext> EndLog;

		// Fires inside Invoke, before the result is returned. Lets a test reach
		// into the middle of a synchronous run — e.g. to abort it.
		TFunction<void(const FString&)> OnInvoke;

		TArray<FString> CallLog;
		TArray<TSharedPtr<FJsonObject>> ArgsLog;

	private:
		TMap<FString, FString> Results;
		TMap<FString, FToolExecutionResult> TypedResults;
	};

	// A captured, supported fingerprint whose state is described by Digest and
	// one object per entry of Objects (name -> digest).
	inline FKismetAIAssetFingerprint MakeFingerprint(const FString& Path, const FString& Digest,
		const TMap<FString, FString>& Objects = {}, const FString& FileSha1 = FString())
	{
		FKismetAIAssetFingerprint F;
		F.AssetPath = Path;
		F.AssetClass = TEXT("Blueprint");
		F.bCaptured = true;
		F.bExists = true;
		F.bSupported = true;
		F.Digest = Digest;
		F.ObjectDigests = Objects;
		F.bFileExists = !FileSha1.IsEmpty();
		F.FileSha1 = FileSha1;
		F.FileSize = FileSha1.IsEmpty() ? -1 : 100;
		return F;
	}

	// A tool error in the shape the loop recognises (IsToolResultError).
	inline FString ToolError(const FString& Code, const FString& Message)
	{
		return FString::Printf(
			TEXT("<tool_use_error>{\"success\":false,\"error\":{\"error_code\":\"%s\",\"message\":\"%s\"}}</tool_use_error>"),
			*Code, *Message);
	}

	// --- event capture ----------------------------------------------------

	// Records the (type, json) stream the agent emits, which is the de-facto
	// protocol the panel, bridge and SSE route all consume.
	struct FEventRecorder
	{
		TArray<TPair<FString, FString>> Events;

		FOnAgentStep MakeCallback()
		{
			FOnAgentStep Cb;
			Cb.BindLambda([this](const FString& Type, const FString& Json)
			{
				Events.Add(TPair<FString, FString>(Type, Json));
			});
			return Cb;
		}

		TArray<FString> Types() const
		{
			TArray<FString> Out;
			for (const TPair<FString, FString>& E : Events) { Out.Add(E.Key); }
			return Out;
		}

		int32 Count(const FString& Type) const
		{
			int32 N = 0;
			for (const TPair<FString, FString>& E : Events) { if (E.Key == Type) { ++N; } }
			return N;
		}

		const FString* FirstJsonOfType(const FString& Type) const
		{
			for (const TPair<FString, FString>& E : Events) { if (E.Key == Type) { return &E.Value; } }
			return nullptr;
		}

		bool HasType(const FString& Type) const { return Count(Type) > 0; }
	};
}
