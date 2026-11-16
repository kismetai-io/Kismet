// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIMessageQueue.h"

// ---------------------------------------------------------------------------
// MakeId
// ---------------------------------------------------------------------------

FString FKismetAIMessageQueue::MakeId()
{
	return FGuid::NewGuid().ToString(EGuidFormats::Digits);
}

// ---------------------------------------------------------------------------
// EscapeJsonString
//
// Unlike asset-name escaping (KismetAIBridge.cpp SearchAssets), queued messages
// carry arbitrary user text — including newlines from Shift+Enter. ToJson's output
// is injected verbatim into a JS expression (BroadcastQueueState → onQueueState),
// so control chars must be escaped or the broadcast is malformed JS and the UI
// never updates. Backslash first, then quote and the control chars.
// ---------------------------------------------------------------------------

FString FKismetAIMessageQueue::EscapeJsonString(const FString& In)
{
	return In
		.Replace(TEXT("\\"), TEXT("\\\\"))
		.Replace(TEXT("\""), TEXT("\\\""))
		.Replace(TEXT("\n"), TEXT("\\n"))
		.Replace(TEXT("\r"), TEXT("\\r"))
		.Replace(TEXT("\t"), TEXT("\\t"));
}

// ---------------------------------------------------------------------------
// FindIndex — linear scan for Id in Items
// ---------------------------------------------------------------------------

int32 FKismetAIMessageQueue::FindIndex(const FString& Id) const
{
	for (int32 i = 0; i < Items.Num(); ++i)
	{
		if (Items[i].Id == Id)
		{
			return i;
		}
	}
	return INDEX_NONE;
}

// ---------------------------------------------------------------------------
// Enqueue
// ---------------------------------------------------------------------------

FString FKismetAIMessageQueue::Enqueue(const FString& Text, const FString& RefsJson, const FString& ImagesJson)
{
	FQueuedMessage Msg;
	Msg.Id         = MakeId();
	Msg.Text       = Text;
	Msg.RefsJson   = RefsJson;
	Msg.ImagesJson = ImagesJson;
	Items.Add(Msg);
	return Msg.Id;
}

// ---------------------------------------------------------------------------
// Remove
// ---------------------------------------------------------------------------

bool FKismetAIMessageQueue::Remove(const FString& Id)
{
	int32 Idx = FindIndex(Id);
	if (Idx == INDEX_NONE)
	{
		return false;
	}
	Items.RemoveAt(Idx);
	return true;
}

// ---------------------------------------------------------------------------
// Move
// ---------------------------------------------------------------------------

bool FKismetAIMessageQueue::Move(const FString& Id, int32 NewIndex)
{
	int32 Idx = FindIndex(Id);
	if (Idx == INDEX_NONE)
	{
		return false;
	}

	// Clamp to valid range
	const int32 LastIdx = Items.Num() - 1;
	NewIndex = FMath::Clamp(NewIndex, 0, LastIdx);

	if (Idx == NewIndex)
	{
		return true;
	}

	FQueuedMessage Msg = Items[Idx];
	Items.RemoveAt(Idx);
	Items.Insert(Msg, NewIndex);
	return true;
}

// ---------------------------------------------------------------------------
// Edit
// ---------------------------------------------------------------------------

bool FKismetAIMessageQueue::Edit(const FString& Id, const FString& NewText)
{
	int32 Idx = FindIndex(Id);
	if (Idx == INDEX_NONE)
	{
		return false;
	}
	Items[Idx].Text = NewText;
	return true;
}

// ---------------------------------------------------------------------------
// PromoteToSteer
//
// If PendingSteer is already set, front-insert M into Items instead of
// overwriting — ensures no message is ever dropped.
// ---------------------------------------------------------------------------

void FKismetAIMessageQueue::PromoteToSteer(const FQueuedMessage& M)
{
	if (!PendingSteer.IsSet())
	{
		PendingSteer = M;
	}
	else
	{
		// PendingSteer occupied — front-insert so dispatch order is:
		//   existing PendingSteer → M → Items[0..N-1]
		Items.Insert(M, 0);
	}
}

// ---------------------------------------------------------------------------
// PromoteQueuedToSteer
// ---------------------------------------------------------------------------

bool FKismetAIMessageQueue::PromoteQueuedToSteer(const FString& Id)
{
	int32 Idx = FindIndex(Id);
	if (Idx == INDEX_NONE)
	{
		return false;
	}
	FQueuedMessage Msg = Items[Idx];
	Items.RemoveAt(Idx);
	PromoteToSteer(Msg);
	return true;
}

// ---------------------------------------------------------------------------
// PickNext — exactly one item dispatched, removed atomically
// ---------------------------------------------------------------------------

TOptional<FQueuedMessage> FKismetAIMessageQueue::PickNext()
{
	if (PendingSteer.IsSet())
	{
		TOptional<FQueuedMessage> Result = PendingSteer;
		PendingSteer.Reset();
		return Result;
	}

	if (Items.Num() > 0)
	{
		FQueuedMessage Msg = Items[0];
		Items.RemoveAt(0);
		return TOptional<FQueuedMessage>(Msg);
	}

	return TOptional<FQueuedMessage>();
}

// ---------------------------------------------------------------------------
// IsEmpty
// ---------------------------------------------------------------------------

bool FKismetAIMessageQueue::IsEmpty() const
{
	return !PendingSteer.IsSet() && Items.Num() == 0;
}

// ---------------------------------------------------------------------------
// HasPendingSteer
// ---------------------------------------------------------------------------

bool FKismetAIMessageQueue::HasPendingSteer() const
{
	return PendingSteer.IsSet();
}

// ---------------------------------------------------------------------------
// ToJson
//
// Returns a JSON array in dispatch order: PendingSteer first (if set), then
// Items in order.  Each element: {"id":"...","text":"..."} (no refs/images).
// ---------------------------------------------------------------------------

FString FKismetAIMessageQueue::ToJson() const
{
	FString Json = TEXT("[");
	bool bFirst = true;

	auto AppendEntry = [&](const FQueuedMessage& Msg)
	{
		if (!bFirst)
		{
			Json += TEXT(",");
		}
		bFirst = false;
		Json += FString::Printf(TEXT("{\"id\":\"%s\",\"text\":\"%s\"}"),
			*EscapeJsonString(Msg.Id),
			*EscapeJsonString(Msg.Text));
	};

	if (PendingSteer.IsSet())
	{
		AppendEntry(PendingSteer.GetValue());
	}

	for (const FQueuedMessage& Msg : Items)
	{
		AppendEntry(Msg);
	}

	Json += TEXT("]");
	return Json;
}
