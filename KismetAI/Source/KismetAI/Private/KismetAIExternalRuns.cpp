// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIExternalRuns.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace KismetAIExternalRuns
{
	FString SseEventNameFor(const FString& Type)
	{
		if (Type == TEXT("done") || Type == TEXT("error") || Type == TEXT("plan_ready")
			|| Type == TEXT("text_delta") || Type == TEXT("text_start") || Type == TEXT("text_end")
			|| Type == TEXT("run"))
		{
			return Type;
		}
		return TEXT("step");
	}

	FString FormatSseFrame(int64 Seq, const FString& Name, const FString& Data)
	{
		// Data is condensed JSON from the serializer: one line, so one data: line.
		return FString::Printf(TEXT("id: %lld\nevent: %s\ndata: %s\n\n"), Seq, *Name, *Data);
	}

	FString FingerprintRequest(const FString& Input, const FString& SessionId, const FString& Blueprint, int32 MaxSteps)
	{
		const FString Joined = FString::Printf(TEXT("%d\x1f%s\x1f%s\x1f%s"), MaxSteps, *SessionId, *Blueprint, *Input);
		const FTCHARToUTF8 Utf8(*Joined);
		FSHAHash Hash;
		FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Hash.Hash);
		return Hash.ToString().ToLower();
	}

	static TSharedPtr<FJsonObject> ParseObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Obj))
		{
			return nullptr;
		}
		return Obj;
	}

	static FString ToCondensed(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	static int64 SizeOf(const FKismetAIRunEvent& Event)
	{
		return static_cast<int64>(Event.Data.Len() + Event.Type.Len() + Event.Name.Len()) * sizeof(TCHAR) + 64;
	}
}


// ── Record ────────────────────────────────────────────────────────────────────

FKismetAIExternalRunRecord::FKismetAIExternalRunRecord(const FString& InRunId, const FString& InClientRequestId,
	const FString& InFingerprint, const FString& InSessionId, FLimits InLimits)
	: RunId(InRunId)
	, ClientRequestId(InClientRequestId)
	, Fingerprint(InFingerprint)
	, Limits(InLimits)
	, CreatedAt(FDateTime::UtcNow())
	, SessionId(InSessionId)
{
	// Seq 1: the run itself, so every stream starts with the id it can
	// reattach by, before the agent has said anything.
	TSharedRef<FJsonObject> First = MakeShared<FJsonObject>();
	First->SetStringField(TEXT("type"), TEXT("run"));
	First->SetStringField(TEXT("run_id"), RunId);
	First->SetStringField(TEXT("session_id"), SessionId);
	if (!ClientRequestId.IsEmpty())
	{
		First->SetStringField(TEXT("client_request_id"), ClientRequestId);
	}
	AppendLocked(TEXT("run"), KismetAIExternalRuns::ToCondensed(First));
}

void FKismetAIExternalRunRecord::Append(const FString& Type, const FString& JsonData)
{
	FScopeLock Lock(&Mutex);
	AppendLocked(Type, JsonData);
}

void FKismetAIExternalRunRecord::AppendLocked(const FString& Type, const FString& JsonData)
{
	FKismetAIRunEvent Event;
	Event.Seq = NextSeq++;
	Event.Type = Type;
	Event.Name = KismetAIExternalRuns::SseEventNameFor(Type);
	Event.Data = JsonData;
	Event.At = FDateTime::UtcNow();
	RetainedBytes += KismetAIExternalRuns::SizeOf(Event);

	LastEventType = Type;
	LastEventAt = Event.At;

	if (Type == TEXT("attempt_start"))
	{
		++ModelAttempts;
		PhaseHint = TEXT("model");
	}
	else if (Type == TEXT("session_start"))
	{
		PhaseHint = TEXT("model");
	}
	else if (Type == TEXT("tool_call"))
	{
		++ToolCalls;
		PhaseHint = TEXT("tool");
		const TSharedPtr<FJsonObject> Obj = KismetAIExternalRuns::ParseObject(JsonData);
		CurrentTool = Obj.IsValid() ? Obj->GetStringField(TEXT("tool")) : FString();
	}
	else if (Type == TEXT("tool_result"))
	{
		++ToolResults;
		PhaseHint = TEXT("between_steps");
		CurrentTool.Reset();
		const TSharedPtr<FJsonObject> Obj = KismetAIExternalRuns::ParseObject(JsonData);
		const TSharedPtr<FJsonObject>* Result = nullptr;
		bool bSuccess = true;
		bool bIsError = false;
		if (Obj.IsValid() && Obj->TryGetBoolField(TEXT("is_error"), bIsError))
		{
			bSuccess = !bIsError;
		}
		else if (Obj.IsValid() && Obj->TryGetObjectField(TEXT("result"), Result) && Result && (*Result).IsValid())
		{
			(*Result)->TryGetBoolField(TEXT("success"), bSuccess);
		}
		else if (Obj.IsValid())
		{
			FString Text;
			if (Obj->TryGetStringField(TEXT("result"), Text) && Text.Contains(TEXT("<tool_use_error>")))
			{
				bSuccess = false;
			}
		}
		if (!bSuccess)
		{
			++ToolErrors;
		}
	}
	else if (Type == TEXT("question"))
	{
		++Questions;
		PhaseHint = TEXT("waiting_for_answer");
	}
	else if (Type == TEXT("token_stats"))
	{
		++ModelRequests;
	}
	else if (Type == TEXT("mutation_outcomes"))
	{
		MutationOutcomes = KismetAIExternalRuns::ParseObject(JsonData);
	}
	else if ((Type == TEXT("done") || Type == TEXT("error")) && !bTerminalEventSeen)
	{
		// Exactly one of these ends a run. The first one is the outcome.
		bTerminalEventSeen = true;
		TerminalEvent = Type;
		TerminalData = KismetAIExternalRuns::ParseObject(JsonData);
		FString Status;
		if (TerminalData.IsValid() && TerminalData->TryGetStringField(TEXT("status"), Status))
		{
			TerminalStatus = Status;
		}
		else
		{
			TerminalStatus = Type == TEXT("done") ? TEXT("completed") : TEXT("error");
		}
	}

	Events.Add(MoveTemp(Event));
	TrimLocked();
}

void FKismetAIExternalRunRecord::TrimLocked()
{
	if (Events.Num() <= Limits.MaxEvents && RetainedBytes <= Limits.MaxBytes)
	{
		return;
	}
	// Drop oldest first, in one block, down to 90% of each bound so trimming is
	// not paid on every append. The newest event is always kept.
	const int32 TargetEvents = FMath::Max(1, Limits.MaxEvents - Limits.MaxEvents / 10);
	const int64 TargetBytes = FMath::Max<int64>(0, Limits.MaxBytes - Limits.MaxBytes / 10);
	int32 Drop = 0;
	int64 Bytes = RetainedBytes;
	while (Drop < Events.Num() - 1 && (Events.Num() - Drop > TargetEvents || Bytes > TargetBytes))
	{
		Bytes -= KismetAIExternalRuns::SizeOf(Events[Drop]);
		++Drop;
	}
	if (Drop > 0)
	{
		Events.RemoveAt(0, Drop, EAllowShrinking::No);
		RetainedBytes = Bytes;
		DroppedEvents += Drop;
	}
}

void FKismetAIExternalRunRecord::SetControllerRun(const FKismetAIRunPtr& InRun)
{
	FScopeLock Lock(&Mutex);
	ControllerRun = InRun;
}

FKismetAIRunPtr FKismetAIExternalRunRecord::GetControllerRun() const
{
	FScopeLock Lock(&Mutex);
	return ControllerRun;
}

void FKismetAIExternalRunRecord::SetFinalText(const FString& Text)
{
	FScopeLock Lock(&Mutex);
	// The answer text, bounded: the event log already carries it in full.
	FinalText = Text.Left(16 * 1024);
}

void FKismetAIExternalRunRecord::SetSessionId(const FString& InSessionId)
{
	FScopeLock Lock(&Mutex);
	if (!InSessionId.IsEmpty())
	{
		SessionId = InSessionId;
	}
}

FKismetAIExternalRunRecord::FCancelResult FKismetAIExternalRunRecord::NoteCancel()
{
	FScopeLock Lock(&Mutex);
	FCancelResult Out;
	Out.PhaseAtRequest = PhaseLocked();
	if (bFinished)
	{
		Out.Ack = ECancelAck::AlreadyFinished;
		return Out;
	}
	if (bCancelRequested)
	{
		Out.Ack = ECancelAck::AlreadyRequested;
		Out.PhaseAtRequest = CancelPhase;
		return Out;
	}
	bCancelRequested = true;
	CancelRequestedAt = FDateTime::UtcNow();
	CancelPhase = Out.PhaseAtRequest;
	Out.Ack = ECancelAck::Requested;
	return Out;
}

void FKismetAIExternalRunRecord::Finish(const FString& FallbackStatus)
{
	FScopeLock Lock(&Mutex);
	if (bFinished)
	{
		return;
	}
	bFinished = true;
	FinishedAt = FDateTime::UtcNow();
	OutcomeStatus = bTerminalEventSeen ? TerminalStatus : FallbackStatus;
	CurrentTool.Reset();
	ControllerRun.Reset();
}

FKismetAIRunEventPage FKismetAIExternalRunRecord::Read(int64 After, int32 Limit) const
{
	FScopeLock Lock(&Mutex);
	FKismetAIRunEventPage Page;
	Page.LastSeq = NextSeq - 1;
	Page.FirstRetainedSeq = Events.Num() > 0 ? Events[0].Seq : 0;
	Page.bFinished = bFinished;
	Page.NextAfter = FMath::Max<int64>(After, 0);

	if (After < 0)
	{
		After = 0;
	}
	if (Page.FirstRetainedSeq > 0 && After + 1 < Page.FirstRetainedSeq)
	{
		Page.bExpired = true;
		return Page;
	}
	const int32 Start = Page.FirstRetainedSeq > 0 ? static_cast<int32>(FMath::Max<int64>(0, After + 1 - Page.FirstRetainedSeq)) : 0;
	const int32 Cap = Limit > 0 ? Limit : MAX_int32;
	for (int32 Index = Start; Index < Events.Num(); ++Index)
	{
		if (Page.Events.Num() >= Cap)
		{
			Page.bMore = true;
			break;
		}
		Page.Events.Add(Events[Index]);
	}
	if (Page.Events.Num() > 0)
	{
		Page.NextAfter = Page.Events.Last().Seq;
	}
	return Page;
}

bool FKismetAIExternalRunRecord::IsFinished() const
{
	FScopeLock Lock(&Mutex);
	return bFinished;
}

FDateTime FKismetAIExternalRunRecord::GetFinishedAt() const
{
	FScopeLock Lock(&Mutex);
	return FinishedAt;
}

FString FKismetAIExternalRunRecord::GetPhase() const
{
	FScopeLock Lock(&Mutex);
	return PhaseLocked();
}

FString FKismetAIExternalRunRecord::PhaseLocked() const
{
	if (bFinished)
	{
		return TEXT("finished");
	}
	if (bTerminalEventSeen)
	{
		return TEXT("finalizing");
	}
	return PhaseHint;
}

TSharedRef<FJsonObject> FKismetAIExternalRunRecord::BuildStatusJson(const TOptional<FKismetAIRunStatus>& Worker) const
{
	FScopeLock Lock(&Mutex);
	TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetStringField(TEXT("run_id"), RunId);
	J->SetStringField(TEXT("origin"), LexToString(EKismetAIRunOrigin::HttpAgentRun));
	if (!ClientRequestId.IsEmpty())
	{
		J->SetStringField(TEXT("client_request_id"), ClientRequestId);
	}
	J->SetStringField(TEXT("session_id"), SessionId);

	const FString State = bFinished ? TEXT("finished") : (bCancelRequested ? TEXT("cancelling") : TEXT("running"));
	J->SetStringField(TEXT("state"), State);
	J->SetStringField(TEXT("phase"), PhaseLocked());
	if (!CurrentTool.IsEmpty())
	{
		J->SetStringField(TEXT("current_tool"), CurrentTool);
	}
	J->SetStringField(TEXT("created_at"), CreatedAt.ToIso8601());
	const FDateTime End = bFinished ? FinishedAt : FDateTime::UtcNow();
	J->SetNumberField(TEXT("elapsed_ms"), FMath::RoundToDouble((End - CreatedAt).GetTotalMilliseconds()));
	if (bFinished)
	{
		J->SetStringField(TEXT("finished_at"), FinishedAt.ToIso8601());
	}

	if (Worker.IsSet())
	{
		TSharedRef<FJsonObject> W = MakeShared<FJsonObject>();
		W->SetStringField(TEXT("state"), LexToString(Worker->State));
		W->SetBoolField(TEXT("lease_held"), Worker->bLeaseHeld);
		W->SetBoolField(TEXT("cancel_requested"), Worker->bCancelRequested);
		J->SetObjectField(TEXT("worker"), W);
	}

	TSharedRef<FJsonObject> Progress = MakeShared<FJsonObject>();
	Progress->SetNumberField(TEXT("model_attempts"), ModelAttempts);
	Progress->SetNumberField(TEXT("model_requests"), ModelRequests);
	Progress->SetNumberField(TEXT("tool_calls"), ToolCalls);
	Progress->SetNumberField(TEXT("tool_results"), ToolResults);
	Progress->SetNumberField(TEXT("tool_errors"), ToolErrors);
	Progress->SetNumberField(TEXT("questions"), Questions);
	J->SetObjectField(TEXT("progress"), Progress);

	TSharedRef<FJsonObject> EventsJson = MakeShared<FJsonObject>();
	EventsJson->SetNumberField(TEXT("first_seq"), Events.Num() > 0 ? Events[0].Seq : 0);
	EventsJson->SetNumberField(TEXT("last_seq"), NextSeq - 1);
	EventsJson->SetNumberField(TEXT("retained"), Events.Num());
	EventsJson->SetNumberField(TEXT("dropped"), DroppedEvents);
	J->SetObjectField(TEXT("events"), EventsJson);

	if (!LastEventType.IsEmpty())
	{
		TSharedRef<FJsonObject> Last = MakeShared<FJsonObject>();
		Last->SetNumberField(TEXT("seq"), NextSeq - 1);
		Last->SetStringField(TEXT("type"), LastEventType);
		Last->SetStringField(TEXT("at"), LastEventAt.ToIso8601());
		J->SetObjectField(TEXT("last_event"), Last);
	}

	TSharedRef<FJsonObject> Cancel = MakeShared<FJsonObject>();
	Cancel->SetBoolField(TEXT("requested"), bCancelRequested);
	if (bCancelRequested)
	{
		Cancel->SetStringField(TEXT("requested_at"), CancelRequestedAt.ToIso8601());
		Cancel->SetStringField(TEXT("phase_at_request"), CancelPhase);
	}
	J->SetObjectField(TEXT("cancel"), Cancel);

	if (bFinished)
	{
		TSharedRef<FJsonObject> Outcome = MakeShared<FJsonObject>();
		Outcome->SetStringField(TEXT("status"), OutcomeStatus);
		Outcome->SetStringField(TEXT("terminal_event"), bTerminalEventSeen ? TerminalEvent : FString(TEXT("none")));
		Outcome->SetBoolField(TEXT("cancel_requested"), bCancelRequested);
		if (TerminalData.IsValid())
		{
			Outcome->SetObjectField(TEXT("terminal"), TerminalData);
		}
		// What the run applied, compiled and saved, per package. Null
		// when the run changed nothing.
		if (MutationOutcomes.IsValid())
		{
			Outcome->SetObjectField(TEXT("mutation_outcomes"), MutationOutcomes);
		}
		else
		{
			Outcome->SetField(TEXT("mutation_outcomes"), MakeShared<FJsonValueNull>());
		}
		Outcome->SetStringField(TEXT("final_text"), FinalText);
		J->SetObjectField(TEXT("outcome"), Outcome);
	}
	else
	{
		J->SetField(TEXT("outcome"), MakeShared<FJsonValueNull>());
	}
	return J;
}

// ── Registry ──────────────────────────────────────────────────────────────────

void FKismetAIExternalRunRegistry::Add(const FKismetAIExternalRunRef& Record)
{
	{
		FScopeLock Lock(&Mutex);
		Records.Add(Record);
	}
	Prune();
}

FKismetAIExternalRunPtr FKismetAIExternalRunRegistry::Find(const FString& RunId) const
{
	FScopeLock Lock(&Mutex);
	for (const FKismetAIExternalRunRef& Record : Records)
	{
		if (Record->GetRunId() == RunId)
		{
			return Record;
		}
	}
	return nullptr;
}

FKismetAIExternalRunPtr FKismetAIExternalRunRegistry::FindByClientRequestId(const FString& ClientRequestId) const
{
	if (ClientRequestId.IsEmpty())
	{
		return nullptr;
	}
	FScopeLock Lock(&Mutex);
	// Newest first: an id is only ever bound to one live record, but an
	// evicted one may be reused later.
	for (int32 Index = Records.Num() - 1; Index >= 0; --Index)
	{
		if (Records[Index]->GetClientRequestId() == ClientRequestId)
		{
			return Records[Index];
		}
	}
	return nullptr;
}

void FKismetAIExternalRunRegistry::Prune(const FDateTime& Now)
{
	FScopeLock Lock(&Mutex);
	// TTL first.
	Records.RemoveAll([this, &Now](const FKismetAIExternalRunRef& Record)
	{
		return Record->IsFinished() && (Now - Record->GetFinishedAt()) > Limits.FinishedTtl;
	});
	// Then the count bound, oldest finished first. Active runs never count.
	int32 Finished = 0;
	for (const FKismetAIExternalRunRef& Record : Records)
	{
		Finished += Record->IsFinished() ? 1 : 0;
	}
	int32 Excess = Finished - Limits.MaxFinishedRuns;
	for (int32 Index = 0; Index < Records.Num() && Excess > 0;)
	{
		if (Records[Index]->IsFinished())
		{
			Records.RemoveAt(Index);
			--Excess;
		}
		else
		{
			++Index;
		}
	}
}

int32 FKismetAIExternalRunRegistry::Num() const
{
	FScopeLock Lock(&Mutex);
	return Records.Num();
}
