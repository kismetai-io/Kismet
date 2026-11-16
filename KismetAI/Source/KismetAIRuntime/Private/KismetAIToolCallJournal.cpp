// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIToolCallJournal.h"
#include "KismetAILog.h"

#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogKismetAIToolCallJournal, Log, All);

namespace KismetAIToolCallJournal
{

const TCHAR* StatusToString(ECallStatus Status)
{
	switch (Status)
	{
	case ECallStatus::Succeeded:      return TEXT("succeeded");
	case ECallStatus::Failed:         return TEXT("failed");
	case ECallStatus::ShortCircuited: return TEXT("short_circuited");
	case ECallStatus::NotStarted:
	default:                          return TEXT("not_started");
	}
}

FString MakeRunId()
{
	return FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
}

FString MakeTurnId(const FString& RunId, int32 TurnIndex)
{
	return FString::Printf(TEXT("%s/t%d"), *RunId, TurnIndex);
}

FString MakeCallId(const FString& TurnId, int32 CallIndex)
{
	return FString::Printf(TEXT("%s/c%d"), *TurnId, CallIndex);
}

FString MakeRequestId(const FString& TurnId, int32 RequestIndex)
{
	return FString::Printf(TEXT("%s/r%d"), *TurnId, RequestIndex);
}

FString MakeAttemptId(const FString& RequestId, int32 AttemptIndex)
{
	return FString::Printf(TEXT("%s/a%d"), *RequestId, AttemptIndex);
}

FString MakeMessageId(const FString& TurnId)
{
	return FString::Printf(TEXT("%s/m"), *TurnId);
}

void StampToolBatch(const TSharedPtr<FJsonObject>& AssistantEntry,
	const FString& RunId, const FString& TurnId, int32 CallCount)
{
	if (!AssistantEntry.IsValid())
	{
		return;
	}
	AssistantEntry->SetStringField(TEXT("run_id"), RunId);
	AssistantEntry->SetStringField(TEXT("turn_id"), TurnId);
	AssistantEntry->SetNumberField(TEXT("tool_call_count"), CallCount);
}

TSharedPtr<FJsonObject> BuildCallEntry(const FCallRecord& Record)
{
	TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("type"), TEXT("tool_result_message"));
	Entry->SetStringField(TEXT("record"), RecordToolCall);
	Entry->SetNumberField(TEXT("schema"), SchemaVersion);
	Entry->SetStringField(TEXT("uuid"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
	Entry->SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());
	Entry->SetStringField(TEXT("run_id"), Record.RunId);
	Entry->SetStringField(TEXT("turn_id"), Record.TurnId);
	Entry->SetStringField(TEXT("call_id"), MakeCallId(Record.TurnId, Record.CallIndex));
	Entry->SetNumberField(TEXT("call_index"), Record.CallIndex);
	Entry->SetNumberField(TEXT("call_count"), Record.CallCount);
	Entry->SetStringField(TEXT("tool_use_id"), Record.ToolUseId);
	Entry->SetStringField(TEXT("tool_name"), Record.ToolName);
	Entry->SetBoolField(TEXT("is_mutation"), Record.bIsMutation);
	Entry->SetStringField(TEXT("status"), StatusToString(Record.Status));
	Entry->SetBoolField(TEXT("attempted"), Record.WasAttempted());
	if (Record.Status == ECallStatus::NotStarted)
	{
		Entry->SetStringField(TEXT("not_started_reason"), TEXT("cancelled"));
	}
	if (Record.bCancelRequestedWhileRunning)
	{
		Entry->SetBoolField(TEXT("cancel_requested_while_running"), true);
	}
	if (Record.MutationOutcome.IsValid())
	{
		Entry->SetObjectField(TEXT("mutation_outcome"), Record.MutationOutcome);
	}

	TArray<TSharedPtr<FJsonValue>> Results;
	if (Record.ResultBlock.IsValid())
	{
		Results.Add(MakeShared<FJsonValueObject>(Record.ResultBlock));
	}
	Entry->SetArrayField(TEXT("results"), Results);
	return Entry;
}

TSharedPtr<FJsonObject> BuildNoteEntry(const FString& RunId, const FString& TurnId,
	const TArray<TSharedPtr<FJsonValue>>& Blocks)
{
	TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("type"), TEXT("tool_result_message"));
	Entry->SetStringField(TEXT("record"), RecordTurnNote);
	Entry->SetNumberField(TEXT("schema"), SchemaVersion);
	Entry->SetStringField(TEXT("uuid"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
	Entry->SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());
	Entry->SetStringField(TEXT("run_id"), RunId);
	Entry->SetStringField(TEXT("turn_id"), TurnId);
	Entry->SetArrayField(TEXT("results"), Blocks);
	return Entry;
}

namespace
{
	TSharedPtr<FJsonObject> MakeErrorResultBlock(const FString& ToolUseId, const TCHAR* Code, const FString& Message)
	{
		// Same envelope every tool error uses, so IsToolResultError and the
		// model read it like any other failure.
		TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetStringField(TEXT("error_code"), Code);
		Error->SetStringField(TEXT("message"), Message);
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetBoolField(TEXT("success"), false);
		Body->SetObjectField(TEXT("error"), Error);

		FString BodyJson;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&BodyJson);
		FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);

		TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
		Block->SetStringField(TEXT("type"), TEXT("tool_result"));
		Block->SetStringField(TEXT("tool_use_id"), ToolUseId);
		Block->SetStringField(TEXT("content"), FString::Printf(TEXT("<tool_use_error>%s</tool_use_error>"), *BodyJson));
		Block->SetBoolField(TEXT("is_error"), true);
		return Block;
	}
}

TSharedPtr<FJsonObject> MakeNotStartedResultBlock(const FString& ToolUseId, const FString& ToolName)
{
	return MakeErrorResultBlock(ToolUseId, NotExecutedCode, FString::Printf(
		TEXT("'%s' was not executed: the run was cancelled before this call started. Nothing was changed by this call."),
		*ToolName));
}

TSharedPtr<FJsonObject> MakeUnrecordedResultBlock(const FString& ToolUseId, const FString& ToolName, const FString& ReconciliationNote)
{
	FString Message = FString::Printf(
		TEXT("No outcome was recorded for '%s'. The editor stopped, or the session log could not be written, after this call was issued, so it may or may not have run and may have changed the asset. Re-read the affected asset before acting; do not assume this call succeeded, failed or was rolled back, and do not replay it blindly."),
		*ToolName);
	if (!ReconciliationNote.IsEmpty())
	{
		Message += TEXT("\n") + ReconciliationNote;
	}
	return MakeErrorResultBlock(ToolUseId, OutcomeUnrecordedCode, Message);
}

FString ResultContentText(const TSharedPtr<FJsonObject>& ResultBlock)
{
	if (!ResultBlock.IsValid())
	{
		return FString();
	}
	FString Text;
	if (ResultBlock->TryGetStringField(TEXT("content"), Text))
	{
		return Text;
	}
	const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
	if (ResultBlock->TryGetArrayField(TEXT("content"), Parts))
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(*Parts, Writer);
		return Out;
	}
	return FString();
}

TSharedPtr<FJsonObject> MakeAmbiguousResultBlock(const FString& ToolUseId, const FString& ToolName,
	const FString& SharedId, int32 CallsSharingId, const TArray<FString>& RecordedEvidence)
{
	FString Message = FString::Printf(
		TEXT("The outcome of '%s' is unknown: %d calls in this turn were issued with the same id '%s', so a recorded result cannot be attributed to any one of them. It may or may not have run and may have changed the asset. Re-read the affected asset before acting; do not assume this call succeeded, failed or was rolled back, and do not replay it blindly."),
		*ToolName, CallsSharingId, *SharedId);
	if (RecordedEvidence.Num() > 0)
	{
		Message += FString::Printf(TEXT(" %d result(s) were recorded under '%s' and may belong to any of those calls: "),
			RecordedEvidence.Num(), *SharedId);
		for (int32 i = 0; i < RecordedEvidence.Num(); ++i)
		{
			Message += FString::Printf(TEXT("[unattributed result %d] %s "), i + 1, *RecordedEvidence[i]);
		}
	}
	return MakeErrorResultBlock(ToolUseId, OutcomeAmbiguousCode, Message.TrimEnd());
}

FString MakeDuplicateIdAlias(const FString& Id, const TSet<FString>& Taken)
{
	for (int32 N = 1; ; ++N)
	{
		FString Alias = FString::Printf(TEXT("%s_dup%d"), *Id, N);
		if (!Taken.Contains(Alias))
		{
			return Alias;
		}
	}
}

bool IsExecutorCancelledResult(const FString& Result)
{
	return Result.Contains(TEXT("\"error_code\":\"CANCELLED\""))
		&& Result.Contains(TEXT("was cancelled before it ran"));
}

// ─────────────────────────────────────────────────────────────────────────────
// FResumeProjector
// ─────────────────────────────────────────────────────────────────────────────

void FResumeProjector::BeforeMessageEntry(TArray<TSharedPtr<FJsonValue>>& Messages)
{
	Close(Messages);
}

void FResumeProjector::OnAssistantAppended(const TSharedPtr<FJsonObject>& Entry, TArray<TSharedPtr<FJsonValue>>& Messages)
{
	Close(Messages);

	FString TurnId;
	if (!Entry.IsValid() || !Entry->TryGetStringField(TEXT("turn_id"), TurnId) || TurnId.IsEmpty())
	{
		return;  // legacy or non-batch assistant message
	}

	const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
	if (!Entry->TryGetArrayField(TEXT("content"), Content))
	{
		return;
	}
	TArray<TPair<FString, FString>> Calls;
	TArray<TSharedPtr<FJsonObject>> Inputs;
	for (const TSharedPtr<FJsonValue>& Block : *Content)
	{
		const TSharedPtr<FJsonObject> Obj = Block.IsValid() ? Block->AsObject() : nullptr;
		FString Type, Id, Name;
		if (Obj.IsValid() && Obj->TryGetStringField(TEXT("type"), Type) && Type == TEXT("tool_use")
			&& Obj->TryGetStringField(TEXT("id"), Id) && !Id.IsEmpty())
		{
			Obj->TryGetStringField(TEXT("name"), Name);
			Calls.Emplace(Id, Name);
			const TSharedPtr<FJsonObject>* Input = nullptr;
			Inputs.Add(Obj->TryGetObjectField(TEXT("input"), Input) && Input ? *Input : nullptr);
		}
	}
	if (Calls.Num() == 0)
	{
		return;
	}

	bOpen = true;
	OpenTurnId = TurnId;
	AssistantIndex = Messages.Num() - 1;
	ExpectedCalls = MoveTemp(Calls);
	ExpectedInputs = MoveTemp(Inputs);
}

bool FResumeProjector::TryConsumeToolResultEntry(const TSharedPtr<FJsonObject>& Entry, TArray<TSharedPtr<FJsonValue>>& Messages)
{
	FString TurnId;
	const bool bStamped = Entry.IsValid() && Entry->TryGetStringField(TEXT("turn_id"), TurnId) && !TurnId.IsEmpty();
	if (!bOpen || !bStamped || TurnId != OpenTurnId)
	{
		// Not part of the open batch: close it so its user message precedes
		// whatever the caller does with this entry.
		Close(Messages);
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
	if (!Entry->TryGetArrayField(TEXT("results"), Results))
	{
		return true;  // a stamped record with no blocks contributes nothing
	}

	// A tool_call record names its call by position too. That is the exact key
	// when one turn carries the same tool_use id twice.
	int32 CallIndex = INDEX_NONE;
	FString RecordKind;
	const bool bIndexed = Entry->TryGetStringField(TEXT("record"), RecordKind) && RecordKind == RecordToolCall
		&& Entry->TryGetNumberField(TEXT("call_index"), CallIndex)
		&& ExpectedCalls.IsValidIndex(CallIndex);
	for (const TSharedPtr<FJsonValue>& Block : *Results)
	{
		const TSharedPtr<FJsonObject> Obj = Block.IsValid() ? Block->AsObject() : nullptr;
		if (!Obj.IsValid())
		{
			continue;
		}
		FString Type, Id;
		Obj->TryGetStringField(TEXT("type"), Type);
		if (Type == TEXT("tool_result"))
		{
			Obj->TryGetStringField(TEXT("tool_use_id"), Id);
			if (Id.IsEmpty())
			{
				continue;
			}
			if (bIndexed && ExpectedCalls[CallIndex].Key == Id && !ReceivedByIndex.Contains(CallIndex))
			{
				ReceivedByIndex.Add(CallIndex, Block);
				TSharedPtr<FJsonObject> Committed = MakeShared<FJsonObject>();
				FJsonObject::Duplicate(Entry, Committed);
				FString RunId;
				OpenTurnId.Split(TEXT("/"), &RunId, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
				Committed->SetStringField(TEXT("run_id"), RunId);
				Committed->SetStringField(TEXT("turn_id"), OpenTurnId);
				Committed->SetStringField(TEXT("call_id"), MakeCallId(OpenTurnId, CallIndex));
				Committed->SetStringField(TEXT("tool_use_id"), Id);
				Committed->SetStringField(TEXT("tool_name"), ExpectedCalls[CallIndex].Value);
				if (ExpectedInputs.IsValidIndex(CallIndex) && ExpectedInputs[CallIndex].IsValid())
				{
					TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
					FJsonObject::Duplicate(ExpectedInputs[CallIndex], Input);
					Committed->SetObjectField(TEXT("input"), Input);
				}
				CommittedCalls.Add(MoveTemp(Committed));
			}
			if (Received.Contains(Id))
			{
				// One terminal record per call. The writer never emits two; if a
				// damaged journal does, the first (the real outcome) stands.
				KISMETAI_LOG(LogKismetAIToolCallJournal, Warning,
					TEXT("Ignoring a second tool_call record for %s in turn %s"), *Id, *OpenTurnId);
				continue;
			}
			Received.Add(Id, Block);
		}
		else
		{
			Notes.Add(Block);
		}
	}
	return true;
}

void FResumeProjector::Finish(TArray<TSharedPtr<FJsonValue>>& Messages)
{
	Close(Messages);
}

void FResumeProjector::Close(TArray<TSharedPtr<FJsonValue>>& Messages)
{
	if (!bOpen)
	{
		return;
	}

	// How many calls of this turn share each id. Providers require unique ids,
	// and a result keyed only by a shared id cannot say which call it answers.
	TMap<FString, int32> Occurrences;
	TSet<FString> Taken;
	for (const TPair<FString, FString>& Call : ExpectedCalls)
	{
		++Occurrences.FindOrAdd(Call.Key);
		Taken.Add(Call.Key);
	}

	// A record matched to its call by call_index is that call's, and evidence
	// for no other.
	TSet<TSharedPtr<FJsonValue>> AttributedByIndex;
	for (const TPair<int32, TSharedPtr<FJsonValue>>& Pair : ReceivedByIndex)
	{
		AttributedByIndex.Add(Pair.Value);
	}

	// Exactly one tool_result per tool_use, in the assistant's order, then any
	// guidance text — the shape the live loop sends.
	TArray<TSharedPtr<FJsonValue>> Content;
	TArray<FString> FinalIds;
	TSet<FString> SeenInTurn;
	for (int32 Index = 0; Index < ExpectedCalls.Num(); ++Index)
	{
		const FString& Id = ExpectedCalls[Index].Key;
		const FString& Name = ExpectedCalls[Index].Value;
		const bool bShared = Occurrences.FindRef(Id) > 1;

		FString FinalId = Id;
		if (SeenInTurn.Contains(Id))
		{
			FinalId = MakeDuplicateIdAlias(Id, Taken);
			Taken.Add(FinalId);
		}
		SeenInTurn.Add(Id);
		FinalIds.Add(FinalId);

		TSharedPtr<FJsonValue> Result;
		if (const TSharedPtr<FJsonValue>* ByIndex = ReceivedByIndex.Find(Index))
		{
			Result = *ByIndex;
		}
		else if (!bShared)
		{
			if (const TSharedPtr<FJsonValue>* ById = Received.Find(Id))
			{
				Result = *ById;
			}
		}

		if (Result.IsValid())
		{
			if (FinalId != Id)
			{
				TSharedPtr<FJsonObject> Renamed = MakeShared<FJsonObject>();
				Renamed->Values = Result->AsObject()->Values;
				Renamed->SetStringField(TEXT("tool_use_id"), FinalId);
				Result = MakeShared<FJsonValueObject>(Renamed);
			}
			Content.Add(Result);
		}
		else if (bShared && Received.Contains(Id) && !AttributedByIndex.Contains(Received[Id]))
		{
			// Something was recorded under the shared id but not for this call's
			// position: quote it, never attribute it.
			const FString Evidence = ResultContentText(Received[Id]->AsObject());
			KISMETAI_LOG(LogKismetAIToolCallJournal, Warning,
				TEXT("Call %d of turn %s shares id %s and has no record of its own; projecting OUTCOME_AMBIGUOUS"),
				Index, *OpenTurnId, *Id);
			Content.Add(MakeShared<FJsonValueObject>(MakeAmbiguousResultBlock(FinalId, Name, Id, Occurrences.FindRef(Id), { Evidence })));
		}
		else
		{
			++UnrecordedCalls;
			KISMETAI_LOG(LogKismetAIToolCallJournal, Warning,
				TEXT("No outcome recorded for %s (%s) in turn %s; projecting OUTCOME_UNRECORDED"),
				*FinalId, *Name, *OpenTurnId);
			const FString CallId = MakeCallId(OpenTurnId, Index);
			Content.Add(MakeShared<FJsonValueObject>(MakeUnrecordedResultBlock(FinalId, Name, ReconciliationNotes.FindRef(CallId))));
			FUnrecordedCall& Unrecorded = UnrecordedCallList.AddDefaulted_GetRef();
			Unrecorded.TurnId = OpenTurnId;
			Unrecorded.CallIndex = Index;
			Unrecorded.ToolUseId = FinalId;
			Unrecorded.ToolName = Name;
			Unrecorded.Input = ExpectedInputs.IsValidIndex(Index) ? ExpectedInputs[Index] : nullptr;
		}
	}
	Content.Append(Notes);

	// Renamed duplicates: rewrite the projected assistant message to match. The
	// k-th tool_use there is ExpectedCalls[k] (the loader only drops thinking).
	bool bAnyRenamed = false;
	for (int32 Index = 0; Index < FinalIds.Num(); ++Index)
	{
		bAnyRenamed |= FinalIds[Index] != ExpectedCalls[Index].Key;
	}
	if (bAnyRenamed && Messages.IsValidIndex(AssistantIndex))
	{
		const TSharedPtr<FJsonObject> Assistant = Messages[AssistantIndex]->AsObject();
		const TArray<TSharedPtr<FJsonValue>>* Blocks = nullptr;
		if (Assistant.IsValid() && Assistant->TryGetArrayField(TEXT("content"), Blocks))
		{
			TArray<TSharedPtr<FJsonValue>> NewBlocks;
			int32 ToolUseIndex = 0;
			for (const TSharedPtr<FJsonValue>& Block : *Blocks)
			{
				const TSharedPtr<FJsonObject> Obj = Block.IsValid() ? Block->AsObject() : nullptr;
				FString Type, Id;
				if (Obj.IsValid() && Obj->TryGetStringField(TEXT("type"), Type) && Type == TEXT("tool_use")
					&& Obj->TryGetStringField(TEXT("id"), Id) && !Id.IsEmpty())
				{
					if (FinalIds.IsValidIndex(ToolUseIndex) && FinalIds[ToolUseIndex] != Id)
					{
						TSharedPtr<FJsonObject> Renamed = MakeShared<FJsonObject>();
						Renamed->Values = Obj->Values;
						Renamed->SetStringField(TEXT("id"), FinalIds[ToolUseIndex]);
						NewBlocks.Add(MakeShared<FJsonValueObject>(Renamed));
						++ToolUseIndex;
						continue;
					}
					++ToolUseIndex;
				}
				NewBlocks.Add(Block);
			}
			TSharedPtr<FJsonObject> NewAssistant = MakeShared<FJsonObject>();
			NewAssistant->Values = Assistant->Values;
			NewAssistant->SetArrayField(TEXT("content"), NewBlocks);
			Messages[AssistantIndex] = MakeShared<FJsonValueObject>(NewAssistant);
		}
	}

	TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("role"), TEXT("user"));
	Msg->SetArrayField(TEXT("content"), Content);
	Messages.Add(MakeShared<FJsonValueObject>(Msg));

	bOpen = false;
	OpenTurnId.Reset();
	AssistantIndex = INDEX_NONE;
	ExpectedCalls.Reset();
	ExpectedInputs.Reset();
	Received.Reset();
	ReceivedByIndex.Reset();
	Notes.Reset();
}

}
