// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIHistoryRepair.h"
#include "KismetAILog.h"

DEFINE_LOG_CATEGORY_STATIC(LogKismetAIHistoryRepair, Log, All);

namespace KismetAIHistoryRepair
{

namespace
{
	FString TypeOf(const TSharedPtr<FJsonValue>& Block)
	{
		const TSharedPtr<FJsonObject> Obj = Block.IsValid() ? Block->AsObject() : nullptr;
		FString Type;
		if (Obj.IsValid())
		{
			Obj->TryGetStringField(TEXT("type"), Type);
		}
		return Type;
	}

	const TArray<TSharedPtr<FJsonValue>>* ContentArrayOf(const TSharedPtr<FJsonValue>& Message)
	{
		const TSharedPtr<FJsonObject> Obj = Message.IsValid() ? Message->AsObject() : nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
		if (Obj.IsValid() && Obj->TryGetArrayField(TEXT("content"), Content))
		{
			return Content;
		}
		return nullptr;
	}

	struct FCall
	{
		FString OriginalId;
		FString FinalId;
		FString Name;
	};

	TArray<FCall> CollectCalls(const TArray<TSharedPtr<FJsonValue>>& Content)
	{
		TArray<FCall> Calls;
		for (const TSharedPtr<FJsonValue>& Block : Content)
		{
			if (TypeOf(Block) != TEXT("tool_use"))
			{
				continue;
			}
			const TSharedPtr<FJsonObject> Obj = Block->AsObject();
			FCall Call;
			Obj->TryGetStringField(TEXT("id"), Call.OriginalId);
			if (Call.OriginalId.IsEmpty())
			{
				continue;
			}
			Obj->TryGetStringField(TEXT("name"), Call.Name);
			Call.FinalId = Call.OriginalId;
			Calls.Add(MoveTemp(Call));
		}
		return Calls;
	}

	TSharedPtr<FJsonValue> CopyWithStringField(const TSharedPtr<FJsonValue>& Value, const TCHAR* Field, const FString& NewValue)
	{
		TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
		Copy->Values = Value->AsObject()->Values;
		Copy->SetStringField(Field, NewValue);
		return MakeShared<FJsonValueObject>(Copy);
	}

	TSharedPtr<FJsonValue> CopyWithContent(const TSharedPtr<FJsonValue>& Message, const TArray<TSharedPtr<FJsonValue>>& Content)
	{
		TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
		Copy->Values = Message->AsObject()->Values;
		Copy->SetArrayField(TEXT("content"), Content);
		return MakeShared<FJsonValueObject>(Copy);
	}

	TSharedPtr<FJsonValue> MakeEnvelope(const TArray<TSharedPtr<FJsonValue>>& Content)
	{
		TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), TEXT("user"));
		Msg->SetArrayField(TEXT("content"), Content);
		return MakeShared<FJsonValueObject>(Msg);
	}

	bool SamePointers(const TArray<TSharedPtr<FJsonValue>>& A, const TArray<TSharedPtr<FJsonValue>>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (int32 i = 0; i < A.Num(); ++i)
		{
			if (A[i] != B[i])
			{
				return false;
			}
		}
		return true;
	}

	bool SameContent(const TArray<TSharedPtr<FJsonValue>>& A, const TArray<TSharedPtr<FJsonValue>>& B)
	{
		return FJsonValue::CompareEqual(FJsonValueArray(A), FJsonValueArray(B));
	}
}

EMessageKind Classify(const TSharedPtr<FJsonValue>& Message)
{
	const TSharedPtr<FJsonObject> Obj = Message.IsValid() ? Message->AsObject() : nullptr;
	FString Role;
	if (!Obj.IsValid() || !Obj->TryGetStringField(TEXT("role"), Role))
	{
		return EMessageKind::Other;
	}

	if (Role == TEXT("user"))
	{
		// String content, or blocks with no tool_result (text, image,
		// document...): words or attachments someone sent. Protected.
		if (const TArray<TSharedPtr<FJsonValue>>* Content = ContentArrayOf(Message))
		{
			for (const TSharedPtr<FJsonValue>& Block : *Content)
			{
				if (TypeOf(Block) == TEXT("tool_result"))
				{
					return EMessageKind::ToolResultEnvelope;
				}
			}
		}
		return EMessageKind::GenuineUser;
	}

	if (Role == TEXT("assistant"))
	{
		if (const TArray<TSharedPtr<FJsonValue>>* Content = ContentArrayOf(Message))
		{
			for (const TSharedPtr<FJsonValue>& Block : *Content)
			{
				FString Id;
				if (TypeOf(Block) == TEXT("tool_use") && Block->AsObject()->TryGetStringField(TEXT("id"), Id) && !Id.IsEmpty())
				{
					return EMessageKind::AssistantToolBatch;
				}
			}
		}
	}
	return EMessageKind::Other;
}

bool FRepairReport::HasChanges() const
{
	return UnrecordedResults || InsertedEnvelopes || AmbiguousResults || RenamedToolUseIds
		|| DroppedDuplicateTurns || DroppedOrphanResults || DroppedExtraResults || !bGenuineUserMessagesPreserved;
}

FString FRepairReport::ToString() const
{
	return FString::Printf(
		TEXT("unrecorded=%d inserted_envelopes=%d ambiguous=%d renamed_ids=%d dropped_duplicate_turns=%d dropped_orphan_results=%d dropped_extra_results=%d genuine_user_messages=%d preserved=%s"),
		UnrecordedResults, InsertedEnvelopes, AmbiguousResults, RenamedToolUseIds, DroppedDuplicateTurns,
		DroppedOrphanResults, DroppedExtraResults, GenuineUserMessages,
		bGenuineUserMessagesPreserved ? TEXT("true") : TEXT("false"));
}

TSharedPtr<FJsonObject> FRepairReport::ToJson() const
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("unrecorded_results"), UnrecordedResults);
	Obj->SetNumberField(TEXT("inserted_envelopes"), InsertedEnvelopes);
	Obj->SetNumberField(TEXT("ambiguous_results"), AmbiguousResults);
	Obj->SetNumberField(TEXT("renamed_tool_use_ids"), RenamedToolUseIds);
	Obj->SetNumberField(TEXT("dropped_duplicate_turns"), DroppedDuplicateTurns);
	Obj->SetNumberField(TEXT("dropped_orphan_results"), DroppedOrphanResults);
	Obj->SetNumberField(TEXT("dropped_extra_results"), DroppedExtraResults);
	Obj->SetNumberField(TEXT("genuine_user_messages"), GenuineUserMessages);
	Obj->SetBoolField(TEXT("genuine_user_messages_preserved"), bGenuineUserMessagesPreserved);
	return Obj;
}

FRepairReport RepairToolPairing(TArray<TSharedPtr<FJsonValue>>& Messages)
{
	using namespace KismetAIToolCallJournal;

	FRepairReport Report;
	const TArray<TSharedPtr<FJsonValue>> In = Messages;

	TArray<TSharedPtr<FJsonValue>> Genuine;
	for (const TSharedPtr<FJsonValue>& M : In)
	{
		if (IsGenuineUserMessage(M))
		{
			Genuine.Add(M);
		}
	}
	Report.GenuineUserMessages = Genuine.Num();

	TArray<TSharedPtr<FJsonValue>> Out;
	Out.Reserve(In.Num() + 2);

	// Every tool_use id already emitted, and which kept turns carried each
	// original id (to tell a re-persisted copy from a reused id).
	TSet<FString> UsedIds;
	TMap<FString, TArray<TSharedPtr<FJsonValue>>> KeptTurnsByOriginalId;

	for (int32 i = 0; i < In.Num(); )
	{
		const TSharedPtr<FJsonValue>& Cur = In[i];
		const EMessageKind Kind = Classify(Cur);

		if (Kind == EMessageKind::ToolResultEnvelope)
		{
			// Not directly after a tool batch (that case is consumed below), so
			// none of its tool_result blocks answers anything. Keep whatever else
			// it carries.
			TArray<TSharedPtr<FJsonValue>> Kept;
			for (const TSharedPtr<FJsonValue>& Block : *ContentArrayOf(Cur))
			{
				if (TypeOf(Block) == TEXT("tool_result"))
				{
					++Report.DroppedOrphanResults;
					continue;
				}
				Kept.Add(Block);
			}
			KISMETAI_LOG(LogKismetAIHistoryRepair, Warning,
				TEXT("Removing orphaned tool_result blocks at message %d (no tool_use right before them)"), i);
			if (Kept.Num() > 0)
			{
				Out.Add(CopyWithContent(Cur, Kept));
			}
			++i;
			continue;
		}

		if (Kind != EMessageKind::AssistantToolBatch)
		{
			// Genuine user messages, assistant text, system messages: as is.
			Out.Add(Cur);
			++i;
			continue;
		}

		const TArray<TSharedPtr<FJsonValue>>& Content = *ContentArrayOf(Cur);
		TArray<FCall> Calls = CollectCalls(Content);
		const bool bHasEnvelope = i + 1 < In.Num() && Classify(In[i + 1]) == EMessageKind::ToolResultEnvelope;

		// 1. A byte-identical copy of a turn already kept: a double-written line.
		{
			bool bCopy = false;
			for (const FCall& Call : Calls)
			{
				if (const TArray<TSharedPtr<FJsonValue>>* Earlier = KeptTurnsByOriginalId.Find(Call.OriginalId))
				{
					for (const TSharedPtr<FJsonValue>& E : *Earlier)
					{
						if (SameContent(*ContentArrayOf(E), Content))
						{
							bCopy = true;
							break;
						}
					}
				}
				if (bCopy)
				{
					break;
				}
			}
			if (bCopy)
			{
				++Report.DroppedDuplicateTurns;
				KISMETAI_LOG(LogKismetAIHistoryRepair, Warning,
					TEXT("Dropping a repeated copy of an earlier assistant turn at message %d (%d tool_use)"), i, Calls.Num());
				++i;
				if (bHasEnvelope)
				{
					// The copy's own results go with it; anything else stays.
					TSet<FString> CopyIds;
					for (const FCall& Call : Calls) { CopyIds.Add(Call.OriginalId); }
					TArray<TSharedPtr<FJsonValue>> Kept;
					for (const TSharedPtr<FJsonValue>& Block : *ContentArrayOf(In[i]))
					{
						FString Id;
						if (TypeOf(Block) == TEXT("tool_result"))
						{
							Block->AsObject()->TryGetStringField(TEXT("tool_use_id"), Id);
							if (!CopyIds.Contains(Id))
							{
								++Report.DroppedOrphanResults;
							}
							continue;
						}
						Kept.Add(Block);
					}
					if (Kept.Num() > 0)
					{
						Out.Add(CopyWithContent(In[i], Kept));
					}
					++i;
				}
				continue;
			}
		}

		// 2. Final ids: unique across everything emitted so far.
		TMap<FString, int32> Occurrences;
		TSet<FString> Taken = UsedIds;
		for (const FCall& Call : Calls)
		{
			++Occurrences.FindOrAdd(Call.OriginalId);
			Taken.Add(Call.OriginalId);
		}
		bool bRenamed = false;
		{
			TSet<FString> SeenInTurn;
			for (FCall& Call : Calls)
			{
				if (UsedIds.Contains(Call.OriginalId) || SeenInTurn.Contains(Call.OriginalId))
				{
					Call.FinalId = MakeDuplicateIdAlias(Call.OriginalId, Taken);
					Taken.Add(Call.FinalId);
					++Report.RenamedToolUseIds;
					bRenamed = true;
					KISMETAI_LOG(LogKismetAIHistoryRepair, Warning,
						TEXT("tool_use id %s at message %d is already taken; renamed to %s"), *Call.OriginalId, i, *Call.FinalId);
				}
				SeenInTurn.Add(Call.OriginalId);
			}
		}

		// 3. One result per call, in call order, then the envelope's other blocks.
		TMap<FString, TArray<TSharedPtr<FJsonValue>>> ResultsById;
		TArray<TSharedPtr<FJsonValue>> OtherBlocks;
		const TArray<TSharedPtr<FJsonValue>>* EnvelopeContent = bHasEnvelope ? ContentArrayOf(In[i + 1]) : nullptr;
		if (EnvelopeContent)
		{
			for (const TSharedPtr<FJsonValue>& Block : *EnvelopeContent)
			{
				if (TypeOf(Block) == TEXT("tool_result"))
				{
					FString Id;
					Block->AsObject()->TryGetStringField(TEXT("tool_use_id"), Id);
					ResultsById.FindOrAdd(Id).Add(Block);
				}
				else
				{
					OtherBlocks.Add(Block);
				}
			}
		}

		TArray<TSharedPtr<FJsonValue>> NewContent;
		TSet<FString> Answered;
		TSet<FString> EvidenceGiven;
		for (const FCall& Call : Calls)
		{
			Answered.Add(Call.OriginalId);
			const TArray<TSharedPtr<FJsonValue>>* Recorded = ResultsById.Find(Call.OriginalId);
			const int32 Sharing = Occurrences.FindRef(Call.OriginalId);

			if (Sharing > 1)
			{
				// Which of the calls sharing this id a recorded result belongs to
				// is unknowable. Quote it once, attribute it to none.
				TArray<FString> Evidence;
				if (Recorded && !EvidenceGiven.Contains(Call.OriginalId))
				{
					for (const TSharedPtr<FJsonValue>& R : *Recorded)
					{
						Evidence.Add(ResultContentText(R->AsObject()));
					}
					EvidenceGiven.Add(Call.OriginalId);
				}
				NewContent.Add(MakeShared<FJsonValueObject>(
					MakeAmbiguousResultBlock(Call.FinalId, Call.Name, Call.OriginalId, Sharing, Evidence)));
				++Report.AmbiguousResults;
				continue;
			}

			if (Recorded && Recorded->Num() > 0)
			{
				if (Recorded->Num() > 1)
				{
					Report.DroppedExtraResults += Recorded->Num() - 1;
					KISMETAI_LOG(LogKismetAIHistoryRepair, Warning,
						TEXT("%d results recorded for %s at message %d; keeping the first"), Recorded->Num(), *Call.OriginalId, i + 1);
				}
				const TSharedPtr<FJsonValue>& First = (*Recorded)[0];
				NewContent.Add(Call.FinalId == Call.OriginalId
					? First
					: CopyWithStringField(First, TEXT("tool_use_id"), Call.FinalId));
				continue;
			}

			NewContent.Add(MakeShared<FJsonValueObject>(MakeUnrecordedResultBlock(Call.FinalId, Call.Name)));
			++Report.UnrecordedResults;
			KISMETAI_LOG(LogKismetAIHistoryRepair, Warning,
				TEXT("No result for %s (%s) after message %d; answering it with OUTCOME_UNRECORDED"), *Call.FinalId, *Call.Name, i);
		}
		for (const TPair<FString, TArray<TSharedPtr<FJsonValue>>>& Pair : ResultsById)
		{
			if (!Answered.Contains(Pair.Key))
			{
				Report.DroppedOrphanResults += Pair.Value.Num();
				KISMETAI_LOG(LogKismetAIHistoryRepair, Warning,
					TEXT("Removing %d tool_result block(s) for unknown id %s at message %d"), Pair.Value.Num(), *Pair.Key, i + 1);
			}
		}
		NewContent.Append(OtherBlocks);

		// Emit the batch (renamed into a copy when needed) and its envelope.
		if (bRenamed)
		{
			TArray<TSharedPtr<FJsonValue>> NewBlocks;
			int32 CallIndex = 0;
			for (const TSharedPtr<FJsonValue>& Block : Content)
			{
				FString Id;
				if (TypeOf(Block) == TEXT("tool_use") && Block->AsObject()->TryGetStringField(TEXT("id"), Id) && !Id.IsEmpty())
				{
					const FCall& Call = Calls[CallIndex++];
					NewBlocks.Add(Call.FinalId == Id ? Block : CopyWithStringField(Block, TEXT("id"), Call.FinalId));
					continue;
				}
				NewBlocks.Add(Block);
			}
			Out.Add(CopyWithContent(Cur, NewBlocks));
		}
		else
		{
			Out.Add(Cur);
		}

		if (EnvelopeContent && SamePointers(*EnvelopeContent, NewContent))
		{
			Out.Add(In[i + 1]);
		}
		else if (EnvelopeContent)
		{
			Out.Add(CopyWithContent(In[i + 1], NewContent));
		}
		else
		{
			// No envelope at all: the next message is genuine user input, another
			// assistant turn, a system message, or nothing. Answer the batch right
			// here; whatever came next keeps its place after it.
			Out.Add(MakeEnvelope(NewContent));
			++Report.InsertedEnvelopes;
		}

		for (const FCall& Call : Calls)
		{
			UsedIds.Add(Call.FinalId);
			KeptTurnsByOriginalId.FindOrAdd(Call.OriginalId).Add(Cur);
		}
		i += bHasEnvelope ? 2 : 1;
	}

	// The invariant, checked rather than assumed: every genuine user message is
	// in the output, the same object, in the same order.
	{
		int32 Next = 0;
		for (const TSharedPtr<FJsonValue>& M : Out)
		{
			if (Next < Genuine.Num() && M == Genuine[Next])
			{
				++Next;
			}
		}
		if (Next != Genuine.Num())
		{
			Report.bGenuineUserMessagesPreserved = false;
			KISMETAI_LOG(LogKismetAIHistoryRepair, Error,
				TEXT("Pairing repair would have lost %d genuine user message(s); keeping the history unrepaired"),
				Genuine.Num() - Next);
			ensureMsgf(false, TEXT("KismetAIHistoryRepair: genuine user message lost"));
			return Report;
		}
	}

	Messages = MoveTemp(Out);
	return Report;
}

}
