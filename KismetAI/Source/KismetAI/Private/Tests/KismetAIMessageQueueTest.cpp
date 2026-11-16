// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIMessageQueue.h"

#include "Misc/AutomationTest.h"

constexpr auto MQTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// FMessageQueueEnqueue
//
// Enqueue appends messages in order, returns unique non-empty ids, and
// ToJson reflects correct order and text content.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMessageQueueEnqueue,
	"KismetAI.MessageQueue.Enqueue", MQTestFlags)

bool FMessageQueueEnqueue::RunTest(const FString& Parameters)
{
	FKismetAIMessageQueue Q;

	// Queue starts empty
	TestTrue(TEXT("Queue starts empty"), Q.IsEmpty());

	FString Id1 = Q.Enqueue(TEXT("alpha"), TEXT("[]"), TEXT("[]"));
	FString Id2 = Q.Enqueue(TEXT("beta"),  TEXT("[]"), TEXT("[]"));
	FString Id3 = Q.Enqueue(TEXT("gamma"), TEXT("[]"), TEXT("[]"));

	// Ids are non-empty
	TestFalse(TEXT("Id1 non-empty"), Id1.IsEmpty());
	TestFalse(TEXT("Id2 non-empty"), Id2.IsEmpty());
	TestFalse(TEXT("Id3 non-empty"), Id3.IsEmpty());

	// Ids are unique
	TestFalse(TEXT("Id1 != Id2"), Id1 == Id2);
	TestFalse(TEXT("Id2 != Id3"), Id2 == Id3);
	TestFalse(TEXT("Id1 != Id3"), Id1 == Id3);

	// Queue is no longer empty
	TestFalse(TEXT("Queue not empty after enqueue"), Q.IsEmpty());

	// ToJson reflects insertion order and text
	FString Json = Q.ToJson();
	int32 AlphaPos = Json.Find(TEXT("alpha"));
	int32 BetaPos  = Json.Find(TEXT("beta"));
	int32 GammaPos = Json.Find(TEXT("gamma"));

	TestTrue(TEXT("alpha in ToJson"), AlphaPos != INDEX_NONE);
	TestTrue(TEXT("beta in ToJson"),  BetaPos  != INDEX_NONE);
	TestTrue(TEXT("gamma in ToJson"), GammaPos != INDEX_NONE);
	TestTrue(TEXT("alpha before beta"),  AlphaPos < BetaPos);
	TestTrue(TEXT("beta before gamma"),  BetaPos  < GammaPos);

	// ToJson contains the ids
	TestTrue(TEXT("Id1 in ToJson"), Json.Contains(Id1));
	TestTrue(TEXT("Id2 in ToJson"), Json.Contains(Id2));
	TestTrue(TEXT("Id3 in ToJson"), Json.Contains(Id3));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FMessageQueueRemove
//
// Remove by id works; unknown id returns false.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMessageQueueRemove,
	"KismetAI.MessageQueue.Remove", MQTestFlags)

bool FMessageQueueRemove::RunTest(const FString& Parameters)
{
	FKismetAIMessageQueue Q;

	FString Id1 = Q.Enqueue(TEXT("one"),   TEXT("[]"), TEXT("[]"));
	FString Id2 = Q.Enqueue(TEXT("two"),   TEXT("[]"), TEXT("[]"));
	FString Id3 = Q.Enqueue(TEXT("three"), TEXT("[]"), TEXT("[]"));

	// Unknown id returns false
	TestFalse(TEXT("Remove unknown id returns false"),
		Q.Remove(TEXT("does-not-exist")));

	// Remove middle item
	TestTrue(TEXT("Remove known id returns true"), Q.Remove(Id2));

	// Middle item no longer in json
	FString Json = Q.ToJson();
	TestTrue(TEXT("one still in json"),   Json.Contains(TEXT("one")));
	TestFalse(TEXT("two removed from json"), Json.Contains(TEXT("two")));
	TestTrue(TEXT("three still in json"), Json.Contains(TEXT("three")));

	// Remove remaining items
	TestTrue(TEXT("Remove Id1"), Q.Remove(Id1));
	TestTrue(TEXT("Remove Id3"), Q.Remove(Id3));
	TestTrue(TEXT("Queue empty after all removed"), Q.IsEmpty());

	// Remove from empty returns false
	TestFalse(TEXT("Remove from empty returns false"), Q.Remove(Id1));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FMessageQueueMove
//
// Move reorders; out-of-range NewIndex clamps; unknown id returns false.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMessageQueueMove,
	"KismetAI.MessageQueue.Move", MQTestFlags)

bool FMessageQueueMove::RunTest(const FString& Parameters)
{
	FKismetAIMessageQueue Q;

	FString IdA = Q.Enqueue(TEXT("A"), TEXT("[]"), TEXT("[]"));
	FString IdB = Q.Enqueue(TEXT("B"), TEXT("[]"), TEXT("[]"));
	FString IdC = Q.Enqueue(TEXT("C"), TEXT("[]"), TEXT("[]"));
	FString IdD = Q.Enqueue(TEXT("D"), TEXT("[]"), TEXT("[]"));

	// Unknown id returns false
	TestFalse(TEXT("Move unknown id returns false"),
		Q.Move(TEXT("nope"), 0));

	// Move last item to front (index 0)
	TestTrue(TEXT("Move D to front"), Q.Move(IdD, 0));
	{
		// Dispatch order should be D, A, B, C
		TOptional<FQueuedMessage> M0 = Q.PickNext();
		TestTrue(TEXT("After Move: first is D"), M0.IsSet() && M0->Text == TEXT("D"));
		TOptional<FQueuedMessage> M1 = Q.PickNext();
		TestTrue(TEXT("After Move: second is A"), M1.IsSet() && M1->Text == TEXT("A"));
		TOptional<FQueuedMessage> M2 = Q.PickNext();
		TestTrue(TEXT("After Move: third is B"), M2.IsSet() && M2->Text == TEXT("B"));
		TOptional<FQueuedMessage> M3 = Q.PickNext();
		TestTrue(TEXT("After Move: fourth is C"), M3.IsSet() && M3->Text == TEXT("C"));
	}

	// Clamp to last index when NewIndex > Num-1
	FString IdX = Q.Enqueue(TEXT("X"), TEXT("[]"), TEXT("[]"));
	FString IdY = Q.Enqueue(TEXT("Y"), TEXT("[]"), TEXT("[]"));
	FString IdZ = Q.Enqueue(TEXT("Z"), TEXT("[]"), TEXT("[]"));

	TestTrue(TEXT("Move X to 99 clamps to last"), Q.Move(IdX, 99));
	{
		TOptional<FQueuedMessage> M0 = Q.PickNext();
		TestTrue(TEXT("After clamp: first is Y"), M0.IsSet() && M0->Text == TEXT("Y"));
		TOptional<FQueuedMessage> M1 = Q.PickNext();
		TestTrue(TEXT("After clamp: second is Z"), M1.IsSet() && M1->Text == TEXT("Z"));
		TOptional<FQueuedMessage> M2 = Q.PickNext();
		TestTrue(TEXT("After clamp: third is X"), M2.IsSet() && M2->Text == TEXT("X"));
	}

	// Clamp to 0 when NewIndex < 0
	FString IdP = Q.Enqueue(TEXT("P"), TEXT("[]"), TEXT("[]"));
	FString IdQ2 = Q.Enqueue(TEXT("Q2"), TEXT("[]"), TEXT("[]"));

	TestTrue(TEXT("Move Q2 to -1 clamps to 0"), Q.Move(IdQ2, -1));
	{
		TOptional<FQueuedMessage> M0 = Q.PickNext();
		TestTrue(TEXT("After neg-clamp: first is Q2"), M0.IsSet() && M0->Text == TEXT("Q2"));
		TOptional<FQueuedMessage> M1 = Q.PickNext();
		TestTrue(TEXT("After neg-clamp: second is P"), M1.IsSet() && M1->Text == TEXT("P"));
	}

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FMessageQueueEdit
//
// Edit updates text; unknown id returns false.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMessageQueueEdit,
	"KismetAI.MessageQueue.Edit", MQTestFlags)

bool FMessageQueueEdit::RunTest(const FString& Parameters)
{
	FKismetAIMessageQueue Q;

	FString Id1 = Q.Enqueue(TEXT("original"), TEXT("[]"), TEXT("[]"));

	// Unknown id returns false
	TestFalse(TEXT("Edit unknown id returns false"),
		Q.Edit(TEXT("nope"), TEXT("updated")));

	// Edit known id
	TestTrue(TEXT("Edit known id returns true"),
		Q.Edit(Id1, TEXT("updated")));

	// Updated text appears in json; original does not
	FString Json = Q.ToJson();
	TestTrue(TEXT("updated in json"),  Json.Contains(TEXT("updated")));
	TestFalse(TEXT("original not in json"), Json.Contains(TEXT("original")));

	// PickNext returns updated text
	TOptional<FQueuedMessage> M = Q.PickNext();
	TestTrue(TEXT("PickNext returns updated text"),
		M.IsSet() && M->Text == TEXT("updated"));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FMessageQueuePickNext
//
// Empty → unset; returns front and removes it; PendingSteer takes precedence
// over Items front; PendingSteer is cleared after return.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMessageQueuePickNext,
	"KismetAI.MessageQueue.PickNext", MQTestFlags)

bool FMessageQueuePickNext::RunTest(const FString& Parameters)
{
	// Empty queue returns unset
	{
		FKismetAIMessageQueue Q;
		TOptional<FQueuedMessage> M = Q.PickNext();
		TestFalse(TEXT("PickNext on empty returns unset"), M.IsSet());
	}

	// Normal FIFO from Items
	{
		FKismetAIMessageQueue Q;
		Q.Enqueue(TEXT("first"),  TEXT("[]"), TEXT("[]"));
		Q.Enqueue(TEXT("second"), TEXT("[]"), TEXT("[]"));
		Q.Enqueue(TEXT("third"),  TEXT("[]"), TEXT("[]"));

		TOptional<FQueuedMessage> M1 = Q.PickNext();
		TestTrue(TEXT("First PickNext returns first item"),
			M1.IsSet() && M1->Text == TEXT("first"));

		TOptional<FQueuedMessage> M2 = Q.PickNext();
		TestTrue(TEXT("Second PickNext returns second item"),
			M2.IsSet() && M2->Text == TEXT("second"));

		TOptional<FQueuedMessage> M3 = Q.PickNext();
		TestTrue(TEXT("Third PickNext returns third item"),
			M3.IsSet() && M3->Text == TEXT("third"));

		TOptional<FQueuedMessage> M4 = Q.PickNext();
		TestFalse(TEXT("Fourth PickNext returns unset (empty)"), M4.IsSet());
	}

	// PendingSteer takes precedence over Items front
	{
		FKismetAIMessageQueue Q;
		Q.Enqueue(TEXT("queue-item"), TEXT("[]"), TEXT("[]"));

		FQueuedMessage Steer;
		Steer.Id   = FKismetAIMessageQueue::MakeId();
		Steer.Text = TEXT("steer-msg");

		Q.PromoteToSteer(Steer);
		TestTrue(TEXT("HasPendingSteer after PromoteToSteer"), Q.HasPendingSteer());

		TOptional<FQueuedMessage> M1 = Q.PickNext();
		TestTrue(TEXT("PickNext returns steer first"),
			M1.IsSet() && M1->Text == TEXT("steer-msg"));
		TestFalse(TEXT("PendingSteer cleared after PickNext"), Q.HasPendingSteer());

		// Items still intact
		TOptional<FQueuedMessage> M2 = Q.PickNext();
		TestTrue(TEXT("Items still available after steer dispatched"),
			M2.IsSet() && M2->Text == TEXT("queue-item"));

		TestTrue(TEXT("Queue empty after all dispatched"), Q.IsEmpty());
	}

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FMessageQueuePromoteToSteerWhilePending
//
// When a PendingSteer is already set, calling PromoteToSteer again must NOT
// overwrite the existing pending steer — instead it front-inserts the new
// message into Items (never drop a message).
//
// Verified via PickNext sequence:
//   PickNext #1 → original pending steer
//   PickNext #2 → front-inserted message (was the "new" steer attempt)
//   PickNext #3 → any remaining Items
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMessageQueuePromoteToSteerWhilePending,
	"KismetAI.MessageQueue.PromoteToSteerWhilePending", MQTestFlags)

bool FMessageQueuePromoteToSteerWhilePending::RunTest(const FString& Parameters)
{
	FKismetAIMessageQueue Q;

	// Existing queue item
	Q.Enqueue(TEXT("existing"), TEXT("[]"), TEXT("[]"));

	// Promote first steer — goes to PendingSteer
	FQueuedMessage Steer1;
	Steer1.Id   = FKismetAIMessageQueue::MakeId();
	Steer1.Text = TEXT("steer-1");
	Q.PromoteToSteer(Steer1);
	TestTrue(TEXT("HasPendingSteer after first promote"), Q.HasPendingSteer());

	// Promote second steer while first is pending — must front-insert into Items
	FQueuedMessage Steer2;
	Steer2.Id   = FKismetAIMessageQueue::MakeId();
	Steer2.Text = TEXT("steer-2");
	Q.PromoteToSteer(Steer2);

	// PendingSteer must still be steer-1
	TestTrue(TEXT("PendingSteer still set"), Q.HasPendingSteer());

	// PickNext #1 must be steer-1 (original pending)
	TOptional<FQueuedMessage> M1 = Q.PickNext();
	TestTrue(TEXT("PickNext #1 is original pending steer (steer-1)"),
		M1.IsSet() && M1->Text == TEXT("steer-1"));
	TestFalse(TEXT("PendingSteer cleared"), Q.HasPendingSteer());

	// PickNext #2 must be steer-2 (front-inserted)
	TOptional<FQueuedMessage> M2 = Q.PickNext();
	TestTrue(TEXT("PickNext #2 is front-inserted steer-2"),
		M2.IsSet() && M2->Text == TEXT("steer-2"));

	// PickNext #3 must be the original queue item
	TOptional<FQueuedMessage> M3 = Q.PickNext();
	TestTrue(TEXT("PickNext #3 is original queue item"),
		M3.IsSet() && M3->Text == TEXT("existing"));

	// Nothing dropped — queue is now empty
	TestTrue(TEXT("Queue empty — no message dropped"), Q.IsEmpty());

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FMessageQueuePromoteQueuedToSteer
//
// PromoteQueuedToSteer removes the item from Items and makes it PendingSteer.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMessageQueuePromoteQueuedToSteer,
	"KismetAI.MessageQueue.PromoteQueuedToSteer", MQTestFlags)

bool FMessageQueuePromoteQueuedToSteer::RunTest(const FString& Parameters)
{
	FKismetAIMessageQueue Q;

	FString IdA = Q.Enqueue(TEXT("A"), TEXT("[]"), TEXT("[]"));
	FString IdB = Q.Enqueue(TEXT("B"), TEXT("[]"), TEXT("[]"));
	FString IdC = Q.Enqueue(TEXT("C"), TEXT("[]"), TEXT("[]"));

	// Unknown id returns false
	TestFalse(TEXT("PromoteQueuedToSteer with unknown id returns false"),
		Q.PromoteQueuedToSteer(TEXT("nope")));

	// Promote middle item (B) to steer
	TestTrue(TEXT("PromoteQueuedToSteer with valid id returns true"),
		Q.PromoteQueuedToSteer(IdB));

	TestTrue(TEXT("HasPendingSteer after promote"), Q.HasPendingSteer());

	// B must no longer appear in Items (check via json — steer is first in json)
	FString Json = Q.ToJson();
	// B should appear in json as the pending steer (first element)
	// A and C should also appear
	TestTrue(TEXT("B appears in json as pending steer"), Json.Contains(TEXT("\"B\"")));
	TestTrue(TEXT("A still in json"), Json.Contains(TEXT("\"A\"")));
	TestTrue(TEXT("C still in json"), Json.Contains(TEXT("\"C\"")));

	// Dispatch order: B (steer), A, C
	TOptional<FQueuedMessage> M1 = Q.PickNext();
	TestTrue(TEXT("First dispatched is B (promoted steer)"),
		M1.IsSet() && M1->Text == TEXT("B"));

	TOptional<FQueuedMessage> M2 = Q.PickNext();
	TestTrue(TEXT("Second dispatched is A"),
		M2.IsSet() && M2->Text == TEXT("A"));

	TOptional<FQueuedMessage> M3 = Q.PickNext();
	TestTrue(TEXT("Third dispatched is C"),
		M3.IsSet() && M3->Text == TEXT("C"));

	TestTrue(TEXT("Queue empty after all dispatched"), Q.IsEmpty());

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FMessageQueueRoundTrip
//
// RefsJson and ImagesJson set at Enqueue survive through PickNext unchanged
// (these fields are NOT in ToJson — assert only on the returned FQueuedMessage).
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMessageQueueRoundTrip,
	"KismetAI.MessageQueue.RoundTrip", MQTestFlags)

bool FMessageQueueRoundTrip::RunTest(const FString& Parameters)
{
	FKismetAIMessageQueue Q;

	const FString Refs   = TEXT("[{\"type\":\"blueprint\",\"path\":\"/Game/BP_Player\"}]");
	const FString Images = TEXT("[\"data:image/png;base64,abc123\"]");

	Q.Enqueue(TEXT("my prompt"), Refs, Images);

	// ToJson must NOT contain the refs or images
	FString Json = Q.ToJson();
	TestFalse(TEXT("ToJson does not contain RefsJson"),   Json.Contains(TEXT("blueprint")));
	TestFalse(TEXT("ToJson does not contain ImagesJson"), Json.Contains(TEXT("base64")));

	// PickNext returns the payload verbatim
	TOptional<FQueuedMessage> M = Q.PickNext();
	TestTrue(TEXT("PickNext returns item"), M.IsSet());
	if (M.IsSet())
	{
		TestEqual(TEXT("Text round-trips"),     M->Text,      FString(TEXT("my prompt")));
		TestEqual(TEXT("RefsJson round-trips"), M->RefsJson,  Refs);
		TestEqual(TEXT("ImagesJson round-trips"), M->ImagesJson, Images);
	}

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FMessageQueueToJsonEscape
//
// ToJson must escape backslash and double-quote in text content.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMessageQueueToJsonEscape,
	"KismetAI.MessageQueue.ToJsonEscape", MQTestFlags)

bool FMessageQueueToJsonEscape::RunTest(const FString& Parameters)
{
	FKismetAIMessageQueue Q;

	// Text that requires escaping: backslash, double-quote, and the control chars
	// a multi-line (Shift+Enter) message carries. ToJson is injected verbatim into
	// a JS expression, so a raw newline would break the onQueueState broadcast.
	Q.Enqueue(TEXT("say \"hello\" and C:\\path\nline2\ttabbed"), TEXT("[]"), TEXT("[]"));

	FString Json = Q.ToJson();

	// The JSON must contain the escaped form
	TestTrue(TEXT("Backslash escaped in json"),
		Json.Contains(TEXT("C:\\\\path")));
	TestTrue(TEXT("Double-quote escaped in json"),
		Json.Contains(TEXT("\\\"hello\\\"")));
	TestTrue(TEXT("Newline escaped in json"),
		Json.Contains(TEXT("path\\nline2")));
	TestTrue(TEXT("Tab escaped in json"),
		Json.Contains(TEXT("line2\\ttabbed")));
	// No raw control characters survive into the output (would break the JS broadcast)
	TestFalse(TEXT("No raw newline in json"), Json.Contains(TEXT("\n")));
	TestFalse(TEXT("No raw tab in json"), Json.Contains(TEXT("\t")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FMessageQueueIsEmpty
//
// IsEmpty is true only when both Items and PendingSteer are empty.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMessageQueueIsEmpty,
	"KismetAI.MessageQueue.IsEmpty", MQTestFlags)

bool FMessageQueueIsEmpty::RunTest(const FString& Parameters)
{
	FKismetAIMessageQueue Q;
	TestTrue(TEXT("New queue is empty"), Q.IsEmpty());

	FString Id = Q.Enqueue(TEXT("x"), TEXT("[]"), TEXT("[]"));
	TestFalse(TEXT("Not empty after enqueue"), Q.IsEmpty());

	Q.Remove(Id);
	TestTrue(TEXT("Empty after remove"), Q.IsEmpty());

	// PendingSteer alone also makes queue non-empty
	FQueuedMessage S;
	S.Id = FKismetAIMessageQueue::MakeId();
	S.Text = TEXT("steer");
	Q.PromoteToSteer(S);
	TestFalse(TEXT("Not empty with PendingSteer set"), Q.IsEmpty());

	Q.PickNext(); // clears PendingSteer
	TestTrue(TEXT("Empty after steer dispatched"), Q.IsEmpty());

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FMessageQueueMakeId
//
// MakeId returns non-empty, unique strings.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMessageQueueMakeId,
	"KismetAI.MessageQueue.MakeId", MQTestFlags)

bool FMessageQueueMakeId::RunTest(const FString& Parameters)
{
	FString Id1 = FKismetAIMessageQueue::MakeId();
	FString Id2 = FKismetAIMessageQueue::MakeId();
	FString Id3 = FKismetAIMessageQueue::MakeId();

	TestFalse(TEXT("MakeId returns non-empty string"), Id1.IsEmpty());
	TestFalse(TEXT("MakeId #1 != MakeId #2"), Id1 == Id2);
	TestFalse(TEXT("MakeId #2 != MakeId #3"), Id2 == Id3);

	return true;
}
