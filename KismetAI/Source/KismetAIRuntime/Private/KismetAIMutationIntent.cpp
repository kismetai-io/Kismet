// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIMutationIntent.h"
#include "KismetAIToolCallJournal.h"
#include "KismetAIAuthority.h"
#include "Serialization/JsonReader.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	FString ReplayResource(const FString& Raw)
	{
		FString Package;
		if (!KismetAIAuthority::NormalizeAssetReference(Raw, Package)) return Raw;
		FString Reference = Raw.TrimStartAndEnd();
		int32 Quote = INDEX_NONE;
		if (Reference.FindChar(TCHAR(39), Quote) && Reference.EndsWith(TEXT("'")))
			Reference = Reference.Mid(Quote + 1, Reference.Len() - Quote - 2);
		if (Reference.StartsWith(TEXT("\"")) && Reference.EndsWith(TEXT("\"")))
			Reference = Reference.Mid(1, Reference.Len() - 2);
		Reference.ReplaceInline(TEXT("\\"), TEXT("/"));
		int32 Slash = INDEX_NONE;
		Reference.FindLastChar(TCHAR('/'), Slash);
		const int32 Dot = Reference.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromStart, Slash + 1);
		const int32 Colon = Reference.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Slash + 1);
		int32 PackageSlash = INDEX_NONE;
		Package.FindLastChar(TCHAR('/'), PackageSlash);
		const FString Leaf = Package.Mid(PackageSlash + 1);
		// Only the standard Package.Asset spelling aliases the package path.
		// Other object names and subobjects remain separate operation operands.
		if (Dot != INDEX_NONE)
		{
			const FString Object = Reference.Mid(Dot + 1);
			if (!Object.Equals(Leaf, ESearchCase::IgnoreCase)) return Package.ToLower() + TEXT(".") + Object.ToLower();
		}
		else if (Colon != INDEX_NONE) return Package.ToLower() + Reference.Mid(Colon).ToLower();
		return Package.ToLower();
	}

	FString NewUuid()
	{
		return FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	}

	void SetCallFields(const TSharedPtr<FJsonObject>& Entry, const FString& RunId, const FString& TurnId,
		int32 CallIndex, const FString& ToolUseId, const FString& ToolName, const FString& ArgsCanonical)
	{
		Entry->SetStringField(TEXT("run_id"), RunId);
		Entry->SetStringField(TEXT("turn_id"), TurnId);
		Entry->SetStringField(TEXT("call_id"), KismetAIToolCallJournal::MakeCallId(TurnId, CallIndex));
		Entry->SetNumberField(TEXT("call_index"), CallIndex);
		Entry->SetStringField(TEXT("tool_use_id"), ToolUseId);
		Entry->SetStringField(TEXT("tool_name"), ToolName);
		Entry->SetStringField(TEXT("args_canonical"), ArgsCanonical);
	}

	TArray<TSharedPtr<FJsonValue>> ToStringArray(const TArray<FString>& In)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& S : In)
		{
			Out.Add(MakeShared<FJsonValueString>(S));
		}
		return Out;
	}

	TArray<FString> FromStringArray(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Obj.IsValid() && Obj->TryGetArrayField(Field, Arr))
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				FString S;
				if (V.IsValid() && V->TryGetString(S))
				{
					Out.Add(S);
				}
			}
		}
		return Out;
	}

	// Worst verdict first: anything that might have applied outranks "no
	// evidence", which outranks "not applied".
	int32 Severity(EKismetAIReconcileVerdict V)
	{
		switch (V)
		{
		case EKismetAIReconcileVerdict::ChangedSinceIntent: return 3;
		case EKismetAIReconcileVerdict::AssetMissing:       return 3;
		case EKismetAIReconcileVerdict::NoEvidence:         return 2;
		case EKismetAIReconcileVerdict::NotApplied:         return 1;
		}
		return 2;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Fingerprint
// ─────────────────────────────────────────────────────────────────────────────

bool FKismetAIAssetFingerprint::SameState(const FKismetAIAssetFingerprint& Other) const
{
	if (!bCaptured || !Other.bCaptured)
	{
		return false;
	}
	if (bExists != Other.bExists)
	{
		return false;
	}
	if (!bExists)
	{
		return true;
	}
	return HasStateEvidence() && Other.HasStateEvidence() && Digest == Other.Digest;
}

bool FKismetAIAssetFingerprint::SameFile(const FKismetAIAssetFingerprint& Other) const
{
	if (bFileExists != Other.bFileExists)
	{
		return false;
	}
	return !bFileExists || (FileSha1 == Other.FileSha1 && FileSize == Other.FileSize);
}

TArray<FString> FKismetAIAssetFingerprint::DescribeDifferences(const FKismetAIAssetFingerprint& Before,
	const FKismetAIAssetFingerprint& After, int32 MaxLines)
{
	TArray<FString> Lines;
	if (Before.bExists != After.bExists)
	{
		Lines.Add(Before.bExists ? TEXT("the asset no longer exists") : TEXT("the asset now exists"));
		return Lines;
	}
	TArray<FString> Keys;
	Before.ObjectDigests.GetKeys(Keys);
	for (const TPair<FString, FString>& Pair : After.ObjectDigests)
	{
		if (!Before.ObjectDigests.Contains(Pair.Key))
		{
			Keys.Add(Pair.Key);
		}
	}
	Keys.Sort();
	int32 Total = 0;
	for (const FString& Key : Keys)
	{
		const FString* B = Before.ObjectDigests.Find(Key);
		const FString* A = After.ObjectDigests.Find(Key);
		FString Line;
		if (B && !A)
		{
			Line = FString::Printf(TEXT("removed: %s"), *Key);
		}
		else if (!B && A)
		{
			Line = FString::Printf(TEXT("added: %s"), *Key);
		}
		else if (B && A && *B != *A)
		{
			Line = FString::Printf(TEXT("changed: %s"), *Key);
		}
		if (Line.IsEmpty())
		{
			continue;
		}
		++Total;
		if (Lines.Num() < MaxLines)
		{
			Lines.Add(MoveTemp(Line));
		}
	}
	if (Total > Lines.Num())
	{
		Lines.Add(FString::Printf(TEXT("... and %d more"), Total - Lines.Num()));
	}
	if (Total == 0 && Before.Digest != After.Digest)
	{
		Lines.Add(TEXT("the asset's state differs (no per-object detail)"));
	}
	return Lines;
}

TSharedPtr<FJsonObject> FKismetAIAssetFingerprint::ToJson() const
{
	TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetStringField(TEXT("asset"), AssetPath);
	J->SetStringField(TEXT("class"), AssetClass);
	J->SetBoolField(TEXT("captured"), bCaptured);
	J->SetBoolField(TEXT("exists"), bExists);
	J->SetBoolField(TEXT("supported"), bSupported);
	if (!UnsupportedReason.IsEmpty())
	{
		J->SetStringField(TEXT("unsupported_reason"), UnsupportedReason);
	}
	J->SetStringField(TEXT("digest"), Digest);
	TSharedPtr<FJsonObject> Objects = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Pair : ObjectDigests)
	{
		Objects->SetStringField(Pair.Key, Pair.Value);
	}
	J->SetObjectField(TEXT("objects"), Objects);
	J->SetBoolField(TEXT("package_dirty"), bPackageDirty);
	TSharedPtr<FJsonObject> File = MakeShared<FJsonObject>();
	File->SetBoolField(TEXT("exists"), bFileExists);
	if (bFileExists)
	{
		File->SetNumberField(TEXT("size"), (double)FileSize);
		File->SetStringField(TEXT("sha1"), FileSha1);
	}
	J->SetObjectField(TEXT("file"), File);
	return J;
}

bool FKismetAIAssetFingerprint::FromJson(const TSharedPtr<FJsonObject>& Json, FKismetAIAssetFingerprint& Out)
{
	Out = FKismetAIAssetFingerprint();
	if (!Json.IsValid() || !Json->TryGetStringField(TEXT("asset"), Out.AssetPath))
	{
		return false;
	}
	Json->TryGetStringField(TEXT("class"), Out.AssetClass);
	Json->TryGetBoolField(TEXT("captured"), Out.bCaptured);
	Json->TryGetBoolField(TEXT("exists"), Out.bExists);
	Json->TryGetBoolField(TEXT("supported"), Out.bSupported);
	Json->TryGetStringField(TEXT("unsupported_reason"), Out.UnsupportedReason);
	Json->TryGetStringField(TEXT("digest"), Out.Digest);
	const TSharedPtr<FJsonObject>* Objects = nullptr;
	if (Json->TryGetObjectField(TEXT("objects"), Objects) && Objects)
	{
		for (const auto& Pair : (*Objects)->Values)
		{
			FString V;
			if (Pair.Value.IsValid() && Pair.Value->TryGetString(V))
			{
				Out.ObjectDigests.Add(FString(Pair.Key), V);
			}
		}
	}
	Json->TryGetBoolField(TEXT("package_dirty"), Out.bPackageDirty);
	const TSharedPtr<FJsonObject>* File = nullptr;
	if (Json->TryGetObjectField(TEXT("file"), File) && File)
	{
		(*File)->TryGetBoolField(TEXT("exists"), Out.bFileExists);
		double Size = -1.0;
		if ((*File)->TryGetNumberField(TEXT("size"), Size))
		{
			Out.FileSize = (int64)Size;
		}
		(*File)->TryGetStringField(TEXT("sha1"), Out.FileSha1);
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Reconciliation
// ─────────────────────────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FKismetAIAssetReconciliation::ToJson() const
{
	TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetStringField(TEXT("asset"), AssetPath);
	J->SetStringField(TEXT("verdict"), KismetAIMutationIntent::VerdictToString(Verdict));
	if (MemoryMatchesBefore.IsSet())
	{
		J->SetBoolField(TEXT("memory_matches_before"), MemoryMatchesBefore.GetValue());
	}
	if (DiskMatchesBefore.IsSet())
	{
		J->SetBoolField(TEXT("disk_matches_before"), DiskMatchesBefore.GetValue());
	}
	J->SetArrayField(TEXT("differences"), ToStringArray(Differences));
	J->SetStringField(TEXT("detail"), Detail);
	return J;
}

bool FKismetAIAssetReconciliation::FromJson(const TSharedPtr<FJsonObject>& Json, FKismetAIAssetReconciliation& Out)
{
	Out = FKismetAIAssetReconciliation();
	FString Verdict;
	if (!Json.IsValid() || !Json->TryGetStringField(TEXT("asset"), Out.AssetPath)
		|| !Json->TryGetStringField(TEXT("verdict"), Verdict)
		|| !KismetAIMutationIntent::VerdictFromString(Verdict, Out.Verdict))
	{
		return false;
	}
	bool b = false;
	if (Json->TryGetBoolField(TEXT("memory_matches_before"), b)) { Out.MemoryMatchesBefore = b; }
	if (Json->TryGetBoolField(TEXT("disk_matches_before"), b)) { Out.DiskMatchesBefore = b; }
	Out.Differences = FromStringArray(Json, TEXT("differences"));
	Json->TryGetStringField(TEXT("detail"), Out.Detail);
	return true;
}

FString FKismetAIReconciliation::BuildModelNote() const
{
	TArray<FString> Lines;
	switch (Verdict)
	{
	case EKismetAIReconcileVerdict::NotApplied:
		Lines.Add(FString::Printf(TEXT("Reconciliation (%s): every asset this call names equals the state recorded just before it ran, so it has no visible effect (an unsaved effect is lost when the editor stops). It was not repeated automatically; redo it only if it is still needed."),
			KismetAIMutationIntent::VerdictToString(Verdict)));
		break;
	case EKismetAIReconcileVerdict::ChangedSinceIntent:
	case EKismetAIReconcileVerdict::AssetMissing:
		Lines.Add(FString::Printf(TEXT("Reconciliation (%s): the asset differs from the state recorded just before this call ran. The call may have been applied, fully or partly, or someone else changed the asset; which one cannot be proven. Repeating this exact call is refused (%s). Re-read the asset and decide from what is there."),
			KismetAIMutationIntent::VerdictToString(Verdict), KismetAIMutationIntent::ReconciliationRequiredCode));
		break;
	case EKismetAIReconcileVerdict::NoEvidence:
		Lines.Add(FString::Printf(TEXT("Reconciliation (%s): there is no recorded before-state to compare with, so whether this call was applied is unknown. Repeating this exact call is refused (%s). Re-read the asset and decide from what is there."),
			KismetAIMutationIntent::VerdictToString(Verdict), KismetAIMutationIntent::ReconciliationRequiredCode));
		break;
	}
	for (const FKismetAIAssetReconciliation& A : Assets)
	{
		FString Line = FString::Printf(TEXT("- %s: %s"), *A.AssetPath, KismetAIMutationIntent::VerdictToString(A.Verdict));
		if (!A.Detail.IsEmpty())
		{
			Line += TEXT(" (") + A.Detail + TEXT(")");
		}
		Lines.Add(Line);
		for (const FString& D : A.Differences)
		{
			Lines.Add(TEXT("    ") + D);
		}
	}
	return FString::Join(Lines, TEXT("\n"));
}

TSharedPtr<FJsonObject> FKismetAIReconciliation::ToEntry() const
{
	TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
	E->SetStringField(TEXT("type"), KismetAIMutationIntent::ReconciliationEntryType);
	E->SetNumberField(TEXT("schema"), KismetAIMutationIntent::SchemaVersion);
	E->SetStringField(TEXT("uuid"), NewUuid());
	E->SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());
	SetCallFields(E, RunId, TurnId, CallIndex, ToolUseId, ToolName, ArgsCanonical);
	E->SetStringField(TEXT("verdict"), KismetAIMutationIntent::VerdictToString(Verdict));
	TArray<TSharedPtr<FJsonValue>> AssetArray;
	for (const FKismetAIAssetReconciliation& A : Assets)
	{
		AssetArray.Add(MakeShared<FJsonValueObject>(A.ToJson()));
	}
	E->SetArrayField(TEXT("assets"), AssetArray);
	E->SetStringField(TEXT("explanation"), Explanation);
	return E;
}

bool FKismetAIReconciliation::FromEntry(const TSharedPtr<FJsonObject>& Entry, FKismetAIReconciliation& Out)
{
	Out = FKismetAIReconciliation();
	FString Type, Verdict;
	int32 Schema = 0;
	if (!Entry.IsValid() || !Entry->TryGetStringField(TEXT("type"), Type)
		|| Type != KismetAIMutationIntent::ReconciliationEntryType
		|| !Entry->TryGetNumberField(TEXT("schema"), Schema) || Schema > KismetAIMutationIntent::SchemaVersion
		|| !Entry->TryGetStringField(TEXT("verdict"), Verdict)
		|| !KismetAIMutationIntent::VerdictFromString(Verdict, Out.Verdict))
	{
		return false;
	}
	Entry->TryGetStringField(TEXT("run_id"), Out.RunId);
	Entry->TryGetStringField(TEXT("turn_id"), Out.TurnId);
	Entry->TryGetStringField(TEXT("call_id"), Out.CallId);
	Entry->TryGetNumberField(TEXT("call_index"), Out.CallIndex);
	Entry->TryGetStringField(TEXT("tool_use_id"), Out.ToolUseId);
	Entry->TryGetStringField(TEXT("tool_name"), Out.ToolName);
	Entry->TryGetStringField(TEXT("args_canonical"), Out.ArgsCanonical);
	Entry->TryGetStringField(TEXT("explanation"), Out.Explanation);
	const TArray<TSharedPtr<FJsonValue>>* AssetArray = nullptr;
	if (Entry->TryGetArrayField(TEXT("assets"), AssetArray))
	{
		for (const TSharedPtr<FJsonValue>& V : *AssetArray)
		{
			FKismetAIAssetReconciliation A;
			if (V.IsValid() && FKismetAIAssetReconciliation::FromJson(V->AsObject(), A))
			{
				Out.Assets.Add(MoveTemp(A));
			}
		}
	}
	return true;
}

namespace KismetAIMutationIntent
{
	const TCHAR* VerdictToString(EKismetAIReconcileVerdict Verdict)
	{
		switch (Verdict)
		{
		case EKismetAIReconcileVerdict::NotApplied:         return TEXT("not_applied");
		case EKismetAIReconcileVerdict::ChangedSinceIntent: return TEXT("changed_since_intent");
		case EKismetAIReconcileVerdict::AssetMissing:       return TEXT("asset_missing");
		case EKismetAIReconcileVerdict::NoEvidence:         return TEXT("no_evidence");
		}
		return TEXT("no_evidence");
	}

	bool VerdictFromString(const FString& In, EKismetAIReconcileVerdict& Out)
	{
		for (EKismetAIReconcileVerdict V : { EKismetAIReconcileVerdict::NotApplied, EKismetAIReconcileVerdict::ChangedSinceIntent,
			EKismetAIReconcileVerdict::AssetMissing, EKismetAIReconcileVerdict::NoEvidence })
		{
			if (In == VerdictToString(V))
			{
				Out = V;
				return true;
			}
		}
		return false;
	}

	FString ReplayKey(const FString& ToolName, const FString& ArgsCanonical)
	{
		TSharedPtr<FJsonObject> Args;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ArgsCanonical), Args) || !Args)
			return ToolName + TEXT("\n") + ArgsCanonical;
		Args->RemoveField(TEXT("expected_revision"));
		for (const TCHAR* Field : {TEXT("blueprint"), TEXT("widget"), TEXT("material"), TEXT("instance"), TEXT("asset"), TEXT("asset_path")})
		{
			FString Resource;
			if (Args->TryGetStringField(Field, Resource)) Args->SetStringField(Field, ReplayResource(Resource));
		}
		const FKismetAIOperation Op = KismetAIAuthority::DescribeOperation(ToolName, Args);
		for (const auto& Target : Op.Targets)
		{
			if (!Target.IsResolved()) continue;
			TArray<FString> Fields;
			Target.Field.ParseIntoArray(Fields, TEXT("+"));
			for (const FString& Field : Fields) Args->RemoveField(Field);
			Args->SetStringField(Target.Field, ReplayResource(Target.Raw));
		}
		TFunction<TSharedPtr<FJsonValue>(const TSharedPtr<FJsonValue>&)> Sort;
		Sort = [&Sort](const TSharedPtr<FJsonValue>& Value) -> TSharedPtr<FJsonValue>
		{
			if (!Value) return MakeShared<FJsonValueNull>();
			if (Value->Type == EJson::Object)
			{
				auto Obj = MakeShared<FJsonObject>();
				TArray<FString> Keys;
				for (const auto& Pair : Value->AsObject()->Values) Keys.Add(FString(Pair.Key));
				Keys.Sort();
				for (const FString& Key : Keys) Obj->SetField(Key, Sort(Value->AsObject()->TryGetField(Key)));
				return MakeShared<FJsonValueObject>(Obj);
			}
			if (Value->Type == EJson::Array)
			{
				TArray<TSharedPtr<FJsonValue>> Values;
				for (const auto& Item : Value->AsArray()) Values.Add(Sort(Item));
				return MakeShared<FJsonValueArray>(Values);
			}
			return Value;
		};
		return ToolName + TEXT("\n") + CanonicalArgs(Sort(MakeShared<FJsonValueObject>(Args))->AsObject());
	}

	FString CanonicalArgs(const TSharedPtr<FJsonObject>& Arguments)
	{
		if (!Arguments.IsValid())
		{
			return TEXT("{}");
		}
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Arguments.ToSharedRef(), Writer);
		return Out;
	}

	TSharedPtr<FJsonObject> BuildIntentEntry(const FString& RunId, const FString& TurnId,
		int32 CallIndex, const FString& ToolUseId, const FString& ToolName, const FString& ArgsCanonical,
		const TArray<FKismetAIAssetFingerprint>& Targets, const TArray<FString>& UnresolvedTargets)
	{
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("type"), IntentEntryType);
		E->SetNumberField(TEXT("schema"), SchemaVersion);
		E->SetStringField(TEXT("uuid"), NewUuid());
		E->SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());
		SetCallFields(E, RunId, TurnId, CallIndex, ToolUseId, ToolName, ArgsCanonical);
		TArray<TSharedPtr<FJsonValue>> TargetArray;
		for (const FKismetAIAssetFingerprint& T : Targets)
		{
			TargetArray.Add(MakeShared<FJsonValueObject>(T.ToJson()));
		}
		E->SetArrayField(TEXT("targets"), TargetArray);
		E->SetArrayField(TEXT("unresolved_targets"), ToStringArray(UnresolvedTargets));
		return E;
	}

	bool ReadIntentTargets(const TSharedPtr<FJsonObject>& Intent,
		TArray<FKismetAIAssetFingerprint>& OutTargets, TArray<FString>& OutUnresolved)
	{
		OutTargets.Reset();
		OutUnresolved.Reset();
		int32 Schema = 0;
		const TArray<TSharedPtr<FJsonValue>>* TargetArray = nullptr;
		if (!Intent.IsValid() || !Intent->TryGetNumberField(TEXT("schema"), Schema) || Schema > SchemaVersion
			|| !Intent->TryGetArrayField(TEXT("targets"), TargetArray))
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& V : *TargetArray)
		{
			FKismetAIAssetFingerprint F;
			if (!V.IsValid() || !FKismetAIAssetFingerprint::FromJson(V->AsObject(), F))
			{
				return false;
			}
			OutTargets.Add(MoveTemp(F));
		}
		OutUnresolved = FromStringArray(Intent, TEXT("unresolved_targets"));
		return true;
	}

	FKismetAIAssetReconciliation ReconcileAsset(const FKismetAIAssetFingerprint& Before, const FKismetAIAssetFingerprint& Now)
	{
		FKismetAIAssetReconciliation R;
		R.AssetPath = Before.AssetPath;

		if (!Before.bCaptured || !Now.bCaptured)
		{
			R.Verdict = EKismetAIReconcileVerdict::NoEvidence;
			const FString& Reason = !Before.bCaptured ? Before.UnsupportedReason : Now.UnsupportedReason;
			R.Detail = Reason.IsEmpty() ? FString(TEXT("the asset's state was not captured")) : Reason;
			return R;
		}

		if (Before.bFileExists || Now.bFileExists)
		{
			if (!Before.FileSha1.IsEmpty() || !Before.bFileExists)
			{
				R.DiskMatchesBefore = Before.SameFile(Now);
			}
		}

		// Existence is evidence even for an unsupported class.
		if (Before.bExists && !Now.bExists)
		{
			R.Verdict = EKismetAIReconcileVerdict::AssetMissing;
			R.MemoryMatchesBefore = false;
			R.Detail = TEXT("the asset existed before the call and does not exist now");
			return R;
		}
		if (!Before.bExists && Now.bExists)
		{
			R.Verdict = EKismetAIReconcileVerdict::ChangedSinceIntent;
			R.MemoryMatchesBefore = false;
			R.Detail = TEXT("the asset did not exist before the call and exists now");
			return R;
		}
		if (!Before.bExists && !Now.bExists)
		{
			R.Verdict = EKismetAIReconcileVerdict::NotApplied;
			R.MemoryMatchesBefore = true;
			R.Detail = TEXT("the asset did not exist before the call and still does not");
			return R;
		}
		if (!Before.HasStateEvidence() || !Now.HasStateEvidence())
		{
			R.Verdict = EKismetAIReconcileVerdict::NoEvidence;
			const FString& Reason = !Before.HasStateEvidence() ? Before.UnsupportedReason : Now.UnsupportedReason;
			R.Detail = Reason.IsEmpty() ? FString(TEXT("the asset's state could not be described")) : Reason;
			return R;
		}

		R.MemoryMatchesBefore = Before.Digest == Now.Digest;
		if (R.MemoryMatchesBefore.GetValue() && (!R.DiskMatchesBefore.IsSet() || R.DiskMatchesBefore.GetValue()))
		{
			R.Verdict = EKismetAIReconcileVerdict::NotApplied;
			R.Detail = TEXT("loaded state and package file equal the before-state");
			return R;
		}
		R.Verdict = EKismetAIReconcileVerdict::ChangedSinceIntent;
		if (!R.MemoryMatchesBefore.GetValue())
		{
			R.Differences = FKismetAIAssetFingerprint::DescribeDifferences(Before, Now);
			R.Detail = TEXT("the loaded asset differs from the before-state");
		}
		else
		{
			R.Detail = TEXT("the loaded asset equals the before-state but the package file on disk changed");
		}
		return R;
	}

	FKismetAIReconciliation Reconcile(const FKismetAIUncertainCall& Call, const TArray<FKismetAIAssetFingerprint>& Now)
	{
		FKismetAIReconciliation R;
		R.RunId = Call.RunId;
		R.TurnId = Call.TurnId;
		R.CallId = Call.CallId;
		R.CallIndex = Call.CallIndex;
		R.ToolUseId = Call.ToolUseId;
		R.ToolName = Call.ToolName;
		R.ArgsCanonical = Call.ArgsCanonical;

		TArray<FKismetAIAssetFingerprint> Before;
		TArray<FString> Unresolved;
		if (!Call.Intent.IsValid())
		{
			R.Verdict = EKismetAIReconcileVerdict::NoEvidence;
			R.Explanation = TEXT("No mutation_intent was recorded for this call (it was issued before the intent record was written, or by an older build).");
			return R;
		}
		if (!ReadIntentTargets(Call.Intent, Before, Unresolved))
		{
			R.Verdict = EKismetAIReconcileVerdict::NoEvidence;
			R.Explanation = TEXT("The call's mutation_intent record could not be read.");
			return R;
		}

		int32 Worst = 0;
		for (const FKismetAIAssetFingerprint& B : Before)
		{
			const FKismetAIAssetFingerprint* Match = Now.FindByPredicate([&B](const FKismetAIAssetFingerprint& N)
			{
				return N.AssetPath.Equals(B.AssetPath, ESearchCase::IgnoreCase);
			});
			FKismetAIAssetReconciliation A;
			if (Match)
			{
				A = ReconcileAsset(B, *Match);
			}
			else
			{
				A.AssetPath = B.AssetPath;
				A.Verdict = EKismetAIReconcileVerdict::NoEvidence;
				A.Detail = TEXT("the asset's current state could not be captured");
			}
			Worst = FMath::Max(Worst, Severity(A.Verdict));
			R.Assets.Add(MoveTemp(A));
		}
		for (const FString& Raw : Unresolved)
		{
			FKismetAIAssetReconciliation A;
			A.AssetPath = Raw;
			A.Verdict = EKismetAIReconcileVerdict::NoEvidence;
			A.Detail = TEXT("the call names this asset in a form that could not be resolved to a package");
			Worst = FMath::Max(Worst, Severity(A.Verdict));
			R.Assets.Add(MoveTemp(A));
		}
		if (R.Assets.Num() == 0)
		{
			R.Verdict = EKismetAIReconcileVerdict::NoEvidence;
			R.Explanation = TEXT("The call names no asset that could be fingerprinted.");
			return R;
		}
		// The call's verdict is its worst asset's (ties keep the first such asset's kind).
		for (const FKismetAIAssetReconciliation& A : R.Assets)
		{
			if (Severity(A.Verdict) == Worst)
			{
				R.Verdict = A.Verdict;
				break;
			}
		}
		switch (R.Verdict)
		{
		case EKismetAIReconcileVerdict::NotApplied:
			R.Explanation = TEXT("Every target equals its recorded before-state; the call has no visible effect.");
			break;
		case EKismetAIReconcileVerdict::NoEvidence:
			R.Explanation = TEXT("At least one target has no comparable before-state or current state.");
			break;
		default:
			R.Explanation = TEXT("At least one target differs from its recorded before-state; the call may have been applied or the asset changed otherwise.");
			break;
		}
		return R;
	}
}
