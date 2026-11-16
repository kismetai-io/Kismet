// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIContextProjection.h"
#include "KismetAIHistoryRepair.h"
#include "KismetAIAuthority.h"

#include "Misc/SecureHash.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace KismetAIContextProjection
{

namespace
{
	const TCHAR* const ClearedMarkerPrefix = TEXT("[tool result cleared, was ");

	TSharedPtr<FJsonObject> ObjectOf(const TSharedPtr<FJsonValue>& Value)
	{
		return Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
	}

	bool StrictString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, FString& Out)
	{
		const auto Value = Object.IsValid() ? Object->TryGetField(Key) : nullptr;
		return Value.IsValid() && Value->Type == EJson::String && Value->TryGetString(Out);
	}

	bool PackageIdentityFitsHint(const FString& Package)
	{
		// A context hint is deliberately smaller than the set of legal asset names.
		// Never shorten an identity: an unsupported name makes the report unavailable.
		if (Package.Len() > 256 || !Package.StartsWith(TEXT("/")) || Package.EndsWith(TEXT("/"))
			|| Package.Contains(TEXT("//"))) return false;
		int32 Slashes = 0;
		for (const TCHAR C : Package)
		{
			if (C == TEXT('/')) { ++Slashes; continue; }
			if (!(C >= TEXT('a') && C <= TEXT('z')) && !(C >= TEXT('A') && C <= TEXT('Z'))
				&& !(C >= TEXT('0') && C <= TEXT('9')) && C != TEXT('_')) return false;
		}
		return Slashes >= 2;
	}

	FString ReportedMutationHint(const FString& Body)
	{
		auto Hint = MakeShared<FJsonObject>();
		Hint->SetStringField(TEXT("provenance"), TEXT("reported_unverified"));
		Hint->SetBoolField(TEXT("outcome_available"), false);
		// Only model-visible text is available here. It cannot certify execution,
		// saving or reconciliation, even when its fields resemble a typed result.
		auto ReadReportedFields = [&]() -> TSharedPtr<FJsonObject>
		{
			if (Body.Len() > 256 * 1024) return nullptr;
			FString Json = Body.TrimStartAndEnd();
			if (Json.RemoveFromStart(TEXT("<tool_use_error>")) && !Json.RemoveFromEnd(TEXT("</tool_use_error>"))) return nullptr;
			TSharedPtr<FJsonObject> Envelope;
			if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Envelope) || !Envelope.IsValid()) return nullptr;
			const TSharedPtr<FJsonObject>* Data = nullptr;
			const TSharedPtr<FJsonObject>* Outcome = nullptr;
			if (!Envelope->TryGetObjectField(TEXT("data"), Data) || !Data || !Data->IsValid()
				|| !(*Data)->TryGetObjectField(TEXT("outcome"), Outcome) || !Outcome || !Outcome->IsValid()) return nullptr;
			FString Package, Compile, Save, Recovery;
			if (!StrictString(*Outcome, TEXT("package"), Package) || !PackageIdentityFitsHint(Package)
				|| !StrictString(*Outcome, TEXT("compile_status"), Compile) || (Compile != TEXT("succeeded") && Compile != TEXT("failed") && Compile != TEXT("not_run"))
				|| !StrictString(*Outcome, TEXT("save_status"), Save) || (Save != TEXT("saved") && Save != TEXT("failed") && Save != TEXT("not_requested"))
				|| !StrictString(*Outcome, TEXT("recovery_status"), Recovery) || (Recovery != TEXT("not_tracked") && Recovery != TEXT("restored")
					&& Recovery != TEXT("partially_restored") && Recovery != TEXT("restore_failed"))) return nullptr;
			const auto Applied = (*Outcome)->TryGetField(TEXT("applied_in_memory"));
			if (!Applied.IsValid() || Applied->Type != EJson::Boolean) return nullptr;
			auto Reported = MakeShared<FJsonObject>();
			Reported->SetStringField(TEXT("package"), Package);
			Reported->SetBoolField(TEXT("applied_in_memory"), Applied->AsBool());
			Reported->SetStringField(TEXT("compile_status"), Compile);
			Reported->SetStringField(TEXT("save_status"), Save);
			Reported->SetStringField(TEXT("recovery_status"), Recovery);
			for (const TCHAR* Key : { TEXT("retry_safe"), TEXT("package_was_dirty_before"), TEXT("save_included_preexisting_changes") })
			{
				const auto Value = (*Outcome)->TryGetField(Key);
				if (!Value.IsValid()) continue;
				if (Value->Type != EJson::Boolean) return nullptr;
				Reported->SetBoolField(Key, Value->AsBool());
			}
			return Reported;
		};
		if (const auto Reported = ReadReportedFields())
		{
			Hint->SetBoolField(TEXT("outcome_available"), true);
			Hint->SetObjectField(TEXT("reported_outcome"), Reported);
		}
		FString Json;
		FJsonSerializer::Serialize(Hint, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
		return TEXT("\n") + Json;
	}

	// Role plus the ids a message carries: what pairing and grouping depend on.
	FString MessageShape(const TSharedPtr<FJsonValue>& Message)
	{
		const TSharedPtr<FJsonObject> Obj = ObjectOf(Message);
		if (!Obj.IsValid())
		{
			return TEXT("?");
		}
		FString Role;
		Obj->TryGetStringField(TEXT("role"), Role);

		TArray<FString> UseIds;
		TArray<FString> ResultIds;
		const TArray<TSharedPtr<FJsonValue>>* Blocks = nullptr;
		if (Obj->TryGetArrayField(TEXT("content"), Blocks))
		{
			for (const TSharedPtr<FJsonValue>& BlockValue : *Blocks)
			{
				const TSharedPtr<FJsonObject> Block = ObjectOf(BlockValue);
				FString Type, Id;
				if (!Block.IsValid() || !Block->TryGetStringField(TEXT("type"), Type))
				{
					continue;
				}
				if (Type == TEXT("tool_use") && Block->TryGetStringField(TEXT("id"), Id))
				{
					UseIds.Add(Id);
				}
				else if (Type == TEXT("tool_result") && Block->TryGetStringField(TEXT("tool_use_id"), Id))
				{
					ResultIds.Add(Id);
				}
			}
		}
		if (Role == TEXT("assistant"))
		{
			return TEXT("A[") + FString::Join(UseIds, TEXT(",")) + TEXT("]");
		}
		if (Role == TEXT("user"))
		{
			return ResultIds.Num() > 0 ? TEXT("R[") + FString::Join(ResultIds, TEXT(",")) + TEXT("]") : FString(TEXT("U"));
		}
		return TEXT("O:") + Role;
	}
}

TSharedPtr<FJsonValue> MakeUserTextMessage(const FString& Text)
{
	TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("role"), TEXT("user"));
	Msg->SetStringField(TEXT("content"), Text);
	return MakeShared<FJsonValueObject>(Msg);
}

FString MakeClearedMarker(int32 OriginalChars)
{
	return FString::Printf(TEXT("%s%d chars]"), ClearedMarkerPrefix, OriginalChars);
}

bool IsClearedMarker(const FString& Body)
{
	return Body.StartsWith(ClearedMarkerPrefix);
}

FString ToolResultBodyText(const TSharedPtr<FJsonObject>& Block)
{
	FString Body;
	if (!Block.IsValid())
	{
		return Body;
	}
	if (Block->TryGetStringField(TEXT("content"), Body))
	{
		return Body;
	}
	const TSharedPtr<FJsonValue> Content = Block->TryGetField(TEXT("content"));
	if (Content.IsValid())
	{
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
		FJsonSerializer::Serialize(Content, FString(), Writer);
	}
	return Body;
}

int32 ClearToolResults(TArray<TSharedPtr<FJsonValue>>& Messages, const TSet<FString>& ToolUseIds,
	TMap<FString, FString>* OutClearedBodies)
{
	if (ToolUseIds.Num() == 0)
	{
		return 0;
	}
	// Association must be unambiguous and adjacent. A read result, an orphan,
	// or a duplicate id cannot borrow a mutation call's reported outcome.
	TMap<FString, int32> UseCounts;
	TMap<FString, int32> MutationMessageIndices;
	for (int32 Index = 0; Index < Messages.Num(); ++Index)
	{
		const auto Message = ObjectOf(Messages[Index]);
		FString Role;
		const TArray<TSharedPtr<FJsonValue>>* Blocks = nullptr;
		if (!StrictString(Message, TEXT("role"), Role) || Role != TEXT("assistant") || !Message->TryGetArrayField(TEXT("content"), Blocks)) continue;
		for (const auto& Value : *Blocks)
		{
			const auto Block = ObjectOf(Value);
			FString Type, Id, Name;
			if (!StrictString(Block, TEXT("type"), Type) || Type != TEXT("tool_use") || !StrictString(Block, TEXT("id"), Id)) continue;
			++UseCounts.FindOrAdd(Id);
			const TSharedPtr<FJsonObject>* Input = nullptr;
			if (StrictString(Block, TEXT("name"), Name) && Block->TryGetObjectField(TEXT("input"), Input) && Input && Input->IsValid()
				&& KismetAIAuthority::DescribeOperation(Name, *Input).RequiresAuthority()) MutationMessageIndices.Add(Id, Index);
		}
	}
	int32 Cleared = 0;
	for (int32 MsgIdx = 0; MsgIdx < Messages.Num(); ++MsgIdx)
	{
		const TSharedPtr<FJsonObject> MsgObj = ObjectOf(Messages[MsgIdx]);
		const TArray<TSharedPtr<FJsonValue>>* ContentArray = nullptr;
		if (!MsgObj.IsValid() || !MsgObj->TryGetArrayField(TEXT("content"), ContentArray))
		{
			continue;
		}

		TArray<TSharedPtr<FJsonValue>> NewContent;
		bool bChanged = false;
		for (int32 BlockIdx = 0; BlockIdx < ContentArray->Num(); ++BlockIdx)
		{
			const TSharedPtr<FJsonObject> Block = ObjectOf((*ContentArray)[BlockIdx]);
			FString Type, ToolUseId;
			if (!Block.IsValid() || !Block->TryGetStringField(TEXT("type"), Type) || Type != TEXT("tool_result")
				|| !Block->TryGetStringField(TEXT("tool_use_id"), ToolUseId) || !ToolUseIds.Contains(ToolUseId))
			{
				continue;
			}
			const FString Body = ToolResultBodyText(Block);
			if (IsClearedMarker(Body))
			{
				continue;  // already cleared on an earlier pass
			}
			if (!bChanged)
			{
				NewContent = *ContentArray;
				bChanged = true;
			}
			TSharedPtr<FJsonObject> NewBlock = MakeShared<FJsonObject>();
			NewBlock->Values = Block->Values;
			FString Marker = MakeClearedMarker(Body.Len());
			FString Role;
			const int32* MutationIndex = MutationMessageIndices.Find(ToolUseId);
			if (StrictString(MsgObj, TEXT("role"), Role) && Role == TEXT("user") && UseCounts.FindRef(ToolUseId) == 1
				&& MutationIndex && *MutationIndex == MsgIdx - 1) Marker += ReportedMutationHint(Body);
			NewBlock->SetStringField(TEXT("content"), Marker);
			NewContent[BlockIdx] = MakeShared<FJsonValueObject>(NewBlock);
			if (OutClearedBodies && !OutClearedBodies->Contains(ToolUseId))
			{
				OutClearedBodies->Add(ToolUseId, Body);
			}
			++Cleared;
		}
		if (bChanged)
		{
			TSharedPtr<FJsonObject> NewMsg = MakeShared<FJsonObject>();
			NewMsg->Values = MsgObj->Values;
			NewMsg->SetArrayField(TEXT("content"), NewContent);
			Messages[MsgIdx] = MakeShared<FJsonValueObject>(NewMsg);
		}
	}
	return Cleared;
}

FString ComputeAnchor(const TArray<TSharedPtr<FJsonValue>>& Messages)
{
	FString Shape;
	for (const TSharedPtr<FJsonValue>& Message : Messages)
	{
		Shape += MessageShape(Message);
		Shape += TEXT("|");
	}
	const FTCHARToUTF8 Utf8(*Shape);
	uint8 Digest[20];
	FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
	return BytesToHex(Digest, 20).ToLower();
}

TArray<TSharedPtr<FJsonValue>> BuildSummarizedProjection(const TArray<TSharedPtr<FJsonValue>>& Messages,
	int32 HeadCount, int32 TailCount, const TSharedPtr<FJsonValue>& SummaryMessage, int32* OutRetainedUserMessages)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	const int32 SpanEnd = Messages.Num() - TailCount;
	int32 Retained = 0;
	for (int32 i = 0; i < HeadCount; ++i)
	{
		Out.Add(Messages[i]);
	}
	Out.Add(SummaryMessage);
	for (int32 i = HeadCount; i < SpanEnd; ++i)
	{
		if (KismetAIHistoryRepair::IsGenuineUserMessage(Messages[i]))
		{
			Out.Add(Messages[i]);
			++Retained;
		}
	}
	for (int32 i = SpanEnd; i < Messages.Num(); ++i)
	{
		Out.Add(Messages[i]);
	}
	if (OutRetainedUserMessages)
	{
		*OutRetainedUserMessages = Retained;
	}
	return Out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Records
// ─────────────────────────────────────────────────────────────────────────────

const TCHAR* FProjectionRecord::OpToString(EProjectionOp Op)
{
	return Op == EProjectionOp::Summarize ? TEXT("summarize") : TEXT("microcompact");
}

TSharedPtr<FJsonObject> FProjectionRecord::ToJson() const
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("op"), OpToString(Op));
	Obj->SetNumberField(TEXT("messages_before"), MessagesBefore);
	Obj->SetStringField(TEXT("anchor"), Anchor);
	TArray<TSharedPtr<FJsonValue>> Ids;
	for (const FString& Id : ClearedToolUseIds)
	{
		Ids.Add(MakeShared<FJsonValueString>(Id));
	}
	Obj->SetArrayField(TEXT("cleared_tool_use_ids"), Ids);
	if (Op == EProjectionOp::Summarize)
	{
		Obj->SetNumberField(TEXT("head_messages"), HeadMessages);
		Obj->SetNumberField(TEXT("tail_messages"), TailMessages);
		Obj->SetNumberField(TEXT("retained_user_messages"), RetainedUserMessages);
		Obj->SetStringField(TEXT("summary_message"), SummaryMessageText);
	}
	return Obj;
}

bool FProjectionRecord::FromJson(const TSharedPtr<FJsonObject>& Obj, FProjectionRecord& Out, FString& OutError)
{
	Out = FProjectionRecord();
	if (!Obj.IsValid())
	{
		OutError = TEXT("projection is not an object");
		return false;
	}
	FString Op;
	Obj->TryGetStringField(TEXT("op"), Op);
	if (Op == TEXT("microcompact"))
	{
		Out.Op = EProjectionOp::Microcompact;
	}
	else if (Op == TEXT("summarize"))
	{
		Out.Op = EProjectionOp::Summarize;
	}
	else
	{
		OutError = FString::Printf(TEXT("unknown projection op '%s'"), *Op);
		return false;
	}

	double Number = 0.0;
	if (!Obj->TryGetNumberField(TEXT("messages_before"), Number) || Number < 0.0)
	{
		OutError = TEXT("messages_before missing");
		return false;
	}
	Out.MessagesBefore = (int32)Number;
	if (!Obj->TryGetStringField(TEXT("anchor"), Out.Anchor) || Out.Anchor.IsEmpty())
	{
		OutError = TEXT("anchor missing");
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Ids = nullptr;
	if (Obj->TryGetArrayField(TEXT("cleared_tool_use_ids"), Ids))
	{
		for (const TSharedPtr<FJsonValue>& Id : *Ids)
		{
			FString S;
			if (Id.IsValid() && Id->TryGetString(S) && !S.IsEmpty())
			{
				Out.ClearedToolUseIds.Add(S);
			}
		}
	}

	if (Out.Op == EProjectionOp::Summarize)
	{
		double Head = 0.0, Tail = 0.0, Retained = 0.0;
		if (!Obj->TryGetNumberField(TEXT("head_messages"), Head) || !Obj->TryGetNumberField(TEXT("tail_messages"), Tail)
			|| Head < 0.0 || Tail < 0.0)
		{
			OutError = TEXT("summary boundary missing");
			return false;
		}
		Out.HeadMessages = (int32)Head;
		Out.TailMessages = (int32)Tail;
		Obj->TryGetNumberField(TEXT("retained_user_messages"), Retained);
		Out.RetainedUserMessages = (int32)Retained;
		if (!Obj->TryGetStringField(TEXT("summary_message"), Out.SummaryMessageText) || Out.SummaryMessageText.IsEmpty())
		{
			OutError = TEXT("summary_message missing");
			return false;
		}
	}
	return true;
}

EApplyResult Apply(const FProjectionRecord& Record, TArray<TSharedPtr<FJsonValue>>& Messages,
	TMap<FString, FString>* OutClearedBodies, FString* OutDetail)
{
	auto Say = [OutDetail](const FString& Text)
	{
		if (OutDetail) { *OutDetail = Text; }
	};

	if (Messages.Num() != Record.MessagesBefore)
	{
		Say(FString::Printf(TEXT("decided on %d message(s), the projection has %d"), Record.MessagesBefore, Messages.Num()));
		return EApplyResult::AnchorMismatch;
	}
	if (ComputeAnchor(Messages) != Record.Anchor)
	{
		Say(TEXT("the projection's message structure differs from the one the decision was made on"));
		return EApplyResult::AnchorMismatch;
	}
	if (Record.Op == EProjectionOp::Summarize
		&& (Record.HeadMessages < 1 || Record.TailMessages < 0 || Record.HeadMessages + Record.TailMessages > Messages.Num()))
	{
		Say(TEXT("summary boundary outside the projection"));
		return EApplyResult::Invalid;
	}

	TArray<TSharedPtr<FJsonValue>> Working = Messages;
	TMap<FString, FString> Bodies;
	ClearToolResults(Working, TSet<FString>(Record.ClearedToolUseIds), &Bodies);

	if (Record.Op == EProjectionOp::Summarize)
	{
		int32 Retained = 0;
		TArray<TSharedPtr<FJsonValue>> Candidate = BuildSummarizedProjection(Working, Record.HeadMessages,
			Record.TailMessages, MakeUserTextMessage(Record.SummaryMessageText), &Retained);
		if (Retained != Record.RetainedUserMessages)
		{
			Say(FString::Printf(TEXT("the summarized span holds %d user message(s), the record says %d"),
				Retained, Record.RetainedUserMessages));
			return EApplyResult::AnchorMismatch;
		}
		Working = MoveTemp(Candidate);
	}

	Messages = MoveTemp(Working);
	if (OutClearedBodies)
	{
		for (const TPair<FString, FString>& Pair : Bodies)
		{
			if (!OutClearedBodies->Contains(Pair.Key))
			{
				OutClearedBodies->Add(Pair.Key, Pair.Value);
			}
		}
	}
	return EApplyResult::Applied;
}

TSharedPtr<FJsonObject> BuildSyntheticEntry(const FString& Kind, const FString& Text, const FString& RunId, const FString& TurnId)
{
	TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("type"), SyntheticEntryType);
	Entry->SetNumberField(TEXT("schema"), SyntheticRecordSchema);
	Entry->SetStringField(TEXT("uuid"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
	Entry->SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());
	Entry->SetStringField(TEXT("origin"), TEXT("orchestrator"));
	Entry->SetStringField(TEXT("kind"), Kind);
	Entry->SetStringField(TEXT("role"), TEXT("user"));
	Entry->SetStringField(TEXT("content"), Text);
	if (!RunId.IsEmpty())
	{
		Entry->SetStringField(TEXT("run_id"), RunId);
	}
	if (!TurnId.IsEmpty())
	{
		Entry->SetStringField(TEXT("turn_id"), TurnId);
	}
	return Entry;
}

}
