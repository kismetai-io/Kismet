// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIJournalReducer.h"
#include "KismetAIContextProjection.h"
#include "KismetAILog.h"
#include "KismetAISessionStorage.h"
#include "KismetAIToolCallJournal.h"
#include "KismetAIToolRegistry.h"

#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogKismetAIJournal, Log, All);

namespace
{
	constexpr int32 ExcerptChars = 160;

	FString Condensed(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Out;
	}

	FString FieldAsText(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		const TSharedPtr<FJsonValue> Value = Obj.IsValid() ? Obj->TryGetField(Field) : nullptr;
		FString Text;
		if (Value.IsValid() && Value->TryGetString(Text))
		{
			return Text;
		}
		return TEXT("(not a number)");
	}

	FJournalDiagnostic MakeDiagnostic(EJournalIssue Issue, const FJournalLine* Line, const FString& Detail)
	{
		FJournalDiagnostic D;
		D.Issue = Issue;
		D.Detail = Detail;
		if (Line)
		{
			D.Line = Line->Line;
			D.ByteOffset = Line->ByteOffset;
			D.ByteLength = Line->ByteLength;
		}
		return D;
	}

	TSharedPtr<FJsonObject> BuildUserMessage(const TSharedPtr<FJsonObject>& Entry, const FString& Content)
	{
		TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), TEXT("user"));

		// Images make the content a block array; otherwise it stays a bare
		// string, matching exactly what the live loop sent.
		const TArray<TSharedPtr<FJsonValue>>* Images = nullptr;
		if (Entry->TryGetArrayField(TEXT("images"), Images) && Images->Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Blocks;
			for (const TSharedPtr<FJsonValue>& Img : *Images)
			{
				const TSharedPtr<FJsonObject> ImgObj = Img.IsValid() ? Img->AsObject() : nullptr;
				if (!ImgObj.IsValid()) { continue; }
				TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
				Block->SetStringField(TEXT("type"), TEXT("image"));
				TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
				Source->SetStringField(TEXT("type"), TEXT("base64"));
				FString MediaType, Data;
				ImgObj->TryGetStringField(TEXT("media_type"), MediaType);
				ImgObj->TryGetStringField(TEXT("data"), Data);
				Source->SetStringField(TEXT("media_type"), MediaType);
				Source->SetStringField(TEXT("data"), Data);
				Block->SetObjectField(TEXT("source"), Source);
				Blocks.Add(MakeShared<FJsonValueObject>(Block));
			}
			TSharedPtr<FJsonObject> TextBlock = MakeShared<FJsonObject>();
			TextBlock->SetStringField(TEXT("type"), TEXT("text"));
			TextBlock->SetStringField(TEXT("text"), Content);
			Blocks.Add(MakeShared<FJsonValueObject>(TextBlock));
			Msg->SetArrayField(TEXT("content"), Blocks);
		}
		else
		{
			Msg->SetStringField(TEXT("content"), Content);
		}
		return Msg;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Diagnostics and integrity
// ─────────────────────────────────────────────────────────────────────────────

const TCHAR* FJournalDiagnostic::IssueToString(EJournalIssue Issue)
{
	switch (Issue)
	{
	case EJournalIssue::TruncatedFinalRecord:       return TEXT("truncated_final_record");
	case EJournalIssue::SealedTruncatedRecord:      return TEXT("sealed_truncated_record");
	case EJournalIssue::MalformedRecord:            return TEXT("malformed_record");
	case EJournalIssue::UntypedRecord:              return TEXT("untyped_record");
	case EJournalIssue::DuplicateRecord:            return TEXT("duplicate_record");
	case EJournalIssue::ConflictingDuplicateRecord: return TEXT("conflicting_duplicate_record");
	case EJournalIssue::UnsupportedRecordSchema:    return TEXT("unsupported_record_schema");
	case EJournalIssue::UnsupportedJournalVersion:  return TEXT("unsupported_journal_version");
	case EJournalIssue::UnreadableFile:             return TEXT("unreadable_file");
	case EJournalIssue::UserMessageLost:            return TEXT("user_message_lost");
	case EJournalIssue::ProjectionNotReproduced:    return TEXT("projection_not_reproduced");
	}
	return TEXT("unknown");
}

TSharedPtr<FJsonObject> FJournalDiagnostic::ToJson() const
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("issue"), IssueToString(Issue));
	if (Line > 0) { Obj->SetNumberField(TEXT("line"), Line); }
	if (ByteOffset >= 0) { Obj->SetNumberField(TEXT("byte_offset"), (double)ByteOffset); }
	if (ByteLength >= 0) { Obj->SetNumberField(TEXT("byte_length"), (double)ByteLength); }
	if (!Detail.IsEmpty()) { Obj->SetStringField(TEXT("detail"), Detail); }
	if (!Excerpt.IsEmpty()) { Obj->SetStringField(TEXT("excerpt"), Excerpt); }
	return Obj;
}

TArray<TSharedPtr<FJsonObject>> FParsedJournal::Entries() const
{
	TArray<TSharedPtr<FJsonObject>> Out;
	Out.Reserve(Lines.Num());
	for (const FJournalLine& L : Lines)
	{
		Out.Add(L.Entry);
	}
	return Out;
}

FString FContextReplayReport::ToString() const
{
	return FString::Printf(TEXT("context replay: %d applied, %d not reproduced, %d legacy, %d synthetic"),
		AppliedRecords, NotReproducedRecords, LegacyCompactionRecords, SyntheticMessages);
}

TSharedPtr<FJsonObject> FContextReplayReport::ToJson() const
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("applied_records"), AppliedRecords);
	Obj->SetNumberField(TEXT("not_reproduced_records"), NotReproducedRecords);
	Obj->SetNumberField(TEXT("legacy_compaction_records"), LegacyCompactionRecords);
	Obj->SetNumberField(TEXT("synthetic_messages"), SyntheticMessages);
	if (EffectiveLimit > 0)
	{
		Obj->SetNumberField(TEXT("effective_limit"), EffectiveLimit);
	}
	if (!Model.IsEmpty())
	{
		Obj->SetStringField(TEXT("model"), Model);
	}
	return Obj;
}

FString FSessionSnapshot::DamageNoticeKey(const FJournalDiagnostic& Diagnostic)
{
	switch (Diagnostic.Issue)
	{
	case EJournalIssue::TruncatedFinalRecord:
	case EJournalIssue::SealedTruncatedRecord:
	case EJournalIssue::MalformedRecord:
	case EJournalIssue::UntypedRecord:
		// Line and offset: stable in an append-only file, and the same for a
		// cut-short final record and the sealed record it becomes.
		return FString::Printf(TEXT("L%d@%lld"), Diagnostic.Line, Diagnostic.ByteOffset);
	default:
		return FString();
	}
}

bool FSessionSnapshot::CanResume() const
{
	return Integrity != EJournalIntegrity::UnsupportedVersion && Integrity != EJournalIntegrity::Unreadable;
}

const TCHAR* FSessionSnapshot::IntegrityToString(EJournalIntegrity Value)
{
	switch (Value)
	{
	case EJournalIntegrity::Intact:             return TEXT("intact");
	case EJournalIntegrity::Recovered:          return TEXT("recovered");
	case EJournalIntegrity::Damaged:            return TEXT("damaged");
	case EJournalIntegrity::UnsupportedVersion: return TEXT("unsupported_version");
	case EJournalIntegrity::Missing:            return TEXT("missing");
	case EJournalIntegrity::Unreadable:         return TEXT("unreadable");
	}
	return TEXT("unknown");
}

TSharedPtr<FJsonObject> FSessionSnapshot::IntegrityToJson() const
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("integrity"), IntegrityToString(Integrity));
	Obj->SetNumberField(TEXT("journal_version"), JournalVersion);
	Obj->SetNumberField(TEXT("supported_journal_version"), KismetAIJournalReducer::JournalFormatVersion);
	Obj->SetNumberField(TEXT("skipped_lines"), SkippedEntryCount);
	Obj->SetNumberField(TEXT("unrecorded_calls"), UnrecordedCalls);
	Obj->SetBoolField(TEXT("user_messages_preserved"), bUserMessagesPreserved);
	Obj->SetObjectField(TEXT("context_replay"), ContextReplay.ToJson());
	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FJournalDiagnostic& D : Diagnostics)
	{
		Items.Add(MakeShared<FJsonValueObject>(D.ToJson()));
	}
	Obj->SetArrayField(TEXT("diagnostics"), Items);
	return Obj;
}

namespace KismetAIJournalReducer
{

// ─────────────────────────────────────────────────────────────────────────────
// Decoding
// ─────────────────────────────────────────────────────────────────────────────

FParsedJournal ParseJournalBytes(const TArray<uint8>& Bytes)
{
	FParsedJournal Out;
	const int64 Size = Bytes.Num();
	int64 Start = 0;
	if (Size >= 3 && Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF)
	{
		Start = 3;  // UTF-8 BOM; the writer never emits one, an editor might
	}

	// A malformed line waits for the next line to learn whether AppendEntry
	// sealed it (a journal_seal record whose at_offset is the line's end).
	struct FPending
	{
		bool bActive = false;
		FJournalDiagnostic Diag;
		int64 EndOffset = 0;   // offset of the line's newline (= file size when it was sealed)
		bool bTerminated = false;
	} Pending;

	auto FlushPending = [&Out, &Pending](bool bSealed)
	{
		if (!Pending.bActive) { return; }
		Pending.Diag.Issue = bSealed ? EJournalIssue::SealedTruncatedRecord
			: !Pending.bTerminated ? EJournalIssue::TruncatedFinalRecord
			: EJournalIssue::MalformedRecord;
		Pending.Diag.Detail = bSealed
			? TEXT("Record cut short by an interrupted write; a later append sealed it")
			: !Pending.bTerminated ? TEXT("Final record cut short (no trailing newline)")
			: TEXT("Record is not valid JSON");
		Out.Diagnostics.Add(Pending.Diag);
		Pending.bActive = false;
	};

	int32 LineNo = 0;
	while (Start < Size)
	{
		int64 End = Start;
		while (End < Size && Bytes[End] != '\n') { ++End; }
		const bool bTerminated = End < Size;
		++LineNo;

		int64 ContentLen = End - Start;
		if (ContentLen > 0 && Bytes[Start + ContentLen - 1] == '\r') { --ContentLen; }

		bool bBlank = true;
		for (int64 i = Start; i < Start + ContentLen && bBlank; ++i)
		{
			const uint8 C = Bytes[i];
			bBlank = C == ' ' || C == '\t' || C == '\r';
		}

		if (!bBlank)
		{
			const FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Start), (int32)ContentLen);
			const FString Text(Conv.Length(), Conv.Get());

			TSharedPtr<FJsonObject> Entry;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			if (FJsonSerializer::Deserialize(Reader, Entry) && Entry.IsValid())
			{
				// Does this line seal the pending malformed one?
				FString Type;
				double AtOffset = -1.0;
				const bool bSeal = Entry->TryGetStringField(TEXT("type"), Type) && Type == SealEntryType
					&& Entry->TryGetNumberField(TEXT("at_offset"), AtOffset);
				FlushPending(Pending.bActive && bSeal && (int64)AtOffset == Pending.EndOffset);

				FJournalLine Line;
				Line.Entry = Entry;
				Line.Line = LineNo;
				Line.ByteOffset = Start;
				Line.ByteLength = ContentLen;
				Out.Lines.Add(MoveTemp(Line));
			}
			else
			{
				FlushPending(false);
				++Out.MalformedLineCount;
				Pending.bActive = true;
				Pending.Diag = FJournalDiagnostic();
				Pending.Diag.Line = LineNo;
				Pending.Diag.ByteOffset = Start;
				Pending.Diag.ByteLength = ContentLen;
				Pending.Diag.Excerpt = Text.Left(ExcerptChars);
				Pending.EndOffset = End;
				Pending.bTerminated = bTerminated;
			}
		}
		Start = End + 1;
	}
	FlushPending(false);
	return Out;
}

TArray<TSharedPtr<FJsonObject>> ParseEntries(const FString& Jsonl, int32& OutSkipped)
{
	const FTCHARToUTF8 Utf8(*Jsonl);
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	const FParsedJournal Parsed = ParseJournalBytes(Bytes);
	OutSkipped = Parsed.MalformedLineCount;
	return Parsed.Entries();
}

// ─────────────────────────────────────────────────────────────────────────────
// Reduction
// ─────────────────────────────────────────────────────────────────────────────

FSessionSnapshot Reduce(const FParsedJournal& Journal)
{
	FSessionSnapshot Snapshot;
	// Journal records and the legacy projection operations share this wire codec.
	// Only this boundary holds JSON messages; callers receive canonical values.
	TArray<TSharedPtr<FJsonValue>> JournalMessages;
	TArray<TSharedPtr<FJsonValue>> JournalContext;
	Snapshot.EntryCount = Journal.Lines.Num();
	Snapshot.SkippedEntryCount = Journal.MalformedLineCount;
	Snapshot.Diagnostics = Journal.Diagnostics;

	bool bUnsupportedVersion = false;
	bool bSawHeader = false;

	// Records that carry a uuid; a byte-identical repeat is a double-written
	// line, a different record under the same uuid cannot be resolved.
	TMap<FString, FString> SerializedByUuid;

	// The objects built from user_message entries: each must still be in the
	// final projection (an invariant, checked rather than assumed).
	TArray<TSharedPtr<FJsonValue>> UserMessages;

	// Same per-call folding as before.
	KismetAIToolCallJournal::FResumeProjector BatchProjector;

	// Intents and reconciliations by call_id, first record wins. A
	// reconciliation is written after the call it is about (on a later resume),
	// so it is indexed up front and the projector appends its note when it
	// projects that call's missing outcome.
	TMap<FString, TSharedPtr<FJsonObject>> IntentsByCall;
	TMap<FString, TSharedPtr<FJsonObject>> ReconciliationEntriesByCall;
	{
		TMap<FString, FString> Notes;
		for (const FJournalLine& Line : Journal.Lines)
		{
			FString Type, CallId;
			if (!Line.Entry.IsValid() || !Line.Entry->TryGetStringField(TEXT("type"), Type)
				|| !Line.Entry->TryGetStringField(TEXT("call_id"), CallId) || CallId.IsEmpty())
			{
				continue;
			}
			if (Type == KismetAIMutationIntent::IntentEntryType)
			{
				if (!IntentsByCall.Contains(CallId))
				{
					IntentsByCall.Add(CallId, Line.Entry);
				}
			}
			else if (Type == KismetAIMutationIntent::ReconciliationEntryType)
			{
				FKismetAIReconciliation Reconciliation;
				if (!ReconciliationEntriesByCall.Contains(CallId) && FKismetAIReconciliation::FromEntry(Line.Entry, Reconciliation))
				{
					ReconciliationEntriesByCall.Add(CallId, Line.Entry);
					Notes.Add(CallId, Reconciliation.BuildModelNote());
					Snapshot.Reconciliations.Add(MoveTemp(Reconciliation));
				}
			}
		}
		BatchProjector.SetReconciliationNotes(MoveTemp(Notes));
	}

	// The effective context, built alongside Messages. Canonical messages
	// are copied into it lazily (the projector appends a tool batch only when it
	// closes), and every projection record is applied to it where it was written.
	int32 ContextSynced = 0;
	auto SyncContext = [&JournalMessages, &JournalContext, &ContextSynced]()
	{
		for (; ContextSynced < JournalMessages.Num(); ++ContextSynced)
		{
			JournalContext.Add(JournalMessages[ContextSynced]);
		}
	};

	for (const FJournalLine& Line : Journal.Lines)
	{
		const TSharedPtr<FJsonObject>& Entry = Line.Entry;
		FString Type;
		if (!Entry.IsValid() || !Entry->TryGetStringField(TEXT("type"), Type) || Type.IsEmpty())
		{
			Snapshot.Diagnostics.Add(MakeDiagnostic(EJournalIssue::UntypedRecord, &Line, TEXT("Record has no type")));
			continue;
		}

		FString Uuid;
		if (Entry->TryGetStringField(TEXT("uuid"), Uuid) && !Uuid.IsEmpty())
		{
			const FString Serialized = Condensed(Entry);
			if (const FString* Previous = SerializedByUuid.Find(Uuid))
			{
				if (*Previous == Serialized)
				{
					Snapshot.Diagnostics.Add(MakeDiagnostic(EJournalIssue::DuplicateRecord, &Line,
						FString::Printf(TEXT("Repeat of record %s; the later copy is ignored"), *Uuid)));
					continue;
				}
				Snapshot.Diagnostics.Add(MakeDiagnostic(EJournalIssue::ConflictingDuplicateRecord, &Line,
					FString::Printf(TEXT("A different record reuses id %s; both are kept"), *Uuid)));
			}
			else
			{
				SerializedByUuid.Add(Uuid, Serialized);
			}
		}

		if (Type == TEXT("session_start"))
		{
			if (bSawHeader)
			{
				continue;  // only the first header describes the journal
			}
			bSawHeader = true;
			Entry->TryGetStringField(TEXT("model"), Snapshot.Model);
			if (Entry->HasField(TEXT("v")))
			{
				// A JSON number only: UE would also parse "1" from a string, but the
				// writer never emits one, so anything else is not this format.
				double Version = 0.0;
				const TSharedPtr<FJsonValue> VersionValue = Entry->TryGetField(TEXT("v"));
				const bool bNumber = VersionValue.IsValid() && VersionValue->Type == EJson::Number
					&& VersionValue->TryGetNumber(Version);
				if (!bNumber || Version != FMath::FloorToDouble(Version) || Version < 0.0 || Version > JournalFormatVersion)
				{
					bUnsupportedVersion = true;
					Snapshot.JournalVersion = bNumber ? (int32)Version : -1;
					Snapshot.Diagnostics.Add(MakeDiagnostic(EJournalIssue::UnsupportedJournalVersion, &Line,
						FString::Printf(TEXT("Journal format version %s; this build reads up to %d"),
							*FieldAsText(Entry, TEXT("v")), JournalFormatVersion)));
				}
				else
				{
					Snapshot.JournalVersion = (int32)Version;
				}
			}
			continue;
		}

		if (Type == KismetAIContextProjection::SyntheticEntryType)
		{
			BatchProjector.BeforeMessageEntry(JournalMessages);
			FString Content;
			if (!Entry->TryGetStringField(TEXT("content"), Content) || Content.IsEmpty())
			{
				continue;
			}
			// The same constructor the live loop used, so the replayed message is
			// the one the model saw.
			FSyntheticMessageInfo Info;
			Info.Message = KismetAIContextProjection::MakeUserTextMessage(Content);
			Entry->TryGetStringField(TEXT("kind"), Info.Kind);
			Entry->TryGetStringField(TEXT("uuid"), Info.Uuid);
			JournalMessages.Add(Info.Message);
			if (Info.Kind == KismetAIContextProjection::SyntheticKind::JournalDamageNotice)
			{
				const TArray<TSharedPtr<FJsonValue>>* Covers = nullptr;
				if (Entry->TryGetArrayField(TEXT("covers"), Covers))
				{
					for (const TSharedPtr<FJsonValue>& Key : *Covers)
					{
						FString K;
						if (Key.IsValid() && Key->TryGetString(K)) { Snapshot.NoticedDamageKeys.Add(K); }
					}
				}
			}
			Snapshot.SyntheticMessages.Add(MoveTemp(Info));
			continue;
		}

		if (Type == KismetAIContextProjection::CompactionEntryType)
		{
			double Schema = 1.0;
			Entry->TryGetNumberField(TEXT("schema"), Schema);
			FString Status;
			const TSharedPtr<FJsonObject>* ResultObj = nullptr;
			if (Entry->TryGetObjectField(TEXT("result"), ResultObj) && ResultObj && ResultObj->IsValid())
			{
				(*ResultObj)->TryGetStringField(TEXT("status"), Status);
			}
			const TSharedPtr<FJsonObject>* ProjectionObj = nullptr;
			const bool bHasProjection = Entry->TryGetObjectField(TEXT("projection"), ProjectionObj)
				&& ProjectionObj && ProjectionObj->IsValid();
			if (Status != TEXT("committed"))
			{
				continue;  // a failed attempt changed nothing the model saw
			}
			if (Schema < KismetAIContextProjection::CompactionRecordSchema || !bHasProjection)
			{
				// Committed, but the summary was never persisted.
				++Snapshot.ContextReplay.LegacyCompactionRecords;
				continue;
			}
			if (Schema > KismetAIContextProjection::CompactionRecordSchema)
			{
				++Snapshot.ContextReplay.NotReproducedRecords;
				Snapshot.Diagnostics.Add(MakeDiagnostic(EJournalIssue::ProjectionNotReproduced, &Line,
					FString::Printf(TEXT("context_compaction schema %s is newer than %d; the full history is kept from here"),
						*FieldAsText(Entry, TEXT("schema")), KismetAIContextProjection::CompactionRecordSchema)));
				continue;
			}

			KismetAIContextProjection::FProjectionRecord Record;
			FString Detail;
			if (!KismetAIContextProjection::FProjectionRecord::FromJson(*ProjectionObj, Record, Detail))
			{
				++Snapshot.ContextReplay.NotReproducedRecords;
				Snapshot.Diagnostics.Add(MakeDiagnostic(EJournalIssue::ProjectionNotReproduced, &Line,
					FString::Printf(TEXT("Unreadable projection record (%s); the full history is kept from here"), *Detail)));
				continue;
			}

			// Bring the context to exactly the projection the live loop held when
			// it wrote this line: everything journaled before it, pairing repaired
			// (the loop repairs before every call).
			BatchProjector.BeforeMessageEntry(JournalMessages);
			SyncContext();
			KismetAIHistoryRepair::RepairToolPairing(JournalContext);

			const KismetAIContextProjection::EApplyResult Applied = KismetAIContextProjection::Apply(
				Record, JournalContext, &Snapshot.ClearedToolResultBodies, &Detail);
			if (Applied != KismetAIContextProjection::EApplyResult::Applied)
			{
				++Snapshot.ContextReplay.NotReproducedRecords;
				Snapshot.Diagnostics.Add(MakeDiagnostic(EJournalIssue::ProjectionNotReproduced, &Line,
					FString::Printf(TEXT("%s record not reproduced (%s); the full history is kept from here"),
						KismetAIContextProjection::FProjectionRecord::OpToString(Record.Op), *Detail)));
				continue;
			}
			KismetAIHistoryRepair::RepairToolPairing(JournalContext);
			++Snapshot.ContextReplay.AppliedRecords;
			const TSharedPtr<FJsonObject>* EstimateObj = nullptr;
			double Limit = 0.0;
			if (Entry->TryGetObjectField(TEXT("estimate"), EstimateObj) && EstimateObj && EstimateObj->IsValid()
				&& (*EstimateObj)->TryGetNumberField(TEXT("effective_limit"), Limit))
			{
				Snapshot.ContextReplay.EffectiveLimit = (int32)Limit;
			}
			const TSharedPtr<FJsonObject>* ConfigObj = nullptr;
			if (Entry->TryGetObjectField(TEXT("config"), ConfigObj) && ConfigObj && ConfigObj->IsValid())
			{
				(*ConfigObj)->TryGetStringField(TEXT("model"), Snapshot.ContextReplay.Model);
			}
			continue;
		}

		if (Type == TEXT("user_message"))
		{
			BatchProjector.BeforeMessageEntry(JournalMessages);
			FString Content;
			Entry->TryGetStringField(TEXT("content"), Content);

			// The first user message is the session's title, minus the agent's
			// injected <references> block.
			if (Snapshot.Title.IsEmpty())
			{
				FString ForTitle = Content;
				FKismetAISessionStorage::StripLeadingReferencesBlock(ForTitle);
				Snapshot.Title = ForTitle.TrimStartAndEnd();
			}

			const TSharedPtr<FJsonValue> Msg = MakeShared<FJsonValueObject>(BuildUserMessage(Entry, Content));
			JournalMessages.Add(Msg);
			UserMessages.Add(Msg);
			bool bSynthetic = false;
			if (Entry->TryGetBoolField(TEXT("synthetic"), bSynthetic) && bSynthetic)
			{
				// Older truncation nudge: still projected as before, but its
				// provenance is reported.
				FSyntheticMessageInfo Info;
				Info.Message = Msg;
				Info.Kind = KismetAIContextProjection::SyntheticKind::LegacySynthetic;
				Entry->TryGetStringField(TEXT("uuid"), Info.Uuid);
				Info.bLegacy = true;
				Snapshot.SyntheticMessages.Add(MoveTemp(Info));
			}
			continue;
		}

		if (Type == TEXT("assistant_message"))
		{
			BatchProjector.BeforeMessageEntry(JournalMessages);
			const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
			if (!Entry->TryGetArrayField(TEXT("content"), Content) || Content->Num() == 0)
			{
				continue;  // an empty assistant message is not a valid request turn
			}
			// The provider block array, verbatim: thinking with its signature and
			// any block or field this build does not know.
			TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
			const TSharedPtr<FJsonObject>* Extensions = nullptr;
			if (Entry->TryGetObjectField(TEXT("message_extensions"), Extensions) && Extensions->IsValid())
			{
				FJsonObject::Duplicate(*Extensions, Msg);
			}
			// Journal identity is not provider metadata. Only the explicit extension
			// payload crosses this boundary, with role/content owned by the record.
			Msg->SetStringField(TEXT("role"), TEXT("assistant"));
			Msg->SetArrayField(TEXT("content"), *Content);
			JournalMessages.Add(MakeShared<FJsonValueObject>(Msg));
			BatchProjector.OnAssistantAppended(Entry, JournalMessages);
			continue;
		}

		if (Type == TEXT("tool_result_message"))
		{
			FString Record;
			if (Entry->TryGetStringField(TEXT("record"), Record)
				&& (Record == KismetAIToolCallJournal::RecordToolCall || Record == KismetAIToolCallJournal::RecordTurnNote)
				&& Entry->HasField(TEXT("schema")))
			{
				double Schema = 0.0;
				if (!Entry->TryGetNumberField(TEXT("schema"), Schema) || Schema > KismetAIToolCallJournal::SchemaVersion)
				{
					// Not interpreted: the call it would answer stays unrecorded.
					Snapshot.Diagnostics.Add(MakeDiagnostic(EJournalIssue::UnsupportedRecordSchema, &Line,
						FString::Printf(TEXT("%s record schema %s is newer than %d; its outcome is treated as unrecorded"),
							*Record, *FieldAsText(Entry, TEXT("schema")), KismetAIToolCallJournal::SchemaVersion)));
					continue;
				}
			}

			if (BatchProjector.TryConsumeToolResultEntry(Entry, JournalMessages))
			{
				continue;
			}
			// Legacy batch form. Note the key: the writer stores these under
			// "results", not "content".
			const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
			if (!Entry->TryGetArrayField(TEXT("results"), Results) || Results->Num() == 0)
			{
				continue;
			}
			TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
			Msg->SetStringField(TEXT("role"), TEXT("user"));
			Msg->SetArrayField(TEXT("content"), *Results);
			JournalMessages.Add(MakeShared<FJsonValueObject>(Msg));
			continue;
		}

		if ((Type == TEXT("error") || Type == TEXT("done")) && Entry->HasField(TEXT("outcome")))
		{
			Entry->TryGetStringField(TEXT("status"), Snapshot.Status);
			if (!Entry->TryGetStringField(TEXT("stop_reason"), Snapshot.StopReason)) { Entry->TryGetStringField(TEXT("error_code"), Snapshot.StopReason); }
			continue;
		}

		if (Type == TEXT("session_meta"))
		{
			// Re-appended on every terminal path, so the last one wins.
			Entry->TryGetStringField(TEXT("status"), Snapshot.Status);
			Entry->TryGetStringField(TEXT("stop_reason"), Snapshot.StopReason);
			continue;
		}

		// token_stats, error, history_repair, context_compaction, journal_seal,
		// journal_recovery and anything a newer build writes: not part of the
		// conversation.
	}

	BatchProjector.Finish(JournalMessages);
	Snapshot.CommittedCalls = BatchProjector.GetCommittedCalls();
	Snapshot.UnrecordedCalls = BatchProjector.GetUnrecordedCallCount();
	for (const KismetAIToolCallJournal::FResumeProjector::FUnrecordedCall& Call : BatchProjector.GetUnrecordedCalls())
	{
		const FString CallId = KismetAIToolCallJournal::MakeCallId(Call.TurnId, Call.CallIndex);
		const TSharedPtr<FJsonObject>* Intent = IntentsByCall.Find(CallId);
		if (!Intent && !FKismetAIToolRegistry::IsMutation(Call.ToolName))
		{
			continue;  // a read with no outcome changed nothing worth reconciling
		}
		FKismetAIUncertainCall& Uncertain = Snapshot.UncertainCalls.AddDefaulted_GetRef();
		Call.TurnId.Split(TEXT("/"), &Uncertain.RunId, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		Uncertain.TurnId = Call.TurnId;
		Uncertain.CallId = CallId;
		Uncertain.CallIndex = Call.CallIndex;
		Uncertain.ToolUseId = Call.ToolUseId;
		Uncertain.ToolName = Call.ToolName;
		Uncertain.Intent = Intent ? *Intent : nullptr;
		if (!Uncertain.Intent.IsValid() || !Uncertain.Intent->TryGetStringField(TEXT("args_canonical"), Uncertain.ArgsCanonical))
		{
			Uncertain.ArgsCanonical = KismetAIMutationIntent::CanonicalArgs(Call.Input);
		}
		if (const TSharedPtr<FJsonObject>* Rec = ReconciliationEntriesByCall.Find(CallId))
		{
			Uncertain.Reconciliation = *Rec;
		}
	}
	SyncContext();
	Snapshot.ContextReplay.SyntheticMessages = Snapshot.SyntheticMessages.Num();

	// The pairing repair the loop also applies before every call. Running it here
	// makes the recovery projection provider-valid before anything is sent; it
	// answers missing results with OUTCOME_UNRECORDED and never removes a
	// genuine user message.
	Snapshot.Repair = KismetAIHistoryRepair::RepairToolPairing(JournalMessages);

	// Every user_message entry is still a genuine user message, as the same object.
	Snapshot.UserMessageEntries = UserMessages.Num();
	for (const TSharedPtr<FJsonValue>& Msg : UserMessages)
	{
		if (!JournalMessages.Contains(Msg) || !KismetAIHistoryRepair::IsGenuineUserMessage(Msg))
		{
			Snapshot.bUserMessagesPreserved = false;
		}
	}
	// The context: repaired like Messages. Compaction keeps every genuine user
	// message verbatim, so each user_message (and synthetic message) object
	// must still be in it; if not, something is wrong with a record, and the
	// context falls back to the full history rather than lose the user's words.
	if (Snapshot.ContextReplay.AppliedRecords == 0)
	{
		JournalContext = JournalMessages;
	}
	else
	{
		const KismetAIHistoryRepair::FRepairReport ContextRepair = KismetAIHistoryRepair::RepairToolPairing(JournalContext);
		bool bContextKeepsUser = ContextRepair.bGenuineUserMessagesPreserved;
		for (const TSharedPtr<FJsonValue>& Msg : UserMessages)
		{
			bContextKeepsUser &= JournalContext.Contains(Msg);
		}
		if (!bContextKeepsUser)
		{
			Snapshot.Diagnostics.Add(MakeDiagnostic(EJournalIssue::ProjectionNotReproduced, nullptr,
				TEXT("The replayed context lost a user message; the full history is used instead")));
			KISMETAI_LOG(LogKismetAIJournal, Error, TEXT("Context replay lost a user message; falling back to the full history"));
			Snapshot.ContextReplay.NotReproducedRecords += Snapshot.ContextReplay.AppliedRecords;
			Snapshot.ContextReplay.AppliedRecords = 0;
			Snapshot.ContextReplay.EffectiveLimit = 0;
			Snapshot.ContextReplay.Model.Empty();
			JournalContext = JournalMessages;
		}
	}

	if (!Snapshot.bUserMessagesPreserved || !Snapshot.Repair.bGenuineUserMessagesPreserved)
	{
		Snapshot.bUserMessagesPreserved = false;
		Snapshot.Diagnostics.Add(MakeDiagnostic(EJournalIssue::UserMessageLost, nullptr,
			TEXT("A user message from the journal is missing from the projection")));
		KISMETAI_LOG(LogKismetAIJournal, Error, TEXT("Journal reduction lost a user message; this is a bug"));
	}

	if (bUnsupportedVersion)
	{
		Snapshot.Integrity = EJournalIntegrity::UnsupportedVersion;
	}
	else
	{
		Snapshot.Integrity = EJournalIntegrity::Intact;
		for (const FJournalDiagnostic& D : Snapshot.Diagnostics)
		{
			switch (D.Issue)
			{
			case EJournalIssue::MalformedRecord:
			case EJournalIssue::UntypedRecord:
			case EJournalIssue::ConflictingDuplicateRecord:
			case EJournalIssue::UserMessageLost:
				Snapshot.Integrity = EJournalIntegrity::Damaged;
				break;
			default:
				if (Snapshot.Integrity == EJournalIntegrity::Intact)
				{
					Snapshot.Integrity = EJournalIntegrity::Recovered;
				}
				break;
			}
		}
	}
	Snapshot.Messages = KismetAIAgentCodec::FromAnthropicArray(JournalMessages);
	Snapshot.Context = KismetAIAgentCodec::FromAnthropicArray(JournalContext);
	return Snapshot;
}

FSessionSnapshot Reduce(const TArray<TSharedPtr<FJsonObject>>& Entries)
{
	FParsedJournal Journal;
	Journal.Lines.Reserve(Entries.Num());
	for (const TSharedPtr<FJsonObject>& Entry : Entries)
	{
		if (!Entry.IsValid())
		{
			continue;
		}
		FJournalLine Line;
		Line.Entry = Entry;
		Journal.Lines.Add(MoveTemp(Line));
	}
	return Reduce(Journal);
}

FSessionSnapshot ReduceJournalBytes(const TArray<uint8>& Bytes)
{
	return Reduce(ParseJournalBytes(Bytes));
}

FSessionSnapshot ReduceJsonl(const FString& Jsonl)
{
	const FTCHARToUTF8 Utf8(*Jsonl);
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	return ReduceJournalBytes(Bytes);
}

// ─────────────────────────────────────────────────────────────────────────────
// Request projection
// ─────────────────────────────────────────────────────────────────────────────

TArray<FAgentMessage> ProjectResumeRequest(const TArray<FAgentMessage>& Messages)
{
	TArray<FAgentMessage> Out;
	Out.Reserve(Messages.Num());
	for (const FAgentMessage& Message : Messages)
	{
		FAgentMessage Copy = Message;
		if (Copy.Role == EAgentRole::Assistant)
		{
			const int32 Removed = Copy.Parts.RemoveAll([](const FAgentContentPart& Part)
			{
				return Part.Type == EAgentPartType::Thinking || Part.Type == EAgentPartType::RedactedThinking;
			});
			if (Removed > 0 && Copy.Parts.IsEmpty())
			{
				continue;
			}
		}
		Out.Add(MoveTemp(Copy));
	}
	return Out;
}

TArray<TSharedPtr<FJsonValue>> ProjectResumeRequest(const TArray<TSharedPtr<FJsonValue>>& Messages)
{
	return KismetAIAgentCodec::ToAnthropicArray(ProjectResumeRequest(KismetAIAgentCodec::FromAnthropicArray(Messages)));
}

}
