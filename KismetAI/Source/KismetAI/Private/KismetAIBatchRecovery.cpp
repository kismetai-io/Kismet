// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIBatchRecovery.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Editor/Transactor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Misc/ITransaction.h"
#include "CoreGlobals.h"
#include "UObject/UnrealType.h"
#include "SceneTypes.h"

DEFINE_LOG_CATEGORY_STATIC(LogKismetAIBatchRecovery, Log, All);

namespace KismetAIBatchRecovery
{
	namespace
	{
		// Game-thread-only, like every caller.
		ERestoreFault GRestoreFault = ERestoreFault::None;
		TFunction<void()> GAfterRevert;

		FString DescribeIdentity(const UObject* Object)
		{
			if (!Object)
			{
				return TEXT("null");
			}
			return FString::Printf(TEXT("%p %s%s"), Object, *Object->GetPathName(),
				IsValid(Object) ? TEXT("") : TEXT(" [garbage]"));
		}

		// Cache keys a material regenerates on every PostEditChange — including
		// the PostEditUndo that performs the rollback — so they never compare
		// equal across an edit and its undo. They identify compiled shader maps
		// and lighting caches, not authored state (Material.cpp CreateGuid(StateId),
		// MaterialInterface.h SetLightingGuid).
		bool IsRegeneratedCacheKey(const UObject* Object, const FProperty* Prop)
		{
			static const FName StateId(TEXT("StateId"));
			static const FName LightingGuid(TEXT("LightingGuid"));
			return Object->IsA<UMaterialInterface>() && (Prop->GetFName() == StateId || Prop->GetFName() == LightingGuid);
		}

		// Every non-transient, non-deprecated property as exported text. Object
		// references export as paths, so expression input connections and
		// pin-less node settings are covered without per-class knowledge.
		void AddProperties(FKismetAIStateSnapshot& Out, const FString& Prefix, UObject* Object)
		{
			for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
			{
				const FProperty* Prop = *It;
				if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated) || IsRegeneratedCacheKey(Object, Prop))
				{
					continue;
				}
				for (int32 Index = 0; Index < Prop->ArrayDim; ++Index)
				{
					FString Value;
					Prop->ExportText_InContainer(Index, Value, Object, nullptr, Object, PPF_None);
					const FString Key = Prop->ArrayDim > 1
						? FString::Printf(TEXT("%s/prop/%s[%d]"), *Prefix, *Prop->GetName(), Index)
						: FString::Printf(TEXT("%s/prop/%s"), *Prefix, *Prop->GetName());
					Out.Entries.Add(Key, MoveTemp(Value));
				}
			}
		}

		FString NodeKey(const UEdGraphNode* Node)
		{
			return FString::Printf(TEXT("node/%s|%s"), *Node->NodeGuid.ToString(), *Node->GetName());
		}

		FString PinLinkLabel(const UEdGraphPin* Pin)
		{
			if (!Pin)
			{
				return TEXT("null");
			}
			const UEdGraphNode* Owner = Pin->GetOwningNodeUnchecked();
			return FString::Printf(TEXT("%s|%s:%s"),
				Owner ? *Owner->NodeGuid.ToString() : TEXT("?"),
				Owner ? *Owner->GetName() : TEXT("?"),
				*Pin->PinId.ToString());
		}
	}

	FKismetAIStateSnapshot CaptureGraph(const UEdGraph* Graph)
	{
		FKismetAIStateSnapshot Out;
		if (!Graph)
		{
			return Out;
		}
		Out.Entries.Add(TEXT("graph/identity"), DescribeIdentity(Graph));

		TArray<FString> NodeKeys;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				NodeKeys.Add(TEXT("null"));
				continue;
			}
			const FString Key = NodeKey(Node);
			NodeKeys.Add(Key);
			Out.Entries.Add(Key + TEXT("/identity"), DescribeIdentity(Node));
			Out.Entries.Add(Key + TEXT("/class"), Node->GetClass()->GetPathName());
			AddProperties(Out, Key, Node);

			TArray<FString> PinOrder;
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin)
				{
					PinOrder.Add(TEXT("null"));
					continue;
				}
				const FString PinKey = FString::Printf(TEXT("%s/pin/%s"), *Key, *Pin->PinId.ToString());
				PinOrder.Add(Pin->PinId.ToString());
				Out.Entries.Add(PinKey + TEXT("/name"), Pin->PinName.ToString());
				Out.Entries.Add(PinKey + TEXT("/direction"), Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"));
				Out.Entries.Add(PinKey + TEXT("/type"), FString::Printf(TEXT("%s|%s|%s|%d|%d"),
					*Pin->PinType.PinCategory.ToString(),
					*Pin->PinType.PinSubCategory.ToString(),
					*GetPathNameSafe(Pin->PinType.PinSubCategoryObject.Get()),
					(int32)Pin->PinType.ContainerType,
					Pin->PinType.bIsReference ? 1 : 0));
				Out.Entries.Add(PinKey + TEXT("/default"), Pin->DefaultValue);
				Out.Entries.Add(PinKey + TEXT("/default_object"), GetPathNameSafe(Pin->DefaultObject));
				Out.Entries.Add(PinKey + TEXT("/default_text"), Pin->DefaultTextValue.ToString());
				Out.Entries.Add(PinKey + TEXT("/flags"), FString::Printf(TEXT("hidden=%d orphaned=%d"),
					Pin->bHidden ? 1 : 0, Pin->bOrphanedPin ? 1 : 0));

				TArray<FString> Links;
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					Links.Add(PinLinkLabel(Linked));
				}
				Links.Sort();
				Out.Entries.Add(PinKey + TEXT("/links"), FString::Join(Links, TEXT(",")));
			}
			Out.Entries.Add(Key + TEXT("/pins"), FString::Join(PinOrder, TEXT(",")));
		}
		// Order matters to the editor (draw order, describe output), so it is
		// part of the state, not just membership.
		Out.Entries.Add(TEXT("graph/nodes"), FString::Join(NodeKeys, TEXT(",")));
		return Out;
	}

	FKismetAIStateSnapshot CaptureMaterial(UMaterial* Material)
	{
		FKismetAIStateSnapshot Out;
		if (!Material)
		{
			return Out;
		}
		Out.Entries.Add(TEXT("material/identity"), DescribeIdentity(Material));
		AddProperties(Out, TEXT("material"), Material);
		if (UMaterialEditorOnlyData* EditorOnly = Material->GetEditorOnlyData())
		{
			Out.Entries.Add(TEXT("material_editor_only/identity"), DescribeIdentity(EditorOnly));
			// Holds the expression collection and the property inputs; both
			// export their object references as paths.
			AddProperties(Out, TEXT("material_editor_only"), EditorOnly);
		}

		for (int32 PropIndex = 0; PropIndex < MP_MAX; ++PropIndex)
		{
			const FExpressionInput* Input = Material->GetExpressionInputForProperty(static_cast<EMaterialProperty>(PropIndex));
			if (!Input)
			{
				continue;
			}
			Out.Entries.Add(FString::Printf(TEXT("material/input/%d"), PropIndex), FString::Printf(TEXT("%s:%d"),
				Input->Expression ? *Input->Expression->GetName() : TEXT("none"), Input->OutputIndex));
		}

		TArray<FString> ExpressionKeys;
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			if (!Expression)
			{
				ExpressionKeys.Add(TEXT("null"));
				continue;
			}
			const FString Key = FString::Printf(TEXT("expr/%s|%s"),
				*Expression->MaterialExpressionGuid.ToString(), *Expression->GetName());
			ExpressionKeys.Add(Key);
			Out.Entries.Add(Key + TEXT("/identity"), DescribeIdentity(Expression));
			Out.Entries.Add(Key + TEXT("/class"), Expression->GetClass()->GetPathName());
			AddProperties(Out, Key, Expression);
		}
		Out.Entries.Add(TEXT("material/expressions"), FString::Join(ExpressionKeys, TEXT(",")));
		return Out;
	}

	FScopedRestoreFaultInjection::FScopedRestoreFaultInjection(ERestoreFault InFault, TFunction<void()> InAfterRevert)
	{
		GRestoreFault = InFault;
		GAfterRevert = MoveTemp(InAfterRevert);
	}

	FScopedRestoreFaultInjection::~FScopedRestoreFaultInjection()
	{
		GRestoreFault = ERestoreFault::None;
		GAfterRevert = nullptr;
	}
}

bool FKismetAIStateSnapshot::Compare(const FKismetAIStateSnapshot& Before, const FKismetAIStateSnapshot& After,
	TArray<FString>& OutDifferences, int32 MaxDiffs)
{
	int32 Total = 0;
	auto Report = [&](FString&& Line)
	{
		if (Total++ < MaxDiffs)
		{
			OutDifferences.Add(MoveTemp(Line));
		}
	};

	TArray<FString> Keys;
	Before.Entries.GetKeys(Keys);
	Keys.Sort();
	for (const FString& Key : Keys)
	{
		const FString& BeforeValue = Before.Entries.FindChecked(Key);
		if (const FString* AfterValue = After.Entries.Find(Key))
		{
			if (!AfterValue->Equals(BeforeValue, ESearchCase::CaseSensitive))
			{
				Report(FString::Printf(TEXT("changed %s: '%s' -> '%s'"), *Key,
					*BeforeValue.Left(200), *AfterValue->Left(200)));
			}
		}
		else
		{
			Report(FString::Printf(TEXT("missing %s (was '%s')"), *Key, *BeforeValue.Left(200)));
		}
	}

	TArray<FString> AfterKeys;
	After.Entries.GetKeys(AfterKeys);
	AfterKeys.Sort();
	for (const FString& Key : AfterKeys)
	{
		if (!Before.Entries.Contains(Key))
		{
			Report(FString::Printf(TEXT("added %s = '%s'"), *Key, *After.Entries.FindChecked(Key).Left(200)));
		}
	}

	if (Total > MaxDiffs)
	{
		OutDifferences.Add(FString::Printf(TEXT("... and %d more difference(s)"), Total - MaxDiffs));
	}
	return Total == 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// FKismetAIAtomicBatchScope
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	bool IsAnyTransactionOpen()
	{
		return GUndo != nullptr || (GEditor && GEditor->IsTransactionActive());
	}
}

FKismetAIAtomicBatchScope::FKismetAIAtomicBatchScope(const FText& Description)
	: bOutermost(!IsAnyTransactionOpen())
{
	Transaction.Emplace(Description);
	bHasTransaction = Transaction->IsOutstanding() && GUndo != nullptr;
	if (bHasTransaction)
	{
		TransactionId = GUndo->GetContext().TransactionId;
	}
}

FKismetAIAtomicBatchScope::~FKismetAIAtomicBatchScope()
{
	// Ends the transaction normally when it is still open (the batch succeeded,
	// or its revert did not run): it becomes an ordinary undo entry.
	Transaction.Reset();
}

bool FKismetAIAtomicBatchScope::Track(UObject* Object)
{
	if (!Object)
	{
		return true;
	}
	if (!Object->HasAnyFlags(RF_Transactional))
	{
		Object->SetFlags(RF_Transactional);
	}
	// Modify() returns false both when the object could not be recorded and
	// when it already was, so ask the transaction instead.
	Object->Modify();
	const bool bRecorded = GUndo != nullptr && GUndo->ContainsObject(Object);
	if (!bRecorded)
	{
		++UntrackedCount;
		UE_LOG(LogKismetAIBatchRecovery, Warning, TEXT("Atomic batch could not record %s for rollback"), *GetPathNameSafe(Object));
	}
	return bRecorded;
}

bool FKismetAIAtomicBatchScope::IsUndoEntryKept() const
{
	if (bReverted)
	{
		return false;
	}
	if (Transaction.IsSet() && Transaction->IsOutstanding())
	{
		return true; // ends with the scope and stays in the buffer
	}
	const UTransBuffer* Buffer = GEditor ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
	return Buffer && TransactionId.IsValid() && Buffer->FindTransactionIndex(TransactionId) != INDEX_NONE;
}

bool FKismetAIAtomicBatchScope::RevertAndDiscard()
{
	using namespace KismetAIBatchRecovery;

	if (bReverted || !CanRestore() || GRestoreFault == ERestoreFault::SkipRevert)
	{
		return false;
	}
	UTransBuffer* Buffer = GEditor ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
	if (!Buffer || !TransactionId.IsValid())
	{
		return false;
	}

	// Commit the batch's transaction. It is the outermost one, so End()
	// finalizes it and appends it as the newest undo entry.
	Transaction.Reset();

	// Undo applies the newest entry. Make sure that entry is exactly ours —
	// nothing can run between End() and here (same game-thread call), but the
	// buffer can refuse or reshape it: an undo barrier, a transient (PIE-only)
	// transaction that End() dropped, or pending redo entries.
	const int32 Index = Buffer->FindTransactionIndex(TransactionId);
	const FTransaction* Committed = Index != INDEX_NONE ? Buffer->GetTransaction(Index) : nullptr;
	if (!Committed
		|| Index != Buffer->GetQueueLength() - 1
		|| Buffer->GetUndoCount() != 0
		|| Committed->HasExpired()
		|| !Buffer->CanUndo())
	{
		UE_LOG(LogKismetAIBatchRecovery, Warning,
			TEXT("Atomic batch rollback refused: batch transaction index %d of %d, undo count %d, can undo %d"),
			Index, Buffer->GetQueueLength(), Buffer->GetUndoCount(), Buffer->CanUndo() ? 1 : 0);
		return false;
	}

	// bCanRedo=false: the undone entry is removed from the buffer, so the
	// batch leaves no undo/redo entry behind. Only this one entry is applied;
	// every earlier entry (a human's edits included) is untouched.
	if (!GEditor->UndoTransaction(/*bCanRedo=*/false))
	{
		UE_LOG(LogKismetAIBatchRecovery, Warning, TEXT("Atomic batch rollback: the editor refused to undo the batch transaction"));
		return false;
	}
	bReverted = true;
	if (Buffer->FindTransactionIndex(TransactionId) != INDEX_NONE)
	{
		UE_LOG(LogKismetAIBatchRecovery, Warning, TEXT("Atomic batch rollback: the batch transaction is still in the undo buffer after undo"));
	}

	if (GRestoreFault == ERestoreFault::AfterRevert && GAfterRevert)
	{
		GAfterRevert();
	}
	return true;
}
