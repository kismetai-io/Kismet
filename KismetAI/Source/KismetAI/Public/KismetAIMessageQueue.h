// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"
#include "Misc/Optional.h"

// ---------------------------------------------------------------------------
// FQueuedMessage — a single queued prompt captured at enqueue time.
// RefsJson and ImagesJson are opaque blobs stored verbatim and returned
// unchanged by PickNext; they are NOT included in ToJson output.
// ---------------------------------------------------------------------------

struct FQueuedMessage
{
	FString Id;
	FString Text;
	FString RefsJson;
	FString ImagesJson;
};

// ---------------------------------------------------------------------------
// FKismetAIMessageQueue — pure state machine, no UE threading dependencies.
//
// Dispatch order:
//   1. PendingSteer (if set) — returned first by PickNext.
//   2. Items[0] … Items[N-1] — FIFO.
//
// Invariants:
//   Single source of truth — no state duplicated elsewhere.
//   Payload (Text/RefsJson/ImagesJson) returned verbatim by PickNext.
//   Exactly one item dispatched per PickNext; chosen item removed atomically.
//   Never silently drop a message — PromoteToSteer-while-pending
//        front-inserts into Items instead of overwriting.
// ---------------------------------------------------------------------------

class KISMETAI_API FKismetAIMessageQueue
{
public:

	// -----------------------------------------------------------------------
	// Enqueue / modify
	// -----------------------------------------------------------------------

	/** Append a new message; returns the generated Id. */
	FString Enqueue(const FString& Text, const FString& RefsJson, const FString& ImagesJson);

	/** Remove by Id. Returns false if not found. */
	bool Remove(const FString& Id);

	/**
	 * Reorder: move the item with the given Id to NewIndex.
	 * NewIndex is clamped to [0, Items.Num()-1].
	 * Returns false if Id not found.
	 */
	bool Move(const FString& Id, int32 NewIndex);

	/** Replace the Text of the item with the given Id. Returns false if not found. */
	bool Edit(const FString& Id, const FString& NewText);

	// -----------------------------------------------------------------------
	// Steer promotion
	// -----------------------------------------------------------------------

	/**
	 * Promote M to PendingSteer if it is currently unset.
	 * If PendingSteer is ALREADY set, front-insert M into Items instead
	 * (never drop or overwrite the existing pending steer).
	 */
	void PromoteToSteer(const FQueuedMessage& M);

	/**
	 * Find+remove the item with the given Id from Items, then call
	 * PromoteToSteer on it.  Returns false if Id not found.
	 */
	bool PromoteQueuedToSteer(const FString& Id);

	// -----------------------------------------------------------------------
	// Dispatch
	// -----------------------------------------------------------------------

	/**
	 * Pop the next item in dispatch order:
	 *   1. PendingSteer (cleared after return).
	 *   2. Items[0] (removed after return).
	 *   3. TOptional unset if both are empty.
	 */
	TOptional<FQueuedMessage> PickNext();

	// -----------------------------------------------------------------------
	// Inspection
	// -----------------------------------------------------------------------

	bool IsEmpty() const;
	bool HasPendingSteer() const;

	/**
	 * Serialise to a JSON array in dispatch order:
	 *   [PendingSteer (if set), Items[0], ..., Items[N-1]]
	 * Each element is {"id":"...","text":"..."} only (no refs/images).
	 * Text is JSON-escaped (backslash and double-quote at minimum).
	 */
	FString ToJson() const;

	/** Returns a fresh GUID string in Digits format. */
	static FString MakeId();

private:

	TArray<FQueuedMessage> Items;
	TOptional<FQueuedMessage> PendingSteer;

	/** Find the index of the item with the given Id; returns INDEX_NONE if absent. */
	int32 FindIndex(const FString& Id) const;

	/** Escape a string for inclusion inside a JSON string value. */
	static FString EscapeJsonString(const FString& In);
};
