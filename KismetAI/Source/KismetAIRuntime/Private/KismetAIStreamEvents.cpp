// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIStreamEvents.h"
#include "KismetAIToolCallJournal.h"
#include "KismetAILog.h"
#include "KismetAIDiagnostics.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

using KismetAIStreamEvents::EAttemptStatus;

const TCHAR* KismetAIStreamEvents::StatusToString(EAttemptStatus Status)
{
	switch (Status)
	{
	case EAttemptStatus::Completed:   return TEXT("completed");
	case EAttemptStatus::Interrupted: return TEXT("interrupted");
	case EAttemptStatus::Failed:      return TEXT("failed");
	case EAttemptStatus::Aborted:     return TEXT("aborted");
	}
	return TEXT("failed");
}

FString KismetAIStreamEvents::ToJson(const TSharedPtr<FJsonObject>& Obj)
{
	FString Output;
	if (!Obj.IsValid()) { return TEXT("{}"); }
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
	FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
	return Output;
}

// The cell outlives the stack-owned request when a client retains callbacks.
// Its lock drains a callback already executing before destruction revokes access.
struct FKismetAIModelRequestStream::FCallbackLifetime
{
	FCriticalSection Mutex;
	FKismetAIModelRequestStream* Stream = nullptr;

	void Invoke(TFunctionRef<void(FKismetAIModelRequestStream&)> Callback)
	{
		FScopeLock Lock(&Mutex);
		if (Stream) { Callback(*Stream); }
	}
};

FKismetAIModelRequestStream::FKismetAIModelRequestStream(const FOnAgentStep& InStepCallback,
	FJournalAppend InJournalAppend, const FString& InRunId, const FString& InTurnId, int32 InRequestIndex)
	: CallbackLifetime(MakeShared<FCallbackLifetime, ESPMode::ThreadSafe>())
	, StepCallback(InStepCallback)
	, JournalAppend(MoveTemp(InJournalAppend))
	, RunId(InRunId)
	, TurnId(InTurnId)
	, RequestId(KismetAIToolCallJournal::MakeRequestId(InTurnId, InRequestIndex))
	, MessageId(KismetAIToolCallJournal::MakeMessageId(InTurnId))
{
	CallbackLifetime->Stream = this;
}

FKismetAIModelRequestStream::~FKismetAIModelRequestStream()
{
	FScopeLock Lock(&CallbackLifetime->Mutex);
	CallbackLifetime->Stream = nullptr;
}

FModelCallbacks FKismetAIModelRequestStream::MakeCallbacks()
{
	FModelCallbacks Callbacks;
	Callbacks.OnTextDelta = [Lifetime = CallbackLifetime](const FString& Text)
	{ Lifetime->Invoke([&Text](FKismetAIModelRequestStream& Stream) { Stream.OnTextDelta(Text); }); };
	Callbacks.OnThinkingDelta = [Lifetime = CallbackLifetime](const FString& Text)
	{ Lifetime->Invoke([&Text](FKismetAIModelRequestStream& Stream) { Stream.OnThinkingDelta(Text); }); };
	Callbacks.OnThinkingComplete = [Lifetime = CallbackLifetime](const FString& Text)
	{ Lifetime->Invoke([&Text](FKismetAIModelRequestStream& Stream) { Stream.OnThinkingComplete(Text); }); };
	Callbacks.OnAttemptStart = [Lifetime = CallbackLifetime](int32 Index)
	{ Lifetime->Invoke([Index](FKismetAIModelRequestStream& Stream) { Stream.OpenAttempt(Index); }); };
	Callbacks.OnAttemptRetrying = [Lifetime = CallbackLifetime](int32 Index, const FModelCallOutcome& Failed)
	{ Lifetime->Invoke([Index, &Failed](FKismetAIModelRequestStream& Stream) { Stream.OnAttemptRetrying(Index, Failed); }); };
	Callbacks.OnAttemptCompleted = [Lifetime = CallbackLifetime](const FModelCallOutcome& Outcome)
	{ Lifetime->Invoke([&Outcome](FKismetAIModelRequestStream& Stream) { Stream.OnAttemptCompleted(Outcome); }); };
	return Callbacks;
}

void FKismetAIModelRequestStream::OpenAttempt(int32 InAttemptIndex)
{
	if (bFinalClosed) { return; }
	if (bOpen && InAttemptIndex == AttemptIndex)
	{
		return;
	}
	if (bOpen)
	{
		// The client started a new attempt without reporting the old one's end.
		// Never let two attempts share a segment: close the old one as replaced.
		CloseAttempt(EAttemptStatus::Interrupted, nullptr, /*bWillRetry=*/true);
	}

	if (bFinalClosed) { return; }
	bOpen = true;
	AttemptIndex = InAttemptIndex;
	AttemptStartedAt = FPlatformTime::Seconds();
	AttemptDiagnostics.Reset();
	AttemptId = KismetAIToolCallJournal::MakeAttemptId(RequestId, InAttemptIndex);
	bTextStarted = false;
	AttemptText.Reset();
	AttemptThinking.Reset();
	PendingThinkingBlock.Reset();

	TSharedPtr<FJsonObject> Event = MakeIdentity(TEXT("attempt_start"));
	if (!PreviousAttemptId.IsEmpty())
	{
		Event->SetStringField(TEXT("replaces"), PreviousAttemptId);
	}
	KismetAIDiagnostics::Record(TEXT("attempt_start"), Event);
	Emit(TEXT("attempt_start"), Event);
}

void FKismetAIModelRequestStream::EnsureOpen()
{
	if (!bOpen)
	{
		OpenAttempt(AttemptIndex == INDEX_NONE ? 0 : AttemptIndex + 1);
	}
}

void FKismetAIModelRequestStream::OnTextDelta(const FString& Text)
{
	EnsureOpen();
	if (bFinalClosed) { return; }
	if (!bTextStarted)
	{
		bTextStarted = true;
		Emit(TEXT("text_start"), MakeContentEvent(TEXT("text_start")));
	}
	// A consumer may close synchronously while handling text_start.
	if (bFinalClosed) { return; }
	AttemptText += Text;
	TSharedPtr<FJsonObject> Event = MakeContentEvent(TEXT("text_delta"));
	Event->SetStringField(TEXT("text"), Text);
	Emit(TEXT("text_delta"), Event);
}

void FKismetAIModelRequestStream::OnThinkingDelta(const FString& Text)
{
	EnsureOpen();
	if (bFinalClosed) { return; }
	PendingThinkingBlock += Text;
	TSharedPtr<FJsonObject> Event = MakeContentEvent(TEXT("thinking_delta"));
	Event->SetStringField(TEXT("text"), Text);
	Emit(TEXT("thinking_delta"), Event);
}

void FKismetAIModelRequestStream::OnThinkingComplete(const FString& Text)
{
	EnsureOpen();
	if (bFinalClosed) { return; }
	AttemptThinking += Text;
	PendingThinkingBlock.Reset();
	TSharedPtr<FJsonObject> Event = MakeContentEvent(TEXT("thinking"));
	Event->SetStringField(TEXT("text"), Text);
	Emit(TEXT("thinking"), Event);
}

void FKismetAIModelRequestStream::OnAttemptRetrying(int32 InAttemptIndex, const FModelCallOutcome& Failed)
{
	if (bFinalClosed) { return; }
	if (!bOpen || InAttemptIndex != AttemptIndex)
	{
		// Nothing streamed for it and it was never opened (a client that does
		// not report starts): open it so the failure is still on record.
		OpenAttempt(InAttemptIndex);
	}
	if (bFinalClosed) { return; }
	CloseAttempt(EAttemptStatus::Interrupted, &Failed, /*bWillRetry=*/true);
}

void FKismetAIModelRequestStream::OnAttemptCompleted(const FModelCallOutcome& Outcome)
{
	if (bFinalClosed || !bOpen) return;
	AttemptDiagnostics = MakeShared<FJsonObject>();
	AttemptDiagnostics->SetNumberField(TEXT("http_status"), Outcome.HttpStatusCode);
	AttemptDiagnostics->SetStringField(TEXT("error_code"), Outcome.ErrorType);
	TArray<TSharedPtr<FJsonObject>> Reports = Outcome.UsageReports;
	const TSharedPtr<FJsonObject>* Report = nullptr;
	if (Reports.IsEmpty() && Outcome.Response.IsValid() && Outcome.Response->TryGetObjectField(TEXT("usage"), Report))
	{
		Reports.Add(*Report);
	}
	auto Usage = MakeShared<FJsonObject>();
	bool bComplete = !Outcome.bUsageIncomplete && !Reports.IsEmpty();
	for (const auto& Item : Reports)
	{
		if (!Item.IsValid()) { bComplete = false; continue; }
		double Input = 0, Output = 0;
		bComplete &= Item->TryGetNumberField(TEXT("input_tokens"), Input) && FMath::IsFinite(Input) && Input >= 0
			&& Item->TryGetNumberField(TEXT("output_tokens"), Output) && FMath::IsFinite(Output) && Output >= 0;
		for (const TCHAR* Key : { TEXT("input_tokens"), TEXT("output_tokens"), TEXT("cache_read_input_tokens"),
			TEXT("cache_creation_input_tokens"), TEXT("total_tokens"), TEXT("reasoning_tokens") })
		{
			double Value = 0, Previous = 0;
			if (!Item->TryGetNumberField(Key, Value) || !FMath::IsFinite(Value) || Value < 0) continue;
			Usage->TryGetNumberField(Key, Previous);
			if (FMath::IsFinite(Previous + Value)) Usage->SetNumberField(Key, Previous + Value);
		}
	}
	AttemptDiagnostics->SetObjectField(TEXT("usage"), Usage);
	AttemptDiagnostics->SetStringField(TEXT("usage_provenance"), Usage->Values.IsEmpty() ? TEXT("unavailable") : TEXT("reported"));
	AttemptDiagnostics->SetStringField(TEXT("usage_scope"), TEXT("attempt"));
	AttemptDiagnostics->SetBoolField(TEXT("usage_complete"), bComplete);
	FString FinishReason;
	if (Outcome.Response.IsValid() && Outcome.Response->TryGetStringField(TEXT("stop_reason"), FinishReason))
		AttemptDiagnostics->SetStringField(TEXT("finish_reason"), FinishReason);
}

void FKismetAIModelRequestStream::CloseFinal(EAttemptStatus Status, const FModelCallOutcome& Outcome)
{
	FScopeLock Lock(&CallbackLifetime->Mutex);
	CallbackLifetime->Stream = nullptr;
	if (bFinalClosed) { return; }
	bFinalClosed = true;
	if (!bOpen)
	{
		return;
	}
	CloseAttempt(Status, &Outcome, /*bWillRetry=*/false);
}

void FKismetAIModelRequestStream::CloseAttempt(EAttemptStatus Status, const FModelCallOutcome* Outcome, bool bWillRetry)
{
	check(bOpen);
	// Mark closed before external consumers can reenter through terminal events.
	bOpen = false;
	// A text segment never outlives its attempt.
	if (bTextStarted)
	{
		Emit(TEXT("text_end"), MakeContentEvent(TEXT("text_end")));
	}

	TSharedPtr<FJsonObject> Event = MakeIdentity(TEXT("attempt_end"));
	Event->SetStringField(TEXT("status"), KismetAIStreamEvents::StatusToString(Status));
	Event->SetBoolField(TEXT("will_retry"), bWillRetry);
	if (Status != EAttemptStatus::Completed)
	{
		if (Outcome)
		{
			Event->SetStringField(TEXT("error_code"), Outcome->ErrorType);
			Event->SetStringField(TEXT("message"), Outcome->ErrorMessage);
			Event->SetNumberField(TEXT("http_status"), Outcome->HttpStatusCode);
		}
		Event->SetStringField(TEXT("text"), AttemptText);
		Event->SetStringField(TEXT("thinking"), AttemptThinking + PendingThinkingBlock);
	}
	// CallWithRetry's final Outcome may aggregate earlier attempts. Only the
	// single-attempt callback can certify this attempt's usage.
	auto Diagnostic = KismetAIDiagnostics::SafeProjection(Event);
	Diagnostic->SetNumberField(TEXT("duration_ms"), FMath::Max(0.0, (FPlatformTime::Seconds() - AttemptStartedAt) * 1000.0));
	Diagnostic->SetStringField(TEXT("usage_provenance"), TEXT("unavailable"));
	Diagnostic->SetStringField(TEXT("usage_scope"), TEXT("attempt"));
	Diagnostic->SetBoolField(TEXT("usage_complete"), false);
	if (AttemptDiagnostics.IsValid())
	{
		for (const auto& Field : AttemptDiagnostics->Values) Diagnostic->SetField(FString(Field.Key), Field.Value);
	}
	else if (Outcome && Outcome->Response.IsValid())
	{
		FString FinishReason;
		if (Outcome->Response->TryGetStringField(TEXT("stop_reason"), FinishReason))
			Diagnostic->SetStringField(TEXT("finish_reason"), FinishReason);
	}
	KismetAIDiagnostics::Record(TEXT("attempt_end"), Diagnostic);
	Emit(TEXT("attempt_end"), Event);

	if (Status == EAttemptStatus::Completed)
	{
		CompletedAttemptId = AttemptId;
	}
	else if (JournalAppend)
	{
		// The attempt's own record, so a replay shows what the live run showed
		// and marks it the same way. Best effort: losing it loses display
		// fidelity, never conversation state (the reducer ignores the type).
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("type"), KismetAIStreamEvents::AttemptEntryType);
		Entry->SetNumberField(TEXT("schema"), KismetAIStreamEvents::AttemptEntrySchema);
		Entry->SetStringField(TEXT("uuid"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
		Entry->SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());
		for (const auto& Field : Event->Values)
		{
			const FString Key(Field.Key);
			if (Key != TEXT("type")) { Entry->SetField(Key, Field.Value); }
		}
		if (!JournalAppend(Entry))
		{
			KISMETAI_LOG(LogKismetAIAgent, Warning, TEXT("Could not journal %s attempt %s"),
				KismetAIStreamEvents::StatusToString(Status), *AttemptId);
		}
	}

	PreviousAttemptId = AttemptId;
	bOpen = false;
	bTextStarted = false;
}

void FKismetAIModelRequestStream::StampCommit(const TSharedPtr<FJsonObject>& Entry) const
{
	if (!Entry.IsValid()) { return; }
	Entry->SetStringField(TEXT("message_id"), MessageId);
	Entry->SetStringField(TEXT("request_id"), RequestId);
	if (!CompletedAttemptId.IsEmpty())
	{
		Entry->SetStringField(TEXT("attempt_id"), CompletedAttemptId);
	}
}

TSharedPtr<FJsonObject> FKismetAIModelRequestStream::MakeIdentity(const TCHAR* Type) const
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("type"), Type);
	Obj->SetStringField(TEXT("run_id"), RunId);
	Obj->SetStringField(TEXT("turn_id"), TurnId);
	Obj->SetStringField(TEXT("request_id"), RequestId);
	Obj->SetStringField(TEXT("attempt_id"), AttemptId);
	Obj->SetStringField(TEXT("message_id"), MessageId);
	Obj->SetNumberField(TEXT("attempt"), AttemptIndex);
	return Obj;
}

TSharedPtr<FJsonObject> FKismetAIModelRequestStream::MakeContentEvent(const TCHAR* Type) const
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("type"), Type);
	Obj->SetStringField(TEXT("attempt_id"), AttemptId);
	Obj->SetStringField(TEXT("message_id"), MessageId);
	return Obj;
}

void FKismetAIModelRequestStream::Emit(const TCHAR* Type, const TSharedPtr<FJsonObject>& Payload) const
{
	if (StepCallback.IsBound())
	{
		StepCallback.Execute(Type, KismetAIStreamEvents::ToJson(Payload));
	}
}
