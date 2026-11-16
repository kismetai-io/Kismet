// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Real rollback for atomic edit batches.
//
// edit_blueprint and edit_material accept `atomic: true`: if any operation in
// the batch fails, the batch must leave the asset exactly as it found it. The
// handlers used to call FScopedTransaction::Cancel() and report "rolled back,
// graph unchanged". In UE 5.8 that is not a rollback: UTransBuffer::Cancel
// (EditorTransaction.cpp) pops the in-flight transaction record and throws it
// away without applying it, so every edit made before the failing operation
// stayed in memory — and the record a user could have undone was gone.
//
// The mechanism: the batch runs inside its own outermost transaction. On
// failure that transaction is committed and then undone with
// UEditorEngine::UndoTransaction(bCanRedo=false), after checking that the
// newest undo entry is exactly the batch's own transaction (by transaction id,
// no pending redo, not expired, no undo barrier in the way). Undo applies only
// that entry and, with bCanRedo=false, removes it; every earlier entry — a
// human's transaction included — keeps both its record and its effect. This is
// a scoped undo of one identified entry, never a blind global Undo.
//
// Why not the engine's "GUndo->Apply(); Cancel()" idiom (AbortTracking,
// CancelUndoTransaction)? It works for plain objects but not for graphs:
// UEdGraphPin serialization asks UTransBuffer::IsObjectTransacting, which
// ensure()s and answers false unless UTransBuffer::Undo/Redo set its private
// CurrentTransaction. Applied that way, a deleted node came back without the
// links on its peers (EdGraphPin.cpp ResolveReferencesToPin / SerializePin).
// The committed-then-undone path goes through Undo, so pin links resolve.
//
// Known cost: committing the batch transaction discards pending redo entries
// and lets the buffer evict its oldest entries when over its memory budget —
// the same thing any successful edit does.
//
// Two conditions make that safe, and the scope enforces both:
//   - The batch's transaction must be the OUTERMOST one. Nested inside another
//     open transaction its records belong to the outer transaction and could
//     only be undone together with it (and the old Cancel() path would have
//     discarded the whole outer transaction). An atomic batch is refused up
//     front in that case, before anything is changed.
//   - Every object the batch can change must be recorded before it changes.
//     Several engine edit paths do not call Modify() (material expression
//     connect/disconnect/delete, raw property writes), so the handlers record
//     the whole affected object set before the first operation.
//
// The rollback is then VERIFIED, not assumed: a semantic snapshot of the
// graph or material taken before the batch is compared with the state after
// restoration. Only an equal snapshot is reported as `restored`.
//
// Game thread only.

#pragma once

#include "CoreMinimal.h"
#include "ScopedTransaction.h"

class UEdGraph;
class UMaterial;
class UObject;

// A flat, ordered key -> value description of an asset's editable state. Keys
// are stable across the restore (node GUID + name, pin id), values carry
// object identity (address + path), class, position, every non-transient
// property, pin types, defaults and links. Two snapshots of an unchanged asset
// compare equal; any field the batch could have touched shows up as a diff.
struct FKismetAIStateSnapshot
{
	TMap<FString, FString> Entries;

	bool IsEmpty() const { return Entries.Num() == 0; }

	// Describes every key that was added, removed or changed, up to MaxDiffs
	// lines (one extra line says how many were cut). True when equal.
	static bool Compare(const FKismetAIStateSnapshot& Before, const FKismetAIStateSnapshot& After,
		TArray<FString>& OutDifferences, int32 MaxDiffs = 25);
};

namespace KismetAIBatchRecovery
{
	// Graph nodes (set, identity, GUIDs, class, properties), pins (id, name,
	// direction, type, default value/object/text) and links.
	FKismetAIStateSnapshot CaptureGraph(const UEdGraph* Graph);

	// Expressions (set, identity, GUIDs, class, properties — which include
	// every expression input connection), material property inputs (BaseColor
	// etc.) and the material's own non-transient properties.
	FKismetAIStateSnapshot CaptureMaterial(UMaterial* Material);

	// Test seam: make the next restoration fail on purpose.
	enum class ERestoreFault : uint8
	{
		None,
		// The revert step does not run (as if no transaction were available).
		SkipRevert,
		// The revert runs, then AfterRevert is invoked before verification —
		// used to simulate state the transaction record did not cover.
		AfterRevert,
	};

	struct FScopedRestoreFaultInjection
	{
		explicit FScopedRestoreFaultInjection(ERestoreFault InFault, TFunction<void()> InAfterRevert = nullptr);
		~FScopedRestoreFaultInjection();
		FScopedRestoreFaultInjection(const FScopedRestoreFaultInjection&) = delete;
		FScopedRestoreFaultInjection& operator=(const FScopedRestoreFaultInjection&) = delete;
	};
}

// The batch's recovery boundary: one outermost transaction whose record is
// applied (not just discarded) when the batch must be undone.
class FKismetAIAtomicBatchScope
{
public:
	explicit FKismetAIAtomicBatchScope(const FText& Description);
	~FKismetAIAtomicBatchScope();

	FKismetAIAtomicBatchScope(const FKismetAIAtomicBatchScope&) = delete;
	FKismetAIAtomicBatchScope& operator=(const FKismetAIAtomicBatchScope&) = delete;

	// False when another transaction was already open when the batch began, or
	// no transaction could be opened. Restoration is impossible without
	// touching someone else's transaction, so atomic batches must not run.
	bool CanRestore() const { return bOutermost && bHasTransaction; }

	// Record Object's current state in this batch's transaction (sets
	// RF_Transactional when missing, as the graph importer does for nodes).
	// Returns false when the object could not be recorded.
	bool Track(UObject* Object);

	// Number of Track() calls that could not record their object.
	int32 GetUntrackedCount() const { return UntrackedCount; }

	// Commit this batch's transaction, check it is the newest undo entry, and
	// undo exactly that entry (removing it). Returns false when the revert did
	// not run; nothing was reverted then, and IsUndoEntryKept() says whether
	// the batch is still in the undo buffer for a human to undo.
	bool RevertAndDiscard();

	// The revert ran and the batch's undo entry was consumed.
	bool WasReverted() const { return bReverted; }

	// The batch's transaction is (or, once the scope ends, will be) an
	// ordinary entry in the undo buffer.
	bool IsUndoEntryKept() const;

private:
	bool bOutermost = false;
	bool bHasTransaction = false;
	bool bReverted = false;
	int32 UntrackedCount = 0;
	FGuid TransactionId;
	TOptional<FScopedTransaction> Transaction;
};
