// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Cooperative cancellation.
//
// Cancellation used to be a volatile bool on the agent, a killed curl process,
// and an event trigger — checked at three points in the loop. It did not reach a
// tool already queued on the game thread, and there was no way to express "this
// turn's work" as distinct from "this run's work".
//
// A token tree fixes the shape: the session holds a root, each run derives a
// child, each turn a child of that. Cancelling any node cancels its subtree, so
// aborting a run does not require finding and poking every in-flight thing.
//
// Cancellation here is COOPERATIVE and it is about not *waiting*, not about not
// *finishing*. Unreal operations are not interruptible mid-call: once
// FKismetEditorUtilities::CompileBlueprint is running, it runs. What a token
// buys is that we stop waiting on it, stop starting new work, and record the
// outcome honestly.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "HAL/ThreadSafeBool.h"
#include "Misc/ScopeLock.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

class KISMETAIRUNTIME_API FKismetAICancellationToken
{
public:
	FKismetAICancellationToken() : State(MakeShared<FState, ESPMode::ThreadSafe>()) {}

	bool IsCancelled() const { return State->bCancelled; }

	// Cancels this token and every child derived from it. Idempotent, and safe
	// from any thread — abort arrives on the game thread while the worker is
	// mid-turn.
	void Cancel()
	{
		TArray<TSharedPtr<FState, ESPMode::ThreadSafe>> ToVisit;
		TArray<TPair<uint64, TFunction<void()>>> ToNotify;
		{
			FScopeLock Lock(&State->Mutex);
			if (State->bCancelled)
			{
				return;   // already cancelled; do not re-walk the tree
			}
			State->bCancelled = true;
			ToVisit = State->Children;
			ToNotify = MoveTemp(State->Listeners);
			State->Listeners.Reset();
		}

		// Notify and walk outside the lock: a listener may take its own locks,
		// and a child's Cancel takes the child's — holding this one across
		// either would invite a lock-order inversion. Listeners of this token
		// run before its children are cancelled.
		for (TPair<uint64, TFunction<void()>>& Listener : ToNotify)
		{
			Listener.Value();
		}
		for (const TSharedPtr<FState, ESPMode::ThreadSafe>& Child : ToVisit)
		{
			FKismetAICancellationToken(Child).Cancel();
		}
	}

	// Wake something that is waiting rather than polling IsCancelled.
	//
	// OnCancel runs exactly once, on the thread that cancels the token (any
	// thread, including the game thread), outside every token lock. If the
	// token is already cancelled it runs before Subscribe returns, on the
	// caller's thread, and the returned id is 0.
	//
	// Unsubscribe guarantees only that OnCancel will not START after it
	// returns; a call already in progress on another thread may still be
	// running. So OnCancel must own (not borrow) everything it touches — in
	// practice, capture a shared pointer to the waiter's state, never a stack
	// reference or a raw FEvent the waiter will return to the pool.
	uint64 Subscribe(TFunction<void()> OnCancel) const
	{
		{
			FScopeLock Lock(&State->Mutex);
			if (!State->bCancelled)
			{
				const uint64 Id = ++State->NextListenerId;
				State->Listeners.Emplace(Id, MoveTemp(OnCancel));
				return Id;
			}
		}
		OnCancel();
		return 0;
	}

	void Unsubscribe(uint64 ListenerId) const
	{
		if (ListenerId == 0)
		{
			return;
		}
		FScopeLock Lock(&State->Mutex);
		State->Listeners.RemoveAll([ListenerId](const TPair<uint64, TFunction<void()>>& L) { return L.Key == ListenerId; });
	}

	// Tests: listeners still registered (a waiter that forgot to unsubscribe
	// would show up here).
	int32 NumListenersForTests() const
	{
		FScopeLock Lock(&State->Mutex);
		return State->Listeners.Num();
	}

	// A token that cancels when this one does, and can also be cancelled alone.
	// Used for one turn, or one tool call, within a run.
	FKismetAICancellationToken CreateChild()
	{
		TSharedPtr<FState, ESPMode::ThreadSafe> ChildState = MakeShared<FState, ESPMode::ThreadSafe>();
		bool bAlreadyCancelled = false;
		{
			FScopeLock Lock(&State->Mutex);
			bAlreadyCancelled = State->bCancelled;
			if (!bAlreadyCancelled)
			{
				State->Children.Add(ChildState);
			}
		}
		// A child of an already-cancelled parent starts cancelled, rather than
		// silently escaping the cancellation it was born into.
		if (bAlreadyCancelled)
		{
			ChildState->bCancelled = true;
		}
		return FKismetAICancellationToken(ChildState);
	}

private:
	struct FState
	{
		FCriticalSection Mutex;
		TArray<TSharedPtr<FState, ESPMode::ThreadSafe>> Children;
		TArray<TPair<uint64, TFunction<void()>>> Listeners;
		uint64 NextListenerId = 0;
		FThreadSafeBool bCancelled = false;
	};

	explicit FKismetAICancellationToken(TSharedPtr<FState, ESPMode::ThreadSafe> InState)
		: State(MoveTemp(InState)) {}

	TSharedPtr<FState, ESPMode::ThreadSafe> State;
};
